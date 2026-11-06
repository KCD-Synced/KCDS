// Synced NPCs: the NetworkNpc schema, NPC_STATE and the simulator election.
#include "kcd2s/integration/npc_barks.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/npc.hpp"
#include "../local/movement.hpp"
#include "kcd2s/testkit.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
using namespace kcd2s;
using namespace kcd2s::integration;

namespace {

// A server with three players' clients over lossless links and one NPC at the origin. Each
// client also has a raw connection on its link that sends hand-made NPC_STATEs, which its
// ClientPeer would refuse to send.
struct NpcWorld {
  replication::SchemaRegistry schema = local::player_schema();
  Server server{schema};
  struct Peer {
    std::pair<std::unique_ptr<transport::SimLink>, std::unique_ptr<transport::SimLink>> links;
    std::unique_ptr<ClientPeer> client;
    std::unique_ptr<transport::Connection> raw;
    core::EntityId player{};
    core::PeerId id{};
  };
  std::array<Peer, 3> peers;
  core::EntityId npc{};
  double now{};

  explicit NpcWorld(bool native_ready = true) {
    for (auto& p : peers) {
      p.links = transport::make_sim_pair({}, {});
      p.player = local::spawn_player(server.world());
      p.id = server.add_client(p.links.first.get(), p.player, 1000);
      server.set_owner(p.player, p.id);
      server.configure_world(p.id, {});
      p.client = std::make_unique<ClientPeer>(schema, p.links.second.get());
      p.raw = std::make_unique<transport::Connection>(p.links.second.get());
      p.raw->set_channel_tier(kNpcMotionChannel, transport::DeliveryTier::Unreliable);
      // The raw connection numbers its packets on the same link as the client's own. Moved far
      // ahead (and its packets dropped before the server reads them), its numbers stay apart
      // from the client's, so the server takes the packets of both as new.
      for (int i = 0; i < 60000; ++i) {
        // A second apart: a quiet connection only sends its keep-alive this often.
        p.raw->flush(double(i));
      }
      p.links.first->poll();
      transport::Datagram dropped;
      while (p.links.first->recv(dropped)) {
      }
    }
    place(0, 400);
    place(1, 400);
    place(2, 400);
    NpcSpawn spawn;
    spawn.name = "Hans";
    spawn.soul = "4A2B6C8D-1E3F-4a5b-8c7d-9e0f1a2b3c4d";
    npc = server.spawn_npc(spawn);
    pump(30);
    for (auto& p : peers) {
      p.client->mark_world_ready(1);
      if (native_ready) {
        p.client->mark_gameplay_started(1);
      }
    }
    pump(30);
  }

  void pump(int ticks) {
    for (int i = 0; i < ticks; ++i) {
      now += 1. / 60;
      server.world().set_tick(server.world().tick() + 1);
      for (auto& p : peers) {
        p.links.first->poll();
        p.links.second->poll();
      }
      server.tick(now);
      for (auto& p : peers) {
        p.client->tick(now);
      }
    }
  }

  // Puts the player `x` metres from the NPC along the x axis.
  void place(unsigned peer, float x) {
    server.world().write(peers[peer].player, local::Position, core::Vec3{x, 0, 0});
  }

  std::int64_t value(core::FieldId field) const {
    return std::get<std::int64_t>(*server.world().read(npc, field));
  }

  core::Vec3 position() const {
    return std::get<core::Vec3>(*server.world().read(npc, NpcPosition));
  }

  std::string native_state() const {
    return std::get<std::string>(*server.world().read(npc, NpcNativeState));
  }

  std::string text(core::FieldId field) const {
    return std::get<std::string>(*server.world().read(npc, field));
  }

  bool flag(core::FieldId field) const {
    return std::get<bool>(*server.world().read(npc, field));
  }

  core::EntityId simulator() const {
    return core::EntityId(value(NpcSimulator));
  }

  // A state the simulator `peer` could send now, at `x` on the x axis.
  NpcState state(unsigned peer, std::uint64_t sequence, float x) const {
    NpcState s;
    s.revision = 1;
    s.epoch = std::uint32_t(value(NpcEpoch));
    s.source = peers[peer].player;
    s.npc = npc;
    s.sequence = sequence;
    s.position = {x, 0, 0};
    s.velocity = {1, 0, 0};
    s.animations = local::encode_animation({});
    return s;
  }

  // Sends through the client's own ClientPeer, which fills in revision and epoch.
  bool send(unsigned peer, const NpcState& s) {
    const bool sent = peers[peer].client->send_npc_state(s);
    pump(4);
    return sent;
  }

  // Sends the bytes as they are, past the ClientPeer's own checks.
  void send_raw(unsigned peer, const std::vector<std::uint8_t>& bytes,
                std::uint8_t channel = kNpcMotionChannel) {
    peers[peer].raw->queue(channel, bytes);
    peers[peer].raw->flush(now);
    pump(4);
  }

  std::string outfit() const {
    return std::get<std::string>(*server.world().read(npc, NpcOutfit));
  }
};

core::Quat facing() {
  return core::Quat{0, 0, 0, 1};
}

}  // namespace

KCD2S_TEST(npc, state_round_trips_with_the_horse_layout_under_its_own_magic) {
  NpcState s;
  s.revision = 3;
  s.epoch = 4;
  s.source = 5;
  s.npc = 6;
  s.sequence = 7;
  s.position = {1, 2, 3};
  s.velocity = {0.5f, 0, 0};
  s.rotation = {0, 0, 0.6f, 0.8f};
  s.motion_rotation = {0, 0, 0, 1};
  s.motion_type = 2;
  s.motion_flags = 1;
  s.motion_valid = true;
  s.mask = 0x101;
  s.params[17] = 9;
  s.animations = local::encode_animation({});
  const auto bytes = encode_npc_state(s);
  // 'KNS1', then the horse state's fields: 165 bytes and the animations, then the hands text and
  // its length (two bytes).
  KCD2S_CHECK(bytes.size() == 165 + s.animations.size() + 2);
  KCD2S_CHECK(bytes[0] == 0x4b && bytes[1] == 0x4e && bytes[2] == 0x53 && bytes[3] == 0x31);
  NpcState decoded;
  KCD2S_CHECK(decode_npc_state(bytes, decoded));
  KCD2S_CHECK(decoded.revision == 3 && decoded.epoch == 4 && decoded.source == 5);
  KCD2S_CHECK(decoded.npc == 6 && decoded.sequence == 7);
  KCD2S_CHECK(decoded.position == s.position && decoded.velocity == s.velocity);
  KCD2S_CHECK(decoded.rotation == s.rotation && decoded.motion_rotation == s.motion_rotation);
  KCD2S_CHECK(decoded.motion_type == 2 && decoded.motion_flags == 1 && decoded.motion_valid);
  KCD2S_CHECK(decoded.mask == 0x101 && decoded.params[17] == 9);
  KCD2S_CHECK(decoded.animations == s.animations);

  // The same fields as a horse's state, told apart by the magic only.
  HorseState horse;
  KCD2S_CHECK(!decode_horse(bytes, horse));
  static_cast<BodyState&>(horse) = s;
  horse.horse = s.npc;
  const auto horse_bytes = encode_horse(horse);
  KCD2S_CHECK(!decode_npc_state(horse_bytes, decoded));
  KCD2S_CHECK(std::equal(bytes.begin() + 4, bytes.end() - 2, horse_bytes.begin() + 4));

  // What the hands hold travels at the end (issue 016).
  NpcState holding = s;
  holding.hands = "R I0123456789ABCDEF0123456789ABCDEF JRightHandSlot";
  NpcState read_back;
  KCD2S_CHECK(decode_npc_state(encode_npc_state(holding), read_back));
  KCD2S_CHECK(read_back.hands == holding.hands && read_back.npc == 6);
  holding.hands = std::string(kNpcHandsMax + 1, 'x');
  KCD2S_CHECK(encode_npc_state(holding).empty());
  holding.hands = "bad	text";
  KCD2S_CHECK(!valid_npc(holding));

  // Truncated, padded and invalid values are refused.
  auto cut = bytes;
  cut.pop_back();
  KCD2S_CHECK(!decode_npc_state(cut, decoded));
  auto padded = bytes;
  padded.push_back(0);
  KCD2S_CHECK(!decode_npc_state(padded, decoded));
  NpcState bad = s;
  bad.position.x = std::numeric_limits<float>::quiet_NaN();
  KCD2S_CHECK(!decode_npc_state(encode_npc_state(bad), decoded));
  bad = s;
  bad.position.y = 2e6f;
  KCD2S_CHECK(!decode_npc_state(encode_npc_state(bad), decoded));
  bad = s;
  bad.rotation = {0, 0, 0, 0};
  KCD2S_CHECK(!valid_npc(bad));
  bad = s;
  bad.sequence = 0;
  KCD2S_CHECK(!valid_npc(bad));
  bad = s;
  bad.npc = 0;
  KCD2S_CHECK(!valid_npc(bad));
  bad = s;
  bad.motion_type = 7;
  KCD2S_CHECK(!decode_npc_state(encode_npc_state(bad), decoded));
  bad = s;
  bad.animations = "not an animation";
  KCD2S_CHECK(!decode_npc_state(encode_npc_state(bad), decoded));
}

