// Horse trample and the ragdoll bodies of trampled and knocked-out replicas:
// the collision relay and damage, the ragdoll physics and pose, and the trample test replay.
#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "../../../Backend/integration/local/animation.hpp"
#include "kcd2s/core/ids.hpp"
#include "kcd2s/core/math.hpp"
#include "kcd2s/integration/combat.hpp"

namespace kcds::game_client {

// A trampled replica falls by the game's own reaction for this long.
inline constexpr ULONGLONG kTrampleFallMs = 3500;
// HitInfo+24: the hit type; the actor collision handler queues type 3.
inline constexpr unsigned kCollisionHit = 3;
bool trample_falling(kcd2s::core::EntityId player);

// The collision hit event's creation (hooked: the original), a soul's derived stat, the damage
// type's construction and destruction, and an articulated part's parameters.
using CreateCollisionHitEvent = void**(__fastcall*)(void*, void**, const void*);
extern CreateCollisionHitEvent original_create_collision_hit_event;
void** __fastcall hooked_create_collision_hit_event(void* self, void** event, const void* data);
using SoulStat = float(__fastcall*)(void* soul, int stat, void* context);
extern SoulStat native_soul_stat;
using DamageTypeInit = void*(__fastcall*)(void* type);
using DamageTypeFree = void(__fastcall*)(void* type);
extern DamageTypeInit damage_type_init;
extern DamageTypeFree damage_type_free;
using PartParamsInit = void*(__fastcall*)(void* params);
extern PartParamsInit native_part_params_init;
bool apply_trample_damage(const kcd2s::integration::NativeReactionData& data, void* horse_actor,
                          void* victim_actor);

// The actor's physicalization profile, as the script binding SetPhysicalizationProfile sets it.
enum class PhysicsProfile : std::uint8_t {
  Alive = 1,
  Ragdoll = 2,
  Sleep = 3,
};
bool set_physics_profile(void* actor, PhysicsProfile profile);

// The ragdoll pose: one articulated part's transform is this many bytes.
inline constexpr std::size_t kPartTransformSize = 28;
void* articulated_physics(void* entity);
std::string capture_ragdoll_pose(void* entity);
bool valid_ragdoll_pose(const std::string& pose);
unsigned apply_ragdoll_pose(void* entity, const std::string& pose);

// A trampled replica's own ragdoll, and its heading while it gets up.
inline constexpr float kTrampleRagdollFollowMeters = 0.1f;
bool trample_ragdoll_physics(kcd2s::core::EntityId player);
bool heading_of(const float* m, kcd2s::core::Quat& heading);
kcd2s::core::Quat turn_between(const kcd2s::core::Quat& from, const kcd2s::core::Quat& to);
const kcd2s::core::Quat& replica_heading(kcd2s::core::EntityId player,
                                         const kcd2s::core::Quat& owner);
// Whether the replica is getting up from lying (a trample's or its owner's ragdoll, for
// kTrampleGettingUpMs after it is animated again).
bool replica_getting_up(kcd2s::core::EntityId player);
bool tick_trample_ragdoll(kcd2s::core::EntityId player,
                          const std::optional<kcd2s::local::AnimationState>& clips,
                          std::uint64_t applied_body_sequence, void* actor, void* skeleton,
                          void* entity, const kcd2s::core::Vec3& owner);

// A replica whose owner lies as a ragdoll for another reason (a knock-out), and the owner's
// ragdoll pose it last took.
extern std::unordered_set<kcd2s::core::EntityId> owner_ragdolled;
struct AppliedRagdollPose {
  void* physics = nullptr;
  std::string pose;
  unsigned updates = 0;
};
extern std::unordered_map<kcd2s::core::EntityId, AppliedRagdollPose> applied_ragdoll_poses;
// `body_sequence`: the sequence of the owner's base-layer clip last applied to the replica.
bool tick_owner_ragdoll(kcd2s::core::EntityId player, void* actor, bool owner_ragdoll,
                        std::uint64_t body_sequence, void* entity);
// A replica whose owner stopped lying is animated again (its living physicalization profile) in the
// frame after its owner's getting up went on it, as the owner's game does: its getting up's clip
// starts on the ragdoll, and that clip's start callback turns the ragdoll into a living body
// (profile 1 through), whose blend from the ragdoll (skeleton pose
// slot 112) gives the entity the heading the clip gets up with. Animated before the clip, the
// replica got another heading, about 180 degrees from its owner's, and spun on the ground or stood
// up facing away (live traces). Called after the owner's clips are applied, and at the start
// of the next frame by tick_owner_ragdoll.
void finish_owner_get_up(kcd2s::core::EntityId player, void* actor, std::uint64_t body_sequence);
// Test hook (KCDS_TEST_REPLICA_TRACE): while a body's hips are near the ground and for 1.5 s
// after, every 50 ms, its entity's forward and up axes and its hips' rotation, in the owner's game
// ("own") and in the others ("replica").
void trace_lying_body(const char* who, kcd2s::core::EntityId player, void* entity);

// The trample relay of the local player's horse, and the test replay of the last trample.
extern HANDLE trample_replay_event;
bool slow_ridden_horse_collision(const std::uint8_t* hit);
bool relay_trample(const std::uint8_t* hit);
void tick_trample_replay();

}  // namespace kcds::game_client
