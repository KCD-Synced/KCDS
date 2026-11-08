// Headless client peer: receive snapshots -> apply to the mirror, report
// the newest applied baseline tick back, learn which entity it owns (SET_OWNER), and send its
// owned entity's AUTH_OWNER fields upstream. Engine-agnostic; the real client bridge consumes the
// same Mirror to drive WHGame entities.
#ifndef KCD2S_INTEGRATION_CLIENT_PEER_HPP
#define KCD2S_INTEGRATION_CLIENT_PEER_HPP

#include <algorithm>
#include <optional>
#include "kcd2s/integration/body_state_publication.hpp"
#include "kcd2s/integration/player_input_publication.hpp"
#include "kcd2s/integration/mount_order.hpp"
#include "kcd2s/integration/appearance.hpp"
#include "kcd2s/integration/carry.hpp"
#include "kcd2s/integration/combat_marker.hpp"
#include "kcd2s/integration/restricted_area_marker.hpp"
#include "kcd2s/integration/third_person.hpp"
#include "kcd2s/integration/camera_shot.hpp"
#include "kcd2s/integration/weapon_hits.hpp"
#include "kcd2s/integration/world_ray.hpp"
#include "kcd2s/integration/cuffs.hpp"
#include "kcd2s/integration/frozen.hpp"
#include "kcd2s/integration/player_attach.hpp"
#include "kcd2s/integration/noclip_allowed.hpp"
#include "kcd2s/integration/interaction_rules.hpp"
#include "kcd2s/integration/npc_barks.hpp"
#include "kcd2s/integration/voice_messages.hpp"
#include "kcd2s/integration/whistle.hpp"
#include "kcd2s/integration/level_travel.hpp"
#include "kcd2s/integration/dialogue.hpp"
#include "kcd2s/integration/vendor.hpp"
#include "kcd2s/integration/custom_buffs.hpp"
#include "kcd2s/integration/interactions.hpp"
#include "kcd2s/integration/ladders_stones.hpp"
#include "kcd2s/integration/siege.hpp"
#include "kcd2s/integration/pillory.hpp"
#include "kcd2s/integration/level_edits.hpp"
#include "kcd2s/integration/session_end.hpp"
#include "kcd2s/integration/shots.hpp"
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
#include "kcd2s/integration/body_carry.hpp"
#include "kcd2s/integration/freecam.hpp"
#include "kcd2s/integration/needs.hpp"
#include "kcd2s/integration/pickpocket.hpp"
#include "kcd2s/integration/player_animation.hpp"
#include "kcd2s/integration/world_items.hpp"
#include "kcd2s/integration/combat.hpp"
#include "kcd2s/integration/chat.hpp"
#include "kcd2s/integration/horse.hpp"
#include "kcd2s/integration/npc.hpp"
#include "kcd2s/integration/knockout.hpp"
#include "kcd2s/integration/movement_mode.hpp"
#include "kcd2s/integration/player_identity.hpp"
#include "kcd2s/integration/buffs.hpp"
#include "kcd2s/integration/crafting.hpp"
#include "kcd2s/integration/dice.hpp"
#include "kcd2s/integration/cookpot.hpp"
#include "kcd2s/integration/crafting_table.hpp"
#include "kcd2s/integration/dice_view.hpp"
#include "kcd2s/integration/gathering.hpp"
#include "kcd2s/integration/inventory.hpp"
#include "kcd2s/integration/damage.hpp"
#include "kcd2s/integration/item_overrides.hpp"
#include "kcd2s/integration/item_types.hpp"
#include "kcd2s/integration/stash.hpp"
#include "kcd2s/integration/player_stats.hpp"
#include "kcd2s/integration/player_engine_commands.hpp"
#include "kcd2s/integration/progression.hpp"
#include "kcd2s/integration/world_config.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include <unordered_map>
#include <utility>
#include <vector>

#include "kcd2s/replication/schema.hpp"
#include "kcd2s/replication/snapshot.hpp"
#include "kcd2s/replication/world.hpp"
#include "kcd2s/transport/connection.hpp"
#include "kcd2s/transport/datagram.hpp"

namespace kcd2s::integration {

class ClientPeer {
 public:
  ClientPeer(const replication::SchemaRegistry& schema, transport::IDatagramLink* link)
      : schema_(schema), mirror_(schema), conn_(link) {
    conn_.set_channel_tier(kCombatChannel, transport::DeliveryTier::ReliableOrdered);
    conn_.set_channel_tier(kChatChannel, transport::DeliveryTier::ReliableOrdered);
    conn_.set_channel_payload_budget(kScriptChannel, 4096);
    conn_.set_channel_tier(kHorseMotionChannel, transport::DeliveryTier::UnreliableSequenced);
    conn_.set_channel_tier(kHorseMountChannel, transport::DeliveryTier::ReliableOrdered);
    conn_.set_channel_tier(kNpcMotionChannel, transport::DeliveryTier::Unreliable);
    conn_.set_channel_tier(kChannelReplAck_, transport::DeliveryTier::UnreliableSequenced);
  }

  // One client tick: apply inbound snapshots + control (SET_OWNER), then report applied baseline.
  void tick(double now);
  bool send_script(const sdk::Value& value);
  std::vector<sdk::Value> take_scripts() {
    auto result = std::move(script_inbox_);
    script_inbox_.clear();
    return result;
  }
  bool send_chat(core::EntityId player, const std::string& text);
  bool send_horse_state(HorseState state);
  bool send_horse_mount(core::EntityId player, core::EntityId horse, bool mount);
  // The native health or state of a horse this client simulates changed (HORSE_HEALTH, horse.hpp):
  // `epoch` is the simulation epoch the client holds, `state` kHorseAlive, kHorseKnockedOut or
  // kHorseDead. False when its world is not ready, it holds no such horse or the values do not go
  // together (a living horse has some health).
  // HORSE_STAMINA (horse.hpp): the current stamina of a horse this client simulates.
  bool send_horse_stamina(core::EntityId horse, std::uint32_t epoch, float stamina);
  bool send_horse_health(core::EntityId horse, std::uint32_t epoch, float health,
                         std::uint8_t state);
  // Sends the body state of an NPC this client simulates (NPC_STATE, npc.hpp), with the revision
  // of this client's world and the NPC's epoch as it holds them. False when its world is not
  // ready, it does not own `state.source`, it holds no such NPC, the NPC's simulator is another
  // player or a value is invalid.
  bool send_npc_state(NpcState state);
  // What the body of an NPC the local player simulates wears (NPC_OUTFIT, npc.hpp); the revision
  // and epoch are filled in. False when it cannot be sent.
  bool send_npc_outfit(core::EntityId npc, const std::string& outfit);
  // How the dead body of an NPC the local player simulates lies (NPC_POSE, npc.hpp).
  bool send_npc_pose(core::EntityId npc, const std::string& pose);
  // The character components the game chose for the body of an NPC the local player simulates
  // (NPC_LOOK, npc.hpp); false when it cannot be sent.
  bool send_npc_look(core::EntityId npc, const std::string& look);
  // Health this client's game would have taken off a body of an NPC (NPC_HIT, npc.hpp); the
  // revision is filled in. False when its world is not ready, it holds no such NPC or the hit
  // cannot travel.
  bool send_npc_hit(NpcHit hit);
  // A blow between an NPC and a player for the game on the other side (NPC_BLOW, npc.hpp); the
  // revision is filled in. False when its world is not ready, it holds no such NPC or the blow
  // cannot travel.
  bool send_npc_blow(NpcBlow blow);
  // The local player asked to lock on to the NPC (NPC_ENGAGE, npc.hpp); the revision is filled
  // in. False when its world is not ready or it holds no such NPC.
  bool send_npc_engage(core::EntityId npc);
  // The order the NPC walks ended (NPC_INTENT_DONE, npc.hpp): `epoch` is the simulation epoch the
  // order ran in, `sequence` the order's own number and `status` kNpcIntentReached, ...Blocked or
  // ...Failed. False when its world is not ready, it holds no such NPC or a value is invalid.
  // The player pressed the use key looking at a server NPC's body (NPC_USE, npc.hpp).
  bool send_npc_use(core::EntityId npc);
  // The clip a resource played on an NPC simulated here is over (NPC_ANIMATION_END, npc.hpp).
  bool send_npc_animation_end(core::EntityId npc, std::uint32_t epoch, std::uint32_t sequence,
                              std::uint8_t reason);
  bool send_npc_intent_done(core::EntityId npc, std::uint32_t epoch, std::uint32_t sequence,
                            std::uint8_t status);
  std::vector<ChatMessage> take_chat() {
    auto result = std::move(chat_inbox_);
    chat_inbox_.clear();
    return result;
  }

