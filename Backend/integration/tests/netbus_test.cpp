#include "kcd2s/integration/netbus.hpp"

#include <memory>
#include <string>

#include "kcd2s/sdk/value.hpp"
#include "kcd2s/testkit.hpp"
#include "kcd2s/transport/connection.hpp"
#include "kcd2s/transport/datagram.hpp"
#include "kcd2s/wire/packet.hpp"

using namespace kcd2s::integration;
using kcd2s::sdk::Value;
using kcd2s::transport::Connection;
using kcd2s::transport::make_sim_pair;

// Two peers A<->B over a SimLink; route channel-2 payloads into each NetBus and drive
// flush/receive.
struct Pair {
  std::unique_ptr<kcd2s::transport::SimLink> la, lb;
  std::unique_ptr<Connection> ca, cb;
  std::unique_ptr<NetBus> a, b;
  double t = 0.0;

  Pair() {
    auto p = make_sim_pair({}, {});
    la = std::move(p.first);
    lb = std::move(p.second);
    ca = std::make_unique<Connection>(la.get());
    cb = std::make_unique<Connection>(lb.get());
    a = std::make_unique<NetBus>(*ca);
    b = std::make_unique<NetBus>(*cb);
  }

  void pump(int rounds = 4) {
    const std::uint8_t ev = static_cast<std::uint8_t>(kcd2s::wire::Channel::Events);
    for (int i = 0; i < rounds; ++i) {
      ca->flush(t);
      cb->flush(t);
      for (auto& m : ca->receive(t))
        if (m.channel == ev)
          a->feed(m.bytes);
      for (auto& m : cb->receive(t))
        if (m.channel == ev)
          b->feed(m.bytes);
      t += 0.01;
    }
  }
};

KCD2S_TEST(netbus, event_b_to_a) {
  Pair p;
  std::string got;
  int calls = 0;
  p.a->on("chat", [&](const Value::Array& args) {
    ++calls;
    if (!args.empty() && args[0].tag() == kcd2s::sdk::Tag::Str)
      got = args[0].as_str();
  });
  p.b->emit("chat", {Value("hi from B")});
  p.pump();
  KCD2S_CHECK_EQ(calls, 1);
  KCD2S_CHECK_EQ(got, std::string("hi from B"));
}

KCD2S_TEST(netbus, event_carries_structured_args) {
  Pair p;
  Value::Map captured;
  bool hit = false;
  p.a->on("spawn", [&](const Value::Array& args) {
    hit = true;
    if (args.size() == 1 && args[0].tag() == kcd2s::sdk::Tag::Map)
      captured = args[0].as_map();
  });
  Value::Map payload = {{"id", Value(kcd2s::sdk::EntityRef{42})}, {"hp", Value(0.5)}};
  p.b->emit("spawn", {Value(payload)});
  p.pump();
  KCD2S_CHECK(hit);
  KCD2S_CHECK_EQ(captured.size(), std::size_t{2});
  KCD2S_CHECK_EQ(captured[0].second.as_entity().id, kcd2s::core::EntityId{42});
}

KCD2S_TEST(netbus, rpc_request_response) {
  Pair p;
  p.a->register_rpc("add", [](const Value::Array& args) -> Value {
    std::int64_t sum = 0;
    for (const auto& v : args)
      if (v.tag() == kcd2s::sdk::Tag::Int)
        sum += v.as_int();
    return Value(sum);
  });

  std::int64_t result = -1;
  p.b->call("add", {Value(std::int64_t{2}), Value(std::int64_t{40})},
            [&](const Value& r) { result = r.as_int(); });
  KCD2S_CHECK_EQ(p.b->pending_calls(), std::size_t{1});
  p.pump();  // request B->A, response A->B
  KCD2S_CHECK_EQ(result, std::int64_t{42});
  KCD2S_CHECK_EQ(p.b->pending_calls(), std::size_t{0});
}

KCD2S_TEST(netbus, rpc_unknown_returns_null) {
  Pair p;
  bool got = false;
  bool was_null = false;
  p.b->call("nope", {}, [&](const Value& r) {
    got = true;
    was_null = r.is_null();
  });
  p.pump();
  KCD2S_CHECK(got);
  KCD2S_CHECK(was_null);
}

KCD2S_TEST(netbus, malformed_payload_ignored) {
  Pair p;
  // Feeding garbage must not crash or dispatch.
  p.a->feed({0xFF, 0x00, 0x13});
  p.a->feed({});
  KCD2S_CHECK_EQ(p.a->pending_calls(), std::size_t{0});
}