KCD2S_TEST(npc, schema_fields_and_spawn_defaults) {
  const auto schema = local::player_schema();
  const auto* type = schema.get(core::type_id("NetworkNpc"));
  KCD2S_CHECK(type != nullptr);
  if (!type) {
    return;
  }
  using replication::FieldKind;
  struct Expected {
    const char* name;
    core::FieldId id;
    FieldKind kind;
  };
  const Expected expected[] = {
      {"position", NpcPosition, FieldKind::Vec3},
      {"rotation", NpcRotation, FieldKind::Quat},
      {"soul", NpcSoul, FieldKind::Str},
      {"actorClass", NpcActorClass, FieldKind::Str},
      {"name", NpcName, FieldKind::Str},
      {"scriptId", NpcScriptId, FieldKind::I64},
      {"dimension", NpcDimension, FieldKind::I64},
      {"simulator", NpcSimulator, FieldKind::I64},
      {"simulationEpoch", NpcEpoch, FieldKind::I64},
      {"velocity", NpcVelocity, FieldKind::Vec3},
      {"nativeState", NpcNativeState, FieldKind::Str},
      {"teleport", NpcTeleport, FieldKind::I64},
      {"frozen", NpcFrozen, FieldKind::Bool},
      {"health", NpcHealth, FieldKind::F32},
      {"alive", NpcAlive, FieldKind::Bool},
      {"outfit", NpcOutfit, FieldKind::Str},
      {"animation", NpcAnimation, FieldKind::Str},
      {"speech", NpcSpeech, FieldKind::Str},
      {"appearance", NpcAppearance, FieldKind::Str},
      {"outfitFixed", NpcOutfitFixed, FieldKind::Bool},
      {"life", NpcLife, FieldKind::I64},
      {"intent", NpcIntent, FieldKind::Str},
      {"defense", NpcDefense, FieldKind::F32},
      {"damage", NpcDamage, FieldKind::F32},
      {"nameplate", NpcNameplate, FieldKind::Str},
      {"kinematic", NpcKinematic, FieldKind::Bool},
      {"ragdollPose", NpcRagdollPose, FieldKind::Str},
      {"brain", NpcBrain, FieldKind::Bool},
      {"targetable", NpcTargetable, FieldKind::Bool},
      {"carcassClosed", NpcCarcassClosed, FieldKind::Bool},
      {"attackable", NpcAttackable, FieldKind::Bool},
      {"talking", NpcTalking, FieldKind::Str},
      {"face", NpcFace, FieldKind::Str},
      {"interactable", NpcInteractable, FieldKind::Bool},
      {"levelGuid", NpcLevelGuid, FieldKind::Str},
  };
  KCD2S_CHECK(type->fields.size() == std::size(expected));
  for (const auto& field : expected) {
    const auto* found = type->find(field.name);
    KCD2S_CHECK(found != nullptr);
    if (!found) {
      continue;
    }
    KCD2S_CHECK_EQ(found->id, field.id);
    KCD2S_CHECK(found->kind == field.kind);
    KCD2S_CHECK(found->auth == replication::Authority::Server);
  }

  Server server{schema};
  NpcSpawn spawn;
  spawn.position = {10, 20, 30};
  spawn.rotation = {0, 0, 0, 1.05f};
  spawn.actor_class = "NPC_Female";
  spawn.name = "Theresa";
  spawn.dimension = 7;
  const auto npc = server.spawn_npc(spawn);
  KCD2S_CHECK(npc != core::kInvalidEntity);
  const auto read = [&](core::FieldId field) { return *server.world().read(npc, field); };
  KCD2S_CHECK(std::get<core::Vec3>(read(NpcPosition)) == spawn.position);
  // Stored at unit length.
  KCD2S_CHECK(std::abs(std::get<core::Quat>(read(NpcRotation)).w - 1) < 1e-6f);
  KCD2S_CHECK(std::get<std::string>(read(NpcSoul)).empty());
  KCD2S_CHECK(std::get<std::string>(read(NpcActorClass)) == "NPC_Female");
  KCD2S_CHECK(std::get<std::string>(read(NpcName)) == "Theresa");
  KCD2S_CHECK(std::get<std::int64_t>(read(NpcScriptId)) == std::int64_t(npc));
  KCD2S_CHECK(std::get<std::int64_t>(read(NpcDimension)) == 7);
  KCD2S_CHECK(server.world().slot(npc)->dimension == 7);
  KCD2S_CHECK(std::get<std::int64_t>(read(NpcSimulator)) == 0);
  KCD2S_CHECK(std::get<std::int64_t>(read(NpcEpoch)) == 0);
  KCD2S_CHECK(std::get<std::string>(read(NpcNativeState)).empty());
  KCD2S_CHECK(std::get<std::int64_t>(read(NpcTeleport)) == 0);
  KCD2S_CHECK(!std::get<bool>(read(NpcFrozen)));
  KCD2S_CHECK(std::get<float>(read(NpcHealth)) == kNpcMaxHealth);
  KCD2S_CHECK(std::get<bool>(read(NpcAlive)));
  KCD2S_CHECK(std::get<std::string>(read(NpcOutfit)).empty());
  KCD2S_CHECK(std::get<std::string>(read(NpcAnimation)).empty());
  KCD2S_CHECK(std::get<std::int64_t>(read(NpcLife)) == 0);

  // Health: death, then a revive raises the life; values out of range are refused.
  KCD2S_CHECK(server.set_npc_health(npc, 40, true));
  KCD2S_CHECK(std::get<float>(read(NpcHealth)) == 40);
  KCD2S_CHECK(server.set_npc_health(npc, 0, false));
  KCD2S_CHECK(!std::get<bool>(read(NpcAlive)));
  KCD2S_CHECK(std::get<std::int64_t>(read(NpcLife)) == 0);
  KCD2S_CHECK(server.set_npc_health(npc, 100, true));
  KCD2S_CHECK(std::get<bool>(read(NpcAlive)));
  KCD2S_CHECK(std::get<std::int64_t>(read(NpcLife)) == 1);
  KCD2S_CHECK(server.set_npc_health(npc, 60, true));
  KCD2S_CHECK(std::get<std::int64_t>(read(NpcLife)) == 1);
  KCD2S_CHECK(!server.set_npc_health(npc, 0, true));
  KCD2S_CHECK(!server.set_npc_health(npc, -1, false));
  KCD2S_CHECK(!server.set_npc_health(npc, kNpcMaxHealth + 1, true));
  KCD2S_CHECK(!server.set_npc_health(npc, std::numeric_limits<float>::quiet_NaN(), true));
  KCD2S_CHECK(!server.set_npc_health(npc + 1000, 50, true));
  KCD2S_CHECK(std::get<float>(read(NpcHealth)) == 60);

  // Animations: "<sequence>|<loop>|<clip>" or "" to stop; control characters and over-long text
  // are refused.
  KCD2S_CHECK(server.set_npc_animation(npc, "1|1|reading_stand_inv_in"));
  KCD2S_CHECK(std::get<std::string>(read(NpcAnimation)) == "1|1|reading_stand_inv_in");
  KCD2S_CHECK(!server.set_npc_animation(npc, "2|0|line\nbreak"));
  KCD2S_CHECK(!server.set_npc_animation(npc, std::string(kNpcAnimationMax + 1, 'a')));
  KCD2S_CHECK(!server.set_npc_animation(npc + 1000, "1|0|a"));
  KCD2S_CHECK(std::get<std::string>(read(NpcAnimation)) == "1|1|reading_stand_inv_in");
  KCD2S_CHECK(server.set_npc_animation(npc, ""));
  KCD2S_CHECK(std::get<std::string>(read(NpcAnimation)).empty());

  // Intents: "<sequence>|<kind>|<arguments...>" or "" for none; control characters and over-long
  // text are refused.
  KCD2S_CHECK(std::get<std::string>(read(NpcIntent)).empty());
  KCD2S_CHECK(server.set_npc_intent(npc, "1|moveTo|10|20|30|walk|1.5|auto"));
  KCD2S_CHECK(std::get<std::string>(read(NpcIntent)) == "1|moveTo|10|20|30|walk|1.5|auto");
  KCD2S_CHECK(!server.set_npc_intent(npc, "2|attack|5|mel\nee"));
  KCD2S_CHECK(!server.set_npc_intent(npc, std::string(kNpcIntentMax + 1, 'a')));
  KCD2S_CHECK(!server.set_npc_intent(npc + 1000, "1|follow|2|walk|3"));
  KCD2S_CHECK(std::get<std::string>(read(NpcIntent)) == "1|moveTo|10|20|30|walk|1.5|auto");
  KCD2S_CHECK(server.set_npc_intent(npc, ""));
  KCD2S_CHECK(std::get<std::string>(read(NpcIntent)).empty());

  // Speech: "<sequence>|<milliseconds>|<text>" in UTF-8, one line.
  KCD2S_CHECK(std::get<std::string>(read(NpcSpeech)).empty());
  KCD2S_CHECK(server.set_npc_speech(npc, "1|3000|Dobr\xc3\xbd de\xc5\x88!"));
  KCD2S_CHECK(std::get<std::string>(read(NpcSpeech)) == "1|3000|Dobr\xc3\xbd de\xc5\x88!");
  KCD2S_CHECK(!server.set_npc_speech(npc, "2|3000|two\nlines"));
  KCD2S_CHECK(!server.set_npc_speech(npc, std::string(kNpcSpeechMax + 1, 's')));
  KCD2S_CHECK(!server.set_npc_speech(npc + 1000, "1|3000|a"));
  KCD2S_CHECK(server.set_npc_speech(npc, ""));
  KCD2S_CHECK(std::get<std::string>(read(NpcSpeech)).empty());

  // Appearance: four component names between three '|', an empty one the soul's own.
  KCD2S_CHECK(std::get<std::string>(read(NpcAppearance)).empty());
  KCD2S_CHECK(server.set_npc_appearance(npc, "|m_head_004|m_hair_001_blonde|m_beard_07"));
  KCD2S_CHECK(std::get<std::string>(read(NpcAppearance)) ==
              "|m_head_004|m_hair_001_blonde|m_beard_07");
  KCD2S_CHECK(server.set_npc_appearance(npc, "|||"));
  KCD2S_CHECK(!server.set_npc_appearance(npc, "||||"));
  KCD2S_CHECK(!server.set_npc_appearance(npc, "a|b|c"));
  KCD2S_CHECK(!server.set_npc_appearance(npc, "|two words||"));
  const std::string long_head(kAppearanceNameMax + 1, 'h');
  KCD2S_CHECK(!server.set_npc_appearance(npc, "|" + long_head + "||"));
  KCD2S_CHECK(!server.set_npc_appearance(npc + 1000, "|||"));
  KCD2S_CHECK(std::get<std::string>(read(NpcAppearance)) == "|||");

  // Names, frozen and teleports go through the server; bad values are refused.
  KCD2S_CHECK(server.set_npc_name(npc, "Theresa the Miller"));
  KCD2S_CHECK(!server.set_npc_name(npc, std::string(kNpcNameMax + 1, 'a')));
  KCD2S_CHECK(!server.set_npc_name(npc, "line\nbreak"));
  KCD2S_CHECK(std::get<std::string>(read(NpcName)) == "Theresa the Miller");
  KCD2S_CHECK(server.set_npc_frozen(npc, true));
  KCD2S_CHECK(std::get<bool>(read(NpcFrozen)));
  KCD2S_CHECK(server.teleport_npc(npc, {1, 2, 3}, facing()));
  KCD2S_CHECK(std::get<std::int64_t>(read(NpcTeleport)) == 1);
  KCD2S_CHECK(std::get<std::int64_t>(read(NpcEpoch)) == 1);
  KCD2S_CHECK(!server.teleport_npc(npc, {std::numeric_limits<float>::infinity(), 0, 0}, facing()));
  KCD2S_CHECK(!server.teleport_npc(npc, {1, 2, 3}, core::Quat{0, 0, 0, 0}));
  KCD2S_CHECK(std::get<std::int64_t>(read(NpcTeleport)) == 1);
  KCD2S_CHECK(!server.teleport_npc(npc + 1000, {1, 2, 3}, facing()));

  // What a spawn refuses.
  NpcSpawn bad = spawn;
  bad.actor_class = "Horse";
  KCD2S_CHECK(server.spawn_npc(bad) == core::kInvalidEntity);
  bad = spawn;
  bad.soul = "guard";
  KCD2S_CHECK(server.spawn_npc(bad) == core::kInvalidEntity);
  bad = spawn;
  bad.soul = "4a2b6c8d1e3f4a5b8c7d9e0f1a2b3c4d";
  KCD2S_CHECK(server.spawn_npc(bad) == core::kInvalidEntity);
  bad = spawn;
  bad.position.z = std::numeric_limits<float>::quiet_NaN();
  KCD2S_CHECK(server.spawn_npc(bad) == core::kInvalidEntity);
  bad = spawn;
  bad.name = std::string(kNpcNameMax + 1, 'n');
  KCD2S_CHECK(server.spawn_npc(bad) == core::kInvalidEntity);
  KCD2S_CHECK(valid_npc_soul("4A2B6C8D-1E3F-4a5b-8c7d-9e0f1a2b3c4d"));
  KCD2S_CHECK(valid_npc_soul(""));
}

