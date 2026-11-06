#include <chrono>
#include <thread>
#include <vector>

#include "kcd2s/testkit.hpp"
#include "kcd2s/voice/control_link.hpp"
#include "kcd2s/voice/voice_tenants.hpp"

using kcd2s::core::Vec3;
using namespace kcd2s::voice;

namespace {

ControlMessage round_trip(const ControlMessage& message, bool& good) {
  const auto decoded = decode_control(encode_control(message));
  good = decoded.has_value();
  return decoded.value_or(ControlMessage{});
}

}  // namespace

KCD2S_TEST(voice_control, every_message_survives_coding) {
  bool good = false;
  ControlMessage join;
  join.kind = ControlKind::Join;
  join.player = 7;
  join.session = 9;
  join.key.fill(3);
  ControlMessage out = round_trip(join, good);
  KCD2S_CHECK(good && out.player == 7 && out.session == 9 && out.key == join.key);

  ControlMessage hello;
  hello.kind = ControlKind::Hello;
  hello.server_id = "server one";
  out = round_trip(hello, good);
  KCD2S_CHECK(good && out.server_id == "server one");

  ControlMessage positions;
  positions.kind = ControlKind::Positions;
  positions.positions.push_back(ControlPosition{4, Vec3{1.0f, 2.0f, 3.0f}, 5});
  out = round_trip(positions, good);
  KCD2S_CHECK(good && out.positions.size() == 1 && out.positions[0].world == 5);

  ControlMessage mute;
  mute.kind = ControlKind::ListenerMute;
  mute.player = 1;
  mute.other = 2;
  mute.flag = true;
  out = round_trip(mute, good);
  KCD2S_CHECK(good && out.other == 2 && out.flag);
}

KCD2S_TEST(voice_control, junk_is_refused) {
  KCD2S_CHECK(!decode_control({}).has_value());
  KCD2S_CHECK(!decode_control({200, 1, 2}).has_value());
  KCD2S_CHECK(!decode_control({std::uint8_t(ControlKind::Join), 1}).has_value());
  auto valid = encode_control(ControlMessage{ControlKind::Enabled});
  valid.push_back(9);
  KCD2S_CHECK(!decode_control(valid).has_value());
  ControlMessage tier;
  tier.kind = ControlKind::Tier;
  tier.tier = 9;
  KCD2S_CHECK(!decode_control(encode_control(tier)).has_value());
}

KCD2S_TEST(voice_control, the_link_needs_the_secret_and_refuses_repeats_and_changes) {
  const ControlRandom game = make_control_random();
  const ControlRandom voice = make_control_random();
  const auto key_of = [&](const char* secret, bool from_game) {
    return control_key(secret, from_game, "one", game, voice);
  };
  ControlSealer sealer(key_of("right secret", true));
  const std::vector<std::uint8_t> plain{1, 2, 3};
  const auto frame = sealer.seal(plain);

  ControlOpener good(key_of("right secret", true));
  KCD2S_CHECK(good.feed(frame.data(), frame.size()));
  const auto opened = good.next();
  KCD2S_CHECK(opened.has_value() && *opened == plain);
  KCD2S_CHECK(!good.feed(frame.data(), frame.size()));

  ControlOpener wrong(key_of("another secret", true));
  KCD2S_CHECK(!wrong.feed(frame.data(), frame.size()));

  ControlOpener reversed(key_of("right secret", false));
  KCD2S_CHECK(!reversed.feed(frame.data(), frame.size()));

  auto changed = frame;
  changed.back() ^= 1;
  ControlOpener strict(key_of("right secret", true));
  KCD2S_CHECK(!strict.feed(changed.data(), changed.size()));

  // A new connection has a new key: what was captured cannot be played to it.
  ControlOpener later(control_key("right secret", true, "one", make_control_random(), voice));
  KCD2S_CHECK(!later.feed(frame.data(), frame.size()));
  // Nor can one server's frame pass as another's.
  ControlOpener other(control_key("right secret", true, "two", game, voice));
  KCD2S_CHECK(!other.feed(frame.data(), frame.size()));
}

