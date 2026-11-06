#include "kcd2s/transport/handshake.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/core/version.hpp"
#include "kcd2s/testkit.hpp"
#include "kcd2s/transport/datagram.hpp"
#include "kcd2s/wire/packet.hpp"

using kcd2s::transport::ClientHandshake;
using kcd2s::transport::ConnectionGate;
using kcd2s::transport::CookieAuth;
using kcd2s::transport::Datagram;
using kcd2s::transport::encode_challenge_response;
using kcd2s::transport::encode_connect_request;
using kcd2s::transport::make_loopback_pair;

KCD2S_TEST(handshake, full_three_way_admits) {
  ConnectionGate gate(0xA11CE5EC1234u);
  ClientHandshake client(0xC0FFEEu);
  const std::uint64_t addr = 0xABCDu;

  // client -> CONNECT_REQUEST
  auto req = client.start();
  KCD2S_CHECK(client.state() == ClientHandshake::State::AwaitingChallenge);

  std::vector<std::uint8_t> challenge;
  KCD2S_CHECK(gate.on_datagram(addr, req, challenge));  // -> CHALLENGE
  KCD2S_CHECK(!gate.admitted(addr));                    // stateless: nothing committed yet

  // server -> CHALLENGE ; client -> CHALLENGE_RESPONSE
  std::vector<std::uint8_t> response;
  KCD2S_CHECK(client.on_datagram(challenge, response));
  KCD2S_CHECK(client.state() == ClientHandshake::State::AwaitingAccept);

  // client -> CHALLENGE_RESPONSE ; server verifies + admits
  std::vector<std::uint8_t> accept;
  KCD2S_CHECK(gate.on_datagram(addr, response, accept));
  KCD2S_CHECK(gate.admitted(addr));
  KCD2S_CHECK_EQ(gate.admitted_count(), std::size_t{1});

  // server -> CONNECT_ACCEPT ; client established, no further reply
  std::vector<std::uint8_t> none;
  KCD2S_CHECK(!client.on_datagram(accept, none));
  KCD2S_CHECK(client.established());
}

KCD2S_TEST(handshake, forged_cookie_rejected) {
  ConnectionGate gate(0x5EC12E7u);
  const std::uint64_t addr = 0x111u;
  // A response with a cookie the attacker just made up (never saw a real CHALLENGE).
  auto forged = encode_challenge_response(/*server_nonce=*/0x1234u, /*cookie=*/0xDEADBEEFu);
  std::vector<std::uint8_t> reply;
  KCD2S_CHECK(!gate.on_datagram(addr, forged, reply));
  KCD2S_CHECK(!gate.admitted(addr));
  KCD2S_CHECK_EQ(gate.admitted_count(), std::size_t{0});
}

KCD2S_TEST(handshake, wrong_secret_cookie_rejected) {
  // A cookie minted under a different secret must not verify against the real gate.
  ConnectionGate real_gate(0xAAAAAAAAu);
  CookieAuth attacker(0xBBBBBBBBu);
  const std::uint64_t addr = 0x222u;
  const std::uint64_t sn = 0x777u;
  auto resp = encode_challenge_response(sn, attacker.cookie(addr, sn));
  std::vector<std::uint8_t> reply;
  KCD2S_CHECK(!real_gate.on_datagram(addr, resp, reply));
  KCD2S_CHECK(!real_gate.admitted(addr));
}

KCD2S_TEST(handshake, cookie_is_bound_to_address) {
  // A valid response for addr A, replayed from addr B, is rejected (cookie binds the address).
  ConnectionGate gate(0x9000u);
  const std::uint64_t addrA = 0xAAA0u;
  const std::uint64_t addrB = 0xBBB0u;

  ClientHandshake a(0x1u);
  auto req = a.start();
  std::vector<std::uint8_t> challenge;
  KCD2S_CHECK(gate.on_datagram(addrA, req, challenge));
  std::vector<std::uint8_t> response;
  KCD2S_CHECK(a.on_datagram(challenge, response));

  std::vector<std::uint8_t> reply;
  KCD2S_CHECK(!gate.on_datagram(addrB, response, reply));  // replayed under B -> rejected
  KCD2S_CHECK(!gate.admitted(addrB));
  // But the legitimate owner (addr A) is still admitted by the same response.
  KCD2S_CHECK(gate.on_datagram(addrA, response, reply));
  KCD2S_CHECK(gate.admitted(addrA));
}

KCD2S_TEST(handshake, channel_traffic_before_handshake_is_ignored) {
  ConnectionGate gate(0x1u);
  const std::uint64_t addr = 0x333u;
  // A real KSTP channel packet arriving before the handshake completes.
  kcd2s::wire::Packet p;
  p.channels.push_back({static_cast<std::uint8_t>(kcd2s::wire::Channel::Snapshot), {1, 2, 3}});
  const auto chan = kcd2s::wire::write_packet(p);
  std::vector<std::uint8_t> reply;
  KCD2S_CHECK(!gate.on_datagram(addr, chan, reply));  // not a handshake kind -> ignored
  KCD2S_CHECK(!gate.admitted(addr));
}

