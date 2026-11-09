#include "kcd2s/integration/server.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/core/seq.hpp"
#include "kcd2s/integration/control.hpp"
#include "kcd2s/integration/combat_capabilities.hpp"
#include "kcd2s/integration/knockout.hpp"
#include "kcd2s/integration/player_identity.hpp"
#include "kcd2s/integration/protocol.hpp"
#include "kcd2s/integration/server_name.hpp"
#include "kcd2s/replication/field_codec.hpp"
#include "kcd2s/wire/packet.hpp"

namespace kcd2s::integration {

namespace {
// The character types, known at compile time.
constexpr core::TypeId kPlayerType = core::type_id("NetworkPlayer");
constexpr core::TypeId kNpcType = core::type_id("NetworkNpc");
constexpr core::TypeId kHorseType = core::type_id("NetworkHorse");

bool is_character(core::TypeId type) {
  return type == kPlayerType || type == kNpcType || type == kHorseType;
}

// Pending records cover at most 256 recent snapshots. Numeric std::map order reverses their
// age at tick wrap; compare age relative to the just-built snapshot when bounding the window.
template <class Pending>
void trim_pending_snapshots(Pending& pending, core::Tick current) {
  while (pending.size() > 256) {
    auto oldest = pending.begin();
    for (auto it = std::next(pending.begin()); it != pending.end(); ++it) {
      if (core::Tick(current - it->first) > core::Tick(current - oldest->first)) {
        oldest = it;
      }
    }
    pending.erase(oldest);
  }
}

template <class Pending>
void erase_acknowledged_snapshots(Pending& pending, core::Tick acknowledged) {
  for (auto it = pending.begin(); it != pending.end();) {
    if (it->first == acknowledged || core::seq_greater(acknowledged, it->first)) {
      it = pending.erase(it);
    } else {
      ++it;
    }
  }
}

// Scope gate shared by interest filtering, ownership election and the auxiliary
// combat / force-state control traffic. Two entities on different maps never interact, nor do two
// in different *numbered* virtual worlds of one map (the map's global world bridges both ways,
// replication::worlds_meet), and a privately restricted entity is only ever exposed to its single
// target connection.
// Centralising the rule keeps every scope-sensitive path in step instead of each
// re-deriving (and sometimes forgetting) the dimension/private-visibility checks.
bool scope_allows(const replication::EntitySlot& viewer, const replication::EntitySlot& target,
                  PeerId peer) {
  // A connection always keeps its own viewer avatar. A private-visibility restriction or a
  // world change on that avatar limits who else receives it, never its own controlling client.
  if (&viewer == &target)
    return true;
  if (target.visibleTo != 0 && target.visibleTo != peer)
    return false;
  return replication::worlds_meet(viewer.dimension, target.dimension);
}

// Decode one channel-3 upstream message and apply it under server authority: only AUTH_OWNER fields
// of an entity actually owned by `sender` are written; AUTH_SERVER fields and non-owner writes are
// decoded (to keep the stream aligned) but dropped.
void apply_upstream(replication::World& world, PeerId sender,
                    const std::vector<std::uint8_t>& payload, double now, std::uint32_t revision,
                    std::unordered_map<core::EntityId, PlayerInputPublication>& publications,
                    MovementCheckMode movement_mode,
                    std::unordered_map<core::EntityId, MovementCheck>& movement,
                    std::vector<MovementRefusal>& refused) {
  core::BitReader r(payload);
  const auto wire_entity = r.read_varuint();
  const core::EntityId e = static_cast<core::EntityId>(wire_entity);
  const std::uint8_t epoch = static_cast<std::uint8_t>(r.read_bits(8));
  const std::uint64_t count = r.read_bits(16);
  if (r.overrun() || wire_entity != e)
    return;

  const replication::EntitySlot* slot = world.slot(e);
  const replication::TypeDef* td = slot ? world.schema().get(slot->type) : nullptr;
  if (!td)
    return;  // unknown/dead entity -> cannot decode its fields

  // Authority: owner must match AND the message's forced-state epoch must be current (a stale epoch
  // means this input predates a server ForceState and is dropped until the owner adopts the bump).
  const bool owns = slot->owner == sender && slot->stateEpoch == epoch;
  PlayerInputFields updates;
  updates.reserve(std::min<std::size_t>(count, td->fields.size()));
  for (std::uint64_t i = 0; i < count; ++i) {
    const std::uint64_t fid = r.read_bits(16);
    if (r.overrun() || fid >= td->fields.size())
      return;
    const replication::FieldDef& fd = td->fields[static_cast<std::size_t>(fid)];
    replication::FieldValue val = replication::decode_field(r, fd);
    if (r.overrun())
      return;
    if (owns && fd.auth == replication::Authority::Owner)
      updates.emplace_back(static_cast<core::FieldId>(fid), std::move(val));
  }
  if (!owns) {
    return;
  }
  // Validate the entire packet before applying any field. In particular, a malformed tail must
  // not leave a new clock paired with only a prefix of the owner's movement or action state.
  bool publish_clock = true;
  if (td->id == kPlayerType) {
    bool changed = false;
    if (PlayerInputPublication::idle(updates)) {
      for (const auto& [fid, value] : updates) {
        if (fid != local::MotionClock) {
          const auto* current = world.read(e, fid);
          if (!current || !same_player_idle_field(td->fields[fid], *current, value)) {
            changed = true;
            break;
          }
        }
      }
    }
    publish_clock = publications[e].publish(*td, updates, {e, sender, epoch, revision}, now,
                                            changed, true, kPlayerIdleServerPublicationSeconds);
    // Validation with reasons: the step to a new position over the owner's own clock.
    const core::Vec3* position = nullptr;
    std::int64_t clock_ms = -1;
    const std::string* rider = nullptr;
    for (const auto& [fid, value] : updates) {
      if (fid == local::Position) {
        position = std::get_if<core::Vec3>(&value);
      } else if (fid == local::MotionClock) {
        if (const auto* clock = std::get_if<std::int64_t>(&value)) {
          clock_ms = *clock;
        }
      } else if (fid == local::NativeRiderState) {
        rider = std::get_if<std::string>(&value);
      }
    }
    if (movement_mode != MovementCheckMode::Off && position && clock_ms >= 0) {
      if (!rider) {
        const auto* current = world.read(e, local::NativeRiderState);
        rider = current ? std::get_if<std::string>(current) : nullptr;
      }
      const bool mounted = rider && !rider->empty();
      auto verdict = check_movement(movement[e], *position, clock_ms, epoch, mounted);
      if (!verdict.ok) {
        refused.push_back({e, std::move(verdict)});
      }
    }
  }
  for (auto& [fid, value] : updates) {
    if (!publish_clock &&
        (fid == local::MotionClock || fid == local::Animations || fid == local::WeaponAnimations)) {
      // Same-start idle progress refreshes together with its clock. Writing it while withholding
      // the clock would still dirty every snapshot and leave the timeline without a new sample.
      continue;
    }
    world.write(e, fid, std::move(value));
  }
}
}  // namespace

PeerId Server::add_client(IDatagramLink* link, EntityId viewer, float range) {
  const PeerId id = next_peer_++;
  Client c;
  c.conn = std::make_unique<Connection>(link);
  c.conn->set_channel_payload_budget(kScriptChannel, 4096);
  c.conn->set_channel_tier(kCombatChannel, transport::DeliveryTier::ReliableOrdered);
  c.conn->set_channel_tier(kChatChannel, transport::DeliveryTier::ReliableOrdered);
  c.conn->set_channel_tier(kHorseMotionChannel, transport::DeliveryTier::UnreliableSequenced);
  c.conn->set_channel_tier(kHorseMountChannel, transport::DeliveryTier::ReliableOrdered);
  c.conn->set_channel_tier(kNpcMotionChannel, transport::DeliveryTier::Unreliable);
  c.conn->set_channel_tier(kChannelReplAck, transport::DeliveryTier::UnreliableSequenced);
  c.viewer = viewer;
  c.range = range;
  c.resident_characters.insert(viewer);
  clients_.emplace(id, std::move(c));
  viewer_peers_[viewer] = id;
  return id;
}

void Server::enable_parallel(unsigned threads) {
  pool_ = std::make_unique<core::ThreadPool>(threads);
}

void Server::despawn(EntityId e) {
  body_publications_.erase(e);
  horse_sequences_.erase(e);
  horse_health_windows_.erase(e);
  horse_stamina_windows_.erase(e);
  npcs_.erase(e);
  world_.despawn(e);
  grid_.remove(e);  // so it is not returned by any AOI query -> confirmed-but-absent -> DESTRUCT
}

void Server::set_owner(EntityId e, PeerId owner) {
  replication::EntitySlot* s = world_.slot(e);
  if (!s)
    return;
  auto it = clients_.find(owner);
  if (it == clients_.end())
    return;
  // Authority is recorded unconditionally: this setter is a deliberate server-side
  // assignment and never silently fails. The *notification*, however, carries the
  // entity id and type, so it honours live visibility exactly like every other
  // outbound path. A SetOwner for an entity the peer cannot currently see (another
  // numbered dimension, or a private-visibility restriction) would disclose its
  // existence across that boundary, so the message is withheld while the owner is out
  // of scope; a later in-scope assignment delivers it normally.
  s->owner = owner;
  const replication::EntitySlot* vs = world_.slot(it->second.viewer);
  if (!vs || !scope_allows(*vs, *s, owner))
    return;
  it->second.conn->queue(static_cast<std::uint8_t>(wire::Channel::Control),
                         encode_set_owner(e, s->type, s->stateEpoch));  // flushed next tick
}

bool Server::configure_world(PeerId peer, const WorldConfig& config) {
  auto it = clients_.find(peer);
  if (it == clients_.end() || !valid_world_config(config))
    return false;
  auto& c = it->second;
  if (c.world_config && config.revision <= c.world_config->revision)
    return false;
  c.world_config = config;
  c.player_input_publications.clear();
  c.world_ready = false;
  c.gameplay_ready = false;
  c.knocked_out = false;
  c.conn->queue(0, encode_world_config(config));
  return true;
}

bool Server::revive(EntityId player) {
  if (!world_.slot(player)) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player || !c.knocked_out) {
      continue;
    }
    // Reject contacts and committed effects from the previous life before REVIVE reaches its
    // owner. Both messages use the existing ordered control channel; no transform is changed.
    force_state(player);
    c.knocked_out = false;
    c.conn->queue(0, encode_revive(player));
    return true;
  }
  return false;
}

bool Server::knock_out(EntityId player, float health) {
  const auto message = encode_knock_out(health);
  if (!world_.slot(player) || !decode_knock_out(message, health)) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player || !c.world_ready || c.knocked_out) {
      continue;
    }
    c.conn->queue(0, message);
    return true;
  }
  return false;
}

void Server::set_hit_rules(bool enabled, bool harm) {
  if (enabled == hit_rules_ && harm == harm_rules_) {
    return;
  }
  hit_rules_ = enabled;
  harm_rules_ = harm;
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.world_ready) {
      c.conn->queue(0, encode_hit_rules(enabled, harm));
    }
  }
}

void Server::set_server_name(std::string name) {
  server_name_ = std::move(name);
}

bool Server::send_hit_verdict(EntityId player, const HitVerdict& verdict) {
  const auto message = encode_hit_verdict(verdict);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer == player && c.world_ready) {
      c.conn->queue(0, message);
      return true;
    }
  }
  return false;
}

bool Server::request_combat_start(EntityId player, EntityId opponent) {
  const auto message = encode_combat_start(std::uint32_t(opponent));
  if (message.empty() || player == opponent) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer == player && c.world_ready) {
      c.conn->queue(0, message);
      return true;
    }
  }
  return false;
}

bool Server::request_combat_cancel(EntityId player, EntityId opponent) {
  const auto message = encode_combat_cancel(std::uint32_t(opponent));
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer == player && c.world_ready) {
      c.conn->queue(0, message);
      return true;
    }
  }
  return false;
}

bool Server::set_knockout_vision(EntityId player, std::uint8_t percent) {
  if (percent > kKnockoutVisionMax) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    c.conn->queue(0, encode_knockout_vision(percent));
    return true;
  }
  return false;
}

bool Server::send_combat_marker(EntityId player, bool shown) {
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    c.conn->queue(0, encode_combat_marker(shown));
    return true;
  }
  return false;
}

bool Server::send_session_end(EntityId player, const SessionEnd& end) {
  const auto message = encode_session_end(end);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    c.conn->queue(0, message);
    return true;
  }
  return false;
}

bool Server::send_restricted_area_marker(EntityId player, bool shown) {
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    c.conn->queue(0, encode_restricted_area_marker(shown));
    return true;
  }
  return false;
}

bool Server::send_interactions(EntityId player, const std::vector<Interaction>& list) {
  auto message = encode_interactions(list);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    c.conn->queue(0, std::move(message));
    return true;
  }
  return false;
}

bool Server::send_cuffs(EntityId player, bool cuffed) {
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    c.conn->queue(0, encode_cuffs(cuffed));
    return true;
  }
  return false;
}

void Server::send_player_attach(const PlayerAttach& attach) {
  for (auto& [pid, c] : clients_) {
    (void)pid;
    PlayerAttach order = attach;
    order.flags = attach.flags & kAttachFollowLook;
    if (c.viewer == EntityId(attach.player)) {
      order.flags |= kAttachIsLocalPlayer;
    }
    if (attach.target != 0 && c.viewer == EntityId(attach.target)) {
      order.flags |= kAttachToLocalPlayer;
    }
    c.conn->queue(0, encode_player_attach(order));
  }
}

bool Server::send_frozen(EntityId player, bool frozen) {
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    c.conn->queue(0, encode_frozen(frozen));
    return true;
  }
  return false;
}

bool Server::send_lobby(EntityId player, bool held) {
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    c.conn->queue(0, encode_lobby(held));
    return true;
  }
  return false;
}

bool Server::send_noclip_allowed(EntityId player, bool allowed) {
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    c.conn->queue(0, encode_noclip_allowed(allowed));
    return true;
  }
  return false;
}

bool Server::send_mount_order(EntityId player, const MountOrder& order) {
  return send_to_ready_player(player, encode_mount_order(order));
}

bool Server::send_pull_down_order(EntityId player, EntityId attacker, EntityId rider) {
  PullDownOrder order;
  order.attacker = std::uint32_t(attacker);
  order.rider = std::uint32_t(rider);
  return send_to_ready_player(player, encode_pull_down_order(order));
}

bool Server::send_rear_order(EntityId player) {
  return send_to_ready_player(player, encode_rear_order());
}

bool Server::send_dismount_order(EntityId player) {
  return send_to_ready_player(player, encode_dismount_order());
}

bool Server::send_voice_muffle(EntityId player, EntityId talker, std::uint8_t amount) {
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player || !c.world_ready) {
      continue;
    }
    c.conn->queue(0, encode_voice_muffle(talker, amount));
    return true;
  }
  return false;
}

bool Server::send_voice_state(EntityId player, const VoiceState& state) {
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player || !c.world_ready) {
      continue;
    }
    c.conn->queue(0, encode_voice_state(state));
    return true;
  }
  return false;
}

bool Server::send_whistle(EntityId player, std::uint32_t whistler) {
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player || !c.world_ready) {
      continue;
    }
    c.conn->queue(0, encode_whistle(whistler));
    return true;
  }
  return false;
}

bool Server::send_interaction_rules(EntityId player, std::uint16_t allowed) {
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    c.conn->queue(0, encode_interaction_rules(allowed));
    return true;
  }
  return false;
}

bool Server::send_dialogue_page(EntityId player, const DialoguePage& page) {
  auto message = encode_dialogue_page(page);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player || !c.world_ready) {
      continue;
    }
    c.conn->queue(0, std::move(message));
    return true;
  }
  return false;
}

bool Server::send_dialogue_close(EntityId player, const DialogueClose& close) {
  auto message = encode_dialogue_close(close);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player || !c.world_ready) {
      continue;
    }
    c.conn->queue(0, std::move(message));
    return true;
  }
  return false;
}

bool Server::send_craft_verdict(EntityId player, const CraftVerdict& verdict) {
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player || !c.world_ready) {
      continue;
    }
    c.conn->queue(0, encode_craft_verdict(verdict));
    return true;
  }
  return false;
}

bool Server::send_gather_verdict(EntityId player, const GatherVerdict& verdict) {
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player || !c.world_ready) {
      continue;
    }
    c.conn->queue(0, encode_gather_verdict(verdict));
    return true;
  }
  return false;
}

bool Server::send_gather_plants(EntityId player, const GatherPlants& plants) {
  auto message = encode_gather_plants(plants);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player || !c.world_ready) {
      continue;
    }
    c.conn->queue(0, std::move(message));
    return true;
  }
  return false;
}

bool Server::send_pot_looks(EntityId player, const PotLooks& looks) {
  auto message = encode_pot_looks(looks);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player || !c.world_ready) {
      continue;
    }
    c.conn->queue(0, std::move(message));
    return true;
  }
  return false;
}

bool Server::send_dice_tables(EntityId player, const DiceTables& tables) {
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer == player && c.world_ready) {
      c.conn->queue(0, encode_dice_tables(tables));
      return true;
    }
  }
  return false;
}

bool Server::send_dice_watch(EntityId player, const DiceWatch& watch) {
  auto message = encode_dice_watch(watch);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player || !c.world_ready) {
      continue;
    }
    c.conn->queue(0, std::move(message));
    return true;
  }
  return false;
}

bool Server::send_dice_state(EntityId player, const DiceState& state) {
  auto message = encode_dice_state(state);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player || !c.world_ready) {
      continue;
    }
    c.conn->queue(0, std::move(message));
    return true;
  }
  return false;
}

bool Server::send_vendor_show(EntityId player, const VendorShow& show) {
  auto message = encode_vendor_show(show);
  if (message.empty()) {
    return false;
  }
  // The game's shop screen takes its keeper from the NPC's body in the game of the player who
  // trades, and only the body that runs the NPC has one (a replica has no WUID): the trading
  // player runs the keeper, the way a fighter runs the NPC it fights.
  if (show.npc != 0) {
    note_npc_fight(EntityId(show.npc), player, npc_clock_);
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player || !c.world_ready) {
      continue;
    }
    c.conn->queue(0, std::move(message));
    return true;
  }
  return false;
}

bool Server::send_vendor_haggle_reply(EntityId player, const VendorHaggleReply& reply) {
  auto message = encode_vendor_haggle_reply(reply);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player || !c.world_ready) {
      continue;
    }
    c.conn->queue(0, std::move(message));
    return true;
  }
  return false;
}

bool Server::send_vendor_close(EntityId player, const VendorClose& close) {
  auto message = encode_vendor_close(close);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player || !c.world_ready) {
      continue;
    }
    c.conn->queue(0, std::move(message));
    return true;
  }
  return false;
}

bool Server::send_third_person(EntityId player, std::uint8_t mode) {
  if (mode > kThirdPersonForced) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    c.conn->queue(0, encode_third_person(mode));
    return true;
  }
  return false;
}

namespace {

// Queues a control message to the ready client that views `player`; false when there is none or
// the message is empty.
template <class Clients>
bool queue_to_ready(Clients& clients, EntityId player, const std::vector<std::uint8_t>& message) {
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients) {
    (void)pid;
    if (c.viewer == player && c.world_ready) {
      c.conn->queue(0, message);
      return true;
    }
  }
  return false;
}

}  // namespace

bool Server::send_world_raycast(EntityId player, const WorldRayRequest& ray) {
  return queue_to_ready(clients_, player, encode_world_raycast(ray));
}

bool Server::send_camera_shot(EntityId player, const CameraShot& shot) {
  return queue_to_ready(clients_, player, encode_camera_shot(shot));
}

bool Server::send_camera_clear(EntityId player) {
  return queue_to_ready(clients_, player, encode_camera_clear());
}

bool Server::send_pillory(EntityId player, const PilloryOrder& order) {
  const auto message = encode_pillory(order);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer == player && c.world_ready) {
      c.conn->queue(0, message);
      return true;
    }
  }
  return false;
}

bool Server::send_item_notice(EntityId player, const ItemNotice& notice) {
  const auto message = encode_item_notice(notice);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer == player && c.world_ready) {
      c.conn->queue(0, message);
      return true;
    }
  }
  return false;
}

bool Server::send_pickpocket_pockets(EntityId player, const PickpocketPockets& pockets) {
  const auto message = encode_pickpocket_pockets(pockets);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer == player && c.world_ready) {
      c.conn->queue(0, message);
      return true;
    }
  }
  return false;
}

bool Server::send_grime_order(EntityId player, const GrimeOrder& order) {
  const auto message = encode_grime_order(order);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer == player && c.world_ready) {
      c.conn->queue(0, message);
      return true;
    }
  }
  return false;
}

bool Server::send_world_time(EntityId player, const WorldTime& time) {
  const auto message = encode_world_time(time);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer == player && c.world_ready) {
      c.conn->queue(0, message);
      return true;
    }
  }
  return false;
}

bool Server::send_world_weather(EntityId player, const WorldWeather& weather) {
  return send_to_ready_player(player, encode_world_weather(weather));
}

bool Server::send_gate(EntityId player, const GateState& state) {
  return send_to_ready_player(player, encode_gate(state));
}

bool Server::send_to_ready_player(EntityId player, const std::vector<std::uint8_t>& message) {
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer == player && c.world_ready) {
      c.conn->queue(0, message);
      return true;
    }
  }
  return false;
}

bool Server::send_marker_show(EntityId player, const MarkerShow& marker) {
  return send_to_ready_player(player, encode_marker_show(marker));
}

bool Server::send_marker_hide(EntityId player, std::uint32_t marker) {
  return send_to_ready_player(player, encode_marker_hide(marker));
}

bool Server::send_blip_show(EntityId player, const BlipShow& blip) {
  return send_to_ready_player(player, encode_blip_show(blip));
}

bool Server::send_blip_hide(EntityId player, std::uint32_t blip) {
  return send_to_ready_player(player, encode_blip_hide(blip));
}

bool Server::send_quest_show(EntityId player, const QuestShow& quest) {
  return send_to_ready_player(player, encode_quest_show(quest));
}

