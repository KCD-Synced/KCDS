// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "held_items.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../props/pose_math.hpp"
#include "../props/prop_entity.hpp"
#include "../nameplates/nameplate_paint.hpp"
#include "remote.hpp"
#include "distance_lod.hpp"

namespace kcds::game_client {

namespace {

// IEntity GetCharacter(slot), the body's character in slot 0; its mesh slot's model and the
// slot's flags (bits 1 and 2 draw it, as appearance/disguise.cpp clears them).
constexpr std::size_t kGetCharacter = 101;
constexpr std::size_t kGetStatObj = 103;
constexpr std::size_t kSetSlotFlags = 98;
constexpr std::size_t kGetSlotFlags = 99;
constexpr int kMeshSlot = 0;
constexpr std::uint32_t kDrawBits = 3;
// ICharacterInstance GetIAttachmentManager; IAttachmentManager CreateAttachment(name, type,
// joint, force) with type 0 a joint's attachment, and GetInterfaceByName.
constexpr std::size_t kAttachmentManager = 9;
constexpr std::size_t kCreateAttachment = 2;
constexpr std::size_t kAttachmentByName = 8;
constexpr int kJointAttachment = 0;
// IAttachment (CAttachmentBONE): its place on the joint, absolute (slot 9, kept at + 272) or
// relative (slot 11, kept at + 244; the flag at + 242 says which), HideAttachment, AddBinding,
// the bound object, ClearBinding; its joint's name (a string at + 104).
constexpr std::size_t kSetAbsolute = 9;
constexpr std::size_t kSetRelative = 11;
constexpr std::size_t kAbsolutePlace = 272;
constexpr std::size_t kRelativePlace = 244;
constexpr std::size_t kPlaceIsRelative = 242;
constexpr std::size_t kHideAttachment = 17;
constexpr std::size_t kAddBinding = 26;
constexpr std::size_t kBoundObject = 28;
constexpr std::size_t kClearBinding = 30;
constexpr std::size_t kJointName = 104;
// IAttachmentObject GetAttachmentType, an entity binding's type, and its entity's id (+ 8).
constexpr std::size_t kAttachmentType = 1;
constexpr int kEntityBinding = 4;
constexpr std::size_t kBoundEntity = 8;
// A CCGFAttachment: 24 bytes, its mesh's reference at + 8 (IStatObj AddRef, slot 1).
constexpr std::size_t kMeshBindingBytes = 24;
constexpr std::size_t kMeshBindingMesh = 8;
constexpr std::size_t kAddRef = 1;

using Allocate = void*(__fastcall*)(std::size_t size);
Allocate allocate = nullptr;
void* mesh_binding_table = nullptr;

template <class Fn>
Fn slot_of(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index]);
}

template <class T>
T at(void* object, std::size_t offset) {
  return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset);
}

// An item entity one of a replica's attachments holds (a hand, a scabbard, a back slot), drawn on
// its skeleton by a mirror attachment: the entity's id (0: none) and its mesh slot's flags before.
struct Held {
  void* mirror = nullptr;
  std::string mirror_name;
  std::uint32_t entity = 0;
  std::uint32_t flags = 0;
  // Where the entity sits on its joint (position, turn), learnt while the body stands still.
  float grip_position[3] = {};
  float grip_rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  bool grip_known = false;
};
struct ReplicaItems {
  // By the game's attachment that holds the entity.
  std::unordered_map<void*, Held> by_source;
  std::unordered_set<void*> mirrors;
  unsigned next_mirror = 0;
  // Where the body stood last frame.
  float last_position[3] = {};
  bool last_known = false;
};
// A body that moved less than this since the last frame stands still: the game's placement of
// what it holds is then right, whichever pose of the body it went by.
constexpr float kStillMeters = 0.002f;
// Engine thread only: by the body's engine id.
std::unordered_map<std::uint32_t, ReplicaItems> held;
// Attachments looked at per body at most (a body has a few dozen).
constexpr unsigned kMostAttachments = 256;
constexpr std::size_t kAttachmentByIndex = 9;

void* entity_system_now() {
  void* framework = framework_slot ? *framework_slot : nullptr;
  void* actor_system =
      framework ? at<void*>(framework, 1304) : nullptr;  // as replicas/deferred_moves.cpp
  return actor_system ? at<void*>(actor_system, 48) : nullptr;
}