  const replication::Mirror& mirror() const {
    return mirror_;
  }
  core::Tick baseline() const {
    return mirror_.last_applied();
  }
  const std::optional<WorldConfig>& world_config() const {
    return world_config_;
  }
  bool world_ready() const {
    return world_ready_;
  }
  // Call only after the engine loaded THIS revision in debug mode without quests.
  bool mark_world_ready(std::uint32_t revision) {
    if (!world_config_ || world_config_->revision != revision)
      return false;
    world_ready_ = true;
    conn_.queue(0, encode_world_ready(revision));
    return true;
  }
  // Call only after mark_world_ready for THIS revision, when the game took its own loading screen
  // down. Reported once per revision.
  bool mark_gameplay_started(std::uint32_t revision) {
    if (!world_ready_ || !world_config_ || world_config_->revision != revision || gameplay_started_)
      return false;
    gameplay_started_ = true;
    conn_.queue(0, encode_gameplay_ready(revision));
    return true;
  }
  bool gameplay_started() const {
    return gameplay_started_;
  }
  // The name the local player connects under (NICKNAME); the server takes the first.
  bool send_nickname(const std::string& name) {
    auto message = encode_nickname(name);
    if (!world_config_ || message.empty())
      return false;
    conn_.queue(0, std::move(message));
    return true;
  }
  // The local player's progression (PROGRESSION), when it differs from
  // what this connection last sent for the current world. False when nothing was sent.
  bool send_progression(const Progression& progression) {
    if (!world_ready_ || !world_config_ || progression_sent_ == progression)
      return false;
    auto message = encode_progression(progression);
    if (message.empty())
      return false;
    conn_.queue(0, std::move(message));
    progression_sent_ = progression;
    return true;
  }
  // XP the local player's game produced and held back for the server (XP_GAINING).
  bool send_xp_gaining(const XpGain& gain) {
    auto message = encode_xp_gaining(gain);
    if (!world_ready_ || !world_config_ || message.empty())
      return false;
    conn_.queue(0, std::move(message));
    return true;
  }
  // The status effects on the local player's body (BUFFS), when they differ from what
  // this connection last sent for the current world. False when nothing was sent.
  bool send_buffs(const std::vector<BuffEntry>& buffs) {
    if (!world_ready_ || !world_config_ || buffs_sent_ == buffs)
      return false;
    auto message = encode_buffs(buffs);
    if (message.empty())
      return false;
    conn_.queue(0, std::move(message));
    buffs_sent_ = buffs;
    return true;
  }
  // An effect of a claimed kind the local player's game tried to apply and the client turned
  // down (BUFF_BLOCKED).
  bool send_buff_blocked(const Guid& buff) {
    if (!world_ready_ || !world_config_)
      return false;
    conn_.queue(0, encode_buff_blocked(buff));
    return true;
  }
  // A perk the local player chose on the perk screen and the game allows, held back for the
  // server (PERK_LEARNING).
  bool send_perk_learning(const PerkId& perk) {
    if (!world_ready_ || !world_config_)
      return false;
    conn_.queue(0, encode_perk_learning(perk));
    return true;
  }
  // Health the local player's game took off them (PLAYER_DAMAGE); the server reports
  // it to the resources as playerDamage. False before the world or for a report that cannot
  // travel.
  bool send_player_damage(const PlayerDamage& damage) {
    auto message = encode_player_damage(damage);
    if (!world_ready_ || message.empty())
      return false;
    conn_.queue(0, std::move(message));
    return true;
  }
  // A blow from another player the local player's game holds for the server's ruling
  // (PLAYER_HIT). False before the world or for a report that cannot travel.
  bool send_player_hit(const PlayerHitReport& hit) {
    auto message = encode_player_hit(hit);
    if (!world_ready_ || message.empty())
      return false;
    conn_.queue(0, std::move(message));
    return true;
  }
  // The server's rulings on the local player's held blows since the last call (HIT_VERDICT),
  // oldest first.
  std::vector<HitVerdict> take_hit_verdicts() {
    return std::exchange(hit_verdicts_, {});
  }
  // Whether blows from other players wait for the server's ruling (HIT_RULES): only
  // while some resource handles playerHit.
  bool hit_rules() const {
    return hit_rules_;
  }
  // The local player was knocked out by a lethal hit; the server reports it to the
  // resources as playerDied.
  bool send_knocked_out() {
    if (!world_ready_ || !world_config_)
      return false;
    conn_.queue(0, encode_knocked_out(world_config_->revision));
    return true;
  }
  // The opponents the server ended melee combat with since the last call (COMBAT_CANCEL, issue),
  // oldest first.
  std::vector<std::uint32_t> take_combat_cancels() {
    return std::exchange(combat_cancels_, {});
  }
  // How much of the world the local player sees while knocked out, in percent (KNOCKOUT_VISION;
  // 0, a black screen, until a resource sets it).
  std::uint8_t knockout_vision() const {
    return knockout_vision_;
  }
  // Whether the server shows the game's combat marker, when it said so since the last call
  // (COMBAT_MARKER); empty otherwise.
  std::optional<bool> take_combat_marker() {
    return std::exchange(combat_marker_, std::nullopt);
  }
  // The server ended this session and said why (SESSION_END), once; empty otherwise.
  std::optional<SessionEnd> take_session_end() {
    return std::exchange(session_end_, std::nullopt);
  }
  // Whether the server shows the game's restricted-area marker, when it said so since the last
  // call (RESTRICTED_AREA_MARKER); empty otherwise.
  std::optional<bool> take_restricted_area_marker() {
    return std::exchange(restricted_area_marker_, std::nullopt);
  }
  // The third-person mode the server set (kThirdPersonAllowed, kThirdPersonDisabled or
  // kThirdPersonForced), when it said so since the last call (THIRD_PERSON); empty
  // otherwise.
  std::optional<std::uint8_t> take_third_person() {
    return std::exchange(third_person_, std::nullopt);
  }
  // The rays the server asked this game to trace since the last call, oldest first
  // (WORLD_RAYCAST).
  std::vector<WorldRayRequest> take_world_rays() {
    return std::exchange(world_rays_, {});
  }
  // The answer to a WORLD_RAYCAST (WORLD_RAYCAST_REPLY). False when nothing was sent.
  bool send_world_ray_reply(const WorldRayReply& reply) {
    auto message = encode_world_raycast_reply(reply);
    if (!world_ready_ || !world_config_ || message.empty())
      return false;
    conn_.queue(0, std::move(message));
    return true;
  }
  // The camera orders the server gave since the last call, oldest first (CAMERA_SHOT and
  // CAMERA_CLEAR): a shot to film, or empty to give the player's own view back.
  std::vector<std::optional<CameraShot>> take_camera_orders() {
    return std::exchange(camera_orders_, {});
  }
  // A shot that ended without the server clearing it (CAMERA_SHOT_ENDED).
  bool send_camera_shot_ended(CameraShotEnd reason) {
    auto message = encode_camera_shot_ended(reason);
    if (!world_config_ || message.empty())
      return false;
    conn_.queue(0, std::move(message));
    return true;
  }
  // Whether the server cuffed the local player, when it said so since the last call (CUFFS);
  // empty otherwise.
  // The server's player.mount orders since the last call (MOUNT_ORDER).
  std::vector<MountOrder> take_mount_orders() {
    return std::exchange(mount_orders_, {});
  }
  // Whether the server ordered the player off their horse since the last call (DISMOUNT_ORDER).
  bool take_dismount_order() {
    return std::exchange(dismount_ordered_, false);
  }
  // The pull-downs the server ordered since the last call (Horse.dragRider).
  std::vector<PullDownOrder> take_pull_down_orders() {
    return std::exchange(pull_down_orders_, {});
  }
  // Whether the server ordered the player's horse to rear and throw them off.
  bool take_rear_order() {
    return std::exchange(rear_ordered_, false);
  }
  std::optional<bool> take_cuffs() {
    return std::exchange(cuffs_, std::nullopt);
  }
  // Whether the server froze or freed the local player since the last call (FROZEN).
  std::optional<bool> take_frozen() {
    return std::exchange(frozen_, std::nullopt);
  }
  // The server's orders about players attached to others since the last call, oldest first
  // (Player.attachTo).
  std::vector<PlayerAttach> take_player_attaches() {
    return std::exchange(player_attaches_, {});
  }
  // Whether the server allowed or forbade the local player NoClip since the last call
  // (NOCLIP_ALLOWED).
  std::optional<bool> take_noclip_allowed() {
    return std::exchange(noclip_allowed_, std::nullopt);
  }
  // Which of the game's own interactions the server allows the local player, when it said so
  // since the last call (INTERACTION_RULES); empty otherwise.
  std::optional<std::uint16_t> take_interaction_rules() {
    return std::exchange(interaction_rules_, std::nullopt);
  }
  // The server's name (SERVER_NAME), when it told one since the last call.
  std::optional<std::string> take_server_name() {
    return std::exchange(server_name_, std::nullopt);
  }
  // The connection's round trip, smoothed, in seconds (0 before the first acknowledgement).
  double rtt_seconds() const {
    return conn_.rtt_seconds();
  }
  // Voice chat (kcd2s/integration/voice_messages.hpp): the newest state the server told, once.
  std::optional<VoiceState> take_voice_state() {
    return std::exchange(voice_state_, std::nullopt);
  }
  // Talkers' muffling as it changed: talker -> 0..255.
  std::vector<std::pair<std::uint32_t, std::uint8_t>> take_voice_muffles() {
    return std::exchange(voice_muffles_, {});
  }
  bool send_voice_key(const std::array<std::uint8_t, 32>& key);
  bool send_voice_tier(std::uint8_t tier);
  // The whistles the server told of since the last call (WHISTLE), oldest first: 0 to whistle,
  // otherwise the player who whistled nearby.
  std::vector<std::uint32_t> take_whistles() {
    return std::exchange(whistles_, {});
  }
  // The local player whistled (WHISTLE). False without a ready world.
  bool send_whistle_report();
  // The local player's fast travel ended at (x, y, z) (LEVEL_TRAVEL). False without a ready world.
  bool send_travel_end(float x, float y, float z);
  // A bubble the game showed over a server NPC this client simulates (NPC_BARK). False without a
  // ready world or for an unusable bark.
  bool send_npc_bark(const NpcBark& bark) {
    if (!world_ready_ || !world_config_)
      return false;
    auto message = encode_npc_bark(bark);
    if (message.empty())
      return false;
    conn_.queue(0, std::move(message));
    return true;
  }
  // The local player's world interactions, when the server gave a new list since the last call
  // (INTERACTIONS); empty otherwise.
  std::optional<std::vector<Interaction>> take_interactions() {
    return std::exchange(interactions_, std::nullopt);
  }
  // Blows between an NPC and a player that this client's game plays (NPC_BLOW, npc.hpp, issue),
  // oldest first.
  std::vector<NpcBlow> take_npc_blows() {
    return std::exchange(npc_blows_, {});
  }
  // Tells the server that the local player used a world interaction (INTERACTION_FIRED). False
  // before the world is ready.
  bool send_interaction_fired(std::uint32_t id);
  // The server's conversation pages and closes for the local player since the last call, in the
  // order sent (DIALOGUE_PAGE and DIALOGUE_CLOSE). A burst keeps the first
  // kMaxDialogueOrdersWaiting.
  std::vector<DialogueOrder> take_dialogue_orders() {
    return std::exchange(dialogue_orders_, {});
  }
  // The row the local player picked, or a conversation that ended on their side (DIALOGUE_CHOICE
  // and DIALOGUE_CANCEL). False before the world is ready or for one that cannot travel.
  bool send_dialogue_choice(const DialogueChoice& choice);
  bool send_dialogue_cancel(const DialogueCancel& cancel);
  // The server's vendor shelves, closes and haggle answers for the local player since the last
  // call, in the order sent (VENDOR_SHOW, VENDOR_CLOSE and VENDOR_HAGGLE_REPLY,
  // 0.4.1). A burst keeps the first kMaxVendorOrdersWaiting.
  std::vector<VendorOrder> take_vendor_orders() {
    return std::exchange(vendor_orders_, {});
  }
  // The basket the local player confirmed on a vendor's screen (VENDOR_TRADE) and a price they
  // offered in its haggle (VENDOR_HAGGLE), false before the world is ready; a vendor screen that
  // went away on their side (VENDOR_CLOSED; false without a world config). False too for one that
  // cannot travel.
  bool send_vendor_trade(const VendorTrade& trade);
  bool send_vendor_haggle(const VendorHaggle& haggle);
  bool send_vendor_closed(const VendorClosed& closed);
  // The siege engines' spawns, updates and removals and the shots they let go since the last
  // call, oldest first (SIEGE_ENGINE and SIEGE_SHOT).
  std::vector<SiegeEngineOrder> take_siege_orders() {
    return std::exchange(siege_orders_, {});
  }
  std::vector<SiegeShot> take_siege_shots() {
    return std::exchange(siege_shots_, {});
  }
  // What the local player asks of a siege engine (SIEGE_USE) and where a shot this game reports
  // came down (SIEGE_IMPACT). False before the world is ready or for one that cannot travel.
  bool send_siege_use(const SiegeUse& use) {
    return send_siege(encode_siege_use(use));
  }
  bool send_siege_impact(const SiegeImpact& impact) {
    return send_siege(encode_siege_impact(impact));
  }
  // Tells the server an event of the local player's prop placement (PLACER_REPORT).
  // False before the world is ready or when the report cannot travel.
  bool send_placer_report(const PlacerReport& report);
  // The siege ladders' and stone piles' orders since the last call, oldest first (SIEGE_LADDER
  // and STONE_PILE).
  std::vector<SiegeLadderOrder> take_siege_ladder_orders() {
    return std::exchange(siege_ladder_orders_, {});
  }
  std::vector<StonePileOrder> take_stone_pile_orders() {
    return std::exchange(stone_pile_orders_, {});
  }
  // The local player's push or raise of a ladder, or the end of the step they acted (SIEGE_LADDER),
  // and the stone they took off a pile or where it came down (STONE_PILE). False before the world
  // is ready or for one that cannot travel.
  bool send_siege_ladder_report(const SiegeLadderReport& report) {
    return send_siege(encode_siege_ladder_report(report));
  }
  bool send_stone_pile_report(const StonePileReport& report) {
    return send_siege(encode_stone_pile_report(report));
  }
  // The server's orders to put the local player into a pillory or take them out since the last
  // call, oldest first (PILLORY). A burst keeps the newest kMaxPilloryOrders.
  std::vector<PilloryOrder> take_pillory_orders() {
    return std::exchange(pillory_orders_, {});
  }
  // Tells the server about a pillory (PILLORY_RELEASED): the local player is out of
  // theirs (kPilloryLockpicked when a lock pick or a key opened its lock, kPilloryLeft
  // otherwise), or a pillory lock was opened in this game (kPilloryLockOpened, with the lock's
  // name).
  bool send_pillory_report(const PilloryReport& report) {
    if (!world_ready_ || !world_config_) {
      return false;
    }
    auto message = encode_pillory_report(report);
    if (message.empty()) {
      return false;
    }
    conn_.queue(0, std::move(message));
    return true;
  }
  // The APPEARANCE requests for the local player received since the last call, oldest first.
  std::vector<AppearanceRequest> take_appearance_requests() {
    return std::exchange(appearance_requests_, {});
  }
  // The server's progression orders for the local player received since the last call, oldest
  // first (PROGRESSION_ORDER).
  std::vector<ProgressionOrder> take_progression_orders() {
    return std::exchange(progression_orders_, {});
  }
  // The server's buff orders for the local player received since the last call, oldest first
  // (BUFF_ORDER).
  std::vector<BuffOrder> take_buff_orders() {
    return std::exchange(buff_orders_, {});
  }
  // The server's shows and hides of the resources' own buffs on the local player received since
  // the last call, oldest first (CUSTOM_BUFF).
  std::vector<CustomBuff> take_custom_buffs() {
    return std::exchange(custom_buffs_, {});
  }
  // The newest stock the server sent for the local player since the last call (INVENTORY_SHOW);
  // an older one is replaced, except for quickslots it restored, which a newer stock
  // without them keeps.
  std::optional<InventoryShow> take_inventory_show() {
    return std::exchange(inventory_show_, std::nullopt);
  }
  // The server's verdicts on the local player's uses since the last call, oldest first
  // (INVENTORY_CONSUME).
  std::vector<InventoryConsume> take_inventory_consumes() {
    return std::exchange(inventory_consumes_, {});
  }
  // The server's answers to the local player's asks to open a crafting station since the last
  // call (CRAFT_VERDICT), oldest first.
  std::vector<CraftVerdict> take_craft_verdicts() {
    return std::exchange(craft_verdicts_, {});
  }
  // The local player's dice matches as the server tells them since the last call (DICE_STATE),
  // oldest first.
  std::vector<DiceState> take_dice_states() {
    return std::exchange(dice_states_, {});
  }
  // Whether the server answers a dice table's use (DICE_TABLES): the last word since
  // the last call, if any.
  std::optional<DiceTables> take_dice_tables() {
    return std::exchange(dice_tables_, std::nullopt);
  }
  // Dice matches nearby as the local player watches them (DICE_WATCH), oldest first.
  std::vector<DiceWatch> take_dice_watches() {
    return std::exchange(dice_watches_, {});
  }
  // The server's answers to the local player's picks (GATHER_VERDICT) and the plants
  // picked or grown back in the local player's world (GATHER_PLANTS) since the last call, oldest
  // first.
  std::vector<GatherVerdict> take_gather_verdicts() {
    return std::exchange(gather_verdicts_, {});
  }
  std::vector<GatherPlants> take_gather_plants() {
    return std::exchange(gather_plants_, {});
  }
  // How the stew pots of the local player's world look (POT_LOOKS) since the last
  // call, oldest first.
  std::vector<PotLooks> take_pot_looks() {
    return std::exchange(pot_looks_, {});
  }
  // What the server told the local player to carry or put down since the last call, oldest first
  // (CARRY_ORDER).
  std::vector<CarryOrder> take_carry_orders() {
    return std::exchange(carry_orders_, {});
  }
  // What the containers the local player holds hold, since the last call, oldest first
  // (STASH_SHOW).
  std::vector<StashShow> take_stash_shows() {
    return std::exchange(stash_shows_, {});
  }
  // Item types the server's resources registered since the last call, oldest first (ITEM_TYPES).
  std::vector<ItemTypes> take_item_types() {
    return std::exchange(item_types_, {});
  }
  // The newest set of what the server changes of the game's own items, when one came since the
  // last call (ITEM_OVERRIDES).
  std::optional<ItemOverrides> take_item_overrides() {
    return std::exchange(item_overrides_, std::nullopt);
  }
  // The server's notices of a limit the player reached since the last call, oldest first
  // (ITEM_LIMIT_NOTICE); at most kMaxQueuedItemLimitNotices wait.
  static constexpr std::size_t kMaxQueuedItemLimitNotices = 8;
  std::vector<ItemLimitNotice> take_item_limit_notices() {
    return std::exchange(item_limit_notices_, {});
  }
  // What the server says the game calls items since the last call, oldest first (ITEM_NAMES).
  std::vector<ItemNames> take_item_names() {
    return std::exchange(item_names_, {});
  }
  // Tells the server about a projectile the local player's game launched (SHOT).
  // False when nothing was sent.
  bool send_shot(const Shot& shot) {
    if (!world_ready_ || !world_config_)
      return false;
    auto message = encode_shot(shot);
    if (message.empty())
      return false;
    conn_.queue(0, std::move(message));
    return true;
  }
  // Tells the server about a projectile the bow of an NPC this client simulates fired
  // (NPC_SHOT). False when nothing was sent.
  bool send_npc_shot(std::uint32_t npc, const Shot& shot) {
    if (!world_ready_ || !world_config_)
      return false;
    auto message = encode_npc_shot(npc, shot);
    if (message.empty())
      return false;
    conn_.queue(0, std::move(message));
    return true;
  }
  // Other players' and NPCs' shots since the last call, oldest first: (shooter's network id,
  // shot).
  std::vector<std::pair<std::uint32_t, Shot>> take_shot_relays() {
    return std::exchange(shot_relays_, {});
  }
  // Asks the server whether the local player's game takes up one of the level's items
  // (WORLD_PICKUP). False when nothing was sent.
  // Where one of the local player's projectiles landed.
  bool send_shot_landed(const ShotLanded& landed) {
    if (!world_ready_ || !world_config_)
      return false;
    auto message = encode_shot_landed(landed);
    if (message.empty())
      return false;
    conn_.queue(0, std::move(message));
    return true;
  }
  bool send_world_pickup(const std::string& key) {
    if (!world_ready_ || !world_config_)
      return false;
    auto message = encode_world_pickup(key);
    if (message.empty())
      return false;
    conn_.queue(0, std::move(message));
    return true;
  }
  // The server's verdicts on the level's items the local player would take up since the last
  // call, oldest first: (key, granted).
  std::vector<std::pair<std::string, bool>> take_world_pickup_verdicts() {
    return std::exchange(world_pickup_verdicts_, {});
  }
  // The server's notices of units the local player gained or lost since the last call.
  std::vector<ItemNotice> take_item_notices() {
    return std::exchange(item_notices_, {});
  }
  // The pickpocketing rules the server sent since the last call (PICKPOCKET_RULE).
  std::vector<PickpocketRule> take_pickpocket_rules() {
    return std::exchange(pickpocket_rules_, {});
  }
  // The server's orders to clean or soil the local player (GRIME_ORDER).
  std::vector<GrimeOrder> take_grime_orders() {
    return std::exchange(grime_orders_, {});
  }
  // The marker shows and hides the server sent since the last call, in order (MARKER_SHOW and
  // MARKER_HIDE).
  std::vector<MarkerOrder> take_marker_orders() {
    return std::exchange(marker_orders_, {});
  }
  // The blip shows and hides the server sent since the last call, in order (BLIP_SHOW and
  // BLIP_HIDE).
  std::vector<BlipOrder> take_blip_orders() {
    return std::exchange(blip_orders_, {});
  }
  // The quest shows, hides and track orders the server sent since the last call, in order
  // (QUEST_SHOW, QUEST_HIDE and QUEST_TRACK).
  std::vector<QuestOrder> take_quest_orders() {
    return std::exchange(quest_orders_, {});
  }
  // Tells the server the local player started or stopped following a quest (QUEST_TRACKING).
  // False when nothing was sent.
  bool send_quest_tracking(const QuestTracking& tracking) {
    if (!world_ready_ || !world_config_) {
      return false;
    }
    auto message = encode_quest_tracking(tracking);
    if (message.empty()) {
      return false;
    }
    conn_.queue(0, std::move(message));
    return true;
  }
  // The newest world time the server sent, once (WORLD_TIME); a later one replaces an
  // older one that was not taken yet.
  std::optional<WorldTime> take_world_time() {
    return std::exchange(world_time_, std::nullopt);
  }
  // The gate states the server sent since the last call (GATE), oldest first.
  std::vector<GateState> take_gate_states() {
    return std::exchange(gate_states_, {});
  }
  // The newest world weather the server sent, once (WORLD_WEATHER); a later one
  // replaces an older one that was not taken yet.
  std::optional<WorldWeather> take_world_weather() {
    return std::exchange(world_weather_, std::nullopt);
  }
  // The door states the server set since the last call, oldest first (DOOR_SET).
  std::vector<DoorState> take_door_sets() {
    return std::exchange(door_sets_, {});
  }
  // The smart objects scripts placed or took down since the last call, oldest first
  // (SMART_OBJECT).
  // The level's graves (graves.hpp): a grave the client's level has (kReport) or the local
  // player dug out (kDug), to the server.
  bool send_grave(const Grave& grave) {
    if (!world_ready_ || !world_config_ ||
        (grave.kind != GraveKind::Report && grave.kind != GraveKind::Dug &&
         grave.kind != GraveKind::Digging))
      return false;
    auto message = encode_grave(grave);
    if (message.empty())
      return false;
    conn_.queue(0, std::move(message));
    return true;
  }
  // The graves' states the server set since the last call, oldest first.
  std::vector<Grave> take_grave_sets() {
    return std::exchange(grave_sets_, {});
  }
  std::vector<SmartObjectOrder> take_smart_objects() {
    return std::exchange(smart_objects_, {});
  }
  // The doors scripts hung or took down since the last call, oldest first (DOOR_SPAWN).
  std::vector<DoorSpawn> take_door_spawns() {
    return std::exchange(door_spawns_, {});
  }
  // The nearest-door questions the server asked since the last call (DOOR_NEAREST).
  std::vector<std::uint32_t> take_door_nearest_requests() {
    return std::exchange(door_nearest_requests_, {});
  }
  // The doors near the local player and their state (DOOR_REPORT). False when nothing
  // was sent.
  bool send_door_report(const std::vector<DoorEntry>& doors) {
    return send_control(encode_door_report(doors));
  }
  // The local player's own use of a door, before the game applies it (DOOR_USE).
  bool send_door_use(const DoorUse& use) {
    return send_control(encode_door_use(use));
  }
  // The answer to a nearest-door question (DOOR_NEAREST_REPLY).
  bool send_door_nearest_reply(const DoorNearestReply& reply) {
    return send_control(encode_door_nearest_reply(reply));
  }
  // The props and effects the server placed since the last call, oldest first (PROP and VFX).
  std::vector<PropOrder> take_prop_orders() {
    return std::exchange(prop_orders_, {});
  }
  std::vector<VfxOrder> take_vfx_orders() {
    return std::exchange(vfx_orders_, {});
  }
  // Prop and effect orders dropped because kMaxPlacedOrders were already waiting, since the start.
  std::uint64_t dropped_placed_orders() const {
    return dropped_placed_orders_;
  }
  // The point lights the server placed, changed or removed since the last call, oldest first
  // (LIGHT).
  std::vector<LightOrder> take_light_orders() {
    return std::exchange(light_orders_, {});
  }
  // The recipe table patches the server sent since the last call, oldest first (RECIPE_TABLE).
  std::vector<RecipeTable> take_recipe_tables() {
    return std::exchange(recipe_tables_, {});
  }
  // And the recipe book's pictures of those recipes (RECIPE_PICTURE).
  std::vector<RecipePicture> take_recipe_pictures() {
    return std::exchange(recipe_pictures_, {});
  }
  // The server's cart orders since the last call, oldest first (CART).
  std::vector<CartOrder> take_cart_orders() {
    return std::exchange(cart_orders_, {});
  }
  // Tells the server where a rigid prop this game simulates is (PROP_REPORT).
  bool send_prop_report(const PropReport& report) {
    if (!world_ready_) {
      return false;
    }
    auto message = encode_prop_report(report);
    if (message.empty()) {
      return false;
    }
    conn_.queue(0, std::move(message));
    return true;
  }
  // Tells the server what the local player did with a cart (CART_REPORT).
  bool send_cart_report(const CartReport& report) {
    if (!world_ready_) {
      return false;
    }
    auto message = encode_cart_report(report);
    if (message.empty()) {
      return false;
    }
    conn_.queue(0, std::move(message));
    return true;
  }
  // The level edits applied and removed since the last call, oldest first (LEVEL_EDIT_APPLY and
  // LEVEL_EDIT_REMOVE).
  std::vector<LevelEditChange> take_level_edit_changes() {
    return std::exchange(level_edit_changes_, {});
  }
  // The local player's prop previews and placement since the last call, oldest first (GHOST and
  // PLACER), and the gizmos the server drew or cleared (GIZMO).
  std::vector<GhostOrder> take_ghost_orders() {
    return std::exchange(ghost_orders_, {});
  }
  std::vector<PlacerOrder> take_placer_orders() {
    return std::exchange(placer_orders_, {});
  }
  std::vector<GizmoOrder> take_gizmo_orders() {
    return std::exchange(gizmo_orders_, {});
  }
  // What victims carry, for this client's loot screen (PICKPOCKET_POCKETS).
  std::vector<PickpocketPockets> take_pickpocket_pockets() {
    return std::exchange(pickpocket_pockets_, {});
  }
  // The carries of players' bodies the server sent since the last call (BODY_CARRY).
  std::vector<BodyCarry> take_body_carries() {
    return std::exchange(body_carries_, {});
  }
  // The local player picked up or let go of a player's body (BODY_CARRY_REPORT).
  bool send_body_carry_report(const BodyCarryReport& report) {
    if (!world_ready_ || !world_config_)
      return false;
    auto message = encode_body_carry_report(report);
    if (message.empty())
      return false;
    conn_.queue(0, std::move(message));
    return true;
  }
  // The server's orders to play or stop the local player's animation since the last call, oldest
  // first (ANIMATION_ORDER). A burst keeps the newest kMaxAnimationOrders.
  std::vector<AnimationOrder> take_animation_orders() {
    return std::exchange(animation_orders_, {});
  }
  // How a request of the server's ended in the local player's game, once per request
  // (ANIMATION_END).
  // A prop of the running request went into the hand or was put down.
  bool send_animation_prop_cue(const AnimationPropCue& cue) {
    if (!world_ready_ || !world_config_) {
      return false;
    }
    auto message = encode_animation_prop_cue(cue);
    if (message.empty()) {
      return false;
    }
    conn_.queue(0, std::move(message));
    return true;
  }
  bool send_animation_end(const AnimationEnd& end) {
    if (!world_ready_ || !world_config_) {
      return false;
    }
    auto message = encode_animation_end(end);
    if (message.empty()) {
      return false;
    }
    conn_.queue(0, std::move(message));
    return true;
  }
  // What the local player's swing met that is not a body, once per thing per attack (WEAPON_HIT).
  bool send_weapon_hit(const WorldRayHit& hit) {
    if (!world_ready_ || !world_config_) {
      return false;
    }
    auto message = encode_weapon_hit(hit);
    if (message.empty()) {
      return false;
    }
    conn_.queue(0, std::move(message));
    return true;
  }
  // Whether the server lets the local player fly the free camera, when it said so since the last
  // call (FREECAM); empty otherwise.
  std::optional<bool> take_freecam() {
    return std::exchange(freecam_, std::nullopt);
  }
  // Whether the server lets the local player's sleep and food needs run, when it said so since
  // the last call (NEEDS); empty otherwise.
  std::optional<bool> take_needs() {
    return std::exchange(needs_, std::nullopt);
  }
  // A request to rob a player, or how a theft ended (PICKPOCKET).
  bool send_pickpocket_report(const PickpocketReport& report) {
    if (!world_ready_ || !world_config_)
      return false;
    auto message = encode_pickpocket_report(report);
    if (message.empty())
      return false;
    conn_.queue(0, std::move(message));
    return true;
  }
  // The lists of the level's items taken up since the last call, oldest first.
  std::vector<WorldTaken> take_world_taken() {
    return std::exchange(world_taken_, {});
  }
  // Whether the server turned the level's items on or off since the last call
  // (WORLD_ITEMS_ENABLED).
  // The latest list of places the level's items are taken from (WORLD_ITEMS_REMOVED), if one came.
  std::optional<std::vector<RemovedItemArea>> take_world_items_removed() {
    return std::exchange(world_items_removed_, std::nullopt);
  }
  std::optional<bool> take_world_items_enabled() {
    return std::exchange(world_items_enabled_, std::nullopt);
  }
  // Tells the server where the item the local player's carry let go of lies (CARRY_REPORT).
  // False when nothing was sent.
  bool send_carry_put_down(const CarryPutDown& put_down) {
    if (!world_ready_ || !world_config_)
      return false;
    auto message = encode_carry_put_down(put_down);
    if (message.empty())
      return false;
    conn_.queue(0, std::move(message));
    return true;
  }
  // Tells the server what the local player's game does at a crafting station, or asks whether it
  // may open one (CRAFT_REPORT). False when nothing was sent.
  // What lies on the alchemy table the local player brews at (CRAFT_TABLE). False
  // when not in a ready world or the table cannot travel.
  bool send_craft_table(const CraftTable& table) {
    if (!world_ready_) {
      return false;
    }
    auto message = encode_craft_table(table);
    if (message.empty()) {
      return false;
    }
    conn_.queue(0, std::move(message));
    return true;
  }
  bool send_craft_report(const CraftReport& report) {
    if (!world_ready_ || !world_config_)
      return false;
    auto message = encode_craft_report(report);
    if (message.empty())
      return false;
    conn_.queue(0, std::move(message));
    return true;
  }
  // Tells the server what the local player did in their dice match (DICE_MOVE). False
  // when nothing was sent.
  // Tells the server where the dice table's things are in the local player's game (DICE_VIEW).
  // False when nothing was sent.
  bool send_dice_view(const DiceView& view) {
    if (!world_ready_ || !world_config_)
      return false;
    auto message = encode_dice_view(view);
    if (message.empty())
      return false;
    conn_.queue(0, std::move(message));
    return true;
  }
  bool send_dice_move(const DiceMove& move) {
    if (!world_ready_ || !world_config_)
      return false;
    auto message = encode_dice_move(move);
    if (message.empty())
      return false;
    conn_.queue(0, std::move(message));
    return true;
  }
  // Tells the server a herb the local player's game picked (GATHER_REPORT). False
  // when nothing was sent.
  bool send_gather_report(const GatherReport& report) {
    if (!world_ready_ || !world_config_)
      return false;
    auto message = encode_gather_report(report);
    if (message.empty())
      return false;
    conn_.queue(0, std::move(message));
    return true;
  }
  // Tells the server what the local player's game shows of its inventory, or what it held
  // (INVENTORY_REPORT). False when nothing was sent.
  bool send_inventory_report(const InventoryReport& report) {
    if (!world_ready_ || !world_config_)
      return false;
    auto message = encode_inventory_report(report);
    if (message.empty())
      return false;
    conn_.queue(0, std::move(message));
    return true;
  }
  // Tells the server the local player opened a container's lock in their game (STASH_UNLOCK).
  // Sent before the open's INVENTORY_REPORT on the same channel. False when nothing
  // was sent.
  bool send_stash_unlock(const StashUnlock& unlock) {
    if (!world_ready_ || !world_config_) {
      return false;
    }
    auto message = encode_stash_unlock(unlock);
    if (message.empty()) {
      return false;
    }
    conn_.queue(0, std::move(message));
    return true;
  }
  // The SET_STAT requests for the local player received since the last call, oldest first
  // (Player.setStat). Legacy only: empty after bind_player_engine_commands.
  std::vector<StatRequest> take_stat_requests() {
    if (!world_ready_) {
      return {};
    }
    return player_engine_commands_.take_legacy_stats();
  }
  // Bind the scalar native-body identity before applying ordered commands. First bind adopts
  // staged current-world receipts in place; a replacement incarnation discards the old stream.
  // Use a strictly increasing incarnation on body/session/world replacement, never on KO,
  // revival or FORCE_STATE. Caller holds its normal peer/network lock; no native calls here.
  bool bind_player_engine_commands(const PlayerEngineScope& scope) {
    if (!world_config_ || world_config_->revision != scope.revision || !owns(scope.owner) ||
        player_engine_commands_closed_) {
      return false;
    }
    return player_engine_commands_.bind(scope);
  }
  // Recheck immediately before each native command mutation, under the caller's peer lock.
  // Does not pop a command or consume the session-end reason. SESSION_END remains revoked
  // even while its old world configuration, readiness and ownership are still visible.
  bool current_player_engine_commands(const PlayerEngineScope& scope) const {
    return world_config_ && world_ready_ && world_config_->revision == scope.revision &&
           owns(scope.owner) && !player_engine_commands_closed_ &&
           player_engine_commands_.current(scope);
  }
  // One command per call, so the engine can recheck its actual native KO state after each
  // mutation and hold a later command until native wake is confirmed. Always inspect status
  // and log changes in overflows, including after reset; overflow poisons the current stream.
  PlayerEngineCommandPoll take_player_engine_command(const PlayerEngineScope& scope) {
    if (!world_config_ || !world_ready_ || world_config_->revision != scope.revision ||
        !owns(scope.owner) || player_engine_commands_closed_) {
      return player_engine_commands_.status(PlayerEngineCommandStatus::ScopeMismatch);
    }
    return player_engine_commands_.take(scope);
  }
  // Engine world/body/session cleanup. Does not touch native objects. A fresh incarnation must
  // be bound before more ordered receipts can apply; callers must not reuse a revoked token.
  void invalidate_player_engine_commands() {
    player_engine_commands_.invalidate_body();
  }
  // Non-consuming diagnostics remain available while the native consumer waits for wake.
  // Log every change in overflows; a scope reset clears poison but retains lifetime counters.
  PlayerEngineCommandDiagnostics player_engine_command_diagnostics() const {
    return player_engine_commands_.diagnostics();
  }
  // The pace rule the server put on the local player (MOVEMENT_MODE, Player.setPace;
  // none until a resource sets one).
  MovementMode movement_mode() const {
    return movement_mode_;
  }
  // Whether the resources want the local player's body drawn (BODY_VISIBLE; drawn until a
  // resource asks otherwise).
  bool body_visible() const {
    return body_visible_;
  }
  // The mesh the resources want the local player's body drawn as (DISGUISE; "" for its own body
  // until a resource asks otherwise).
  const std::string& disguise() const {
    return disguise_;
  }
  // The items the resources want the local player to wear outside their inventory (WORN, issue),
  // and how many lists came so far (a new list, even the same, counts).
  const std::vector<ItemClassId>& worn() const {
    return worn_;
  }
  std::uint64_t worn_revision() const {
    return worn_revision_;
  }
  // REVIVE requests for the local player received since the last call. Legacy only: empty
  // after bind_player_engine_commands. Migrate both legacy consumers together.
  std::size_t take_revives() {
    if (!world_ready_) {
      return 0;
    }
    return player_engine_commands_.take_legacy_revives();
  }
  bool owns(core::EntityId e) const {
    return owned_.count(e) != 0;
  }