bool Server::send_quest_hide(EntityId player, std::uint32_t quest) {
  return send_to_ready_player(player, encode_quest_hide(quest));
}

bool Server::send_quest_track(EntityId player, const QuestTracking& track) {
  return send_to_ready_player(player, encode_quest_track(track));
}

bool Server::send_door_set(EntityId player, const DoorState& state) {
  const auto message = encode_door_set(state);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer == player && c.world_ready) {
      c.conn->queue(0, message);
      return true;
    }
  }
  return false;
}

bool Server::send_door_nearest(EntityId player, std::uint32_t request) {
  const auto message = encode_door_nearest(request);
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer == player && c.world_ready) {
      c.conn->queue(0, message);
      return true;
    }
  }
  return false;
}

namespace {

// Whether a player whose avatar is in `viewer_world` sees something placed in `placed_world`.
bool world_sees(std::uint32_t viewer_world, std::uint32_t placed_world) {
  return replication::worlds_meet(viewer_world, placed_world);
}

}  // namespace

bool Server::send_smart_object(EntityId player, const SmartObjectOrder& order,
                               std::uint32_t dimension) {
  const auto message = encode_smart_object(order);
  if (message.empty()) {
    return false;
  }
  bool sent = false;
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (!c.world_ready || (player != 0 && c.viewer != player)) {
      continue;
    }
    if (!order.remove) {
      const auto* slot = world_.slot(c.viewer);
      if (!slot || !world_sees(slot->dimension, dimension)) {
        continue;
      }
    }
    c.conn->queue(0, message);
    sent = true;
  }
  return sent;
}

bool Server::send_door_spawn(EntityId player, const DoorSpawn& door, std::uint32_t dimension) {
  const auto message = encode_door_spawn(door);
  if (message.empty()) {
    return false;
  }
  bool sent = false;
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (!c.world_ready || (player != 0 && c.viewer != player)) {
      continue;
    }
    if (!door.remove) {
      const auto* slot = world_.slot(c.viewer);
      if (!slot || !world_sees(slot->dimension, dimension)) {
        continue;
      }
    }
    c.conn->queue(0, message);
    sent = true;
  }
  return sent;
}

bool Server::send_prop(EntityId player, const PropOrder& order) {
  const auto message = encode_prop(order);
  if (message.empty()) {
    return false;
  }
  bool sent = false;
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (!c.world_ready || (player != 0 && c.viewer != player)) {
      continue;
    }
    if (order.kind == PropKind::Spawn) {
      const auto* slot = world_.slot(c.viewer);
      if (!slot || !world_sees(slot->dimension, order.dimension)) {
        continue;
      }
    }
    c.conn->queue(0, message);
    sent = true;
  }
  return sent;
}

bool Server::send_siege_engine(EntityId player, SiegeEngineOrder order, EntityId operator_id) {
  bool sent = false;
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (!c.world_ready || (player != 0 && c.viewer != player)) {
      continue;
    }
    if (order.kind == SiegeOrderKind::Spawn) {
      const auto* slot = world_.slot(c.viewer);
      if (!slot || !world_sees(slot->dimension, order.dimension)) {
        continue;
      }
    }
    order.state.mine = operator_id != 0 && c.viewer == operator_id;
    auto message = encode_siege_engine(order);
    if (message.empty()) {
      return sent;
    }
    c.conn->queue(0, std::move(message));
    sent = true;
  }
  return sent;
}

bool Server::send_vfx(EntityId player, const VfxOrder& order) {
  const auto message = encode_vfx(order);
  if (message.empty()) {
    return false;
  }
  const bool by_world = order.kind == VfxKind::Spawn || order.kind == VfxKind::Burst;
  bool sent = false;
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (!c.world_ready || (player != 0 && c.viewer != player)) {
      continue;
    }
    if (by_world) {
      const auto* slot = world_.slot(c.viewer);
      if (!slot || !world_sees(slot->dimension, order.dimension)) {
        continue;
      }
    }
    c.conn->queue(0, message);
    sent = true;
  }
  return sent;
}

bool Server::send_light(EntityId player, const LightOrder& order) {
  const auto message = encode_light(order);
  if (message.empty()) {
    return false;
  }
  bool sent = false;
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (!c.world_ready || (player != 0 && c.viewer != player)) {
      continue;
    }
    c.conn->queue(0, message);
    sent = true;
  }
  return sent;
}

bool Server::send_recipe_table(EntityId player, const RecipeTable& table) {
  return send_to_ready(player, encode_recipe_table(table));
}

bool Server::send_recipe_picture(EntityId player, const RecipePicture& picture) {
  const auto parts = encode_recipe_picture(picture);
  if (parts.empty()) {
    return false;
  }
  // Its parts go out a few a tick (tick): queued at once they would be one datagram of a MiB.
  bool sent = false;
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (!c.world_ready || (player != 0 && c.viewer != player) ||
        c.picture_parts.size() + parts.size() > kMaxPicturePartsQueued) {
      continue;
    }
    c.picture_parts.insert(c.picture_parts.end(), parts.begin(), parts.end());
    sent = true;
  }
  return sent;
}

bool Server::send_to_ready(EntityId player, const std::vector<std::uint8_t>& message) {
  if (message.empty()) {
    return false;
  }
  bool sent = false;
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (!c.world_ready || (player != 0 && c.viewer != player)) {
      continue;
    }
    c.conn->queue(0, message);
    sent = true;
  }
  return sent;
}

bool Server::send_cart(EntityId player, const CartOrder& order) {
  const auto message = encode_cart(order);
  if (message.empty()) {
    return false;
  }
  bool sent = false;
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (!c.world_ready || (player != 0 && c.viewer != player)) {
      continue;
    }
    // The driver's game runs the cart; it is not told where it already is.
    if (order.kind == CartKind::Pose && c.viewer == order.player) {
      continue;
    }
    if (order.kind == CartKind::Spawn) {
      const auto* slot = world_.slot(c.viewer);
      if (!slot || !world_sees(slot->dimension, order.dimension)) {
        continue;
      }
    }
    c.conn->queue(0, message);
    sent = true;
  }
  return sent;
}

bool Server::send_siege_shot(SiegeShot shot, std::uint32_t dimension, EntityId reporter) {
  bool sent = false;
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (!c.world_ready) {
      continue;
    }
    const auto* slot = world_.slot(c.viewer);
    if (!slot || !world_sees(slot->dimension, dimension)) {
      continue;
    }
    shot.report = reporter != 0 && c.viewer == reporter;
    auto message = encode_siege_shot(shot);
    if (message.empty()) {
      return sent;
    }
    c.conn->queue(0, std::move(message));
    sent = true;
  }
  return sent;
}

bool Server::send_siege_ladder(EntityId player, SiegeLadderOrder order, EntityId actor) {
  bool sent = false;
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (!c.world_ready || (player != 0 && c.viewer != player)) {
      continue;
    }
    if (order.kind == LadderMessageKind::Refused && c.viewer != player) {
      continue;
    }
    if (order.kind == LadderMessageKind::Spawn) {
      const auto* slot = world_.slot(c.viewer);
      if (!slot || !world_sees(slot->dimension, order.dimension)) {
        continue;
      }
    }
    order.mine = actor != 0 && c.viewer == actor;
    auto message = encode_siege_ladder(order);
    if (message.empty()) {
      return sent;
    }
    c.conn->queue(0, std::move(message));
    sent = true;
  }
  return sent;
}

bool Server::send_stone_pile(EntityId player, const StonePileOrder& order) {
  const auto message = encode_stone_pile(order);
  if (message.empty()) {
    return false;
  }
  bool sent = false;
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (!c.world_ready || (player != 0 && c.viewer != player)) {
      continue;
    }
    if (order.kind == StonePileMessageKind::Spawn) {
      const auto* slot = world_.slot(c.viewer);
      if (!slot || !world_sees(slot->dimension, order.dimension)) {
        continue;
      }
    }
    c.conn->queue(0, message);
    sent = true;
  }
  return sent;
}

namespace {

// Queues a message for every ready client whose viewer is `player` (0: every ready client).
template <class Clients>
bool queue_to_viewers(Clients& clients, EntityId player, const std::vector<std::uint8_t>& message) {
  if (message.empty()) {
    return false;
  }
  bool sent = false;
  for (auto& [pid, c] : clients) {
    (void)pid;
    if (!c.world_ready || (player != 0 && c.viewer != player)) {
      continue;
    }
    c.conn->queue(0, message);
    sent = true;
  }
  return sent;
}

}  // namespace

bool Server::send_ghost(EntityId player, const GhostOrder& order) {
  return player != 0 && queue_to_viewers(clients_, player, encode_ghost(order));
}

bool Server::send_placer(EntityId player, const PlacerOrder& order) {
  return player != 0 && queue_to_viewers(clients_, player, encode_placer(order));
}

bool Server::send_gizmo(EntityId player, const GizmoOrder& order) {
  return queue_to_viewers(clients_, player, encode_gizmo(order));
}

bool Server::send_body_carry(EntityId player, const BodyCarry& carry) {
  const auto message = encode_body_carry(carry);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer == player && c.world_ready) {
      c.conn->queue(0, message);
      return true;
    }
  }
  return false;
}

bool Server::send_animation_order(EntityId player, const AnimationOrder& order) {
  const auto message = encode_animation_order(order);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer == player && c.world_ready) {
      c.conn->queue(0, message);
      return true;
    }
  }
  return false;
}

bool Server::send_freecam(EntityId player, bool allowed) {
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer == player && c.world_ready) {
      c.conn->queue(0, encode_freecam(allowed));
      return true;
    }
  }
  return false;
}

bool Server::send_needs(EntityId player, bool enabled) {
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer == player && c.world_ready) {
      c.conn->queue(0, encode_needs(enabled));
      return true;
    }
  }
  return false;
}

bool Server::send_pickpocket_rule(EntityId player, const PickpocketRule& rule) {
  const auto message = encode_pickpocket_rule(rule);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer == player && c.world_ready) {
      c.conn->queue(0, message);
      return true;
    }
  }
  return false;
}

bool Server::send_world_pickup_verdict(EntityId player, const std::string& key, bool granted) {
  const auto message = encode_world_pickup_verdict(key, granted);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer == player && c.world_ready) {
      c.conn->queue(0, message);
      return true;
    }
  }
  return false;
}

bool Server::send_world_taken(EntityId player, const WorldTaken& taken) {
  const auto message = encode_world_taken(taken);
  if (message.empty()) {
    return false;
  }
  bool sent = false;
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.world_ready && (player == 0 || c.viewer == player)) {
      c.conn->queue(0, message);
      sent = true;
    }
  }
  return sent;
}

bool Server::send_world_items_removed(EntityId player, const std::vector<RemovedItemArea>& areas) {
  const auto message = encode_world_items_removed(areas);
  if (message.empty()) {
    return false;
  }
  bool sent = false;
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.world_ready && (player == 0 || c.viewer == player)) {
      c.conn->queue(0, message);
      sent = true;
    }
  }
  return sent;
}

bool Server::send_world_items_enabled(EntityId player, bool enabled) {
  const auto message = encode_world_items_enabled(enabled);
  bool sent = false;
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.world_ready && (player == 0 || c.viewer == player)) {
      c.conn->queue(0, message);
      sent = true;
    }
  }
  return sent;
}

bool Server::send_level_edit(EntityId player, const LevelEdit& edit, std::uint32_t world) {
  const auto message = encode_level_edit(edit);
  if (message.empty()) {
    return false;
  }
  bool sent = false;
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (!c.world_ready || (player != 0 && c.viewer != player)) {
      continue;
    }
    const auto* viewer = world_.slot(c.viewer);
    const bool in_world = !viewer || replication::worlds_meet(viewer->dimension, world);
    if (in_world) {
      c.conn->queue(0, message);
      sent = true;
    }
  }
  return sent;
}

bool Server::send_level_edit_remove(EntityId player, std::uint32_t id) {
  const auto message = encode_level_edit_remove(id);
  if (message.empty()) {
    return false;
  }
  bool sent = false;
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.world_ready && (player == 0 || c.viewer == player)) {
      c.conn->queue(0, message);
      sent = true;
    }
  }
  return sent;
}

bool Server::request_carry(EntityId player, const CarryOrder& order) {
  const auto message = encode_carry_order(order);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    c.conn->queue(0, message);
    return true;
  }
  return false;
}

bool Server::request_appearance(EntityId player, const AppearanceRequest& request) {
  if (!valid_appearance_request(request)) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    c.conn->queue(0, encode_appearance(request));
    return true;
  }
  return false;
}

bool Server::request_stat(EntityId player, const StatRequest& request) {
  const auto message = encode_stat_request(request);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    if (!c.world_ready || c.knocked_out) {
      return false;
    }
    c.conn->queue(0, message);
    return true;
  }
  return false;
}

bool Server::request_progression_order(EntityId player, const ProgressionOrder& order) {
  const auto message = encode_progression_order(order);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    if (!c.world_ready) {
      return false;
    }
    c.conn->queue(0, message);
    return true;
  }
  return false;
}

bool Server::request_custom_buff(EntityId player, const CustomBuff& buff) {
  const auto message = encode_custom_buff(buff);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    if (!c.world_ready) {
      return false;
    }
    c.conn->queue(0, message);
    return true;
  }
  return false;
}

bool Server::request_buff_order(EntityId player, const BuffOrder& order) {
  const auto message = encode_buff_order(order);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    if (!c.world_ready) {
      return false;
    }
    c.conn->queue(0, message);
    return true;
  }
  return false;
}

bool Server::request_inventory_show(EntityId player, const InventoryShow& show) {
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    if (!c.world_ready) {
      return false;
    }
    // The connection's epoch comes with its first stock; a new connection gets a new one.
    if (c.inventory_epoch == 0) {
      c.inventory_epoch = ++last_inventory_epoch_;
      if (c.inventory_epoch == 0) {
        c.inventory_epoch = ++last_inventory_epoch_;
      }
    }
    InventoryShow stamped = show;
    stamped.epoch = c.inventory_epoch;
    const auto message = encode_inventory_show(stamped);
    if (message.empty()) {
      return false;
    }
    c.conn->queue(0, message);
    return true;
  }
  return false;
}

bool Server::request_inventory_changes_ack(EntityId player, const InventoryChangesAck& ack) {
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    if (!c.world_ready || !c.inventory_batch_open || ack.epoch != c.inventory_epoch ||
        ack.sequence != c.inventory_sequence) {
      return false;
    }
    c.inventory_batch_open = false;
    c.inventory_answer = ack;
    c.conn->queue(0, encode_inventory_changes_ack(ack));
    return true;
  }
  return false;
}

void Server::handle_inventory_changes(Client& c, InventoryReport report, bool busy, double now) {
  InventoryChangesAck answer;
  answer.epoch = report.epoch;
  answer.sequence = report.sequence;
  if (report.epoch == 0 || report.epoch != c.inventory_epoch) {
    answer.verdict = InventoryChangesAck::Verdict::Stale;
  } else if (report.sequence == c.inventory_sequence) {
    // The same batch again: its answer once the scripts gave it. One they never answered is
    // refused after a while.
    if (c.inventory_batch_open && now - c.inventory_batch_at >= kInventoryBatchAnswerSeconds) {
      answer.verdict = InventoryChangesAck::Verdict::Refused;
      c.inventory_batch_open = false;
      c.inventory_answer = answer;
    }
    if (c.inventory_batch_open || !c.inventory_answer) {
      return;
    }
    answer = *c.inventory_answer;
  } else if (c.inventory_batch_open || report.sequence != c.inventory_sequence + 1) {
    answer.verdict = InventoryChangesAck::Verdict::OutOfOrder;
  } else if (busy) {
    // Too many reports this second, or too many waiting for the scripts: the client keeps the
    // batch and sends it again.
    answer.verdict = InventoryChangesAck::Verdict::Busy;
  } else {
    c.inventory_sequence = report.sequence;
    c.inventory_batch_open = true;
    c.inventory_batch_at = now;
    c.inventory_answer.reset();
    inventory_report_inbox_.emplace_back(c.viewer, std::move(report));
    return;
  }
  c.conn->queue(0, encode_inventory_changes_ack(answer));
}

bool Server::request_inventory_consume(EntityId player, const InventoryConsume& consume) {
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    if (!c.world_ready) {
      return false;
    }
    c.conn->queue(0, encode_inventory_consume(consume));
    return true;
  }
  return false;
}

bool Server::request_stash_show(EntityId player, const StashShow& show) {
  const auto message = encode_stash_show(show);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    if (!c.world_ready) {
      return false;
    }
    c.conn->queue(0, message);
    return true;
  }
  return false;
}

bool Server::request_item_types(EntityId player, const ItemTypes& types) {
  const auto message = encode_item_types(types);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (player == 0 || c.viewer == player) {
      c.conn->queue(0, message);
    }
  }
  return true;
}

bool Server::request_item_overrides(EntityId player, const ItemOverrides& overrides) {
  const auto message = encode_item_overrides(overrides);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (player == 0 || c.viewer == player) {
      c.conn->queue(0, message);
    }
  }
  return true;
}

bool Server::request_item_limit_notice(EntityId player, const ItemLimitNotice& notice) {
  const auto message = encode_item_limit_notice(notice);
  if (message.empty() || player == 0) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer == player) {
      c.conn->queue(0, message);
    }
  }
  return true;
}

bool Server::request_item_names(EntityId player, const ItemNames& names) {
  const auto message = encode_item_names(names);
  if (message.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (player == 0 || c.viewer == player) {
      c.conn->queue(0, message);
    }
  }
  return true;
}

bool Server::request_movement_mode(EntityId player, const MovementMode& mode) {
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    c.conn->queue(0, encode_movement_mode(mode));
    return true;
  }
  return false;
}

bool Server::request_body_visible(EntityId player, bool visible) {
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    c.conn->queue(0, encode_body_visible(visible));
    return true;
  }
  return false;
}

bool Server::request_worn(EntityId player, const std::vector<ItemClassId>& items) {
  const auto bytes = encode_worn(items);
  if (bytes.empty()) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    c.conn->queue(0, bytes);
    return true;
  }
  return false;
}

bool Server::request_disguise(EntityId player, const std::string& model) {
  if (!valid_disguise(model)) {
    return false;
  }
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer != player) {
      continue;
    }
    c.conn->queue(0, encode_disguise(model));
    return true;
  }
  return false;
}

void Server::force_state(EntityId e) {
  replication::EntitySlot* s = world_.slot(e);
  if (!s)
    return;
  // Fence: owner input with the old epoch is now rejected until adopted. The bump is stamped so
  // observers also receive the new epoch in an UPDATE even when no field value changes.
  world_.bump_state_epoch(e);
  // Every replica that already holds the entity needs the epoch fence even when no
  // field changed, because combat events can reference either avatar and an
  // owner-only notification would leave observers stale. But an observer that never
  // constructed the entity (different dimension, or outside a private-visibility
  // target) must not receive its id over an auxiliary control message: that would
  // leak existence across the scope boundary and waste bandwidth at player scale. A
  // future observer instead adopts the bumped epoch from the entity's next CONSTRUCT,
  // which always carries the current stateEpoch.
  for (auto& [pid, c] : clients_) {
    const replication::EntitySlot* viewer = world_.slot(c.viewer);
    const bool is_owner = s->owner == pid;
    // The current owner also needs the fence before its first snapshot. Visibility still applies
    // to it, and an observer is notified only while it holds and may still see the entity.
    if (!viewer || !scope_allows(*viewer, *s, pid) || (!is_owner && !c.sent_scope.count(e))) {
      continue;
    }
    c.conn->queue(static_cast<std::uint8_t>(wire::Channel::Control),
                  encode_force_state(e, s->stateEpoch));
  }
}

bool Server::force_transform(EntityId e, core::Vec3 pos) {
  // Reject before any state mutation: a teleport to a non-finite coordinate is never a legitimate
  // authoritative command and must not corrupt the slot or reach the engine transform path.
  if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z))
    return false;
  replication::EntitySlot* s = world_.slot(e);
  if (!s)
    return false;  // unknown or despawned id (ids are never reused, so it names no live entity)
  const replication::TypeDef* td = world_.schema().get(s->type);
  if (!td || td->position_field == replication::TypeDef::kNoField)
    return false;  // no replicated position to carry the teleport; nothing authoritative to write
  // Write through World::write so the replicated position FIELD (and its change tick) is updated,
  // not just the interest-hot slot.position: subsequent snapshots and SDK reads must report the new
  // position. write() mirrors the value into slot.position for the type's position field.
  if (!world_.write(e, td->position_field, replication::FieldValue{pos}))
    return false;
  // Fence: stale owner input (and a replayed reconnect teleport) is now rejected. The bump is
  // stamped so observers also receive the new epoch in their next UPDATE.
  world_.bump_state_epoch(e);
  // Deliver only to clients that already hold the entity and may currently see it, carrying the
  // bumped epoch. Entity ids are never reused, so a command queued before a reconnect names the old
  // entity's id and can never mutate the entity that replaced it.
  for (auto& [pid, c] : clients_) {
    const replication::EntitySlot* vs = world_.slot(c.viewer);
    // The owner always needs the teleport for the entity it controls, even if that entity is not in
    // its own snapshot scope (a client is not necessarily sent a construct for what it owns);
    // observers get it only when it is in their scope. Both still pass the visibility gate.
    const bool is_owner = s->owner == pid;
    if (!vs || !scope_allows(*vs, *s, pid) || (!is_owner && !c.sent_scope.count(e)))
      continue;
    c.conn->queue(static_cast<std::uint8_t>(wire::Channel::Control),
                  encode_force_transform(e, s->stateEpoch, pos));
  }
  return true;
}