KCD2S_TEST(voice_control, the_opening_comes_in_pieces_and_refuses_junk) {
  const ControlRandom random = make_control_random();
  const auto opening = encode_control_opening("server one", random);
  std::string id;
  ControlRandom read{};
  std::size_t used = 0;
  std::vector<std::uint8_t> part(opening.begin(), opening.end() - 1);
  KCD2S_CHECK_EQ(decode_control_opening(part, id, read, used), 0);
  KCD2S_CHECK_EQ(decode_control_opening(opening, id, read, used), 1);
  KCD2S_CHECK(id == "server one");
  KCD2S_CHECK(read == random);
  KCD2S_CHECK_EQ(used, opening.size());
  KCD2S_CHECK_EQ(decode_control_opening({0xFF, 0xFF}, id, read, used), -1);
}

KCD2S_TEST(voice_control, a_frame_may_arrive_in_pieces) {
  const ControlRandom random{};
  const Key key = control_key("s", true, "one", random, random);
  ControlSealer sealer(key);
  const auto one = sealer.seal({1});
  const auto two = sealer.seal({2, 2});
  std::vector<std::uint8_t> stream = one;
  stream.insert(stream.end(), two.begin(), two.end());
  ControlOpener opener(key);
  for (const std::uint8_t byte : stream) {
    KCD2S_CHECK(opener.feed(&byte, 1));
  }
  KCD2S_CHECK(opener.next().has_value());
  KCD2S_CHECK(opener.next().has_value());
  KCD2S_CHECK(!opener.next().has_value());
}

namespace {

struct TenantsRig {
  std::uint64_t now = 1000;
  std::vector<std::pair<Address, std::vector<std::uint8_t>>> sent;
  std::mutex mutex;
  VoiceTenants tenants;

  TenantsRig()
      : tenants(
            RelayConfig{},
            [this](const Address& to, const std::vector<std::uint8_t>& data) {
              std::lock_guard<std::mutex> lock(mutex);
              sent.emplace_back(to, data);
            },
            [this] { return now; }, 7777) {
  }

  std::size_t sent_count() {
    std::lock_guard<std::mutex> lock(mutex);
    return sent.size();
  }