  // Queue the owned entity's AUTH_OWNER field updates upstream (channel 3, unreliable). Sent on the
  // next tick()/flush. No-op if the entity's owning type is unknown to this client.
  void send_input(core::EntityId e,
                  const std::vector<std::pair<core::FieldId, replication::FieldValue>>& updates);

  bool send_native_effects(core::EntityId source, core::EntityId target,
                           const NativeMeleeData& cause, const NativeEffects& effects,
                           bool executor_is_target = false);
  bool send_native_contact(core::EntityId source, core::EntityId target,
                           const NativeContactData& data, std::uint64_t attack_generation = 0);
  bool send_attack_generation(core::EntityId source, core::EntityId recipient,
                              std::uint64_t generation, std::uint8_t native_kind);
  bool send_melee(core::EntityId source, core::EntityId target, const NativeMeleeData& data,
                  bool executor_is_target = false, bool authority_is_target = false);
  bool send_combat_action(core::EntityId source, core::EntityId target,
                          const CombatActionData& data);
  bool send_sync_hit(core::EntityId source, core::EntityId target, const NativeSyncHitData& data);
  bool send_reaction(core::EntityId source, core::EntityId target, const NativeReactionData& data,
                     bool authority_is_target = false);
  // Revalidate queued or held combat before native application. Call under the same external
  // synchronization as tick(): ownership, scope or epochs can change after initial receipt.
  // Epochs are available only for live mirrored entities in the currently ready world.
  std::optional<std::uint8_t> combat_epoch(core::EntityId entity) const;
  bool current_combat_context(const MeleeEvent& event) const;
  std::vector<MeleeEvent> take_melee() {
    auto result = std::move(melee_inbox_);
    melee_inbox_.clear();
    result.insert(result.end(), observer_contacts_.begin(), observer_contacts_.end());
    observer_contacts_.clear();
    std::erase_if(result, [this](const MeleeEvent& event) {
      const bool current = current_combat_context(event);
      return !current;
    });
    return result;
  }

