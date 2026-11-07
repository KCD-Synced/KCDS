// The horses this client simulates and the replicas of horses others simulate (split
// out of game_client.cpp, where it was native_horses.inl). Engine update thread only.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../../Backend/integration/local/runtime.hpp"
#include "owned_horse.hpp"

namespace kcds::game_client {

namespace core = kcd2s::core;
using HorseMount = char(__fastcall*)(void*, void*, char);
// The game calls it with (actor, 1, 0) and only forwards the rest.
using HorseDismount = std::int64_t(__fastcall*)(void*, std::uint64_t, std::uint64_t);
using HumanMounted = char(__fastcall*)(void*);
using RemoteDismount = std::int64_t(__fastcall*)(void*, char, char);
using RiderReady = char(__fastcall*)(void*);
using RiderActionGetter = void**(__fastcall*)(void*, void**, unsigned char);
using RiderActionEvent = std::intptr_t(__fastcall*)(void*);

// A horse in the world: one this client simulates (authority) or the replica of one another
// client simulates.
struct NativeHorse {
  std::uint32_t engine_id{};
  core::Vec3 position{};
  core::Quat orientation{};
  void* actor{};
  void* skeleton{};
  void* animated{};
  std::uint64_t sequence{};
  std::uint64_t animation_sequences[16]{};
  core::EntityId completion_owner{};
  std::uint64_t mount_completed_generation{}, dismount_completed_generation{};
  bool replica_transition{};
  std::uint32_t epoch{};
  // This client simulates the horse (it is the HorseSimulator).
  bool authority{};
  core::EntityId replica_rider{};
  double next_mount_attempt{}, next_diag{}, next_head_diag{};
  // Where the replica stood when this client last followed it, and when.
  core::Vec3 followed_position{};
  double followed_at{};
  // The horseOwner field this client last saw, -1 before the first.
  std::int64_t owner = -1;
  // The look the body was last built from, and the tack it last wore.
  std::string look;
  std::string gear;
  // Whether the pieces of that tack that show a name of their own were logged.
  bool gear_names_logged = false;
  // The soul its carry limit was last given to, null for none.
  void* carry_soul{};
  // Health: what the simulator last reported (health, state 0 alive / 1 knocked out
  // / 2 dead) and when it may report again; the `life` the body last followed; when the
  // knock-out immortality is checked again.
  float sent_health = -1;
  int sent_state = -1;
  double next_health_report{};
  // Whether this game's local player may ride it (HorseRiders): its script's mountIsLegal, set.
  bool mount_legal = false;
  // How dirty the server made it (HorseDirt), as last put on its body; -1 the game's own.
  float dirt = -1;
  // The share of its pace it moves at, as last applied (Horse.speedScale); -1 the game's own.
  float speed_scale = -1;
  // The body the scale was given to (horse_speed_scale.hpp keys it by its IEntity).
  void* speed_body = nullptr;
  // Who leads it on a rope (HorseLead, Horse.follow with rope): a network id, 0 for none.
  core::EntityId lead = 0;
  // Stamina (Horse.stamina): what the simulator last reported and when it may report again.
  float sent_stamina = -1;
  double next_stamina_report{};
  std::int64_t life = -1;
  double next_immortality_check{};
  // The server's health last seen, so the simulator applies a resource's change only when it comes.
  float seen_server_health = -1;
  // The order: the `intent` text last taken, its sequence, and a patrol's points, the
  // leg it walks, its direction (pingPong turns back), whether it loops or turns, its speed and
  // wait, and when the wait at a point ends (0 while walking).
  std::string intent;
  std::uint32_t order_sequence{};
  std::vector<core::Vec3> patrol;
  std::size_t leg{};
  int leg_step = 1;
  std::string patrol_mode;
  int order_speed = 1;
  double patrol_wait{};
  double leg_starts_at{};
};

// The game's mount and dismount (hooked) and the natives the riders use.
extern HorseMount original_horse_mount, native_remote_horse_mount;
extern HorseDismount original_horse_dismount;
extern RemoteDismount native_remote_horse_dismount;
extern HumanMounted native_human_mounted;
extern RiderReady native_rider_ready;
extern RiderActionGetter native_rider_action_getter;
extern RiderActionEvent native_mount_completed, native_dismount_completed, native_dismount_detach;
// C_Horse's Init (its vtable slot 7): tells a horse's actor.
extern void* native_horse_init;

// The horses in the world, by network id.
extern std::unordered_map<core::EntityId, NativeHorse> native_horses;
extern bool player_horses_installed;

char __fastcall hooked_horse_mount(void* actor, void* horse, char instant);
std::int64_t __fastcall hooked_horse_dismount(void* actor, std::uint64_t a2, std::uint64_t a3);
bool simulated_horse_actor(void* actor);
bool horse_replica_actor(void* actor);
// The key of a piece a server horse wears, "<horse network id>_<class as scripts write it>"
// (item_class_script_text, the server's classBytesHex), or empty for an item no server horse
// wears; its own name
// and description are under kcds_nm_h_<key> and kcds_in_h_<key> (inventory/row_names.hpp).
std::string horse_piece_key(void* item);
// Logs the horse's worn pieces that show a name of their own, once per tack change.
void log_named_horse_pieces(core::EntityId id, NativeHorse& horse);
float matrix_yaw(const float* m);
float rotation_yaw(const core::Quat& q);
void follow_replicated_horse(NativeHorse& horse, void* entity, const core::Vec3& p,
                             const core::Quat& q, const core::Vec3* velocity, double now);
std::string capture_native_rider_state(void* actor);
void synchronize_native_rider_completion(core::EntityId owner, void* actor,
                                         const std::string& payload);
bool native_rider_transition(void* actor);
void clear_native_horses(void* entity_system);
unsigned ridden_horse_engine(core::EntityId rider);
// The server's player.mount: the local player gets on the horse's body here with the
// game's own mount, the get-on or the instant seat. False when the horse has no body here or the
// player stands farther than kMountOrderReach from it.
bool mount_ordered_horse(core::EntityId horse, bool instant);
// The server's player.dismount (or a refused mount): the local player gets off with the game's own
// dismount. False when they ride nothing.
bool dismount_ordered();
// The server's Horse.buckRider: the horse the local player rides does the game's own.
bool rear_ordered();
// The server's Horse.dragRider: the game's own pull-down of the attacker's body on the rider's.
bool pull_down_ordered(std::uint32_t attacker, std::uint32_t rider);
// Every frame in the world: a pulled-down local rider leaves the saddle.
void tick_pull_down();
void* ridden_horse_actor(core::EntityId rider);
void horse_walk_finished(core::EntityId id, std::uint32_t sequence, std::uint8_t status,
                         double now);
// Detach departing/changed native rider pairs before either body is retired; never mounts.
template <class Entities>
void detach_retiring_riders(const Entities& entities);

// Retire departures before any network character factory runs.
template <class Entities>
void retire_native_horses(const Entities& entities, void* entity_system);

// Every frame, with the replicated entities (instantiated for the client's mirror).
template <class Entities>
void tick_native_horses(const Entities& entities, void* actor_system, void* entity_system,
                        double now);
template <class Entities>
void tick_native_riders(const Entities& entities, core::EntityId own, void* local_actor,
                        void* actor_system, double now);

}  // namespace kcds::game_client