// The entity a hand's attachment holds (an entity binding), or 0.
std::uint32_t entity_in(void* hand) {
  using Get = void*(__fastcall*)(void* object);
  using Type = int(__fastcall*)(void* binding);
  void* bound = hand ? slot_of<Get>(hand, kBoundObject)(hand) : nullptr;
  if (!bound || slot_of<Type>(bound, kAttachmentType)(bound) != kEntityBinding) {
    return 0;
  }
  return at<std::uint32_t>(bound, kBoundEntity);
}

// The attachment `name` on the hand's joint, where the hand's attachment sits; null when it cannot
// be made.
void* mirror_of(void* manager, void* hand, const char* name) {
  using ByName = void*(__fastcall*)(void* manager, const char* name);
  using Create =
      void*(__fastcall*)(void* manager, const char* name, int type, const char* joint, char force);
  using Place = char(__fastcall*)(void* attachment, const void* place);
  void* mirror = slot_of<ByName>(manager, kAttachmentByName)(manager, name);
  if (!mirror) {
    const char* joint = at<const char*>(hand, kJointName);
    if (!joint || !*joint) {
      return nullptr;
    }
    mirror = slot_of<Create>(manager, kCreateAttachment)(manager, name, kJointAttachment, joint, 0);
    if (!mirror) {
      return nullptr;
    }
  }
  auto* bytes = static_cast<std::uint8_t*>(hand);
  if (bytes[kPlaceIsRelative]) {
    slot_of<Place>(mirror, kSetRelative)(mirror, bytes + kRelativePlace);
  } else {
    slot_of<Place>(mirror, kSetAbsolute)(mirror, bytes + kAbsolutePlace);
  }
  return mirror;
}

// Hangs `mesh` on the attachment as the game hangs a mesh by name.
bool bind_mesh(void* mirror, void* mesh) {
  if (!allocate || !mesh_binding_table) {
    return false;
  }
  void* memory = allocate(kMeshBindingBytes);
  if (!memory) {
    return false;
  }
  auto* words = static_cast<void**>(memory);
  words[0] = mesh_binding_table;
  words[1] = nullptr;
  words[2] = nullptr;
  using AddRef = void(__fastcall*)(void* mesh);
  slot_of<AddRef>(mesh, kAddRef)(mesh);
  *reinterpret_cast<void**>(static_cast<std::uint8_t*>(memory) + kMeshBindingMesh) = mesh;
  using AddBinding =
      std::uint32_t(__fastcall*)(void* attachment, void* object, void* skin, std::uint32_t flags);
  using Hide = void(__fastcall*)(void* attachment, std::uint32_t hidden);
  slot_of<AddBinding>(mirror, kAddBinding)(mirror, memory, nullptr, 0);
  slot_of<Hide>(mirror, kHideAttachment)(mirror, 0);
  return true;
}

std::uint32_t slot_flags(void* entity) {
  using Get = std::uint32_t(__fastcall*)(void* entity, int slot);
  return slot_of<Get>(entity, kGetSlotFlags)(entity, kMeshSlot);
}

void set_slot_flags(void* entity, std::uint32_t flags) {
  using Set = void(__fastcall*)(void* entity, int slot, std::uint32_t flags);
  slot_of<Set>(entity, kSetSlotFlags)(entity, kMeshSlot, flags);
}

// The entity's flame, light and sound stay on the entity, and the game places the entity from the
// body's pose of this frame or of the last one, as its update falls: a running replica's torch
// flame jumped by a step a frame against the mesh in the hand. So the entity is put on its joint
// as the body stands now, with the grip learnt while the body stood still.
void place_on_joint(void* body, void* source, void* item, Held& state, bool still) {
  namespace entity = kcds::runtime::props::entity;
  namespace pose_math = kcds::runtime::pose_math;
  const char* joint = at<const char*>(source, kJointName);
  float joint_position[3];
  float joint_rotation[4];
  if (!joint || !*joint || !entity::joint_pose(body, joint, joint_position, joint_rotation)) {
    return;
  }
  if (still) {
    float item_position[3];
    float item_rotation[4];
    entity::world_pose(item, item_position, item_rotation);
    pose_math::relative(joint_position, joint_rotation, item_position, item_rotation,
                        state.grip_position, state.grip_rotation);
    pose_math::normalize(state.grip_rotation);
    state.grip_known = true;
  }
  if (!state.grip_known) {
    return;
  }
  float position[3];
  float rotation[4];
  pose_math::compose(joint_position, joint_rotation, state.grip_position, state.grip_rotation,
                     position, rotation);
  entity::place_in_world(item, position, rotation);
}

