// The other players' bodies in this game: each replica's engine objects and what it last applied
// of its owner's reports (split out of game_client.cpp). Engine update thread only.
#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "../../../Backend/integration/local/runtime.hpp"
#include "../animation/human_overlay.hpp"
#include "../buffs/replica_buffs.hpp"
#include "../knockout/replica_wake.hpp"
#include "distance_lod_policy.hpp"

namespace kcds::game_client {

namespace core = kcd2s::core;

// A replica: its engine objects and what it last applied of its owner's reports.
struct Remote {
  std::uint32_t engine_id;
  void* actor;
  // Made as the game's PlayerFemale (the female skeleton and animation database) for an owner
  // whose body is a woman's; a change of gender makes the body anew.
  bool female = false;
  // A settled idle body rests between full updates (replicas/distance_lod.cpp, replica_can_rest).
  std::uint64_t rest_revision = 0;
  bool rest_idle = false;
  // On foot and out of any fight, moving or not: off screen it may update less often.
  bool rest_calm = false;
  // The owner's state the last full update saw, and whether a newer one waits for its turn.
  std::uint64_t light_revision = 0;
  bool light_pending = false;
  // The walk the last full update asked of the body (its movement request without the turn): a
  // light frame asks it again, as the game's own movement takes a request each frame. Without it
  // the legs fell out of the stride between full updates and snapped back on the next one.
  bool light_motion = false;
  std::uint32_t light_motion_type = 0;
  core::Vec3 light_motion_velocity{};
  bool light_motion_strafe = false;
  unsigned rested_frames = 0;
  void* autonomy_context{};
  void* skeleton{};
  // Since when the body has had no animation on its base layer (0 while it has one), and whether
  // that was logged: such a body is drawn in its default pose, arms out.
  std::uint64_t no_base_clip_since = 0;
  bool no_base_clip_logged = false;
  void* look{};
  kcds::runtime::replicas::DistanceLodPolicy distance_lod;
  std::uint64_t animation_sequences[16]{};
  // The owner's look-pose sequence that waits for the game's mount or dismount to end, once
  // logged (look/look_pose_layer.hpp).
  std::uint64_t waiting_look_pose = 0;
  std::uint64_t human_overlay_sequence = 0;
  std::uint64_t attempted_overlay_sequence = 0;
  std::uint64_t attempted_overlay_native_starts = 0;
  std::uint64_t applied_overlay_native_starts = 0;
  std::uint64_t observed_overlay_sequence = 0;
  std::optional<kcds::runtime::human_overlay::Scope> human_overlay_scope;
  std::shared_ptr<const kcds::runtime::human_overlay::Membership> human_overlay_membership;
  std::uint64_t unavailable_overlay_alias = 0;
  int human_overlay_clip = -1;
  std::array<int, 16> stopping_overlay_clips{};
  unsigned stopping_overlay_count = 0;
  // The weapon in the replica's hands and the owner's weapon clips it played.
  void* weapon_skeleton{};
  std::uint64_t weapon_sequences[16]{};
  // Its aim pose blender, held where its owner aims (ranged/aim.hpp).
  void* aim{};
  kcds::runtime::knockout::ReplicaWake wake;
  std::optional<kcds::runtime::knockout::ReplicaWakeScope> wake_scope;
  bool wake_presentation_held = false;
  void* riposte_ability_soul = nullptr;
  std::optional<bool> attempted_riposte_ability;
  std::uint8_t riposte_ability_epoch = 0;
  // The owner's replicaSoul report last given to this replica's soul, and that soul.
  void* replica_soul_applied_to = nullptr;
  std::string replica_soul_applied;
  // The owner's replicaBuffs report last given to this replica's soul, that soul and when.
  void* replica_buffs_applied_to = nullptr;
  std::string replica_buffs_applied;
  ULONGLONG replica_buffs_applied_at = 0;
  kcds::runtime::buffs::replica::Mirror replica_buffs_mirror;
  // What voice chat's effects read of the owner.
  bool voice_visor_closed = false;
  // The visor state shown on the replica's body, and the actor it was shown on.
  bool visor_shown_closed = false;
  void* visor_shown_on = nullptr;
  bool voice_riding = false;
  std::string voice_vitals;
  std::string voice_abilities;
};

// The replicas by their owners' network ids.
extern std::unordered_map<core::EntityId, Remote> remotes;

}  // namespace kcds::game_client
