// A real voice server and a real link over loopback sockets.
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "kcd2s/testkit.hpp"
#include "kcd2s/voice/remote_relay.hpp"
#include "kcd2s/voice/voice_daemon.hpp"

using kcd2s::core::Vec3;
using namespace kcd2s::voice;

namespace {

bool wait_until(const std::function<bool()>& ready, int milliseconds = 8000) {
  for (int waited = 0; waited < milliseconds; waited += 10) {
    if (ready()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return ready();
}

VoiceDaemonOptions daemon_options() {
  VoiceDaemonOptions options;
  options.bind = "127.0.0.1";
  options.voice_port = 0;
  options.control_port = 0;
  options.tenants["one"] = "secret of one";
  options.tenants["two"] = "secret of two";
  return options;
}

RemoteRelayOptions link_options(const VoiceDaemon& daemon, const char* id, const char* secret) {
  RemoteRelayOptions options;
  options.url = "127.0.0.1:" + std::to_string(daemon.control_port());
  options.server_id = id;
  options.secret = secret;
  return options;
}

// A player's game: its socket and session key.
struct FakeClient {
  net::Handle socket = net::kNoSocket;
  Key session_key{};
  Key public_key{};
  std::uint32_t session = 0;
  std::uint64_t counter = 0;

  FakeClient(const RemoteWelcome& welcome, std::uint32_t session_number) : session(session_number) {
    std::uint16_t port = 0;
    socket = net::udp_open("127.0.0.1", 0, port);
    const KeyPair keys = make_key_pair();
    public_key = keys.public_key;
    derive_session_key(keys.secret_key, welcome.public_key, keys.public_key, welcome.public_key,
                       session_key);
  }
  ~FakeClient() {
    net::close_socket(socket);
  }
  void send(const RemoteWelcome& welcome, const std::vector<std::uint8_t>& payload) {
    VoiceHeader header;
    header.tag = kVoiceUpTag;
    header.id = session;
    header.counter = ++counter;
    std::vector<std::uint8_t> sealed;
    seal_voice(header, session_key, payload.data(), payload.size(), sealed);
    net::udp_send(socket, welcome.endpoint, sealed.data(), sealed.size());
  }
  // A frame that reached this game, opened with the talker's key is not needed: its header is
  // enough to tell who talks.
  bool heard_from(kcd2s::core::EntityId talker, int milliseconds) {
    std::uint8_t buffer[kMaxVoiceDatagram];
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < until) {
      Address from;
      const int got = net::udp_recv(socket, buffer, sizeof(buffer), from, 20);
      VoiceHeader header;
      if (got > 0 && read_voice_header(buffer, std::size_t(got), header) && header.id == talker) {
        return true;
      }
    }
    return false;
  }
};

}  // namespace

KCD2S_TEST(voice_daemon, a_voice_crosses_a_real_link_and_a_wrong_secret_gets_nothing) {
  VoiceDaemon daemon(daemon_options());
  KCD2S_CHECK(daemon.start());

  RemoteRelay wrong(link_options(daemon, "one", "not the secret"));
  wrong.start();
  RemoteRelay unknown(link_options(daemon, "nobody", "secret of one"));
  unknown.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  KCD2S_CHECK(!wrong.connected());
  KCD2S_CHECK(!unknown.connected());
  KCD2S_CHECK_EQ(daemon.tenant_count(), std::size_t{0});
  wrong.stop();
  unknown.stop();

  RemoteRelay link(link_options(daemon, "one", "secret of one"));
  link.start();
  std::optional<RemoteWelcome> welcome;
  KCD2S_CHECK(wait_until([&] {
    welcome = link.take_welcome();
    return welcome.has_value();
  }));
  KCD2S_CHECK(welcome.has_value());
  KCD2S_CHECK_EQ(daemon.tenant_count(), std::size_t{1});
  KCD2S_CHECK(welcome->session_base != 0);

  FakeClient talker(*welcome, welcome->session_base + 1);
  FakeClient listener(*welcome, welcome->session_base + 2);
  link.join(11, talker.session, talker.public_key);
  link.join(22, listener.session, listener.public_key);
  link.set_position(11, Vec3{0.0f, 0.0f, 0.0f}, 0);
  link.set_position(22, Vec3{1.0f, 0.0f, 0.0f}, 0);
  link.mark_synced();
  KCD2S_CHECK(wait_until([&] { return daemon.player_count("one") == 2; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // The listener's first frame tells the relay where it is.
  listener.send(*welcome, {});
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  talker.send(*welcome, {1, 2, 3});
  KCD2S_CHECK(listener.heard_from(11, 2000));

  // The game server hears who talks, and when that ended.
  std::vector<TalkChange> changes;
  KCD2S_CHECK(wait_until([&] {
    for (const TalkChange& change : link.take_talk_changes()) {
      changes.push_back(change);
    }
    return changes.size() >= 2;
  }));
  if (changes.size() >= 2) {
    KCD2S_CHECK(changes[0].player == 11 && changes[0].talking);
    KCD2S_CHECK(changes[1].player == 11 && !changes[1].talking);
  }

  // Switched off from the game server: nothing crosses.
  link.set_enabled(false);
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  talker.send(*welcome, {4});
  KCD2S_CHECK(!listener.heard_from(11, 200));
  link.set_enabled(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  talker.send(*welcome, {5});
  KCD2S_CHECK(listener.heard_from(11, 2000));

  // A player who left is gone at once.
  link.remove_player(11);
  KCD2S_CHECK(wait_until([&] { return daemon.player_count("one") == 1; }));
}

KCD2S_TEST(voice_daemon, a_lost_link_mutes_and_comes_back_by_itself) {
  VoiceDaemon daemon(daemon_options());
  KCD2S_CHECK(daemon.start());
  auto link = std::make_unique<RemoteRelay>(link_options(daemon, "one", "secret of one"));
  link->start();
  std::optional<RemoteWelcome> welcome;
  KCD2S_CHECK(wait_until([&] {
    welcome = link->take_welcome();
    return welcome.has_value();
  }));
  KCD2S_CHECK(welcome.has_value());
  KCD2S_CHECK(daemon.tenant_enabled("one"));

  link.reset();
  KCD2S_CHECK(wait_until([&] { return !daemon.tenant_enabled("one"); },
                         int(VoiceTenants::kLinkLostMuteMs) + 3000));

  link = std::make_unique<RemoteRelay>(link_options(daemon, "one", "secret of one"));
  link->start();
  std::optional<RemoteWelcome> again;
  KCD2S_CHECK(wait_until([&] {
    again = link->take_welcome();
    return again.has_value();
  }));
  KCD2S_CHECK(again.has_value());
  KCD2S_CHECK(daemon.tenant_enabled("one"));
  KCD2S_CHECK(again->public_key == welcome->public_key);
  KCD2S_CHECK_EQ(again->session_base, welcome->session_base);
}

KCD2S_TEST(voice_daemon, the_link_comes_back_when_the_voice_server_starts_later) {
  // The port of a server that is not there yet.
  std::uint16_t control_port = 0;
  const net::Handle probe = net::tcp_listen("127.0.0.1", 0, control_port);
  net::close_socket(probe);

  RemoteRelayOptions options;
  options.url = "127.0.0.1:" + std::to_string(control_port);
  options.server_id = "one";
  options.secret = "secret of one";
  RemoteRelay link(options);
  link.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  KCD2S_CHECK(!link.connected());
  // Telling a relay with no link only drops it.
  link.set_enabled(false);
  link.set_position(1, Vec3{}, 0);

  VoiceDaemonOptions daemon_settings = daemon_options();
  daemon_settings.control_port = control_port;
  VoiceDaemon daemon(daemon_settings);
  KCD2S_CHECK(daemon.start());
  KCD2S_CHECK(wait_until([&] { return link.take_welcome().has_value(); }));
}

KCD2S_TEST(voice_daemon, a_stop_ends_quickly_even_with_a_silent_connection) {
  VoiceDaemon daemon(daemon_options());
  KCD2S_CHECK(daemon.start());
  const auto ip = net::resolve_ipv4("127.0.0.1");
  KCD2S_CHECK(ip.has_value());
  const net::Handle silent = net::tcp_connect(*ip, daemon.control_port(), 1000);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto before = std::chrono::steady_clock::now();
  daemon.stop();
  const auto took = std::chrono::steady_clock::now() - before;
  KCD2S_CHECK(took < std::chrono::seconds(2));
  net::close_socket(silent);
}