KCD2S_TEST(handshake, garbage_and_truncated_ignored) {
  ConnectionGate gate(0x1u);
  const std::uint64_t addr = 0x444u;
  std::vector<std::uint8_t> reply;
  KCD2S_CHECK(!gate.on_datagram(addr, {}, reply));                                   // empty
  KCD2S_CHECK(!gate.on_datagram(addr, {kcd2s::transport::kConnectRequest}, reply));  // truncated
  KCD2S_CHECK(!gate.on_datagram(addr, {kcd2s::transport::kChallengeResponse, 0, 0}, reply));
  KCD2S_CHECK(!gate.on_datagram(addr, {0xFF, 0xFF, 0xFF}, reply));  // unknown kind
  KCD2S_CHECK_EQ(gate.admitted_count(), std::size_t{0});
}

// The build proof: a client that copies our fingerprint and answers the real CHALLENGE
// (right cookie) but holds another build's key is not admitted; neither is a response without the
// proof (the old 17-byte form) or one whose proof was changed. Our own key is admitted.
KCD2S_TEST(handshake, build_proof_required) {
  ConnectionGate gate(0x5A17u);
  const std::uint64_t addr = 0xB07u;
  std::vector<std::uint8_t> challenge;
  KCD2S_CHECK(gate.on_datagram(addr, encode_connect_request(0x42u), challenge));
  kcd2s::core::BitReader r(challenge);
  r.read_bits(8);
  const std::uint64_t sn = r.read_bits(64);
  const std::uint64_t cookie = r.read_bits(64);
  std::vector<std::uint8_t> reply;

  std::array<std::uint8_t, 32> foreign_key = kcd2s::core::handshake_key();
  foreign_key[0] ^= 0x01;
  KCD2S_CHECK(!gate.on_datagram(addr, encode_challenge_response(sn, cookie, foreign_key), reply));

  std::vector<std::uint8_t> old_form = encode_challenge_response(sn, cookie);
  old_form.resize(17);
  KCD2S_CHECK(!gate.on_datagram(addr, old_form, reply));

  std::vector<std::uint8_t> tampered = encode_challenge_response(sn, cookie);
  tampered.back() ^= 0x80;
  KCD2S_CHECK(!gate.on_datagram(addr, tampered, reply));
  KCD2S_CHECK(!gate.admitted(addr));

  KCD2S_CHECK(gate.on_datagram(addr, encode_challenge_response(sn, cookie), reply));
  KCD2S_CHECK(gate.admitted(addr));
}

// The proof is the HMAC of the nonce and the cookie: another nonce gives another proof, so a
// captured response cannot answer a later CHALLENGE.
KCD2S_TEST(handshake, build_proof_depends_on_nonce) {
  const auto& key = kcd2s::core::handshake_key();
  const auto first = kcd2s::transport::challenge_proof(key, 0x1u, 0x99u);
  const auto again = kcd2s::transport::challenge_proof(key, 0x1u, 0x99u);
  const auto other = kcd2s::transport::challenge_proof(key, 0x2u, 0x99u);
  KCD2S_CHECK(first == again);
  KCD2S_CHECK(first != other);
}

KCD2S_TEST(handshake, foreign_build_salt_rejected) {
  ConnectionGate gate(0x1234u);
  const std::uint64_t addr = 0xF00Du;

  // A client of another build -> different fingerprint -> rejected at CONNECT_REQUEST, before any
  // cookie work.
  auto foreign = encode_connect_request(0xC0FFEEu, kcd2s::core::build_fingerprint() ^ 0xDEADu);
  std::vector<std::uint8_t> reply;
  KCD2S_CHECK(!gate.on_datagram(addr, foreign, reply));
  KCD2S_CHECK(!gate.admitted(addr));

  // A client built from the SAME salt (our own fingerprint) gets a CHALLENGE.
  auto ours = encode_connect_request(0xC0FFEEu);  // uses core::build_fingerprint()
  KCD2S_CHECK(gate.on_datagram(addr, ours, reply));
}

// A client of another build hears the server's version: the refusal of its own
// request, then the text naming the version. A client of this build gets no such answer.
KCD2S_TEST(handshake, other_build_told_server_version) {
  ConnectionGate gate(0x1234u);
  const auto foreign =
      encode_connect_request(0xC0FFEEu, kcd2s::core::build_fingerprint() ^ 0xDEADu);
  std::vector<std::uint8_t> refusal;
  std::vector<std::uint8_t> text;
  KCD2S_CHECK(gate.refuse_other_build(foreign, "0.3.4.1", refusal, text));
  KCD2S_CHECK(
      !gate.refuse_other_build(encode_connect_request(0xC0FFEEu), "0.3.4.1", refusal, text));
  KCD2S_CHECK(!gate.refuse_other_build(foreign, "not a version", refusal, text));

  KCD2S_CHECK(gate.refuse_other_build(foreign, "0.3.4.1", refusal, text));
  ClientHandshake client(0xC0FFEEu);
  client.start();
  std::vector<std::uint8_t> reply;
  KCD2S_CHECK(!client.on_datagram(refusal, reply));
  KCD2S_CHECK(client.refused());
  KCD2S_CHECK(client.refusal().reason == kcd2s::transport::RefusalReason::OtherVersion);
  KCD2S_CHECK(!client.on_datagram(text, reply));
  KCD2S_CHECK(client.refusal_text() == kcd2s::transport::other_version_text("0.3.4.1"));
  KCD2S_CHECK(kcd2s::transport::refused_version(client.refusal_text()) == "0.3.4.1");
  KCD2S_CHECK(kcd2s::transport::refused_version("The gate is locked.").empty());
  KCD2S_CHECK(kcd2s::transport::refused_version("This server runs KCDS 0.3.<b>. x").empty());
}