bool Server::force_rotation(EntityId e, core::Quat rotation) {
  if (!valid_force_rotation(rotation))
    return false;
  replication::EntitySlot* s = world_.slot(e);
  if (!s)
    return false;
  const replication::TypeDef* td = world_.schema().get(s->type);
  if (!td)
    return false;
  const auto field = std::find_if(td->fields.begin(), td->fields.end(), [](const auto& f) {
    return f.kind == replication::FieldKind::Quat && f.name == "rotation";
  });
  if (field == td->fields.end())
    return false;
  if (!world_.write(e, field->id, replication::FieldValue{rotation}))
    return false;
  // Same fence and delivery rules as force_transform: stale owner input carrying the old rotation
  // is rejected, and only clients that hold and may see the entity (or own it) are told.
  world_.bump_state_epoch(e);
  for (auto& [pid, c] : clients_) {
    const replication::EntitySlot* vs = world_.slot(c.viewer);
    const bool is_owner = s->owner == pid;
    if (!vs || !scope_allows(*vs, *s, pid) || (!is_owner && !c.sent_scope.count(e)))
      continue;
    c.conn->queue(static_cast<std::uint8_t>(wire::Channel::Control),
                  encode_force_rotation(e, s->stateEpoch, rotation));
  }
  return true;
}

PeerId Server::elect_owner(EntityId e) const {
  const replication::EntitySlot* es = world_.slot(e);
  if (!es)
    return 0;
  const replication::Vec3 ep = es->position;
  PeerId best = 0;
  float best_d2 = 0.0f;
  bool have = false;
  for (const auto& [pid, c] : clients_) {
    const replication::EntitySlot* vs = world_.slot(c.viewer);
    if (!vs)
      continue;
    const float dx = vs->position.x - ep.x;
    const float dy = vs->position.y - ep.y;
    const float dz = vs->position.z - ep.z;
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 > c.range * c.range)
      continue;  // outside this client's interest -> not a candidate
    if (!scope_allows(*vs, *es, pid))
      continue;  // in another dimension or hidden by private visibility -> cannot own it
    if (!have || d2 < best_d2) {
      have = true;
      best_d2 = d2;
      best = pid;
    }
  }
  return best;  // 0 => no eligible client; entity becomes server-owned
}

PeerId Server::elect_horse_simulator(EntityId horse) const {
  const auto* slot = world_.slot(horse);
  if (!slot) {
    return 0;
  }
  const auto eligible_peer = [&](EntityId player) -> PeerId {
    const auto indexed = viewer_peers_.find(player);
    const auto* viewer = world_.slot(player);
    return indexed != viewer_peers_.end() && viewer &&
                   body_simulator_candidate(horse, *slot, player, *viewer)
               ? indexed->second
               : 0;
  };
  const auto rider = world_.read(horse, HorseRider);
  if (rider && std::get<std::int64_t>(*rider) != 0) {
    // A mounted horse belongs to its rider's native simulation. During that rider's reload it
    // waits for gameplay readiness; another nearby client must not drive it in the meantime.
    return eligible_peer(EntityId(std::get<std::int64_t>(*rider)));
  }
  const auto owner = world_.read(horse, HorseOwner);
  if (owner) {
    if (const auto peer = eligible_peer(EntityId(std::get<std::int64_t>(*owner)))) {
      return peer;
    }
  }
  const auto current = world_.read(horse, HorseSimulator);
  if (current) {
    if (const auto peer = eligible_peer(EntityId(std::get<std::int64_t>(*current)))) {
      return peer;
    }
  }
  PeerId best = 0;
  float best_distance = 0;
  for (const auto& [peer, client] : clients_) {
    const auto* viewer = world_.slot(client.viewer);
    if (!viewer || !body_simulator_candidate(horse, *slot, client.viewer, *viewer)) {
      continue;
    }
    const float dx = viewer->position.x - slot->position.x;
    const float dy = viewer->position.y - slot->position.y;
    const float dz = viewer->position.z - slot->position.z;
    const float distance = character_limit_ == 0
                               ? dx * dx + dy * dy + dz * dz
                               : grid_.horizontal_distance_sq(viewer->position, slot->position);
    if (distance > client.range * client.range) {
      continue;
    }
    if (!best || distance < best_distance || (distance == best_distance && peer < best)) {
      best = peer;
      best_distance = distance;
    }
  }
  return best;
}

void Server::remove_client(PeerId p) {
  if (clients_.find(p) == clients_.end())
    return;

  // Snapshot the entities this peer owned BEFORE erasing it, so it is not its own re-election
  // candidate.
  std::vector<EntityId> orphaned;
  for (EntityId e : world_.live_entities()) {
    const replication::EntitySlot* s = world_.slot(e);
    if (s && s->owner == p)
      orphaned.push_back(e);
  }
  const auto departed_viewer = clients_.at(p).viewer;
  pose_histories_.erase(departed_viewer);
  departed_reliable_.sent += clients_.at(p).conn->reliable_sent_count();
  departed_reliable_.retransmits += clients_.at(p).conn->reliable_retransmit_count();
  clients_.erase(p);
  const auto indexed = viewer_peers_.find(departed_viewer);
  if (indexed != viewer_peers_.end() && indexed->second == p) {
    viewer_peers_.erase(indexed);
    // Another connection with the same player (never made by the host) takes the index over.
    for (const auto& [pid, c] : clients_) {
      if (c.viewer == departed_viewer) {
        viewer_peers_[departed_viewer] = pid;
        break;
      }
    }
  }
  for (auto e : world_.live_entities()) {
    auto s = world_.slot(e);
    if (!s || s->type != kHorseType)
      continue;
    auto sim = world_.read(e, HorseSimulator), rider = world_.read(e, HorseRider);
    if (!sim || !rider)
      continue;
    if (std::get<std::int64_t>(*rider) == departed_viewer)
      world_.write(e, HorseRider, std::int64_t(0));
    if (std::get<std::int64_t>(*sim) == departed_viewer) {
      auto heir = elect_horse_simulator(e);
      auto it = clients_.find(heir);
      initialize_horse(e, it == clients_.end() ? 0 : it->second.viewer);
    }
  }
  // The NPCs the departed client simulated go to the next player near them, or to nobody, now
  // rather than at the next election.
  for (const auto& [npc, simulation] : npcs_) {
    (void)simulation;
    const auto* slot = world_.slot(npc);
    const auto* simulator = world_.read(npc, NpcSimulator);
    if (!slot || !simulator ||
        std::get<std::int64_t>(*simulator) != std::int64_t(departed_viewer)) {
      continue;
    }
    set_npc_simulator(npc, choose_npc_simulator(npc, *slot));
  }

  for (EntityId e : orphaned) {
    replication::EntitySlot* s = world_.slot(e);
    if (!s)
      continue;
    const PeerId heir = elect_owner(e);
    s->owner = heir;
    // Fence: any input still carrying the pre-handoff epoch is now rejected. Stamped so observers
    // learn the new epoch even if the heir's fields do not change.
    world_.bump_state_epoch(e);
    if (heir == 0)
      continue;  // server-owned: nobody to notify
    auto it = clients_.find(heir);
    if (it != clients_.end())
      it->second.conn->queue(static_cast<std::uint8_t>(wire::Channel::Control),
                             encode_set_owner(e, s->type, s->stateEpoch));  // flushed next tick
  }
}

bool Server::set_dimension(EntityId entity, std::uint32_t world) {
  auto* slot = world_.slot(entity);
  if (!slot)
    return false;
  slot->dimension = world;
  return true;
}

bool Server::set_visible_to(EntityId entity, EntityId player) {
  auto* slot = world_.slot(entity);
  if (!slot)
    return false;
  PeerId peer = 0;
  if (player != core::kInvalidEntity) {
    const auto* target = world_.slot(player);
    if (!target || !target->owner || !clients_.count(target->owner))
      return false;
    peer = target->owner;
  }
  slot->visibleTo = peer;
  return true;
}

std::vector<EntityId> Server::visible_entities(PeerId peer) const {
  const auto client = clients_.find(peer);
  if (client == clients_.end())
    return {};
  // The latest build's set. Under load it can be a few ticks old (snapshot_stride_), so an entity
  // despawned since is left out.
  if (client->second.interest_built) {
    std::vector<EntityId> visible;
    visible.reserve(client->second.interest.size());
    for (const EntityId entity : client->second.interest) {
      if (world_.slot(entity)) {
        visible.push_back(entity);
      }
    }
    return visible;
  }
  std::vector<EntityId> interest;
  replication::RowMembership membership;
  interest_of(peer, client->second, interest, membership);
  interest = character_interest(peer, client->second, std::move(interest), membership);
  std::sort(interest.begin(), interest.end());
  return interest;
}

CharacterAdmissionStats Server::character_admission(PeerId peer) const {
  const auto found = clients_.find(peer);
  return found == clients_.end() ? CharacterAdmissionStats{} : found->second.character_admission;
}

CharacterAdmissionStats Server::character_admission_totals() const {
  CharacterAdmissionStats total;
  for (const auto& [peer, client] : clients_) {
    (void)peer;
    const auto& stats = client.character_admission;
    total.candidates += stats.candidates;
    total.admitted += stats.admitted;
    total.resident += stats.resident;
    total.retiring += stats.retiring;
    total.deferred += stats.deferred;
  }
  return total;
}

bool Server::holds_character(const Client& c, EntityId entity) const {
  if (!c.confirmed.count(entity)) {
    return false;
  }
  if (character_limit_ == 0) {
    return true;
  }
  return !c.leaving.count(entity) && c.interest_built &&
         std::find(c.interest.begin(), c.interest.end(), entity) != c.interest.end();
}

namespace {
// Snapshot priority weights. Optional entries are ordered by
//   player bonus + combat bonus + entering bonus + kStarvationWeight * ticks deferred
//     - kDistanceWeight * horizontal distance,
// after the mandatory entries (the client's own avatar and the entities it owns).
// Another player goes before every NPC (with some 230 level people in a village
// nearly every snapshot was full, and another player's turn showed about a second late).
constexpr float kOtherPlayerBonus = 2000.0f;
constexpr float kCombatPartnerBonus = 1000.0f;  // fought this client's avatar recently
constexpr float kCombatBonus = 200.0f;          // in any combat recently
constexpr float kEnteringBonus = 100.0f;        // not yet constructed on the client
constexpr float kStarvationWeight = 4.0f;       // per tick an entry has waited
constexpr float kDistanceWeight = 1.0f;         // per world unit (metre)
constexpr core::Tick kCombatWindowTicks = 180;  // 3 s at 60 Hz
// An optional entry that has waited this long may be sent beyond the budget (one per snapshot),
// even while the client's own and owned entities use up all of its credit every tick.
constexpr core::Tick kMaxDeferralTicks = 60;  // 1 s at 60 Hz
// Snapshot payloads stay well inside one channel block (u16 length) even with a large burst.
constexpr double kMaxCreditBytes = 32768.0;
// Distance rates: an entity in view gets an UPDATE at most every so many ticks, by its horizontal
// distance from the client's avatar (60 Hz ticks: full rate, 20 Hz, 5 Hz, 2 Hz). The client's own
// and owned entities and the ones in a fight with it or near it go at full rate. With hundreds of
// players in one place most of them are far from each viewer, so most of the delta work and bytes
// go to the near ones; a replica's timeline widens its delay to the gap between its samples.
// In a crowd (hundreds within a few metres) distance alone keeps everyone at full rate, so the
// full-rate and 20 Hz circles also shrink to the client's kFullRateRank-th and kMidRateRank-th
// nearest characters; the rest of a crowd within 50 m goes at 10 Hz.
constexpr float kFullRateMetres = 25.0f;
constexpr float kMidRateMetres = 50.0f;
constexpr float kSlowRateMetres = 100.0f;
constexpr core::Tick kFullEvery = 1;
constexpr core::Tick kMidEvery = 3;
constexpr core::Tick kCrowdEvery = 6;
constexpr core::Tick kSlowEvery = 12;
constexpr core::Tick kFarthestEvery = 30;
// A client's refused movement steps are logged at most this often (every one is counted).
constexpr double kMovementLogSeconds = 10.0;
constexpr std::size_t kFullRateRank = 24;
constexpr std::size_t kMidRateRank = 72;
constexpr double kMinBurstBytes = 4096.0;

bool recent(core::Tick when, core::Tick now) {
  return now - when <= kCombatWindowTicks;
}

// Ticks the longest-waiting deferred entry of a client has waited (0 when nothing waits).
core::Tick longest_deferral(const auto& deferred_since, core::Tick now) {
  core::Tick longest = 0;
  for (const auto& [entity, since] : deferred_since) {
    (void)entity;
    longest = std::max(longest, static_cast<core::Tick>(now - since.first_tick));
  }
  return longest;
}
}  // namespace

void Server::note_combat(EntityId a, EntityId b) {
  const core::Tick now = world_.tick();
  combat_tick_[a] = now;
  combat_tick_[b] = now;
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.viewer == a)
      c.combat_partners[b] = now;
    if (c.viewer == b)
      c.combat_partners[a] = now;
  }
}

bool Server::visible_to(PeerId pid, const Client& c, const replication::EntitySlot& viewer,
                        EntityId e, const replication::EntitySlot& slot) const {
  if (e == c.viewer)
    return true;  // the client's own avatar is always in scope
  // Dimension and private visibility are checked before anything is serialized, for owned
  // entities too: an owner separated from its entity must not learn the entity's id.
  if (!scope_allows(viewer, slot, pid))
    return false;
  if (slot.owner == pid)
    return true;
  return (slot.flags & replication::kFlagVisible) != 0;
}

void Server::interest_of(PeerId pid, const Client& c, std::vector<EntityId>& aoi,
                         replication::RowMembership& membership) const {
  aoi.clear();
  membership.begin(world_);
  const replication::EntitySlot* viewer = world_.slot(c.viewer);
  if (!viewer)
    return;
  const replication::Vec3 vpos = viewer->position;
  const float enter = c.range;
  const float leave = c.range + hysteresis_;
  const float enter_sq = enter * enter;
  const float leave_sq = leave * leave;

  auto admit = [&](EntityId e) {
    if (c.leaving.count(e))
      return;  // its DESTRUCT is still unacknowledged; re-enter with a fresh CONSTRUCT later
    if (membership.insert(e))
      aoi.push_back(e);
  };

  for (EntityId e : grid_.query(vpos, leave)) {
    const replication::EntitySlot* slot = world_.slot(e);
    if (!slot || !visible_to(pid, c, *viewer, e, *slot))
      continue;
    const float d2 = grid_.horizontal_distance_sq(slot->position, vpos);
    // Enter within the range; once in scope, stay until beyond range + hysteresis.
    if (d2 <= enter_sq || (c.sent_scope.count(e) && d2 <= leave_sq))
      admit(e);
  }
  for (EntityId e : always_visible_) {
    const replication::EntitySlot* slot = world_.slot(e);
    if (slot && visible_to(pid, c, *viewer, e, *slot))
      admit(e);
  }
  admit(c.viewer);
  auto owned = owned_.find(pid);
  if (owned != owned_.end()) {
    for (EntityId e : owned->second) {
      const replication::EntitySlot* slot = world_.slot(e);
      if (slot && visible_to(pid, c, *viewer, e, *slot))
        admit(e);
    }
  }
}

std::vector<EntityId> Server::character_interest(PeerId pid, const Client& c,
                                                 std::vector<EntityId> interest,
                                                 const replication::RowMembership& eligible,
                                                 CharacterAdmissionStats* stats) const {
  const auto* viewer = world_.slot(c.viewer);
  if (!viewer) {
    if (stats) {
      *stats = {};
      stats->resident = c.resident_characters.size();
      stats->retiring = stats->resident;
    }
    return {};
  }
  if (character_limit_ == 0) {
    if (stats) {
      *stats = {};
      for (const auto entity : interest) {
        const auto* slot = world_.slot(entity);
        if (slot && is_character(slot->type)) {
          ++stats->candidates;
        }
      }
      stats->admitted = stats->candidates;
      stats->resident = c.resident_characters.size();
    }
    return interest;
  }
  std::vector<CharacterCandidate> candidates;
  std::vector<EntityId> result;
  for (const auto entity : interest) {
    const auto* slot = world_.slot(entity);
    if (!slot || !is_character(slot->type)) {
      result.push_back(entity);
      continue;
    }
    CharacterCandidate candidate;
    candidate.entity = entity;
    candidate.group = entity;
    candidate.distance = std::sqrt(grid_.horizontal_distance_sq(viewer->position, slot->position));
    if (slot->owner == pid) {
      candidate.priority = 2;
    }
    if (slot->type == kHorseType) {
      const auto* rider = world_.read(entity, HorseRider);
      const EntityId rider_id = rider ? EntityId(std::get<std::int64_t>(*rider)) : 0;
      if (rider_id && eligible.contains(rider_id)) {
        candidate.group = rider_id;
      }
      const auto* simulator = world_.read(entity, HorseSimulator);
      if (simulator && std::get<std::int64_t>(*simulator) == std::int64_t(c.viewer)) {
        candidate.priority = std::max(candidate.priority, 1u);
      }
    } else if (slot->type == kNpcType) {
      const auto* simulator = world_.read(entity, NpcSimulator);
      if (simulator && std::get<std::int64_t>(*simulator) == std::int64_t(c.viewer)) {
        candidate.priority = std::max(candidate.priority, 1u);
      }
      const auto simulation = npcs_.find(entity);
      if (simulation != npcs_.end() && simulation->second.fighter == c.viewer &&
          npc_clock_ < simulation->second.fight_until) {
        candidate.priority = 3;
      }
    }
    const auto fight = c.combat_partners.find(entity);
    if (fight != c.combat_partners.end() && recent(fight->second, world_.tick())) {
      candidate.priority = 3;
    }
    candidates.push_back(candidate);
  }
  const auto admission =
      admit_characters(candidates, c.resident_characters, c.viewer, character_limit_, hysteresis_);
  result.insert(result.end(), admission.entities.begin(), admission.entities.end());
  if (stats) {
    *stats = admission.stats;
  }
  return result;
}

float Server::priority_for(const Client& c, EntityId e, const replication::Vec3& viewer_position,
                           core::Tick snapTick) const {
  float score = 0.0f;
  if (const replication::EntitySlot* slot = world_.slot(e)) {
    score -=
        kDistanceWeight * std::sqrt(grid_.horizontal_distance_sq(slot->position, viewer_position));
    if (slot->type == kPlayerType) {
      score += kOtherPlayerBonus;
    }
  }
  auto partner = c.combat_partners.find(e);
  if (partner != c.combat_partners.end() && recent(partner->second, snapTick)) {
    score += kCombatPartnerBonus;
  } else {
    auto fight = combat_tick_.find(e);
    if (fight != combat_tick_.end() && recent(fight->second, snapTick))
      score += kCombatBonus;
  }
  // An entity the client does not have yet outranks fresher state of nearby visible ones.
  if (!c.confirmed.count(e))
    score += kEnteringBonus;
  auto waiting = c.deferred_since.find(e);
  if (waiting != c.deferred_since.end())
    score += kStarvationWeight * static_cast<float>(snapTick - waiting->second.first_tick);
  return score;
}

namespace {
// The fields that change continuously while a body moves: a far entity's change of these waits for
// its next due UPDATE. Anything else (a death, a pose, health, an outfit) goes at once.
bool motion_field(core::TypeId type, core::FieldId field) {
  if (type == kPlayerType) {
    switch (field) {
      case local::Position:
      case local::Rotation:
      case local::Velocity:
      case local::Grounded:
      case local::Gait:
      case local::MotionType:
      case local::MotionVelocity:
      case local::MotionRotation:
      case local::MotionFlags:
      case local::MotionValid:
      case local::MotionParams0:
      case local::MotionParams1:
      case local::MotionParams2:
      case local::MotionParams3:
      case local::MotionParams4:
      case local::MotionParams5:
      case local::MotionMask:
      case local::MotionClock:
      case local::LookTarget:
      case local::LookDirection:
      case local::AimTarget:
        return true;
      default:
        return false;
    }
  }
  if (type == kNpcType) {
    return field == NpcPosition || field == NpcRotation || field == NpcVelocity ||
           field == NpcNativeState;
  }
  if (type == kHorseType) {
    return field == HorsePosition || field == HorseRotation || field == HorseNativeState;
  }
  return false;
}
}  // namespace

void Server::note_urgent_changes() {
  urgent_tick_.clear();
  if (!distance_rates_) {
    return;
  }
  for (EntityId e : world_.live_entities()) {
    const replication::EntitySlot* slot = world_.slot(e);
    if (!slot || !is_character(slot->type)) {
      continue;
    }
    auto known = not_motion_.find(slot->type);
    if (known == not_motion_.end()) {
      std::vector<core::FieldId> fields;
      if (const replication::TypeDef* def = world_.schema().get(slot->type)) {
        for (std::size_t field = 0; field < def->fields.size(); ++field) {
          if (!motion_field(slot->type, static_cast<core::FieldId>(field))) {
            fields.push_back(static_cast<core::FieldId>(field));
          }
        }
      }
      known = not_motion_.emplace(slot->type, std::move(fields)).first;
    }
    core::Tick latest = 0;
    for (core::FieldId field : known->second) {
      const core::Tick changed = world_.field_change_tick(e, field);
      if (core::seq_greater(changed, latest)) {
        latest = changed;
      }
    }
    urgent_tick_[e] = latest;
  }
}

core::Tick Server::update_every(const Client& c, EntityId e,
                                const replication::Vec3& viewer_position,
                                core::Tick snapTick) const {
  const replication::EntitySlot* slot = world_.slot(e);
  if (!slot) {
    return 1;
  }
  auto partner = c.combat_partners.find(e);
  if (partner != c.combat_partners.end() && recent(partner->second, snapTick)) {
    return 1;
  }
  const float distance_sq = grid_.horizontal_distance_sq(slot->position, viewer_position);
  if (distance_sq <= c.full_rate_sq) {
    return kFullEvery;
  }
  if (distance_sq <= c.mid_rate_sq) {
    return kMidEvery;
  }
  if (distance_sq <= kMidRateMetres * kMidRateMetres) {
    return kCrowdEvery;
  }
  if (distance_sq <= kSlowRateMetres * kSlowRateMetres) {
    return kSlowEvery;
  }
  return kFarthestEvery;
}

