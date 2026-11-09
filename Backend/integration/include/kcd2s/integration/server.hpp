// Headless authoritative server tick pipeline, engine-agnostic so it runs
// under the synthetic-world tests and, later, behind the real game server binary.
//
// Server::tick() per client: drain acks/upstream -> (caller sims the World) -> interest query ->
// per-client snapshot build -> send over the transport Connection on channel 1. The World is the
// single source of truth; this class owns no game logic.
#ifndef KCD2S_INTEGRATION_SERVER_HPP
#define KCD2S_INTEGRATION_SERVER_HPP

#include <optional>
#include "kcd2s/integration/body_state_publication.hpp"
#include "kcd2s/integration/player_input_publication.hpp"
#include "kcd2s/integration/appearance.hpp"
#include "kcd2s/integration/body_carry.hpp"
#include "kcd2s/integration/carry.hpp"
#include "kcd2s/integration/client_natives.hpp"
#include "kcd2s/integration/freecam.hpp"
#include "kcd2s/integration/mount_order.hpp"
#include "kcd2s/integration/needs.hpp"
#include "kcd2s/integration/grime.hpp"
#include "kcd2s/integration/quests.hpp"
#include "kcd2s/integration/world_markers.hpp"
#include "kcd2s/integration/world_time.hpp"
#include "kcd2s/integration/gates.hpp"
#include "kcd2s/integration/world_weather.hpp"
#include "kcd2s/integration/doors.hpp"
#include "kcd2s/integration/lights.hpp"
#include "kcd2s/integration/graves.hpp"
#include "kcd2s/integration/smart_objects.hpp"
#include "kcd2s/integration/recipe_tables.hpp"
#include "kcd2s/integration/placed_objects.hpp"
#include "kcd2s/integration/carts.hpp"
#include "kcd2s/integration/previews.hpp"
#include "kcd2s/integration/gizmos.hpp"
#include "kcd2s/integration/pickpocket.hpp"
#include "kcd2s/integration/player_animation.hpp"
#include "kcd2s/integration/combat_marker.hpp"
#include "kcd2s/integration/character_admission.hpp"
#include "kcd2s/integration/restricted_area_marker.hpp"
#include "kcd2s/integration/third_person.hpp"
#include "kcd2s/integration/camera_shot.hpp"
#include "kcd2s/integration/weapon_hits.hpp"
#include "kcd2s/integration/world_ray.hpp"
#include "kcd2s/integration/cuffs.hpp"
#include "kcd2s/integration/frozen.hpp"
#include "kcd2s/integration/lobby.hpp"
#include "kcd2s/integration/player_attach.hpp"
#include "kcd2s/integration/noclip_allowed.hpp"
#include "kcd2s/integration/interaction_rules.hpp"
#include "kcd2s/integration/npc_barks.hpp"
#include "kcd2s/integration/voice_messages.hpp"
#include "kcd2s/integration/whistle.hpp"
#include "kcd2s/integration/level_travel.hpp"
#include "kcd2s/integration/crafting.hpp"
#include "kcd2s/integration/dialogue.hpp"
#include "kcd2s/integration/dice.hpp"
#include "kcd2s/integration/cookpot.hpp"
#include "kcd2s/integration/crafting_table.hpp"
#include "kcd2s/integration/dice_view.hpp"
#include "kcd2s/integration/gathering.hpp"
#include "kcd2s/integration/vendor.hpp"
#include "kcd2s/integration/custom_buffs.hpp"
#include "kcd2s/integration/interactions.hpp"
#include "kcd2s/integration/ladders_stones.hpp"
#include "kcd2s/integration/siege.hpp"
#include "kcd2s/integration/pillory.hpp"
#include "kcd2s/integration/level_edits.hpp"
#include "kcd2s/integration/session_end.hpp"
#include "kcd2s/integration/shots.hpp"
#include "kcd2s/integration/world_items.hpp"
#include "kcd2s/integration/combat.hpp"
#include "kcd2s/integration/chat.hpp"
#include "kcd2s/integration/horse.hpp"
#include "kcd2s/integration/melee_reach.hpp"
#include "kcd2s/integration/movement_check.hpp"
#include "kcd2s/integration/npc.hpp"
#include "kcd2s/integration/movement_mode.hpp"
#include "kcd2s/integration/buffs.hpp"
#include "kcd2s/integration/inventory.hpp"
#include "kcd2s/integration/damage.hpp"
#include "kcd2s/integration/item_overrides.hpp"
#include "kcd2s/integration/item_types.hpp"
#include "kcd2s/integration/stash.hpp"
#include "kcd2s/integration/player_stats.hpp"
#include "kcd2s/integration/progression.hpp"
#include "kcd2s/integration/world_config.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "kcd2s/core/threading.hpp"

#include "kcd2s/core/ids.hpp"
#include "kcd2s/replication/interest.hpp"
#include "kcd2s/replication/schema.hpp"
#include "kcd2s/replication/snapshot.hpp"
#include "kcd2s/replication/world.hpp"
#include "kcd2s/transport/connection.hpp"
#include "kcd2s/transport/datagram.hpp"

namespace kcd2s::integration {

using core::EntityId;
using core::PeerId;
using replication::ClientRepl;
using replication::InterestConfig;
using replication::InterestGrid;
using replication::SchemaRegistry;
using replication::World;
using transport::Connection;
using transport::IDatagramLink;

// Per-peer and aggregate snapshot byte budgets. Budgets are long-run rates with a
// bounded burst: every tick each peer earns min(peer_bytes_per_tick, total_bytes_per_tick / peers)
// of credit, capped at `burst_bytes`, and its snapshot may spend what it holds. Entries that do not
// fit are deferred by priority (own and owned entities first, then combat, starvation and
// distance) and retried on later ticks; a deferred UPDATE loses no field. If the top optional
// entry does not fit, it may be sent alone beyond the budget once credit is full or an entry has
// waited 60 ticks. Smaller updates cannot keep a large construct waiting forever. Debt is
// limited to one burst. Reliable channels are not budgeted here. All zero disables the budget.
struct SnapshotBudget {
  std::uint32_t peer_bytes_per_tick = 0;   // 0 = no per-peer limit
  std::uint64_t total_bytes_per_tick = 0;  // 0 = no aggregate limit
  std::uint32_t burst_bytes = 0;           // credit cap; 0 = 4 x the per-tick share, >= 4096
};

// Cumulative snapshot budget outcome (instrumentation for the opt-in server metrics).
struct SnapshotBudgetCounters {
  // Every how many ticks a peer currently gets a snapshot.
  std::size_t snapshot_stride = 1;
  std::uint64_t ticks = 0;
  std::uint64_t snapshots = 0;
  std::uint64_t snapshot_bytes = 0;
  std::uint64_t budget_limited_snapshots = 0;  // snapshots that deferred at least one entry
  std::uint64_t deferred_entries = 0;          // CONSTRUCT or UPDATE entries deferred
  std::uint64_t deferred_constructs = 0;
  std::uint64_t overdraw_snapshots = 0;       // the top entry was sent beyond the budget
  std::uint64_t aggregate_limited_ticks = 0;  // the aggregate share was below the peer budget
  std::uint64_t largest_snapshot_bytes = 0;
  std::uint64_t update_cache_hits = 0;
  std::uint64_t update_cache_misses = 0;
  std::uint64_t update_cache_fallbacks = 0;
  std::uint64_t update_cache_busy_fallbacks = 0;
  std::uint64_t update_cache_variant_fallbacks = 0;
  std::uint64_t update_cache_memory_fallbacks = 0;
  std::uint64_t update_cache_phase_fallbacks = 0;
};

// Cumulative opt-in diagnostics, aggregated serially after all snapshot workers join. Per-viewer
// elapsed durations overlap when workers run in parallel: their sums are not wall or CPU time.
struct SnapshotProfileCounters {
  std::uint64_t snapshots = 0;
  double serial_setup_seconds = 0;
  double interest_seconds = 0;
  double preparation_seconds = 0;
  double snapshot_call_seconds = 0;
  double bookkeeping_seconds = 0;
  replication::SnapshotTimings builder;
};

class Server {
 public:
  explicit Server(const SchemaRegistry& schema, InterestConfig cfg = {})
      : world_(schema), grid_(cfg) {
  }

  World& world() {
    return world_;
  }
  const World& world() const {
    return world_;
  }

  // Register a client reachable over `link` (non-owning), viewing from entity `viewer` with the
  // given interest range. Returns its PeerId.
  PeerId add_client(IDatagramLink* link, EntityId viewer, float range);

  // Disconnect a client. Any entity it owned is re-delegated: the server elects a new owner
  // (nearest remaining client whose viewer is within its interest range, else server-owned),
  // bumps the entity's forced-state epoch to fence the departed owner's in-flight input, and sends
  // the new owner a SET_OWNER. Server stays authoritative throughout. No-op for an unknown peer.
  void remove_client(PeerId p);

  // Despawn an entity: remove it from the world AND the interest grid, so it leaves every client's
  // AOI next tick and is cleanly DESTRUCTed (never left dangling in the grid).
  void despawn(EntityId e);

  // Soft per-client byte budget per snapshot (0 = unlimited). Legacy per-snapshot cap without
  // credit or mandatory entries; set_snapshot_budget() takes precedence when it is enabled.
  void set_byte_budget(std::uint32_t b) {
    byte_budget_ = b;
  }
  void set_snapshot_budget(const SnapshotBudget& budget) {
    budget_ = budget;
  }
  // Movement steps refused since the start (validation with reasons).
  std::uint64_t movement_refusals() const {
    return movement_refusals_;
  }
  // What the server does with an impossible movement step (observe by default: log it).
  void set_movement_check(MovementCheckMode mode) {
    movement_mode_ = mode;
  }
  // Melee contacts out of reach since the start, and what the server does with one (observe by
  // default: log it). Off also stops keeping the players' pose history.
  std::uint64_t melee_refusals() const {
    return melee_refusals_;
  }
  void set_melee_reach_check(MeleeReachMode mode) {
    melee_reach_mode_ = mode;
    if (mode == MeleeReachMode::Off) {
      pose_histories_.clear();
    }
  }
  // Distance rates (on by default): far entities' UPDATEs go to a client less often.
  void set_distance_rates(bool on) {
    distance_rates_ = on;
  }
  const SnapshotBudget& snapshot_budget() const {
    return budget_;
  }
  const SnapshotBudgetCounters& budget_counters() const {
    return budget_counters_;
  }
  // Configure only between ticks. Disabled by default; no diagnostic clock calls when false.
  void set_snapshot_profiling(bool enabled) {
    snapshot_profiling_ = enabled;
  }
  const SnapshotProfileCounters& snapshot_profile_counters() const {
    return snapshot_profile_counters_;
  }

