// Real UDP and trusted-script IPC: approval cannot migrate to a different join attempt.
#include "../local/runtime.hpp"
#include "kcd2s/testkit.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
using namespace kcd2s::local;
namespace transport = kcd2s::transport;

class ScriptFixture {
 public:
  ScriptFixture() {
    folder = std::filesystem::temp_directory_path() /
             ("kcds-licence-binding-" + std::to_string(nonce()));
    std::filesystem::create_directory(folder);
    std::ofstream script(folder / "fixture.cjs");
    script << R"JS('use strict';
const fs = require('node:fs');
const path = require('node:path');
const readline = require('node:readline');
const { encode, decode } = require(process.env.KCDS_TEST_CODEC);
const events = path.join(process.argv[2], 'events.log');
let first;
const send = value => process.stdout.write(encode(value).toString('hex') + '\n');
readline.createInterface({ input: process.stdin }).on('line', line => {
  const event = decode(Buffer.from(line, 'hex'));
  if (event.op === 'playerConnecting') {
    first ??= event.request;
    if (event.licence === 'allow-now') {
      send({ op: 'playerConnectingResult', request: event.request, allowed: true });
    } else if (event.licence === 'denied' || event.nickname === 'different') {
      send({ op: 'playerConnectingResult', request: event.request, allowed: false });
    }
  } else if (event.op === 'console' && event.text === 'approve-first') {
    send({ op: 'playerConnectingResult', request: first, allowed: true });
  } else if (event.op === 'joinQueued') {
    send({ op: 'joinQueueSet', id: event.id, line: 'queue-' + event.id });
  }
  fs.appendFileSync(events, JSON.stringify(event, (_, value) =>
    typeof value === 'bigint' ? Number(value) : value) + '\n');
});
)JS";
  }

  ~ScriptFixture() {
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
  }

  void start(Host& host) const {
#ifdef _WIN32
    SetEnvironmentVariableA("KCDS_TEST_CODEC", KCDS_TEST_CODEC_PATH);
#else
    setenv("KCDS_TEST_CODEC", KCDS_TEST_CODEC_PATH, 1);
#endif
    host.start_resources(process_text(KCDS_TEST_NODE_PATH),
                         process_text((folder / "fixture.cjs").string()),
                         process_text(folder.string()));
  }

  unsigned count(const std::string& operation) const {
    std::ifstream events(folder / "events.log");
    std::string line;
    unsigned result = 0;
    while (std::getline(events, line)) {
      if (line.find("\"op\":\"" + operation + "\"") != std::string::npos) {
        ++result;
      }
    }
    return result;
  }

  std::filesystem::path folder;
};

