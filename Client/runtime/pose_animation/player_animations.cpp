// Windows before the rest (the console's slot).
#include "../core/client_internal.hpp"

#include "player_animations.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../props/prop_entity.hpp"
#include "joint_turns.hpp"
#include "kcd2s/integration/pose_animation.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::pose_animation::players {
namespace {

namespace integration = kcd2s::integration;
namespace entity = props::entity;

// The actor's entity (C_Player + 56), as player_props reads it.
constexpr std::size_t kActorEntity = 56;

struct Playing {
  std::string field;
  std::optional<integration::PoseAnimation> animation;
  // A field naming an animation defined by name that this client does not have yet: looked up
  // again each frame until it comes.
  bool waiting = false;
  std::chrono::steady_clock::time_point started;
  void* character = nullptr;
  // The joints by name on this character: their ids, -1 for one it does not have; looked up
  // again each second (ids_at).
  std::map<std::string, int> ids;
  std::chrono::steady_clock::time_point ids_at{};
  // The hips' id and whether they are lifted now (joint_turns::set_shift).
  int hips = -2;
  bool lifted = false;
  // Whether its keys turn a leg's joints (the feet lock is off meanwhile: keep_feet_free).
  bool legs = false;
};

std::unordered_map<std::uint32_t, Playing> playing;

// The game locks the feet with IK while additive animations play (ca_LockFeetWithIK, WHGame
// "If this is set to 1, then we lock the feet to prevent sliding when additive
// animations are used"). A pose animation's turns are additive (joint_turns), so the feet were
// put back where the body's own animation had them: a baked clip's feet ignored their keys and
// pointed out as the idle stands (any foot key looked the same). While an
// animation that turns a leg's joints plays on this client the lock is off; the game's own
// value comes back once none does.
constexpr const char* kLockFeet = "ca_LockFeetWithIK";
using GetCVar = void*(__fastcall*)(void* console, const char* name);
using CVarGetInt = int(__fastcall*)(void* cvar);
using CVarSetInt = void(__fastcall*)(void* cvar, int value);
constexpr std::size_t kConsoleGetCVar = 23;
constexpr std::size_t kCVarGetInt = 2;
constexpr std::size_t kCVarSetInt = 7;
// The lock's value before this client turned it off; nothing while it is the game's own.
std::optional<int> feet_lock_before;

void* feet_lock_cvar() {
  void* console = game_client::console_slot ? *game_client::console_slot : nullptr;
  return console ? game_client::slot<GetCVar>(console, kConsoleGetCVar)(console, kLockFeet)
                 : nullptr;
}

void keep_feet_free() {
  const bool wanted = std::any_of(playing.begin(), playing.end(), [](const auto& entry) {
    return entry.second.legs && entry.second.animation;
  });
  if (wanted == feet_lock_before.has_value()) {
    return;
  }
  void* cvar = feet_lock_cvar();
  if (!cvar) {
    return;
  }
  if (wanted) {
    feet_lock_before = game_client::slot<CVarGetInt>(cvar, kCVarGetInt)(cvar);
    game_client::slot<CVarSetInt>(cvar, kCVarSetInt)(cvar, 0);
  } else {
    game_client::slot<CVarSetInt>(cvar, kCVarSetInt)(cvar, *feet_lock_before);
    feet_lock_before.reset();
  }
  game_client::log("POSE feet lock %s (%s=%d)", wanted ? "off for the legs' keys" : "back",
                   kLockFeet, game_client::slot<CVarGetInt>(cvar, kCVarGetInt)(cvar));
}

// Whether an animation's keys turn a leg's joints (the thighs, knees, feet or toes).
bool turns_legs(const integration::PoseAnimation& animation) {
  for (const auto& key : animation.keys) {
    for (const auto& [name, turn] : key.turns) {
      if (name.find("Leg") != std::string::npos || name.find("Foot") != std::string::npos ||
          name.find("Toe") != std::string::npos) {
        return true;
      }
    }
  }
  return false;
}

// The animations defined by name (Animations.define longer than the field): every
// client gets each once (the reserved client event kcds:poseAnimation, from the client worker).
constexpr std::size_t kMostDefined = 256;
std::mutex defined_mutex;
std::unordered_map<std::string, integration::PoseAnimation> defined;

void stop(Playing& state) {
  if (state.character) {
    joint_turns::forget(state.character);
    if (state.lifted) {
      joint_turns::forget_shift(state.character);
    }
    state.character = nullptr;
  }
  state.ids.clear();
  state.hips = -2;
  state.lifted = false;
}

// The field's animation: its own text, or the one defined by the name it gives; waiting: it
// names one this client does not have yet.
std::optional<integration::PoseAnimation> animation_of(const std::string& field, bool& waiting) {
  waiting = false;
  if (field.empty()) {
    return std::nullopt;
  }
  std::string name;
  bool loop = true;
  if (!integration::pose_animation_reference(field, name, loop)) {
    return integration::pose_animation_of(field);
  }
  std::lock_guard<std::mutex> lock(defined_mutex);
  const auto found = defined.find(name);
  if (found == defined.end()) {
    waiting = true;
    return std::nullopt;
  }
  integration::PoseAnimation animation = found->second;
  animation.loop = loop;
  return animation;
}

}  // namespace

