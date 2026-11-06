// Real UDP on 127.0.0.1 with ephemeral ports: a local Host and native local::Client peers.
// a graceful disconnect removes the player for everyone within a short bound,
// a notice from anyone but the session holder is ignored, and the inactivity timeout still
// removes a client whose notice never arrives.
#include "../local/runtime.hpp"

#include <functional>
#include <thread>
#include <vector>

#include "kcd2s/testkit.hpp"

using kcd2s::local::Client;
using kcd2s::local::clock_seconds;
using kcd2s::local::Host;

namespace {

const kcd2s::local::Winsock& winsock() {
  static const kcd2s::local::Winsock instance;
  return instance;
}

// Ticks the host and the listed clients every ~2 ms until `done` holds or `timeout` seconds pass.
bool run_until(Host& host, const std::vector<Client*>& clients, double timeout,
               const std::function<bool()>& done) {
  const double deadline = clock_seconds() + timeout;
  while (clock_seconds() < deadline) {
    const double now = clock_seconds();
    host.tick(now);
    for (Client* client : clients) {
      client->tick(now);
      const auto& config = client->peer().world_config();
      if (config && !client->peer().world_ready()) {
        // Headless peers stand in for the engine's world-loaded callback.
        client->peer().mark_world_ready(config->revision);
      }
    }
    if (done()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

bool mirrors(Client& viewer, kcd2s::core::EntityId entity) {
  const auto& entities = viewer.peer().mirror().entities();
  return entities.find(entity) != entities.end();
}

// Both clients own an avatar and each one mirrors the other's avatar.
bool both_joined(Client& a, Client& b) {
  const auto avatar_a = a.own_avatar();
  const auto avatar_b = b.own_avatar();
  if (avatar_a == kcd2s::core::kInvalidEntity || avatar_b == kcd2s::core::kInvalidEntity) {
    return false;
  }
  return mirrors(a, avatar_b) && mirrors(b, avatar_a);
}

}  // namespace

KCD2S_TEST(graceful_disconnect, other_peer_observes_removal_quickly) {
  winsock();
  Host host("127.0.0.1", 0);
  Client a("127.0.0.1", host.port());
  Client b("127.0.0.1", host.port());
  const bool joined = run_until(host, {&a, &b}, 10.0, [&] { return both_joined(a, b); });
  KCD2S_CHECK(joined);
  if (!joined) {
    return;
  }
  KCD2S_CHECK_EQ(host.member_count(), std::size_t{2});
  const auto avatar_a = a.own_avatar();

  const double disconnected_at = clock_seconds();
  KCD2S_CHECK(a.disconnect());
  KCD2S_CHECK(a.disconnected());
  const bool removed = run_until(host, {&b}, 5.0, [&] { return !mirrors(b, avatar_a); });
  const double observed_after = clock_seconds() - disconnected_at;
  KCD2S_CHECK(removed);
  KCD2S_CHECK(observed_after < 1.0);
  std::printf("    graceful removal observed by the other peer after %.2f ms\n",
              observed_after * 1000.0);

  // Let the remaining notice copies arrive: removal happens once, and only through the notice.
  run_until(host, {&b}, 0.3, [] { return false; });
  KCD2S_CHECK_EQ(host.member_count(), std::size_t{1});
  KCD2S_CHECK_EQ(host.departures().disconnect, std::uint64_t{1});
  KCD2S_CHECK_EQ(host.departures().timeout, std::uint64_t{0});

  // Idempotent on the client too: nothing more is sent.
  KCD2S_CHECK(!a.disconnect());
  KCD2S_CHECK(b.own_avatar() != kcd2s::core::kInvalidEntity);
}

// ids are sequential per session and never reused. A player who leaves and joins again
// is a new entity with the next id; the old id resolves to nothing for the server and for the
// other peer. The connection slot (Player.playerIndex) is reused, as the public contract allows.
KCD2S_TEST(graceful_disconnect, reconnect_gets_the_next_id_and_the_old_id_resolves_to_nothing) {
  winsock();
  Host host("127.0.0.1", 0);
  Client a("127.0.0.1", host.port());
  Client b("127.0.0.1", host.port());
  const bool joined = run_until(host, {&a, &b}, 10.0, [&] { return both_joined(a, b); });
  KCD2S_CHECK(joined);
  if (!joined) {
    return;
  }
  const auto avatar_a = a.own_avatar();
  const auto avatar_b = b.own_avatar();
  const bool first_two_ids =
      (avatar_a == 1u && avatar_b == 2u) || (avatar_a == 2u && avatar_b == 1u);
  KCD2S_CHECK(first_two_ids);
  const bool first_two_slots = host.player_index(avatar_a) + host.player_index(avatar_b) == 1;
  KCD2S_CHECK(first_two_slots);
  const auto slot_a = host.player_index(avatar_a);

  KCD2S_CHECK(a.disconnect());
  const bool removed = run_until(host, {&b}, 5.0, [&] { return !mirrors(b, avatar_a); });
  KCD2S_CHECK(removed);
  KCD2S_CHECK(!host.entity_alive(avatar_a));
  KCD2S_CHECK_EQ(host.player_index(avatar_a), kcd2s::integration::PlayerSlots::kUnassigned);

  Client again("127.0.0.1", host.port());
  const bool rejoined = run_until(host, {&again, &b}, 10.0, [&] { return both_joined(again, b); });
  KCD2S_CHECK(rejoined);
  if (!rejoined) {
    return;
  }
  const auto reconnected = again.own_avatar();
  KCD2S_CHECK_EQ(reconnected, kcd2s::core::EntityId{3});
  KCD2S_CHECK_EQ(host.player_index(reconnected), slot_a);
  KCD2S_CHECK(!host.entity_alive(avatar_a));
  KCD2S_CHECK(!mirrors(b, avatar_a));
  KCD2S_CHECK(mirrors(b, reconnected));
}

// the resources reserve their entity ids when they start, before anyone joins, and
// the first players still get 1 and 2. The resources' ids follow the players' block.
KCD2S_TEST(graceful_disconnect, players_count_from_one_after_the_resources_reserved_their_ids) {
  winsock();
  Host host("127.0.0.1", 0);
  const auto script_ids = host.reserve_script_ids();
  KCD2S_CHECK(script_ids > kcd2s::core::EntityId{2});
  Client a("127.0.0.1", host.port());
  Client b("127.0.0.1", host.port());
  const bool joined = run_until(host, {&a, &b}, 10.0, [&] { return both_joined(a, b); });
  KCD2S_CHECK(joined);
  if (!joined) {
    return;
  }
  const auto avatar_a = a.own_avatar();
  const auto avatar_b = b.own_avatar();
  const bool first_two_ids =
      (avatar_a == 1u && avatar_b == 2u) || (avatar_a == 2u && avatar_b == 1u);
  KCD2S_CHECK(first_two_ids);
}

KCD2S_TEST(graceful_disconnect, notice_from_another_session_is_ignored) {
  winsock();
  Host host("127.0.0.1", 0);
  Client victim("127.0.0.1", host.port());
  const bool joined = run_until(host, {&victim}, 10.0,
                                [&] { return victim.own_avatar() != kcd2s::core::kInvalidEntity; });
  KCD2S_CHECK(joined);
  const auto victim_session = victim.session_credentials();
  KCD2S_CHECK(victim_session.has_value());
  if (!joined || !victim_session) {
    return;
  }

  // An attacker with its own admitted session, sending from its own address.
  kcd2s::local::UdpSocket attacker(kcd2s::local::address("127.0.0.1", 0));
  const sockaddr_in server = kcd2s::local::address("127.0.0.1", host.port());
  kcd2s::transport::ClientHandshake handshake(0xA77AC4E5ull);
  kcd2s::transport::Datagram pending = handshake.start();
  std::optional<kcd2s::transport::SessionCredentials> attacker_session;
  double last_send = -1.0;
  const bool attacker_joined = run_until(host, {&victim}, 5.0, [&] {
    const double now = clock_seconds();
    if (now - last_send >= 0.05) {
      attacker.send(server, pending);
      last_send = now;
    }
    sockaddr_in from{};
    kcd2s::transport::Datagram reply_in;
    while (attacker.recv(from, reply_in)) {
      kcd2s::transport::Datagram reply_out;
      if (handshake.on_datagram(reply_in, reply_out)) {
        attacker_session = kcd2s::transport::session_credentials_from_handshake(reply_out);
        pending = reply_out;
        attacker.send(server, reply_out);
      }
    }
    return handshake.established() && host.member_count() == 2;
  });
  KCD2S_CHECK(attacker_joined);
  KCD2S_CHECK(attacker_session.has_value());
  if (!attacker_joined || !attacker_session) {
    return;
  }

  // The victim's real pair from the wrong address, and a forged pair: both ignored.
  attacker.send(server, kcd2s::transport::encode_disconnect_notice(
                            *victim_session, kcd2s::transport::DisconnectReason::ClientQuit));
  kcd2s::transport::SessionCredentials forged = *attacker_session;
  forged.cookie ^= 0x1;
  attacker.send(server, kcd2s::transport::encode_disconnect_notice(
                            forged, kcd2s::transport::DisconnectReason::ClientQuit));
  run_until(host, {&victim}, 0.3, [] { return false; });
  KCD2S_CHECK_EQ(host.member_count(), std::size_t{2});
  KCD2S_CHECK_EQ(host.departures().disconnect, std::uint64_t{0});
  KCD2S_CHECK(victim.own_avatar() != kcd2s::core::kInvalidEntity);

  // The attacker's own valid notice removes only the attacker.
  attacker.send(server, kcd2s::transport::encode_disconnect_notice(
                            *attacker_session, kcd2s::transport::DisconnectReason::ClientQuit));
  const bool attacker_removed =
      run_until(host, {&victim}, 2.0, [&] { return host.member_count() == 1; });
  KCD2S_CHECK(attacker_removed);
  KCD2S_CHECK_EQ(host.departures().disconnect, std::uint64_t{1});
  KCD2S_CHECK(victim.own_avatar() != kcd2s::core::kInvalidEntity);
}

KCD2S_TEST(graceful_disconnect, timeout_still_removes_a_client_whose_notice_is_lost) {
  winsock();
  Host host("127.0.0.1", 0);
  host.set_inactivity_timeout(1.0);
  Client a("127.0.0.1", host.port());
  Client b("127.0.0.1", host.port());
  const bool joined = run_until(host, {&a, &b}, 10.0, [&] { return both_joined(a, b); });
  KCD2S_CHECK(joined);
  if (!joined) {
    return;
  }
  const auto avatar_a = a.own_avatar();

  // `a` vanishes without a notice reaching the server (crash, or every copy dropped): it simply
  // stops sending. Only the inactivity timeout can remove it.
  const double silent_at = clock_seconds();
  const bool removed = run_until(host, {&b}, 5.0, [&] { return !mirrors(b, avatar_a); });
  const double observed_after = clock_seconds() - silent_at;
  KCD2S_CHECK(removed);
  KCD2S_CHECK(observed_after >= 0.9);
  KCD2S_CHECK_EQ(host.departures().timeout, std::uint64_t{1});
  KCD2S_CHECK_EQ(host.departures().disconnect, std::uint64_t{0});
  KCD2S_CHECK_EQ(host.member_count(), std::size_t{1});
}

KCD2S_TEST(graceful_disconnect, disconnect_before_admission_sends_nothing) {
  winsock();
  // Nobody listens on this socket's peer: the handshake never completes.
  kcd2s::local::UdpSocket unused(kcd2s::local::address("127.0.0.1", 0));
  Client lonely("127.0.0.1", unused.local_port());
  lonely.tick(clock_seconds());
  KCD2S_CHECK(!lonely.session_credentials().has_value());
  KCD2S_CHECK(!lonely.disconnect());
  KCD2S_CHECK(lonely.disconnected());
  lonely.tick(clock_seconds());  // inert after disconnect
}