KCD2S_TEST(handshake, wrong_protocol_request_rejected) {
  ConnectionGate gate(0x1u);
  const std::uint64_t addr = 0x555u;
  // Hand-build a CONNECT_REQUEST with a bad protocol id.
  std::vector<std::uint8_t> bad = {kcd2s::transport::kConnectRequest, 0x00, 0x00};
  for (int i = 0; i < 8; ++i)
    bad.push_back(0);  // client nonce
  std::vector<std::uint8_t> reply;
  KCD2S_CHECK(!gate.on_datagram(addr, bad, reply));
}

KCD2S_TEST(handshake, previous_animation_protocol_is_rejected_even_with_matching_build_salt) {
  ConnectionGate gate(0x1234u);
  const std::uint64_t addr = 0xF003u;
  auto previous = encode_connect_request(0xC0FFEEu);
  // Same fingerprint, nonce and complete request; only the old protocol differs.
  previous[1] = 0x52;
  previous[2] = 0x4B;
  std::vector<std::uint8_t> reply;
  KCD2S_CHECK(!gate.on_datagram(addr, previous, reply));
  KCD2S_CHECK(reply.empty());
  KCD2S_CHECK(!gate.admitted(addr));
  KCD2S_CHECK_EQ(gate.admitted_count(), std::size_t{0});
  const auto current = encode_connect_request(0xC0FFEEu);
  KCD2S_CHECK(gate.on_datagram(addr, current, reply));
  KCD2S_CHECK(!reply.empty());
  KCD2S_CHECK_EQ(reply[0], kcd2s::transport::kChallenge);
}

KCD2S_TEST(handshake, flows_over_sim_link) {
  // Drive the whole handshake through the datagram substrate to show it is link-ready.
  auto [cl, sv] = make_loopback_pair();  // cl = client endpoint, sv = server endpoint
  ConnectionGate gate(0x7E57u);
  ClientHandshake client(0x2468u);
  const std::uint64_t addr = 0x1A2Bu;

  cl->send(client.start());  // CONNECT_REQUEST onto the wire

  bool admitted = false;
  for (int step = 0; step < 8 && !admitted; ++step) {
    Datagram dg;
    while (sv->recv(dg)) {  // server side
      std::vector<std::uint8_t> reply;
      if (gate.on_datagram(addr, dg, reply))
        sv->send(reply);
      admitted = gate.admitted(addr);
    }
    while (cl->recv(dg)) {  // client side
      std::vector<std::uint8_t> reply;
      if (client.on_datagram(dg, reply))
        cl->send(reply);
    }
  }
  KCD2S_CHECK(gate.admitted(addr));
  KCD2S_CHECK(client.established());
}

// ---- CONNECT_REFUSED (server full), ----

using kcd2s::transport::ConnectRefusal;
using kcd2s::transport::decode_connect_refused;
using kcd2s::transport::encode_connect_refused;
using kcd2s::transport::HandshakePacer;
using kcd2s::transport::RefusalBudget;
using kcd2s::transport::RefusalReason;

KCD2S_TEST(handshake, connect_refused_wire_contract) {
  ConnectRefusal refusal;
  refusal.reason = RefusalReason::ServerFull;
  refusal.retry_after_ms = 5000;
  refusal.echo = 0x0123456789ABCDEFull;
  const auto bytes = encode_connect_refused(refusal);

  // u8 kind | u16 protocolId | u8 reason | u16 retryAfterMs | u64 echo, little-endian fields.
  const std::uint16_t proto = kcd2s::wire::kProtocolId;
  const std::vector<std::uint8_t> expected = {
      0x05,
      static_cast<std::uint8_t>(proto & 0xFFu),
      static_cast<std::uint8_t>(proto >> 8),
      0x01,
      0x88,
      0x13,
      0xEF,
      0xCD,
      0xAB,
      0x89,
      0x67,
      0x45,
      0x23,
      0x01,
  };
  KCD2S_CHECK(bytes == expected);
  KCD2S_CHECK_EQ(bytes.size(), kcd2s::transport::kConnectRefusedSize);
  // Protocol 3 requires sparse native animation decoding. The refusal envelope
  // remains the same size and retains the exact current protocol identifier.
  KCD2S_CHECK_EQ(proto, std::uint16_t{0x4B53});

  // Never larger than either handshake datagram it can answer: no amplification.
  KCD2S_CHECK(bytes.size() < encode_connect_request(1).size());
  KCD2S_CHECK(bytes.size() < encode_challenge_response(1, 2).size());
  KCD2S_CHECK_EQ(encode_connect_request(1).size(), kcd2s::transport::kConnectRequestSize);
  KCD2S_CHECK_EQ(encode_challenge_response(1, 2).size(), kcd2s::transport::kChallengeResponseSize);

  const auto decoded = decode_connect_refused(bytes);
  KCD2S_CHECK(decoded.has_value());
  KCD2S_CHECK(decoded->reason == RefusalReason::ServerFull);
  KCD2S_CHECK_EQ(decoded->retry_after_ms, std::uint16_t{5000});
  KCD2S_CHECK_EQ(decoded->echo, std::uint64_t{0x0123456789ABCDEFull});
}