KCD2S_TEST(npc, unpublished_sequences_still_fence_old_states_and_late_views_get_full_state) {
  NpcWorld w;
  w.place(0, 10);
  w.place(1, 30);
  w.place(2, 10000);
  w.pump(30);
  KCD2S_CHECK(w.simulator() == w.peers[0].player);
  auto state = w.state(0, 1, 0);
  local::AnimationState animations;
  animations[0].sequence = 1;
  animations[0].clip = 8;
  state.animations = local::encode_animation(animations);
  w.send_raw(0, encode_npc_state(state));
  const auto first = w.native_state();
  const auto first_tick = w.server.world().field_change_tick(w.npc, NpcNativeState);
  KCD2S_CHECK(!first.empty());

  state.sequence = 10;
  animations[0].params.normalized_time = 0.4f;
  state.animations = local::encode_animation(animations);
  w.send_raw(0, encode_npc_state(state));
  KCD2S_CHECK(w.native_state() == first);
  KCD2S_CHECK(w.server.world().field_change_tick(w.npc, NpcNativeState) == first_tick);
  state.sequence = 9;
  state.position.x = 5;
  w.send_raw(0, encode_npc_state(state));
  KCD2S_CHECK(w.position().x == 0);

  // A new native start does not wait for the idle refresh, even though the body is still.
  state.sequence = 11;
  state.position.x = 0;
  animations[0].sequence = 2;
  animations[0].clip = 12;
  state.animations = local::encode_animation(animations);
  w.send_raw(0, encode_npc_state(state));
  KCD2S_CHECK(w.native_state() != first);
  w.place(2, 30);
  w.pump(30);
  const auto* joined = w.peers[2].client->mirror().get(w.npc);
  KCD2S_CHECK(joined != nullptr);
  if (joined) {
    const auto& raw = std::get<std::string>(joined->fields[NpcNativeState]);
    NpcState decoded;
    KCD2S_CHECK(decode_npc_state(std::vector<std::uint8_t>(raw.begin(), raw.end()), decoded));
    KCD2S_CHECK(decoded.sequence == 11);
    const auto clips = local::decode_animation(decoded.animations);
    KCD2S_CHECK(clips.has_value());
    if (clips) {
      KCD2S_CHECK(clips->at(0).clip == 12);
    }
  }
  // Progress is still periodically published as a complete payload for reseeding/loss recovery.
  state.sequence = 12;
  animations[0].params.normalized_time = 0.8f;
  state.animations = local::encode_animation(animations);
  w.send_raw(0, encode_npc_state(state));
  NpcState refreshed;
  const auto raw = w.native_state();
  KCD2S_CHECK(decode_npc_state(std::vector<std::uint8_t>(raw.begin(), raw.end()), refreshed));
  KCD2S_CHECK(refreshed.sequence == 12);
}

KCD2S_TEST(npc, state_from_the_simulator_is_taken_and_relayed) {
  NpcWorld w;
  w.place(0, 10);
  w.place(1, 30);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  KCD2S_CHECK_EQ(w.value(NpcEpoch), std::int64_t(1));
  const auto changes = w.server.take_npc_simulator_changes();
  KCD2S_CHECK(changes.size() == 1);
  if (changes.size() == 1) {
    KCD2S_CHECK(changes[0] == (NpcSimulatorChange{w.npc, w.peers[0].player, 0, 1}));
  }

  // The simulator's state is written and its bytes reach the other clients unchanged.
  const auto first = w.state(0, 1, 2);
  KCD2S_CHECK(w.send(0, first));
  KCD2S_CHECK(w.position().x == 2);
  KCD2S_CHECK(std::get<core::Vec3>(*w.server.world().read(w.npc, NpcVelocity)).x == 1);
  const auto first_bytes = encode_npc_state(first);
  KCD2S_CHECK(w.native_state() == std::string(first_bytes.begin(), first_bytes.end()));
  w.pump(10);
  const auto* replica = w.peers[1].client->mirror().get(w.npc);
  KCD2S_CHECK(replica != nullptr);
  if (replica) {
    KCD2S_CHECK(std::get<core::Vec3>(replica->fields[NpcPosition]).x == 2);
    const auto& raw = std::get<std::string>(replica->fields[NpcNativeState]);
    NpcState relayed;
    KCD2S_CHECK(decode_npc_state(std::vector<std::uint8_t>(raw.begin(), raw.end()), relayed));
    KCD2S_CHECK(std::int64_t(relayed.epoch) == std::get<std::int64_t>(replica->fields[NpcEpoch]));
    KCD2S_CHECK(std::int64_t(relayed.source) ==
                std::get<std::int64_t>(replica->fields[NpcSimulator]));
    KCD2S_CHECK(relayed.revision == 1);
  }

  // Another player's client: its ClientPeer will not send, and forged bytes are refused, also
  // when they name the simulator as their source.
  auto forged = w.state(1, 5, 5);
  KCD2S_CHECK(!w.peers[1].client->send_npc_state(forged));
  w.send_raw(1, encode_npc_state(forged));
  KCD2S_CHECK(w.position().x == 2);
  forged.source = w.peers[0].player;
  w.send_raw(1, encode_npc_state(forged));
  KCD2S_CHECK(w.position().x == 2);

  // An old or repeated sequence is refused; a newer one is taken.
  KCD2S_CHECK(w.send(0, w.state(0, 1, 6)));
  KCD2S_CHECK(w.position().x == 2);
  KCD2S_CHECK(w.send(0, w.state(0, 2, 7)));
  KCD2S_CHECK(w.position().x == 7);

  // A state for another world revision is refused, as for horses.
  auto revision = w.state(0, 3, 8);
  revision.revision = 2;
  w.send_raw(0, encode_npc_state(revision));
  KCD2S_CHECK(w.position().x == 7);

  // Bad values are refused.
  auto nan = w.state(0, 4, 0);
  nan.position.x = std::numeric_limits<float>::quiet_NaN();
  w.send_raw(0, encode_npc_state(nan));
  auto horse_magic = w.state(0, 4, 9);
  HorseState as_horse;
  static_cast<BodyState&>(as_horse) = horse_magic;
  as_horse.horse = w.npc;
  w.send_raw(0, encode_horse(as_horse));
  KCD2S_CHECK(w.position().x == 7);
  // The raw path itself is taken when the state is right.
  w.send_raw(0, encode_npc_state(w.state(0, 4, 9)));
  KCD2S_CHECK(w.position().x == 9);

  // A teleport starts a new epoch: a state the simulator sent before it saw the teleport is
  // refused and the body stays where the server put it.
  KCD2S_CHECK(w.server.teleport_npc(w.npc, {-50, 0, 0}, facing()));
  KCD2S_CHECK_EQ(w.value(NpcEpoch), std::int64_t(2));
  KCD2S_CHECK_EQ(w.value(NpcTeleport), std::int64_t(1));
  KCD2S_CHECK(w.native_state().empty());
  KCD2S_CHECK(w.send(0, w.state(0, 5, 11)));
  KCD2S_CHECK(w.position().x == -50);
  w.pump(10);
  const auto* simulated = w.peers[0].client->mirror().get(w.npc);
  KCD2S_CHECK(simulated && std::get<std::int64_t>(simulated->fields[NpcTeleport]) == 1);
  KCD2S_CHECK(w.send(0, w.state(0, 1, 12)));
  KCD2S_CHECK(w.position().x == 12);
  KCD2S_CHECK(w.server.take_npc_simulator_changes().empty());
}

KCD2S_TEST(npc, states_are_limited_per_second) {
  NpcWorld w;
  w.place(0, 10);
  w.pump(90);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  // Forty raw states in one datagram: bypass the normal sender's motion cadence so this
  // exercises the server's independent flood limit. The first kMaxNpcStatesPerSecond are taken.
  for (std::uint64_t sequence = 100; sequence < 140; ++sequence) {
    w.peers[0].raw->queue(kNpcMotionChannel,
                          encode_npc_state(w.state(0, sequence, float(sequence))));
  }
  w.peers[0].raw->flush(w.now);
  w.pump(4);
  KCD2S_CHECK(w.position().x == float(100 + kMaxNpcStatesPerSecond - 1));
  // Within the same second still nothing; a second later the next is taken.
  w.send_raw(0, encode_npc_state(w.state(0, 140, 140)));
  KCD2S_CHECK(w.position().x == float(100 + kMaxNpcStatesPerSecond - 1));
  w.pump(60);
  w.send_raw(0, encode_npc_state(w.state(0, 141, 141)));
  KCD2S_CHECK(w.position().x == 141);
}

KCD2S_TEST(npc, the_first_simulator_tells_the_face_unless_a_resource_chose_one) {
  NpcLookReport report;
  report.npc = 6;
  report.epoch = 7;
  report.revision = 8;
  report.look = "m_body_pale_02|m_head_005_b00|m_hair_002|m_beard_01";
  const auto bytes = encode_npc_look(report);
  KCD2S_CHECK(bytes.size() == 13 + report.look.size() && bytes[0] == kNpcLookTag);
  NpcLookReport decoded;
  KCD2S_CHECK(decode_npc_look(bytes, decoded) && decoded == report);
  report.look = "not a look";
  KCD2S_CHECK(encode_npc_look(report).empty());

  const auto look = [](const NpcWorld& w) {
    return std::get<std::string>(*w.server.world().read(w.npc, NpcAppearance));
  };
  NpcWorld w;
  w.place(0, 10);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  // Another player's client is not heard.
  KCD2S_CHECK(w.peers[1].client->send_npc_look(w.npc, "b|h1|r|d"));
  w.pump(4);
  KCD2S_CHECK(look(w).empty());
  // The simulator's is written, and replicated.
  KCD2S_CHECK(w.peers[0].client->send_npc_look(w.npc, "b|h2|r|d"));
  w.pump(10);
  KCD2S_CHECK(look(w) == "b|h2|r|d");
  const auto* replica = w.peers[1].client->mirror().get(w.npc);
  KCD2S_CHECK(replica && std::get<std::string>(replica->fields[NpcAppearance]) == "b|h2|r|d");
  // The first one stays: a new simulator's body is built from it, not the other way round.
  w.place(0, 40);
  w.place(1, 0);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
  KCD2S_CHECK(w.peers[1].client->send_npc_look(w.npc, "b|h3|r|d"));
  w.pump(4);
  KCD2S_CHECK(look(w) == "b|h2|r|d");
  // A resource's choice wins over any report.
  KCD2S_CHECK(w.server.set_npc_appearance(w.npc, "b|h4|r|d"));
  KCD2S_CHECK(w.peers[1].client->send_npc_look(w.npc, "b|h5|r|d"));
  w.pump(4);
  KCD2S_CHECK(look(w) == "b|h4|r|d");
}