// One attachment of one replica: the item it holds now against what its skeleton draws.
void tick_attachment(void* entity_system, void* body, void* manager, void* source, Held& state,
                     ReplicaItems& items, bool still) {
  using GetStatObj = void*(__fastcall*)(void* entity, int slot);
  using Clear = void(__fastcall*)(void* attachment, std::uint32_t flags);
  const std::uint32_t id = entity_in(source);
  void* item = id ? get_entity(entity_system, id) : nullptr;
  if (id != state.entity) {
    // What the attachment held before draws itself again, and its mesh leaves the skeleton.
    if (state.mirror) {
      slot_of<Clear>(state.mirror, kClearBinding)(state.mirror, 0);
    }
    if (void* before = state.entity ? get_entity(entity_system, state.entity) : nullptr) {
      set_slot_flags(before, slot_flags(before) | (state.flags & kDrawBits));
    }
    state.entity = 0;
    state.flags = 0;
    state.grip_known = false;
    void* mesh = item ? slot_of<GetStatObj>(item, kGetStatObj)(item, kMeshSlot) : nullptr;
    if (mesh && state.mirror_name.empty()) {
      state.mirror_name = "kcds_item_" + std::to_string(items.next_mirror++);
    }
    // The mirror sits where the source does (placed again for a new item).
    void* mirror = mesh ? mirror_of(manager, source, state.mirror_name.c_str()) : nullptr;
    if (mirror) {
      state.mirror = mirror;
      items.mirrors.insert(mirror);
    }
    if (mirror && bind_mesh(mirror, mesh)) {
      state.entity = id;
      state.flags = slot_flags(item);
    }
  }
  // The entity's own mesh stays hidden while the skeleton draws it (the game may draw it again).
  if (state.entity && item) {
    const std::uint32_t flags = slot_flags(item);
    if (flags & kDrawBits) {
      set_slot_flags(item, flags & ~kDrawBits);
    }
    place_on_joint(body, source, item, state, still);
  }
}

}  // namespace

bool install_held_items() {
  allocate = reinterpret_cast<Allocate>(
      resolve_unique("GameAllocate",
                     "40 53 48 83 EC 20 83 3D ?? ?? ?? ?? 00 48 8B D9 75 05 E8 ?? ?? ?? ?? 48 8B "
                     "05 ?? ?? ?? ?? 48 8D 54 24 38 45 33 C0 FF D0"));
  // CCGFAttachment's table, from the game's attachment of a mesh by name (its lea).
  mesh_binding_table =
      resolve_unique("MeshAttachmentTable",
                     "48 85 FF 74 1A 48 8D 05 ?? ?? ?? ?? 48 C7 47 08 00 00 00 00 48 89 07", 8, 12);
  return allocate && mesh_binding_table;
}

void tick_replica_held_items() {
  void* entity_system = entity_system_now();
  if (!entity_system || !allocate || !mesh_binding_table) {
    return;
  }
  using GetCharacter = void*(__fastcall*)(void* entity, int slot);
  using Get = void*(__fastcall*)(void* object);
  using ByIndex = void*(__fastcall*)(void* manager, unsigned index);
  std::unordered_set<std::uint32_t> bodies;
  for (const auto& [id, remote] : remotes) {
    bodies.insert(remote.engine_id);
  }
  for (auto it = held.begin(); it != held.end();) {
    it = bodies.count(it->first) ? std::next(it) : held.erase(it);
  }
  std::vector<void*> sources;
  for (const auto& [id, remote] : remotes) {
    void* body = remote.engine_id ? get_entity(entity_system, remote.engine_id) : nullptr;
    void* character = body ? slot_of<GetCharacter>(body, kGetCharacter)(body, 0) : nullptr;
    void* manager = character ? slot_of<Get>(character, kAttachmentManager)(character) : nullptr;
    if (!manager) {
      continue;
    }
    auto& items = held[remote.engine_id];
    float body_position[3];
    kcds::runtime::props::entity::world_position(body, body_position);
    bool still = false;
    if (items.last_known) {
      const float dx = body_position[0] - items.last_position[0];
      const float dy = body_position[1] - items.last_position[1];
      const float dz = body_position[2] - items.last_position[2];
      still = dx * dx + dy * dy + dz * dz < kStillMeters * kStillMeters;
    }
    std::copy(body_position, body_position + 3, items.last_position);
    items.last_known = true;
    // Listed first: a new mirror joins the manager's list.
    sources.clear();
    for (unsigned index = 0; index < kMostAttachments; ++index) {
      void* attachment = slot_of<ByIndex>(manager, kAttachmentByIndex)(manager, index);
      if (!attachment) {
        break;
      }
      if (items.mirrors.count(attachment)) {
        continue;
      }
      if (entity_in(attachment) || items.by_source.count(attachment)) {
        sources.push_back(attachment);
      }
    }
    for (void* source : sources) {
      tick_attachment(entity_system, body, manager, source, items.by_source[source], items, still);
    }
  }
}