KCD2S_TEST(handshake, connect_refused_decode_is_strict) {
  ConnectRefusal refusal;
  refusal.echo = 7;
  const auto good = encode_connect_refused(refusal);

  auto truncated = good;
  truncated.pop_back();
  KCD2S_CHECK(!decode_connect_refused(truncated).has_value());
  auto padded = good;
  padded.push_back(0);
  KCD2S_CHECK(!decode_connect_refused(padded).has_value());
  auto wrong_protocol = good;
  wrong_protocol[1] ^= 0x01;
  KCD2S_CHECK(!decode_connect_refused(wrong_protocol).has_value());
  auto wrong_kind = good;
  wrong_kind[0] = kcd2s::transport::kConnectAccept;
  KCD2S_CHECK(!decode_connect_refused(wrong_kind).has_value());
  KCD2S_CHECK(!decode_connect_refused({}).has_value());
}

KCD2S_TEST(handshake, gate_refuses_only_valid_handshakes_and_keeps_no_state) {
  ConnectionGate gate(0xFEEDu);
  const std::uint64_t addr = 0xAB01u;
  std::vector<std::uint8_t> reply;

  // A valid request is refused with its own client nonce echoed.
  KCD2S_CHECK(
      gate.refuse(addr, encode_connect_request(0x1234u), RefusalReason::ServerFull, 5000, reply));
  auto refusal = decode_connect_refused(reply);
  KCD2S_CHECK(refusal.has_value());
  KCD2S_CHECK_EQ(refusal->echo, std::uint64_t{0x1234u});
  KCD2S_CHECK_EQ(refusal->retry_after_ms, std::uint16_t{5000});
  KCD2S_CHECK(!gate.admitted(addr));
  KCD2S_CHECK_EQ(gate.admitted_count(), std::size_t{0});

  // Foreign builds, forged cookies, garbage and non-handshake traffic get no reply at all.
  const auto foreign = encode_connect_request(1, kcd2s::core::build_fingerprint() ^ 0x1u);
  KCD2S_CHECK(!gate.refuse(addr, foreign, RefusalReason::ServerFull, 5000, reply));
  KCD2S_CHECK(!gate.refuse(addr, encode_challenge_response(0x55u, 0xDEADu),
                           RefusalReason::ServerFull, 5000, reply));
  KCD2S_CHECK(!gate.refuse(addr, {kcd2s::transport::kConnectRequest}, RefusalReason::ServerFull,
                           5000, reply));
  KCD2S_CHECK(
      !gate.refuse(addr, encode_connect_refused({}), RefusalReason::ServerFull, 5000, reply));
  KCD2S_CHECK(!gate.refuse(addr, {}, RefusalReason::ServerFull, 5000, reply));

  // A verified CHALLENGE_RESPONSE is refused with the server nonce echoed, and not admitted.
  ClientHandshake client(0x99u);
  std::vector<std::uint8_t> challenge;
  KCD2S_CHECK(gate.on_datagram(addr, client.start(), challenge));
  std::vector<std::uint8_t> response;
  KCD2S_CHECK(client.on_datagram(challenge, response));
  KCD2S_CHECK(gate.refuse(addr, response, RefusalReason::ServerFull, 5000, reply));
  KCD2S_CHECK(!gate.admitted(addr));
  // The same response replayed from another address does not verify, so it is not answered.
  KCD2S_CHECK(!gate.refuse(addr + 1, response, RefusalReason::ServerFull, 5000, reply));
}

KCD2S_TEST(handshake, client_accepts_only_its_own_refusal) {
  ConnectionGate gate(0xC0DEu);
  const std::uint64_t addr = 0x77u;
  ClientHandshake client(0xABCDu);
  const auto request = client.start();
  std::vector<std::uint8_t> none;

  // A refusal echoing a different nonce (an off-path guess) is ignored.
  ConnectRefusal spoofed;
  spoofed.echo = 0xABCEu;
  KCD2S_CHECK(!client.on_datagram(encode_connect_refused(spoofed), none));
  KCD2S_CHECK(client.state() == ClientHandshake::State::AwaitingChallenge);

  std::vector<std::uint8_t> refused;
  KCD2S_CHECK(gate.refuse(addr, request, RefusalReason::ServerFull, 3000, refused));
  KCD2S_CHECK(!client.on_datagram(refused, none));
  KCD2S_CHECK(client.refused());
  KCD2S_CHECK(!client.established());
  KCD2S_CHECK_EQ(client.refusal().retry_after_ms, std::uint16_t{3000});

  // A later attempt starts over and can still complete once capacity is available.
  std::vector<std::uint8_t> challenge;
  KCD2S_CHECK(gate.on_datagram(addr, client.start(), challenge));
  KCD2S_CHECK(client.state() == ClientHandshake::State::AwaitingChallenge);
  std::vector<std::uint8_t> response;
  KCD2S_CHECK(client.on_datagram(challenge, response));
  std::vector<std::uint8_t> accept;
  KCD2S_CHECK(gate.on_datagram(addr, response, accept));
  KCD2S_CHECK(!client.on_datagram(accept, none));
  KCD2S_CHECK(client.established());

  // An established client ignores refusals, even replayed matching ones.
  KCD2S_CHECK(!client.on_datagram(refused, none));
  KCD2S_CHECK(client.established());
}