KCD2S_TEST(npc, the_simulator_alone_tells_how_the_dead_body_lies) {
  // A pose: the entity's position and rotation, then the ragdoll's parts.
  const float header[7] = {1, 2, 3, 0, 0, 0, 1};
  std::string pose(reinterpret_cast<const char*>(header), sizeof(header));
  pose += "parts";
  NpcPoseReport report;
  report.npc = 6;
  report.epoch = 7;
  report.revision = 8;
  report.pose = pose;
  const auto bytes = encode_npc_pose(report);
  KCD2S_CHECK(bytes.size() == 13 + pose.size() && bytes[0] == kNpcPoseTag);
  NpcPoseReport decoded;
  KCD2S_CHECK(decode_npc_pose(bytes, decoded) && decoded == report);

  // No parts, a position that is not a number, or too many bytes: the pose cannot travel.
  report.pose = pose.substr(0, kNpcPoseHeader);
  KCD2S_CHECK(encode_npc_pose(report).empty());
  float bad[7] = {1, 2, 3, 0, 0, 0, 1};
  bad[0] = std::numeric_limits<float>::quiet_NaN();
  report.pose = std::string(reinterpret_cast<const char*>(bad), sizeof(bad)) + "parts";
  KCD2S_CHECK(encode_npc_pose(report).empty());
  report.pose = pose + std::string(kNpcPoseMax, 'p');
  KCD2S_CHECK(encode_npc_pose(report).empty());

  NpcWorld w;
  w.place(0, 10);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  KCD2S_CHECK(w.text(NpcRagdollPose).empty());

  // A living NPC's body does not lie: its simulator's pose is not taken.
  KCD2S_CHECK(w.peers[0].client->send_npc_pose(w.npc, pose));
  w.pump(4);
  KCD2S_CHECK(w.text(NpcRagdollPose).empty());

  // Dead: the simulator's pose is taken and reaches every client.
  KCD2S_CHECK(w.server.set_npc_health(w.npc, 0.0f, false));
  w.pump(4);
  KCD2S_CHECK(w.peers[0].client->send_npc_pose(w.npc, pose));
  w.pump(4);
  KCD2S_CHECK(w.text(NpcRagdollPose) == pose);
  w.pump(10);
  const auto* replica = w.peers[1].client->mirror().get(w.npc);
  KCD2S_CHECK(replica && std::get<std::string>(replica->fields[NpcRagdollPose]) == pose);

  // Another player's client changes nothing.
  const float other_header[7] = {9, 9, 9, 0, 0, 0, 1};
  const std::string other =
      std::string(reinterpret_cast<const char*>(other_header), sizeof(other_header)) + "other";
  KCD2S_CHECK(w.peers[1].client->send_npc_pose(w.npc, other));
  w.pump(4);
  KCD2S_CHECK(w.text(NpcRagdollPose) == pose);

  // A revive makes a standing body: the pose of the last life goes.
  KCD2S_CHECK(w.server.set_npc_health(w.npc, 100.0f, true));
  KCD2S_CHECK(w.text(NpcRagdollPose).empty());
}

KCD2S_TEST(npc, the_simulator_alone_tells_what_the_body_wears) {
  NpcOutfitReport report;
  report.npc = 6;
  report.epoch = 7;
  report.revision = 8;
  report.outfit = std::string("\x01\x02outfit", 8);
  const auto bytes = encode_npc_outfit(report);
  KCD2S_CHECK(bytes.size() == 13 + report.outfit.size() && bytes[0] == kNpcOutfitTag);
  NpcOutfitReport decoded;
  KCD2S_CHECK(decode_npc_outfit(bytes, decoded) && decoded == report);
  report.outfit = std::string(kNpcOutfitMax + 1, 'o');
  KCD2S_CHECK(encode_npc_outfit(report).empty());
  report.outfit.clear();
  report.npc = 0;
  KCD2S_CHECK(encode_npc_outfit(report).empty());

  NpcWorld w;
  w.place(0, 10);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  KCD2S_CHECK(w.peers[0].client->send_npc_outfit(w.npc, "hat,coat"));
  w.pump(4);
  KCD2S_CHECK(w.outfit() == "hat,coat");
  w.pump(10);
  const auto* replica = w.peers[1].client->mirror().get(w.npc);
  KCD2S_CHECK(replica && std::get<std::string>(replica->fields[NpcOutfit]) == "hat,coat");

  // At most kMaxNpcOutfitsPerSecond in a second.
  w.pump(60);
  for (unsigned i = 0; i < kMaxNpcOutfitsPerSecond + 2; ++i) {
    KCD2S_CHECK(w.peers[0].client->send_npc_outfit(w.npc, "outfit " + std::to_string(i)));
  }
  w.pump(4);
  const std::string kept = w.outfit();
  w.pump(60);

  // Another player's client changes nothing.
  KCD2S_CHECK(w.peers[1].client->send_npc_outfit(w.npc, "rags"));
  w.pump(4);
  KCD2S_CHECK(w.outfit() == kept);

  // After a handoff the new simulator tells it and the old one no longer does.
  w.place(0, 40);
  w.place(1, 0);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
  KCD2S_CHECK(w.peers[0].client->send_npc_outfit(w.npc, "old"));
  w.pump(4);
  KCD2S_CHECK(w.outfit() == kept);
  KCD2S_CHECK(w.peers[1].client->send_npc_outfit(w.npc, "new"));
  w.pump(4);
  KCD2S_CHECK(w.outfit() == "new");

  // A resource's outfit (Npc.setOutfit) is fixed: the simulator's reports are no longer taken.
  const std::string bow = "KO2" + std::string(16, '\x01') + std::string("\x04\xff\xff", 3);
  KCD2S_CHECK(w.server.set_npc_outfit(w.npc, bow));
  KCD2S_CHECK(w.outfit() == bow);
  KCD2S_CHECK(std::get<bool>(*w.server.world().read(w.npc, NpcOutfitFixed)));
  w.pump(60);
  KCD2S_CHECK(w.peers[1].client->send_npc_outfit(w.npc, "simulator's own"));
  w.pump(4);
  KCD2S_CHECK(w.outfit() == bow);
  KCD2S_CHECK(!w.server.set_npc_outfit(w.npc, "KO1"));
  KCD2S_CHECK(!w.server.set_npc_outfit(w.npc, "KO2" + std::string(18, 'x')));
  KCD2S_CHECK(!w.server.set_npc_outfit(w.npc + 1000, bow));
  KCD2S_CHECK(w.server.set_npc_outfit(w.npc, "KO2"));
  KCD2S_CHECK(w.outfit() == "KO2");
}

KCD2S_TEST(npc, hits_are_taken_from_the_attacker_and_from_the_simulator) {
  NpcHit hit;
  hit.npc = 6;
  hit.revision = 8;
  hit.attacker = 9;
  hit.amount = 12.5f;
  hit.reason = 1;
  hit.subpart = 23;
  const auto bytes = encode_npc_hit(hit);
  KCD2S_CHECK(bytes.size() == kNpcHitBytes && bytes[0] == kNpcHitTag);
  NpcHit decoded;
  KCD2S_CHECK(decode_npc_hit(bytes, decoded) && decoded == hit);
  NpcHit bad = hit;
  bad.amount = 0;
  KCD2S_CHECK(encode_npc_hit(bad).empty());
  bad.amount = kMaxDamage * 2;
  KCD2S_CHECK(encode_npc_hit(bad).empty());
  bad = hit;
  bad.reason = kDamageReasons;
  KCD2S_CHECK(encode_npc_hit(bad).empty());
  bad = hit;
  bad.subpart = -2;
  KCD2S_CHECK(encode_npc_hit(bad).empty());
  bad = hit;
  bad.npc = 0;
  KCD2S_CHECK(encode_npc_hit(bad).empty());
  auto truncated = bytes;
  truncated.pop_back();
  KCD2S_CHECK(!decode_npc_hit(truncated, decoded));

  NpcWorld w;
  w.place(0, 10);
  w.place(1, 20);
  w.place(2, 400);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  const auto hit_by = [&](unsigned peer, core::EntityId attacker, float amount) {
    NpcHit h;
    h.npc = w.npc;
    h.attacker = attacker;
    h.amount = amount;
    h.reason = 1;
    const bool sent = w.peers[peer].client->send_npc_hit(h);
    w.pump(4);
    return sent;
  };
  // A player's own blow, from the simulator and from another client near the NPC.
  KCD2S_CHECK(hit_by(0, w.peers[0].player, 10));
  KCD2S_CHECK(hit_by(1, w.peers[1].player, 20));
  // A blow in another player's name, or a fall reported by a client that does not simulate it.
  KCD2S_CHECK(hit_by(1, w.peers[0].player, 30));
  KCD2S_CHECK(hit_by(1, 0, 40));
  // The simulator tells of a fall, and of another NPC's blow, but not of an unknown attacker.
  KCD2S_CHECK(hit_by(0, 0, 50));
  KCD2S_CHECK(hit_by(0, w.npc, 60));
  KCD2S_CHECK(hit_by(0, w.npc + 1000, 70));
  // Too far away to have struck it (its client may no longer hold the NPC at all).
  w.place(1, kNpcHitRange + 10);
  w.pump(4);
  hit_by(1, w.peers[1].player, 80);
  auto taken = w.server.take_npc_hits();
  std::vector<float> amounts;
  for (const auto& h : taken) {
    amounts.push_back(h.amount);
  }
  KCD2S_CHECK((amounts == std::vector<float>{10, 20, 50, 60}));
  KCD2S_CHECK_EQ(taken[0].attacker, w.peers[0].player);
  KCD2S_CHECK_EQ(taken[1].attacker, w.peers[1].player);
  KCD2S_CHECK_EQ(taken[2].attacker, core::kInvalidEntity);
  KCD2S_CHECK_EQ(taken[3].attacker, w.npc);

  // A dead NPC takes no hits.
  KCD2S_CHECK(w.server.set_npc_health(w.npc, 0, false));
  w.pump(10);
  hit_by(0, w.peers[0].player, 5);
  KCD2S_CHECK(w.server.take_npc_hits().empty());
  KCD2S_CHECK(w.server.set_npc_health(w.npc, 100, true));
  w.pump(10);

  // At most kMaxNpcHitsPerSecond a second from one client.
  w.pump(60);
  for (unsigned i = 0; i < kMaxNpcHitsPerSecond + 5; ++i) {
    NpcHit h;
    h.npc = w.npc;
    h.attacker = w.peers[0].player;
    h.amount = 1;
    KCD2S_CHECK(w.peers[0].client->send_npc_hit(h));
  }
  w.pump(4);
  KCD2S_CHECK_EQ(w.server.take_npc_hits().size(), std::size_t(kMaxNpcHitsPerSecond));
}