  // Record a combat exchange between two entities. For a few seconds both rank above other
  // optional snapshot entries, and above everything else for the clients controlling them.
  // Relayed combat events call this; it is public for scripted combat and tests.
  void note_combat(EntityId a, EntityId b);

  // Interest hysteresis in world units: an entity enters a client's view within its range and
  // leaves only beyond range + hysteresis, so an entity near the edge does not flap between
  // CONSTRUCT and DESTRUCT. 0 (the default) means enter and leave at the same distance.
  void set_interest_hysteresis(float meters) {
    hysteresis_ = meters > 0.0f ? meters : 0.0f;
  }
  float interest_hysteresis() const {
    return hysteresis_;
  }

  // Optional shared budget for network players, NPCs and horses, including the local avatar.
  // Zero preserves unrestricted interest. Retiring bodies reserve their slots until DESTRUCT ACK.
  void set_character_limit(unsigned limit) {
    character_limit_ = limit;
  }
  unsigned character_limit() const {
    return character_limit_;
  }
  CharacterAdmissionStats character_admission(PeerId peer) const;
  CharacterAdmissionStats character_admission_totals() const;

  // Grant ownership of an entity to a peer: sets slot.owner and sends a reliable SET_OWNER control
  // message so the owner can author its AUTH_OWNER fields upstream. No-op for an unknown
  // entity/peer.
  void set_owner(EntityId e, PeerId owner);
  bool configure_world(PeerId peer, const WorldConfig& config);
  bool set_dimension(EntityId entity, std::uint32_t world);
  // `player` names an entity owned by the desired connection; zero clears it.
  // The restriction captures the connection, not future owners of that entity.
  bool set_visible_to(EntityId entity, EntityId player = core::kInvalidEntity);
  // Current interest set of a peer (bounded range with hysteresis, dimension and private
  // visibility scope, its avatar and owned in-scope entities) as of the latest tick.
  std::vector<EntityId> visible_entities(PeerId peer) const;

  // Forced-state fence (§7): bump the entity's epoch and notify the owner. Owner input carrying the
  // old epoch is rejected until the owner adopts the bump (FORCE_STATE control event).
  void force_state(EntityId e);

  // Native force (§7): authoritatively teleport an existing entity to `pos`, fence stale owner
  // input by bumping the epoch, and deliver a FORCE_XFORM control event (carrying the bumped epoch)
  // to every in-scope client that already holds the entity. Returns false for an unknown entity or
  // non-finite coordinates; the deliberate server-side authority never produces an engine call with
  // an invalid target. Delivery honours the same visibility gate as every other outbound path.
  // `correction`: the movement check puts the player back; its owner is told so (kForceCorrection)
  // and may blend a small one in.
  bool force_transform(EntityId e, core::Vec3 pos, bool correction = false);

  // Native force of an entity's rotation, with the same epoch fence and delivery as
  // force_transform, carried by FORCE_ROT. Returns false for an unknown entity, a type without a
  // replicated Quat "rotation" field, or a rotation that is not a finite unit quaternion.
  bool force_rotation(EntityId e, core::Quat rotation);

  // Build per-client snapshots on a thread pool (the per-client build is read-only on the world, so
  // it fans out safely). `threads==0` => hardware_concurrency. Only used when client count >= 2.
  void enable_parallel(unsigned threads = 0);
  // Worker threads of the parallel snapshot builder (0 = serial build).
  std::size_t snapshot_workers() const {
    return pool_ ? pool_->size() : 0;
  }
  // Wall times of the most recent tick's phases (instrumentation): receiving what every client
  // sent (decoding, handling, applying owner input), building every client's snapshot, and
  // queueing and flushing them.
  double last_receive_seconds() const {
    return last_receive_seconds_;
  }
  double last_build_seconds() const {
    return last_build_seconds_;
  }
  double last_send_seconds() const {
    return last_send_seconds_;
  }

