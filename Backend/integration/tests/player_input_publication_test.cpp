#include "kcd2s/integration/player_input_publication.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/testkit.hpp"
#include "kcd2s/wire/packet.hpp"

using namespace kcd2s;
using namespace kcd2s::integration;

namespace {
PlayerInputFields idle_fields() {
  return {{local::MotionClock, std::int64_t{1000}},
          {local::Position, core::Vec3{1, 2, 3}},
          {local::Rotation, core::Quat{}},
          {local::Velocity, core::Vec3{}},
          {local::MotionVelocity, core::Vec3{}},
          {local::PoseValid, true},
          {local::MotionValid, true},
          {local::MotionFlags, std::int64_t{1}},
          {local::Posture, posture_field({})},
          {local::AimEnabled, false},
          {local::HeldProp, std::int64_t{0}},
          {local::WeaponState, std::int64_t{0}},
          {local::NativeRiderState, std::string{}},
          {local::RagdollPose, std::string{}},
          {local::Seats, std::string{}},
          {local::Carry, std::string{}},
          {local::StationAction, std::string{}},
          {local::AnimationProps, std::string{}},
          {local::Animations, local::encode_animation({})},
          {local::Outfit, std::string{"whole appearance payload"}}};
}

void set(PlayerInputFields& fields, core::FieldId id, replication::FieldValue value) {
  for (auto& field : fields) {
    if (field.first == id) {
      field.second = std::move(value);
      return;
    }
  }
  fields.emplace_back(id, std::move(value));
}

std::vector<std::uint8_t> encode(const replication::TypeDef& type, core::EntityId entity,
                                 std::uint8_t epoch, const PlayerInputFields& fields) {
  core::BitWriter writer;
  writer.write_varuint(entity);
  writer.write_bits(epoch, 8);
  writer.write_bits(fields.size(), 16);
  for (const auto& [id, value] : fields) {
    writer.write_bits(id, 16);
    replication::encode_field(writer, type.fields[id], value);
  }
  return writer.bytes();
}

struct RawOwner {
  replication::SchemaRegistry schema = local::player_schema();
  Server server{schema};
  decltype(transport::make_loopback_pair()) links = transport::make_loopback_pair();
  transport::Connection connection{links.second.get()};
  core::EntityId entity = local::spawn_player(server.world());
  core::PeerId owner = server.add_client(links.first.get(), entity, 1000);

  RawOwner() {
    server.set_owner(entity, owner);
  }
  void send_bytes(const std::vector<std::uint8_t>& bytes, double now) {
    connection.queue(static_cast<std::uint8_t>(wire::Channel::Upstream), bytes);
    connection.flush(now);
    server.world().set_tick(server.world().tick() + 1);
    server.tick(now);
    connection.receive(now);
  }
  void send(const PlayerInputFields& fields, double now, std::uint8_t epoch = 0) {
    send_bytes(encode(*schema.get("NetworkPlayer"), entity, epoch, fields), now);
  }
  std::int64_t clock() const {
    return std::get<std::int64_t>(*server.world().read(entity, local::MotionClock));
  }
};

struct DroppableLink : transport::IDatagramLink {
  transport::IDatagramLink* link = nullptr;
  bool drop = false;
  void send(const std::uint8_t* data, std::size_t length) override {
    if (!drop) {
      link->send(data, length);
    }
  }
  bool recv(transport::Datagram& data) override {
    return link->recv(data);
  }
  void poll() override {
    link->poll();
  }
};

struct OwnerPeer {
  replication::SchemaRegistry schema = local::player_schema();
  Server server{schema};
  decltype(transport::make_loopback_pair()) links = transport::make_loopback_pair();
  DroppableLink link;
  std::unique_ptr<ClientPeer> peer;
  core::EntityId entity = local::spawn_player(server.world());
  core::PeerId owner = server.add_client(links.first.get(), entity, 1000);

  OwnerPeer() {
    link.link = links.second.get();
    peer = std::make_unique<ClientPeer>(schema, &link);
    server.set_owner(entity, owner);
    for (unsigned i = 0; i < 4; ++i) {
      server.world().set_tick(i + 1);
      server.tick(0);
      peer->tick(0);
    }
  }
  void send(const PlayerInputFields& fields, double now, bool drop = false) {
    peer->tick(now);
    peer->send_input(entity, fields);
    link.drop = drop;
    peer->tick(now);
    link.drop = false;
    server.world().set_tick(server.world().tick() + 1);
    server.tick(now);
  }
  std::int64_t clock() const {
    return std::get<std::int64_t>(*server.world().read(entity, local::MotionClock));
  }
};
}  // namespace