KCD2S_TEST(npc, blows_go_to_the_game_on_the_other_side_of_the_fight) {
  NpcBlow blow;
  blow.npc = 6;
  blow.revision = 8;
  blow.player = 9;
  blow.direction = NpcBlowDirection::NpcStruckPlayer;
  const float health = 12.5f;
  std::memcpy(blow.hit.data() + 48, &health, sizeof(health));
  const auto bytes = encode_npc_blow(blow);
  KCD2S_CHECK(bytes.size() == kNpcBlowBytes && bytes[0] == kNpcBlowTag);
  NpcBlow decoded;
  KCD2S_CHECK(decode_npc_blow(bytes, decoded) && decoded == blow);
  // A HitInfo with an entity id or an item handle of the sender's process does not travel.
  NpcBlow local = blow;
  local.hit[40] = 7;
  KCD2S_CHECK(encode_npc_blow(local).empty());
  KCD2S_CHECK(portable_npc_hit(local.hit.data()) == blow.hit);
  auto bad_direction = bytes;
  bad_direction[13] = 2;
  KCD2S_CHECK(!decode_npc_blow(bad_direction, decoded));

  NpcWorld w;
  w.place(0, 10);
  w.place(1, 20);
  w.place(2, 30);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  const auto send = [&](unsigned peer, core::EntityId player, NpcBlowDirection direction) {
    NpcBlow b;
    b.npc = w.npc;
    b.player = player;
    b.direction = direction;
    KCD2S_CHECK(w.peers[peer].client->send_npc_blow(b));
    w.pump(6);
  };
  const auto received = [&](unsigned peer) { return w.peers[peer].client->take_npc_blows(); };
  // A player's blow on the NPC goes to its simulator only.
  send(1, w.peers[1].player, NpcBlowDirection::PlayerStruckNpc);
  auto at_simulator = received(0);
  KCD2S_CHECK_EQ(at_simulator.size(), std::size_t(1));
  KCD2S_CHECK_EQ(at_simulator[0].player, w.peers[1].player);
  KCD2S_CHECK(at_simulator[0].direction == NpcBlowDirection::PlayerStruckNpc);
  KCD2S_CHECK(received(1).empty() && received(2).empty());
  // That player fights the NPC now, so its client runs it (the_player_fighting_the_npc_...).
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
  // Not in another player's name, and not from the simulator itself.
  send(0, w.peers[2].player, NpcBlowDirection::PlayerStruckNpc);
  send(1, w.peers[1].player, NpcBlowDirection::PlayerStruckNpc);
  KCD2S_CHECK(received(1).empty());
  // The NPC's blow on a player goes from its simulator to that player's own client only.
  send(1, w.peers[2].player, NpcBlowDirection::NpcStruckPlayer);
  auto at_struck = received(2);
  KCD2S_CHECK_EQ(at_struck.size(), std::size_t(1));
  KCD2S_CHECK(at_struck[0].direction == NpcBlowDirection::NpcStruckPlayer);
  KCD2S_CHECK(received(0).empty() && received(1).empty());
  // Only the simulator tells of the NPC's blows, and not of one on its own player.
  send(0, w.peers[2].player, NpcBlowDirection::NpcStruckPlayer);
  send(1, w.peers[1].player, NpcBlowDirection::NpcStruckPlayer);
  KCD2S_CHECK(received(1).empty() && received(2).empty());
  // A dead NPC fights no more.
  KCD2S_CHECK(w.server.set_npc_health(w.npc, 0, false));
  w.pump(10);
  send(0, w.peers[0].player, NpcBlowDirection::PlayerStruckNpc);
  KCD2S_CHECK(received(1).empty());
  KCD2S_CHECK(w.server.set_npc_health(w.npc, 100, true));
  w.pump(10);
  // At most kMaxNpcBlowsPerSecond a second from one client.
  w.pump(60);
  for (unsigned i = 0; i < kMaxNpcBlowsPerSecond + 5; ++i) {
    NpcBlow b;
    b.npc = w.npc;
    b.player = w.peers[0].player;
    KCD2S_CHECK(w.peers[0].client->send_npc_blow(b));
  }
  w.pump(6);
  KCD2S_CHECK_EQ(received(1).size(), std::size_t(kMaxNpcBlowsPerSecond));
}

KCD2S_TEST(npc, the_player_fighting_the_npc_simulates_it) {
  NpcWorld w;
  w.place(0, 10);
  w.place(1, 30);
  w.place(2, 50);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  const auto strike = [&](unsigned peer) {
    NpcHit h;
    h.npc = w.npc;
    h.attacker = w.peers[peer].player;
    h.amount = 1;
    KCD2S_CHECK(w.peers[peer].client->send_npc_hit(h));
    w.pump(20);
  };
  // The farther player strikes the NPC: its client runs the NPC from then on.
  strike(1);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
  // Another player's blows do not take the fight over while it lasts.
  strike(2);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
  // The NPC's blow on the third player, from the simulator, does not either.
  NpcBlow blow;
  blow.npc = w.npc;
  blow.player = w.peers[2].player;
  blow.direction = NpcBlowDirection::NpcStruckPlayer;
  KCD2S_CHECK(w.peers[1].client->send_npc_blow(blow));
  w.pump(20);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
  // When the fight is over the NPC stays with the fight's game while it witnesses its own fight
  // (kNpcWitnessSeconds: that game's brain saw the attack); then the nearest
  // player's rule holds again: the first player, 20 m nearer, takes the NPC back.
  w.pump(int(kNpcFightSeconds * 60) + 30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
  w.pump(int((kNpcWitnessSeconds - kNpcFightSeconds) * 60) + 30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  // A new fight goes to the player in it.
  strike(2);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[2].player);
  // A fighter that went too far away no longer runs it.
  w.place(2, kNpcSimulatorKeepRange + 20);
  w.pump(30);
  KCD2S_CHECK(w.simulator() != w.peers[2].player);
}

KCD2S_TEST(npc, a_level_person_guid_is_kept_and_a_malformed_one_refused) {
  NpcWorld w;
  w.place(0, 10);
  w.pump(10);
  const auto read = [&]() {
    return std::get<std::string>(*w.server.world().read(w.npc, NpcLevelGuid));
  };
  KCD2S_CHECK(read().empty());
  KCD2S_CHECK(w.server.set_npc_level_guid(w.npc, "002f4216abf742e1"));
  KCD2S_CHECK_EQ(read(), std::string("002f4216abf742e1"));
  KCD2S_CHECK(!w.server.set_npc_level_guid(w.npc, "002F4216ABF742E1"));
  KCD2S_CHECK(!w.server.set_npc_level_guid(w.npc, "002f4216"));
  KCD2S_CHECK(w.server.set_npc_level_guid(w.npc, ""));
  KCD2S_CHECK(read().empty());
}

KCD2S_TEST(npc, interactable_is_on_by_default_and_set_by_the_server) {
  NpcWorld w;
  w.place(0, 10);
  w.pump(10);
  const auto read = [&]() {
    return std::get<bool>(*w.server.world().read(w.npc, NpcInteractable));
  };
  KCD2S_CHECK(read());
  KCD2S_CHECK(w.server.set_npc_interactable(w.npc, false));
  KCD2S_CHECK(!read());
  KCD2S_CHECK(!w.server.set_npc_interactable(EntityId(999999), false));
}

KCD2S_TEST(npc, walking_into_the_npc_is_no_fight_and_keeps_its_simulator) {
  NpcWorld w;
  w.place(0, 10);
  w.place(1, 30);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  // The farther player's collision with the NPC (E_DamageReason 4): it stays with the nearest.
  NpcHit bump;
  bump.npc = w.npc;
  bump.attacker = w.peers[1].player;
  bump.amount = 0.5f;
  bump.reason = 4;
  KCD2S_CHECK(w.peers[1].client->send_npc_hit(bump));
  w.pump(20);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  // A blow in combat does take it.
  NpcHit blow = bump;
  blow.reason = 1;
  KCD2S_CHECK(w.peers[1].client->send_npc_hit(blow));
  w.pump(20);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
}

KCD2S_TEST(npc, a_player_locking_on_near_the_npc_fights_it) {
  NpcEngage engage;
  engage.npc = 6;
  engage.revision = 8;
  const auto bytes = encode_npc_engage(engage);
  KCD2S_CHECK(bytes.size() == kNpcEngageBytes && bytes[0] == kNpcEngageTag);
  NpcEngage decoded;
  KCD2S_CHECK(decode_npc_engage(bytes, decoded) && decoded == engage);
  engage.npc = 0;
  KCD2S_CHECK(encode_npc_engage(engage).empty());

  NpcWorld w;
  w.place(0, 2);
  w.place(1, 5);
  w.place(2, 30);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  // Too far away to lock on: nothing changes.
  KCD2S_CHECK(w.peers[2].client->send_npc_engage(w.npc));
  w.pump(20);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  // Near enough: the NPC goes to that player's client.
  KCD2S_CHECK(w.peers[1].client->send_npc_engage(w.npc));
  w.pump(20);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
  // A dead NPC is not engaged.
  KCD2S_CHECK(w.server.set_npc_health(w.npc, 0, false));
  w.pump(int(kNpcFightSeconds * 60) + 30);
  KCD2S_CHECK(w.peers[2].client->send_npc_engage(w.npc));
  w.place(2, 4);
  w.pump(4);
  KCD2S_CHECK(w.peers[2].client->send_npc_engage(w.npc));
  w.pump(20);
  KCD2S_CHECK(w.simulator() != w.peers[2].player);
}

KCD2S_TEST(npc, the_nearest_player_simulates_with_hysteresis) {
  NpcWorld w;
  // Nobody within 150 m: no simulator yet.
  KCD2S_CHECK_EQ(w.simulator(), core::EntityId(0));
  KCD2S_CHECK(w.server.take_npc_simulator_changes().empty());

  w.place(0, 100);
  w.place(1, 140);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  auto changes = w.server.take_npc_simulator_changes();
  KCD2S_CHECK(changes.size() == 1);

  // 15 m nearer is not enough to take it over; 21 m is.
  w.place(1, 85);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  w.place(1, 79);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
  KCD2S_CHECK_EQ(w.value(NpcEpoch), std::int64_t(2));
  changes = w.server.take_npc_simulator_changes();
  KCD2S_CHECK(changes.size() == 1);
  if (changes.size() == 1) {
    KCD2S_CHECK(changes[0] == (NpcSimulatorChange{w.npc, w.peers[1].player, w.peers[0].player, 2}));
  }

  // The simulator keeps it up to 170 m, against a player at 150 m who is only 15 m nearer.
  w.place(0, 400);
  w.place(1, 165);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
  w.place(2, 150);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
  // Beyond 170 m it goes to the nearest player within 150 m.
  w.place(1, 171);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[2].player);
  // Nobody near: nobody simulates, and the NPC stays where it was.
  const auto before = w.position();
  w.place(2, 200);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), core::EntityId(0));
  KCD2S_CHECK(w.position() == before);
  KCD2S_CHECK_EQ(w.value(NpcEpoch), std::int64_t(4));
  changes = w.server.take_npc_simulator_changes();
  KCD2S_CHECK(changes.size() == 2);
  if (changes.size() == 2) {
    KCD2S_CHECK(changes[0] == (NpcSimulatorChange{w.npc, w.peers[2].player, w.peers[1].player, 3}));
    KCD2S_CHECK(changes[1] == (NpcSimulatorChange{w.npc, 0, w.peers[2].player, 4}));
  }

  // The simulator leaves: the next player near takes over at once, without an election.
  w.place(0, 10);
  w.place(1, 50);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  w.server.take_npc_simulator_changes();
  w.server.remove_client(w.peers[0].id);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
  changes = w.server.take_npc_simulator_changes();
  KCD2S_CHECK(changes.size() == 1);
  if (changes.size() == 1) {
    KCD2S_CHECK(changes[0].previous == w.peers[0].player);
  }

  // A client whose world is loading again does not simulate.
  WorldConfig reload;
  reload.revision = 2;
  KCD2S_CHECK(w.server.configure_world(w.peers[1].id, reload));
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), core::EntityId(0));

  // Dimensions: a player of the global world may simulate an NPC of world 7; a player of
  // world 3 may not.
  KCD2S_CHECK(w.server.set_npc_dimension(w.npc, 7));
  KCD2S_CHECK_EQ(w.value(NpcDimension), std::int64_t(7));
  w.place(2, 20);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[2].player);
  KCD2S_CHECK(w.server.set_dimension(w.peers[2].player, 3));
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), core::EntityId(0));
  // Three changes since the departure: the reload, world 7 and world 3.
  KCD2S_CHECK(w.server.take_npc_simulator_changes().size() == 3);

  // A despawned NPC is forgotten.
  w.server.despawn(w.npc);
  w.pump(30);
  KCD2S_CHECK(w.server.take_npc_simulator_changes().empty());
}