  // A server-authored native teleport (§7 FORCE_XFORM) that passed the authorisation fence, waiting
  // to be written into the engine's CEntity on the engine/render tick. The Windows bridge drains
  // this and calls the validated CEntity::SetWorldTM path; it is intentionally NOT applied on the
  // network thread. `epoch` and `revision` let the consumer re-validate after it drains under the
  // network mutex and before it touches the engine unlocked.
  struct ForcedTransform {
    core::EntityId entity = 0;
    core::Vec3 position;
    // FORCE_XFORM carries a position, FORCE_ROT a rotation; each command sets exactly one.
    bool has_position = true;
    bool has_rotation = false;
    core::Quat rotation;
    std::uint8_t epoch = 0;      // forced-state epoch this teleport was authorised at
    std::uint32_t revision = 0;  // world-config revision in effect when accepted (0 = unconfigured)
  };
  // Drain the queue, validating each command AT DRAIN (not merely at receipt): the consumer drains
  // under the network mutex, then calls the engine unlocked, so a command that went stale while
  // queued must be filtered here or it would mutate a since-changed entity. A command survives only
  // if its entity is still known (not despawned), its epoch is still the current adopted one (not
  // superseded by a newer forced state), and its world revision still matches (not reset by a
  // reconnect / map change). The queue is always cleared, matching take_melee().
  //
  // Draining also adopts each surviving command's epoch for an owned entity. The caller must apply
  // the returned transforms before it captures and sends that entity's next input, so the new epoch
  // never carries a pose from before the teleport. Until the drain, owner input keeps the old epoch
  // and the server rejects it.
  //
  // Every force bumps the entity's epoch, so a position and a rotation forced back to back arrive
  // with consecutive epochs and only the newer one would still match. Commands are therefore merged
  // per entity first: the newest position and the newest rotation travel together under that
  // entity's newest epoch, and survive only if no other forced state has superseded it since. A
  // command from an older world revision is discarded by a newer one instead of being merged.
  std::vector<ForcedTransform> take_forced_transforms() {
    std::vector<ForcedTransform> merged;
    for (const ForcedTransform& t : forced_transforms_) {
      auto same = std::find_if(merged.begin(), merged.end(),
                               [&](const ForcedTransform& m) { return m.entity == t.entity; });
      if (same == merged.end() || same->revision != t.revision) {
        if (same != merged.end())
          merged.erase(same);
        merged.push_back(t);
        continue;
      }
      if (t.has_position) {
        same->position = t.position;
        same->has_position = true;
      }
      if (t.has_rotation) {
        same->rotation = t.rotation;
        same->has_rotation = true;
      }
      same->epoch = t.epoch;
    }
    std::vector<ForcedTransform> result;
    result.reserve(merged.size());
    for (const ForcedTransform& t : merged) {
      const bool entity_live = mirror_.get(t.entity) != nullptr || owned_.count(t.entity) != 0;
      const auto adopted = combat_epochs_.find(t.entity);
      const bool epoch_current = adopted != combat_epochs_.end() && adopted->second == t.epoch;
      const bool world_current = !world_config_ || world_config_->revision == t.revision;
      if (entity_live && epoch_current && world_current) {
        result.push_back(t);
        auto owned = owned_.find(t.entity);
        const bool adopts_not_older_epoch =
            owned != owned_.end() && static_cast<std::int8_t>(t.epoch - owned->second.epoch) >= 0;
        if (adopts_not_older_epoch) {
          owned->second.epoch = t.epoch;
        }
      }
    }
    forced_transforms_.clear();
    return result;
  }