void Server::measure_crowd(Client& c, const replication::Vec3& viewer_position) const {
  c.full_rate_sq = kFullRateMetres * kFullRateMetres;
  c.mid_rate_sq = kMidRateMetres * kMidRateMetres;
  c.crowd_distances.clear();
  for (EntityId e : c.interest) {
    const replication::EntitySlot* slot = world_.slot(e);
    if (e != c.viewer && slot && is_character(slot->type)) {
      c.crowd_distances.push_back(grid_.horizontal_distance_sq(slot->position, viewer_position));
    }
  }
  auto& distances = c.crowd_distances;
  if (distances.size() > kFullRateRank) {
    std::nth_element(distances.begin(), distances.begin() + kFullRateRank, distances.end());
    c.full_rate_sq = std::min(c.full_rate_sq, distances[kFullRateRank]);
  }
  if (distances.size() > kMidRateRank) {
    std::nth_element(distances.begin() + kFullRateRank, distances.begin() + kMidRateRank,
                     distances.end());
    c.mid_rate_sq = std::min(c.mid_rate_sq, distances[kMidRateRank]);
  }
}

// Read-only on the world + the (already-synced) interest grid, writes only this client's scope
// bookkeeping + returns its own buffer — safe to run concurrently across clients.
//
// Resync model: build against inScope = `confirmed`, so entities the client has NOT confirmed are
// (re-)CONSTRUCTed this tick, and UPDATEs are emitted only for confirmed entities. A dropped
// CONSTRUCT therefore never yields an UPDATE for an entity the client lacks.
std::vector<std::uint8_t> Server::build_one(PeerId pid, Client& c, core::Tick snapTick,
                                            double share, double credit_cap,
                                            replication::RowMembership& membership) {
  auto* profile = snapshot_profiling_ ? &c.last_profile : nullptr;
  std::chrono::steady_clock::time_point phase_started{};
  if (profile) {
    *profile = {};
    profile->snapshots = 1;
    phase_started = std::chrono::steady_clock::now();
  }
  const auto finish_phase = [&](double SnapshotProfileCounters::* field) {
    if (profile) {
      const auto now = std::chrono::steady_clock::now();
      profile->*field += std::chrono::duration<double>(now - phase_started).count();
      phase_started = now;
    }
  };
  replication::Vec3 vpos{};
  if (const replication::EntitySlot* vs = world_.slot(c.viewer))
    vpos = vs->position;
  // Reuse the client-owned ordered ID buffer and the chunk-owned membership. The optional
  // character policy reads the full eligible membership before replacing it with admitted IDs.
  interest_of(pid, c, c.interest, membership);
  finish_phase(&SnapshotProfileCounters::interest_seconds);
  c.interest =
      character_interest(pid, c, std::move(c.interest), membership, &c.character_admission);
  if (character_limit_ != 0) {
    membership.begin(world_);
    for (const auto entity : c.interest) {
      membership.insert(entity);
    }
  }
  std::sort(c.interest.begin(), c.interest.end());
  const replication::SnapshotAOI aoi{c.interest, membership};

  // Build with inScope = the FULL confirmed set so build_snapshot emits DESTRUCT for entities that
  // left the view (confirmed \ aoi). It reports the constructs it ACTUALLY emitted (a byte budget
  // may defer some) via out_constructed, so we only mark those pending-for-confirm.
  const std::unordered_set<EntityId>* scope = &c.confirmed;
  std::unordered_set<EntityId> extended_scope;
  // Unconfirmed live entities still need CONSTRUCT, but an unconfirmed departed
  // entity can already exist on the client and therefore needs DESTRUCT.
  for (EntityId e : c.sent_scope) {
    if (!membership.contains(e) && !c.confirmed.count(e)) {
      if (scope == &c.confirmed) {
        extended_scope = c.confirmed;
        scope = &extended_scope;
      }
      extended_scope.insert(e);
    }
  }
  const replication::SnapshotView view{c.repl.ackedBaseline, c.repl.entityBaselines, *scope};

  // Combat partners older than the window no longer matter.
  for (auto it = c.combat_partners.begin(); it != c.combat_partners.end();) {
    if (recent(it->second, snapTick))
      ++it;
    else
      it = c.combat_partners.erase(it);
  }

  const bool credit_budget = share > 0.0;
  replication::BuildOptions opts;
  std::unordered_set<EntityId> mandatory;
  if (credit_budget) {
    c.credit = std::min(c.credit + share, credit_cap);
    opts.byte_budget = c.credit >= 1.0 ? static_cast<std::uint32_t>(c.credit) : 1u;
    opts.allow_overdraw =
        c.credit >= credit_cap || longest_deferral(c.deferred_since, snapTick) >= kMaxDeferralTicks;
    if (world_.alive(c.viewer))
      mandatory.insert(c.viewer);
    auto owned = owned_.find(pid);
    if (owned != owned_.end())
      mandatory.insert(owned->second.begin(), owned->second.end());
    opts.mandatory = &mandatory;
    opts.priority_of = [&](EntityId e) { return priority_for(c, e, vpos, snapTick); };
  } else {
    opts.byte_budget = byte_budget_;
  }
  if (distance_rates_) {
    measure_crowd(c, vpos);
    const auto owned = owned_.find(pid);
    opts.update_due = [&, owned](EntityId e) {
      // Only characters move continuously; the rest (doors, props) has no motion to hold.
      const auto urgent = urgent_tick_.find(e);
      if (e == c.viewer || urgent == urgent_tick_.end()) {
        return true;
      }
      if (owned != owned_.end() &&
          std::find(owned->second.begin(), owned->second.end(), e) != owned->second.end()) {
        return true;
      }
      const auto last = c.last_update.find(e);
      return last == c.last_update.end() ||
             snapTick - last->second >= update_every(c, e, vpos, snapTick);
    };
    opts.urgent_change_tick = [&](EntityId e) {
      const auto urgent = urgent_tick_.find(e);
      return urgent == urgent_tick_.end() ? core::Tick{0} : urgent->second;
    };
  }
  opts.constructs = &construct_cache_;
  opts.updates = &update_cache_;
  opts.timings = profile ? &profile->builder : nullptr;

  replication::SnapshotStats stats;
  std::vector<EntityId> constructed, updated;
  finish_phase(&SnapshotProfileCounters::preparation_seconds);
  auto payload = replication::build_snapshot_view(world_, snapTick, aoi, view, &stats, &opts,
                                                  &constructed, &updated);
  finish_phase(&SnapshotProfileCounters::snapshot_call_seconds);
  if (credit_budget) {
    // Debt is bounded by one burst: mandatory traffic above the budget for a long time must not
    // keep optional entries waiting for just as long after it calms down.
    c.credit = std::max(c.credit - static_cast<double>(payload.size()), -credit_cap);
  }

  // Starvation bookkeeping: keep the first-deferred tick of entities that are still waiting.
  c.deferral_pass = !c.deferral_pass;
  for (EntityId e : stats.deferred) {
    auto [entry, inserted] =
        c.deferred_since.try_emplace(e, Client::Deferral{snapTick, c.deferral_pass});
    (void)inserted;
    entry->second.seen_pass = c.deferral_pass;
  }
  for (auto entry = c.deferred_since.begin(); entry != c.deferred_since.end();) {
    if (entry->second.seen_pass != c.deferral_pass) {
      entry = c.deferred_since.erase(entry);
    } else {
      ++entry;
    }
  }

  for (EntityId e : constructed) {
    c.sent_scope.insert(e);
    const auto* slot = world_.slot(e);
    if (slot && is_character(slot->type)) {
      c.resident_characters.insert(e);
    }
  }
  c.character_admission.resident = c.resident_characters.size();
  if (distance_rates_) {
    for (EntityId e : updated) {
      c.last_update[e] = snapTick;
    }
    // An entity out of view starts again at full rate when it comes back.
    for (auto entry = c.last_update.begin(); entry != c.last_update.end();) {
      if (membership.contains(entry->first)) {
        ++entry;
      } else {
        entry = c.last_update.erase(entry);
      }
    }
  }
  c.pending_at[snapTick] = std::move(constructed);
  c.pending_updates[snapTick] = std::move(updated);
  auto& leaves = c.pending_leaves[snapTick];
  for (EntityId e : c.sent_scope) {
    if (!membership.contains(e)) {
      leaves.push_back(e);
      c.leaving.insert(e);
    }
  }
  // Bound the pending map if a client never acks (stale ticks are harmless to drop).
  trim_pending_snapshots(c.pending_at, snapTick);
  trim_pending_snapshots(c.pending_updates, snapTick);
  trim_pending_snapshots(c.pending_leaves, snapTick);

  // Keep departed entities confirmed until an acknowledged packet carries their DESTRUCT.
  // A dropped snapshot must not leave a permanent ghost on the client.

  c.last_stats = std::move(stats);
  c.interest_built = true;
  finish_phase(&SnapshotProfileCounters::bookkeeping_seconds);
  return payload;
}

std::vector<std::pair<PeerId, std::vector<std::uint8_t>>> Server::build_all(core::Tick snapTick) {
  std::chrono::steady_clock::time_point setup_started{};
  if (snapshot_profiling_) {
    setup_started = std::chrono::steady_clock::now();
  }
  // Serial: keep the interest grid in sync with the authoritative world (mutates the grid), and
  // index owned and always-visible entities for the read-only build below.
  owned_.clear();
  always_visible_.clear();
  for (EntityId e : world_.live_entities()) {
    const replication::EntitySlot* s = world_.slot(e);
    grid_.set_position(e, s->position);
    if (s->owner != 0)
      owned_[s->owner].push_back(e);
    if (s->flags & replication::kFlagAlwaysVisible)
      always_visible_.push_back(e);
  }
  for (auto it = combat_tick_.begin(); it != combat_tick_.end();) {
    if (recent(it->second, snapTick))
      ++it;
    else
      it = combat_tick_.erase(it);
  }
  construct_cache_.reset(world_.row_count());
  update_cache_.reset(world_);
  // Serial too: the builds below only read it.
  note_urgent_changes();

  // Stable client order (by PeerId) for deterministic output regardless of map iteration or
  // threading. Under load each peer's turn comes every snapshot_stride_ ticks, so the tick keeps
  // its rate and each peer gets fewer snapshots, not late ones. A peer off its turn
  // keeps its place in the result, with an empty payload.
  const std::vector<PeerId> ids = client_ids();
  const auto build_started = std::chrono::steady_clock::now();

  // Per-peer credit share this tick: the peer budget, limited by an even split of the aggregate.
  double share = 0.0;
  bool aggregate_limited = false;
  const bool budgeted = budget_.peer_bytes_per_tick != 0 || budget_.total_bytes_per_tick != 0;
  if (budgeted && !ids.empty()) {
    share =
        budget_.peer_bytes_per_tick != 0 ? double(budget_.peer_bytes_per_tick) : kMaxCreditBytes;
    if (budget_.total_bytes_per_tick != 0) {
      const double even = double(budget_.total_bytes_per_tick) / double(ids.size());
      if (even < share) {
        share = even;
        aggregate_limited = true;
      }
    }
  }
  // A peer's share covers the ticks it waited for its turn.
  share *= double(snapshot_stride_);
  double credit_cap = budget_.burst_bytes != 0 ? double(budget_.burst_bytes)
                                               : std::max(4.0 * share, kMinBurstBytes);
  credit_cap = std::min(std::max(credit_cap, share), kMaxCreditBytes);

  std::vector<std::pair<PeerId, std::vector<std::uint8_t>>> out(ids.size());
  const std::size_t chunks = std::min(ids.size(), std::max(std::size_t{1}, snapshot_workers()));
  if (snapshot_memberships_.size() < chunks) {
    snapshot_memberships_.resize(chunks);
  }
  const std::size_t chunk_size = chunks == 0 ? 0 : (ids.size() + chunks - 1) / chunks;
  if (snapshot_profiling_) {
    snapshot_profile_counters_.serial_setup_seconds +=
        std::chrono::duration<double>(std::chrono::steady_clock::now() - setup_started).count();
  }
  // Schedule chunk indices, so exclusive scratch ownership does not depend on which OS worker
  // runs a task or on ThreadPool's internal range partitioning. World rows stay immutable here.
  for_each_client(chunks, [&](std::size_t chunk) {
    auto& membership = snapshot_memberships_[chunk];
    const auto end = std::min((chunk + 1) * chunk_size, ids.size());
    for (std::size_t i = chunk * chunk_size; i < end; ++i) {
      if ((i + snapTick) % snapshot_stride_ != 0) {
        out[i] = {ids[i], {}};
        continue;
      }
      Client& c = clients_.at(ids[i]);
      out[i] = {ids[i], build_one(ids[i], c, snapTick, share, credit_cap, membership)};
    }
  });

  // The next ticks' turns, by what this one took: a build that would miss the tick spreads further,
  // one that would fit with less spreading comes back.
  const double build_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - build_started).count();
  if (build_seconds > kSnapshotBuildTargetSeconds && snapshot_stride_ < kMaxSnapshotStride) {
    ++snapshot_stride_;
  } else if (snapshot_stride_ > 1 &&
             build_seconds * double(snapshot_stride_) / double(snapshot_stride_ - 1) <
                 kSnapshotBuildTargetSeconds * 0.7) {
    --snapshot_stride_;
  }
  budget_counters_.snapshot_stride = snapshot_stride_;

  // Serial: aggregate the per-client outcome.
  budget_counters_.ticks += 1;
  if (aggregate_limited)
    budget_counters_.aggregate_limited_ticks += 1;
  for (const auto& [pid, payload] : out) {
    if (payload.empty()) {
      continue;
    }
    const auto& client = clients_.at(pid);
    const replication::SnapshotStats& stats = client.last_stats;
    if (snapshot_profiling_) {
      const auto& profile = client.last_profile;
      snapshot_profile_counters_.snapshots += profile.snapshots;
      snapshot_profile_counters_.interest_seconds += profile.interest_seconds;
      snapshot_profile_counters_.preparation_seconds += profile.preparation_seconds;
      snapshot_profile_counters_.snapshot_call_seconds += profile.snapshot_call_seconds;
      snapshot_profile_counters_.bookkeeping_seconds += profile.bookkeeping_seconds;
      for (std::size_t i = 0; i < profile.builder.seconds.size(); ++i) {
        snapshot_profile_counters_.builder.seconds[i] += profile.builder.seconds[i];
      }
    }
    budget_counters_.snapshots += 1;
    budget_counters_.snapshot_bytes += payload.size();
    budget_counters_.update_cache_hits += stats.update_cache_hits;
    budget_counters_.update_cache_misses += stats.update_cache_misses;
    budget_counters_.update_cache_fallbacks += stats.update_cache_fallbacks;
    budget_counters_.update_cache_busy_fallbacks += stats.update_cache_busy_fallbacks;
    budget_counters_.update_cache_variant_fallbacks += stats.update_cache_variant_fallbacks;
    budget_counters_.update_cache_memory_fallbacks += stats.update_cache_memory_fallbacks;
    budget_counters_.update_cache_phase_fallbacks += stats.update_cache_phase_fallbacks;
    if (payload.size() > budget_counters_.largest_snapshot_bytes)
      budget_counters_.largest_snapshot_bytes = payload.size();
    if (!stats.deferred.empty())
      budget_counters_.budget_limited_snapshots += 1;
    budget_counters_.deferred_entries += stats.deferred.size();
    budget_counters_.deferred_constructs += stats.deferred_constructs;
    if (stats.overdraw)
      budget_counters_.overdraw_snapshots += 1;
  }
  return out;
}

bool Server::peer_world_ready(PeerId peer) const {
  const auto found = clients_.find(peer);
  return found != clients_.end() && found->second.world_ready;
}
bool Server::peer_gameplay_ready(PeerId peer) const {
  const auto found = clients_.find(peer);
  return found != clients_.end() && found->second.world_ready && found->second.gameplay_ready;
}
double Server::peer_rtt_seconds(PeerId peer) const {
  const auto found = clients_.find(peer);
  return found == clients_.end() ? 0.0 : found->second.conn->rtt_seconds();
}
bool Server::player_ready(EntityId player) const {
  const auto peer = viewer_peers_.find(player);
  return peer != viewer_peers_.end() && peer_world_ready(peer->second);
}
bool Server::player_gameplay_ready(EntityId player) const {
  const auto peer = viewer_peers_.find(player);
  return peer != viewer_peers_.end() && peer_gameplay_ready(peer->second);
}
void Server::send_chat(EntityId player, const ChatMessage& message) {
  if (!valid_chat_text(message.text, 2048) ||
      (!message.author.empty() && !valid_chat_text(message.author, 96)))
    return;
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (c.world_config && c.world_ready && (!player || c.viewer == player))
      c.conn->queue(kChatChannel, encode_chat_message(c.world_config->revision, message));
  }
}
bool Server::initialize_horse(EntityId horse, EntityId simulator) {
  auto h = world_.slot(horse);
  if (!h || h->type != kHorseType)
    return false;
  const auto rider = world_.read(horse, HorseRider);
  if (simulator && rider && std::get<std::int64_t>(*rider) != 0 &&
      EntityId(std::get<std::int64_t>(*rider)) != simulator) {
    return false;
  }
  bool found = simulator == 0;
  for (const auto& [pid, c] : clients_)
    if (c.viewer == simulator) {
      auto p = world_.slot(simulator);
      found = p && p->owner == pid && body_simulator_candidate(horse, *h, simulator, *p);
    }
  if (!found)
    return false;
  auto old = world_.read(horse, HorseEpoch);
  const auto epoch = old ? std::get<std::int64_t>(*old) + 1 : 1;
  world_.write(horse, HorseSimulator, std::int64_t(simulator));
  world_.write(horse, HorseEpoch, epoch);
  world_.write(horse, HorseNativeState, std::string{});
  horse_sequences_.erase(horse);
  return true;
}

bool Server::set_horse_health(EntityId horse, float health, std::int64_t state) {
  const auto* slot = world_.slot(horse);
  if (!slot || slot->type != kHorseType || state < kHorseAlive || state > kHorseDead ||
      !valid_horse_health(health, std::uint8_t(state))) {
    return false;
  }
  const auto* was = world_.read(horse, HorseCondition);
  const auto* life = world_.read(horse, HorseLife);
  if (!was || !life) {
    return false;
  }
  // A wake or revive: the bodies made for the earlier life are gone, so the clients make new ones.
  if (state == kHorseAlive && std::get<std::int64_t>(*was) != kHorseAlive) {
    world_.write(horse, HorseLife, std::get<std::int64_t>(*life) + 1);
  }
  return world_.write(horse, HorseHealth, health) && world_.write(horse, HorseCondition, state);
}

void Server::handle_horse_stamina(PeerId sender, Client& c, const std::vector<std::uint8_t>& bytes,
                                  double now) {
  HorseStaminaReport report;
  if (!decode_horse_stamina(bytes, report)) {
    return;
  }
  if (!c.world_config || !c.world_ready || !c.gameplay_ready) {
    return;
  }
  const auto* player = world_.slot(c.viewer);
  const auto* horse = world_.slot(report.horse);
  if (!player || player->owner != sender || player->type != kPlayerType || !horse ||
      horse->type != kHorseType || !holds_character(c, report.horse)) {
    return;
  }
  // Only the horse's simulator, in the current epoch, tells its stamina.
  const auto* simulator = world_.read(report.horse, HorseSimulator);
  const auto* epoch = world_.read(report.horse, HorseEpoch);
  if (!simulator || !epoch || std::get<std::int64_t>(*simulator) != std::int64_t(c.viewer) ||
      std::get<std::int64_t>(*epoch) != std::int64_t(report.epoch)) {
    return;
  }
  HorseHealthWindow& window = horse_stamina_windows_[report.horse];
  if (window.start < 0 || now - window.start >= 1) {
    window.start = now;
    window.taken = 0;
  }
  if (window.taken >= kMaxHorseStaminasPerSecond ||
      horse_stamina_inbox_.size() >= kMaxHorseHealthChangesWaiting) {
    return;
  }
  ++window.taken;
  horse_stamina_inbox_.push_back(HorseStaminaChange{report.horse, report.stamina});
}

void Server::handle_horse_health(PeerId sender, Client& c, const std::vector<std::uint8_t>& bytes,
                                 double now) {
  HorseHealthReport report;
  if (!decode_horse_health(bytes, report)) {
    return;
  }
  if (!c.world_config || !c.world_ready || !c.gameplay_ready) {
    return;
  }
  const auto* player = world_.slot(c.viewer);
  const auto* horse = world_.slot(report.horse);
  if (!player || player->owner != sender || player->type != kPlayerType || !horse ||
      horse->type != kHorseType || !holds_character(c, report.horse)) {
    return;
  }
  // Only the horse's simulator, in the current epoch, tells what became of its health.
  const auto* simulator = world_.read(report.horse, HorseSimulator);
  const auto* epoch = world_.read(report.horse, HorseEpoch);
  const auto* state = world_.read(report.horse, HorseCondition);
  const auto* health = world_.read(report.horse, HorseHealth);
  const auto* knock_out = world_.read(report.horse, HorseKnockOut);
  if (!simulator || !epoch || !state || !health || !knock_out ||
      std::get<std::int64_t>(*simulator) != std::int64_t(c.viewer) ||
      std::get<std::int64_t>(*epoch) != std::int64_t(report.epoch)) {
    return;
  }
  HorseHealthWindow& window = horse_health_windows_[report.horse];
  if (window.start < 0 || now - window.start >= 1) {
    window.start = now;
    window.taken = 0;
  }
  if (window.taken >= kMaxHorseHealthsPerSecond) {
    return;
  }
  ++window.taken;
  // A report only moves a horse down: a dead one stays dead, and a knocked-out one wakes only by a
  // resource, so the one thing it can still take is the death of a knocked-out horse.
  const std::int64_t was = std::get<std::int64_t>(*state);
  std::int64_t next = report.state;
  float next_health = report.health;
  if (was == kHorseDead || (was == kHorseKnockedOut && next != kHorseDead)) {
    return;
  }
  // The horse's knockOut option: a death leaves it knocked out instead.
  if (next == kHorseDead && std::get<bool>(*knock_out)) {
    next = kHorseKnockedOut;
  }
  if (next == kHorseDead) {
    next_health = 0.0f;
  }
  if (next == was && next_health == std::get<float>(*health)) {
    return;
  }
  if (horse_health_inbox_.size() >= kMaxHorseHealthChangesWaiting) {
    return;
  }
  HorseHealthChange change;
  change.horse = report.horse;
  change.previous_health = std::get<float>(*health);
  change.health = next_health;
  change.previous_state = was;
  change.state = next;
  world_.write(report.horse, HorseHealth, next_health);
  world_.write(report.horse, HorseCondition, next);
  horse_health_inbox_.push_back(change);
}
namespace {
// Client scripts may move their own player at most this often; the rest is dropped.
constexpr unsigned kClientTransformsPerSecond = 10;

// A script number arrives as an Int when it is integral and as a Double otherwise.
bool script_number(const sdk::Value& message, const char* name, double& out) {
  const auto* field = script_field(message, name);
  if (!field)
    return false;
  if (field->tag() == sdk::Tag::Double)
    out = field->as_double();
  else if (field->tag() == sdk::Tag::Int)
    out = double(field->as_int());
  else
    return false;
  return std::isfinite(out);
}
}  // namespace