KCD2S_TEST(handshake, client_accepts_refusal_of_its_response) {
  ConnectionGate gate(0xBEEFu);
  const std::uint64_t addr = 0x31u;
  ClientHandshake client(0x4242u);
  std::vector<std::uint8_t> challenge;
  KCD2S_CHECK(gate.on_datagram(addr, client.start(), challenge));
  std::vector<std::uint8_t> response;
  KCD2S_CHECK(client.on_datagram(challenge, response));

  // While awaiting ACCEPT, a refusal echoing the client nonce (a late reply to the request) is
  // ignored; one echoing the server nonce of this challenge is accepted.
  ConnectRefusal stale;
  stale.echo = 0x4242u;
  std::vector<std::uint8_t> none;
  KCD2S_CHECK(!client.on_datagram(encode_connect_refused(stale), none));
  KCD2S_CHECK(client.state() == ClientHandshake::State::AwaitingAccept);

  std::vector<std::uint8_t> refused;
  KCD2S_CHECK(gate.refuse(addr, response, RefusalReason::ServerFull, 5000, refused));
  KCD2S_CHECK(!client.on_datagram(refused, none));
  KCD2S_CHECK(client.refused());
}

KCD2S_TEST(handshake, refusal_budget_bounds_aggregate_rate) {
  RefusalBudget budget(10.0, 5.0);
  int granted = 0;
  for (int i = 0; i < 100; ++i) {
    if (budget.try_take(100.0))
      granted += 1;
  }
  KCD2S_CHECK_EQ(granted, 5);  // burst only, within one instant

  // Over one further second at most per_second more are granted, however many are asked.
  granted = 0;
  for (int i = 1; i <= 1000; ++i) {
    if (budget.try_take(100.0 + i * 0.001))
      granted += 1;
  }
  KCD2S_CHECK(granted >= 9 && granted <= 10);

  // Idle time refills up to the burst, never beyond it.
  granted = 0;
  for (int i = 0; i < 100; ++i) {
    if (budget.try_take(1000.0))
      granted += 1;
  }
  KCD2S_CHECK_EQ(granted, 5);
  // A clock that goes backwards does not mint tokens.
  KCD2S_CHECK(!budget.try_take(50.0));
}

KCD2S_TEST(handshake, pacer_backs_off_unanswered_and_refused_attempts) {
  HandshakePacer pacer(0);  // seed 0: no jitter
  KCD2S_CHECK(pacer.due(0.0));

  // Unanswered: four sends at 0.25 s, then 0.5 s, 1 s, and capped at 2 s.
  double now = 0.0;
  std::vector<double> gaps;
  for (int i = 0; i < 20; ++i) {
    pacer.sent(now);
    gaps.push_back(pacer.next_send() - now);
    now = pacer.next_send();
  }
  KCD2S_CHECK_EQ(gaps[0], 0.25);
  KCD2S_CHECK_EQ(gaps[3], 0.25);
  KCD2S_CHECK_EQ(gaps[4], 0.5);
  KCD2S_CHECK_EQ(gaps[8], 1.0);
  KCD2S_CHECK_EQ(gaps[12], 2.0);
  KCD2S_CHECK_EQ(gaps[19], 2.0);

  // Progress resets the resend interval.
  pacer.progressed();
  pacer.sent(now);
  KCD2S_CHECK_EQ(pacer.next_send() - now, 0.25);

  // Refusals: the server hint, doubled per consecutive refusal, capped at 60 s.
  KCD2S_CHECK_EQ(pacer.refused(now, 5000), 5.0);
  KCD2S_CHECK(!pacer.due(now + 4.9));
  KCD2S_CHECK(pacer.due(now + 5.0));
  KCD2S_CHECK_EQ(pacer.refused(now, 5000), 10.0);
  KCD2S_CHECK_EQ(pacer.refused(now, 5000), 20.0);
  KCD2S_CHECK_EQ(pacer.refused(now, 5000), 40.0);
  KCD2S_CHECK_EQ(pacer.refused(now, 5000), 60.0);
  KCD2S_CHECK_EQ(pacer.refused(now, 5000), 60.0);
  KCD2S_CHECK_EQ(pacer.consecutive_refusals(), 6u);

  // A missing or tiny hint still waits at least one second; jitter adds at most 25%.
  HandshakePacer no_hint(0);
  KCD2S_CHECK_EQ(no_hint.refused(0.0, 0), 1.0);
  HandshakePacer jittered(999);
  const double delay = jittered.refused(0.0, 5000);
  KCD2S_CHECK(delay > 5.0 && delay < 6.25);
}

KCD2S_TEST(handshake, refusal_reasons_round_trip) {
  for (const auto reason :
       {RefusalReason::ServerFull, RefusalReason::Rejected, RefusalReason::AdmissionTimeout}) {
    ConnectRefusal refusal;
    refusal.reason = reason;
    refusal.retry_after_ms = 5000;
    refusal.echo = 7;
    const auto decoded = kcd2s::transport::decode_connect_refused(encode_connect_refused(refusal));
    KCD2S_CHECK(decoded.has_value());
    KCD2S_CHECK(decoded && decoded->reason == reason);
  }
}

// ---- REFUSAL_TEXT (the reason a playerConnecting handler gave), ----

using kcd2s::transport::decode_refusal_text;
using kcd2s::transport::encode_refusal_text;
using kcd2s::transport::kMaxRefusalTextBytes;
using kcd2s::transport::sanitize_refusal_text;
using kcd2s::transport::valid_refusal_text;

