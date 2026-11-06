#include <chrono>
#include <cstdio>
#include <vector>

#include "kcd2s/testkit.hpp"
#include "kcd2s/voice/frame.hpp"

using namespace kcd2s::voice;

namespace {

Key shared_key() {
  const KeyPair client = make_key_pair();
  const KeyPair server = make_key_pair();
  Key key{};
  derive_session_key(client.secret_key, server.public_key, client.public_key, server.public_key,
                     key);
  return key;
}

std::vector<std::uint8_t> sealed(const Key& key, std::uint64_t counter = 7) {
  const std::vector<std::uint8_t> opus{1, 2, 3, 4, 5, 6, 7, 8};
  VoiceHeader header;
  header.tag = kVoiceUpTag;
  header.id = 42;
  header.counter = counter;
  header.loudness = 200;
  std::vector<std::uint8_t> out;
  seal_voice(header, key, opus.data(), opus.size(), out);
  return out;
}

}  // namespace

KCD2S_TEST(voice_frame, both_ends_derive_the_same_key) {
  const KeyPair client = make_key_pair();
  const KeyPair server = make_key_pair();
  Key on_client{};
  Key on_server{};
  KCD2S_CHECK(derive_session_key(client.secret_key, server.public_key, client.public_key,
                                 server.public_key, on_client));
  KCD2S_CHECK(derive_session_key(server.secret_key, client.public_key, client.public_key,
                                 server.public_key, on_server));
  KCD2S_CHECK(on_client == on_server);
  KCD2S_CHECK(on_client != client.public_key);
}

KCD2S_TEST(voice_frame, a_low_order_peer_key_is_refused) {
  const KeyPair own = make_key_pair();
  const Key zero{};
  Key key{};
  KCD2S_CHECK(!derive_session_key(own.secret_key, zero, own.public_key, zero, key));
}

KCD2S_TEST(voice_frame, a_frame_comes_back_as_it_went) {
  const Key key = shared_key();
  const auto data = sealed(key);
  VoiceHeader header;
  KCD2S_CHECK(read_voice_header(data.data(), data.size(), header));
  KCD2S_CHECK_EQ(header.id, std::uint32_t{42});
  KCD2S_CHECK_EQ(header.counter, std::uint64_t{7});
  KCD2S_CHECK_EQ(header.loudness, std::uint8_t{200});
  std::vector<std::uint8_t> payload;
  KCD2S_CHECK(open_voice(data.data(), data.size(), header, key, payload));
  KCD2S_CHECK((payload == std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8}));
}

KCD2S_TEST(voice_frame, a_change_to_any_byte_or_another_key_is_refused) {
  const Key key = shared_key();
  const auto data = sealed(key);
  std::vector<std::uint8_t> payload;
  for (std::size_t i = 0; i < data.size(); ++i) {
    auto changed = data;
    changed[i] ^= 0x40;
    VoiceHeader header;
    if (!read_voice_header(changed.data(), changed.size(), header)) {
      continue;
    }
    KCD2S_CHECK(!open_voice(changed.data(), changed.size(), header, key, payload));
    KCD2S_CHECK(payload.empty());
  }
  VoiceHeader header;
  KCD2S_CHECK(read_voice_header(data.data(), data.size(), header));
  KCD2S_CHECK(!open_voice(data.data(), data.size(), header, shared_key(), payload));
}

KCD2S_TEST(voice_frame, garbage_and_long_frames_are_refused) {
  const std::vector<std::uint8_t> short_one{kVoiceUpTag, 1, 2};
  VoiceHeader header;
  KCD2S_CHECK(!read_voice_header(short_one.data(), short_one.size(), header));
  KCD2S_CHECK(!read_voice_header(nullptr, 0, header));
  const std::vector<std::uint8_t> other(64, 0x53);
  KCD2S_CHECK(!read_voice_header(other.data(), other.size(), header));
  const std::vector<std::uint8_t> huge(kMaxVoiceDatagram + 1, kVoiceUpTag);
  KCD2S_CHECK(!read_voice_header(huge.data(), huge.size(), header));

  const std::vector<std::uint8_t> long_payload(kMaxVoicePayload + 1, 1);
  VoiceHeader sending;
  sending.tag = kVoiceUpTag;
  std::vector<std::uint8_t> out;
  KCD2S_CHECK(!seal_voice(sending, Key{}, long_payload.data(), long_payload.size(), out));
}

KCD2S_TEST(voice_frame, replay_window_takes_each_counter_once) {
  ReplayWindow window;
  KCD2S_CHECK(window.may_accept(100));
  window.mark(100);
  KCD2S_CHECK(!window.may_accept(100));
  KCD2S_CHECK(window.may_accept(99));
  window.mark(99);
  KCD2S_CHECK(!window.may_accept(99));
  KCD2S_CHECK(window.may_accept(101));
  window.mark(164);
  KCD2S_CHECK(!window.may_accept(100));
  KCD2S_CHECK(window.may_accept(101));
  window.mark(1000);
  KCD2S_CHECK(!window.may_accept(936));
  KCD2S_CHECK(window.may_accept(937));
}

KCD2S_TEST(voice_frame, cost_of_one_listener_is_written_out) {
  // Timing is evidence, never a pass or fail threshold.
  const Key key = shared_key();
  const auto data = sealed(key);
  const std::vector<std::uint8_t> opus(60, 9);
  VoiceHeader header;
  header.tag = kVoiceDownTag;
  header.id = 7;
  std::vector<std::uint8_t> out;
  const int rounds = 20000;
  const auto started = std::chrono::steady_clock::now();
  for (int i = 0; i < rounds; ++i) {
    header.counter = static_cast<std::uint64_t>(i);
    seal_voice(header, key, opus.data(), opus.size(), out);
  }
  const std::chrono::duration<double, std::micro> took = std::chrono::steady_clock::now() - started;
  std::printf("voice seal of 60 bytes: %.2f us each\n", took.count() / rounds);
}