  // One server tick (§9). snapTick is the World's current tick; the caller advances the World tick
  // and performs sim writes before calling this.
  void tick(double now);
  std::vector<ChatSubmission> take_chat() {
    auto result = std::move(chat_inbox_);
    chat_inbox_.clear();
    return result;
  }
  bool send_script(PeerId peer, const sdk::Value& value);
  // Players knocked out by a lethal hit since the last call (KNOCKED_OUT).
  std::vector<EntityId> take_knockouts() {
    auto result = std::move(knockout_inbox_);
    knockout_inbox_.clear();
    return result;
  }
  // The names players connected under since the last call, cleaned (NICKNAME); a
  // name with nothing left after cleaning is not reported.
  std::vector<std::pair<EntityId, std::string>> take_nicknames() {
    auto result = std::move(nickname_inbox_);
    nickname_inbox_.clear();
    return result;
  }
  // The XP players' games produced and held back for the server since the last call
  // (XP_GAINING), oldest first; at most kMaxXpGainsPerSecond per client.
  std::vector<std::pair<EntityId, XpGain>> take_xp_gains() {
    auto result = std::move(xp_gain_inbox_);
    xp_gain_inbox_.clear();
    return result;
  }
  // The perks players chose on their perk screens and held back for the server since the last
  // call (PERK_LEARNING), oldest first; at most kMaxPerkLearningsPerSecond per client.
  std::vector<std::pair<EntityId, PerkId>> take_perk_learnings() {
    auto result = std::move(perk_learning_inbox_);
    perk_learning_inbox_.clear();
    return result;
  }
  // The status effects players' clients reported since the last call (BUFFS), oldest
  // first; at most kMaxBuffReportsPerSecond per client.
  std::vector<std::pair<EntityId, std::vector<BuffEntry>>> take_buff_reports() {
    return std::exchange(buff_report_inbox_, {});
  }
  // The effects of claimed kinds players' clients turned down since the last call (BUFF_BLOCKED),
  // oldest first; at most kMaxBuffBlocksPerSecond per client.
  std::vector<std::pair<EntityId, Guid>> take_buff_blocks() {
    return std::exchange(buff_block_inbox_, {});
  }
  // What players' clients reported about their inventories since the last call
  // (INVENTORY_REPORT), oldest first: the revision their game shows, and once per
  // connection the census of what it held. At most kMaxInventoryReportsPerSecond per client.
  std::vector<std::pair<EntityId, InventoryReport>> take_inventory_reports() {
    return std::exchange(inventory_report_inbox_, {});
  }
  // The containers' locks players opened in their games since the last call (STASH_UNLOCK, issue),
  // oldest first; at most kMaxStashUnlocksPerSecond per client. Taken before the inventory
  // reports, so an unlock reaches the scripts before the open its client sent after it.
  // What each client's game build gave it (CLIENT_NATIVES), once per connection, oldest first.
  std::vector<std::pair<EntityId, ClientNatives>> take_client_natives() {
    return std::exchange(client_natives_inbox_, {});
  }
  std::vector<std::pair<EntityId, StashUnlock>> take_stash_unlocks() {
    return std::exchange(stash_unlock_inbox_, {});
  }
  // The quests players started or stopped following in their games since the last call
  // (QUEST_TRACKING), oldest first; at most kMaxQuestTrackingPerSecond per client.
  std::vector<std::pair<EntityId, QuestTracking>> take_quest_tracking() {
    return std::exchange(quest_tracking_inbox_, {});
  }
  // The health players' own games took off them since the last call (PLAYER_DAMAGE),
  // oldest first; at most kMaxDamageReportsPerSecond per client.
  std::vector<std::pair<EntityId, PlayerDamage>> take_player_damage() {
    return std::exchange(damage_inbox_, {});
  }
  // The blows players' clients hold for the server's ruling since the last call (PLAYER_HIT),
  // oldest first; at most kMaxDamageReportsPerSecond per client.
  std::vector<std::pair<EntityId, PlayerHitReport>> take_player_hits() {
    return std::exchange(hit_inbox_, {});
  }
  // The attacks players swung at other players since the last call: the attacker and
  // the opponent of each attack press the server relayed, oldest first.
  std::vector<std::pair<EntityId, EntityId>> take_player_attacks() {
    return std::exchange(attack_inbox_, {});
  }
  // Where the items players' carries let go of lie since the last call (CARRY_REPORT),
  // oldest first. At most kMaxCarryReportsPerSecond per client.
  std::vector<std::pair<EntityId, CarryPutDown>> take_carry_put_downs() {
    return std::exchange(carry_put_down_inbox_, {});
  }
  // The level's items players' games would take up since the last call (WORLD_PICKUP, issue),
  // oldest first. At most kMaxWorldPickupsPerSecond per client.
  std::vector<std::pair<EntityId, std::string>> take_world_pickups() {
    return std::exchange(world_pickup_inbox_, {});
  }
  // What players' games did with carts since the last call (CART_REPORT), oldest
  // first. At most kMaxCartReportsPerSecond per client.
  std::vector<std::pair<EntityId, CartReport>> take_cart_reports() {
    return std::exchange(cart_report_inbox_, {});
  }
  // Where the rigid props players' games simulate are since the last call (PROP_REPORT, issue),
  // oldest first. At most kMaxPropReportsPerSecond per client.
  std::vector<std::pair<EntityId, PropReport>> take_prop_reports() {
    return std::exchange(prop_report_inbox_, {});
  }
  // The projectiles of players' shots that landed since the last call (SHOT_LANDED),
  // oldest first: each matched one of the player's shots still in flight, which it ends.
  std::vector<std::pair<EntityId, ShotLanded>> take_shot_landings() {
    return std::exchange(shot_landed_inbox_, {});
  }
  // What thieves' clients reported since the last call (PICKPOCKET), oldest first:
  // requests to rob a player and how thefts ended.
  std::vector<std::pair<EntityId, PickpocketReport>> take_pickpocket_reports() {
    return std::exchange(pickpocket_inbox_, {});
  }
  // The doors players' clients reported since the last call (DOOR_REPORT), oldest
  // first.
  std::vector<std::pair<EntityId, std::vector<DoorEntry>>> take_door_reports() {
    return std::exchange(door_report_inbox_, {});
  }
  // The doors players used since the last call (DOOR_USE), oldest first.
  std::vector<std::pair<EntityId, DoorUse>> take_door_uses() {
    return std::exchange(door_use_inbox_, {});
  }
  // The answers to the nearest-door questions since the last call (DOOR_NEAREST_REPLY).
  std::vector<std::pair<EntityId, DoorNearestReply>> take_door_replies() {
    return std::exchange(door_reply_inbox_, {});
  }
  // The answers to the rays players' clients traced since the last call (WORLD_RAYCAST_REPLY),
  // oldest first.
  std::vector<std::pair<EntityId, WorldRayReply>> take_world_ray_replies() {
    return std::exchange(world_ray_inbox_, {});
  }
  // The camera shots that ended on players' clients without being cleared (CAMERA_SHOT_ENDED),
  // oldest first.
  std::vector<std::pair<EntityId, CameraShotEnd>> take_camera_shot_ends() {
    return std::exchange(camera_shot_end_inbox_, {});
  }
  // Carriers' pick-ups and let-gos of a player's body since the last call (BODY_CARRY_REPORT).
  std::vector<std::pair<EntityId, BodyCarryReport>> take_body_carry_reports() {
    return std::exchange(body_carry_inbox_, {});
  }
  // How the requests of the server's animation orders ended in the players' games since the last
  // call (ANIMATION_END), oldest first. At most kMaxAnimationEndsPerSecond per client.
  std::vector<std::pair<EntityId, AnimationEnd>> take_animation_ends() {
    return std::exchange(animation_end_inbox_, {});
  }
  // The props players' animations took into a hand or put down since the last call
  // (ANIMATION_PROP_CUE), oldest first, at most kMaxAnimationPropCuesPerSecond per
  // client.
  std::vector<std::pair<EntityId, AnimationPropCue>> take_animation_prop_cues() {
    return std::exchange(animation_prop_cue_inbox_, {});
  }
  // What the players' swings met that is not a body since the last call (WEAPON_HIT),
  // oldest first: each within kWeaponHitReach of where the server has that player, at most
  // kMaxWeaponHitsPerSecond per client.
  std::vector<std::pair<EntityId, WorldRayHit>> take_weapon_hits() {
    return std::exchange(weapon_hit_inbox_, {});
  }
  // The graves players' clients reported since the last call (GRAVE kReport and kDug, issue),
  // oldest first: a dug one within kGraveDigReach of where the server has that player, at
  // most kMaxGraveMessagesPerSecond per client.
  std::vector<std::pair<EntityId, Grave>> take_graves() {
    return std::exchange(grave_inbox_, {});
  }
  // The progression snapshots players' clients reported since the last call (PROGRESSION),
  // oldest first.
  std::vector<std::pair<EntityId, Progression>> take_progressions() {
    auto result = std::move(progression_inbox_);
    progression_inbox_.clear();
    return result;
  }
  // Sends REVIVE to the player's client. False when the player is unknown or not knocked out.
  bool revive(EntityId player);
  // Sends KNOCK_OUT to the player's client: knocked out with `health` (finite, positive). False
  // when the player is unknown, has no world or is knocked out already; KNOCKED_OUT follows.
  bool knock_out(EntityId player, float health);
  // Sends the player's knock-out vision (percent, 0-100) to its client. False when the player is
  // unknown or the value is out of range.
  bool set_knockout_vision(EntityId player, std::uint8_t percent);
  // Tells the player's client to end melee combat with the opponent (COMBAT_CANCEL).
  // False when the player has no world.
  bool request_combat_cancel(EntityId player, EntityId opponent);
  // Tells the player's client to start melee combat with the opponent (COMBAT_START): it locks
  // the local player on them. False when the player has no world or is the opponent.
  bool request_combat_start(EntityId player, EntityId opponent);
  // Tells the player's client whether it shows the game's combat marker (COMBAT_MARKER, issue).
  // False when the player is unknown.
  bool send_combat_marker(EntityId player, bool shown);
  // Tells the player's client that the server ends its session and why (SESSION_END, issue);
  // the caller drops the player afterwards. False when the player is unknown or the
  // reason is too long.
  bool send_session_end(EntityId player, const SessionEnd& end);
  // Tells the player's client whether it shows the game's restricted-area marker
  // (RESTRICTED_AREA_MARKER). False when the player is unknown.
  bool send_restricted_area_marker(EntityId player, bool shown);
  // Tells the player's client which third-person mode applies (THIRD_PERSON): 0
  // allowed, 1 disabled, 2 forced. False when the player is unknown or the mode is not one of
  // those.
  bool send_third_person(EntityId player, std::uint8_t mode);
  // Asks a ready player's client to trace a ray (WORLD_RAYCAST). False when the player
  // has no ready world or the ray is not valid_ray.
  bool send_world_raycast(EntityId player, const WorldRayRequest& ray);
  // Films a ready player's camera from a shot, or gives their own view back (CAMERA_SHOT,
  // CAMERA_CLEAR). False when the player has no ready world or the shot cannot travel.
  bool send_camera_shot(EntityId player, const CameraShot& shot);
  bool send_camera_clear(EntityId player);
  // Tells the player's client that the player is cuffed or not (CUFFS). False when
  // the player is unknown.
  bool send_cuffs(EntityId player, bool cuffed);
  // FROZEN to the player's own client (Player.frozen); false when no client views it.
  bool send_frozen(EntityId player, bool frozen);
  // Tells the player's client it is held out of the world or let in (LOBBY). False when the player
  // has no connection.
  bool send_lobby(EntityId player, bool held);
  // PLAYER_ATTACH to every client (Player.attachTo), each told whether it is about its
  // own player.
  void send_player_attach(const PlayerAttach& attach);
  // NOCLIP_ALLOWED to the player's own client (Player.noclipAllowed); false when no
  // client views it.
  bool send_noclip_allowed(EntityId player, bool allowed);
  // player.mount (mount_order.hpp): the player's game gets on the horse. False when
  // the player is not in a ready world or the order is not one.
  bool send_mount_order(EntityId player, const MountOrder& order);
  // player.dismount and a refused mount (DISMOUNT_ORDER): the player's game gets off.
  bool send_dismount_order(EntityId player);
  // Horse.buckRider: the horse this player rides rears and throws them off.
  bool send_rear_order(EntityId player);
  // Horse.dragRider: to one of the two players, the attacker and the rider.
  bool send_pull_down_order(EntityId player, EntityId attacker, EntityId rider);
  // Tells the player's client which of the game's own interactions it allows
  // (INTERACTION_RULES). False when the player is unknown.
  bool send_interaction_rules(EntityId player, std::uint16_t allowed);
  // Tells the player's client to go into the pillory of that holder entity or to leave it
  // (PILLORY). False when the player is unknown or has no world yet, or the order
  // cannot travel.
  bool send_pillory(EntityId player, const PilloryOrder& order);
  // What the players' clients said about a pillory since the last call (PILLORY_RELEASED, issue),
  // oldest first: the reason (kPilloryLockpicked, kPilloryLeft, or kPilloryLockOpened with
  // the name of the lock opened in that player's game). At most kMaxPilloryReportsPerSecond per
  // client.
  std::vector<std::pair<EntityId, PilloryReport>> take_pillory_releases() {
    return std::exchange(pillory_release_inbox_, {});
  }
  // Gives the player's client its whole list of world interactions (INTERACTIONS).
  // False when the player is unknown or the list cannot travel.
  bool send_interactions(EntityId player, const std::vector<Interaction>& list);
  // The interactions the players used since the last call (INTERACTION_FIRED), oldest
  // first. At most kMaxInteractionsFiredPerSecond per client.
  std::vector<std::pair<EntityId, std::uint32_t>> take_interactions_fired() {
    return std::exchange(interaction_inbox_, {});
  }
  // Voice chat's control messages (kcd2s/integration/voice_messages.hpp).
  bool send_voice_state(EntityId player, const VoiceState& state);
  bool send_voice_muffle(EntityId player, EntityId talker, std::uint8_t amount);
  std::vector<std::pair<EntityId, std::array<std::uint8_t, 32>>> take_voice_keys() {
    return std::exchange(voice_key_inbox_, {});
  }
  std::vector<std::pair<EntityId, std::uint8_t>> take_voice_tier_requests() {
    return std::exchange(voice_tier_inbox_, {});
  }
  // The players who whistled since the last call (WHISTLE), at most one a second each.
  std::vector<EntityId> take_whistles() {
    return std::exchange(whistle_inbox_, {});
  }
  // Where the fast travels players began since the last call go (LEVEL_TRAVEL), at most one a
  // second each.
  std::vector<std::pair<EntityId, TravelTarget>> take_travel_targets() {
    return std::exchange(travel_target_inbox_, {});
  }
  // What server NPCs said on their simulators since the last call (NPC_BARK): (simulator, bark).
  std::vector<std::pair<EntityId, NpcBark>> take_npc_barks() {
    return std::exchange(npc_bark_inbox_, {});
  }
  // Tells the player's client to whistle (whistler 0) or that the player `whistler` whistled
  // near them (WHISTLE). False when the player is unknown or has no ready world.
  bool send_whistle(EntityId player, std::uint32_t whistler);
  // Shows a conversation's page on the player's screen, or takes the conversation off it
  // (DIALOGUE_PAGE and DIALOGUE_CLOSE). False when the player has no ready world or
  // the page cannot travel.
  bool send_dialogue_page(EntityId player, const DialoguePage& page);
  bool send_dialogue_close(EntityId player, const DialogueClose& close);
  // The rows players picked and the conversations they ended on their side since the last call
  // (DIALOGUE_CHOICE and DIALOGUE_CANCEL), oldest first. At most
  // kMaxDialogueReportsPerSecond per client.
  std::vector<std::pair<EntityId, DialogueChoice>> take_dialogue_choices() {
    return std::exchange(dialogue_choice_inbox_, {});
  }
  std::vector<std::pair<EntityId, DialogueCancel>> take_dialogue_cancels() {
    return std::exchange(dialogue_cancel_inbox_, {});
  }
  // What players' games did at crafting stations since the last call (CRAFT_REPORT),
  // oldest first; at most kMaxCraftReportsPerSecond per client. An Ask waits for
  // send_craft_verdict.
  std::vector<std::pair<EntityId, CraftReport>> take_craft_reports() {
    return std::exchange(craft_report_inbox_, {});
  }
  // What lies on the alchemy tables players brew at (CRAFT_TABLE), oldest first; at
  // most kMaxCraftTablesPerSecond per client.
  std::vector<std::pair<EntityId, CraftTable>> take_craft_tables() {
    return std::exchange(craft_table_inbox_, {});
  }
  // Whether the player's Ask may go on (CRAFT_VERDICT). False when the player has no ready world.
  bool send_craft_verdict(EntityId player, const CraftVerdict& verdict);
  // The moves players made in their dice matches since the last call (DICE_MOVE),
  // oldest first; at most kMaxDiceMovesPerSecond per client.
  std::vector<std::pair<EntityId, DiceMove>> take_dice_moves() {
    return std::exchange(dice_move_inbox_, {});
  }
  // Where the dice table's things are in the throwers' games (DICE_VIEW), oldest first; at most
  // kMaxDiceViewsPerSecond per client.
  std::vector<std::pair<EntityId, DiceView>> take_dice_views() {
    return std::exchange(dice_view_inbox_, {});
  }
  // A dice match as a player nearby sees it (DICE_WATCH). False when the player has no ready
  // world or the view cannot travel.
  bool send_dice_watch(EntityId player, const DiceWatch& watch);
  // Whether a resource answers a dice table's use (DICE_TABLES). False when the
  // player has no ready world.
  bool send_dice_tables(EntityId player, const DiceTables& tables);
  // A dice match as the player sees it (DICE_STATE). False when the player has no ready world or
  // the state cannot travel.
  bool send_dice_state(EntityId player, const DiceState& state);
  // Herbs players' games picked since the last call (GATHER_REPORT), oldest first; at
  // most kMaxGatherReportsPerSecond per client. Each waits for send_gather_verdict.
  std::vector<std::pair<EntityId, GatherReport>> take_gather_reports() {
    return std::exchange(gather_report_inbox_, {});
  }
  // Whether a player's pick stands (GATHER_VERDICT), and plants picked or grown back in the
  // player's world (GATHER_PLANTS). False when the player has no ready world or the plants
  // cannot travel.
  bool send_gather_verdict(EntityId player, const GatherVerdict& verdict);
  bool send_gather_plants(EntityId player, const GatherPlants& plants);
  // How stew pots of the player's world look (POT_LOOKS). False when the player
  // has no ready world or the looks cannot travel.
  bool send_pot_looks(EntityId player, const PotLooks& looks);
  // Shows a vendor's shelf on the player's shop screen, or takes the screen down (VENDOR_SHOW and
  // VENDOR_CLOSE). False when the player has no ready world or the shelf
  // cannot travel.
  bool send_vendor_show(EntityId player, const VendorShow& show);
  bool send_vendor_close(EntityId player, const VendorClose& close);
  // The trader's answer to a haggle offer of the player (VENDOR_HAGGLE_REPLY, 0.4.1).
  bool send_vendor_haggle_reply(EntityId player, const VendorHaggleReply& reply);
  // The baskets players confirmed, the vendor screens that went away on their side and the haggle
  // offers they made since the last call (VENDOR_TRADE, VENDOR_CLOSED and VENDOR_HAGGLE), oldest
  // first. At most kMaxVendorReportsPerSecond per client.
  std::vector<std::pair<EntityId, VendorTrade>> take_vendor_trades() {
    return std::exchange(vendor_trade_inbox_, {});
  }
  std::vector<std::pair<EntityId, VendorClosed>> take_vendor_closes() {
    return std::exchange(vendor_closed_inbox_, {});
  }
  std::vector<std::pair<EntityId, VendorHaggle>> take_vendor_haggles() {
    return std::exchange(vendor_haggle_inbox_, {});
  }
  // Tells ready players' clients about a siege engine (SIEGE_ENGINE): one player, or
  // every one with a ready world (0). A spawn reaches only the players whose dimension sees
  // the engine's (the global world is seen by every world). `operator_id` is the player who works
  // it (0 for none): that player's client is told the engine is theirs. False when nobody was
  // told or the order cannot travel.
  bool send_siege_engine(EntityId player, SiegeEngineOrder order, EntityId operator_id);
  // Tells the ready players in the shot's dimension about a shot let go (SIEGE_SHOT); the
  // reporter's client is told to say where it came down.
  bool send_siege_shot(SiegeShot shot, std::uint32_t dimension, EntityId reporter);
  // What players asked of siege engines and where the shots they report came down, since the
  // last call, oldest first (SIEGE_USE and SIEGE_IMPACT).
  std::vector<std::pair<EntityId, SiegeUse>> take_siege_uses() {
    return std::exchange(siege_use_inbox_, {});
  }
  std::vector<std::pair<EntityId, SiegeImpact>> take_siege_impacts() {
    return std::exchange(siege_impact_inbox_, {});
  }
  // The events of the players' prop placements since the last call (PLACER_REPORT),
  // oldest first. At most kMaxPlacerReportsPerSecond per client.
  std::vector<std::pair<EntityId, PlacerReport>> take_placer_reports() {
    return std::exchange(placer_report_inbox_, {});
  }
  // Tells ready players' clients about a siege ladder (SIEGE_LADDER): one player, or
  // every one with a ready world (0). A spawn reaches only the players whose dimension sees
  // the ladder's. `actor` is the player who pushes or raises it (0 for none): that player's client
  // is told the step is theirs. A refusal goes to `player` alone. False when nobody was told or
  // the order cannot travel.
  bool send_siege_ladder(EntityId player, SiegeLadderOrder order, EntityId actor);
  // Tells ready players' clients about a stone pile (STONE_PILE), as send_siege_ladder does.
  bool send_stone_pile(EntityId player, const StonePileOrder& order);
  // What players did with ladders and stone piles since the last call, oldest first.
  std::vector<std::pair<EntityId, SiegeLadderReport>> take_siege_ladder_reports() {
    return std::exchange(siege_ladder_inbox_, {});
  }
  std::vector<std::pair<EntityId, StonePileReport>> take_stone_pile_reports() {
    return std::exchange(stone_pile_inbox_, {});
  }
  // Tells the player's client to carry a new item or put the carried one down (CARRY_ORDER).
  // False when the player is unknown or the order cannot travel.
  bool request_carry(EntityId player, const CarryOrder& order);
  // Tells the player's client how to take a blow it holds (HIT_VERDICT). False when
  // the player has no world or the verdict cannot travel.
  bool send_hit_verdict(EntityId player, const HitVerdict& verdict);
  // Whether blows from other players wait for a ruling, and whether other harm does (HIT_RULES):
  // sent to every client with a world on a change, and to each client as its world gets ready.
  void set_hit_rules(bool enabled, bool harm = false);
  // Harm the players' own games are about to take (PLAYER_HARM), oldest first, by player.
  std::vector<std::pair<EntityId, PlayerHarm>> take_player_harms() {
    return std::exchange(player_harm_inbox_, {});
  }
  // The server's name for the players' watermarks (SERVER_NAME): told to each client
  // as its world gets ready. "" tells none.
  void set_server_name(std::string name);
  // Tells the player's client whether its game takes up the level's item it asked for
  // (WORLD_PICKUP_VERDICT). False when the player has no world or the key is malformed.
  bool send_world_pickup_verdict(EntityId player, const std::string& key, bool granted);
  // Tells the player's client to show the game's notice of units it gained or lost (ITEM_NOTICE).
  // False when the player has no world or the notice cannot travel.
  bool send_item_notice(EntityId player, const ItemNotice& notice);
  // Tells a player's client a pickpocketing rule (PICKPOCKET_RULE): whether its game
  // offers Rob, the answer to its request, or that a thief was caught at its pockets.
  bool send_pickpocket_rule(EntityId player, const PickpocketRule& rule);
  // Tells a player's client whether it may fly the free camera (FREECAM).
  bool send_freecam(EntityId player, bool allowed);
  // Tells a player's client whether its game runs the sleep and food needs (NEEDS).
  // False when the player has no world.
  bool send_needs(EntityId player, bool enabled);
  // Tells one player's client about a carry of a body (BODY_CARRY).
  bool send_body_carry(EntityId player, const BodyCarry& carry);
  // Tells a player's own client to play or stop its animation (ANIMATION_ORDER). False
  // when the player has no world or the order cannot travel.
  bool send_animation_order(EntityId player, const AnimationOrder& order);
  // Tells a player's client to clean or soil its body or clothes (GRIME_ORDER).
  bool send_grime_order(EntityId player, const GrimeOrder& order);
  // Tells a ready player's client the world time and how fast it runs (WORLD_TIME).
  // False when the player has no ready world or the time cannot travel.
  bool send_world_time(EntityId player, const WorldTime& time);
  // Tells a ready player's client the weather, its blend and the wind (WORLD_WEATHER).
  // False when the player has no ready world or the weather cannot travel.
  bool send_world_weather(EntityId player, const WorldWeather& weather);
  // Tells a ready player's client a gate's state (GATE). False when the player has no
  // ready world or the gate is not in kGates.
  bool send_gate(EntityId player, const GateState& state);
  // Shows or hides a world marker or a compass and map blip for a ready player (MARKER_SHOW,
  // MARKER_HIDE, BLIP_SHOW, BLIP_HIDE). False when the player has no ready world or
  // the marker cannot travel.
  bool send_marker_show(EntityId player, const MarkerShow& marker);
  bool send_marker_hide(EntityId player, std::uint32_t marker);
  bool send_blip_show(EntityId player, const BlipShow& blip);
  bool send_blip_hide(EntityId player, std::uint32_t blip);
  // Writes a quest into a ready player's journal, removes it, or makes them follow it or stop
  // (QUEST_SHOW, QUEST_HIDE, QUEST_TRACK). False when the player has no ready
  // world or the quest cannot travel.
  bool send_quest_show(EntityId player, const QuestShow& quest);
  bool send_quest_hide(EntityId player, std::uint32_t quest);
  bool send_quest_track(EntityId player, const QuestTracking& track);
  // Tells a ready player's client to set a door (DOOR_SET). False when the player has
  // no ready world or the door cannot travel.
  bool send_door_set(EntityId player, const DoorState& state);
  // Places a smart object a script made, or takes it down (SMART_OBJECT): one player,
  // or every one with a ready world (0); one placed reaches only the players whose dimension
  // sees `dimension`. False when nobody was told or the order cannot travel.
  bool send_smart_object(EntityId player, const SmartObjectOrder& order, std::uint32_t dimension);
  // A grave's state (GRAVE kSet): one player, or every one with a ready world (0).
  // False when nobody was told or the grave cannot travel.
  bool send_grave_set(EntityId player, const Grave& grave);
  // Hangs a door a script made, or takes it down (DOOR_SPAWN): one player, or every
  // one with a ready world (0); a door hung reaches only the players whose dimension sees
  // `dimension`. False when nobody was told or the door cannot travel.
  bool send_door_spawn(EntityId player, const DoorSpawn& door, std::uint32_t dimension);
  // Asks a ready player's client for the door nearest in sight (DOOR_NEAREST).
  bool send_door_nearest(EntityId player, std::uint32_t request);
  // Tells ready players' clients about a prop or a placed effect (PROP and VFX, and):
  // one player, or every one with a ready world (0). A spawn or burst reaches only the
  // players whose dimension sees the order's (the global world is seen by every world). False
  // when nobody was told or the order cannot travel.
  bool send_prop(EntityId player, const PropOrder& order);
  bool send_vfx(EntityId player, const VfxOrder& order);
  // Tells ready players' clients about a point light (LIGHT): one player, or every
  // one with a ready world (0). False when nobody was told or the order cannot travel.
  bool send_light(EntityId player, const LightOrder& order);
  // Tells ready players' clients about a recipe a resource registered (RECIPE_TABLE):
  // one player, or every one with a ready world (0). False when nobody was told or the patch
  // cannot travel.
  bool send_recipe_table(EntityId player, const RecipeTable& table);
  // And one of an alchemy recipe's pictures in the recipe book (RECIPE_PICTURE), the same way.
  bool send_recipe_picture(EntityId player, const RecipePicture& picture);
  // Tells ready players' clients about a cart (CART): one player, or every one with a
  // ready world (0). A spawn reaches only the players whose dimension sees the cart's; a pose
  // reaches everyone but its driver. False when nobody was told or the order cannot travel.
  bool send_cart(EntityId player, const CartOrder& order);
  // Tells one ready player's client about their prop preview or placement (GHOST and PLACER).
  // False when the player has no ready world or the order cannot travel.
  bool send_ghost(EntityId player, const GhostOrder& order);
  bool send_placer(EntityId player, const PlacerOrder& order);
  // Draws or clears a gizmo in one ready player's game, or every one's (0) (GIZMO).
  // False when nobody was told or the order cannot travel.
  bool send_gizmo(EntityId player, const GizmoOrder& order);
  // Tells a thief's client what the victim carries (PICKPOCKET_POCKETS).
  bool send_pickpocket_pockets(EntityId player, const PickpocketPockets& pockets);
  // Tells the player's client (0: every client with a world) which of the level's items are taken
  // up (WORLD_TAKEN). False when no client got it or the list cannot travel.
  bool send_world_taken(EntityId player, const WorldTaken& taken);
  // Whether the level's items are on (WORLD_ITEMS_ENABLED), to one player or everyone
  // (0) whose world is ready.
  bool send_world_items_enabled(EntityId player, bool enabled);
  // The places a script took the level's items away from (WORLD_ITEMS_REMOVED, World
  // .removeLevelItems), to one player or everyone (0).
  bool send_world_items_removed(EntityId player, const std::vector<RemovedItemArea>& areas);
  // Tells the player's ready client (0: every ready client) of a level edit in dimension
  // `world` (LEVEL_EDIT_APPLY); only a player in that world, or in the global world,
  // gets it (all of them, for an edit of the global world). False when no client got it or the
  // edit cannot travel.
  bool send_level_edit(EntityId player, const LevelEdit& edit, std::uint32_t world);
  // Tells the player's ready client (0: every ready client) that a level edit is gone
  // (LEVEL_EDIT_REMOVE).
  bool send_level_edit_remove(EntityId player, std::uint32_t id);
  // Asks the player's client to put on the named parts of a look (APPEARANCE). False when the
  // player is unknown or a name cannot travel.
  bool request_appearance(EntityId player, const AppearanceRequest& request);
  // Asks the player's client to set one of its writable states (SET_STAT, Player.setStat) or to
  // change it by an amount (ADD_STAT, Player.addStat, a relative request). False when the player
  // is unknown, has no world yet, lies knocked out or the value is refused.
  bool request_stat(EntityId player, const StatRequest& request);
  // Sends one of the server's progression orders to the player's client (PROGRESSION_ORDER).
  // False when the player has no world yet or the order cannot travel.
  bool request_progression_order(EntityId player, const ProgressionOrder& order);
  // Sends one of the server's buff orders to the player's client (BUFF_ORDER). False when the
  // player has no world yet or the order cannot travel.
  bool request_buff_order(EntityId player, const BuffOrder& order);
  // Shows or hides one of the resources' own buffs on the player's client (CUSTOM_BUFF, issue).
  // False when the player has no world yet or the order cannot travel.
  bool request_custom_buff(EntityId player, const CustomBuff& buff);
  // Sends the player's whole inventory to its client (INVENTORY_SHOW). False when the player has
  // no world yet or the stock cannot travel.
  bool request_inventory_show(EntityId player, const InventoryShow& show);
  // Sends the server's verdict on one of the player's uses to their client (INVENTORY_CONSUME).
  // False when the player has no world.
  bool request_inventory_consume(EntityId player, const InventoryConsume& consume);
  // The scripts' answer to the batch of changes the player's client sent last
  // (INVENTORY_CHANGES_ACK). False when the player has no world or it answers no open batch.
  bool request_inventory_changes_ack(EntityId player, const InventoryChangesAck& ack);
  // Sends what a container holds to the player holding it (STASH_SHOW).
  // False when the player has no world or the show cannot travel.
  bool request_stash_show(EntityId player, const StashShow& show);
  // Sends item types a resource registered to the player's client, or with player 0 to every
  // client (ITEM_TYPES). False when the types cannot travel.
  bool request_item_types(EntityId player, const ItemTypes& types);
  // Sends what the server changes of the game's own items (weight, price, limit) to the player's
  // client, or with player 0 to every client (ITEM_OVERRIDES). False when the set
  // cannot travel.
  bool request_item_overrides(EntityId player, const ItemOverrides& overrides);
  // Tells the player's client they would take more of a class than its limit (ITEM_LIMIT_NOTICE).
  // False when the text cannot travel.
  bool request_item_limit_notice(EntityId player, const ItemLimitNotice& notice);
  // Sends what the game calls items to the player's client, or with player 0 to every client
  // (ITEM_NAMES). False when the names cannot travel.
  bool request_item_names(EntityId player, const ItemNames& names);
  // Puts a pace rule on the player (MOVEMENT_MODE, Player.setPace). False when the player
  // has no connection.
  bool request_movement_mode(EntityId player, const MovementMode& mode);
  // Asks the player's client to draw its body or stop drawing it (BODY_VISIBLE). False when the
  // player is unknown.
  bool request_body_visible(EntityId player, bool visible);
  // Asks the player's client to draw its body as the mesh at that path, or as itself for ""
  // (DISGUISE). False when the player is unknown or the path cannot travel.
  bool request_disguise(EntityId player, const std::string& model);
  // The items the player wears for a while outside their inventory (WORN), to their
  // own client; false without one or for a list that cannot travel.
  bool request_worn(EntityId player, const std::vector<ItemClassId>& items);
  std::vector<ScriptSubmission> take_scripts() {
    auto result = std::move(script_inbox_);
    script_inbox_.clear();
    return result;
  }
  void send_chat(EntityId player, const ChatMessage& message);
  bool player_ready(EntityId player) const;
  // The player's client reported that the game took its loading screen down for this world.
  bool player_gameplay_ready(EntityId player) const;
  // Same as player_ready() / player_gameplay_ready() for a known connection, without scanning
  // every client.
  bool peer_world_ready(PeerId peer) const;
  bool peer_gameplay_ready(PeerId peer) const;
  // The connection's smoothed round-trip time in seconds (player.ping); 0 for an
  // unknown peer or before the first acknowledgement.
  double peer_rtt_seconds(PeerId peer) const;
  // Start a new native simulation epoch. A nonzero simulator must be gameplay-ready and hold
  // the horse; a mounted horse permits only its rider. Zero makes it dormant without dismounting.
  bool initialize_horse(EntityId horse, EntityId simulator);
  // The resource host's decision on a horse's health: writes HorseHealth and
  // HorseCondition, and raises HorseLife when a knocked-out or dead horse becomes alive again (a
  // wake or revive), so every client makes the body anew. False for an unknown horse, a health
  // outside 0 to kHorseMaxHealth, a state that is none of the three, or alive with no health.
  bool set_horse_health(EntityId horse, float health, std::int64_t state);
  // The HORSE_HEALTHs that changed a horse since the last call, oldest first.
  // The stamina the horses' simulators reported since the last call (HORSE_STAMINA).
  std::vector<HorseStaminaChange> take_horse_stamina_changes() {
    return std::exchange(horse_stamina_inbox_, {});
  }
  std::vector<HorseHealthChange> take_horse_health_changes() {
    return std::exchange(horse_health_inbox_, {});
  }

