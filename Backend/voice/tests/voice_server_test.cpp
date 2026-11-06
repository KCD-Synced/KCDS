#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "kcd2s/testkit.hpp"
#include "kcd2s/voice/voice_server.hpp"

using kcd2s::core::Vec3;
using namespace kcd2s::voice;

namespace {

struct Sent {
  Address to;
  std::vector<std::uint8_t> data;
};

struct Rig {
  std::vector<Sent> sent;
  std::uint64_t now = 1000;
  VoiceServer server;
  Key key_one{};
  Key key_two{};
  std::uint64_t counter = 0;
  Address one{0x7F000001, 1111};
  Address two{0x7F000001, 2222};

  Rig()
      : server(
            RelayConfig{},
            [this](const Address& to, const std::vector<std::uint8_t>& data) {
              sent.push_back(Sent{to, data});
            },
            [this] { return now; }) {
    key_one.fill(1);
    key_two.fill(2);
    server.add_player(1, 501, key_one);
    server.add_player(2, 502, key_two);
    server.set_position(1, Vec3{0.0f, 0.0f, 0.0f}, 0);
    server.set_position(2, Vec3{5.0f, 0.0f, 0.0f}, 0);
    server.pump();
  }

  void up(std::uint32_t session, const Key& key, const Address& from,
          std::vector<std::uint8_t> payload) {
    VoiceHeader header;
    header.tag = kVoiceUpTag;
    header.id = session;
    header.counter = ++counter;
    header.loudness = 200;
    std::vector<std::uint8_t> out;
    seal_voice(header, key, payload.data(), payload.size(), out);
    server.offer(from, out.data(), out.size());
    server.pump();
  }
};

}  // namespace

KCD2S_TEST(voice_server, a_frame_reaches_a_listener_who_said_hello) {
  Rig rig;
  // The listener's game has not said where it is yet: nothing can go out.
  rig.up(501, rig.key_one, rig.one, {9, 9, 9});
  KCD2S_CHECK(rig.sent.empty());
  rig.up(502, rig.key_two, rig.two, {});
  KCD2S_CHECK(rig.sent.empty());

  rig.up(501, rig.key_one, rig.one, {7, 8});
  KCD2S_CHECK_EQ(rig.sent.size(), std::size_t{1});
  if (rig.sent.empty()) {
    return;
  }
  KCD2S_CHECK(rig.sent[0].to == rig.two);
  VoiceHeader header;
  const auto& data = rig.sent[0].data;
  KCD2S_CHECK(read_voice_header(data.data(), data.size(), header));
  KCD2S_CHECK_EQ(header.tag, kVoiceDownTag);
  KCD2S_CHECK_EQ(header.id, std::uint32_t{1});
  std::vector<std::uint8_t> payload;
  KCD2S_CHECK(open_voice(data.data(), data.size(), header, rig.key_two, payload));
  KCD2S_CHECK((payload == std::vector<std::uint8_t>{7, 8}));
}

KCD2S_TEST(voice_server, bad_keys_unknown_sessions_and_replays_are_refused) {
  Rig rig;
  rig.up(502, rig.key_two, rig.two, {});
  rig.up(501, rig.key_two, rig.one, {1});
  rig.up(999, rig.key_one, rig.one, {1});
  KCD2S_CHECK(rig.sent.empty());
  KCD2S_CHECK_EQ(rig.server.counters().refused, std::uint64_t{2});

  VoiceHeader header;
  header.tag = kVoiceUpTag;
  header.id = 501;
  header.counter = 5000;
  std::vector<std::uint8_t> payload{1};
  std::vector<std::uint8_t> out;
  seal_voice(header, rig.key_one, payload.data(), payload.size(), out);
  rig.server.offer(rig.one, out.data(), out.size());
  rig.server.offer(rig.one, out.data(), out.size());
  rig.server.pump();
  KCD2S_CHECK_EQ(rig.sent.size(), std::size_t{1});
  KCD2S_CHECK_EQ(rig.server.counters().refused, std::uint64_t{3});
}

KCD2S_TEST(voice_server, off_means_nothing_goes_out_and_nobody_talks) {
  Rig rig;
  rig.up(502, rig.key_two, rig.two, {});
  rig.server.set_enabled(false);
  rig.up(501, rig.key_one, rig.one, {1});
  KCD2S_CHECK(rig.sent.empty());
  KCD2S_CHECK(rig.server.take_talk_changes().empty());
  rig.server.set_enabled(true);
  rig.up(501, rig.key_one, rig.one, {1});
  KCD2S_CHECK_EQ(rig.sent.size(), std::size_t{1});
}

KCD2S_TEST(voice_server, talking_starts_with_a_frame_and_ends_with_silence) {
  Rig rig;
  rig.up(501, rig.key_one, rig.one, {1});
  auto changes = rig.server.take_talk_changes();
  KCD2S_CHECK_EQ(changes.size(), std::size_t{1});
  if (!changes.empty()) {
    KCD2S_CHECK_EQ(changes[0].player, std::uint32_t{1});
    KCD2S_CHECK(changes[0].talking);
  }
  rig.up(501, rig.key_one, rig.one, {1});
  KCD2S_CHECK(rig.server.take_talk_changes().empty());
  rig.now += VoiceServer::kTalkEndMs + 1;
  rig.server.pump();
  changes = rig.server.take_talk_changes();
  KCD2S_CHECK_EQ(changes.size(), std::size_t{1});
  if (!changes.empty()) {
    KCD2S_CHECK(!changes[0].talking);
  }
}