  bool wait_for_sent(std::size_t count) {
    for (int i = 0; i < 400; ++i) {
      if (sent_count() >= count) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }

  // Joins a player on a tenant, as the game server would: the client's key pair, and the key it
  // derives. The relay's thread handles it.
  Key join(const std::string& server, const ControlMessage& welcome, std::uint32_t player,
           std::uint32_t session, float x) {
    const KeyPair client = make_key_pair();
    Key session_key{};
    derive_session_key(client.secret_key, welcome.key, client.public_key, welcome.key, session_key);
    ControlMessage join;
    join.kind = ControlKind::Join;
    join.player = player;
    join.session = session;
    join.key = client.public_key;
    tenants.on_control(server, join);
    ControlMessage where;
    where.kind = ControlKind::Positions;
    where.positions.push_back(ControlPosition{player, Vec3{x, 0.0f, 0.0f}, 0});
    tenants.on_control(server, where);
    return session_key;
  }

  void send(const Key& key, std::uint32_t session, std::uint64_t counter,
            std::vector<std::uint8_t> payload, const Address& from) {
    VoiceHeader header;
    header.tag = kVoiceUpTag;
    header.id = session;
    header.counter = counter;
    std::vector<std::uint8_t> sealed;
    seal_voice(header, key, payload.data(), payload.size(), sealed);
    tenants.offer(from, sealed.data(), sealed.size());
  }
};

}  // namespace

KCD2S_TEST(voice_tenants, a_voice_goes_across_a_tenant_and_stays_inside_it) {
  TenantsRig rig;
  const ControlMessage first = rig.tenants.link_opened("one");
  const ControlMessage second = rig.tenants.link_opened("two");
  KCD2S_CHECK(first.session != second.session);
  KCD2S_CHECK(first.key == second.key);
  KCD2S_CHECK_EQ(first.port, std::uint16_t{7777});
  KCD2S_CHECK_EQ(rig.tenants.tenants(), std::size_t{2});

  const Key talker = rig.join("one", first, 1, first.session + 1, 0.0f);
  const Key listener = rig.join("one", first, 2, first.session + 2, 1.0f);
  const Key stranger = rig.join("two", second, 2, second.session + 2, 1.0f);
  const Address talker_at{1, 1};
  const Address listener_at{2, 2};
  const Address stranger_at{3, 3};
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  rig.send(listener, first.session + 2, 1, {}, listener_at);
  rig.send(stranger, second.session + 2, 1, {}, stranger_at);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  rig.send(talker, first.session + 1, 1, {4, 5}, talker_at);
  KCD2S_CHECK(rig.wait_for_sent(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  std::lock_guard<std::mutex> lock(rig.mutex);
  KCD2S_CHECK_EQ(rig.sent.size(), std::size_t{1});
  if (!rig.sent.empty()) {
    KCD2S_CHECK(rig.sent[0].first == listener_at);
  }
}

KCD2S_TEST(voice_tenants, a_session_outside_the_tenants_range_is_refused) {
  TenantsRig rig;
  const ControlMessage first = rig.tenants.link_opened("one");
  const ControlMessage second = rig.tenants.link_opened("two");
  // Tenant one tries to take a session of tenant two's range.
  const Key taken = rig.join("one", first, 1, second.session + 5, 0.0f);
  rig.join("two", second, 1, second.session + 6, 0.0f);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  rig.send(taken, second.session + 5, 1, {1}, Address{1, 1});
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  KCD2S_CHECK_EQ(rig.sent_count(), std::size_t{0});
}

KCD2S_TEST(voice_tenants, a_lost_link_mutes_after_a_while_and_recovers_by_itself) {
  TenantsRig rig;
  rig.tenants.link_opened("one");
  KCD2S_CHECK(rig.tenants.enabled("one"));
  rig.tenants.link_lost("one");
  rig.now += VoiceTenants::kLinkLostMuteMs - 1;
  rig.tenants.tick();
  KCD2S_CHECK(rig.tenants.enabled("one"));
  rig.now += 2;
  rig.tenants.tick();
  KCD2S_CHECK(!rig.tenants.enabled("one"));
  rig.tenants.link_opened("one");
  KCD2S_CHECK(rig.tenants.enabled("one"));
}

KCD2S_TEST(voice_tenants, a_new_link_keeps_those_named_again_and_forgets_the_others) {
  TenantsRig rig;
  const ControlMessage welcome = rig.tenants.link_opened("one");
  const KeyPair stays = make_key_pair();
  const KeyPair leaves = make_key_pair();
  const auto join = [&](std::uint32_t player, const KeyPair& keys) {
    ControlMessage message;
    message.kind = ControlKind::Join;
    message.player = player;
    message.session = welcome.session + player;
    message.key = keys.public_key;
    rig.tenants.on_control("one", message);
  };
  join(1, stays);
  join(2, leaves);
  KCD2S_CHECK_EQ(rig.tenants.players("one"), std::size_t{2});
  rig.tenants.link_lost("one");
  rig.tenants.link_opened("one");
  join(1, stays);
  ControlMessage synced;
  synced.kind = ControlKind::Synced;
  rig.tenants.on_control("one", synced);
  KCD2S_CHECK_EQ(rig.tenants.players("one"), std::size_t{1});
}

KCD2S_TEST(voice_tenants, a_server_that_switched_voice_off_stays_off_after_a_lost_link) {
  TenantsRig rig;
  rig.tenants.link_opened("one");
  ControlMessage off;
  off.kind = ControlKind::Enabled;
  off.flag = false;
  rig.tenants.on_control("one", off);
  rig.tenants.link_lost("one");
  rig.now += VoiceTenants::kLinkLostMuteMs + 1;
  rig.tenants.tick();
  rig.tenants.link_opened("one");
  KCD2S_CHECK(!rig.tenants.enabled("one"));
}