KCD2S_TEST(player_input_publication, idle_full_packet_bandwidth_and_bounded_refresh) {
  const auto schema = local::player_schema();
  const auto& type = *schema.get("NetworkPlayer");
  auto fields = idle_fields();
  PlayerInputPublication publication;
  unsigned published = 0;
  std::size_t baseline_bytes = 0, published_bytes = 0;
  double last = -1;
  for (unsigned frame = 0; frame <= 600; ++frame) {
    const double now = frame / 60.0;
    set(fields, local::MotionClock, std::int64_t(1000 + frame * 17));
    const auto bytes = encode(type, 1, 0, fields).size();
    baseline_bytes += bytes;
    if (publication.publish(type, fields, {1, 2, 0, 1}, now)) {
      if (last >= 0) {
        KCD2S_CHECK(now - last <= kPlayerIdlePublicationSeconds + 1e-8);
      }
      last = now;
      ++published;
      published_bytes += bytes;
    }
  }
  KCD2S_CHECK_EQ(published, 101u);
  KCD2S_CHECK(published_bytes * 5 < baseline_bytes);
}

KCD2S_TEST(player_input_publication, moving_combat_missing_activity_and_scope_bypass) {
  const auto schema = local::player_schema();
  const auto& type = *schema.get("NetworkPlayer");
  const PlayerInputScope scope{1, 2, 3, 4};
  auto idle = idle_fields();
  for (const auto& change :
       PlayerInputFields{{local::Velocity, core::Vec3{0.000001f, 0, 0}},
                         {local::MotionVelocity, core::Vec3{0, 1, 0}},
                         {local::Posture, posture_field(PlayerPosture{false, false, true})},
                         {local::AimEnabled, true},
                         {local::WeaponState, std::int64_t{1}},
                         {local::MotionFlags, std::int64_t{2}},
                         {local::Seats, std::string{"active"}},
                         {local::StationAction, std::string{"active"}}}) {
    auto active = idle;
    set(active, change.first, change.second);
    PlayerInputPublication publication;
    KCD2S_CHECK(publication.publish(type, active, scope, 0));
    KCD2S_CHECK(publication.publish(type, active, scope, .001));
  }
  for (std::size_t i = 3; i < 18; ++i) {
    auto incomplete = idle;
    incomplete.erase(incomplete.begin() + i);
    PlayerInputPublication publication;
    KCD2S_CHECK(publication.publish(type, incomplete, scope, 0));
    KCD2S_CHECK(publication.publish(type, incomplete, scope, .001));
  }
  PlayerInputPublication publication;
  KCD2S_CHECK(publication.publish(type, idle, scope, 0));
  KCD2S_CHECK(!publication.publish(type, idle, scope, .001));
  for (const auto next : {PlayerInputScope{5, 2, 3, 4}, PlayerInputScope{5, 6, 3, 4},
                          PlayerInputScope{5, 6, 7, 4}, PlayerInputScope{5, 6, 7, 8}}) {
    KCD2S_CHECK(publication.publish(type, idle, next, .002));
  }
  KCD2S_CHECK(publication.publish(type, idle, scope, -.1));
}

KCD2S_TEST(player_input_publication, changes_are_exact_and_never_wait_for_refresh) {
  const auto schema = local::player_schema();
  const auto& type = *schema.get("NetworkPlayer");
  const PlayerInputScope scope{1, 2, 0, 1};
  auto fields = idle_fields();
  PlayerInputPublication publication;
  KCD2S_CHECK(publication.publish(type, fields, scope, 0));
  set(fields, local::MotionClock, std::int64_t{1001});
  KCD2S_CHECK(!publication.publish(type, fields, scope, .001));
  set(fields, local::Position, core::Vec3{1, 2, 3.00001f});
  KCD2S_CHECK(publication.publish(type, fields, scope, .002));
  set(fields, local::Rotation, core::Quat{0, 0, .001f, 1});
  KCD2S_CHECK(publication.publish(type, fields, scope, .003));
  local::AnimationState animations{};
  animations[0].sequence = 1;
  animations[0].clip = 8;
  set(fields, local::Animations, local::encode_animation(animations));
  KCD2S_CHECK(publication.publish(type, fields, scope, .004));
  animations[0].params.normalized_time = .25f;
  set(fields, local::Animations, local::encode_animation(animations));
  KCD2S_CHECK(!publication.publish(type, fields, scope, .005));
  set(fields, local::Outfit, std::string{"changed clothing"});
  KCD2S_CHECK(publication.publish(type, fields, scope, .006));
  KCD2S_CHECK(publication.publish(type, fields, scope, .007, true));
}