void Server::handle_client_transform(PeerId sender, Client& c, const sdk::Value& message,
                                     double now) {
  // A client script may move only the player it controls. The request goes through the same
  // authoritative path as a server-side assignment (force_transform / force_rotation), so the
  // server stays the authority and every holder of the entity receives the change.
  if (!c.world_config || !c.world_ready)
    return;
  if (c.transform_window < 0 || now - c.transform_window >= 1) {
    c.transform_window = now;
    c.transform_count = 0;
  }
  if (c.transform_count++ >= kClientTransformsPerSecond)
    return;
  double id = 0;
  if (!script_number(message, "id", id) || id != double(c.viewer))
    return;
  const replication::EntitySlot* player = world_.slot(c.viewer);
  if (!player || player->owner != sender)
    return;
  const auto* field = script_field(message, "field");
  if (!field || field->tag() != sdk::Tag::Str)
    return;
  double x = 0;
  double y = 0;
  double z = 0;
  if (!script_number(message, "x", x) || !script_number(message, "y", y) ||
      !script_number(message, "z", z))
    return;
  if (field->as_str() == "position") {
    force_transform(c.viewer, core::Vec3{float(x), float(y), float(z)});
    return;
  }
  double w = 0;
  if (field->as_str() != "rotation" || !script_number(message, "w", w))
    return;
  const double length = std::sqrt(w * w + x * x + y * y + z * z);
  if (!(length > 1e-6))
    return;
  force_rotation(c.viewer, core::Quat{float(x / length), float(y / length), float(z / length),
                                      float(w / length)});
}

void Server::handle_horse(PeerId sender, Client& c, const std::vector<std::uint8_t>& bytes,
                          bool mount, double now) {
  HorseMount m;
  HorseState state;
  if (mount ? !decode_horse_mount(bytes, m) : !decode_horse(bytes, state))
    return;
  const auto id = mount ? m.horse : state.horse, source = mount ? m.source : state.source;
  const auto revision = mount ? m.revision : state.revision, epoch = mount ? m.epoch : state.epoch;
  auto player = world_.slot(source), horse = world_.slot(id);
  if (!c.world_config || !c.world_ready || !c.gameplay_ready ||
      c.world_config->revision != revision || source != c.viewer || !player ||
      player->owner != sender || player->type != kPlayerType || !horse ||
      horse->type != kHorseType || !body_simulator_candidate(id, *horse, source, *player))
    return;
  auto ep = world_.read(id, HorseEpoch), sim = world_.read(id, HorseSimulator),
       rd = world_.read(id, HorseRider);
  if (!ep || !sim || !rd || std::get<std::int64_t>(*ep) != epoch)
    return;
  if (mount) {
    const auto rider = std::get<std::int64_t>(*rd);
    if (m.mount) {
      if (rider && rider != source)
        return;
      const auto p = player->position, h = horse->position;
      const auto dx = p.x - h.x, dy = p.y - h.y, dz = p.z - h.z;
      if (!horse_vec(p, 1e6f) || dx * dx + dy * dy + dz * dz > 64)
        return;
      for (auto e : world_.live_entities()) {
        auto s = world_.slot(e);
        if (s && s->type == horse->type && e != id) {
          auto r = world_.read(e, HorseRider);
          if (r && std::get<std::int64_t>(*r) == source)
            return;
        }
      }
      if (rider == source)
        return;
      if (!initialize_horse(id, source)) {
        return;
      }
      world_.write(id, HorseRider, std::int64_t(source));
    } else {
      if (rider != source)
        return;
      if (!initialize_horse(id, source)) {
        return;
      }
      world_.write(id, HorseRider, std::int64_t(0));
    }
  } else {
    if (std::get<std::int64_t>(*sim) != source || state.sequence <= horse_sequences_[id])
      return;
    horse_sequences_[id] = state.sequence;
    world_.write(id, HorsePosition, state.position);
    world_.write(id, HorseRotation, state.rotation);
    // Sequence acceptance is independent of publication. A redundant higher sequence must
    // still fence delayed inputs, without forcing the whole native animation blob to every peer.
    // Do not throttle moving reports twice: the sender already bounds their cadence.
    if (body_publications_[id].publish(state, now, false, std::get<std::int64_t>(*rd) != 0)) {
      world_.write(id, HorseNativeState, std::string(bytes.begin(), bytes.end()));
    }
  }
}