KCD2S_TEST(npc, world_only_peers_cannot_take_simulation_before_gameplay_or_after_revision_change) {
  NpcWorld w(false);
  w.place(0, 1);
  w.place(1, 60);
  w.place(2, 2);
  w.pump(60);
  for (const auto& peer : w.peers) {
    KCD2S_CHECK(w.server.peer_world_ready(peer.id));
    KCD2S_CHECK(!w.server.peer_gameplay_ready(peer.id));
    KCD2S_CHECK(peer.client->mirror().get(w.npc) != nullptr);
  }
  KCD2S_CHECK_EQ(w.simulator(), core::EntityId{0});
  // A farther native game wins over closer world-only load probes.
  KCD2S_CHECK(w.peers[1].client->mark_gameplay_started(1));
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
  KCD2S_CHECK(w.peers[0].client->mark_gameplay_started(1));
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  const auto old_epoch = w.value(NpcEpoch);
  WorldConfig next;
  next.revision = 2;
  KCD2S_CHECK(w.server.configure_world(w.peers[0].id, next));
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
  KCD2S_CHECK(w.value(NpcEpoch) > old_epoch);
  KCD2S_CHECK(w.peers[0].client->mark_world_ready(2));
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
  auto rejected = w.state(0, 100, 90);
  rejected.revision = 2;
  const auto position = w.position();
  w.send_raw(0, encode_npc_state(rejected));
  KCD2S_CHECK(w.position() == position);
  KCD2S_CHECK(!w.peers[0].client->mark_gameplay_started(1));
  KCD2S_CHECK(w.peers[0].client->mark_gameplay_started(2));
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
}

KCD2S_TEST(npc, a_pinned_npc_stays_with_its_player_or_goes_dormant) {
  NpcWorld w;
  w.place(0, 10);
  w.place(1, 120);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  // Pinned to the farther player: theirs whatever the nearer one.
  KCD2S_CHECK(w.server.pin_npc(w.npc, w.peers[1].player));
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
  // Out of range it goes dormant rather than to the nearer player.
  w.place(1, 400);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), core::EntityId(0));
  // Unpinned, the election has it again.
  KCD2S_CHECK(w.server.pin_npc(w.npc, core::kInvalidEntity));
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  // A pin to the player who leaves ends with them.
  w.place(1, 50);
  KCD2S_CHECK(w.server.pin_npc(w.npc, w.peers[1].player));
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
  w.server.remove_client(w.peers[1].id);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  // Nothing that is a player is refused.
  KCD2S_CHECK(!w.server.pin_npc(w.npc, w.npc));
  KCD2S_CHECK(!w.server.pin_npc(core::EntityId(999999), w.peers[0].player));
}

KCD2S_TEST(npc, animation_end_message_is_exact_and_rejects_bad_values) {
  NpcAnimationEnd end;
  end.npc = 4162;
  end.epoch = 3;
  end.sequence = 7;
  end.reason = kNpcAnimationInterrupted;
  const auto bytes = encode_npc_animation_end(end);
  KCD2S_CHECK_EQ(bytes.size(), kNpcAnimationEndBytes);
  NpcAnimationEnd decoded;
  KCD2S_CHECK(decode_npc_animation_end(bytes, decoded));
  KCD2S_CHECK(decoded == end);
  auto bad = bytes;
  bad[13] = 3;
  KCD2S_CHECK(!decode_npc_animation_end(bad, decoded));
  bad = bytes;
  bad.pop_back();
  KCD2S_CHECK(!decode_npc_animation_end(bad, decoded));
  end.npc = 0;
  KCD2S_CHECK(encode_npc_animation_end(end).empty());
}

KCD2S_TEST(npc, use_message_is_exact) {
  const auto bytes = encode_npc_use(4154);
  KCD2S_CHECK_EQ(bytes.size(), kNpcUseBytes);
  core::EntityId npc = 0;
  KCD2S_CHECK(decode_npc_use(bytes, npc));
  KCD2S_CHECK_EQ(npc, core::EntityId(4154));
  KCD2S_CHECK(encode_npc_use(0).empty());
  auto bad = bytes;
  bad.push_back(0);
  KCD2S_CHECK(!decode_npc_use(bad, npc));
}

KCD2S_TEST(npc, the_nameplate_and_kinematic_fields) {
  NpcWorld w;
  KCD2S_CHECK(w.text(NpcNameplate).empty());
  KCD2S_CHECK(!w.flag(NpcKinematic));
  KCD2S_CHECK(w.server.set_npc_nameplate(w.npc, "11;FFFFFFFF;"));
  KCD2S_CHECK(w.server.set_npc_nameplate(w.npc, "10;FFFF0000;Captain"));
  KCD2S_CHECK(w.text(NpcNameplate) == "10;FFFF0000;Captain");
  KCD2S_CHECK(!w.server.set_npc_nameplate(w.npc, "01;FFFFFFFF;"));
  KCD2S_CHECK(!w.server.set_npc_nameplate(w.npc, "11;FFFFFFFF;two\nlines"));
  KCD2S_CHECK(!w.server.set_npc_nameplate(w.npc, "11;FFFFFFFF;" + std::string(90, 'x')));
  KCD2S_CHECK(w.server.set_npc_nameplate(w.npc, ""));
  KCD2S_CHECK(w.server.set_npc_kinematic(w.npc, true));
  KCD2S_CHECK(w.flag(NpcKinematic));
}

// The brain and targetable switches: on at first, the server turns each off and on.
KCD2S_TEST(npc, the_brain_and_targetable_fields) {
  NpcWorld w;
  KCD2S_CHECK(w.flag(NpcBrain));
  KCD2S_CHECK(w.flag(NpcTargetable));
  KCD2S_CHECK(w.server.set_npc_brain(w.npc, false));
  KCD2S_CHECK(!w.flag(NpcBrain));
  KCD2S_CHECK(w.flag(NpcTargetable));
  KCD2S_CHECK(w.server.set_npc_targetable(w.npc, false));
  KCD2S_CHECK(!w.flag(NpcTargetable));
  KCD2S_CHECK(w.server.set_npc_brain(w.npc, true));
  KCD2S_CHECK(w.server.set_npc_targetable(w.npc, true));
  KCD2S_CHECK(w.flag(NpcBrain) && w.flag(NpcTargetable));
  KCD2S_CHECK(!w.server.set_npc_brain(core::EntityId(999999), false));
  KCD2S_CHECK(!w.server.set_npc_targetable(core::EntityId(999999), false));
  // A carcass offers the game's Search and Gut until the server closes it.
  KCD2S_CHECK(!w.flag(NpcCarcassClosed));
  KCD2S_CHECK(w.server.set_npc_carcass_closed(w.npc, true));
  KCD2S_CHECK(w.flag(NpcCarcassClosed));
  KCD2S_CHECK(!w.server.set_npc_carcass_closed(core::EntityId(999999), true));
}

// The NPC wire messages and the server rules around them (the integration gate).
namespace {

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    bytes[at + i] = std::uint8_t(value >> (8 * i));
  }
}

void put_float(std::vector<std::uint8_t>& bytes, std::size_t at, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  put_u32(bytes, at, bits);
}

// A message decodes at its exact length only: every shorter prefix and the message with bytes
// too many are refused.
template <typename Message>
void check_exact_length(kcd2s::testkit::Context& kcd2s_ctx, const std::vector<std::uint8_t>& bytes,
                        bool (*decode)(const std::vector<std::uint8_t>&, Message&)) {
  Message decoded;
  KCD2S_CHECK(decode(bytes, decoded));
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    const std::vector<std::uint8_t> prefix(bytes.begin(), bytes.begin() + length);
    KCD2S_CHECK(!decode(prefix, decoded));
  }
  auto longer = bytes;
  longer.push_back(0);
  KCD2S_CHECK(!decode(longer, decoded));
  longer.insert(longer.end(), bytes.begin(), bytes.end());
  KCD2S_CHECK(!decode(longer, decoded));
}

NpcBlow sample_blow(core::EntityId npc, core::EntityId player, NpcBlowDirection direction) {
  NpcBlow blow;
  blow.npc = npc;
  blow.player = player;
  blow.direction = direction;
  const float health = 12.5f;
  std::memcpy(blow.hit.data() + 48, &health, sizeof(health));
  return blow;
}

}  // namespace

