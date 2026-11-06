#include "clip_in_place.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <string>
#include <unordered_map>

#include "../animation/player_animation.hpp"
#include "../core/log.hpp"
#include "../props/pose_math.hpp"
#include "../props/prop_entity.hpp"
#include "joint_turns.hpp"

namespace kcds::runtime::pose_animation::clip_in_place {
namespace {

using Clock = std::chrono::steady_clock;

// How long the hips' way is smoothed over (seconds): the walking of a cutscene goes, a strike's
// sway stays (the studio's, studio/studio_game_clip.cpp).
constexpr float kDriftSeconds = 0.5f;
// How often the log tells where a kept body's hips are.
constexpr auto kLogEvery = std::chrono::seconds(5);
// The hips' parent joints (the skeleton's root) the shift is in the space of; the body's own
// turn without one.
constexpr const char* kRoots[] = {"Human_Male", "Human_Female"};

struct Kept {
  void* character = nullptr;
  int hips = -1;
  // The hips' way from the body's place as the clip has it, smoothed (x, y), and the shift given
  // (in the world, x, y).
  float drift[2] = {0, 0};
  float given[2] = {0, 0};
  bool drift_known = false;
  Clock::time_point measured;
  Clock::time_point logged;
};

std::unordered_map<std::uint32_t, Kept> bodies;

void let_go(Kept& kept) {
  if (kept.character) {
    joint_turns::forget_shift(kept.character);
  }
  kept = Kept{};
}

// The root's turn in the world (the hips' parent's space), else the body's own.
void root_turn(void* entity, float turn[4]) {
  float place[3];
  for (const char* root : kRoots) {
    if (props::entity::has_joint(entity, root) &&
        props::entity::joint_pose(entity, root, place, turn)) {
      return;
    }
  }
  props::entity::world_pose(entity, place, turn);
}

// One body's frame: its shift while it plays a game clip by path; false when it plays none.
bool keep(std::uint32_t engine_id, void* entity, Kept& kept) {
  const int clip = player_animation::body_layer_clip(entity, player_animation::kScriptClipLayer);
  if (clip < 0 || !player_animation::is_game_clip(clip)) {
    return false;
  }
  void* character = props::entity::character(entity);
  const std::string hips_name = props::entity::body_joint_name(entity, "Hips");
  float hips[3];
  float hips_turn[4];
  if (!character || !props::entity::joint_pose(entity, hips_name.c_str(), hips, hips_turn)) {
    return false;
  }
  if (kept.character != character) {
    let_go(kept);
    kept.character = character;
    kept.hips = props::entity::joint_id(entity, hips_name.c_str());
  }
  if (kept.hips < 0) {
    return false;
  }
  float place[3];
  float turn[4];
  props::entity::world_pose(entity, place, turn);
  // The hips' way from the body's place as the clip has them: without the shift given.
  const float off[2] = {hips[0] - kept.given[0] - place[0], hips[1] - kept.given[1] - place[1]};
  const Clock::time_point now = Clock::now();
  if (!kept.drift_known) {
    kept.drift[0] = off[0];
    kept.drift[1] = off[1];
    kept.drift_known = true;
  } else {
    const float seconds = std::chrono::duration<float>(now - kept.measured).count();
    const float share = 1.0f - std::exp(-std::clamp(seconds, 0.0f, 0.5f) / kDriftSeconds);
    for (int axis = 0; axis < 2; ++axis) {
      kept.drift[axis] += (off[axis] - kept.drift[axis]) * share;
    }
  }
  kept.measured = now;
  // The hips back over the body's place: the way taken back, in the root's space.
  kept.given[0] = -kept.drift[0];
  kept.given[1] = -kept.drift[1];
  float root[4];
  root_turn(entity, root);
  float back[4];
  pose_math::conjugate(root, back);
  const float world[3] = {kept.given[0], kept.given[1], 0.0f};
  float local[3];
  pose_math::rotate(back, world, local);
  joint_turns::set_shift(character, kept.hips, {local[0], local[1], local[2]});
  if (now - kept.logged >= kLogEvery) {
    kept.logged = now;
    game_client::log("CLIP IN PLACE body %u: hips %.2f m off its place, taken back (clip %d)",
                     engine_id, std::sqrt(off[0] * off[0] + off[1] * off[1]), clip);
  }
  return true;
}

}  // namespace

void watch(std::uint32_t engine_id) {
  if (engine_id != 0) {
    bodies.try_emplace(engine_id);
  }
}

void tick(void* local_entity) {
  if (local_entity) {
    watch(props::entity::id_of(local_entity));
  }
  for (auto entry = bodies.begin(); entry != bodies.end();) {
    void* entity = props::entity::by_id(entry->first);
    const bool local = entity && entity == local_entity;
    if (entity && keep(entry->first, entity, entry->second)) {
      ++entry;
      continue;
    }
    let_go(entry->second);
    // The player's own body stays watched; another goes until its next clip.
    if (local) {
      ++entry;
    } else {
      entry = bodies.erase(entry);
    }
  }
}

void reset() {
  for (auto& [engine_id, kept] : bodies) {
    let_go(kept);
  }
  bodies.clear();
}

}  // namespace kcds::runtime::pose_animation::clip_in_place