bool handle_runtime_message(const kcd2s::sdk::Value& message) {
  if (integration::script_operation(message) != "poseAnimationDefine") {
    return false;
  }
  const kcd2s::sdk::Value* name = integration::script_field(message, "name");
  const kcd2s::sdk::Value* field = integration::script_field(message, "field");
  if (!name || name->tag() != kcd2s::sdk::Tag::Str || !field ||
      field->tag() != kcd2s::sdk::Tag::Str || !integration::pose_joint_name(name->as_str())) {
    return true;
  }
  std::lock_guard<std::mutex> lock(defined_mutex);
  if (field->as_str().empty()) {
    defined.erase(name->as_str());
    return true;
  }
  const auto animation =
      integration::pose_animation_of(field->as_str(), integration::kMaxDefinedPoseAnimationBytes);
  if (animation && (defined.size() < kMostDefined || defined.count(name->as_str()) != 0)) {
    defined[name->as_str()] = *animation;
  }
  game_client::log("POSE animation %s defined: %s (%zu keys, %zu bytes)", name->as_str().c_str(),
                   animation ? "kept" : "refused", animation ? animation->keys.size() : 0,
                   field->as_str().size());
  return true;
}

void show(std::uint32_t player, void* actor, const std::string& field) {
  Playing& state = playing[player];
  if (field != state.field || state.waiting) {
    const bool was_waiting = state.waiting && field == state.field;
    if (!was_waiting) {
      stop(state);
      state.field = field;
      state.started = std::chrono::steady_clock::now();
    }
    state.animation = animation_of(field, state.waiting);
    state.legs = state.animation && turns_legs(*state.animation);
    keep_feet_free();
    if (!field.empty() && !state.waiting) {
      game_client::log("POSE player=%u animation %s (%zu keys)", player,
                       state.animation ? "playing" : "refused",
                       state.animation ? state.animation->keys.size() : std::size_t(0));
    }
  }
  void* body =
      actor ? *reinterpret_cast<void**>(static_cast<char*>(actor) + kActorEntity) : nullptr;
  void* character = body ? entity::character(body) : nullptr;
  if (!state.animation || !character) {
    stop(state);
    return;
  }
  if (character != state.character) {
    stop(state);
    state.character = character;
  }
  // The joints are looked up by name again every second: a body that joins gets its model after
  // it is in the world, and the ids found on the first skeleton turned the wrong bones of the
  // model (a remote player's animation showed only after it was played again).
  const auto looked = std::chrono::steady_clock::now();
  if (looked - state.ids_at > std::chrono::seconds(1)) {
    state.ids.clear();
    state.hips = -2;
    state.ids_at = looked;
  }
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - state.started).count();
  const float time = integration::pose_play_time(*state.animation, elapsed);
  std::vector<joint_turns::Turn> turns;
  for (const auto& [name, degrees] : integration::pose_turns_at(*state.animation, time)) {
    auto known = state.ids.find(name);
    if (known == state.ids.end()) {
      known = state.ids.emplace(name, entity::joint_id(body, name.c_str())).first;
    }
    if (known->second < 0) {
      continue;
    }
    joint_turns::Turn turn;
    turn.joint = known->second;
    turn.rotation = integration::pose_quaternion(degrees);
    turns.push_back(turn);
  }
  joint_turns::set(character, std::move(turns));
  // The hips lifted as the keys say (a baked clip's bent knees on the ground), in their parent's
  // space (the skeleton's root, upright with the body).
  const bool lifts = std::any_of(state.animation->keys.begin(), state.animation->keys.end(),
                                 [](const integration::PoseKey& key) { return key.lift; });
  if (lifts) {
    if (state.hips == -2) {
      state.hips = entity::joint_id(body, entity::body_joint_name(body, "Hips").c_str());
    }
    if (state.hips >= 0) {
      joint_turns::set_shift(character, state.hips,
                             {0.0f, 0.0f, integration::pose_lift_at(*state.animation, time)});
      state.lifted = true;
    }
  }
}

std::map<char, float> prop_now(std::uint32_t player) {
  const auto found = playing.find(player);
  if (found == playing.end() || !found->second.animation) {
    return {};
  }
  const Playing& state = found->second;
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - state.started).count();
  return integration::pose_prop_at(*state.animation,
                                   integration::pose_play_time(*state.animation, elapsed));
}

std::vector<std::array<float, 3>> lash_now(std::uint32_t player) {
  const auto found = playing.find(player);
  if (found == playing.end() || !found->second.animation) {
    return {};
  }
  const Playing& state = found->second;
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - state.started).count();
  return integration::pose_lash_at(*state.animation,
                                   integration::pose_play_time(*state.animation, elapsed));
}

bool lash_in_body(std::uint32_t player) {
  const auto found = playing.find(player);
  if (found == playing.end() || !found->second.animation) {
    return false;
  }
  const integration::PoseLashTrack& track = found->second.animation->lash_track;
  return !track.frames.empty() && track.body;
}

void keep_only(const std::vector<std::uint32_t>& players) {
  for (auto entry = playing.begin(); entry != playing.end();) {
    if (std::find(players.begin(), players.end(), entry->first) == players.end()) {
      stop(entry->second);
      entry = playing.erase(entry);
    } else {
      ++entry;
    }
  }
  keep_feet_free();
}

}  // namespace kcds::runtime::pose_animation::players