KCD2S_TEST(voice_server, a_player_who_left_hears_nothing_and_cannot_talk) {
  Rig rig;
  rig.up(502, rig.key_two, rig.two, {});
  rig.server.remove_player(2);
  rig.up(501, rig.key_one, rig.one, {1});
  KCD2S_CHECK(rig.sent.empty());
  const auto before = rig.server.counters().refused;
  rig.up(502, rig.key_two, rig.two, {1});
  KCD2S_CHECK_EQ(rig.server.counters().refused, before + 1);
}

KCD2S_TEST(voice_server, a_full_queue_drops_the_oldest_and_a_junk_datagram_takes_no_place) {
  Rig rig;
  VoiceHeader header;
  header.tag = kVoiceUpTag;
  header.id = 501;
  std::vector<std::uint8_t> payload{1};
  std::vector<std::uint8_t> out;
  seal_voice(header, rig.key_one, payload.data(), payload.size(), out);
  for (std::size_t i = 0; i < VoiceServer::kQueueLimit + 3; ++i) {
    rig.server.offer(rig.one, out.data(), out.size());
  }
  KCD2S_CHECK_EQ(rig.server.counters().queue_drops, std::uint64_t{3});
  const std::vector<std::uint8_t> junk{kVoiceUpTag, 1};
  const auto refused = rig.server.counters().refused;
  rig.server.offer(rig.one, junk.data(), junk.size());
  KCD2S_CHECK_EQ(rig.server.counters().refused, refused + 1);
}

KCD2S_TEST(voice_server, its_own_thread_forwards_and_stops_cleanly) {
  std::mutex mutex;
  std::vector<std::vector<std::uint8_t>> received;
  VoiceServer server(
      RelayConfig{},
      [&](const Address&, const std::vector<std::uint8_t>& data) {
        std::lock_guard<std::mutex> lock(mutex);
        received.push_back(data);
      },
      [] { return std::uint64_t{1000}; });
  Key one{};
  Key two{};
  one.fill(1);
  two.fill(2);
  server.start();
  server.add_player(1, 501, one);
  server.add_player(2, 502, two);
  server.set_position(1, Vec3{0.0f, 0.0f, 0.0f}, 0);
  server.set_position(2, Vec3{1.0f, 0.0f, 0.0f}, 0);
  VoiceHeader header;
  header.tag = kVoiceUpTag;
  std::vector<std::uint8_t> out;
  header.id = 502;
  header.counter = 1;
  seal_voice(header, two, nullptr, 0, out);
  server.offer(Address{1, 2}, out.data(), out.size());
  const std::vector<std::uint8_t> payload{3};
  header.id = 501;
  seal_voice(header, one, payload.data(), payload.size(), out);
  server.offer(Address{1, 1}, out.data(), out.size());
  bool arrived = false;
  for (int i = 0; i < 400 && !arrived; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::lock_guard<std::mutex> lock(mutex);
    arrived = !received.empty();
  }
  KCD2S_CHECK(arrived);
  server.stop();
}

KCD2S_TEST(voice_server, over_the_send_budget_a_listener_hears_fewer_talkers_until_it_eases) {
  Rig rig;
  rig.server.set_out_budget(10);
  // A listener and six talkers beside it.
  const Address listener_at{0x7F000001, 3000};
  rig.up(502, rig.key_two, listener_at, {});
  std::vector<Key> keys(6);
  for (std::uint32_t i = 0; i < 6; ++i) {
    keys[i].fill(std::uint8_t(10 + i));
    rig.server.add_player(10 + i, 600 + i, keys[i]);
    rig.server.set_position(10 + i, Vec3{1.0f, 0.0f, 0.0f}, 0);
  }
  rig.server.pump();
  const auto talk_round = [&] {
    rig.now += 20;
    const std::size_t before = rig.sent.size();
    for (std::uint32_t i = 0; i < 6; ++i) {
      rig.up(600 + i, keys[i], Address{0x7F000001, std::uint16_t(4000 + i)}, {1});
    }
    std::size_t to_listener = 0;
    for (std::size_t i = before; i < rig.sent.size(); ++i) {
      if (rig.sent[i].to == listener_at) {
        ++to_listener;
      }
    }
    return to_listener;
  };
  KCD2S_CHECK_EQ(talk_round(), std::size_t{6});
  for (int i = 0; i < 5; ++i) {
    talk_round();
  }
  // A second passes with far more sent than the budget.
  rig.now += 1000;
  rig.server.pump();
  std::size_t fewer = 0;
  for (int i = 0; i < 3; ++i) {
    fewer = talk_round();
  }
  KCD2S_CHECK(fewer <= VoiceServer::kSpeakersOverBudget);
  // The crowd has gone quiet: all talkers can be heard again.
  rig.now += 2000;
  rig.server.pump();
  rig.now += 2000;
  rig.server.pump();
  KCD2S_CHECK_EQ(talk_round(), std::size_t{6});
}

KCD2S_TEST(voice_relay_load, behind_means_fewer_speakers_and_caught_up_brings_them_back) {
  RelayConfig config;
  SpeakerSelector selector;
  config.speakers_allowed = 4;
  for (kcd2s::core::EntityId id = 1; id <= 8; ++id) {
    selector.admit(id, 0.5f, 1000, config);
  }
  KCD2S_CHECK_EQ(selector.active(1000, config), std::size_t{4});
  config.speakers_allowed = 8;
  for (kcd2s::core::EntityId id = 5; id <= 8; ++id) {
    KCD2S_CHECK(selector.admit(id, 0.5f, 1001, config));
  }
  KCD2S_CHECK_EQ(selector.active(1001, config), std::size_t{8});
}
