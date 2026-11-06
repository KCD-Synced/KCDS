// Decoder fuzz / property tests. Every decoder that touches the wire is fed
// seeded random and truncated bytes; the contract is "never crash / no UB (run under ASan+UBSan),
// always reject gracefully". Targets: wire::read_packet, sdk::decode, replication Mirror::apply,
// the server's channel-3 upstream decode, and the transport FragmentingLink reassembly. Each has a
// random-bytes loop plus hand-built adversarial inputs (huge length prefixes, deep nesting,
// truncations of a valid message) for the paths random bytes rarely reach.
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/replication/schema.hpp"
#include "kcd2s/replication/snapshot.hpp"
#include "kcd2s/replication/world.hpp"
#include "kcd2s/sdk/marshal.hpp"
#include "kcd2s/sdk/value.hpp"
#include "kcd2s/testkit.hpp"
#include "kcd2s/transport/connection.hpp"
#include "kcd2s/transport/datagram.hpp"
#include "kcd2s/transport/fragment.hpp"
#include "kcd2s/wire/packet.hpp"

using kcd2s::core::BitWriter;
using kcd2s::core::EntityId;
using kcd2s::core::FieldId;
using kcd2s::integration::Server;
using kcd2s::replication::Authority;
using kcd2s::replication::ClientRepl;
using kcd2s::replication::FieldKind;
using kcd2s::replication::Mirror;
using kcd2s::replication::Priority;
using kcd2s::replication::SchemaRegistry;
using kcd2s::replication::Vec3;
using kcd2s::replication::World;
using kcd2s::transport::Connection;
using kcd2s::transport::Datagram;
using kcd2s::transport::FragmentingLink;
using kcd2s::transport::IDatagramLink;
using kcd2s::transport::make_sim_pair;

namespace {

// Deterministic xorshift64* so every fuzz run reproduces exactly.
struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {
  }
  std::uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s * 0x2545F4914F6CDD1Dull;
  }
  std::uint8_t byte() {
    return static_cast<std::uint8_t>(next());
  }
  std::size_t upto(std::size_t n) {
    return n ? static_cast<std::size_t>(next() % n) : 0;
  }
};

std::vector<std::uint8_t> random_bytes(Rng& g, std::size_t n) {
  std::vector<std::uint8_t> v(n);
  for (auto& b : v)
    b = g.byte();
  return v;
}

// A schema whose first field is a Str, so the construct/field decoder hits the string path (the one
// that must bound an untrusted length).
SchemaRegistry fuzz_schema() {
  SchemaRegistry reg;
  reg.define("Thing")
      .field("name", FieldKind::Str, Priority::Med, Authority::Server)    // field 0 (Str!)
      .field("pos", FieldKind::Vec3, Priority::High, Authority::Owner)    // field 1
      .field("count", FieldKind::I64, Priority::Low, Authority::Server)   // field 2
      .field("flag", FieldKind::Bool, Priority::Low, Authority::Server);  // field 3
  return reg;
}

// Inbound-only link: recv() pops datagrams we injected; send()/poll() are inert.
class InjectLink : public IDatagramLink {
 public:
  using IDatagramLink::send;
  void send(const std::uint8_t*, std::size_t) override {
  }
  bool recv(Datagram& out) override {
    if (q.empty())
      return false;
    out = std::move(q.front());
    q.pop_front();
    return true;
  }
  void poll() override {
  }
  std::deque<Datagram> q;
};

}  // namespace

// ---------------------------------------------------------------------------- wire::read_packet

KCD2S_TEST(fuzz, wire_read_packet_random_bytes) {
  Rng g(0x1111);
  for (int i = 0; i < 4000; ++i) {
    auto buf = random_bytes(g, g.upto(96));
    kcd2s::wire::Packet p;
    if (kcd2s::wire::read_packet(buf, p)) {
      // If it accepted, the result must respect the structural bounds.
      KCD2S_CHECK(p.channels.size() <= kcd2s::wire::kMaxChannelsPerPacket);
    }
  }
}

KCD2S_TEST(fuzz, wire_read_packet_truncations) {
  kcd2s::wire::Packet p;
  p.peerSeq = 7;
  p.flags = kcd2s::wire::kFlagAckValid;
  for (std::uint8_t ch = 0; ch < 3; ++ch) {
    kcd2s::wire::ChannelPayload cp;
    cp.channel = ch;
    cp.bytes = {ch, static_cast<std::uint8_t>(ch + 1), static_cast<std::uint8_t>(ch + 2)};
    p.channels.push_back(cp);
  }
  const auto full = kcd2s::wire::write_packet(p);
  for (std::size_t n = 0; n <= full.size(); ++n) {
    kcd2s::wire::Packet out;
    kcd2s::wire::read_packet(full.data(), n, out);  // just must not crash
  }
  kcd2s::wire::Packet out;
  KCD2S_CHECK(kcd2s::wire::read_packet(full, out));  // the complete buffer parses
  KCD2S_CHECK_EQ(out.channels.size(), std::size_t{3});
}