KCD2S_TEST(player_input_publication, sender_lost_final_stop_recovers_as_a_complete_packet) {
  OwnerPeer pair;
  auto fields = idle_fields();
  set(fields, local::Velocity, core::Vec3{1, 0, 0});
  pair.send(fields, 0);
  set(fields, local::Velocity, core::Vec3{});
  set(fields, local::Outfit, std::string{"new clothing in lost stop"});
  set(fields, local::MotionClock, std::int64_t{1010});
  pair.send(fields, .01, true);
  KCD2S_CHECK_EQ(pair.clock(), 1000);
  for (unsigned i = 1; i < 6; ++i) {
    set(fields, local::MotionClock, std::int64_t(1010 + i * 16));
    pair.send(fields, .01 + i / 60.0);
    KCD2S_CHECK_EQ(pair.clock(), 1000);
  }
  set(fields, local::MotionClock, std::int64_t{1110});
  pair.send(fields, .11);
  KCD2S_CHECK_EQ(pair.clock(), 1110);
  KCD2S_CHECK((std::get<core::Vec3>(*pair.server.world().read(pair.entity, local::Velocity)) ==
               core::Vec3{}));
  KCD2S_CHECK_EQ(std::get<std::string>(*pair.server.world().read(pair.entity, local::Outfit)),
                 std::string("new clothing in lost stop"));
}

KCD2S_TEST(player_input_publication, server_holds_idle_clock_but_publishes_changes_atomically) {
  RawOwner pair;
  auto fields = idle_fields();
  pair.send(fields, 0);
  set(fields, local::MotionClock, std::int64_t{1001});
  pair.send(fields, .001);
  KCD2S_CHECK_EQ(pair.clock(), 1000);
  // Clock deliberately precedes position on the wire: the decision uses the entire packet.
  set(fields, local::Position, core::Vec3{9, 8, 7});
  set(fields, local::MotionClock, std::int64_t{1002});
  pair.send(fields, .002);
  KCD2S_CHECK_EQ(pair.clock(), 1002);
  KCD2S_CHECK((pair.server.world().slot(pair.entity)->position == core::Vec3{9, 8, 7}));
  pair.server.world().write(pair.entity, local::Position, core::Vec3{4, 5, 6});
  set(fields, local::MotionClock, std::int64_t{1003});
  pair.send(fields, .003);
  KCD2S_CHECK_EQ(pair.clock(), 1003);  // a server edit cannot pair the owner pose with an old clock
  set(fields, local::MotionClock, std::int64_t{1004});
  pair.send(fields, .004);
  KCD2S_CHECK_EQ(pair.clock(), 1003);
  pair.send(fields, .103);
  KCD2S_CHECK_EQ(pair.clock(), 1003);
  pair.send(fields, 1.003);
  KCD2S_CHECK_EQ(pair.clock(), 1004);
}

KCD2S_TEST(player_input_publication,
           sender_refresh_survives_arrival_jitter_and_one_lost_heartbeat) {
  RawOwner pair;
  auto fields = idle_fields();
  pair.send(fields, .011);
  set(fields, local::MotionClock, std::int64_t{1100});
  pair.send(fields, .110);
  KCD2S_CHECK_EQ(pair.clock(), 1000);  // sender10Hz liveness need not fan out to all observers
  set(fields, local::MotionClock, std::int64_t{2000});
  pair.send(fields, 1.010);  // 999 ms arrival gap; owner time confirms the1s refresh
  KCD2S_CHECK_EQ(pair.clock(), 2000);
  set(fields, local::MotionClock, std::int64_t{3000});
  pair.send(fields, 2.009);
  KCD2S_CHECK_EQ(pair.clock(), 3000);
  // The4000 packet is lost. The following complete refresh must still publish on arrival.
  set(fields, local::MotionClock, std::int64_t{5000});
  pair.send(fields, 4.008);
  KCD2S_CHECK_EQ(pair.clock(), 5000);
  // Owner time is not an ordering fence; a reset still refreshes on the wall-clock bound.
  set(fields, local::MotionClock, std::int64_t{1});
  pair.send(fields, 5.008);
  KCD2S_CHECK_EQ(pair.clock(), 1);
}