 private:
  // Queues an encoded control message once the world is ready; false for an empty message.
  bool send_control(std::vector<std::uint8_t> message) {
    if (!world_ready_ || !world_config_ || message.empty())
      return false;
    conn_.queue(0, std::move(message));
    return true;
  }
  // Queues an encoded siege message once the world is ready; false for an empty one.
  bool send_siege(std::vector<std::uint8_t> message) {
    return send_control(std::move(message));
  }

  ScriptReceiver script_receiver_;
  // Only current mirrored bodies are retained. Valid reports suppressed by this gate are
  // refreshed periodically; attack/animation and simulation-epoch changes bypass the timer.
  std::unordered_map<core::EntityId, BodyStatePublication> body_publications_;
  std::unordered_map<core::EntityId, PlayerInputPublication> player_input_publications_;
  double player_input_clock_ = 0;
  double body_state_clock_ = 0;
  std::uint32_t script_sequence_ = 0;
  std::vector<sdk::Value> script_inbox_;
  std::vector<ChatMessage> chat_inbox_;
  static constexpr std::uint8_t kChannelReplAck_ = 5;

  struct Owned {
    core::TypeId type = 0;
    std::uint8_t epoch = 0;  // adopted from SET_OWNER / FORCE_STATE, echoed in upstream input
  };