KCD2S_TEST(handshake, refusal_text_wire_contract) {
  const auto bytes = encode_refusal_text(0x0123456789ABCDEFull, "Whitelist only");
  // u8 kind | u16 protocolId | u64 echo | u16 length | text, little-endian fields.
  const std::uint16_t proto = kcd2s::wire::kProtocolId;
  std::vector<std::uint8_t> expected = {
      0x06,
      static_cast<std::uint8_t>(proto & 0xFFu),
      static_cast<std::uint8_t>(proto >> 8),
      0xEF,
      0xCD,
      0xAB,
      0x89,
      0x67,
      0x45,
      0x23,
      0x01,
      14,
      0,
  };
  const std::string text = "Whitelist only";
  expected.insert(expected.end(), text.begin(), text.end());
  KCD2S_CHECK(bytes == expected);
  KCD2S_CHECK_EQ(bytes.size(), kcd2s::transport::kRefusalTextHeaderSize + text.size());

  const auto decoded = decode_refusal_text(bytes);
  KCD2S_CHECK(decoded.has_value());
  KCD2S_CHECK_EQ(decoded->echo, std::uint64_t{0x0123456789ABCDEFull});
  KCD2S_CHECK(decoded->text == text);
  // Older peers' strict CONNECT_REFUSED decoder never mistakes it for a refusal.
  KCD2S_CHECK(!kcd2s::transport::decode_connect_refused(bytes).has_value());
}

KCD2S_TEST(handshake, refusal_text_decode_is_strict) {
  const auto good = encode_refusal_text(7, "No room");
  auto truncated = good;
  truncated.pop_back();
  KCD2S_CHECK(!decode_refusal_text(truncated).has_value());
  auto padded = good;
  padded.push_back('x');
  KCD2S_CHECK(!decode_refusal_text(padded).has_value());
  auto wrong_protocol = good;
  wrong_protocol[1] ^= 0x01;
  KCD2S_CHECK(!decode_refusal_text(wrong_protocol).has_value());
  auto control = good;
  control.back() = 0x07;  // BEL
  KCD2S_CHECK(!decode_refusal_text(control).has_value());
  auto malformed = good;
  malformed.back() = 0xC3;  // a lead byte without its continuation
  KCD2S_CHECK(!decode_refusal_text(malformed).has_value());
  KCD2S_CHECK(!decode_refusal_text({}).has_value());

  // An empty text and one over the limit are refused even with a matching length field.
  std::vector<std::uint8_t> empty(good.begin(), good.begin() + 13);
  empty[11] = 0;
  empty[12] = 0;
  KCD2S_CHECK(!decode_refusal_text(empty).has_value());
  std::vector<std::uint8_t> oversized(good.begin(), good.begin() + 13);
  const std::size_t over = kMaxRefusalTextBytes + 1;
  oversized[11] = static_cast<std::uint8_t>(over & 0xFFu);
  oversized[12] = static_cast<std::uint8_t>(over >> 8);
  oversized.insert(oversized.end(), over, 'a');
  KCD2S_CHECK(!decode_refusal_text(oversized).has_value());
}

KCD2S_TEST(handshake, refusal_text_is_plain_bounded_utf8) {
  KCD2S_CHECK(valid_refusal_text("Server is in maintenance until 20:00"));
  KCD2S_CHECK(valid_refusal_text("Jsi na \xC4\x8D\x65rn\xC3\xA9 listin\xC4\x9B"));  // Czech
  KCD2S_CHECK(!valid_refusal_text(""));
  KCD2S_CHECK(!valid_refusal_text("line\nbreak"));
  KCD2S_CHECK(!valid_refusal_text("\x7F"));
  KCD2S_CHECK(!valid_refusal_text("\xC2\x85"));  // NEL, a C1 control
  KCD2S_CHECK(
      !valid_refusal_text("a\xE2\x80\xAE"
                          "b"));                     // RIGHT-TO-LEFT OVERRIDE
  KCD2S_CHECK(!valid_refusal_text("\xC0\xAF"));      // overlong '/'
  KCD2S_CHECK(!valid_refusal_text("\xED\xA0\x80"));  // a UTF-16 surrogate
  KCD2S_CHECK(!valid_refusal_text(std::string(kMaxRefusalTextBytes + 1, 'a')));
  KCD2S_CHECK(valid_refusal_text(std::string(kMaxRefusalTextBytes, 'a')));

  KCD2S_CHECK(sanitize_refusal_text("  Banned:\tcheating\r\n ") == "Banned: cheating");
  KCD2S_CHECK(sanitize_refusal_text("<b>bold</b>") == "<b>bold</b>");  // text, never markup
  KCD2S_CHECK(sanitize_refusal_text("a\xE2\x80\xAE"
                                    "b\x01"
                                    "c\xFF") == "abc");
  KCD2S_CHECK(sanitize_refusal_text("\x01\x02").empty());

  // A long text is cut on a character boundary: one ASCII byte short of the limit and a 2-byte
  // character do not fit, so the character is dropped whole.
  const std::string long_text = std::string(kMaxRefusalTextBytes - 1, 'a') + "\xC3\xA9";
  const std::string cut = sanitize_refusal_text(long_text);
  KCD2S_CHECK_EQ(cut.size(), kMaxRefusalTextBytes - 1);
  KCD2S_CHECK(valid_refusal_text(cut));
}