// ---------------------------------------------------------------------------- sdk::decode

KCD2S_TEST(fuzz, sdk_decode_random_bytes) {
  Rng g(0x2222);
  for (int i = 0; i < 4000; ++i) {
    auto buf = random_bytes(g, g.upto(80));
    auto r = kcd2s::sdk::decode(buf);
    if (r.ok)
      KCD2S_CHECK(r.consumed <= buf.size());
  }
}

KCD2S_TEST(fuzz, sdk_decode_huge_length_rejected) {
  // Str tag then a 10-byte varuint encoding ~2^63 — the length guard must reject without a giant
  // allocation. (varuint: 9 groups of 0xFF + a final high group.)
  std::vector<std::uint8_t> buf = {static_cast<std::uint8_t>(kcd2s::sdk::Tag::Str)};
  for (int i = 0; i < 9; ++i)
    buf.push_back(0xFF);
  buf.push_back(0x7F);
  auto r = kcd2s::sdk::decode(buf);
  KCD2S_CHECK(!r.ok);

  // Same for Bytes.
  buf[0] = static_cast<std::uint8_t>(kcd2s::sdk::Tag::Bytes);
  KCD2S_CHECK(!kcd2s::sdk::decode(buf).ok);
}

KCD2S_TEST(fuzz, sdk_decode_deep_nesting_rejected) {
  // 200 nested 1-element arrays: deeper than the decoder's depth guard -> reject, never overflow.
  std::vector<std::uint8_t> buf;
  for (int i = 0; i < 200; ++i) {
    buf.push_back(static_cast<std::uint8_t>(kcd2s::sdk::Tag::Array));
    buf.push_back(0x01);  // varuint 1 (one element)
  }
  buf.push_back(static_cast<std::uint8_t>(kcd2s::sdk::Tag::Null));
  auto r = kcd2s::sdk::decode(buf);
  KCD2S_CHECK(!r.ok);  // depth guard trips; no stack overflow
}

KCD2S_TEST(fuzz, sdk_decode_truncations_roundtrip) {
  using kcd2s::sdk::EntityRef;
  using kcd2s::sdk::FuncRef;
  using kcd2s::sdk::Value;
  Value::Map m;
  m.reserve(2);
  m.emplace_back("k1", Value(std::int64_t{-42}));
  m.emplace_back("k2", Value(std::string("hello")));
  Value::Array arr;
  arr.reserve(
      8);  // avoid reallocation (sidesteps a GCC -Wmaybe-uninitialized variant-move warning)
  arr.push_back(Value(true));
  arr.push_back(Value::null());
  arr.push_back(Value(3.14159));
  arr.push_back(Value(std::string("a longer string payload")));
  arr.push_back(Value(std::move(m)));
  arr.push_back(Value(EntityRef{7}));
  arr.push_back(Value(FuncRef{1, 2}));
  arr.push_back(Value::bytes({1, 2, 3, 4, 5}));
  const Value root(std::move(arr));

  const auto enc = kcd2s::sdk::encode(root);
  for (std::size_t n = 0; n < enc.size(); ++n) {
    auto r = kcd2s::sdk::decode(enc.data(), n);  // every short prefix: no crash
    if (r.ok)
      KCD2S_CHECK(r.consumed <= n);
  }
  auto full = kcd2s::sdk::decode(enc);
  KCD2S_CHECK(full.ok);
  KCD2S_CHECK(full.value == root);
  KCD2S_CHECK_EQ(full.consumed, enc.size());
}

// ---------------------------------------------------------------------------- Mirror::apply

KCD2S_TEST(fuzz, mirror_apply_random_bytes) {
  SchemaRegistry reg = fuzz_schema();
  Mirror mir(reg);
  Rng g(0x3333);
  for (int i = 0; i < 4000; ++i) {
    auto buf = random_bytes(g, g.upto(120));
    mir.apply(buf.data(), buf.size());  // bool result irrelevant; must not crash/hang
  }
  // Still usable after all that garbage: a real snapshot applies cleanly.
  World w(reg);
  w.set_tick(1);
  const EntityId e = w.spawn(reg.get("Thing")->id);
  w.write(e, 0, std::string("ok"));
  w.write(e, 1, Vec3{1, 2, 3});
  ClientRepl cr;
  std::vector<EntityId> aoi{e};
  auto snap = kcd2s::replication::build_snapshot(w, w.tick(), aoi, cr);
  KCD2S_CHECK(mir.apply(snap));
  KCD2S_CHECK(mir.get(e) != nullptr);
}