  // NPCs (kcd2s/integration/npc.hpp). Spawns a NetworkNpc under `reserved`, an
  // id the world reserved for the resource host, or under the world's next id without one, with
  // no simulator; the next election gives it one. kInvalidEntity when the spawn is invalid
  // (valid_npc_spawn) or the world refuses the id.
  EntityId spawn_npc(const NpcSpawn& spawn, EntityId reserved = core::kInvalidEntity);
  // Changes an NPC's name, frozen flag or dimension. False for an unknown NPC or a name that
  // is not valid_npc_name.
  bool set_npc_name(EntityId npc, const std::string& name);
  bool set_npc_frozen(EntityId npc, bool frozen);
  // The share of attacks the NPC blocks or parries (npc.hpp NpcDefense); false for an unknown NPC
  // or a value outside 0 to 1.
  bool set_npc_defense(EntityId npc, float defense);
  // The share of their damage the NPC's blows on players take (NpcDamage), 0 to kNpcDamageMax.
  bool set_npc_damage(EntityId npc, float share);
  // The clip a resource plays on the NPC (npc.hpp NpcAnimation); false for an unknown NPC or a
  // text the field cannot carry.
  bool set_npc_animation(EntityId npc, const std::string& animation);
  // The order a resource gave the NPC (npc.hpp NpcIntent); false for an unknown NPC or a text
  // that is not valid_npc_intent.
  bool set_npc_intent(EntityId npc, const std::string& intent);
  // What the NPC says (npc.hpp NpcSpeech); false for an unknown NPC or a text that is not
  // valid_npc_speech.
  bool set_npc_speech(EntityId npc, const std::string& speech);
  // The NPC's nameplate field (npc.hpp NpcNameplate); false for an unknown NPC or a field that is
  // not valid_npc_nameplate.
  bool set_npc_nameplate(EntityId npc, const std::string& field);
  // How its simulator moves it (npc.hpp NpcKinematic).
  bool set_npc_kinematic(EntityId npc, bool kinematic);
  // Whether the game's brain runs the NPC, and whether a player can lock on to it (npc.hpp
  // NpcBrain, NpcTargetable).
  bool set_npc_brain(EntityId npc, bool brain);
  bool set_npc_targetable(EntityId npc, bool targetable);
  // Whether players can fight the NPC (npc.hpp NpcAttackable).
  bool set_npc_attackable(EntityId npc, bool attackable);
  // Whether players can talk to the NPC (npc.hpp NpcInteractable).
  bool set_npc_interactable(EntityId npc, bool interactable);
  // The level's own person the NPC is (npc.hpp NpcLevelGuid); false for a malformed GUID.
  bool set_npc_level_guid(EntityId npc, const std::string& guid);
  // The style the NPC's mouth moves in, "" for none (npc.hpp NpcTalking).
  bool set_npc_talking(EntityId npc, const std::string& style);
  // Its facial expression, "<fragment>|<tags>" or "" (npc.hpp NpcFace).
  bool set_npc_face(EntityId npc, const std::string& face);
  // Whether a dead animal offers neither the game's Search nor its Gut (npc.hpp NpcCarcassClosed).
  bool set_npc_carcass_closed(EntityId npc, bool closed);
  // Npc.pin: the player whose client runs the NPC while in range (kInvalidEntity: the election).
  // False for an unknown NPC or a player that is none.
  bool pin_npc(EntityId npc, EntityId player);
  // The NPC's character components (npc.hpp NpcAppearance); false for an unknown NPC or a text
  // that is not valid_npc_appearance.
  bool set_npc_appearance(EntityId npc, const std::string& appearance);
  // What a resource dresses the NPC in and arms it with (npc.hpp NpcOutfitFixed): writes `outfit`
  // and marks it fixed. False for an unknown NPC or a text that is not valid_npc_fixed_outfit.
  bool set_npc_outfit(EntityId npc, const std::string& outfit);
  // The resource host's decision on the NPC's health: writes `health` and `alive`, and
  // raises `life` when a dead NPC comes alive (every client makes its body anew). False for an
  // unknown NPC, a health outside 0 to kNpcMaxHealth, or alive with no health.
  bool set_npc_health(EntityId npc, float health, bool alive);
  bool set_npc_dimension(EntityId npc, std::uint32_t world);
  // Npc.teleport: writes the NPC's position and rotation, raises its `teleport` counter and its
  // simulation epoch (so a state its simulator sent before cannot put the body back) and stops
  // it. False for an unknown NPC or a position or rotation valid_body_state would refuse.
  bool teleport_npc(EntityId npc, core::Vec3 position, core::Quat rotation);
  // Gives every NPC the simulator the election chooses (npc.hpp). tick() runs it every
  // kNpcElectionSeconds; tests call it directly.
  void elect_npc_simulators();
  // The changes of NPC simulators since the last call, oldest first.
  std::vector<NpcSimulatorChange> take_npc_simulator_changes() {
    return std::exchange(npc_simulator_inbox_, {});
  }
  // The NPC_HITs taken since the last call, oldest first (npcHit).
  std::vector<NpcHit> take_npc_hits() {
    return std::exchange(npc_hit_inbox_, {});
  }
  // The NPC_USEs taken since the last call, oldest first (npcInteract).
  std::vector<NpcUse> take_npc_uses() {
    return std::exchange(npc_use_inbox_, {});
  }
  // The NPC_ENGAGEs taken since the last call, oldest first: the player locked on to the NPC
  // (npcEngage).
  std::vector<NpcUse> take_npc_engages() {
    return std::exchange(npc_engage_inbox_, {});
  }
  // The NPC_ANIMATION_ENDs taken since the last call, oldest first (npcAnimationEnd).
  std::vector<NpcAnimationEnd> take_npc_animation_ends() {
    return std::exchange(npc_animation_end_inbox_, {});
  }
  // The NPC_INTENT_DONEs taken since the last call, oldest first (npcIntentDone).
  std::vector<NpcIntentDone> take_npc_intent_dones() {
    return std::exchange(npc_intent_done_inbox_, {});
  }