  // Upper bound on pending forced teleports between engine drains, so a misbehaving or hostile
  // server cannot grow the queue without limit; the oldest is dropped on overflow (latest-wins).
  static constexpr std::size_t kMaxForcedTransforms_ = 256;

  std::unordered_map<core::EntityId, std::uint8_t> combat_epochs_;
  std::uint64_t next_melee_sequence_ = 1;
  std::vector<MeleeEvent> melee_inbox_;
  std::vector<MeleeEvent> observer_contacts_;
  std::vector<ForcedTransform> forced_transforms_;
  PlayerEngineCommandQueue player_engine_commands_;
  bool player_engine_commands_closed_ = false;
  std::vector<AppearanceRequest> appearance_requests_;
  std::optional<Progression> progression_sent_;
  std::vector<ProgressionOrder> progression_orders_;
  std::optional<std::vector<BuffEntry>> buffs_sent_;
  std::vector<BuffOrder> buff_orders_;
  std::vector<CustomBuff> custom_buffs_;
  std::optional<InventoryShow> inventory_show_;
  std::vector<InventoryConsume> inventory_consumes_;
  std::vector<CraftVerdict> craft_verdicts_;
  std::vector<DiceState> dice_states_;
  std::vector<DiceWatch> dice_watches_;
  std::optional<DiceTables> dice_tables_;
  std::vector<GatherVerdict> gather_verdicts_;
  std::vector<GatherPlants> gather_plants_;
  std::vector<PotLooks> pot_looks_;
  std::vector<StashShow> stash_shows_;
  std::vector<std::uint32_t> combat_cancels_;
  std::vector<CarryOrder> carry_orders_;
  std::vector<HitVerdict> hit_verdicts_;
  bool hit_rules_ = false;
  std::vector<std::pair<std::string, bool>> world_pickup_verdicts_;
  std::vector<WorldTaken> world_taken_;
  std::vector<ItemNotice> item_notices_;
  std::vector<PickpocketRule> pickpocket_rules_;
  std::optional<bool> freecam_;
  std::optional<bool> needs_;
  std::vector<BodyCarry> body_carries_;
  std::vector<AnimationOrder> animation_orders_;
  std::vector<GrimeOrder> grime_orders_;
  std::optional<WorldTime> world_time_;
  std::vector<GateState> gate_states_;
  std::optional<WorldWeather> world_weather_;
  std::vector<MarkerOrder> marker_orders_;
  std::vector<BlipOrder> blip_orders_;
  std::vector<QuestOrder> quest_orders_;
  std::vector<DoorState> door_sets_;
  std::vector<SmartObjectOrder> smart_objects_;
  std::vector<Grave> grave_sets_;
  std::vector<DoorSpawn> door_spawns_;
  std::vector<std::uint32_t> door_nearest_requests_;
  std::vector<PropOrder> prop_orders_;
  std::vector<VfxOrder> vfx_orders_;
  std::uint64_t dropped_placed_orders_ = 0;
  std::vector<LightOrder> light_orders_;
  std::vector<RecipeTable> recipe_tables_;
  std::vector<RecipePicture> recipe_pictures_;
  RecipePictureAssembler recipe_picture_parts_;
  std::vector<CartOrder> cart_orders_;
  std::vector<LevelEditChange> level_edit_changes_;
  std::vector<GhostOrder> ghost_orders_;
  std::vector<PlacerOrder> placer_orders_;
  std::vector<GizmoOrder> gizmo_orders_;
  std::vector<PickpocketPockets> pickpocket_pockets_;
  std::vector<std::pair<std::uint32_t, Shot>> shot_relays_;
  std::vector<ItemTypes> item_types_;
  std::optional<ItemOverrides> item_overrides_;
  std::vector<ItemLimitNotice> item_limit_notices_;
  std::vector<ItemNames> item_names_;
  MovementMode movement_mode_;
  bool body_visible_ = true;
  std::string disguise_;
  std::vector<ItemClassId> worn_;
  std::uint64_t worn_revision_ = 0;
  // 40 % until the server sets one: a black screen read as an endless loading.
  std::uint8_t knockout_vision_ = kKnockoutVisionDefault;
  std::optional<bool> combat_marker_;
  std::optional<SessionEnd> session_end_;
  std::optional<bool> restricted_area_marker_;
  std::optional<std::uint8_t> third_person_;
  std::vector<WorldRayRequest> world_rays_;
  std::vector<std::optional<CameraShot>> camera_orders_;
  std::optional<bool> cuffs_;
  std::optional<bool> frozen_;
  std::vector<PlayerAttach> player_attaches_;
  std::optional<bool> noclip_allowed_;
  std::optional<bool> world_items_enabled_;
  std::optional<std::vector<RemovedItemArea>> world_items_removed_;
  std::vector<MountOrder> mount_orders_;
  bool dismount_ordered_ = false;
  bool rear_ordered_ = false;
  std::vector<PullDownOrder> pull_down_orders_;
  std::optional<std::uint16_t> interaction_rules_;
  std::vector<std::uint32_t> whistles_;
  std::optional<VoiceState> voice_state_;
  std::optional<std::string> server_name_;
  std::vector<std::pair<std::uint32_t, std::uint8_t>> voice_muffles_;
  std::vector<DialogueOrder> dialogue_orders_;
  static constexpr std::size_t kMaxDialogueOrdersWaiting = 32;
  std::vector<VendorOrder> vendor_orders_;
  static constexpr std::size_t kMaxVendorOrdersWaiting = 32;
  std::optional<std::vector<Interaction>> interactions_;
  std::vector<SiegeEngineOrder> siege_orders_;
  std::vector<SiegeShot> siege_shots_;
  std::vector<SiegeLadderOrder> siege_ladder_orders_;
  std::vector<StonePileOrder> stone_pile_orders_;
  std::vector<NpcBlow> npc_blows_;
  static constexpr std::size_t kMaxNpcBlowsWaiting = 32;
  std::vector<PilloryOrder> pillory_orders_;
  std::unordered_map<core::EntityId, std::pair<std::uint8_t, std::uint64_t>> melee_received_;
  std::optional<WorldConfig> world_config_;
  bool world_ready_ = false;
  bool gameplay_started_ = false;
  const replication::SchemaRegistry& schema_;
  replication::Mirror mirror_;
  std::optional<core::Tick> last_replication_ack_;
  double replication_ack_at_ = -1.0;
  static constexpr double kReplicationAckRepeatSeconds = 0.1;
  transport::Connection conn_;
  std::unordered_map<core::EntityId, Owned> owned_;
};

}  // namespace kcd2s::integration

#endif  // KCD2S_INTEGRATION_CLIENT_PEER_HPP