KCD2S_TEST(handshake, refusal_text_goes_only_to_a_verified_response) {
  ConnectionGate gate(0x7E77u);
  const std::uint64_t addr = 0x1234u;
  std::vector<std::uint8_t> reply;
  ClientHandshake client(0x42u);
  const auto request = client.start();
  // An unverified CONNECT_REQUEST never earns the larger frame.
  KCD2S_CHECK(!gate.refusal_text(addr, request, "Whitelist only", reply));

  std::vector<std::uint8_t> challenge;
  KCD2S_CHECK(gate.on_datagram(addr, request, challenge));
  std::vector<std::uint8_t> response;
  KCD2S_CHECK(client.on_datagram(challenge, response));
  KCD2S_CHECK(!gate.refusal_text(addr + 1, response, "Whitelist only", reply));  // replayed
  KCD2S_CHECK(!gate.refusal_text(addr, response, "\x01\x02", reply));  // nothing left to show
  KCD2S_CHECK(gate.refusal_text(addr, response, "Whitelist only", reply));
  KCD2S_CHECK(!gate.admitted(addr));
}

KCD2S_TEST(handshake, client_keeps_the_text_of_its_own_refusal) {
  // The text arrives first (the server sends it first), then the refusal.
  {
    ConnectionGate gate(0x5151u);
    const std::uint64_t addr = 0x9u;
    ClientHandshake client(0x77u);
    std::vector<std::uint8_t> challenge;
    KCD2S_CHECK(gate.on_datagram(addr, client.start(), challenge));
    std::vector<std::uint8_t> response;
    KCD2S_CHECK(client.on_datagram(challenge, response));
    std::vector<std::uint8_t> text;
    std::vector<std::uint8_t> refused;
    KCD2S_CHECK(gate.refusal_text(addr, response, "Whitelist only", text));
    KCD2S_CHECK(gate.refuse(addr, response, RefusalReason::Rejected, 5000, refused));
    std::vector<std::uint8_t> none;
    KCD2S_CHECK(!client.on_datagram(text, none));
    KCD2S_CHECK(client.refusal_text().empty());  // nothing refused yet
    KCD2S_CHECK(!client.on_datagram(refused, none));
    KCD2S_CHECK(client.refused());
    KCD2S_CHECK(client.refusal().reason == RefusalReason::Rejected);
    KCD2S_CHECK(client.refusal_text() == "Whitelist only");
  }
  // Reordered: the refusal first, then its text. A text echoing another nonce is ignored.
  {
    ConnectionGate gate(0x5252u);
    const std::uint64_t addr = 0xAu;
    ClientHandshake client(0x78u);
    std::vector<std::uint8_t> challenge;
    KCD2S_CHECK(gate.on_datagram(addr, client.start(), challenge));
    std::vector<std::uint8_t> response;
    KCD2S_CHECK(client.on_datagram(challenge, response));
    std::vector<std::uint8_t> refused;
    KCD2S_CHECK(gate.refuse(addr, response, RefusalReason::Rejected, 5000, refused));
    std::vector<std::uint8_t> none;
    KCD2S_CHECK(!client.on_datagram(refused, none));
    KCD2S_CHECK(!client.on_datagram(encode_refusal_text(0xBAD, "Spoofed"), none));
    KCD2S_CHECK(client.refusal_text().empty());
    std::vector<std::uint8_t> text;
    KCD2S_CHECK(gate.refusal_text(addr, response, "Come back at 20:00", text));
    KCD2S_CHECK(!client.on_datagram(text, none));
    KCD2S_CHECK(client.refusal_text() == "Come back at 20:00");
  }
}

// a waiting response is told its place, time and the server's lines; the identity
// travels with it.
KCD2S_TEST(handshake, wait_status_and_identity_round_trip) {
  using kcd2s::transport::decode_join_identity;
  using kcd2s::transport::decode_wait_status;
  using kcd2s::transport::encode_join_identity;
  using kcd2s::transport::encode_wait_status;
  using kcd2s::transport::JoinIdentity;
  using kcd2s::transport::WaitField;
  using kcd2s::transport::WaitStatus;
  WaitStatus status;
  status.echo = 77;
  status.place = 4;
  status.waited_seconds = 161;
  status.line = "Queue position: 4";
  status.fields = {{"your place in line", "4"}, {"priority", "VIP"}};
  const auto decoded = decode_wait_status(encode_wait_status(status));
  KCD2S_CHECK(decoded && *decoded == status);
  // At most three fields travel; a field without a label does not; long texts are cut.
  status.fields = {{"a", "1"}, {"", "x"}, {"b", "2"}, {"c", "3"}, {"d", "4"}};
  status.line = std::string(400, 'x');
  const auto cut = decode_wait_status(encode_wait_status(status));
  KCD2S_CHECK(cut && cut->fields.size() == 3 && cut->fields[1].label == "b" &&
              cut->line.size() == kcd2s::transport::kMaxWaitLineBytes);
  auto bytes = encode_wait_status(status);
  bytes.push_back(0);
  KCD2S_CHECK(!decode_wait_status(bytes));
  JoinIdentity identity{9, "Jindřich", "76561198000000000"};
  const auto heard = decode_join_identity(encode_join_identity(identity));
  KCD2S_CHECK(heard && *heard == identity);
  identity.steam_id = "not digits";
  const auto cleaned = decode_join_identity(encode_join_identity(identity));
  KCD2S_CHECK(cleaned && cleaned->steam_id.empty());
  // The Master's licence pass travels when the client has one.
  identity.steam_id = "76561198000000000";
  identity.licence = "eyJ2IjoxfQ.c2lnbmF0dXJl-_";
  const auto passed = decode_join_identity(encode_join_identity(identity));
  KCD2S_CHECK(passed && *passed == identity);
  identity.licence = "not a pass!";
  const auto dropped = decode_join_identity(encode_join_identity(identity));
  KCD2S_CHECK(dropped && dropped->licence.empty());
  identity.licence = std::string(kcd2s::transport::kMaxIdentityLicenceBytes + 1, 'a');
  const auto long_pass = decode_join_identity(encode_join_identity(identity));
  KCD2S_CHECK(long_pass && long_pass->licence.empty());
  // The Discord id and hardware id travel after the pass, also without one.
  identity.licence.clear();
  identity.discord_id = "1563839669496578088";
  identity.hardware_id = std::string(64, 'a');
  const auto reported = decode_join_identity(encode_join_identity(identity));
  KCD2S_CHECK(reported && *reported == identity);
  identity.licence = "eyJ2IjoxfQ.c2lnbmF0dXJl-_";
  const auto all = decode_join_identity(encode_join_identity(identity));
  KCD2S_CHECK(all && *all == identity);
  identity.discord_id = "not digits";
  identity.hardware_id = "ABC";
  const auto cleaned_extras = decode_join_identity(encode_join_identity(identity));
  KCD2S_CHECK(cleaned_extras && cleaned_extras->discord_id.empty() &&
              cleaned_extras->hardware_id.empty() && cleaned_extras->licence == identity.licence);
  // The password proof travels last, also without the other extras.
  kcd2s::transport::JoinIdentity guarded;
  guarded.echo = 7;
  guarded.nickname = "Henry";
  guarded.password_proof = std::string(64, 'b');
  const auto proof_only = decode_join_identity(encode_join_identity(guarded));
  KCD2S_CHECK(proof_only && *proof_only == guarded);
  guarded.licence = "eyJ2IjoxfQ.c2lnbmF0dXJl-_";
  guarded.hardware_id = std::string(64, 'c');
  const auto proof_all = decode_join_identity(encode_join_identity(guarded));
  KCD2S_CHECK(proof_all && *proof_all == guarded);
  // Not a proof's form: not sent.
  guarded.password_proof = "hunter2";
  const auto no_proof = decode_join_identity(encode_join_identity(guarded));
  KCD2S_CHECK(no_proof && no_proof->password_proof.empty() &&
              no_proof->hardware_id == guarded.hardware_id);
  // A proof that is not 64 hex characters on the wire refuses the whole identity.
  guarded.password_proof = std::string(64, 'd');
  auto tampered = encode_join_identity(guarded);
  tampered[tampered.size() - 1] = 'Z';
  KCD2S_CHECK(!decode_join_identity(tampered));
}