  // Build every client's snapshot payload for `snapTick` without sending (interest grid is synced
  // first, serially). Deterministic order (by PeerId). This is what tick() sends; exposed so tests
  // can compare the serial and parallel builds byte-for-byte.
  std::vector<std::pair<PeerId, std::vector<std::uint8_t>>> build_all(core::Tick snapTick);

  const ClientRepl* client_repl(PeerId p) const {
    auto it = clients_.find(p);
    return it == clients_.end() ? nullptr : &it->second.repl;
  }

  // Total snapshot bytes sent across all clients since construction (bandwidth instrumentation).
  std::uint64_t snapshot_bytes_sent() const {
    return snapshot_bytes_sent_;
  }

  // Reliable messages queued or awaiting acknowledgement across all clients (instrumentation).
  std::size_t outstanding_reliable_total() const;
  // Reliable message copies sent and retransmitted, departed peers included (instrumentation).
  struct ReliableCounters {
    std::uint64_t sent = 0;
    std::uint64_t retransmits = 0;
  };
  ReliableCounters reliable_counters() const;

 private:
  // Queues a control message for the player's client once its world is ready.
  bool send_to_ready_player(EntityId player, const std::vector<std::uint8_t>& message);
  // The same for one player, or every one with a ready world (0).
  bool send_to_ready(EntityId player, const std::vector<std::uint8_t>& message);