KCD2S_TEST(player_input_publication, idle_sender10hz_server1hz_and_active_updates_stay_immediate) {
  const auto schema = local::player_schema();
  const auto& type = *schema.get("NetworkPlayer");
  auto fields = idle_fields();
  local::AnimationState native_animation{};
  native_animation[0].sequence = 1;
  native_animation[0].clip = 8;
  PlayerInputPublication sender, server;
  unsigned sent = 0, relayed = 0;
  for (unsigned frame = 0; frame <= 600; ++frame) {
    const double now = frame / 60.0;
    set(fields, local::MotionClock, std::int64_t(1000 + std::llround(now * 1000)));
    native_animation[0].params.normalized_time = float(frame % 60) / 60;
    set(fields, local::Animations, local::encode_animation(native_animation));
    set(fields, local::WeaponAnimations, local::encode_animation(native_animation));
    if (sender.publish(type, fields, {1, 2, 0, 1}, now)) {
      ++sent;
      if (server.publish(type, fields, {1, 2, 0, 1}, now, false, true,
                         kPlayerIdleServerPublicationSeconds)) {
        ++relayed;
      }
    }
  }
  KCD2S_CHECK_EQ(sent, 101u);
  KCD2S_CHECK_EQ(relayed, 11u);
  set(fields, local::Animations, std::string{"changed action"});
  KCD2S_CHECK(server.publish(type, fields, {1, 2, 0, 1}, 10.001, false, true, 1));
  set(fields, local::Posture, posture_field(PlayerPosture{false, false, true}));
  KCD2S_CHECK(server.publish(type, fields, {1, 2, 0, 1}, 10.002, false, true, 1));
  KCD2S_CHECK(server.publish(type, fields, {1, 2, 0, 1}, 10.003, false, true, 1));
}

KCD2S_TEST(player_input_publication,
           idle_animation_progress_is_not_a_new_start_or_parameter_change) {
  const auto schema = local::player_schema();
  const auto& type = *schema.get("NetworkPlayer");
  local::AnimationState initial{};
  initial[0].sequence = 8;
  initial[0].clip = 25;
  const auto first = local::encode_animation(initial);
  auto progressed = initial;
  progressed[0].params.normalized_time = .75f;
  for (const auto field : {local::Animations, local::WeaponAnimations}) {
    KCD2S_CHECK(
        same_player_idle_field(type.fields[field], first, local::encode_animation(progressed)));
    std::vector<local::AnimationState> changes;
    auto changed = progressed;
    ++changed[0].sequence;
    changes.push_back(changed);
    changed = progressed;
    changed[0].clip = -1;
    changes.push_back(changed);
    changed = progressed;
    changed[0].params.transition = -0.0f;  // Exact non-progress parameter bits survive.
    changes.push_back(changed);
    changed = progressed;
    changed[0].params.speed = 2;
    changes.push_back(changed);
    changed = progressed;
    changed[0].params.weight = .5f;
    changes.push_back(changed);
    changed = progressed;
    ++changed[0].params.layer;
    changes.push_back(changed);
    changed = progressed;
    ++changed[0].params.token;
    changes.push_back(changed);
    changed = progressed;
    ++changed[0].params.flags;
    changes.push_back(changed);
    changed = progressed;
    ++changed[0].params.user_token;
    changes.push_back(changed);
    for (unsigned i = 0; i < 8; ++i) {
      changed = progressed;
      changed[0].params.user_data[i] = 1;
      changes.push_back(changed);
    }
    for (const auto& state : changes) {
      KCD2S_CHECK(
          !same_player_idle_field(type.fields[field], first, local::encode_animation(state)));
    }
    KCD2S_CHECK(!same_player_idle_field(type.fields[field], first, std::string("malformed")));
  }
  auto fields = idle_fields();
  set(fields, local::Animations, first);
  PlayerInputPublication sender;
  KCD2S_CHECK(sender.publish(type, fields, {1, 2, 0, 1}, 0));
  set(fields, local::Animations, local::encode_animation(progressed));
  KCD2S_CHECK(!sender.publish(type, fields, {1, 2, 0, 1}, .01));
  KCD2S_CHECK(sender.publish(type, fields, {1, 2, 0, 1}, .1));
  set(fields, local::Posture, posture_field(PlayerPosture{false, false, true}));
  KCD2S_CHECK(sender.publish(type, fields, {1, 2, 0, 1}, .101));
  progressed[0].params.normalized_time = .8f;
  set(fields, local::Animations, local::encode_animation(progressed));
  KCD2S_CHECK(sender.publish(type, fields, {1, 2, 0, 1}, .102));
}

