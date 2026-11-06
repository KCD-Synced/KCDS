#include "kcd2s/sdk/marshal.hpp"

#include <cstdint>

#include "kcd2s/sdk/value.hpp"
#include "kcd2s/testkit.hpp"

using namespace kcd2s::sdk;

static Value roundtrip(const Value& v) {
  auto buf = encode(v);
  auto res = decode(buf);
  return res.ok ? res.value : Value::null();
}

KCD2S_TEST(marshal, scalars_roundtrip) {
  KCD2S_CHECK(roundtrip(Value::null()).is_null());
  KCD2S_CHECK_EQ(roundtrip(Value(true)).as_bool(), true);
  KCD2S_CHECK_EQ(roundtrip(Value(std::int64_t{-123456789})).as_int(), std::int64_t{-123456789});
  KCD2S_CHECK_EQ(roundtrip(Value(3.14159)).as_double(), 3.14159);
  // Bind to a local: as_str() returns a reference into the Value, which must outlive the compare.
  const Value s = roundtrip(Value("hello world"));
  KCD2S_CHECK_EQ(s.as_str(), std::string("hello world"));
}

KCD2S_TEST(marshal, int_full_range) {
  for (std::int64_t v : {INT64_MIN, std::int64_t{-1}, std::int64_t{0}, std::int64_t{1}, INT64_MAX})
    KCD2S_CHECK_EQ(roundtrip(Value(v)).as_int(), v);
}

KCD2S_TEST(marshal, bytes_roundtrip) {
  Value::Bytes b = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0xFF};
  auto out = roundtrip(Value::bytes(b));
  KCD2S_CHECK(out.tag() == Tag::Bytes);
  KCD2S_CHECK(out.as_bytes() == b);
}

KCD2S_TEST(marshal, entity_and_func_refs) {
  auto e = roundtrip(Value(EntityRef{0xABCD1234u}));
  KCD2S_CHECK(e.tag() == Tag::EntityRef);
  KCD2S_CHECK_EQ(e.as_entity().id, kcd2s::core::EntityId{0xABCD1234u});

  auto f = roundtrip(Value(FuncRef{7, 42}));
  KCD2S_CHECK(f.tag() == Tag::FuncRef);
  KCD2S_CHECK((f.as_func() == FuncRef{7, 42}));
}

KCD2S_TEST(marshal, nested_array_and_map) {
  // Mimics a cross-language event payload: ["spawn", {id: entity, pos: [1,2,3], named: true}]
  Value::Map inner = {
      {"id", Value(EntityRef{99})},
      {"pos", Value(Value::Array{Value(1.0), Value(2.0), Value(3.0)})},
      {"named", Value(true)},
  };
  Value::Array args = {Value("spawn"), Value(inner)};
  Value payload{args};

  Value out = roundtrip(payload);
  KCD2S_CHECK(out.tag() == Tag::Array);
  KCD2S_CHECK_EQ(out.as_array().size(), std::size_t{2});
  KCD2S_CHECK_EQ(out.as_array()[0].as_str(), std::string("spawn"));
  const auto& m = out.as_array()[1].as_map();
  KCD2S_CHECK_EQ(m.size(), std::size_t{3});
  KCD2S_CHECK_EQ(m[0].first, std::string("id"));
  KCD2S_CHECK_EQ(m[0].second.as_entity().id, kcd2s::core::EntityId{99});
  KCD2S_CHECK_EQ(m[1].second.as_array().size(), std::size_t{3});
  KCD2S_CHECK_EQ(m[2].second.as_bool(), true);
  KCD2S_CHECK(out == payload);  // deep equality
}

KCD2S_TEST(marshal, decode_reports_consumed) {
  auto buf = encode(Value(std::int64_t{42}));
  auto res = decode(buf);
  KCD2S_CHECK(res.ok);
  KCD2S_CHECK_EQ(res.consumed, buf.size());
}

KCD2S_TEST(marshal, rejects_truncated) {
  auto buf = encode(Value("abcdef"));
  buf.resize(2);  // chop mid-string
  KCD2S_CHECK(!decode(buf).ok);
}

KCD2S_TEST(marshal, rejects_bad_tag) {
  std::vector<std::uint8_t> buf = {0x7F};  // tag far beyond FuncRef
  KCD2S_CHECK(!decode(buf).ok);
}

KCD2S_TEST(marshal, rejects_empty) {
  KCD2S_CHECK(!decode(nullptr, 0).ok);
}