namespace {
float straight_distance(const core::Vec3& a, const core::Vec3& b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  const float dz = a.z - b.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The quaternion at unit length; valid_body_state's bounds keep its length near one.
core::Quat unit_quat(core::Quat q) {
  const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  return core::Quat{q.x / length, q.y / length, q.z / length, q.w / length};
}
}  // namespace

EntityId Server::spawn_npc(const NpcSpawn& spawn, EntityId reserved) {
  if (!valid_npc_spawn(spawn)) {
    return core::kInvalidEntity;
  }
  const core::TypeId type = kNpcType;
  EntityId npc = core::kInvalidEntity;
  if (reserved == core::kInvalidEntity) {
    npc = world_.spawn(type);
  } else {
    npc = world_.spawn_reserved(type, reserved);
  }
  if (npc == core::kInvalidEntity) {
    return npc;
  }
  world_.write(npc, NpcPosition, spawn.position);
  world_.write(npc, NpcRotation, unit_quat(spawn.rotation));
  world_.write(npc, NpcSoul, spawn.soul);
  world_.write(npc, NpcActorClass, spawn.actor_class);
  world_.write(npc, NpcName, spawn.name);
  world_.write(npc, NpcScriptId, std::int64_t(npc));
  world_.write(npc, NpcDimension, std::int64_t(spawn.dimension));
  world_.write(npc, NpcSimulator, std::int64_t(0));
  world_.write(npc, NpcEpoch, std::int64_t(0));
  world_.write(npc, NpcVelocity, core::Vec3{});
  world_.write(npc, NpcNativeState, std::string{});
  world_.write(npc, NpcTeleport, std::int64_t(0));
  world_.write(npc, NpcFrozen, spawn.frozen);
  world_.write(npc, NpcHealth, kNpcMaxHealth);
  world_.write(npc, NpcAlive, true);
  world_.write(npc, NpcOutfit, std::string{});
  world_.write(npc, NpcAnimation, std::string{});
  world_.write(npc, NpcSpeech, std::string{});
  world_.write(npc, NpcAppearance, std::string{});
  world_.write(npc, NpcOutfitFixed, false);
  world_.write(npc, NpcLife, std::int64_t(0));
  world_.write(npc, NpcIntent, std::string{});
  world_.write(npc, NpcDefense, kNpcDefenseGame);
  world_.write(npc, NpcDamage, kNpcDamageGame);
  world_.write(npc, NpcNameplate, std::string{});
  world_.write(npc, NpcKinematic, false);
  world_.write(npc, NpcRagdollPose, std::string{});
  world_.write(npc, NpcBrain, true);
  world_.write(npc, NpcTargetable, true);
  world_.write(npc, NpcAttackable, true);
  world_.write(npc, NpcInteractable, true);
  world_.write(npc, NpcLevelGuid, std::string{});
  world_.write(npc, NpcCarcassClosed, false);
  world_.write(npc, NpcTalking, std::string{});
  world_.write(npc, NpcFace, std::string{});
  set_dimension(npc, spawn.dimension);
  npcs_[npc] = NpcSimulation{};
  return npc;
}

bool Server::set_npc_name(EntityId npc, const std::string& name) {
  if (!npcs_.count(npc) || !valid_npc_name(name)) {
    return false;
  }
  return world_.write(npc, NpcName, name);
}

bool Server::set_npc_animation(EntityId npc, const std::string& animation) {
  if (!npcs_.count(npc) || !valid_npc_animation(animation)) {
    return false;
  }
  return world_.write(npc, NpcAnimation, animation);
}

bool Server::set_npc_intent(EntityId npc, const std::string& intent) {
  if (!npcs_.count(npc) || !valid_npc_intent(intent)) {
    return false;
  }
  return world_.write(npc, NpcIntent, intent);
}

bool Server::set_npc_speech(EntityId npc, const std::string& speech) {
  if (!npcs_.count(npc) || !valid_npc_speech(speech)) {
    return false;
  }
  return world_.write(npc, NpcSpeech, speech);
}

bool Server::set_npc_nameplate(EntityId npc, const std::string& field) {
  if (!npcs_.count(npc) || !valid_npc_nameplate(field)) {
    return false;
  }
  return world_.write(npc, NpcNameplate, field);
}

bool Server::set_npc_kinematic(EntityId npc, bool kinematic) {
  return npcs_.count(npc) && world_.write(npc, NpcKinematic, kinematic);
}

bool Server::set_npc_brain(EntityId npc, bool brain) {
  return npcs_.count(npc) && world_.write(npc, NpcBrain, brain);
}

bool Server::set_npc_face(EntityId npc, const std::string& face) {
  return npcs_.count(npc) && world_.write(npc, NpcFace, face);
}

bool Server::set_npc_talking(EntityId npc, const std::string& style) {
  return npcs_.count(npc) && world_.write(npc, NpcTalking, style);
}

bool Server::set_npc_targetable(EntityId npc, bool targetable) {
  return npcs_.count(npc) && world_.write(npc, NpcTargetable, targetable);
}

bool Server::set_npc_attackable(EntityId npc, bool attackable) {
  return npcs_.count(npc) && world_.write(npc, NpcAttackable, attackable);
}

bool Server::set_npc_interactable(EntityId npc, bool interactable) {
  return npcs_.count(npc) && world_.write(npc, NpcInteractable, interactable);
}

bool Server::set_npc_level_guid(EntityId npc, const std::string& guid) {
  return npcs_.count(npc) && valid_npc_level_guid(guid) && world_.write(npc, NpcLevelGuid, guid);
}

bool Server::set_npc_carcass_closed(EntityId npc, bool closed) {
  return npcs_.count(npc) && world_.write(npc, NpcCarcassClosed, closed);
}

bool Server::pin_npc(EntityId npc, EntityId player) {
  const auto simulation = npcs_.find(npc);
  if (simulation == npcs_.end()) {
    return false;
  }
  if (player != core::kInvalidEntity) {
    const auto* slot = world_.slot(player);
    if (!slot || slot->type != kPlayerType) {
      return false;
    }
  }
  simulation->second.pinned = player;
  return true;
}

bool Server::set_npc_appearance(EntityId npc, const std::string& appearance) {
  if (!npcs_.count(npc) || !valid_npc_appearance(appearance)) {
    return false;
  }
  return world_.write(npc, NpcAppearance, appearance);
}

bool Server::set_npc_outfit(EntityId npc, const std::string& outfit) {
  if (!npcs_.count(npc) || !valid_npc_fixed_outfit(outfit)) {
    return false;
  }
  return world_.write(npc, NpcOutfit, outfit) && world_.write(npc, NpcOutfitFixed, true);
}

bool Server::set_npc_health(EntityId npc, float health, bool alive) {
  if (!npcs_.count(npc) || !(health >= 0.0f) || !(health <= kNpcMaxHealth) ||
      (alive && !(health > 0.0f))) {
    return false;
  }
  const auto* was_alive = world_.read(npc, NpcAlive);
  const auto* life = world_.read(npc, NpcLife);
  if (!was_alive || !life) {
    return false;
  }
  if (alive && !std::get<bool>(*was_alive)) {
    world_.write(npc, NpcLife, std::get<std::int64_t>(*life) + 1);
    // A body made for the new life stands: its last life's pose goes.
    world_.write(npc, NpcRagdollPose, std::string{});
  }
  return world_.write(npc, NpcHealth, health) && world_.write(npc, NpcAlive, alive);
}

bool Server::set_npc_frozen(EntityId npc, bool frozen) {
  if (!npcs_.count(npc)) {
    return false;
  }
  return world_.write(npc, NpcFrozen, frozen);
}

bool Server::set_npc_defense(EntityId npc, float defense) {
  const bool game = defense == kNpcDefenseGame;
  if (!npcs_.count(npc) || !std::isfinite(defense) ||
      (!game && (defense < 0.0f || defense > 1.0f))) {
    return false;
  }
  return world_.write(npc, NpcDefense, defense);
}

bool Server::set_npc_damage(EntityId npc, float share) {
  if (!npcs_.count(npc) || !std::isfinite(share) || share < 0.0f || share > kNpcDamageMax) {
    return false;
  }
  return world_.write(npc, NpcDamage, share);
}

bool Server::set_npc_dimension(EntityId npc, std::uint32_t world) {
  if (!npcs_.count(npc) || !set_dimension(npc, world)) {
    return false;
  }
  // The election moves its simulation to a player of the new world.
  return world_.write(npc, NpcDimension, std::int64_t(world));
}

bool Server::teleport_npc(EntityId npc, core::Vec3 position, core::Quat rotation) {
  if (!npcs_.count(npc) || !horse_vec(position, 1e6f) || !horse_quat(rotation)) {
    return false;
  }
  const auto* counter = world_.read(npc, NpcTeleport);
  if (!counter) {
    return false;
  }
  const std::int64_t teleports = std::get<std::int64_t>(*counter) + 1;
  world_.write(npc, NpcPosition, position);
  world_.write(npc, NpcRotation, unit_quat(rotation));
  world_.write(npc, NpcVelocity, core::Vec3{});
  world_.write(npc, NpcTeleport, teleports);
  start_npc_epoch(npc);
  return true;
}

std::int64_t Server::start_npc_epoch(EntityId npc) {
  const auto* old = world_.read(npc, NpcEpoch);
  const std::int64_t epoch = old ? std::get<std::int64_t>(*old) + 1 : 1;
  world_.write(npc, NpcEpoch, epoch);
  world_.write(npc, NpcNativeState, std::string{});
  auto simulation = npcs_.find(npc);
  if (simulation != npcs_.end()) {
    // The NPC's fight goes on with its new simulator (often the one it fights), and its pin stays.
    NpcSimulation fresh;
    fresh.fighter = simulation->second.fighter;
    fresh.fight_until = simulation->second.fight_until;
    fresh.pinned = simulation->second.pinned;
    simulation->second = fresh;
  }
  return epoch;
}

void Server::set_npc_simulator(EntityId npc, EntityId simulator) {
  const auto* current = world_.read(npc, NpcSimulator);
  if (!current) {
    return;
  }
  const EntityId previous = EntityId(std::get<std::int64_t>(*current));
  if (previous == simulator) {
    return;
  }
  world_.write(npc, NpcSimulator, std::int64_t(simulator));
  const std::int64_t epoch = start_npc_epoch(npc);
  if (npc_simulator_inbox_.size() < kMaxNpcSimulatorChanges) {
    npc_simulator_inbox_.push_back({npc, simulator, previous, epoch});
  }
}

bool Server::body_simulator_candidate(EntityId body, const replication::EntitySlot& body_slot,
                                      EntityId player,
                                      const replication::EntitySlot& player_slot) const {
  const auto indexed = viewer_peers_.find(player);
  if (indexed == viewer_peers_.end()) {
    return false;
  }
  const auto client = clients_.find(indexed->second);
  if (client == clients_.end()) {
    return false;
  }
  const Client& c = client->second;
  if (!c.world_config || !c.world_ready || !c.gameplay_ready ||
      player_slot.owner != indexed->second || player_slot.type != kPlayerType) {
    return false;
  }
  // It simulates only what it holds, so the body must be in its view.
  if (!holds_character(c, body)) {
    return false;
  }
  return scope_allows(player_slot, body_slot, indexed->second);
}

void Server::note_npc_fight(EntityId npc, EntityId player, double now) {
  auto simulation = npcs_.find(npc);
  if (simulation == npcs_.end()) {
    return;
  }
  NpcSimulation& fight = simulation->second;
  if (fight.fighter != player && fight.fighter != core::kInvalidEntity && now < fight.fight_until) {
    return;
  }
  fight.fighter = player;
  fight.fight_until = now + kNpcFightSeconds;
}

void Server::note_npc_witnesses(EntityId npc, EntityId player, double now) {
  const auto* fought = world_.slot(npc);
  if (!fought || player == core::kInvalidEntity) {
    return;
  }
  // Expired zones go; the player's own zone moves to this fight.
  std::erase_if(npc_witness_zones_, [&](const NpcWitnessZone& zone) {
    return zone.until <= now || zone.player == player;
  });
  NpcWitnessZone zone;
  zone.player = player;
  zone.position = fought->position;
  zone.until = now + kNpcWitnessSeconds;
  npc_witness_zones_.push_back(zone);
}

EntityId Server::witness_zone_player(EntityId npc, const replication::EntitySlot& slot) const {
  const core::TypeId player_type = kPlayerType;
  for (const NpcWitnessZone& zone : npc_witness_zones_) {
    if (npc_clock_ >= zone.until ||
        straight_distance(zone.position, slot.position) > kNpcWitnessRadius) {
      continue;
    }
    const auto* player = world_.slot(zone.player);
    if (!player || player->type != player_type) {
      continue;
    }
    if (!body_simulator_candidate(npc, slot, zone.player, *player) ||
        straight_distance(player->position, slot.position) > kNpcSimulatorKeepRange) {
      continue;
    }
    return zone.player;
  }
  return core::kInvalidEntity;
}

EntityId Server::choose_npc_simulator(EntityId npc, const replication::EntitySlot& slot) const {
  const auto* field = world_.read(npc, NpcSimulator);
  const EntityId current = field ? EntityId(std::get<std::int64_t>(*field)) : 0;
  const core::TypeId player_type = kPlayerType;
  // A pinned NPC (Npc.pin) is run by that player's client while it may and is in range, and by
  // nobody otherwise; it does not go to another player.
  if (const auto simulation = npcs_.find(npc);
      simulation != npcs_.end() && simulation->second.pinned != core::kInvalidEntity) {
    const EntityId pinned = simulation->second.pinned;
    const auto* player = world_.slot(pinned);
    if (!player || player->type != player_type) {
      return core::kInvalidEntity;
    }
    const bool may = body_simulator_candidate(npc, slot, pinned, *player) &&
                     straight_distance(player->position, slot.position) <= kNpcSimulatorKeepRange;
    return may ? pinned : core::kInvalidEntity;
  }
  // The player the NPC fights runs it, while it may.
  if (const auto simulation = npcs_.find(npc); simulation != npcs_.end()) {
    const NpcSimulation& fight = simulation->second;
    const auto* fighter = fight.fighter != core::kInvalidEntity && npc_clock_ < fight.fight_until
                              ? world_.slot(fight.fighter)
                              : nullptr;
    if (fighter && fighter->type == player_type &&
        body_simulator_candidate(npc, slot, fight.fighter, *fighter) &&
        straight_distance(fighter->position, slot.position) <= kNpcSimulatorKeepRange) {
      return fight.fighter;
    }
  }
  // A witness of a fight goes to the game that has the fight.
  if (const EntityId witness_of = witness_zone_player(npc, slot);
      witness_of != core::kInvalidEntity) {
    return witness_of;
  }
  EntityId nearest = core::kInvalidEntity;
  float nearest_distance = 0;
  bool current_stays = false;
  float current_distance = 0;
  // The grid measures across the ground; the straight distance decides.
  for (const EntityId player : grid_.query(slot.position, kNpcSimulatorKeepRange)) {
    const auto* player_slot = world_.slot(player);
    if (!player_slot || player_slot->type != player_type) {
      continue;
    }
    if (!body_simulator_candidate(npc, slot, player, *player_slot)) {
      continue;
    }
    const float distance = straight_distance(player_slot->position, slot.position);
    if (player == current && distance <= kNpcSimulatorKeepRange) {
      current_stays = true;
      current_distance = distance;
    }
    if (distance > kNpcSimulatorRange) {
      continue;
    }
    // The lower id wins a tie, so the choice does not depend on the grid's order.
    const bool nearer = nearest == core::kInvalidEntity || distance < nearest_distance ||
                        (distance == nearest_distance && player < nearest);
    if (nearer) {
      nearest = player;
      nearest_distance = distance;
    }
  }
  if (!current_stays) {
    return nearest;
  }
  const bool overtaken = nearest != core::kInvalidEntity && nearest != current &&
                         current_distance - nearest_distance >= kNpcSimulatorCloser;
  if (overtaken) {
    return nearest;
  }
  return current;
}

void Server::elect_npc_simulators() {
  const core::TypeId npc_type = kNpcType;
  for (auto it = npcs_.begin(); it != npcs_.end();) {
    const EntityId npc = it->first;
    const auto* slot = world_.slot(npc);
    if (!slot || slot->type != npc_type) {
      // Gone from the world without despawn(): nothing is left to simulate.
      it = npcs_.erase(it);
      continue;
    }
    // The player it was pinned to left the server: it goes back to the election.
    const EntityId pinned = it->second.pinned;
    if (pinned != core::kInvalidEntity && (!world_.slot(pinned) || !viewer_peers_.count(pinned))) {
      it->second.pinned = core::kInvalidEntity;
    }
    ++it;
    set_npc_simulator(npc, choose_npc_simulator(npc, *slot));
  }
  // A horse nobody owns or rides has no simulator of its own (its owner's client runs an owned
  // one): the nearest player's client takes it, so its orders, health and AI run
  // (live an unowned horse took no order).
  const core::TypeId horse_type = kHorseType;
  for (auto e : world_.live_entities()) {
    const auto* slot = world_.slot(e);
    if (!slot || slot->type != horse_type) {
      continue;
    }
    const auto* simulator = world_.read(e, HorseSimulator);
    if (!simulator) {
      continue;
    }
    const EntityId current = EntityId(std::get<std::int64_t>(*simulator));
    const auto heir = clients_.find(elect_horse_simulator(e));
    const EntityId next = heir == clients_.end() ? 0 : heir->second.viewer;
    if (next != current) {
      initialize_horse(e, next);
    }
  }
}

void Server::handle_npc_outfit(PeerId sender, Client& c, const std::vector<std::uint8_t>& bytes,
                               double now) {
  NpcOutfitReport report;
  if (!decode_npc_outfit(bytes, report)) {
    return;
  }
  if (!c.world_config || !c.world_ready || !c.gameplay_ready ||
      c.world_config->revision != report.revision) {
    return;
  }
  const auto* player = world_.slot(c.viewer);
  if (!player || player->owner != sender || player->type != kPlayerType) {
    return;
  }
  auto simulation = npcs_.find(report.npc);
  if (simulation == npcs_.end() || !world_.slot(report.npc) || !holds_character(c, report.npc)) {
    return;
  }
  // A resource chose what the NPC wears: the simulator's body wears it too and reports nothing.
  const auto* fixed = world_.read(report.npc, NpcOutfitFixed);
  if (fixed && std::get<bool>(*fixed)) {
    return;
  }
  // Only the NPC's simulator, in the current epoch, tells what the body wears.
  const auto* simulator = world_.read(report.npc, NpcSimulator);
  const auto* epoch = world_.read(report.npc, NpcEpoch);
  if (!simulator || !epoch || std::get<std::int64_t>(*simulator) != std::int64_t(c.viewer) ||
      std::get<std::int64_t>(*epoch) != std::int64_t(report.epoch)) {
    return;
  }
  NpcSimulation& taken = simulation->second;
  if (taken.outfit_window < 0 || now - taken.outfit_window >= 1) {
    taken.outfit_window = now;
    taken.outfits = 0;
  }
  if (taken.outfits >= kMaxNpcOutfitsPerSecond) {
    return;
  }
  ++taken.outfits;
  world_.write(report.npc, NpcOutfit, report.outfit);
}

void Server::handle_npc_pose(PeerId sender, Client& c, const std::vector<std::uint8_t>& bytes,
                             double now) {
  NpcPoseReport report;
  if (!decode_npc_pose(bytes, report)) {
    return;
  }
  if (!c.world_config || !c.world_ready || !c.gameplay_ready ||
      c.world_config->revision != report.revision) {
    return;
  }
  const auto* player = world_.slot(c.viewer);
  if (!player || player->owner != sender || player->type != kPlayerType) {
    return;
  }
  auto simulation = npcs_.find(report.npc);
  if (simulation == npcs_.end() || !world_.slot(report.npc) || !holds_character(c, report.npc)) {
    return;
  }
  // Only a dead NPC's body lies, and only its simulator, in the current epoch, tells how.
  const auto* alive = world_.read(report.npc, NpcAlive);
  if (!alive || std::get<bool>(*alive)) {
    return;
  }
  const auto* simulator = world_.read(report.npc, NpcSimulator);
  const auto* epoch = world_.read(report.npc, NpcEpoch);
  if (!simulator || !epoch || std::get<std::int64_t>(*simulator) != std::int64_t(c.viewer) ||
      std::get<std::int64_t>(*epoch) != std::int64_t(report.epoch)) {
    return;
  }
  NpcSimulation& taken = simulation->second;
  if (taken.pose_window < 0 || now - taken.pose_window >= 1) {
    taken.pose_window = now;
    taken.poses = 0;
  }
  if (taken.poses >= kMaxNpcPosesPerSecond) {
    return;
  }
  ++taken.poses;
  world_.write(report.npc, NpcRagdollPose, report.pose);
}

void Server::handle_npc_look(PeerId sender, Client& c, const std::vector<std::uint8_t>& bytes) {
  NpcLookReport report;
  if (!decode_npc_look(bytes, report)) {
    return;
  }
  if (!c.world_config || !c.world_ready || !c.gameplay_ready ||
      c.world_config->revision != report.revision) {
    return;
  }
  const auto* player = world_.slot(c.viewer);
  if (!player || player->owner != sender || player->type != kPlayerType) {
    return;
  }
  if (!npcs_.count(report.npc) || !world_.slot(report.npc) || !holds_character(c, report.npc)) {
    return;
  }
  // Only the NPC's simulator, in the current epoch, tells its body's face, and only the first
  // one: a resource's choice or an earlier simulator's look stays (every body was built from it).
  const auto* simulator = world_.read(report.npc, NpcSimulator);
  const auto* epoch = world_.read(report.npc, NpcEpoch);
  if (!simulator || !epoch || std::get<std::int64_t>(*simulator) != std::int64_t(c.viewer) ||
      std::get<std::int64_t>(*epoch) != std::int64_t(report.epoch)) {
    return;
  }
  const auto* known = world_.read(report.npc, NpcAppearance);
  if (!known || !std::get<std::string>(*known).empty()) {
    return;
  }
  world_.write(report.npc, NpcAppearance, report.look);
}

void Server::handle_npc_intent_done(PeerId sender, Client& c,
                                    const std::vector<std::uint8_t>& bytes, double now) {
  NpcIntentDone done;
  if (!decode_npc_intent_done(bytes, done)) {
    return;
  }
  if (!c.world_config || !c.world_ready || !c.gameplay_ready) {
    return;
  }
  const auto* player = world_.slot(c.viewer);
  if (!player || player->owner != sender || player->type != kPlayerType) {
    return;
  }
  auto simulation = npcs_.find(done.npc);
  const auto* slot = world_.slot(done.npc);
  if (slot && slot->type == kHorseType && holds_character(c, done.npc)) {
    // A horse's order: only its living simulator, in the current epoch, tells it
    // ended; the inbox bounds how many wait.
    const auto* condition = world_.read(done.npc, HorseCondition);
    const auto* simulator = world_.read(done.npc, HorseSimulator);
    const auto* epoch = world_.read(done.npc, HorseEpoch);
    if (condition && simulator && epoch && std::get<std::int64_t>(*condition) == kHorseAlive &&
        std::get<std::int64_t>(*simulator) == std::int64_t(c.viewer) &&
        std::get<std::int64_t>(*epoch) == std::int64_t(done.epoch) &&
        npc_intent_done_inbox_.size() < kMaxNpcIntentDonesWaiting) {
      npc_intent_done_inbox_.push_back(done);
    }
    return;
  }
  if (simulation == npcs_.end() || !slot || !holds_character(c, done.npc)) {
    return;
  }
  // Only a living NPC's simulator, in the current epoch, tells its order ended.
  const auto* alive = world_.read(done.npc, NpcAlive);
  const auto* simulator = world_.read(done.npc, NpcSimulator);
  const auto* epoch = world_.read(done.npc, NpcEpoch);
  if (!alive || !std::get<bool>(*alive) || !simulator || !epoch ||
      std::get<std::int64_t>(*simulator) != std::int64_t(c.viewer) ||
      std::get<std::int64_t>(*epoch) != std::int64_t(done.epoch)) {
    return;
  }
  NpcSimulation& taken = simulation->second;
  if (taken.intent_done_window < 0 || now - taken.intent_done_window >= 1) {
    taken.intent_done_window = now;
    taken.intent_dones = 0;
  }
  if (taken.intent_dones >= kMaxNpcIntentDonesPerSecond ||
      npc_intent_done_inbox_.size() >= kMaxNpcIntentDonesWaiting) {
    return;
  }
  ++taken.intent_dones;
  npc_intent_done_inbox_.push_back(done);
}

void Server::handle_grave(Client& c, const std::vector<std::uint8_t>& bytes, double now) {
  if (!c.world_ready || grave_inbox_.size() >= kMaxGraves) {
    return;
  }
  if (c.grave_window < 0 || now - c.grave_window >= 1) {
    c.grave_window = now;
    c.grave_messages = 0;
  }
  if (c.grave_messages >= kMaxGraveMessagesPerSecond) {
    return;
  }
  Grave grave;
  if (!decode_grave(bytes, grave) || grave.kind == GraveKind::Set) {
    return;
  }
  if (grave.kind == GraveKind::Dug || grave.kind == GraveKind::Digging) {
    // A grave is dug standing at it.
    const auto* player = world_.slot(c.viewer);
    if (!player) {
      return;
    }
    const core::Vec3 at{float(grave.position[0]) / 100.0f, float(grave.position[1]) / 100.0f,
                        float(grave.position[2]) / 100.0f};
    if (!(straight_distance(player->position, at) <= kGraveDigReach)) {
      return;
    }
  }
  ++c.grave_messages;
  if (grave.kind == GraveKind::Digging) {
    // The digging shows on every other screen at once; the state changes with kDug only.
    const auto message = encode_grave(grave);
    for (auto& [pid, other] : clients_) {
      (void)pid;
      if (&other != &c && other.world_ready && !message.empty()) {
        other.conn->queue(0, message);
      }
    }
    return;
  }
  grave_inbox_.emplace_back(c.viewer, grave);
}

bool Server::send_grave_set(EntityId player, const Grave& grave) {
  if (grave.kind != GraveKind::Set) {
    return false;
  }
  const auto message = encode_grave(grave);
  if (message.empty()) {
    return false;
  }
  bool sent = false;
  for (auto& [pid, c] : clients_) {
    (void)pid;
    if (!c.world_ready || (player != 0 && c.viewer != player)) {
      continue;
    }
    c.conn->queue(0, message);
    sent = true;
  }
  return sent;
}

void Server::handle_weapon_hit(Client& c, const std::vector<std::uint8_t>& bytes, double now) {
  if (!c.world_ready || weapon_hit_inbox_.size() >= 256) {
    return;
  }
  if (c.weapon_hit_window < 0 || now - c.weapon_hit_window >= 1) {
    c.weapon_hit_window = now;
    c.weapon_hits = 0;
  }
  if (c.weapon_hits >= kMaxWeaponHitsPerSecond) {
    return;
  }
  WorldRayHit hit;
  if (!decode_weapon_hit(bytes, hit)) {
    return;
  }
  const auto* player = world_.slot(c.viewer);
  if (!player) {
    return;
  }
  const core::Vec3 at{hit.position[0], hit.position[1], hit.position[2]};
  if (!(straight_distance(player->position, at) <= kWeaponHitReach)) {
    return;
  }
  ++c.weapon_hits;
  weapon_hit_inbox_.emplace_back(c.viewer, std::move(hit));
}

void Server::handle_npc_use(PeerId sender, Client& c, const std::vector<std::uint8_t>& bytes,
                            double now) {
  core::EntityId npc = 0;
  if (!decode_npc_use(bytes, npc) || !c.world_config || !c.world_ready || !c.gameplay_ready) {
    return;
  }
  const auto* player = world_.slot(c.viewer);
  if (!player || player->owner != sender || player->type != kPlayerType) {
    return;
  }
  if (!npcs_.count(npc) || !world_.slot(npc) || !holds_character(c, npc)) {
    return;
  }
  if (c.npc_use_window < 0 || now - c.npc_use_window >= 1) {
    c.npc_use_window = now;
    c.npc_uses = 0;
  }
  if (c.npc_uses >= kMaxNpcUsesPerSecond || npc_use_inbox_.size() >= kMaxNpcIntentDonesWaiting) {
    return;
  }
  ++c.npc_uses;
  npc_use_inbox_.push_back(NpcUse{npc, c.viewer});
}

void Server::handle_npc_animation_end(PeerId sender, Client& c,
                                      const std::vector<std::uint8_t>& bytes, double now) {
  NpcAnimationEnd end;
  if (!decode_npc_animation_end(bytes, end)) {
    return;
  }
  if (!c.world_config || !c.world_ready || !c.gameplay_ready) {
    return;
  }
  const auto* player = world_.slot(c.viewer);
  if (!player || player->owner != sender || player->type != kPlayerType) {
    return;
  }
  auto simulation = npcs_.find(end.npc);
  if (simulation == npcs_.end() || !world_.slot(end.npc) || !holds_character(c, end.npc)) {
    return;
  }
  // Only the NPC's simulator, in the current epoch, tells its clip is over.
  const auto* simulator = world_.read(end.npc, NpcSimulator);
  const auto* epoch = world_.read(end.npc, NpcEpoch);
  if (!simulator || !epoch || std::get<std::int64_t>(*simulator) != std::int64_t(c.viewer) ||
      std::get<std::int64_t>(*epoch) != std::int64_t(end.epoch)) {
    return;
  }
  NpcSimulation& taken = simulation->second;
  if (taken.animation_end_window < 0 || now - taken.animation_end_window >= 1) {
    taken.animation_end_window = now;
    taken.animation_ends = 0;
  }
  if (taken.animation_ends >= kMaxNpcAnimationEndsPerSecond ||
      npc_animation_end_inbox_.size() >= kMaxNpcIntentDonesWaiting) {
    return;
  }
  ++taken.animation_ends;
  npc_animation_end_inbox_.push_back(end);
}

void Server::handle_npc_hit(PeerId sender, Client& c, const std::vector<std::uint8_t>& bytes,
                            double now) {
  NpcHit hit;
  if (!decode_npc_hit(bytes, hit)) {
    return;
  }
  if (!c.world_config || !c.world_ready || !c.gameplay_ready ||
      c.world_config->revision != hit.revision) {
    return;
  }
  const auto* player = world_.slot(c.viewer);
  if (!player || player->owner != sender || player->type != kPlayerType) {
    return;
  }
  const auto* npc = world_.slot(hit.npc);
  if (!npcs_.count(hit.npc) || !npc || !holds_character(c, hit.npc)) {
    return;
  }
  const auto* alive = world_.read(hit.npc, NpcAlive);
  if (!alive || !std::get<bool>(*alive)) {
    return;
  }
  if (hit.attacker == c.viewer) {
    // The sender's own player struck: from near enough for a blow or a shot.
    if (straight_distance(player->position, npc->position) > kNpcHitRange) {
      return;
    }
  } else {
    // Anything else that hurt the NPC only its simulator's game sees.
    const auto* simulator = world_.read(hit.npc, NpcSimulator);
    if (!simulator || std::get<std::int64_t>(*simulator) != std::int64_t(c.viewer)) {
      return;
    }
    if (hit.attacker != core::kInvalidEntity && !npcs_.count(hit.attacker)) {
      return;
    }
  }
  if (c.npc_hit_window < 0 || now - c.npc_hit_window >= 1) {
    c.npc_hit_window = now;
    c.npc_hits = 0;
  }
  if (c.npc_hits >= kMaxNpcHitsPerSecond || npc_hit_inbox_.size() >= kMaxNpcHitsWaiting) {
    return;
  }
  ++c.npc_hits;
  npc_hit_inbox_.push_back(hit);
  // A collision (walking into the NPC, E_DamageReason 4) is no fight: it does not take the NPC's
  // simulation over.
  constexpr std::uint8_t kCollisionReason = 4;
  if (hit.attacker == c.viewer && hit.reason != kCollisionReason) {
    note_npc_fight(hit.npc, c.viewer, now);
    note_npc_witnesses(hit.npc, c.viewer, now);
  }
}

void Server::handle_npc_engage(PeerId sender, Client& c, const std::vector<std::uint8_t>& bytes,
                               double now) {
  NpcEngage engage;
  if (!decode_npc_engage(bytes, engage)) {
    return;
  }
  if (!c.world_config || !c.world_ready || !c.gameplay_ready ||
      c.world_config->revision != engage.revision) {
    return;
  }
  const auto* own = world_.slot(c.viewer);
  if (!own || own->owner != sender || own->type != kPlayerType) {
    return;
  }
  const auto* npc = world_.slot(engage.npc);
  const auto* alive = world_.read(engage.npc, NpcAlive);
  if (!npcs_.count(engage.npc) || !npc || !holds_character(c, engage.npc) || !alive ||
      !std::get<bool>(*alive) ||
      straight_distance(own->position, npc->position) > kNpcEngageRange) {
    return;
  }
  if (c.npc_engage_window < 0 || now - c.npc_engage_window >= 1) {
    c.npc_engage_window = now;
    c.npc_engages = 0;
  }
  if (c.npc_engages >= kMaxNpcEngagesPerSecond) {
    return;
  }
  ++c.npc_engages;
  note_npc_fight(engage.npc, c.viewer, now);
  // The resource host decides whether the NPC fights back (an animal).
  if (npc_engage_inbox_.size() < kMaxNpcIntentDonesWaiting) {
    npc_engage_inbox_.push_back(NpcUse{engage.npc, c.viewer});
  }
}

void Server::handle_npc_blow(PeerId sender, Client& c, const std::vector<std::uint8_t>& bytes,
                             double now) {
  NpcBlow blow;
  if (!decode_npc_blow(bytes, blow)) {
    return;
  }
  if (!c.world_config || !c.world_ready || !c.gameplay_ready ||
      c.world_config->revision != blow.revision) {
    return;
  }
  const auto* own = world_.slot(c.viewer);
  if (!own || own->owner != sender || own->type != kPlayerType) {
    return;
  }
  const auto* npc = world_.slot(blow.npc);
  const auto* alive = world_.read(blow.npc, NpcAlive);
  const auto* simulator = world_.read(blow.npc, NpcSimulator);
  if (!npcs_.count(blow.npc) || !npc || !holds_character(c, blow.npc) || !alive ||
      !std::get<bool>(*alive) || !simulator) {
    return;
  }
  const auto simulator_id = core::EntityId(std::get<std::int64_t>(*simulator));
  // The client the blow goes to: the NPC's simulator, or the struck player's own.
  EntityId recipient = core::kInvalidEntity;
  if (blow.direction == NpcBlowDirection::PlayerStruckNpc) {
    if (blow.player != c.viewer || simulator_id == c.viewer ||
        straight_distance(own->position, npc->position) > kNpcHitRange) {
      return;
    }
    recipient = simulator_id;
  } else {
    const auto* struck = world_.slot(blow.player);
    if (simulator_id != c.viewer || blow.player == c.viewer || !struck ||
        struck->type != kPlayerType ||
        straight_distance(struck->position, npc->position) > kNpcHitRange) {
      return;
    }
    recipient = blow.player;
  }
  if (c.npc_blow_window < 0 || now - c.npc_blow_window >= 1) {
    c.npc_blow_window = now;
    c.npc_blows = 0;
  }
  if (c.npc_blows >= kMaxNpcBlowsPerSecond) {
    return;
  }
  ++c.npc_blows;
  note_npc_fight(blow.npc, blow.player, now);
  note_npc_witnesses(blow.npc, blow.player, now);
  for (auto& [pid, other] : clients_) {
    (void)pid;
    if (other.viewer != recipient || !other.world_config || !other.world_ready ||
        !other.gameplay_ready || !holds_character(other, blow.npc)) {
      continue;
    }
    NpcBlow passed = blow;
    passed.revision = other.world_config->revision;
    other.conn->queue(0, encode_npc_blow(passed));
    return;
  }
}

void Server::handle_npc_state(PeerId sender, Client& c, const std::vector<std::uint8_t>& bytes,
                              double now) {
  NpcState state;
  if (!decode_npc_state(bytes, state)) {
    return;
  }
  if (!c.world_config || !c.world_ready || !c.gameplay_ready ||
      c.world_config->revision != state.revision) {
    return;
  }
  // Only the sender's own player, and only for an NPC it holds.
  const auto* player = world_.slot(state.source);
  if (state.source != c.viewer || !player || player->owner != sender ||
      player->type != kPlayerType) {
    return;
  }
  auto simulation = npcs_.find(state.npc);
  if (simulation == npcs_.end() || !world_.slot(state.npc) || !holds_character(c, state.npc)) {
    return;
  }
  const auto* simulator = world_.read(state.npc, NpcSimulator);
  const auto* epoch = world_.read(state.npc, NpcEpoch);
  if (!simulator || !epoch || std::get<std::int64_t>(*simulator) != std::int64_t(state.source) ||
      std::get<std::int64_t>(*epoch) != std::int64_t(state.epoch)) {
    return;
  }
  NpcSimulation& taken = simulation->second;
  if (state.sequence <= taken.sequence) {
    return;
  }
  if (taken.window < 0 || now - taken.window >= 1) {
    taken.window = now;
    taken.states = 0;
  }
  if (taken.states >= kMaxNpcStatesPerSecond) {
    return;
  }
  ++taken.states;
  taken.sequence = state.sequence;
  world_.write(state.npc, NpcPosition, state.position);
  world_.write(state.npc, NpcRotation, state.rotation);
  world_.write(state.npc, NpcVelocity, state.velocity);
  // Preserve complete valid payloads for late join and refresh current native progress at idle,
  // while accepting every monotonic sequence independently of whether a delta is published. What
  // the body holds changing goes out at once (issue 016).
  const bool hands_changed = state.hands != taken.hands;
  if (body_publications_[state.npc].publish(state, now, false) || hands_changed) {
    taken.hands = state.hands;
    world_.write(state.npc, NpcNativeState, std::string(bytes.begin(), bytes.end()));
  }
}

bool Server::send_script(PeerId peer, const sdk::Value& value) {
  // Encoded once: a broadcast goes to every client with the same bytes.
  const std::vector<std::uint8_t> bytes = encode_script(value);
  if (bytes.empty())
    return false;
  bool sent = false;
  for (auto& [id, client] : clients_) {
    if ((peer && id != peer) || !client.world_config)
      continue;
    sent = queue_script_bytes(*client.conn, client.world_config->revision, client.script_sequence,
                              bytes) ||
           sent;
  }
  return sent;
}

Server::ReliableCounters Server::reliable_counters() const {
  ReliableCounters counters = departed_reliable_;
  for (const auto& [peer, client] : clients_) {
    (void)peer;
    counters.sent += client.conn->reliable_sent_count();
    counters.retransmits += client.conn->reliable_retransmit_count();
  }
  return counters;
}

std::size_t Server::outstanding_reliable_total() const {
  std::size_t total = 0;
  for (const auto& [peer, client] : clients_) {
    (void)peer;
    total += client.conn->outstanding_reliable();
  }
  return total;
}

std::vector<PeerId> Server::client_ids() const {
  std::vector<PeerId> ids;
  ids.reserve(clients_.size());
  for (const auto& [pid, c] : clients_) {
    (void)c;
    ids.push_back(pid);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

void Server::for_each_client(std::size_t count, const std::function<void(std::size_t)>& fn) {
  if (pool_ && count >= 2) {
    pool_->parallel_for(count, fn);
    return;
  }
  for (std::size_t i = 0; i < count; ++i) {
    fn(i);
  }
}

void Server::handle_message(PeerId pid, Client& c, transport::DeliveredMessage& msg, double now,
                            std::vector<const std::vector<std::uint8_t>*>& owner_input) {
  if (msg.channel == kScriptChannel) {
    if (!c.world_config)
      return;
    auto value = c.script_receiver.receive(msg.bytes, c.world_config->revision);
    if (!value)
      return;
    const auto op = script_operation(*value);
    if (op == "entityTransform") {
      handle_client_transform(pid, c, *value, now);
      return;
    }
    if (op != "catalogRequest" && op != "catalogChunk" && op != "fileRequest" && op != "event" &&
        op != "resourcesReady" && op != "entityAck" && op != "groundItemRest")
      return;
    if (c.script_window < 0 || now - c.script_window >= 1) {
      c.script_window = now;
      c.script_count = 0;
    }
    if (c.script_count++ < 64 && script_inbox_.size() < 256)
      script_inbox_.push_back({pid, c.viewer, std::move(*value)});
    return;
  }
  if (msg.channel == kHorseMotionChannel || msg.channel == kHorseMountChannel) {
    handle_horse(pid, c, msg.bytes, msg.channel == kHorseMountChannel, now);
    return;
  }
  if (msg.channel == kNpcMotionChannel) {
    handle_npc_state(pid, c, msg.bytes, now);
    return;
  }
  if (msg.channel == kChatChannel) {
    ChatSubmission e;
    std::uint32_t revision = 0;
    if (!c.world_config || !c.world_ready || !decode_chat_submission(msg.bytes, revision, e) ||
        revision != c.world_config->revision || e.player != c.viewer)
      return;
    auto player = world_.slot(e.player);
    if (!player || player->owner != pid)
      return;
    if (c.chat_window < 0 || now - c.chat_window >= 1) {
      c.chat_window = now;
      c.chat_count = 0;
    }
    if (c.chat_count++ < 5 && chat_inbox_.size() < 256)
      chat_inbox_.push_back(std::move(e));
    return;
  }
  if (msg.channel == 0 && c.world_config) {
    std::uint32_t revision = 0;
    if (decode_world_ready(msg.bytes, revision) && revision == c.world_config->revision) {
      if (!c.world_ready && (hit_rules_ || harm_rules_))
        c.conn->queue(0, encode_hit_rules(hit_rules_, harm_rules_));
      if (!c.world_ready && !server_name_.empty())
        c.conn->queue(0, encode_server_name(server_name_));
      c.world_ready = true;
    } else if (decode_gameplay_ready(msg.bytes, revision) && revision == c.world_config->revision &&
               c.world_ready)
      c.gameplay_ready = true;
    else if (decode_knocked_out(msg.bytes, revision) && revision == c.world_config->revision &&
             c.world_ready && !c.knocked_out && knockout_inbox_.size() < 256) {
      c.knocked_out = true;
      knockout_inbox_.push_back(c.viewer);
    } else if (std::string nickname; !c.nickname_received && decode_nickname(msg.bytes, nickname) &&
                                     nickname_inbox_.size() < 256) {
      c.nickname_received = true;
      nickname = clean_nickname(nickname);
      if (!nickname.empty())
        nickname_inbox_.emplace_back(c.viewer, std::move(nickname));
    } else if (Progression progression;
               c.world_ready && decode_progression(msg.bytes, progression)) {
      const auto* player = world_.slot(c.viewer);
      if (!player || player->owner != pid) {
        return;
      }
      const auto* type = world_.schema().get(player->type);
      const auto* ability = type ? type->find("riposteAbility") : nullptr;
      if (ability && ability->kind == replication::FieldKind::I64 &&
          ability->auth == replication::Authority::Server) {
        world_.write(c.viewer, ability->id,
                     std::int64_t(owns_riposte_ability(progression) ? 1 : 0));
      }
      // Native capability replication must not depend on a connected script
      // host or free SDK report capacity. Keep the existing SDK inbox bounded.
      if (progression_inbox_.size() < 256) {
        progression_inbox_.emplace_back(c.viewer, std::move(progression));
      }
    } else if (XpGain gain; c.world_ready && decode_xp_gaining(msg.bytes, gain)) {
      if (c.xp_window < 0 || now - c.xp_window >= 1) {
        c.xp_window = now;
        c.xp_count = 0;
      }
      if (c.xp_count++ < kMaxXpGainsPerSecond && xp_gain_inbox_.size() < 1024)
        xp_gain_inbox_.emplace_back(c.viewer, std::move(gain));
    } else if (PerkId perk{}; c.world_ready && decode_perk_learning(msg.bytes, perk)) {
      if (c.perk_window < 0 || now - c.perk_window >= 1) {
        c.perk_window = now;
        c.perk_count = 0;
      }
      if (c.perk_count++ < kMaxPerkLearningsPerSecond && perk_learning_inbox_.size() < 256)
        perk_learning_inbox_.emplace_back(c.viewer, perk);
    } else if (control_kind(msg.bytes) == kBuffsTag || control_kind(msg.bytes) == kBuffBlockedTag) {
      if (c.buff_window < 0 || now - c.buff_window >= 1) {
        c.buff_window = now;
        c.buff_reports = 0;
        c.buff_blocks = 0;
      }
      std::vector<BuffEntry> buffs;
      Guid blocked{};
      if (c.world_ready && decode_buffs(msg.bytes, buffs) &&
          c.buff_reports++ < kMaxBuffReportsPerSecond && buff_report_inbox_.size() < 256) {
        buff_report_inbox_.emplace_back(c.viewer, std::move(buffs));
      } else if (c.world_ready && decode_buff_blocked(msg.bytes, blocked) &&
                 c.buff_blocks++ < kMaxBuffBlocksPerSecond && buff_block_inbox_.size() < 256) {
        buff_block_inbox_.emplace_back(c.viewer, blocked);
      }
    } else if (control_kind(msg.bytes) == kPlayerHitTag) {
      if (c.hit_window < 0 || now - c.hit_window >= 1) {
        c.hit_window = now;
        c.hit_reports = 0;
      }
      PlayerHitReport hit;
      if (c.world_ready && decode_player_hit(msg.bytes, hit) &&
          c.hit_reports++ < kMaxDamageReportsPerSecond && hit_inbox_.size() < 256) {
        hit_inbox_.emplace_back(c.viewer, hit);
      }
    } else if (control_kind(msg.bytes) == kPlayerHarmTag) {
      // Harm the player's own game holds for a ruling; counted with the blows. One over the limit
      // gets no ruling, and the client's game then takes it as it would have.
      if (c.hit_window < 0 || now - c.hit_window >= 1) {
        c.hit_window = now;
        c.hit_reports = 0;
      }
      PlayerHarm harm;
      if (c.world_ready && harm_rules_ && decode_player_harm(msg.bytes, harm) &&
          c.hit_reports++ < kMaxDamageReportsPerSecond && player_harm_inbox_.size() < 256) {
        player_harm_inbox_.emplace_back(c.viewer, harm);
      }
    } else if (control_kind(msg.bytes) == kPlayerDamageTag) {
      if (c.damage_window < 0 || now - c.damage_window >= 1) {
        c.damage_window = now;
        c.damage_reports = 0;
      }
      PlayerDamage damage;
      if (c.world_ready && decode_player_damage(msg.bytes, damage) &&
          c.damage_reports++ < kMaxDamageReportsPerSecond && damage_inbox_.size() < 256) {
        damage_inbox_.emplace_back(c.viewer, damage);
      }
    } else if (control_kind(msg.bytes) == kShotTag) {
      // A shot: every other client with a world launches it from the shooter's
      // replica.
      if (c.shot_window < 0 || now - c.shot_window >= 1) {
        c.shot_window = now;
        c.shots = 0;
      }
      Shot shot;
      if (c.world_ready && decode_shot(msg.bytes, shot) && c.shots++ < kMaxShotsPerSecond) {
        const auto relay = encode_shot_relay(c.viewer, shot);
        for (auto& [other_pid, other] : clients_) {
          if (other_pid != pid && other.world_ready && !relay.empty()) {
            other.conn->queue(0, relay);
          }
        }
        Client::Flight flight;
        flight.at = now;
        flight.ammo = shot.ammo;
        std::memcpy(flight.position, shot.position, sizeof(flight.position));
        c.flights.push_back(flight);
        if (c.flights.size() > kMaxShotsInFlight) {
          c.flights.pop_front();
        }
      }
    } else if (control_kind(msg.bytes) == kNpcShotTag) {
      // A shot of an NPC's bow: taken only from the NPC's simulator, and every other
      // client with a world launches it from the NPC's body.
      if (c.shot_window < 0 || now - c.shot_window >= 1) {
        c.shot_window = now;
        c.shots = 0;
      }
      std::uint32_t npc = 0;
      Shot shot;
      if (c.world_ready && decode_npc_shot(msg.bytes, npc, shot) &&
          c.shots++ < kMaxShotsPerSecond) {
        const auto* simulator = world_.read(EntityId(npc), NpcSimulator);
        if (simulator && std::get<std::int64_t>(*simulator) == std::int64_t(c.viewer)) {
          const auto relay = encode_shot_relay(npc, shot);
          for (auto& [other_pid, other] : clients_) {
            if (other_pid != pid && other.world_ready && !relay.empty()) {
              other.conn->queue(0, relay);
            }
          }
        }
      }
    } else if (control_kind(msg.bytes) == kPickpocketTag) {
      // A thief's request or result: the resource host decides and answers.
      PickpocketReport report;
      if (c.world_ready && decode_pickpocket_report(msg.bytes, report) &&
          pickpocket_inbox_.size() < 256) {
        pickpocket_inbox_.emplace_back(c.viewer, report);
      }
    } else if (control_kind(msg.bytes) == kDoorReportTag) {
      // The doors near a player and their state: the resource host compares them.
      std::vector<DoorEntry> doors;
      if (c.world_ready && decode_door_report(msg.bytes, doors) &&
          door_report_inbox_.size() < 256) {
        door_report_inbox_.emplace_back(c.viewer, std::move(doors));
      }
    } else if (control_kind(msg.bytes) == kDoorUseTag) {
      // A player's own use of a door, told before the game applies it.
      DoorUse use;
      if (c.world_ready && decode_door_use(msg.bytes, use) && door_use_inbox_.size() < 256) {
        door_use_inbox_.emplace_back(c.viewer, use);
      }
    } else if (control_kind(msg.bytes) == kDoorNearestReplyTag) {
      // The answer to a nearest-door question.
      DoorNearestReply reply;
      if (c.world_ready && decode_door_nearest_reply(msg.bytes, reply) &&
          door_reply_inbox_.size() < 256) {
        door_reply_inbox_.emplace_back(c.viewer, std::move(reply));
      }
    } else if (control_kind(msg.bytes) == kWorldRaycastReplyTag) {
      // What a player's game met along a ray the server asked for.
      WorldRayReply reply;
      if (c.world_ready && decode_world_raycast_reply(msg.bytes, reply) &&
          world_ray_inbox_.size() < 256) {
        world_ray_inbox_.emplace_back(c.viewer, reply);
      }
    } else if (control_kind(msg.bytes) == kCameraShotEndedTag) {
      // A player's camera shot ended without the server clearing it.
      CameraShotEnd reason = CameraShotEnd::Lost;
      if (decode_camera_shot_ended(msg.bytes, reason) && camera_shot_end_inbox_.size() < 256) {
        camera_shot_end_inbox_.emplace_back(c.viewer, reason);
      }
    } else if (control_kind(msg.bytes) == kBodyCarryReportTag) {
      // A carrier's pick-up or let-go of a player's body: the resource host decides.
      BodyCarryReport report;
      if (c.world_ready && decode_body_carry_report(msg.bytes, report) &&
          body_carry_inbox_.size() < 256) {
        body_carry_inbox_.emplace_back(c.viewer, report);
      }
    } else if (control_kind(msg.bytes) == kAnimationEndTag) {
      // How a player's animation request ended in its own game: the resource host
      // raises playerAnimationEnd, once per request.
      if (c.animation_window < 0 || now - c.animation_window >= 1) {
        c.animation_window = now;
        c.animation_ends = 0;
      }
      AnimationEnd end;
      if (c.world_ready && decode_animation_end(msg.bytes, end) &&
          c.animation_ends++ < kMaxAnimationEndsPerSecond && animation_end_inbox_.size() < 256) {
        animation_end_inbox_.emplace_back(c.viewer, end);
      }
    } else if (control_kind(msg.bytes) == kGraveTag) {
      // A grave the player's level has, or one the player dug out.
      handle_grave(c, msg.bytes, now);
    } else if (control_kind(msg.bytes) == kAnimationPropCueTag) {
      // A prop of the player's running animation went into the hand or was put down.
      if (c.prop_cue_window < 0 || now - c.prop_cue_window >= 1) {
        c.prop_cue_window = now;
        c.prop_cues = 0;
      }
      AnimationPropCue cue;
      if (c.world_ready && decode_animation_prop_cue(msg.bytes, cue) &&
          c.prop_cues++ < kMaxAnimationPropCuesPerSecond &&
          animation_prop_cue_inbox_.size() < 256) {
        animation_prop_cue_inbox_.emplace_back(c.viewer, cue);
      }
    } else if (control_kind(msg.bytes) == kWeaponHitTag) {
      // What the player's swing met that is not a body: the player's word, taken
      // within reach of where the server has the player.
      handle_weapon_hit(c, msg.bytes, now);
    } else if (control_kind(msg.bytes) == kNpcOutfitTag) {
      // What the body of an NPC this client simulates wears.
      handle_npc_outfit(pid, c, msg.bytes, now);
    } else if (control_kind(msg.bytes) == kNpcPoseTag) {
      // How the dead body of an NPC this client simulates lies.
      handle_npc_pose(pid, c, msg.bytes, now);
    } else if (control_kind(msg.bytes) == kNpcLookTag) {
      // The face the game chose for the body of an NPC this client simulates.
      handle_npc_look(pid, c, msg.bytes);
    } else if (control_kind(msg.bytes) == kNpcUseTag) {
      // The player pressed the use key looking at a server NPC.
      handle_npc_use(pid, c, msg.bytes, now);
    } else if (control_kind(msg.bytes) == kNpcAnimationEndTag) {
      // The clip a resource played on an NPC this client simulates is over.
      handle_npc_animation_end(pid, c, msg.bytes, now);
    } else if (control_kind(msg.bytes) == kNpcIntentDoneTag) {
      // The order an NPC this client simulates walks ended.
      handle_npc_intent_done(pid, c, msg.bytes, now);
    } else if (control_kind(msg.bytes) == kHorseHealthTag) {
      // The health or state of a horse this client simulates changed.
      handle_horse_health(pid, c, msg.bytes, now);
    } else if (control_kind(msg.bytes) == kHorseStaminaTag) {
      // The stamina of a horse this client simulates changed (Horse.stamina).
      handle_horse_stamina(pid, c, msg.bytes, now);
    } else if (control_kind(msg.bytes) == kNpcHitTag) {
      // Health this client's game would have taken off a body of an NPC.
      handle_npc_hit(pid, c, msg.bytes, now);
    } else if (control_kind(msg.bytes) == kNpcBlowTag) {
      // A blow between an NPC and a player, for the game on the other side.
      handle_npc_blow(pid, c, msg.bytes, now);
    } else if (control_kind(msg.bytes) == kNpcEngageTag) {
      // A player asked to lock on to an NPC.
      handle_npc_engage(pid, c, msg.bytes, now);
    } else if (control_kind(msg.bytes) == kPilloryTag) {
      // A player's client left the pillory it was put in: the resource host raises
      // playerPilloryReleased when a lock pick opened it.
      if (c.pillory_window < 0 || now - c.pillory_window >= 1) {
        c.pillory_window = now;
        c.pillory_reports = 0;
      }
      PilloryReport report;
      if (c.world_ready && decode_pillory_report(msg.bytes, report) &&
          c.pillory_reports++ < kMaxPilloryReportsPerSecond &&
          pillory_release_inbox_.size() < 256) {
        pillory_release_inbox_.emplace_back(c.viewer, std::move(report));
      }
    } else if (control_kind(msg.bytes) == kVoiceKeyTag) {
      std::array<std::uint8_t, 32> key{};
      if (c.world_ready && decode_voice_key(msg.bytes, key) && voice_key_inbox_.size() < 256) {
        voice_key_inbox_.emplace_back(c.viewer, key);
      }
    } else if (control_kind(msg.bytes) == kVoiceTierTag) {
      std::uint8_t tier = 0;
      if (c.world_ready && decode_voice_tier(msg.bytes, tier) && voice_tier_inbox_.size() < 256) {
        voice_tier_inbox_.emplace_back(c.viewer, tier);
      }
    } else if (control_kind(msg.bytes) == kWhistleTag) {
      // The player whistled: the resource host tells the players near them.
      if (c.world_ready && decode_whistle_report(msg.bytes) &&
          (c.whistled_at < 0 || now - c.whistled_at >= 1) && whistle_inbox_.size() < 256) {
        c.whistled_at = now;
        whistle_inbox_.push_back(c.viewer);
      }
    } else if (control_kind(msg.bytes) == kLevelTravelTag) {
      // The player began a fast travel: the resource host decides whether it leads to another
      // of the server's maps.
      TravelTarget target;
      if (c.world_ready && decode_travel_target(msg.bytes, target) &&
          (c.travel_target_at < 0 || now - c.travel_target_at >= 1) &&
          travel_target_inbox_.size() < 256) {
        c.travel_target_at = now;
        travel_target_inbox_.emplace_back(c.viewer, target);
      }
    } else if (control_kind(msg.bytes) == kNpcBarkTag) {
      // A server NPC said something on its simulator: taken only from that simulator, and the
      // resource host lets every player see it.
      if (c.bark_window < 0 || now - c.bark_window >= 1) {
        c.bark_window = now;
        c.barks = 0;
      }
      NpcBark bark;
      if (c.world_ready && decode_npc_bark(msg.bytes, bark) && c.barks++ < kMaxNpcBarksPerSecond &&
          npc_bark_inbox_.size() < 256) {
        const auto* simulator = world_.read(EntityId(bark.npc), NpcSimulator);
        if (simulator && std::get<std::int64_t>(*simulator) == std::int64_t(c.viewer)) {
          npc_bark_inbox_.emplace_back(c.viewer, std::move(bark));
        }
      }
    } else if (control_kind(msg.bytes) == kInteractionFiredTag) {
      // A player used a world interaction: the resource host runs its handler.
      if (c.interaction_window < 0 || now - c.interaction_window >= 1) {
        c.interaction_window = now;
        c.interactions_fired = 0;
      }
      std::uint32_t id = 0;
      if (c.world_ready && decode_interaction_fired(msg.bytes, id) &&
          c.interactions_fired++ < kMaxInteractionsFiredPerSecond &&
          interaction_inbox_.size() < 256) {
        interaction_inbox_.emplace_back(c.viewer, id);
      }
    } else if (control_kind(msg.bytes) == kSiegeLadderTag ||
               control_kind(msg.bytes) == kStonePileTag) {
      // A player's push or raise of a siege ladder, the end of the step they acted, or a stone
      // they took off a pile and where it came down: the resource host rules on all.
      if (c.siege_window < 0 || now - c.siege_window >= 1) {
        c.siege_window = now;
        c.siege_reports = 0;
      }
      const bool allowed = c.world_ready && c.siege_reports++ < kMaxSiegeReportsPerSecond;
      SiegeLadderReport ladder;
      StonePileReport pile;
      if (allowed && decode_siege_ladder_report(msg.bytes, ladder) &&
          siege_ladder_inbox_.size() < kMaxSiegeReports) {
        siege_ladder_inbox_.emplace_back(c.viewer, ladder);
      } else if (allowed && decode_stone_pile_report(msg.bytes, pile) &&
                 stone_pile_inbox_.size() < kMaxSiegeReports) {
        stone_pile_inbox_.emplace_back(c.viewer, pile);
      }
    } else if (control_kind(msg.bytes) == kCraftTableTag) {
      // What lies on the player's alchemy table: a few a second per client.
      if (c.craft_table_window < 0 || now - c.craft_table_window >= 1) {
        c.craft_table_window = now;
        c.craft_tables = 0;
      }
      CraftTable table;
      if (c.world_ready && c.craft_tables++ < kMaxCraftTablesPerSecond &&
          decode_craft_table(msg.bytes, table) && craft_table_inbox_.size() < 256) {
        craft_table_inbox_.emplace_back(c.viewer, std::move(table));
      }
    } else if (control_kind(msg.bytes) == kCraftReportTag) {
      // What the player's game does at a crafting station: the resource
      // host keeps the sessions and answers an Ask.
      if (c.craft_window < 0 || now - c.craft_window >= 1) {
        c.craft_window = now;
        c.craft_reports = 0;
      }
      CraftReport report;
      if (c.world_ready && c.craft_reports++ < kMaxCraftReportsPerSecond &&
          decode_craft_report(msg.bytes, report) && craft_report_inbox_.size() < 256) {
        craft_report_inbox_.emplace_back(c.viewer, std::move(report));
      }
    } else if (control_kind(msg.bytes) == kDiceViewTag) {
      // Where the dice table's things are in the thrower's game.
      if (c.dice_view_window < 0 || now - c.dice_view_window >= 1) {
        c.dice_view_window = now;
        c.dice_views = 0;
      }
      DiceView view;
      if (c.world_ready && c.dice_views++ < kMaxDiceViewsPerSecond &&
          decode_dice_view(msg.bytes, view) && dice_view_inbox_.size() < 256) {
        dice_view_inbox_.emplace_back(c.viewer, std::move(view));
      }
    } else if (control_kind(msg.bytes) == kDiceMoveTag) {
      // A move in the player's dice match: the resource host checks it against the
      // match it runs.
      if (c.dice_window < 0 || now - c.dice_window >= 1) {
        c.dice_window = now;
        c.dice_moves = 0;
      }
      DiceMove move;
      if (c.world_ready && c.dice_moves++ < kMaxDiceMovesPerSecond &&
          decode_dice_move(msg.bytes, move) && dice_move_inbox_.size() < 256) {
        dice_move_inbox_.emplace_back(c.viewer, move);
      }
    } else if (control_kind(msg.bytes) == kGatherReportTag) {
      // A herb the player's game picked: the resource host decides whether it stands.
      if (c.gather_window < 0 || now - c.gather_window >= 1) {
        c.gather_window = now;
        c.gather_reports = 0;
      }
      GatherReport report;
      if (c.world_ready && c.gather_reports++ < kMaxGatherReportsPerSecond &&
          decode_gather_report(msg.bytes, report) && gather_report_inbox_.size() < 256) {
        gather_report_inbox_.emplace_back(c.viewer, report);
      }
    } else if (control_kind(msg.bytes) == kDialogueChoiceTag ||
               control_kind(msg.bytes) == kDialogueCancelTag) {
      // A row the player picked in a conversation, or a conversation they ended: the
      // resource host checks both against the session it keeps.
      if (c.dialogue_window < 0 || now - c.dialogue_window >= 1) {
        c.dialogue_window = now;
        c.dialogue_reports = 0;
      }
      const bool allowed = c.world_ready && c.dialogue_reports++ < kMaxDialogueReportsPerSecond;
      DialogueChoice choice;
      DialogueCancel cancel;
      if (allowed && decode_dialogue_choice(msg.bytes, choice) &&
          dialogue_choice_inbox_.size() < 256) {
        dialogue_choice_inbox_.emplace_back(c.viewer, choice);
      } else if (allowed && decode_dialogue_cancel(msg.bytes, cancel) &&
                 dialogue_cancel_inbox_.size() < 256) {
        dialogue_cancel_inbox_.emplace_back(c.viewer, cancel);
      }
    } else if (control_kind(msg.bytes) == kVendorTradeTag ||
               control_kind(msg.bytes) == kVendorClosedTag ||
               control_kind(msg.bytes) == kVendorHaggleTag) {
      // A basket the player confirmed on a vendor's screen, a screen that went away on their
      // side, or a price they offered in the haggle (0.4.1): the resource
      // host settles, checks and answers them against its sessions.
      if (c.vendor_window < 0 || now - c.vendor_window >= 1) {
        c.vendor_window = now;
        c.vendor_reports = 0;
      }
      const bool allowed = c.vendor_reports++ < kMaxVendorReportsPerSecond;
      VendorTrade trade;
      VendorClosed closed;
      VendorHaggle haggle;
      if (allowed && c.world_ready && decode_vendor_trade(msg.bytes, trade) &&
          vendor_trade_inbox_.size() < 256) {
        vendor_trade_inbox_.emplace_back(c.viewer, std::move(trade));
      } else if (allowed && decode_vendor_closed(msg.bytes, closed) &&
                 vendor_closed_inbox_.size() < 256) {
        vendor_closed_inbox_.emplace_back(c.viewer, closed);
      } else if (allowed && c.world_ready && decode_vendor_haggle(msg.bytes, haggle) &&
                 vendor_haggle_inbox_.size() < 256) {
        vendor_haggle_inbox_.emplace_back(c.viewer, std::move(haggle));
      }
    } else if (control_kind(msg.bytes) == kSiegeUseTag ||
               control_kind(msg.bytes) == kSiegeImpactTag) {
      // A player's ask of a siege engine, or where a shot their game reports came down (issue):
      // the resource host rules on both.
      if (c.siege_window < 0 || now - c.siege_window >= 1) {
        c.siege_window = now;
        c.siege_reports = 0;
      }
      const bool allowed = c.world_ready && c.siege_reports++ < kMaxSiegeReportsPerSecond;
      SiegeUse use;
      SiegeImpact impact;
      if (allowed && decode_siege_use(msg.bytes, use) &&
          siege_use_inbox_.size() < kMaxSiegeReports) {
        siege_use_inbox_.emplace_back(c.viewer, use);
      } else if (allowed && decode_siege_impact(msg.bytes, impact) &&
                 siege_impact_inbox_.size() < kMaxSiegeReports) {
        siege_impact_inbox_.emplace_back(c.viewer, impact);
      }
    } else if (control_kind(msg.bytes) == kPlacerReportTag) {
      // An event of a player's prop placement: the resource host decides on it.
      if (c.placer_report_window < 0 || now - c.placer_report_window >= 1) {
        c.placer_report_window = now;
        c.placer_reports = 0;
      }
      PlacerReport report;
      if (c.world_ready && decode_placer_report(msg.bytes, report) &&
          c.placer_reports++ < kMaxPlacerReportsPerSecond && placer_report_inbox_.size() < 256) {
        placer_report_inbox_.emplace_back(c.viewer, report);
      }
    } else if (control_kind(msg.bytes) == kShotLandedTag) {
      // Where a shot's projectile lies: the resource host lays it as a ground stack.
      // Only a shot of this client still in flight, of that ammunition and from near there, lands,
      // and only once, so no client lays ammunition it did not shoot.
      ShotLanded landed;
      if (c.world_ready && decode_shot_landed(msg.bytes, landed) &&
          shot_landed_inbox_.size() < 256) {
        while (!c.flights.empty() && now - c.flights.front().at > kShotFlightSeconds) {
          c.flights.pop_front();
        }
        for (auto it = c.flights.begin(); it != c.flights.end(); ++it) {
          const float dx = landed.position[0] - it->position[0];
          const float dy = landed.position[1] - it->position[1];
          const float dz = landed.position[2] - it->position[2];
          if (it->ammo == landed.ammo &&
              dx * dx + dy * dy + dz * dz <= kShotFlightReach * kShotFlightReach) {
            c.flights.erase(it);
            shot_landed_inbox_.emplace_back(c.viewer, landed);
            break;
          }
        }
      }
    } else if (control_kind(msg.bytes) == kWorldPickupTag) {
      if (c.world_pickup_window < 0 || now - c.world_pickup_window >= 1) {
        c.world_pickup_window = now;
        c.world_pickups = 0;
      }
      std::string key;
      if (c.world_ready && decode_world_pickup(msg.bytes, key) &&
          c.world_pickups++ < kMaxWorldPickupsPerSecond && world_pickup_inbox_.size() < 256) {
        world_pickup_inbox_.emplace_back(c.viewer, std::move(key));
      }
    } else if (control_kind(msg.bytes) == kPropReportTag) {
      if (c.prop_window < 0 || now - c.prop_window >= 1) {
        c.prop_window = now;
        c.prop_reports = 0;
      }
      PropReport report;
      if (c.world_ready && decode_prop_report(msg.bytes, report) &&
          c.prop_reports++ < kMaxPropReportsPerSecond && prop_report_inbox_.size() < 1024) {
        prop_report_inbox_.emplace_back(c.viewer, report);
      }
    } else if (control_kind(msg.bytes) == kCartReportTag) {
      if (c.cart_window < 0 || now - c.cart_window >= 1) {
        c.cart_window = now;
        c.cart_reports = 0;
      }
      CartReport report;
      if (c.world_ready && decode_cart_report(msg.bytes, report) &&
          c.cart_reports++ < kMaxCartReportsPerSecond && cart_report_inbox_.size() < 256) {
        cart_report_inbox_.emplace_back(c.viewer, report);
      }
    } else if (control_kind(msg.bytes) == kCarryReportTag) {
      if (c.carry_window < 0 || now - c.carry_window >= 1) {
        c.carry_window = now;
        c.carry_reports = 0;
      }
      CarryPutDown put_down;
      if (c.world_ready && decode_carry_put_down(msg.bytes, put_down) &&
          c.carry_reports++ < kMaxCarryReportsPerSecond && carry_put_down_inbox_.size() < 256) {
        carry_put_down_inbox_.emplace_back(c.viewer, put_down);
      }
    } else if (control_kind(msg.bytes) == kInventoryReportTag) {
      if (c.inventory_window < 0 || now - c.inventory_window >= 1) {
        c.inventory_window = now;
        c.inventory_reports = 0;
      }
      InventoryReport report;
      if (!c.world_ready || !decode_inventory_report(msg.bytes, report)) {
        return;
      }
      const bool busy = c.inventory_reports++ >= kMaxInventoryReportsPerSecond ||
                        inventory_report_inbox_.size() >= 256;
      if (report.kind == InventoryReport::Kind::Changes) {
        // A batch is answered even when the server is busy, so the client sends it again
        // instead of losing it; a client that floods gets no more answers than this.
        if (c.inventory_reports <= 4 * kMaxInventoryReportsPerSecond) {
          handle_inventory_changes(c, std::move(report), busy, now);
        }
        return;
      }
      if (busy) {
        return;
      }
      if (report.kind == InventoryReport::Kind::Census) {
        if (c.inventory_census) {
          return;
        }
        c.inventory_census = true;
      }
      inventory_report_inbox_.emplace_back(c.viewer, std::move(report));
    } else if (control_kind(msg.bytes) == kStashUnlockTag) {
      // A container's lock the player opened in their game; the scripts decide.
      if (c.stash_unlock_window < 0 || now - c.stash_unlock_window >= 1) {
        c.stash_unlock_window = now;
        c.stash_unlocks = 0;
      }
      StashUnlock unlock;
      if (c.world_ready && decode_stash_unlock(msg.bytes, unlock) &&
          c.stash_unlocks++ < kMaxStashUnlocksPerSecond && stash_unlock_inbox_.size() < 256) {
        stash_unlock_inbox_.emplace_back(c.viewer, unlock);
      }
    } else if (control_kind(msg.bytes) == kQuestTrackingTag) {
      // A quest the player started or stopped following in their game; the scripts
      // hear of it.
      if (c.quest_tracking_window < 0 || now - c.quest_tracking_window >= 1) {
        c.quest_tracking_window = now;
        c.quest_tracking_reports = 0;
      }
      QuestTracking tracking;
      if (c.world_ready && decode_quest_tracking(msg.bytes, tracking) &&
          c.quest_tracking_reports++ < kMaxQuestTrackingPerSecond &&
          quest_tracking_inbox_.size() < 256) {
        quest_tracking_inbox_.emplace_back(c.viewer, tracking);
      }
    }
    return;
  }
  if (msg.channel == kCombatChannel) {
    MeleeEvent e;
    if (!decode_melee(msg.bytes, e) || !c.world_config || !c.world_ready ||
        e.revision != c.world_config->revision)
      return;
    auto source = world_.slot(e.source);
    auto target = world_.slot(e.target);
    auto author = e.authority_is_target ? target : source;
    auto viewer = e.authority_is_target ? source : target;
    if (!source || !target || !author || !viewer || author->owner != pid ||
        c.viewer != (e.authority_is_target ? e.target : e.source) ||
        source->stateEpoch != e.source_epoch || target->stateEpoch != e.target_epoch ||
        source->type != target->type ||
        !holds_character(c, e.authority_is_target ? e.source : e.target))
      return;
    auto victim = clients_.find(viewer->owner);
    if (victim == clients_.end() || victim->first == pid ||
        victim->second.viewer != (e.authority_is_target ? e.source : e.target) ||
        !victim->second.world_config || !victim->second.world_ready ||
        victim->second.world_config->revision != e.revision ||
        victim->second.world_config->map != c.world_config->map ||
        !holds_character(victim->second, e.authority_is_target ? e.target : e.source) ||
        e.sequence <= c.last_melee_sequence)
      return;
    // Auxiliary combat traffic honours the same scope as snapshots: never relay a
    // hit between avatars that current visibility separates (different numbered
    // dimensions, or a private-visibility restriction). `confirmed` can lag a
    // just-applied world/visibility change by several ticks under loss, so re-check
    // the live slots instead of trusting the construct bookkeeping alone.
    if (!scope_allows(*author, *viewer, victim->first) || !scope_allows(*viewer, *author, pid))
      return;
    // Lag compensation: a contact counts when the pair stood within reach at some moment the
    // reporting game could have shown.
    if (e.contact && melee_reach_mode_ != MeleeReachMode::Off) {
      const auto a = pose_histories_.find(e.source);
      const auto b = pose_histories_.find(e.target);
      if (a != pose_histories_.end() && b != pose_histories_.end()) {
        const MeleeReachVerdict reach = check_melee_reach(a->second, b->second, now);
        if (!reach.ok) {
          ++melee_refusals_;
          const bool enforce = melee_reach_mode_ == MeleeReachMode::Enforce;
          if (now - c.melee_logged_at >= kMovementLogSeconds) {
            c.melee_logged_at = now;
            std::fprintf(stderr, "MELEE peer=%u source=%u target=%u %s%s\n", unsigned(pid),
                         unsigned(e.source), unsigned(e.target), reach.reason.c_str(),
                         enforce ? ", dropped" : "");
          }
          if (enforce) {
            return;
          }
        }
      }
    }
    c.last_melee_sequence = e.sequence;
    if (e.attack_generation_marker &&
        victim->second.conn->outstanding_reliable() >= kObserverReliableLimit) {
      return;
    }
    victim->second.conn->queue(kCombatChannel, msg.bytes);
    // Only the victim owner commits a contact. Other interested clients may run
    // a noncommitting native preview, fenced to this exact attack generation.
    // The victim owner's reaction (its game's evaluated hit) reaches them too, so
    // their replica of the victim takes the same native hit action.
    if ((e.contact && e.attack_generation && !e.contact_data[304]) || e.reaction) {
      for (auto& [observer_id, observer] : clients_) {
        if (observer_id == pid || observer_id == victim->first ||
            observer.observer_contacts_this_tick >= kMaxObserverContactsPerTick ||
            observer.conn->outstanding_reliable() >= kObserverReliableLimit ||
            !observer.world_ready || !observer.world_config ||
            observer.world_config->revision != e.revision ||
            observer.world_config->map != c.world_config->map ||
            !observer.confirmed.count(e.source) || !observer.confirmed.count(e.target) ||
            !observer.interest_built ||
            std::find(observer.interest.begin(), observer.interest.end(), e.source) ==
                observer.interest.end() ||
            std::find(observer.interest.begin(), observer.interest.end(), e.target) ==
                observer.interest.end()) {
          continue;
        }
        const auto* observer_slot = world_.slot(observer.viewer);
        if (!observer_slot || !scope_allows(*observer_slot, *source, observer_id) ||
            !scope_allows(*observer_slot, *target, observer_id)) {
          continue;
        }
        observer.conn->queue(kCombatChannel, msg.bytes);
        ++observer.observer_contacts_this_tick;
      }
    }
    note_combat(e.source, e.target);
    // An attack press the server relayed is an attack the resources hear of (playerAttack).
    const bool attack = e.input_data.action == CombatAction::AttackMouse ||
                        e.input_data.action == CombatAction::Attack;
    if (e.input && attack && e.input_data.mode == 1 && attack_inbox_.size() < 256)
      attack_inbox_.emplace_back(e.source, e.target);
    return;
  }
  if (msg.channel == static_cast<std::uint8_t>(wire::Channel::Upstream)) {
    // AUTH_OWNER input, authority-checked when it is applied after every client's messages.
    if (!c.world_config || c.world_ready)
      owner_input.push_back(&msg.bytes);
    return;
  }
  if (msg.channel == kChannelReplAck) {
    if (msg.bytes.size() != 4) {
      return;
    }
    const core::Tick acked = decode_tick(msg.bytes);
    // Zero is a real snapshot tick after wrap. A first ACK has no prior baseline to compare,
    // but must name an actual retained outbound snapshot, just like every later ACK.
    const auto it = c.pending_at.find(acked);
    if (it == c.pending_at.end() ||
        (c.has_repl_ack && !core::seq_greater(acked, c.repl.ackedBaseline))) {
      return;
    }
    c.has_repl_ack = true;
    c.repl.ackedBaseline = acked;
    // Packet `acked` carried CONSTRUCTs for every entity pending as of that tick -> confirm
    // them, and drop stale pending records up to and including `acked`.
    for (EntityId e : it->second) {
      c.confirmed.insert(e);
      c.repl.entityBaselines[e] = acked;
    }
    auto updates = c.pending_updates.find(acked);
    if (updates != c.pending_updates.end())
      for (EntityId e : updates->second)
        c.repl.entityBaselines[e] = acked;
    auto leaves = c.pending_leaves.find(acked);
    if (leaves != c.pending_leaves.end())
      for (EntityId e : leaves->second) {
        c.confirmed.erase(e);
        c.sent_scope.erase(e);
        if (e != c.viewer) {
          c.resident_characters.erase(e);
        }
        c.leaving.erase(e);
        c.deferred_since.erase(e);
        c.repl.entityBaselines.erase(e);
      }
    erase_acknowledged_snapshots(c.pending_updates, acked);
    erase_acknowledged_snapshots(c.pending_leaves, acked);
    erase_acknowledged_snapshots(c.pending_at, acked);
  }
}

void Server::tick(double now) {
  const auto receive_start = std::chrono::steady_clock::now();
  const std::vector<PeerId> ids = client_ids();
  std::vector<Client*> clients;
  clients.reserve(ids.size());
  for (auto& [pid, client] : clients_) {
    (void)pid;
    client.observer_contacts_this_tick = 0;
    // Where each player stands now, for the melee reach of the contacts that follow.
    if (melee_reach_mode_ != MeleeReachMode::Off) {
      if (const auto* player = world_.slot(client.viewer)) {
        pose_histories_[client.viewer].record(now, player->position);
      }
    }
  }
  for (PeerId pid : ids) {
    clients.push_back(&clients_.at(pid));
  }

  // Phase 1a (parallel when enabled): every client decodes what it sent. A connection reads only
  // its own link and state.
  std::vector<std::vector<transport::DeliveredMessage>> received(ids.size());
  for_each_client(ids.size(), [&](std::size_t i) { received[i] = clients[i]->conn->receive(now); });

  // Phase 1b (serial): handle every message in arrival order: acks confirm the constructs the
  // client received, the rest goes to the inboxes, relays and handlers. Owner input is kept.
  std::vector<std::vector<const std::vector<std::uint8_t>*>> owner_input(ids.size());
  for (std::size_t i = 0; i < ids.size(); ++i) {
    for (auto& msg : received[i]) {
      handle_message(ids[i], *clients[i], msg, now, owner_input[i]);
    }
  }

  // Phase 1c (parallel when enabled): apply every client's owner input in arrival order. A client
  // writes only owner-authored fields of the entities it owns, so the clients write disjoint rows.
  for_each_client(ids.size(), [&](std::size_t i) {
    auto& client = *clients[i];
    for (auto it = client.player_input_publications.begin();
         it != client.player_input_publications.end();) {
      const auto* slot = world_.slot(it->first);
      if (!slot || slot->owner != ids[i] || slot->type != kPlayerType) {
        it = client.player_input_publications.erase(it);
      } else {
        ++it;
      }
    }
    for (const std::vector<std::uint8_t>* bytes : owner_input[i]) {
      apply_upstream(world_, ids[i], *bytes, now,
                     client.world_config ? client.world_config->revision : 0,
                     client.player_input_publications, movement_mode_, client.movement_checks,
                     client.movement_refused);
    }
  });

  // Serial: the refused steps, counted, and logged with their reason at most every
  // kMovementLogSeconds a client; enforced, the player is put back where its last accepted step
  // ended (a forced transform, whose new epoch starts its check again).
  for (std::size_t i = 0; i < ids.size(); ++i) {
    auto& client = *clients[i];
    for (const auto& refusal : client.movement_refused) {
      ++movement_refusals_;
      const bool enforce = movement_mode_ == MovementCheckMode::Enforce;
      if (now - client.movement_logged_at >= kMovementLogSeconds) {
        client.movement_logged_at = now;
        std::fprintf(stderr, "MOVEMENT peer=%u player=%u %s%s\n", unsigned(ids[i]),
                     unsigned(refusal.player), refusal.verdict.reason.c_str(),
                     enforce ? ", put back" : "");
      }
      if (enforce) {
        force_transform(refusal.player, refusal.verdict.accepted);
      }
    }
    client.movement_refused.clear();
  }

  // Phase 1d (serial): the NPCs' simulators follow the players, a few times a second.
  npc_clock_ = now;
  if (npc_election_at_ < 0 || now - npc_election_at_ >= kNpcElectionSeconds) {
    npc_election_at_ = now;
    elect_npc_simulators();
  }

  // Phase 2 (parallel when enabled): build each client's snapshot (read-only world).
  const auto build_start = std::chrono::steady_clock::now();
  last_receive_seconds_ = std::chrono::duration<double>(build_start - receive_start).count();
  auto payloads = build_all(world_.tick());
  const auto send_start = std::chrono::steady_clock::now();
  last_build_seconds_ = std::chrono::duration<double>(send_start - build_start).count();

  // Phase 3 (parallel when enabled): send on the unreliable snapshot channel. build_all lists the
  // clients in the same order as `clients`.
  for_each_client(payloads.size(), [&](std::size_t i) {
    Client& c = *clients[i];
    // Empty: this peer's snapshot waits for its turn. Its connection still flushes below.
    if (!payloads[i].second.empty()) {
      c.conn->queue(static_cast<std::uint8_t>(wire::Channel::Snapshot), payloads[i].second);
    }
    // The recipe book's pictures, a few parts a tick (send_recipe_picture).
    for (std::size_t part = 0; part < kPicturePartsPerTick && !c.picture_parts.empty(); ++part) {
      c.conn->queue(0, c.picture_parts.front());
      c.picture_parts.pop_front();
    }
    c.conn->flush(now);
  });
  for (const auto& [pid, payload] : payloads) {
    (void)pid;
    snapshot_bytes_sent_ += payload.size();
  }
  last_send_seconds_ =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - send_start).count();
}

}  // namespace kcd2s::integration
