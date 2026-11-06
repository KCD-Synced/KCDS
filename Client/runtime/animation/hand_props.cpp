#include "hand_props.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <string>

#include <windows.h>

#include "../props/prop_entity.hpp"
#include "MinHook.h"

namespace kcds::runtime::animation::hand_props {

namespace {

namespace integration = kcd2s::integration;
namespace entity = kcds::runtime::props::entity;

using Take = void(__fastcall*)(void* holder, std::uint32_t thing, int slot, const char* name,
                               const char* joint, bool flag);
using Release = void(__fastcall*)(void* holder, int slot, const char* name, bool keep, bool flag);

constexpr const char* kHandJoints[2] = {"RightHand", "LeftHand"};
constexpr std::size_t kMeshMax = 255;
// An offset is published to this precision (metres, quaternion components), so a hand that sways
// does not change the field every frame.
constexpr float kOffsetStep = 0.01f;
constexpr float kTurnStep = 0.02f;

Natives natives;
Logger log_line = nullptr;
Take original_take = nullptr;
Release original_release = nullptr;
// The entity each hand of the local player holds for the game's own action (0: none).
std::array<std::uint32_t, 2> held{};
// A minigame's tool on each hand, hung there without the hand slots (0: none).
std::array<std::uint32_t, 2> loose{};
// The game names the items it spawns for the player's own minigames after the player.
constexpr char kToolPrefix[] = "player_";
constexpr char kToolClass[] = "PickableItem";
// The lockpicking's second tool (the item "blade"): named after its item,
// not after the player, so it is known by the folder of its model.
constexpr char kLockpickMeshes[] = "keys_lockpicks/";
// A tool hangs rigidly on the hand: its place is read once it has settled and kept, so the field
// does not change, and every other client does not hang the prop again, each time it rounds
// differently.
constexpr ULONGLONG kToolSettleMs = 700;
// A tool farther than this from a hand's joint is not in that hand (a shovel's origin lies a
// little way along its haft).
constexpr float kToolReach = 1.2f;
constexpr ULONGLONG kToolSearchMs = 500;
ULONGLONG next_tool_search = 0;
// When each loose tool was found, and its place on the hand once settled.
std::array<ULONGLONG, 2> loose_since{};
std::array<bool, 2> loose_settled{};
std::array<integration::AnimationProp, 2> loose_prop{};

bool local_holder(void* holder) {
  return holder && natives.local_hands && holder == natives.local_hands();
}

void __fastcall hooked_take(void* holder, std::uint32_t thing, int slot, const char* name,
                            const char* joint, bool flag) {
  original_take(holder, thing, slot, name, joint, flag);
  if (!local_holder(holder) || slot < 0 || slot > 1) {
    return;
  }
  held[std::size_t(slot)] = thing;
  if (log_line) {
    void* body = entity::by_id(thing);
    log_line("HAND take slot=%d entity=%u name=%s mesh=%s attachment=%s", slot, thing,
             entity::name_of(body, 64).c_str(), entity::mesh_file(body, kMeshMax).c_str(),
             name ? name : "");
  }
}

void __fastcall hooked_release(void* holder, int slot, const char* name, bool keep, bool flag) {
  original_release(holder, slot, name, keep, flag);
  if (!local_holder(holder) || slot < 0 || slot > 1) {
    return;
  }
  if (held[std::size_t(slot)] != 0 && log_line) {
    log_line("HAND release slot=%d entity=%u", slot, held[std::size_t(slot)]);
  }
  held[std::size_t(slot)] = 0;
}

float stepped(float value, float step) {
  return std::round(value / step) * step;
}

// q's inverse applied to v (q unit, x y z w).
void unturn(const float q[4], const float v[3], float out[3]) {
  const float x = -q[0];
  const float y = -q[1];
  const float z = -q[2];
  const float w = q[3];
  const float tx = 2.0f * (y * v[2] - z * v[1]);
  const float ty = 2.0f * (z * v[0] - x * v[2]);
  const float tz = 2.0f * (x * v[1] - y * v[0]);
  out[0] = v[0] + w * tx + (y * tz - z * ty);
  out[1] = v[1] + w * ty + (z * tx - x * tz);
  out[2] = v[2] + w * tz + (x * ty - y * tx);
}

// The conjugate of a times b (both unit, x y z w): b in a's frame.
void relative(const float a[4], const float b[4], float out[4]) {
  const float ax = -a[0];
  const float ay = -a[1];
  const float az = -a[2];
  const float aw = a[3];
  out[0] = aw * b[0] + ax * b[3] + ay * b[2] - az * b[1];
  out[1] = aw * b[1] - ax * b[2] + ay * b[3] + az * b[0];
  out[2] = aw * b[2] + ax * b[1] - ay * b[0] + az * b[3];
  out[3] = aw * b[3] - ax * b[0] - ay * b[1] - az * b[2];
}

float distance(const float a[3], const float b[3]) {
  const float dx = a[0] - b[0];
  const float dy = a[1] - b[1];
  const float dz = a[2] - b[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The minigame tools on the local player's hands (`loose`): kept while they exist and stay in
// reach of their hand, looked for again twice a second.
void watch_loose_tools(void* body) {
  float hands[2][3];
  float unused[4];
  for (std::size_t hand = 0; hand < 2; ++hand) {
    if (!entity::joint_pose(body, kHandJoints[hand], hands[hand], unused)) {
      return;
    }
  }
  for (std::size_t hand = 0; hand < 2; ++hand) {
    void* thing = loose[hand] != 0 ? entity::by_id(loose[hand]) : nullptr;
    float position[3];
    if (thing) {
      entity::world_position(thing, position);
    }
    if (loose[hand] != 0 && (!thing || distance(position, hands[hand]) > kToolReach)) {
      if (log_line) {
        log_line("HAND tool gone slot=%zu entity=%u", hand, loose[hand]);
      }
      loose[hand] = 0;
    }
  }
  const ULONGLONG now = GetTickCount64();
  for (std::size_t hand = 0; hand < 2; ++hand) {
    if (loose[hand] == 0) {
      loose_settled[hand] = false;
    }
  }
  if (now < next_tool_search) {
    return;
  }
  next_tool_search = now + kToolSearchMs;
  entity::each_of_class(kToolClass, [&](void* thing) {
    const std::uint32_t id = entity::id_of(thing);
    if (id == loose[0] || id == loose[1] || id == held[0] || id == held[1]) {
      return;
    }
    float position[3];
    entity::world_position(thing, position);
    if (distance(position, hands[0]) > kToolReach && distance(position, hands[1]) > kToolReach) {
      return;
    }
    const std::string name = entity::name_of(thing, 64);
    const std::string mesh = entity::mesh_file(thing, kMeshMax);
    if (name.rfind(kToolPrefix, 0) != 0 && mesh.find(kLockpickMeshes) == std::string::npos) {
      return;
    }
    // Two tools can both lie nearer one hand (the lockpicks at a lock): the second takes the other.
    std::size_t hand = distance(position, hands[0]) <= distance(position, hands[1]) ? 0 : 1;
    if (held[hand] != 0 || loose[hand] != 0) {
      hand = 1 - hand;
    }
    if (held[hand] != 0 || loose[hand] != 0 || distance(position, hands[hand]) > kToolReach) {
      return;
    }
    loose[hand] = id;
    loose_since[hand] = now;
    if (log_line) {
      log_line("HAND tool slot=%zu entity=%u name=%s mesh=%s", hand, id, name.c_str(),
               mesh.c_str());
    }
  });
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  if (!natives.hand_take || !natives.hand_release) {
    return false;
  }
  const bool hooked = MH_CreateHook(natives.hand_take, reinterpret_cast<void*>(&hooked_take),
                                    reinterpret_cast<void**>(&original_take)) == MH_OK &&
                      MH_EnableHook(natives.hand_take) == MH_OK &&
                      MH_CreateHook(natives.hand_release, reinterpret_cast<void*>(&hooked_release),
                                    reinterpret_cast<void**>(&original_release)) == MH_OK &&
                      MH_EnableHook(natives.hand_release) == MH_OK;
  return hooked;
}

std::vector<integration::AnimationProp> props() {
  std::vector<integration::AnimationProp> found;
  void* body = natives.local_entity ? natives.local_entity() : nullptr;
  if (!body) {
    return found;
  }
  watch_loose_tools(body);
  for (std::size_t hand = 0; hand < held.size(); ++hand) {
    const std::uint32_t id = held[hand] != 0 ? held[hand] : loose[hand];
    if (id == 0) {
      loose_settled[hand] = false;
      continue;
    }
    const bool is_loose = held[hand] == 0;
    if (is_loose && loose_settled[hand]) {
      found.push_back(loose_prop[hand]);
      continue;
    }
    void* thing = entity::by_id(id);
    if (!thing) {
      if (held[hand] == id) {
        held[hand] = 0;
      } else {
        loose[hand] = 0;
      }
      continue;
    }
    std::string mesh = entity::mesh_file(thing, kMeshMax);
    for (char& c : mesh) {
      c = c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
    }
    if (mesh.empty()) {
      continue;
    }
    float joint_position[3];
    float joint_rotation[4];
    float thing_position[3];
    float thing_rotation[4];
    if (!entity::joint_pose(body, kHandJoints[hand], joint_position, joint_rotation)) {
      continue;
    }
    entity::world_pose(thing, thing_position, thing_rotation);
    const float delta[3] = {thing_position[0] - joint_position[0],
                            thing_position[1] - joint_position[1],
                            thing_position[2] - joint_position[2]};
    float local[3];
    unturn(joint_rotation, delta, local);
    float turn[4];
    relative(joint_rotation, thing_rotation, turn);
    integration::AnimationProp prop;
    prop.model = mesh;
    prop.left_hand = hand == 1;
    prop.position =
        std::array<float, 3>{stepped(local[0], kOffsetStep), stepped(local[1], kOffsetStep),
                             stepped(local[2], kOffsetStep)};
    std::array<float, 4> rotation{stepped(turn[0], kTurnStep), stepped(turn[1], kTurnStep),
                                  stepped(turn[2], kTurnStep), stepped(turn[3], kTurnStep)};
    const float length = std::sqrt(rotation[0] * rotation[0] + rotation[1] * rotation[1] +
                                   rotation[2] * rotation[2] + rotation[3] * rotation[3]);
    if (length > 0.0f) {
      for (float& component : rotation) {
        component /= length;
      }
      prop.rotation = rotation;
    }
    if (is_loose && GetTickCount64() - loose_since[hand] >= kToolSettleMs) {
      loose_prop[hand] = prop;
      loose_settled[hand] = true;
    }
    found.push_back(std::move(prop));
  }
  return found;
}

}  // namespace kcds::runtime::animation::hand_props