KCD2S_TEST(npc, hit_message_is_exact_and_rejects_bad_values) {
  NpcHit hit;
  hit.npc = 0xfffffffeu;
  hit.revision = 0xffffffffu;
  hit.attacker = 0;
  hit.amount = kMaxDamage;
  hit.reason = kDamageReasons - 1;
  hit.subpart = -1;
  const auto bytes = encode_npc_hit(hit);
  KCD2S_CHECK_EQ(bytes.size(), kNpcHitBytes);
  KCD2S_CHECK_EQ(bytes[0], kNpcHitTag);
  NpcHit decoded;
  KCD2S_CHECK(decode_npc_hit(bytes, decoded) && decoded == hit);
  check_exact_length<NpcHit>(kcd2s_ctx, bytes, decode_npc_hit);

  // The other end of every range: the smallest amount, reason 0, the largest subpart.
  hit.amount = 0.001f;
  hit.reason = 0;
  hit.subpart = std::numeric_limits<std::int32_t>::max();
  KCD2S_CHECK(decode_npc_hit(encode_npc_hit(hit), decoded) && decoded == hit);

  // A message that arrives with a value the encoder would never write is refused.
  const auto patched = [&](auto change) {
    auto copy = bytes;
    change(copy);
    return decode_npc_hit(copy, decoded);
  };
  KCD2S_CHECK(!patched([](auto& b) { b[0] = kNpcBlowTag; }));
  KCD2S_CHECK(!patched([](auto& b) { put_u32(b, 1, 0); }));
  KCD2S_CHECK(!patched([](auto& b) { put_float(b, 13, 0.0f); }));
  KCD2S_CHECK(!patched([](auto& b) { put_float(b, 13, -1.0f); }));
  KCD2S_CHECK(!patched([](auto& b) { put_float(b, 13, kMaxDamage * 2); }));
  KCD2S_CHECK(!patched([](auto& b) { put_float(b, 13, std::numeric_limits<float>::quiet_NaN()); }));
  KCD2S_CHECK(!patched([](auto& b) { put_float(b, 13, std::numeric_limits<float>::infinity()); }));
  KCD2S_CHECK(!patched([](auto& b) { b[17] = kDamageReasons; }));
  KCD2S_CHECK(!patched([](auto& b) { b[17] = 0xff; }));
  KCD2S_CHECK(!patched([](auto& b) { put_u32(b, 18, std::uint32_t(-2)); }));
}

KCD2S_TEST(npc, blow_message_is_exact_and_rejects_bad_values) {
  const NpcBlow blow = sample_blow(0xfffffffeu, 9, NpcBlowDirection::NpcStruckPlayer);
  const auto bytes = encode_npc_blow(blow);
  KCD2S_CHECK_EQ(bytes.size(), kNpcBlowBytes);
  KCD2S_CHECK_EQ(bytes[0], kNpcBlowTag);
  NpcBlow decoded;
  KCD2S_CHECK(decode_npc_blow(bytes, decoded) && decoded == blow);
  check_exact_length<NpcBlow>(kcd2s_ctx, bytes, decode_npc_blow);
  const NpcBlow struck_npc = sample_blow(3, 4, NpcBlowDirection::PlayerStruckNpc);
  KCD2S_CHECK(decode_npc_blow(encode_npc_blow(struck_npc), decoded) && decoded == struck_npc);

  const auto patched = [&](auto change) {
    auto copy = bytes;
    change(copy);
    return decode_npc_blow(copy, decoded);
  };
  KCD2S_CHECK(!patched([](auto& b) { b[0] = kNpcHitTag; }));
  KCD2S_CHECK(!patched([](auto& b) { put_u32(b, 1, 0); }));
  KCD2S_CHECK(!patched([](auto& b) { put_u32(b, 9, 0); }));
  KCD2S_CHECK(!patched([](auto& b) { b[13] = 2; }));
  KCD2S_CHECK(!patched([](auto& b) { b[13] = 0xff; }));
  // A HitInfo holding a pointer or handle of the sender's process is not portable.
  KCD2S_CHECK(!patched([](auto& b) { b[14 + 40] = 7; }));
  // The encoder refuses the same blows.
  NpcBlow bad = blow;
  bad.npc = 0;
  KCD2S_CHECK(encode_npc_blow(bad).empty());
  bad = blow;
  bad.player = 0;
  KCD2S_CHECK(encode_npc_blow(bad).empty());
}

KCD2S_TEST(npc, engage_message_is_exact_and_rejects_bad_values) {
  NpcEngage engage;
  engage.npc = 0xfffffffeu;
  engage.revision = 0xffffffffu;
  const auto bytes = encode_npc_engage(engage);
  KCD2S_CHECK_EQ(bytes.size(), kNpcEngageBytes);
  KCD2S_CHECK_EQ(bytes[0], kNpcEngageTag);
  NpcEngage decoded;
  KCD2S_CHECK(decode_npc_engage(bytes, decoded) && decoded == engage);
  check_exact_length<NpcEngage>(kcd2s_ctx, bytes, decode_npc_engage);

  auto no_npc = bytes;
  put_u32(no_npc, 1, 0);
  KCD2S_CHECK(!decode_npc_engage(no_npc, decoded));
  auto wrong_tag = bytes;
  wrong_tag[0] = kNpcHitTag;
  KCD2S_CHECK(!decode_npc_engage(wrong_tag, decoded));
}

KCD2S_TEST(npc, intent_done_message_is_exact_and_rejects_bad_values) {
  NpcIntentDone done;
  done.npc = 0xfffffffeu;
  done.epoch = 0xffffffffu;
  done.sequence = 0x01020304u;
  done.status = kNpcIntentFailed;
  const auto bytes = encode_npc_intent_done(done);
  KCD2S_CHECK_EQ(bytes.size(), kNpcIntentDoneBytes);
  KCD2S_CHECK_EQ(bytes[0], kNpcIntentDoneTag);
  // Little-endian sequence, status last.
  KCD2S_CHECK_EQ(bytes[9], std::uint8_t(0x04));
  KCD2S_CHECK_EQ(bytes[12], std::uint8_t(0x01));
  KCD2S_CHECK_EQ(bytes[13], kNpcIntentFailed);
  NpcIntentDone decoded;
  KCD2S_CHECK(decode_npc_intent_done(bytes, decoded) && decoded == done);
  check_exact_length<NpcIntentDone>(kcd2s_ctx, bytes, decode_npc_intent_done);

  auto no_npc = bytes;
  put_u32(no_npc, 1, 0);
  KCD2S_CHECK(!decode_npc_intent_done(no_npc, decoded));
  auto unknown_status = bytes;
  unknown_status[13] = 3;
  KCD2S_CHECK(!decode_npc_intent_done(unknown_status, decoded));
  auto wrong_tag = bytes;
  wrong_tag[0] = kNpcEngageTag;
  KCD2S_CHECK(!decode_npc_intent_done(wrong_tag, decoded));

  NpcIntentDone bad = done;
  bad.npc = 0;
  KCD2S_CHECK(encode_npc_intent_done(bad).empty());
  bad = done;
  bad.status = 3;
  KCD2S_CHECK(encode_npc_intent_done(bad).empty());
}

KCD2S_TEST(npc, intent_done_is_taken_from_the_current_simulator_only) {
  NpcWorld w;
  w.place(0, 10);
  w.place(1, 30);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  const auto epoch = [&] {
    return std::uint32_t(std::get<std::int64_t>(*w.server.world().read(w.npc, NpcEpoch)));
  };
  KCD2S_CHECK(w.server.take_npc_intent_dones().empty());

  // The simulator's report in the current epoch reaches the resource host, whole.
  KCD2S_CHECK(w.peers[0].client->send_npc_intent_done(w.npc, epoch(), 7, kNpcIntentBlocked));
  w.pump(6);
  auto taken = w.server.take_npc_intent_dones();
  KCD2S_CHECK_EQ(taken.size(), std::size_t(1));
  if (!taken.empty()) {
    KCD2S_CHECK_EQ(taken[0].npc, w.npc);
    KCD2S_CHECK_EQ(taken[0].epoch, epoch());
    KCD2S_CHECK_EQ(taken[0].sequence, std::uint32_t(7));
    KCD2S_CHECK_EQ(taken[0].status, kNpcIntentBlocked);
  }

  // Another player's client is not the simulator; an old epoch is stale.
  KCD2S_CHECK(w.peers[1].client->send_npc_intent_done(w.npc, epoch(), 8, kNpcIntentReached));
  KCD2S_CHECK(w.peers[0].client->send_npc_intent_done(w.npc, epoch() - 1, 9, kNpcIntentReached));
  w.pump(6);
  KCD2S_CHECK(w.server.take_npc_intent_dones().empty());

  // A value the message cannot carry is not sent at all.
  KCD2S_CHECK(!w.peers[0].client->send_npc_intent_done(w.npc, epoch(), 1, 3));
  KCD2S_CHECK(!w.peers[0].client->send_npc_intent_done(w.npc + 1000, epoch(), 1, 0));

  // After a handoff the new simulator is heard (in its new epoch) and the old one is not.
  w.place(0, 40);
  w.place(1, 0);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
  KCD2S_CHECK(w.peers[0].client->send_npc_intent_done(w.npc, epoch(), 10, kNpcIntentReached));
  w.pump(6);
  KCD2S_CHECK(w.server.take_npc_intent_dones().empty());
  KCD2S_CHECK(w.peers[1].client->send_npc_intent_done(w.npc, epoch(), 11, kNpcIntentReached));
  w.pump(6);
  KCD2S_CHECK_EQ(w.server.take_npc_intent_dones().size(), std::size_t(1));
}

KCD2S_TEST(npc, intent_done_is_limited_per_npc_and_refused_for_a_dead_npc) {
  NpcWorld w;
  w.place(0, 10);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  const auto epoch = std::uint32_t(std::get<std::int64_t>(*w.server.world().read(w.npc, NpcEpoch)));
  // At most kMaxNpcIntentDonesPerSecond in a second.
  for (unsigned i = 0; i < kMaxNpcIntentDonesPerSecond + 3; ++i) {
    KCD2S_CHECK(w.peers[0].client->send_npc_intent_done(w.npc, epoch, i, kNpcIntentReached));
  }
  w.pump(6);
  KCD2S_CHECK_EQ(w.server.take_npc_intent_dones().size(), std::size_t(kMaxNpcIntentDonesPerSecond));
  // The next second takes them again.
  w.pump(70);
  KCD2S_CHECK(w.peers[0].client->send_npc_intent_done(w.npc, epoch, 100, kNpcIntentReached));
  w.pump(6);
  KCD2S_CHECK_EQ(w.server.take_npc_intent_dones().size(), std::size_t(1));

  // A dead NPC walks no order.
  KCD2S_CHECK(w.server.set_npc_health(w.npc, 0, false));
  w.pump(10);
  KCD2S_CHECK(w.peers[0].client->send_npc_intent_done(w.npc, epoch, 101, kNpcIntentReached));
  w.pump(6);
  KCD2S_CHECK(w.server.take_npc_intent_dones().empty());
}

KCD2S_TEST(npc, intent_text_is_bounded_and_replicated) {
  KCD2S_CHECK(valid_npc_intent(""));
  KCD2S_CHECK(valid_npc_intent(std::string(kNpcIntentMax, 'a')));
  KCD2S_CHECK(!valid_npc_intent(std::string(kNpcIntentMax + 1, 'a')));
  KCD2S_CHECK(!valid_npc_intent(std::string(kNpcIntentMax * 2, 'a')));
  KCD2S_CHECK(valid_npc_intent("1|patrol|walk|once|1.5|0|auto|1,2,3;4,5,6"));
  KCD2S_CHECK(valid_npc_intent("1|lookAt|point|1|2|3 \xc4\x8d"));
  for (const char control : {'\0', '\t', '\n', '\r', '\x1f', '\x7f'}) {
    KCD2S_CHECK(!valid_npc_intent(std::string("1|moveTo|") + control + "x"));
  }

  NpcWorld w;
  w.place(0, 10);
  w.pump(30);
  // The longest order is taken, and every client that holds the NPC sees it whole.
  const std::string longest = "1|patrol|" + std::string(kNpcIntentMax - 9, 'p');
  KCD2S_CHECK_EQ(longest.size(), kNpcIntentMax);
  KCD2S_CHECK(w.server.set_npc_intent(w.npc, longest));
  w.pump(30);
  for (auto& peer : w.peers) {
    const auto* replica = peer.client->mirror().get(w.npc);
    KCD2S_CHECK(replica != nullptr);
    if (replica) {
      KCD2S_CHECK(std::get<std::string>(replica->fields[NpcIntent]) == longest);
    }
  }
  // One byte more is refused and the order stays.
  KCD2S_CHECK(!w.server.set_npc_intent(w.npc, longest + "p"));
  KCD2S_CHECK(std::get<std::string>(*w.server.world().read(w.npc, NpcIntent)) == longest);
}