  std::vector<ScriptSubmission> script_inbox_;
  std::vector<ChatSubmission> chat_inbox_;
  std::vector<EntityId> knockout_inbox_;
  std::vector<std::pair<EntityId, std::string>> nickname_inbox_;
  std::vector<std::pair<EntityId, Progression>> progression_inbox_;
  std::vector<std::pair<EntityId, XpGain>> xp_gain_inbox_;
  std::vector<std::pair<EntityId, PerkId>> perk_learning_inbox_;
  // XP gains taken per client per second: far above what play produces, low enough that a client
  // cannot flood the resource host.
  static constexpr unsigned kMaxXpGainsPerSecond = 64;
  // Perks asked for per client per second: a player clicks them one at a time.
  static constexpr unsigned kMaxPerkLearningsPerSecond = 8;
  std::vector<std::pair<EntityId, std::vector<BuffEntry>>> buff_report_inbox_;
  std::vector<std::pair<EntityId, Guid>> buff_block_inbox_;
  // Buff reports per client per second: the client reads its buffs four times a second and
  // reports only a change.
  static constexpr unsigned kMaxBuffReportsPerSecond = 8;
  // Blocked effects per client per second: the client already keeps each effect to two a second.
  static constexpr unsigned kMaxBuffBlocksPerSecond = 16;
  std::vector<std::pair<EntityId, InventoryReport>> inventory_report_inbox_;
  // The last inventory epoch given to a connection.
  std::uint32_t last_inventory_epoch_ = 0;
  std::vector<std::pair<EntityId, StashUnlock>> stash_unlock_inbox_;
  std::vector<std::pair<EntityId, ClientNatives>> client_natives_inbox_;
  std::vector<std::pair<EntityId, QuestTracking>> quest_tracking_inbox_;
  std::vector<std::pair<EntityId, PlayerDamage>> damage_inbox_;
  std::vector<std::pair<EntityId, PlayerHitReport>> hit_inbox_;
  bool hit_rules_ = false;
  bool harm_rules_ = false;
  std::vector<std::pair<EntityId, PlayerHarm>> player_harm_inbox_;
  std::string server_name_;
  std::vector<std::pair<EntityId, EntityId>> attack_inbox_;
  std::vector<std::pair<EntityId, CarryPutDown>> carry_put_down_inbox_;
  std::vector<std::pair<EntityId, std::string>> world_pickup_inbox_;
  std::vector<std::pair<EntityId, CartReport>> cart_report_inbox_;
  std::vector<std::pair<EntityId, PropReport>> prop_report_inbox_;
  std::vector<std::pair<EntityId, ShotLanded>> shot_landed_inbox_;
  std::vector<std::pair<EntityId, PickpocketReport>> pickpocket_inbox_;
  std::vector<std::pair<EntityId, std::vector<DoorEntry>>> door_report_inbox_;
  std::vector<std::pair<EntityId, DoorUse>> door_use_inbox_;
  std::vector<std::pair<EntityId, DoorNearestReply>> door_reply_inbox_;
  std::vector<std::pair<EntityId, WorldRayReply>> world_ray_inbox_;
  std::vector<std::pair<EntityId, CameraShotEnd>> camera_shot_end_inbox_;
  std::vector<std::pair<EntityId, BodyCarryReport>> body_carry_inbox_;
  std::vector<std::pair<EntityId, AnimationEnd>> animation_end_inbox_;
  std::vector<std::pair<EntityId, AnimationPropCue>> animation_prop_cue_inbox_;
  std::vector<std::pair<EntityId, WorldRayHit>> weapon_hit_inbox_;
  std::vector<std::pair<EntityId, Grave>> grave_inbox_;
  std::vector<std::pair<EntityId, PilloryReport>> pillory_release_inbox_;
  std::vector<std::pair<EntityId, std::uint32_t>> interaction_inbox_;
  std::vector<EntityId> whistle_inbox_;
  std::vector<std::pair<EntityId, TravelTarget>> travel_target_inbox_;
  std::vector<std::pair<EntityId, std::array<std::uint8_t, 32>>> voice_key_inbox_;
  std::vector<std::pair<EntityId, std::uint8_t>> voice_tier_inbox_;
  std::vector<std::pair<EntityId, NpcBark>> npc_bark_inbox_;
  std::vector<std::pair<EntityId, SiegeUse>> siege_use_inbox_;
  std::vector<std::pair<EntityId, SiegeImpact>> siege_impact_inbox_;
  std::vector<std::pair<EntityId, PlacerReport>> placer_report_inbox_;
  std::vector<std::pair<EntityId, SiegeLadderReport>> siege_ladder_inbox_;
  std::vector<std::pair<EntityId, StonePileReport>> stone_pile_inbox_;
  std::vector<std::pair<EntityId, DialogueChoice>> dialogue_choice_inbox_;
  std::vector<std::pair<EntityId, DialogueCancel>> dialogue_cancel_inbox_;
  std::vector<std::pair<EntityId, CraftReport>> craft_report_inbox_;
  std::vector<std::pair<EntityId, CraftTable>> craft_table_inbox_;
  std::vector<std::pair<EntityId, DiceMove>> dice_move_inbox_;
  std::vector<std::pair<EntityId, DiceView>> dice_view_inbox_;
  std::vector<std::pair<EntityId, GatherReport>> gather_report_inbox_;
  std::vector<std::pair<EntityId, VendorTrade>> vendor_trade_inbox_;
  std::vector<std::pair<EntityId, VendorClosed>> vendor_closed_inbox_;
  std::vector<std::pair<EntityId, VendorHaggle>> vendor_haggle_inbox_;
  // A shot's projectile lands within this long and this far from where it left, or never (it
  // struck a body or broke).
  static constexpr double kShotFlightSeconds = 30;
  static constexpr float kShotFlightReach = 400;
  // Shots of a client still in flight.
  static constexpr std::size_t kMaxShotsInFlight = 32;
  // The level's items a client asks to take up per second.
  static constexpr unsigned kMaxWorldPickupsPerSecond = 4;
  // A driver reports at most 10 times a second (Client/runtime/carts), plus enters and leaves.
  static constexpr unsigned kMaxCartReportsPerSecond = 16;
  // A client simulating props reports each about ten times a second while it moves.
  static constexpr unsigned kMaxPropReportsPerSecond = 160;
  // Shots a client relays per second (a crossbow or bow shoots far fewer).
  static constexpr unsigned kMaxShotsPerSecond = 8;
  static constexpr unsigned kMaxCarryReportsPerSecond = 4;
  // Inventory reports per client per second: one per stock the server sent, which a resource
  // changes far less often.
  static constexpr unsigned kMaxInventoryReportsPerSecond = 16;
  // A batch of inventory changes the scripts have not answered for this long is refused when the
  // client sends it again, so a lost answer does not stop the client's reports for good.
  static constexpr double kInventoryBatchAnswerSeconds = 30;
  static constexpr unsigned kMaxDamageReportsPerSecond = 30;
  // The recipe book's pictures (RECIPE_PICTURE): parts sent to a client each tick, and the most
  // waiting for it (64 pictures of the largest kind).
  static constexpr std::size_t kPicturePartsPerTick = 2;
  static constexpr std::size_t kMaxPicturePartsQueued = 64 * 65;
  struct Client {
    // RECIPE_PICTURE parts not yet queued on the control channel, oldest first.
    std::deque<std::vector<std::uint8_t>> picture_parts;
    // Kept per client: parallel upstream processing never mutates another client's cache.
    std::unordered_map<EntityId, PlayerInputPublication> player_input_publications;
    // Validation with reasons: each owned player's movement check, the steps refused this tick
    // (logged after the parallel input phase) and when this client was last logged.
    std::unordered_map<EntityId, MovementCheck> movement_checks;
    std::unordered_map<EntityId, MovementStrikes> movement_strikes;
    std::vector<MovementRefusal> movement_refused;
    double movement_logged_at = -1.0e9;
    bool natives_reported = false;  // a client reports its game build once per connection
    // False when the client's game build lacks combat's functions (CLIENT_NATIVES): no blow is
    // relayed to or from it.
    bool combat = true;
    double melee_logged_at = -1.0e9;
    ScriptReceiver script_receiver;
    std::uint32_t script_sequence = 0;
    double script_window = -1;
    unsigned script_count = 0;
    double chat_window = -1;
    unsigned chat_count = 0;
    double xp_window = -1;
    unsigned xp_count = 0;
    double perk_window = -1;
    unsigned perk_count = 0;
    double buff_window = -1;
    unsigned buff_reports = 0;
    unsigned buff_blocks = 0;
    double inventory_window = -1;
    unsigned inventory_reports = 0;
    double stash_unlock_window = -1;
    unsigned stash_unlocks = 0;
    double quest_tracking_window = -1;
    unsigned quest_tracking_reports = 0;
    // NPC_USEs taken in the second that began at npc_use_window.
    double npc_use_window = -1;
    unsigned npc_uses = 0;
    // DIALOGUE_CHOICEs and DIALOGUE_CANCELs taken in the second that began at dialogue_window.
    double dialogue_window = -1;
    unsigned dialogue_reports = 0;
    // CRAFT_REPORTs taken in the second that began at craft_window.
    double craft_window = -1;
    unsigned craft_reports = 0;
    // CRAFT_TABLEs taken in the second that began at craft_table_window.
    double craft_table_window = -1;
    unsigned craft_tables = 0;
    // DICE_MOVEs taken in the second that began at dice_window.
    double dice_window = -1;
    unsigned dice_moves = 0;
    // DICE_VIEWs taken in the second that began at dice_view_window.
    double dice_view_window = -1;
    unsigned dice_views = 0;
    // GATHER_REPORTs taken in the second that began at gather_window.
    double gather_window = -1;
    unsigned gather_reports = 0;
    // VENDOR_TRADEs and VENDOR_CLOSEDs taken in the second that began at vendor_window.
    double vendor_window = -1;
    unsigned vendor_reports = 0;
    double carry_window = -1;
    unsigned carry_reports = 0;
    double animation_window = -1;
    unsigned animation_ends = 0;
    // WEAPON_HITs taken in the second that began at weapon_hit_window.
    double weapon_hit_window = -1;
    unsigned weapon_hits = 0;
    // GRAVE messages taken in the second that began at grave_window.
    double grave_window = -1;
    unsigned grave_messages = 0;
    // ANIMATION_PROP_CUEs taken in the second that began at prop_cue_window.
    double prop_cue_window = -1;
    unsigned prop_cues = 0;
    double pillory_window = -1;
    unsigned pillory_reports = 0;
    double interaction_window = -1;
    // When the client's last whistle counted (WHISTLE, one a second).
    double whistled_at = -1;
    // When the client's last travel target counted (LEVEL_TRAVEL, one a second).
    double travel_target_at = -1;
    double bark_window = -1;
    unsigned barks = 0;
    unsigned interactions_fired = 0;
    // SIEGE_USEs, SIEGE_IMPACTs and the SIEGE_LADDER and STONE_PILE reports taken in the second
    // that began at siege_window.
    double siege_window = -1;
    unsigned siege_reports = 0;
    // PLACER_REPORTs taken in the second that began at placer_report_window.
    double placer_report_window = -1;
    unsigned placer_reports = 0;
    // NPC_HITs taken in the second that began at npc_hit_window.
    double npc_hit_window = -1;
    unsigned npc_hits = 0;
    // NPC_BLOWs taken in the second that began at npc_blow_window.
    double npc_blow_window = -1;
    unsigned npc_blows = 0;
    // NPC_ENGAGEs taken in the second that began at npc_engage_window.
    double npc_engage_window = -1;
    unsigned npc_engages = 0;
    double world_pickup_window = -1;
    unsigned world_pickups = 0;
    double cart_window = -1;
    unsigned cart_reports = 0;
    double prop_window = -1;
    unsigned prop_reports = 0;
    double shot_window = -1;
    unsigned shots = 0;
    // The client's shots whose projectile may still land: when, its ammunition, where it left.
    struct Flight {
      double at = 0;
      std::array<std::uint8_t, 16> ammo{};
      float position[3] = {};
    };
    std::deque<Flight> flights;
    bool inventory_census = false;  // a client takes its census once per connection
    // Batches of changes the client's game made: this connection's epoch (set with its first
    // stock), the last batch the server took, whether the scripts still owe it an answer, and the
    // answer they gave, sent again for a batch that comes twice.
    std::uint32_t inventory_epoch = 0;
    std::uint32_t inventory_sequence = 0;
    bool inventory_batch_open = false;
    double inventory_batch_at = 0;
    std::optional<InventoryChangesAck> inventory_answer;
    double damage_window = -1;
    unsigned damage_reports = 0;
    double hit_window = -1;
    unsigned hit_reports = 0;
    double transform_window = -1;
    unsigned transform_count = 0;
    std::optional<WorldConfig> world_config;
    bool world_ready = false;
    bool gameplay_ready = false;     // the client took its loading screen down (playerReady)
    bool knocked_out = false;        // its player lies knocked out until a resource revives it
    bool nickname_received = false;  // a connection names its player once
    std::uint64_t last_melee_sequence = 0;
    unsigned observer_contacts_this_tick = 0;
    std::unique_ptr<Connection> conn;
    EntityId viewer = 0;
    float range = 0.0f;
    ClientRepl repl;
    bool has_repl_ack = false;
    // Full-resync bookkeeping (lossy-safe). `confirmed` = entities the client has acked having
    // constructed; UPDATEs go only to these. `pending_at[tick]` = entities CONSTRUCTed in that
    // tick's packet (re-sent every tick until confirmed), so an ack of tick T confirms exactly the
    // constructs the client received in packet T.
    std::unordered_set<EntityId> confirmed;
    // Constructs may arrive before their acknowledgement. Keep every potentially
    // delivered entity until its DESTRUCT is acknowledged, including unconfirmed ones.
    std::unordered_set<EntityId> sent_scope;
    // Character subset of sent_scope, independent of the world's live rows: a despawned body
    // still consumes a slot until its DESTRUCT is acknowledged. The local avatar reserves one.
    std::unordered_set<EntityId> resident_characters;
    CharacterAdmissionStats character_admission;
    std::map<core::Tick, std::vector<EntityId>> pending_at;
    std::map<core::Tick, std::vector<EntityId>> pending_updates;
    std::map<core::Tick, std::vector<EntityId>> pending_leaves;
    // Entities whose DESTRUCT was sent but not yet acknowledged. They stay out of view until the
    // acknowledgement, so an entity that re-enters is CONSTRUCTed again with its full state.
    std::unordered_set<EntityId> leaving;
    // Snapshot byte credit (only with a SnapshotBudget) and the tick each currently deferred
    // entity was first deferred (starvation input for the priority).
    double credit = 0.0;
    struct Deferral {
      core::Tick first_tick = 0;
      bool seen_pass = false;
    };
    std::unordered_map<EntityId, Deferral> deferred_since;
    // Independent of world tick: repeated builds of the same tick still prune
    // entries that no longer wait, without reallocating the continuing entries.
    bool deferral_pass = false;
    // Entities this client's avatar recently fought (tick of the last relayed combat event).
    std::unordered_map<EntityId, core::Tick> combat_partners;
    // The tick each entity in view last had an UPDATE built for this client (distance rates).
    std::unordered_map<EntityId, core::Tick> last_update;
    // This build's full-rate and 20 Hz circles (squared metres; smaller in a crowd) and the
    // characters' distances they were measured from.
    float full_rate_sq = 0.0f;
    float mid_rate_sq = 0.0f;
    std::vector<float> crowd_distances;
    // Outcome of the latest build, aggregated serially after a (possibly parallel) build.
    replication::SnapshotStats last_stats;
    SnapshotProfileCounters last_profile;
    // Interest set of the latest build (visible_entities), once there was one.
    std::vector<EntityId> interest;
    bool interest_built = false;
  };

