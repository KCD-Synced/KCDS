#include "studio_pose.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "../pose_animation/joint_turns.hpp"
#include "../props/prop_entity.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "studio_game_clip.hpp"

namespace kcds::runtime::studio::pose {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;
namespace joint_turns = pose_animation::joint_turns;

constexpr std::size_t kSetPlaybackScaleSlot = 41;
constexpr int kMostJoints = 1024;

using SetPlaybackScale = void(__fastcall*)(void* character, float scale);

template <typename Function>
Function slot(void* object, std::size_t index) {
  return reinterpret_cast<Function>((*static_cast<void***>(object))[index]);
}

std::mutex turns_mutex;
std::vector<joint_turns::Turn> turns;
// How far the hips are lifted (metres; a baked clip's keys), and whether they were.
float lift = 0.0f;
bool lifted = false;
bool turns_changed = false;
std::atomic<bool> idle{false};
// The character the turns were given to last (the studio's body).
void* posed = nullptr;

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = integration::script_operation(message);
  if (operation == "studioIdle") {
    const sdk::Value* on = integration::script_field(message, "on");
    idle.store(on && on->tag() == sdk::Tag::Bool && on->as_bool());
    return true;
  }
  if (operation != "studioPose") {
    return false;
  }
  const sdk::Value* layer_field = integration::script_field(message, "layer");
  const sdk::Value* op_field = integration::script_field(message, "op");
  if ((layer_field && layer_field->tag() == sdk::Tag::Int) ||
      (op_field && op_field->tag() == sdk::Tag::Int)) {
    joint_turns::set_layer(
        layer_field && layer_field->tag() == sdk::Tag::Int ? unsigned(layer_field->as_int()) : 16u,
        op_field && op_field->tag() == sdk::Tag::Int ? short(op_field->as_int()) : short(-1));
  }
  std::vector<joint_turns::Turn> next;
  const sdk::Value* joints = integration::script_field(message, "joints");
  if (joints && joints->tag() == sdk::Tag::Array) {
    for (const sdk::Value& entry : joints->as_array()) {
      if (entry.tag() != sdk::Tag::Array || entry.as_array().size() != 5) {
        continue;
      }
      const auto& parts = entry.as_array();
      double numbers[5] = {0, 0, 0, 0, 0};
      bool usable = true;
      for (int index = 0; index < 5; ++index) {
        if (parts[index].tag() == sdk::Tag::Int) {
          numbers[index] = double(parts[index].as_int());
        } else if (parts[index].tag() == sdk::Tag::Double) {
          numbers[index] = parts[index].as_double();
        } else {
          usable = false;
        }
      }
      if (!usable || numbers[0] < 0 || numbers[0] >= kMostJoints) {
        continue;
      }
      joint_turns::Turn turn;
      turn.joint = int(numbers[0]);
      for (int index = 0; index < 4; ++index) {
        turn.rotation[std::size_t(index)] = float(numbers[index + 1]);
      }
      next.push_back(turn);
    }
  }
  const sdk::Value* lift_field = integration::script_field(message, "lift");
  double lifted_by = 0.0;
  if (lift_field && lift_field->tag() == sdk::Tag::Double) {
    lifted_by = lift_field->as_double();
  } else if (lift_field && lift_field->tag() == sdk::Tag::Int) {
    lifted_by = double(lift_field->as_int());
  }
  std::lock_guard<std::mutex> lock(turns_mutex);
  turns = std::move(next);
  lift = std::isfinite(lifted_by) ? float(std::clamp(lifted_by, -1.0, 0.5)) : 0.0f;
  turns_changed = true;
  return true;
}

void tick(void* local_entity) {
  void* character = props::entity::character(local_entity);
  if (!character) {
    return;
  }
  // Held still unless the idle plays, or a clip of the game's own plays (it would stay on its
  // first frame: live, the executioner stood 30 s).
  const bool moving = idle.load() || game_clip::playing();
  slot<SetPlaybackScale>(character, kSetPlaybackScaleSlot)(character, moving ? 1.0f : 0.0f);
  std::vector<joint_turns::Turn> now;
  float lift_now = 0.0f;
  {
    std::lock_guard<std::mutex> lock(turns_mutex);
    if (!turns_changed && character == posed) {
      return;
    }
    turns_changed = false;
    now = turns;
    lift_now = lift;
  }
  // The hips lifted as the animation's keys say (the body's place stays).
  if (lift_now != 0.0f) {
    const std::string hips = props::entity::body_joint_name(local_entity, "Hips");
    const int id = props::entity::joint_id(local_entity, hips.c_str());
    if (id >= 0) {
      joint_turns::set_shift(character, id, {0.0f, 0.0f, lift_now});
      lifted = true;
    }
  } else if (lifted) {
    joint_turns::forget_shift(character);
    lifted = false;
  }
  // The joints the body does not have are left out by the game.
  const int count = props::entity::joint_count(local_entity);
  std::erase_if(now, [count](const joint_turns::Turn& turn) { return turn.joint >= count; });
  if (posed && posed != character) {
    joint_turns::forget(posed);
  }
  joint_turns::set(character, std::move(now));
  posed = character;
}

}  // namespace kcds::runtime::studio::pose