// the launcher's join token travels last, with or without a password proof.
KCD2S_TEST(handshake, join_identity_carries_the_launchers_join_token) {
  using kcd2s::transport::decode_join_identity;
  using kcd2s::transport::encode_join_identity;
  kcd2s::transport::JoinIdentity joining;
  joining.echo = 9;
  joining.nickname = "Henry";
  joining.join_token = "one-time.ticket_42:AbC+/=";
  const auto token_only = decode_join_identity(encode_join_identity(joining));
  KCD2S_CHECK(token_only && *token_only == joining);
  joining.password_proof = std::string(64, 'e');
  joining.hardware_id = std::string(64, 'f');
  const auto with_proof = decode_join_identity(encode_join_identity(joining));
  KCD2S_CHECK(with_proof && *with_proof == joining);
  // Not a token's form: not sent.
  joining.join_token = "has space";
  const auto no_token = decode_join_identity(encode_join_identity(joining));
  KCD2S_CHECK(no_token && no_token->join_token.empty() &&
              no_token->password_proof == joining.password_proof);
  joining.join_token = std::string(256, 'a');
  const auto too_long = decode_join_identity(encode_join_identity(joining));
  KCD2S_CHECK(too_long && too_long->join_token.empty());
  // A token that is not a token's form on the wire refuses the whole identity.
  joining.join_token = "abc";
  auto tampered = encode_join_identity(joining);
  tampered[tampered.size() - 1] = ' ';
  KCD2S_CHECK(!decode_join_identity(tampered));
  // An empty password proof with nothing after it is not a valid identity.
  joining.password_proof.clear();
  auto cut = encode_join_identity(joining);
  cut.resize(cut.size() - 4);
  KCD2S_CHECK(!decode_join_identity(cut));
}

KCD2S_TEST(handshake, client_keeps_the_wait_status_of_its_own_response) {
  ConnectionGate gate(0xA11CE5EC1234u);
  ClientHandshake client(0xC0FFEEu);
  const std::uint64_t addr = 0xABCDu;
  std::vector<std::uint8_t> challenge;
  KCD2S_CHECK(gate.on_datagram(addr, client.start(), challenge));
  std::vector<std::uint8_t> response;
  KCD2S_CHECK(client.on_datagram(challenge, response));
  const auto nonce = gate.verified_response(addr, response);
  KCD2S_CHECK(nonce && *nonce == client.server_nonce());
  KCD2S_CHECK(!gate.verified_response(addr + 1, response));
  kcd2s::transport::WaitStatus other;
  other.echo = *nonce + 1;
  other.place = 2;
  std::vector<std::uint8_t> none;
  client.on_datagram(kcd2s::transport::encode_wait_status(other), none);
  KCD2S_CHECK(!client.wait_status());
  kcd2s::transport::WaitStatus own = other;
  own.echo = *nonce;
  client.on_datagram(kcd2s::transport::encode_wait_status(own), none);
  KCD2S_CHECK(client.wait_status() && client.wait_status()->place == 2);
  // A new attempt forgets it.
  client.start();
  KCD2S_CHECK(!client.wait_status());
}