  std::unordered_map<EntityId, std::uint64_t> horse_sequences_;
  std::unordered_map<EntityId, BodyStatePublication> body_publications_;
  void handle_horse(PeerId sender, Client& client, const std::vector<std::uint8_t>& bytes,
                    bool mount, double now);
  // The HORSE_HEALTHs taken per horse in the second that began at the window's start.
  struct HorseHealthWindow {
    double start = -1;
    unsigned taken = 0;
  };
  std::unordered_map<EntityId, HorseHealthWindow> horse_health_windows_;
  std::vector<HorseHealthChange> horse_health_inbox_;
  static constexpr std::size_t kMaxHorseHealthChangesWaiting = 1024;
  // HORSE_STAMINA taken in the second that began at each horse's window start.
  std::unordered_map<EntityId, HorseHealthWindow> horse_stamina_windows_;
  std::vector<HorseStaminaChange> horse_stamina_inbox_;
  // Takes a simulator's HORSE_HEALTH (horse.hpp) on its conditions and writes what it changes.
  void handle_horse_stamina(PeerId sender, Client& client, const std::vector<std::uint8_t>& bytes,
                            double now);
  void handle_horse_health(PeerId sender, Client& client, const std::vector<std::uint8_t>& bytes,
                           double now);
  // What the server keeps of a live NPC's current simulation epoch: the newest state sequence it
  // took and the NPC_STATEs taken in the current one-second window.
  struct NpcSimulation {
    std::uint64_t sequence = 0;
    // What its simulator's body held when its state was last published (NpcState::hands): a
    // change goes out at once, not with the next idle refresh (issue 016).
    std::string hands;
    double window = -1;
    unsigned states = 0;
    // NPC_OUTFITs taken in the second that began at outfit_window.
    double outfit_window = -1;
    unsigned outfits = 0;
    // NPC_POSEs taken in the second that began at pose_window.
    double pose_window = -1;
    unsigned poses = 0;
    // NPC_INTENT_DONEs taken in the second that began at intent_done_window.
    double intent_done_window = -1;
    unsigned intent_dones = 0;
    // NPC_ANIMATION_ENDs taken in the second that began at animation_end_window.
    double animation_end_window = -1;
    unsigned animation_ends = 0;
    // The player the NPC fights, until fight_until (npc.hpp kNpcFightSeconds).
    EntityId fighter = core::kInvalidEntity;
    double fight_until = -1;
    // The player whose client runs it whatever the distances (Npc.pin), kInvalidEntity for none.
    EntityId pinned = core::kInvalidEntity;
  };
  // The time of the last tick, for the fights of NPCs.
  double npc_clock_ = 0;
  // A blow between the NPC and the player was taken: the player fights it, unless another
  // player's fight with it still lasts.
  void note_npc_fight(EntityId npc, EntityId player, double now);
  // Where a player fought an NPC, until `until` (npc.hpp kNpcWitnessSeconds): the people around
  // it are run by that player's client too. One a player, moved by each new fight.
  struct NpcWitnessZone {
    EntityId player = core::kInvalidEntity;
    core::Vec3 position{};
    double until = -1;
  };
  std::vector<NpcWitnessZone> npc_witness_zones_;
  // A blow between the NPC and the player: the place becomes that player's witness zone.
  void note_npc_witnesses(EntityId npc, EntityId player, double now);
  // The player whose witness zone the NPC at `position` stands in, kInvalidEntity for none.
  EntityId witness_zone_player(EntityId npc, const replication::EntitySlot& slot) const;
  std::unordered_map<EntityId, NpcSimulation> npcs_;
  std::vector<NpcSimulatorChange> npc_simulator_inbox_;
  // At most this many changes wait for take_npc_simulator_changes (more than live entities).
  static constexpr std::size_t kMaxNpcSimulatorChanges = 0x10000;
  std::vector<NpcHit> npc_hit_inbox_;
  static constexpr std::size_t kMaxNpcHitsWaiting = 1024;
  std::vector<NpcIntentDone> npc_intent_done_inbox_;
  std::vector<NpcAnimationEnd> npc_animation_end_inbox_;
  std::vector<NpcUse> npc_use_inbox_;
  std::vector<NpcUse> npc_engage_inbox_;
  static constexpr std::size_t kMaxNpcIntentDonesWaiting = 1024;
  double npc_election_at_ = -1;
  void handle_npc_look(PeerId sender, Client& client, const std::vector<std::uint8_t>& bytes);
  // A batch of changes the client's game made: taken for the scripts, or answered at once.
  void handle_inventory_changes(Client& client, InventoryReport report, bool busy, double now);
  // A snapshot acknowledgement (REPL_ACK): only this client's replication state changes.
  void handle_repl_ack(Client& client, const std::vector<std::uint8_t>& bytes);
  void handle_npc_pose(PeerId sender, Client& client, const std::vector<std::uint8_t>& bytes,
                       double now);
  void handle_npc_outfit(PeerId sender, Client& client, const std::vector<std::uint8_t>& bytes,
                         double now);
  void handle_npc_hit(PeerId sender, Client& client, const std::vector<std::uint8_t>& bytes,
                      double now);
  // Passes an NPC_BLOW on to the client on the other side of the fight (npc.hpp).
  void handle_npc_blow(PeerId sender, Client& client, const std::vector<std::uint8_t>& bytes,
                       double now);
  // A player asked to lock on to the NPC: it fights it (npc.hpp NPC_ENGAGE).
  void handle_npc_engage(PeerId sender, Client& client, const std::vector<std::uint8_t>& bytes,
                         double now);
  // The simulator's order for the NPC ended (npc.hpp NPC_INTENT_DONE): kept for the resource host.
  void handle_npc_use(PeerId sender, Client& client, const std::vector<std::uint8_t>& bytes,
                      double now);
  // What the player's swing met that is not a body (weapon_hits.hpp WEAPON_HIT): kept for the
  // resource host when it lies within reach of the player, at most kMaxWeaponHitsPerSecond.
  void handle_weapon_hit(Client& client, const std::vector<std::uint8_t>& bytes, double now);
  // A grave the player's level has or the player dug out (graves.hpp GRAVE): kept for the
  // resources (take_graves).
  void handle_grave(Client& client, const std::vector<std::uint8_t>& bytes, double now);
  void handle_npc_animation_end(PeerId sender, Client& client,
                                const std::vector<std::uint8_t>& bytes, double now);
  void handle_npc_intent_done(PeerId sender, Client& client, const std::vector<std::uint8_t>& bytes,
                              double now);
  void handle_npc_state(PeerId sender, Client& client, const std::vector<std::uint8_t>& bytes,
                        double now);
  // The simulator the election gives the NPC now (npc.hpp), 0 for none.
  EntityId choose_npc_simulator(EntityId npc, const replication::EntitySlot& slot) const;
  // A native body needs a client whose gameplay started in this revision, which owns its player,
  // holds the body and has it in scope. Merely loading the replicated world is insufficient.
  bool body_simulator_candidate(EntityId body, const replication::EntitySlot& body_slot,
                                EntityId player, const replication::EntitySlot& player_slot) const;
  // Makes `simulator` the NPC's simulator: a new epoch, no native state, and a change for the
  // resource host.
  void set_npc_simulator(EntityId npc, EntityId simulator);
  // Raises the NPC's simulation epoch and empties its native state (a new simulator or a
  // teleport): the simulator's earlier states are refused.
  std::int64_t start_npc_epoch(EntityId npc);
  // A client script assigning Entity.position or Entity.rotation of its own player.
  void handle_client_transform(PeerId sender, Client& client, const sdk::Value& message,
                               double now);
  std::vector<std::uint8_t> build_one(PeerId pid, Client& c, core::Tick snapTick, double share,
                                      double credit_cap, replication::RowMembership& membership);
  // The connected clients by PeerId: the order of every per-client phase and of build_all.
  std::vector<PeerId> client_ids() const;
  // fn(i) for i in [0, count), on the snapshot workers when there are any and inline otherwise.
  // fn(i) may touch only the state of the i-th client and the rows of the entities it owns.
  void for_each_client(std::size_t count, const std::function<void(std::size_t)>& fn);
  // One message a client sent, in arrival order. Owner input is checked here and kept in
  // `owner_input` to be applied after every client's messages were handled.
  void handle_message(PeerId pid, Client& c, transport::DeliveredMessage& msg, double now,
                      std::vector<const std::vector<std::uint8_t>*>& owner_input);
  // Interest set of one client: in-range entities (with hysteresis) that pass the dimension
  // and private visibility scope, always-visible entities, its avatar and the entities it owns
  // that are in its scope. Empty while the client has no live avatar.
  void interest_of(PeerId pid, const Client& c, std::vector<EntityId>& interest,
                   replication::RowMembership& membership) const;
  std::vector<EntityId> character_interest(PeerId pid, const Client& c,
                                           std::vector<EntityId> interest,
                                           const replication::RowMembership& eligible,
                                           CharacterAdmissionStats* stats = nullptr) const;
  bool holds_character(const Client& c, EntityId entity) const;
  PeerId elect_horse_simulator(EntityId horse) const;
  bool visible_to(PeerId pid, const Client& c, const replication::EntitySlot& viewer, EntityId e,
                  const replication::EntitySlot& slot) const;
  // Fills urgent_tick_ for this snapshot tick (serial, before the builds).
  void note_urgent_changes();
  // The client's full-rate and 20 Hz circles for this build, from its nearest characters.
  void measure_crowd(Client& c, const replication::Vec3& viewer_position) const;
  // How many ticks apart an entity's UPDATEs go to this client (distance rates).
  core::Tick update_every(const Client& c, EntityId e, const replication::Vec3& viewer_position,
                          core::Tick snapTick) const;
  float priority_for(const Client& c, EntityId e, const replication::Vec3& viewer_position,
                     core::Tick snapTick) const;