KCD2S_TEST(fuzz, mirror_apply_huge_string_field_rejected) {
  // Hand-build a construct whose Str field (field 0) claims a ~2^63 length with no bytes behind it.
  // Before the field-codec fix this resized/looped on the bogus length; now it must reject.
  SchemaRegistry reg = fuzz_schema();
  const auto type = reg.get("Thing")->id;
  BitWriter w;
  w.write_bits(5, 32);  // snapTick
  w.write_bits(0, 32);  // baselineTick
  w.write_varuint(1);   // nConstruct
  w.write_varuint(1);   // entityId
  w.write_bits(type, 32);
  w.write_bits(0, 8);  // epoch
  // field 0 (Str): a 10-byte varuint ~2^63, then nothing.
  for (int i = 0; i < 9; ++i)
    w.write_bits(0xFF, 8);
  w.write_bits(0x7F, 8);
  const auto buf = w.bytes();

  Mirror mir(reg);
  KCD2S_CHECK(!mir.apply(buf));  // truncated/over-long -> rejected, no hang or bad_alloc
}

KCD2S_TEST(fuzz, mirror_apply_truncations) {
  SchemaRegistry reg = fuzz_schema();
  World w(reg);
  w.set_tick(3);
  const EntityId e = w.spawn(reg.get("Thing")->id);
  w.write(e, 0, std::string(200, 'x'));  // a long Str so the string-decode path spans many prefixes
  w.write(e, 1, Vec3{9, 8, 7});
  w.write(e, 2, std::int64_t{123456});
  w.write(e, 3, true);
  ClientRepl cr;
  std::vector<EntityId> aoi{e};
  const auto snap = kcd2s::replication::build_snapshot(w, w.tick(), aoi, cr);

  for (std::size_t n = 0; n < snap.size(); ++n) {
    Mirror mir(reg);
    mir.apply(snap.data(), n);  // every short prefix: graceful, no crash
  }
  Mirror full(reg);
  KCD2S_CHECK(full.apply(snap));  // the complete snapshot applies
  KCD2S_CHECK(full.get(e) != nullptr);
}

// ---------------------------------------------------------------------------- server upstream
// decode

KCD2S_TEST(fuzz, server_upstream_random_bytes) {
  SchemaRegistry reg = fuzz_schema();
  Server server(reg);
  World& w = server.world();
  w.set_tick(1);
  const EntityId thing = w.spawn(reg.get("Thing")->id);
  w.write(thing, 0, std::string("server-owned-name"));
  w.write(thing, 1, Vec3{0, 0, 0});
  w.write(thing, 2, std::int64_t{999});  // AUTH_SERVER: upstream must never change this

  auto pr = make_sim_pair({}, {});
  auto s_link = std::move(pr.first);
  auto c_link = std::move(pr.second);
  const auto pid = server.add_client(s_link.get(), thing, 1.0e6f);
  server.set_owner(thing, pid);  // owner set server-side immediately (epoch 0)
  (void)pid;

  Connection client(c_link.get());
  const std::uint8_t up = static_cast<std::uint8_t>(kcd2s::wire::Channel::Upstream);

  Rng g(0x4444);
  double now = 0.0;
  for (int i = 0; i < 3000; ++i) {
    auto buf = random_bytes(g, g.upto(64));
    client.queue(up, buf);
    client.flush(now);
    w.set_tick(w.tick() + 1);
    server.tick(now);  // decodes arbitrary upstream bytes under authority checks
    now += 0.016;
  }
  // The server survived, and no forged upstream ever wrote the AUTH_SERVER field.
  const auto* slot = w.slot(thing);
  KCD2S_CHECK(slot != nullptr);
  KCD2S_CHECK_EQ(std::get<std::int64_t>(*w.read(thing, 2)), std::int64_t{999});
}

// ---------------------------------------------------------------------------- FragmentingLink

KCD2S_TEST(fuzz, fragment_reassembly_random_datagrams) {
  InjectLink link;
  FragmentingLink fl(&link, 96);
  Rng g(0x5555);
  for (int i = 0; i < 4000; ++i) {
    // A mix of tiny, header-sized, and oversized random datagrams (some will parse as fragment
    // headers with wild msg/index/count/total fields).
    link.q.push_back(random_bytes(g, g.upto(40)));
    if ((i & 7) == 0)
      link.q.push_back(random_bytes(g, 11 + g.upto(60)));
    Datagram out;
    while (fl.recv(out)) {
    }  // drain whatever reassembled
    if ((i & 15) == 0)
      fl.poll();                                         // exercise the timeout sweep
    KCD2S_CHECK(fl.partial_count() <= std::size_t{64});  // bounded: no unbounded growth
  }
}

KCD2S_TEST(fuzz, fragment_truncated_headers) {
  InjectLink link;
  FragmentingLink fl(&link, 96);
  // Fragment tag (1) followed by every truncation of an 11-byte header -> must be ignored cleanly.
  for (std::size_t n = 1; n <= kcd2s::transport::kFragHeaderBytes; ++n) {
    Datagram d(n, 0xFF);
    d[0] = 1;  // fragment tag
    link.q.push_back(std::move(d));
  }
  Datagram out;
  KCD2S_CHECK(!fl.recv(out));  // nothing completes, nothing crashes
  KCD2S_CHECK_EQ(fl.partial_count(), std::size_t{0});
}