KCD2S_TEST(player_input_publication,
           idle_progress_does_not_dirty_server_animation_between_refreshes) {
  RawOwner pair;
  auto fields = idle_fields();
  local::AnimationState animation{};
  animation[0].sequence = 1;
  animation[0].clip = 8;
  const auto initial = local::encode_animation(animation);
  set(fields, local::Animations, initial);
  pair.send(fields, 0);
  animation[0].params.normalized_time = .25f;
  const auto progressed = local::encode_animation(animation);
  set(fields, local::Animations, progressed);
  set(fields, local::MotionClock, std::int64_t{1100});
  pair.send(fields, .1);
  KCD2S_CHECK_EQ(pair.clock(), 1000);
  KCD2S_CHECK_EQ(std::get<std::string>(*pair.server.world().read(pair.entity, local::Animations)),
                 initial);
  set(fields, local::MotionClock, std::int64_t{2000});
  pair.send(fields, 1);
  KCD2S_CHECK_EQ(pair.clock(), 2000);
  KCD2S_CHECK_EQ(std::get<std::string>(*pair.server.world().read(pair.entity, local::Animations)),
                 progressed);
  ++animation[0].sequence;
  animation[0].clip = 9;
  const auto next = local::encode_animation(animation);
  set(fields, local::Animations, next);
  set(fields, local::MotionClock, std::int64_t{2001});
  pair.send(fields, 1.001);
  KCD2S_CHECK_EQ(pair.clock(), 2001);
  KCD2S_CHECK_EQ(std::get<std::string>(*pair.server.world().read(pair.entity, local::Animations)),
                 next);
}

KCD2S_TEST(player_input_publication,
           server_rejects_malformed_tail_and_stale_authority_without_prefix) {
  RawOwner pair;
  auto fields = idle_fields();
  pair.send(fields, 0);
  set(fields, local::MotionClock, std::int64_t{2000});
  set(fields, local::Position, core::Vec3{9, 9, 9});
  auto bytes = encode(*pair.schema.get("NetworkPlayer"), pair.entity, 0, fields);
  bytes.pop_back();
  pair.send_bytes(bytes, .01);
  KCD2S_CHECK_EQ(pair.clock(), 1000);
  KCD2S_CHECK((pair.server.world().slot(pair.entity)->position == core::Vec3{1, 2, 3}));
  pair.send(fields, .02, 1);
  KCD2S_CHECK_EQ(pair.clock(), 1000);
  core::BitWriter unknown;
  unknown.write_varuint(pair.entity);
  unknown.write_bits(0, 8);
  unknown.write_bits(2, 16);
  unknown.write_bits(local::MotionClock, 16);
  replication::encode_field(unknown, pair.schema.get("NetworkPlayer")->fields[local::MotionClock],
                            std::int64_t{3000});
  unknown.write_bits(65535, 16);
  pair.send_bytes(unknown.bytes(), .025);
  KCD2S_CHECK_EQ(pair.clock(), 1000);
  pair.server.world().slot(pair.entity)->owner = pair.owner + 1;
  pair.send(fields, .03);
  KCD2S_CHECK_EQ(pair.clock(), 1000);
  pair.server.world().slot(pair.entity)->owner = pair.owner;
  pair.server.world().slot(pair.entity)->stateEpoch = 1;
  pair.send(fields, .04, 1);
  KCD2S_CHECK_EQ(pair.clock(), 2000);
}