  // Delegation policy stub: pick the remaining client best suited to own `e` (nearest viewer within
  // interest range), or 0 for server-owned if none qualifies. Call with `exclude` already removed.
  PeerId elect_owner(EntityId e) const;

  World world_;
  InterestGrid grid_;
  std::uint32_t byte_budget_ = 0;
  bool distance_rates_ = true;
  std::uint64_t movement_refusals_ = 0;
  MovementCheckMode movement_mode_ = MovementCheckMode::Observe;
  // Where each player stood over the last moments (melee_reach.hpp), by player entity.
  std::unordered_map<EntityId, PoseHistory> pose_histories_;
  MeleeReachMode melee_reach_mode_ = MeleeReachMode::Observe;
  std::uint64_t melee_refusals_ = 0;
  // Per snapshot tick (distance rates): each character's latest change that may not wait, and per
  // type the fields that are not motion.
  std::unordered_map<EntityId, core::Tick> urgent_tick_;
  std::unordered_map<core::TypeId, std::vector<core::FieldId>> not_motion_;
  SnapshotBudget budget_;
  SnapshotBudgetCounters budget_counters_;
  bool snapshot_profiling_ = false;
  SnapshotProfileCounters snapshot_profile_counters_;
  float hysteresis_ = 0.0f;
  unsigned character_limit_ = 0;
  // Rebuilt serially at the start of every build_all: entities per owning peer and entities
  // flagged always visible. Read-only while snapshots are built.
  std::unordered_map<PeerId, std::vector<EntityId>> owned_;
  std::vector<EntityId> always_visible_;
  // Tick of the last relayed combat event per participating entity.
  std::unordered_map<EntityId, core::Tick> combat_tick_;
  // CONSTRUCT entries shared by the snapshot builds of one tick (reset in build_all).
  replication::ConstructCache construct_cache_;
  replication::UpdateCache update_cache_;
  // One reusable membership per snapshot chunk, not per viewer. A chunk exclusively owns its
  // scratch until the parallel build joins; begin() starts a fresh membership for each viewer.
  // Every how many ticks a peer's snapshot is built, by how long the builds take.
  static constexpr double kSnapshotBuildTargetSeconds = 0.008;
  static constexpr std::size_t kMaxSnapshotStride = 6;
  std::size_t snapshot_stride_ = 1;
  std::vector<replication::RowMembership> snapshot_memberships_;
  double last_receive_seconds_ = 0.0;
  double last_build_seconds_ = 0.0;
  double last_send_seconds_ = 0.0;
  PeerId next_peer_ = 1;
  std::unordered_map<PeerId, Client> clients_;
  // The connection of each client's player (its viewer), for lookups by player.
  std::unordered_map<EntityId, PeerId> viewer_peers_;
  std::unique_ptr<core::ThreadPool> pool_;
  std::uint64_t snapshot_bytes_sent_ = 0;
  ReliableCounters departed_reliable_;
};

}  // namespace kcd2s::integration

#endif  // KCD2S_INTEGRATION_SERVER_HPP
