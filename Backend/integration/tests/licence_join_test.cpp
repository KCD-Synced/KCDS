// Real UDP: platform proof work starts after the challenge and never blocks ticks.
#include "../local/runtime.hpp"
#include "kcd2s/testkit.hpp"

#include <atomic>
#include <future>

namespace {
using namespace kcd2s::local;

void tick_for(Host& host, Client& client, double seconds) {
  const auto end = clock_seconds() + seconds;
  while (clock_seconds() < end) {
    host.tick(clock_seconds());
    client.tick(clock_seconds());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}
}  // namespace

KCD2S_TEST(licence_join, proof_wait_does_not_send_a_response_or_block_other_clients) {
  Winsock winsock;
  Host host("127.0.0.1", 0);
  Client client("127.0.0.1", host.port());
  Client other("127.0.0.1", host.port());
  std::promise<void> release;
  const auto ready = release.get_future().share();
  std::atomic<std::uint64_t> observed{0};
  std::atomic<unsigned> calls{0};
  client.set_licence_provider([&](std::uint64_t nonce) {
    observed.store(nonce);
    ++calls;
    ready.wait();
    return std::string("test-proof");
  });
  const auto deadline = clock_seconds() + 2;
  while (clock_seconds() < deadline && (!observed.load() || !other.established())) {
    host.tick(clock_seconds());
    client.tick(clock_seconds());
    other.tick(clock_seconds());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const bool waited = !client.established() && observed.load() != 0;
  const bool independent = other.established() && host.member_count() == 1;
  // Release before assertions so a failed test can never leave an async worker hung.
  release.set_value();
  // The pending response still respects the transport's 250 ms resend interval.
  tick_for(host, client, 0.6);
  KCD2S_CHECK(waited);
  KCD2S_CHECK(independent);
  KCD2S_CHECK(client.established());
  KCD2S_CHECK_EQ(calls.load(), 1u);
  const auto session = client.session_credentials();
  KCD2S_CHECK(session.has_value());
  if (session) {
    KCD2S_CHECK_EQ(session->server_nonce, observed.load());
  }
}

KCD2S_TEST(licence_join, listed_host_without_a_verifier_fails_closed) {
  Winsock winsock;
  Host host("127.0.0.1", 0);
  KCD2S_CHECK(!host.master_listing("abc123"));
  Client client("127.0.0.1", host.port());
  tick_for(host, client, 0.1);
  KCD2S_CHECK(!client.established());
  KCD2S_CHECK_EQ(host.member_count(), std::size_t{0});
}

KCD2S_TEST(licence_join, binding_preserves_every_nonce_bit) {
  KCD2S_CHECK_EQ(kcd2s::transport::licence_binding(1), std::string("0000000000000001"));
  KCD2S_CHECK_EQ(kcd2s::transport::licence_binding(UINT64_MAX), std::string("ffffffffffffffff"));
  KCD2S_CHECK_EQ(kcd2s::transport::licence_binding(0x0123456789abcdefULL),
                 std::string("0123456789abcdef"));
}

KCD2S_TEST(licence_join, waiting_connection_renews_its_proof_for_the_same_handshake) {
  Winsock winsock;
  Host host("127.0.0.1", 0);
  host.master_listing("abc123");  // no verifier: keep the client awaiting admission
  Client client("127.0.0.1", host.port());
  std::atomic<unsigned> calls{0};
  std::atomic<std::uint64_t> observed{0};
  std::atomic<bool> same_nonce{true};
  client.set_licence_provider([&](std::uint64_t nonce) {
    const auto before = observed.exchange(nonce);
    if (before && before != nonce) {
      same_nonce.store(false);
    }
    ++calls;
    return Client::LicenceProof{"test-renewal-proof", 1.0};
  });
  tick_for(host, client, 1.4);
  KCD2S_CHECK(calls.load() >= 2);
  KCD2S_CHECK(same_nonce.load());
  KCD2S_CHECK(!client.established());
}