void pump(Host& host, double seconds, double offset = 0) {
  const double deadline = clock_seconds() + seconds;
  while (clock_seconds() < deadline) {
    host.tick(clock_seconds() + offset);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

bool wait_events(Host& host, const ScriptFixture& fixture, const char* operation, unsigned count,
                 double offset = 0) {
  const double deadline = clock_seconds() + 3;
  while (clock_seconds() < deadline && fixture.count(operation) < count) {
    pump(host, 0.005, offset);
  }
  pump(host, 0.03, offset);  // Deliver the fixture's corresponding verdict to the native host.
  return fixture.count(operation) >= count;
}

struct Challenge {
  Datagram response;
  std::uint64_t nonce = 0;
};

class RawPeer {
 public:
  explicit RawPeer(Host& host)
      : socket(address("127.0.0.1", 0)), server(address("127.0.0.1", host.port())) {
  }

  Challenge challenge(Host& host, std::uint64_t client_nonce) {
    sockaddr_in from{};
    Datagram received;
    while (socket.recv(from, received)) {
    }
    transport::ClientHandshake handshake(client_nonce);
    socket.send(server, handshake.start());
    const double deadline = clock_seconds() + 2;
    Challenge result;
    while (clock_seconds() < deadline) {
      host.tick(clock_seconds());
      while (socket.recv(from, received)) {
        if (handshake.on_datagram(received, result.response)) {
          result.nonce = handshake.server_nonce();
          return result;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return {};
  }

  void respond(Host& host, const Challenge& challenge, const std::string& pass,
               const std::string& nickname = "original", double offset = 0) {
    transport::JoinIdentity identity{challenge.nonce, nickname, "76561198000000000", pass};
    socket.send(server, transport::encode_join_identity(identity));
    socket.send(server, challenge.response);
    pump(host, 0.02, offset);
  }

  std::string queue_line() {
    sockaddr_in from{};
    Datagram received;
    std::string result;
    while (socket.recv(from, received)) {
      if (const auto status = transport::decode_wait_status(received)) {
        result = status->line;
      }
    }
    return result;
  }

  UdpSocket socket;
  sockaddr_in server;
};

void changed_approval(kcd2s::testkit::Context& kcd2s_ctx, bool replace_nonce, bool replace_nickname,
                      bool late_result) {
  Winsock winsock;
  ScriptFixture fixture;
  Host host("127.0.0.1", 0);
  fixture.start(host);
  RawPeer peer(host);
  const auto first = peer.challenge(host, 101);
  KCD2S_CHECK(first.nonce != 0);
  peer.respond(host, first, "deferred");
  KCD2S_CHECK(wait_events(host, fixture, "playerConnecting", 1));
  if (!late_result) {
    host.console_command("approve-first");
    KCD2S_CHECK(wait_events(host, fixture, "console", 1));
  }
  const auto replacement = replace_nonce ? peer.challenge(host, 202) : first;
  KCD2S_CHECK(replacement.nonce != 0);
  peer.respond(host, replacement, replace_nickname ? "deferred" : "denied",
               replace_nickname ? "different" : "original");
  KCD2S_CHECK(wait_events(host, fixture, "playerConnecting", 2));
  if (late_result) {
    host.console_command("approve-first");
    KCD2S_CHECK(wait_events(host, fixture, "console", 1));
  }
  peer.respond(host, replacement, replace_nickname ? "deferred" : "denied",
               replace_nickname ? "different" : "original");
  KCD2S_CHECK_EQ(host.member_count(), std::size_t{0});

  // A fresh verdict for the current proof still admits a legitimate attempt.
  peer.respond(host, replacement, "allow-now");
  KCD2S_CHECK(wait_events(host, fixture, "playerConnecting", 3));
  peer.respond(host, replacement, "allow-now");
  KCD2S_CHECK_EQ(host.member_count(), std::size_t{1});
}
}  // namespace

KCD2S_TEST(licence_admission_binding, approved_nonce_cannot_authorize_another_handshake) {
  changed_approval(kcd2s_ctx, true, false, false);
}

KCD2S_TEST(licence_admission_binding, approved_identity_cannot_be_replaced) {
  changed_approval(kcd2s_ctx, false, true, false);
}

KCD2S_TEST(licence_admission_binding, changed_proof_ignores_the_previous_late_verdict) {
  changed_approval(kcd2s_ctx, false, false, true);
}

KCD2S_TEST(licence_admission_binding, settled_approval_has_an_admission_deadline) {
  Winsock winsock;
  ScriptFixture fixture;
  Host host("127.0.0.1", 0);
  fixture.start(host);
  RawPeer peer(host);
  const auto attempt = peer.challenge(host, 303);
  peer.respond(host, attempt, "deferred");
  KCD2S_CHECK(wait_events(host, fixture, "playerConnecting", 1));
  host.console_command("approve-first");
  KCD2S_CHECK(wait_events(host, fixture, "console", 1));
  peer.respond(host, attempt, "deferred", "original", 31);
  KCD2S_CHECK_EQ(host.member_count(), std::size_t{0});
}

KCD2S_TEST(licence_admission_binding,
           queue_identity_changes_reset_decisions_but_renewal_keeps_place) {
  Winsock winsock;
  ScriptFixture fixture;
  config::HostSettings settings;
  settings.max_peers = 1;
  Host host("127.0.0.1", 0, {}, settings);
  RawPeer blocker(host);
  const auto occupied = blocker.challenge(host, 404);
  blocker.respond(host, occupied, "unused");
  KCD2S_CHECK_EQ(host.member_count(), std::size_t{1});
  fixture.start(host);
  RawPeer queued(host);
  const auto first = queued.challenge(host, 505);
  queued.respond(host, first, "proof-one");
  KCD2S_CHECK(wait_events(host, fixture, "joinQueued", 1));
  queued.respond(host, first, "proof-one");
  const auto original_line = queued.queue_line();
  KCD2S_CHECK(!original_line.empty());
  queued.respond(host, first, "proof-renewed");
  KCD2S_CHECK_EQ(fixture.count("joinQueued"), 1u);
  KCD2S_CHECK_EQ(queued.queue_line(), original_line);
  const auto replacement = queued.challenge(host, 606);
  queued.respond(host, replacement, "proof-other");
  KCD2S_CHECK(wait_events(host, fixture, "joinQueued", 2));
  queued.respond(host, replacement, "proof-other");
  KCD2S_CHECK(queued.queue_line() != original_line);
  KCD2S_CHECK_EQ(fixture.count("joinUnqueued"), 1u);
}

KCD2S_TEST(licence_admission_binding,
           public_configuration_requires_verification_before_first_listing) {
  Winsock winsock;
  Host host("127.0.0.1", 0);
  host.require_licence_verifier(true);
  host.require_licence_verifier(false);  // Once required, heartbeat state cannot disable it.
  RawPeer peer(host);
  const auto attempt = peer.challenge(host, 707);
  peer.respond(host, attempt, "unverified");
  KCD2S_CHECK_EQ(host.member_count(), std::size_t{0});

  Host development("127.0.0.1", 0);
  development.require_licence_verifier(false);
  RawPeer private_peer(development);
  const auto private_attempt = private_peer.challenge(development, 808);
  private_peer.respond(development, private_attempt, "unused");
  KCD2S_CHECK_EQ(development.member_count(), std::size_t{1});
}

KCD2S_TEST(licence_admission_binding, public_server_waits_for_listing_even_with_a_live_verifier) {
  Winsock winsock;
  ScriptFixture fixture;
  Host host("127.0.0.1", 0);
  fixture.start(host);
  host.require_licence_verifier(true);
  host.require_master_listing();
  RawPeer peer(host);
  const auto attempt = peer.challenge(host, 850);
  peer.respond(host, attempt, "allow-now");
  pump(host, 0.05);
  KCD2S_CHECK_EQ(host.member_count(), std::size_t{0});
  KCD2S_CHECK_EQ(fixture.count("playerConnecting"), 0u);
  KCD2S_CHECK(!host.master_listing(""));
  KCD2S_CHECK(!host.master_listing("invalid-id"));
  peer.respond(host, attempt, "allow-now");
  KCD2S_CHECK_EQ(host.member_count(), std::size_t{0});
  KCD2S_CHECK(host.master_listing("abc123"));
  peer.respond(host, attempt, "allow-now");
  KCD2S_CHECK(wait_events(host, fixture, "playerConnecting", 1));
  peer.respond(host, attempt, "allow-now");
  KCD2S_CHECK_EQ(host.member_count(), std::size_t{1});
}

KCD2S_TEST(licence_admission_binding, abandoned_admission_cannot_permanently_fill_capacity) {
  Winsock winsock;
  ScriptFixture fixture;
  config::HostSettings settings;
  settings.max_peers = 1;
  Host host("127.0.0.1", 0, {}, settings);
  fixture.start(host);
  RawPeer abandoned(host);
  const auto missing = abandoned.challenge(host, 909);
  abandoned.respond(host, missing, "deferred");
  KCD2S_CHECK(wait_events(host, fixture, "playerConnecting", 1));

  RawPeer next(host);
  const auto waiting = next.challenge(host, 1001);
  next.respond(host, waiting, "allow-now");
  KCD2S_CHECK(wait_events(host, fixture, "joinQueued", 1));
  KCD2S_CHECK_EQ(fixture.count("playerConnecting"), 1u);
  // Only the queued peer sends another packet; the abandoned source never returns.
  next.respond(host, waiting, "allow-now", "original", 31);
  KCD2S_CHECK(wait_events(host, fixture, "playerConnecting", 2, 31));
  next.respond(host, waiting, "allow-now", "original", 31);
  KCD2S_CHECK_EQ(host.member_count(), std::size_t{1});
}