// ---- Dev trace for crowd tests (KCDS_CROWD_TRACE=1), to be removed with the tests ----

namespace {

// One traced replica over the current second.
struct Traced {
  float last[3] = {};
  bool has_last = false;
  unsigned frames = 0;
  unsigned still = 0;  // frames the body did not move
  unsigned pops = 0;   // frames it moved more than kPopMeters
  float max_step = 0.0f;
  float max_speed = 0.0f;  // the fastest step for its frame's length (m/s)
  unsigned fast = 0;       // frames faster than kFastMetersPerSecond: a jump, not a long frame
  // Each held item's offset from the body last frame (by the item entity's id).
  std::unordered_map<std::uint32_t, std::array<float, 3>> offsets;
  float max_item_jump = 0.0f;
  unsigned item_pops = 0;
  unsigned items = 0;
  unsigned mirrored = 0;
  std::uint64_t sequences[4] = {};
  unsigned restarts[4] = {};
  // Frames it rested on its own movement, was only placed (light) and took a full update.
  unsigned rested = 0;
  unsigned light = 0;
  unsigned full = 0;
  unsigned on_screen = 0;  // frames it was in the camera's view (or close to it)
  float last_yaw = 0.0f;
  bool has_yaw = false;
  unsigned turns = 0;     // frames its facing changed more than kTurnDegrees
  float max_turn = 0.0f;  // the largest change of facing in one frame (degrees)
};
constexpr float kTurnDegrees = 10.0f;
constexpr float kPopMeters = 0.25f;
constexpr float kFastMetersPerSecond = 8.0f;
// This frame's length and the second's longest frame.
float frame_seconds = 0.0f;
float longest_frame = 0.0f;
unsigned long_frames = 0;
LARGE_INTEGER last_frame_at{};
constexpr float kItemPopMeters = 0.12f;
constexpr float kTraceMeters = 40.0f;
constexpr std::size_t kTracedBodies = 16;
std::unordered_map<core::EntityId, Traced> traced;
ULONGLONG next_trace_report = 0;
void* trace_local_entity = nullptr;

float distance(const float a[3], const float b[3]) {
  const float dx = a[0] - b[0];
  const float dy = a[1] - b[1];
  const float dz = a[2] - b[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// One frame of one traced body: its step, its held items against it, its clip restarts.
void sample(void* entity_system, const Remote& remote, void* body, Traced& one) {
  using GetCharacter = void*(__fastcall*)(void* entity, int slot);
  using Get = void*(__fastcall*)(void* object);
  using ByIndex = void*(__fastcall*)(void* manager, unsigned index);
  float at[3] = {};
  kcds::runtime::props::entity::world_position(body, at);
  if (one.has_last) {
    const float step = distance(at, one.last);
    one.max_step = std::max(one.max_step, step);
    one.still += step < 0.001f ? 1 : 0;
    one.pops += step > kPopMeters ? 1 : 0;
    if (frame_seconds > 0.0f) {
      const float speed = step / frame_seconds;
      one.max_speed = std::max(one.max_speed, speed);
      one.fast += speed > kFastMetersPerSecond ? 1 : 0;
    }
  }
  std::copy(at, at + 3, one.last);
  one.has_last = true;
  ++one.frames;
  // Its facing (the entity's x axis about the up axis): a walk on a circle turns it about a degree
  // a frame; a step over kTurnDegrees is a turn blink.
  using WorldMatrix = const float*(__fastcall*)(void* entity);
  if (const float* matrix = slot_of<WorldMatrix>(body, 32)(body)) {
    const float yaw = std::atan2(matrix[4], matrix[0]);
    if (one.has_yaw) {
      const float turn = std::fabs(std::remainder(yaw - one.last_yaw, 6.2831853f)) * 57.29578f;
      one.max_turn = std::max(one.max_turn, turn);
      one.turns += turn > kTurnDegrees ? 1 : 0;
    }
    one.last_yaw = yaw;
    one.has_yaw = true;
  }
  for (int layer = 0; layer < 4; ++layer) {
    if (remote.animation_sequences[layer] != one.sequences[layer]) {
      one.restarts[layer] += one.sequences[layer] ? 1 : 0;
      one.sequences[layer] = remote.animation_sequences[layer];
    }
  }
  void* character = slot_of<GetCharacter>(body, kGetCharacter)(body, 0);
  void* manager = character ? slot_of<Get>(character, kAttachmentManager)(character) : nullptr;
  if (!manager) {
    return;
  }
  one.items = 0;
  one.mirrored = 0;
  for (unsigned index = 0; index < kMostAttachments; ++index) {
    void* attachment = slot_of<ByIndex>(manager, kAttachmentByIndex)(manager, index);
    if (!attachment) {
      break;
    }
    const std::uint32_t id = entity_in(attachment);
    void* item = id ? get_entity(entity_system, id) : nullptr;
    if (!item) {
      continue;
    }
    ++one.items;
    one.mirrored += (slot_flags(item) & kDrawBits) ? 0 : 1;
    float place[3] = {};
    kcds::runtime::props::entity::world_position(item, place);
    const std::array<float, 3> offset{place[0] - at[0], place[1] - at[1], place[2] - at[2]};
    const auto before = one.offsets.find(id);
    if (before != one.offsets.end()) {
      const float jump = distance(offset.data(), before->second.data());
      one.max_item_jump = std::max(one.max_item_jump, jump);
      one.item_pops += jump > kItemPopMeters ? 1 : 0;
    }
    one.offsets[id] = offset;
  }
}

}  // namespace

void note_crowd_trace_local(void* local_entity) {
  trace_local_entity = local_entity;
}

namespace {
unsigned focus_id = 0;
char focus_path = '-';
float focus_target[3] = {};
// Bodies that moved in the last reported second: the focus prefers one of them.
std::unordered_set<core::EntityId> recently_moving;
}  // namespace

unsigned crowd_trace_focus() {
  return focus_id;
}

namespace {
float focus_before[3] = {};
}

void note_crowd_trace_before_update() {
  void* entity_system = focus_id ? entity_system_now() : nullptr;
  const auto found = entity_system ? remotes.find(focus_id) : remotes.end();
  void* body = found != remotes.end() && found->second.engine_id
                   ? get_entity(entity_system, found->second.engine_id)
                   : nullptr;
  if (body) {
    kcds::runtime::props::entity::world_position(body, focus_before);
  }
}

void note_crowd_trace_path(unsigned id, char path, float x, float y, float z) {
  const auto one = traced.find(id);
  if (one != traced.end()) {
    one->second.rested += path == 'r' ? 1 : 0;
    one->second.light += path == 'l' ? 1 : 0;
    one->second.full += path == 'f' ? 1 : 0;
  }
  if (id == focus_id) {
    focus_path = path;
    focus_target[0] = x;
    focus_target[1] = y;
    focus_target[2] = z;
  }
}

void tick_crowd_trace() {
  static const bool enabled = std::getenv("KCDS_CROWD_TRACE") != nullptr;
  void* entity_system = enabled ? entity_system_now() : nullptr;
  if (!entity_system || !trace_local_entity) {
    return;
  }
  LARGE_INTEGER counter{};
  LARGE_INTEGER frequency{};
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  frame_seconds =
      last_frame_at.QuadPart
          ? float(double(counter.QuadPart - last_frame_at.QuadPart) / double(frequency.QuadPart))
          : 0.0f;
  last_frame_at = counter;
  longest_frame = std::max(longest_frame, frame_seconds);
  long_frames += frame_seconds > 0.05f ? 1 : 0;
  float me[3] = {};
  kcds::runtime::props::entity::world_position(trace_local_entity, me);
  // The nearest bodies this frame.
  std::vector<std::pair<float, core::EntityId>> nearest;
  for (const auto& [id, remote] : remotes) {
    void* body = remote.engine_id ? get_entity(entity_system, remote.engine_id) : nullptr;
    if (!body) {
      continue;
    }
    float at[3] = {};
    kcds::runtime::props::entity::world_position(body, at);
    const float d = distance(at, me);
    if (d <= kTraceMeters) {
      nearest.emplace_back(d, id);
    }
  }
  std::sort(nearest.begin(), nearest.end());
  if (nearest.size() > kTracedBodies) {
    nearest.resize(kTracedBodies);
  }
  // One body traced every frame, kept while it stays among the nearest and keeps moving; the
  // nearest moving one otherwise.
  bool focus_near = false;
  for (const auto& [d, id] : nearest) {
    focus_near = focus_near || (id == focus_id && recently_moving.count(id));
  }
  if (!focus_near) {
    focus_id = nearest.empty() ? 0 : nearest.front().second;
    for (const auto& [d, id] : nearest) {
      if (recently_moving.count(id)) {
        focus_id = id;
        break;
      }
    }
  }
  for (const auto& [d, id] : nearest) {
    const Remote& remote = remotes.at(id);
    void* body = get_entity(entity_system, remote.engine_id);
    sample(entity_system, remote, body, traced[id]);
    float at[3] = {};
    kcds::runtime::props::entity::world_position(body, at);
    traced[id].on_screen += replica_off_screen(at) ? 0 : 1;
    if (id == focus_id) {
      log("CROWDP id=%u t=%.4f dt=%.1f body=(%.3f %.3f %.3f) target=(%.3f %.3f %.3f) path=%c "
          "pre=(%.3f %.3f %.3f)",
          id, double(counter.QuadPart) / double(frequency.QuadPart), frame_seconds * 1000.0f, at[0],
          at[1], at[2], focus_target[0], focus_target[1], focus_target[2], focus_path,
          focus_before[0], focus_before[1], focus_before[2]);
      focus_path = '-';
    }
  }
  const ULONGLONG now = GetTickCount64();
  if (now < next_trace_report) {
    return;
  }
  next_trace_report = now + 1000;
  unsigned paint_us = 0;
  unsigned painted = 0;
  unsigned paired = 0;
  unsigned unpaired = 0;
  kcds::runtime::nameplates::take_paint_stats(paint_us, painted, paired, unpaired);
  log("CROWD me=(%.1f %.1f %.1f) remotes=%zu near=%zu longest_frame=%.0fms frames_over_50ms=%u "
      "plates_paint=%uus plates=%u plates_frames=%u/%u",
      me[0], me[1], me[2], remotes.size(), nearest.size(), longest_frame * 1000.0f, long_frames,
      paint_us, painted, paired, unpaired);
  longest_frame = 0.0f;
  long_frames = 0;
  recently_moving.clear();
  for (const auto& [d, id] : nearest) {
    Traced& one = traced[id];
    if (one.max_speed > 0.3f) {
      recently_moving.insert(id);
    }
    log("CROWD body=%u d=%.1f frames=%u still=%u pops=%u max_step=%.2f max_speed=%.1f fast=%u "
        "items=%u mirrored=%u item_pops=%u item_jump=%.2f restarts=%u/%u/%u/%u paths=%u/%u/%u "
        "on_screen=%u turns=%u max_turn=%.0f",
        id, d, one.frames, one.still, one.pops, one.max_step, one.max_speed, one.fast, one.items,
        one.mirrored, one.item_pops, one.max_item_jump, one.restarts[0], one.restarts[1],
        one.restarts[2], one.restarts[3], one.rested, one.light, one.full, one.on_screen, one.turns,
        one.max_turn);
  }
  // Each body's second starts again; one that left the nearest keeps nothing.
  std::unordered_map<core::EntityId, Traced> kept;
  for (const auto& [d, id] : nearest) {
    Traced fresh;
    const Traced& one = traced[id];
    std::copy(one.last, one.last + 3, fresh.last);
    fresh.has_last = one.has_last;
    fresh.offsets = one.offsets;
    std::copy(one.sequences, one.sequences + 4, fresh.sequences);
    kept.emplace(id, std::move(fresh));
  }
  traced = std::move(kept);
}

}  // namespace kcds::game_client
