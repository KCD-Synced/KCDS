#include "kcd2s/sdk/shared_state.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/sdk/value.hpp"
#include "kcd2s/testkit.hpp"

using kcd2s::sdk::BagAuthority;
using kcd2s::sdk::SharedState;
using kcd2s::sdk::SharedStateStore;
using kcd2s::sdk::Value;

KCD2S_TEST(shared_state, set_get_and_noop) {
  SharedState b;
  KCD2S_CHECK(b.get("hp") == nullptr);
  KCD2S_CHECK(b.set("hp", Value(std::int64_t{100})));  // new -> changed
  KCD2S_CHECK(b.get("hp") != nullptr);
  KCD2S_CHECK_EQ(b.get("hp")->as_int(), std::int64_t{100});
  KCD2S_CHECK(!b.set("hp", Value(std::int64_t{100})));  // same value -> no-op
  KCD2S_CHECK(b.set("hp", Value(std::int64_t{90})));    // changed
  KCD2S_CHECK_EQ(b.size(), std::size_t{1});
}

KCD2S_TEST(shared_state, dirty_tracking) {
  SharedState b;
  KCD2S_CHECK(!b.dirty());
  b.set("name", Value(std::string("Henry")));
  KCD2S_CHECK(b.dirty());
  b.clear_dirty();
  KCD2S_CHECK(!b.dirty());
  b.erase("name");
  KCD2S_CHECK(b.dirty());  // a removal is dirty too (tombstone)
  b.clear_dirty();
  KCD2S_CHECK(!b.erase("name"));  // already gone
  KCD2S_CHECK(!b.dirty());
}

KCD2S_TEST(shared_state, delta_roundtrip_sets_and_removes) {
  SharedState src;
  src.set("hp", Value(std::int64_t{100}));
  src.set("name", Value(std::string("Henry")));
  src.set("pos", Value(Value::Array{Value(1.5), Value(2.5), Value(3.5)}));

  SharedState dst;
  KCD2S_CHECK(dst.apply_delta(src.encode_delta()));
  KCD2S_CHECK_EQ(dst.size(), std::size_t{3});
  KCD2S_CHECK_EQ(dst.get("hp")->as_int(), std::int64_t{100});
  KCD2S_CHECK_EQ(dst.get("name")->as_str(), std::string("Henry"));
  KCD2S_CHECK_EQ(dst.get("pos")->as_array().size(), std::size_t{3});

  // A follow-up delta: change one, remove one.
  src.clear_dirty();
  src.set("hp", Value(std::int64_t{75}));
  src.erase("name");
  KCD2S_CHECK(dst.apply_delta(src.encode_delta()));
  KCD2S_CHECK_EQ(dst.get("hp")->as_int(), std::int64_t{75});
  KCD2S_CHECK(dst.get("name") == nullptr);  // removed
  KCD2S_CHECK_EQ(dst.size(), std::size_t{2});
}

KCD2S_TEST(shared_state, apply_fires_change_callbacks) {
  SharedState src;
  src.set("hp", Value(std::int64_t{100}));
  SharedState dst;
  dst.apply_delta(src.encode_delta());  // seed: hp=100

  struct Rec {
    std::string key;
    Value old_v, new_v;
  };
  std::vector<Rec> changes;
  auto cb = [&](const std::string& k, const Value& o, const Value& n) {
    changes.push_back({k, o, n});
  };

  src.clear_dirty();
  src.set("hp", Value(std::int64_t{40}));
  src.set("gold", Value(std::int64_t{7}));
  KCD2S_CHECK(dst.apply_delta(src.encode_delta(), cb));

  // hp changed 100->40, gold is new (null->7).
  KCD2S_CHECK_EQ(changes.size(), std::size_t{2});
  bool saw_hp = false, saw_gold = false;
  for (const auto& c : changes) {
    if (c.key == "hp") {
      saw_hp = true;
      KCD2S_CHECK_EQ(c.old_v.as_int(), std::int64_t{100});
      KCD2S_CHECK_EQ(c.new_v.as_int(), std::int64_t{40});
    } else if (c.key == "gold") {
      saw_gold = true;
      KCD2S_CHECK(c.old_v.is_null());
      KCD2S_CHECK_EQ(c.new_v.as_int(), std::int64_t{7});
    }
  }
  KCD2S_CHECK(saw_hp && saw_gold);

  // A removal reports (old -> null).
  std::vector<Rec> rem;
  auto cb2 = [&](const std::string& k, const Value& o, const Value& n) {
    rem.push_back({k, o, n});
  };
  src.clear_dirty();
  src.erase("gold");
  KCD2S_CHECK(dst.apply_delta(src.encode_delta(), cb2));
  KCD2S_CHECK_EQ(rem.size(), std::size_t{1});
  KCD2S_CHECK_EQ(rem[0].key, std::string("gold"));
  KCD2S_CHECK(rem[0].new_v.is_null());
  KCD2S_CHECK(dst.get("gold") == nullptr);
}

KCD2S_TEST(shared_state, server_authority_rejects_client_forge) {
  SharedStateStore store;
  // Server creates a server-authority bag and writes it.
  KCD2S_CHECK(store.author_write("global", "mission", Value(std::string("rattay")), true));
  KCD2S_CHECK(store.find("global") != nullptr);
  KCD2S_CHECK_EQ(store.find("global")->get("mission")->as_str(), std::string("rattay"));

  // A client cannot author the server-owned bag.
  KCD2S_CHECK(!store.author_write("global", "mission", Value(std::string("hacked")), false));
  KCD2S_CHECK_EQ(store.find("global")->get("mission")->as_str(), std::string("rattay"));

  // A Client-authority bag accepts client writes.
  store.bag("player:3:ui", BagAuthority::Client);
  KCD2S_CHECK(store.author_write("player:3:ui", "menu_open", Value(true), false));
  KCD2S_CHECK(store.find("player:3:ui")->get("menu_open")->as_bool());
}

KCD2S_TEST(shared_state, apply_delta_fuzz_rejects_garbage) {
  // Seeded random + hand-built adversarial buffers must never crash and must reject cleanly.
  std::uint64_t s = 0xBA6F00D1234ull;
  auto nextb = [&]() -> std::uint8_t {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<std::uint8_t>(s);
  };
  for (int i = 0; i < 4000; ++i) {
    const std::size_t n = static_cast<std::size_t>(nextb()) % 48;
    std::vector<std::uint8_t> buf(n);
    for (auto& x : buf)
      x = nextb();
    SharedState b;
    b.apply_delta(buf);  // bool result irrelevant; must not crash/hang (run under ASan)
  }
  // A huge key length must be rejected, not allocated.
  SharedState b;
  std::vector<std::uint8_t> bad = {0x01};  // nSet = 1
  for (int i = 0; i < 9; ++i)
    bad.push_back(0xFF);  // key length ~2^63
  bad.push_back(0x7F);
  KCD2S_CHECK(!b.apply_delta(bad));
  // Trailing garbage after a valid entry is rejected.
  SharedState src;
  src.set("k", Value(std::int64_t{1}));
  auto good = src.encode_delta();
  good.push_back(0xAB);  // extra byte
  SharedState b2;
  KCD2S_CHECK(!b2.apply_delta(good));
}