KCD2S_TEST(npc, a_blow_hit_or_lock_on_for_an_npc_that_is_gone_is_dropped) {
  NpcWorld w;
  w.place(0, 2);
  w.place(1, 5);
  w.place(2, 400);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  // The server removes the NPC while the messages about it are on their way: the second player's
  // client still holds it when it sends them.
  w.server.despawn(w.npc);
  NpcBlow blow;
  blow.npc = w.npc;
  blow.player = w.peers[1].player;
  KCD2S_CHECK(w.peers[1].client->send_npc_blow(blow));
  NpcHit hit;
  hit.npc = w.npc;
  hit.attacker = w.peers[1].player;
  hit.amount = 5;
  hit.reason = 1;
  KCD2S_CHECK(w.peers[1].client->send_npc_hit(hit));
  KCD2S_CHECK(w.peers[1].client->send_npc_engage(w.npc));
  w.pump(30);
  KCD2S_CHECK(w.peers[0].client->take_npc_blows().empty());
  KCD2S_CHECK(w.server.take_npc_hits().empty());
}

KCD2S_TEST(npc, a_dead_npc_takes_no_blow_and_no_fight_in_either_direction) {
  NpcWorld w;
  w.place(0, 10);
  w.place(1, 30);
  w.place(2, 50);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  KCD2S_CHECK(w.server.set_npc_health(w.npc, 0, false));
  w.pump(10);
  const auto send = [&](unsigned peer, core::EntityId player, NpcBlowDirection direction) {
    NpcBlow blow;
    blow.npc = w.npc;
    blow.player = player;
    blow.direction = direction;
    KCD2S_CHECK(w.peers[peer].client->send_npc_blow(blow));
    w.pump(6);
  };
  // The simulator's blow on a player and a player's blow on the NPC, both dropped.
  send(0, w.peers[1].player, NpcBlowDirection::NpcStruckPlayer);
  send(1, w.peers[1].player, NpcBlowDirection::PlayerStruckNpc);
  for (auto& peer : w.peers) {
    KCD2S_CHECK(peer.client->take_npc_blows().empty());
  }
  // Neither made a fighter: once the NPC lives again the nearest player keeps it.
  KCD2S_CHECK(w.server.set_npc_health(w.npc, 100, true));
  w.pump(60);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  // And alive, the same blow is taken.
  send(1, w.peers[1].player, NpcBlowDirection::PlayerStruckNpc);
  KCD2S_CHECK_EQ(w.peers[0].client->take_npc_blows().size(), std::size_t(1));
}

KCD2S_TEST(npc, engage_is_refused_beyond_range_and_above_the_rate) {
  NpcWorld w;
  w.place(0, 2);
  w.place(1, kNpcEngageRange + 0.5f);
  w.place(2, 400);
  w.pump(30);
  const auto simulator_of = [&](core::EntityId npc) {
    return core::EntityId(std::get<std::int64_t>(*w.server.world().read(npc, NpcSimulator)));
  };
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  // Half a metre beyond the range: refused. Exactly at the range: taken.
  KCD2S_CHECK(w.peers[1].client->send_npc_engage(w.npc));
  w.pump(20);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  w.place(1, kNpcEngageRange);
  w.pump(4);
  KCD2S_CHECK(w.peers[1].client->send_npc_engage(w.npc));
  w.pump(20);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);

  // At most kMaxNpcEngagesPerSecond a second from one client, over all NPCs: after that many
  // locks on the first NPC, the lock on a second one in the same second is dropped.
  NpcSpawn spawn;
  spawn.name = "Jan";
  const core::EntityId other = w.server.spawn_npc(spawn);
  w.pump(30);
  KCD2S_CHECK_EQ(simulator_of(other), w.peers[0].player);
  w.place(1, 4);
  w.pump(60);
  for (unsigned i = 0; i < kMaxNpcEngagesPerSecond; ++i) {
    KCD2S_CHECK(w.peers[1].client->send_npc_engage(w.npc));
  }
  KCD2S_CHECK(w.peers[1].client->send_npc_engage(other));
  w.pump(20);
  KCD2S_CHECK_EQ(simulator_of(w.npc), w.peers[1].player);
  KCD2S_CHECK_EQ(simulator_of(other), w.peers[0].player);
  // The next second the same lock on is taken.
  w.pump(60);
  KCD2S_CHECK(w.peers[1].client->send_npc_engage(other));
  w.pump(20);
  KCD2S_CHECK_EQ(simulator_of(other), w.peers[1].player);
}

KCD2S_TEST(npc, the_fighter_keeps_the_npc_for_the_fight_seconds_then_the_nearest_takes_it_back) {
  NpcWorld w;
  w.place(0, 2);
  w.place(1, 5);
  w.place(2, 400);
  w.pump(30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
  KCD2S_CHECK(w.peers[1].client->send_npc_engage(w.npc));
  w.pump(20);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
  // The fighter falls back to 40 m, 38 m behind the nearest player; the fight still holds the NPC.
  w.place(1, 40);
  w.pump(int(kNpcFightSeconds * 60) - 120);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
  // The nearest player's own lock-on does not take a running fight over.
  KCD2S_CHECK(w.peers[0].client->send_npc_engage(w.npc));
  w.pump(20);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[1].player);
  // Past the fight seconds after the last blow, the nearest player has the NPC again.
  w.pump(int(kNpcFightSeconds * 60) + 30);
  KCD2S_CHECK_EQ(w.simulator(), w.peers[0].player);
}

KCD2S_TEST(npc, a_bark_travels_with_its_npc_text_and_localization) {
  using namespace kcd2s::integration;
  NpcBark bark;
  bark.npc = 4114;
  bark.localize = true;
  bark.text = "bark_bandit_attack_03";
  NpcBark back;
  KCD2S_CHECK(decode_npc_bark(encode_npc_bark(bark), back) && back == bark);
  // No NPC, no text, control characters or too long: it does not travel.
  NpcBark none = bark;
  none.npc = 0;
  KCD2S_CHECK(encode_npc_bark(none).empty());
  NpcBark empty = bark;
  empty.text.clear();
  KCD2S_CHECK(encode_npc_bark(empty).empty());
  NpcBark control = bark;
  control.text = "line\nbreak";
  KCD2S_CHECK(encode_npc_bark(control).empty());
  NpcBark longer = bark;
  longer.text.assign(kMaxNpcBarkBytes + 1, 'a');
  KCD2S_CHECK(encode_npc_bark(longer).empty());
}

KCD2S_TEST(npc, a_bark_carries_the_voice_file_the_game_played_with_it) {
  using namespace kcd2s::integration;
  NpcBark bark;
  bark.npc = 4114;
  bark.localize = true;
  bark.text = "bark_bandit_attack_03";
  bark.voice = "Localization/dialog/kutnohorsko/drak/fejk_bark_alchymist_delejte_o1Hp";
  bark.purpose = 0x9a3c11d2u;
  NpcBark back;
  KCD2S_CHECK(decode_npc_bark(encode_npc_bark(bark), back) && back == bark);
  // A path out of the game folder, an absolute one, odd characters or too long: no bark.
  for (const char* voice : {"../outside", "/root/line", "C:/line", "line two"}) {
    NpcBark wrong = bark;
    wrong.voice = voice;
    KCD2S_CHECK(encode_npc_bark(wrong).empty());
  }
  NpcBark longer = bark;
  longer.voice.assign(kMaxNpcVoiceBytes + 1, 'a');
  KCD2S_CHECK(encode_npc_bark(longer).empty());
  // A line the game said without a bubble travels with its voice alone.
  NpcBark spoken = bark;
  spoken.text.clear();
  KCD2S_CHECK(decode_npc_bark(encode_npc_bark(spoken), back) && back == spoken);
  spoken.voice.clear();
  KCD2S_CHECK(encode_npc_bark(spoken).empty());
  // A message cut short of its voice does not decode.
  auto bytes = encode_npc_bark(bark);
  bytes.resize(bytes.size() - 4);
  KCD2S_CHECK(!decode_npc_bark(bytes, back));
}

KCD2S_TEST(npc, the_people_around_a_fight_go_to_the_game_that_has_it) {
  NpcWorld w;
  w.place(0, 10);
  w.place(1, 30);
  w.place(2, 400);
  const auto simulator_of = [&](core::EntityId npc) {
    return core::EntityId(std::get<std::int64_t>(*w.server.world().read(npc, NpcSimulator)));
  };
  // A witness beside the NPC the player will fight, and a person far from it.
  NpcSpawn spawn;
  spawn.name = "Marta";
  const core::EntityId witness = w.server.spawn_npc(spawn);
  spawn.name = "Ota";
  const core::EntityId far = w.server.spawn_npc(spawn);
  KCD2S_CHECK(w.server.teleport_npc(witness, {2, 0, 0}, facing()));
  KCD2S_CHECK(w.server.teleport_npc(far, {-80, 0, 0}, facing()));
  w.pump(60);
  KCD2S_CHECK_EQ(simulator_of(w.npc), w.peers[0].player);
  KCD2S_CHECK_EQ(simulator_of(witness), w.peers[0].player);
  KCD2S_CHECK_EQ(simulator_of(far), w.peers[0].player);
  // The farther player strikes the NPC: the NPC and the witness beside it go to that player's
  // game, the far person does not.
  NpcHit hit;
  hit.npc = w.npc;
  hit.attacker = w.peers[1].player;
  hit.amount = 1;
  KCD2S_CHECK(w.peers[1].client->send_npc_hit(hit));
  w.pump(30);
  KCD2S_CHECK_EQ(simulator_of(w.npc), w.peers[1].player);
  KCD2S_CHECK_EQ(simulator_of(witness), w.peers[1].player);
  KCD2S_CHECK_EQ(simulator_of(far), w.peers[0].player);
  // Past the fight, the witness stays with the fight's game until the witness seconds are over.
  w.pump(int(kNpcFightSeconds * 60) + 30);
  KCD2S_CHECK_EQ(simulator_of(witness), w.peers[1].player);
  w.pump(int((kNpcWitnessSeconds - kNpcFightSeconds) * 60) + 30);
  KCD2S_CHECK_EQ(simulator_of(witness), w.peers[0].player);
  KCD2S_CHECK_EQ(simulator_of(w.npc), w.peers[0].player);
}
