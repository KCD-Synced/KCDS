#include "kcd2s/sdk/nui.hpp"

#include <string>
#include <vector>

#include "kcd2s/sdk/marshal.hpp"
#include "kcd2s/sdk/value.hpp"
#include "kcd2s/testkit.hpp"

using kcd2s::sdk::NuiBus;
using kcd2s::sdk::Value;
using kcd2s::sdk::value_to_json;

KCD2S_TEST(nui, value_to_json_covers_types) {
  KCD2S_CHECK_EQ(value_to_json(Value::null()), std::string("null"));
  KCD2S_CHECK_EQ(value_to_json(Value(true)), std::string("true"));
  KCD2S_CHECK_EQ(value_to_json(Value(std::int64_t{-7})), std::string("-7"));
  KCD2S_CHECK_EQ(value_to_json(Value(std::string("hi \"there\"\n"))),
                 std::string("\"hi \\\"there\\\"\\n\""));

  Value::Map m;
  m.emplace_back("k", Value(std::int64_t{1}));
  m.emplace_back("arr", Value(Value::Array{Value(true), Value::null()}));
  const std::string j = value_to_json(Value(std::move(m)));
  KCD2S_CHECK_EQ(j, std::string("{\"k\":1,\"arr\":[true,null]}"));  // map order preserved
}

KCD2S_TEST(nui, ui_registration_from_manifest_page) {
  NuiBus bus;
  KCD2S_CHECK(bus.ui_page("chat") == nullptr);
  bus.register_ui("chat", "html/index.html");
  KCD2S_CHECK(bus.ui_page("chat") != nullptr);
  KCD2S_CHECK_EQ(*bus.ui_page("chat"), std::string("html/index.html"));
  bus.unregister_ui("chat");
  KCD2S_CHECK(bus.ui_page("chat") == nullptr);
}

KCD2S_TEST(nui, resource_to_ui_message_captured) {
  NuiBus bus;
  std::vector<std::pair<std::string, Value>> sent;
  bus.set_outgoing([&](const std::string& res, const Value& v) { sent.emplace_back(res, v); });

  Value::Map m;
  m.emplace_back("type", Value(std::string("open")));
  bus.send_message("chat", Value(std::move(m)));

  KCD2S_CHECK_EQ(sent.size(), std::size_t{1});
  KCD2S_CHECK_EQ(sent[0].first, std::string("chat"));
  KCD2S_CHECK_EQ(value_to_json(sent[0].second), std::string("{\"type\":\"open\"}"));
}

KCD2S_TEST(nui, ui_to_resource_callback_dispatch) {
  NuiBus bus;
  int calls = 0;
  bus.register_callback("chat", "send", [&](const Value& body) {
    ++calls;
    // Echo back an ack carrying the received message length.
    std::string msg = body.tag() == kcd2s::sdk::Tag::Str ? body.as_str() : std::string();
    Value::Map ok;
    ok.emplace_back("ok", Value(true));
    ok.emplace_back("len", Value(static_cast<std::int64_t>(msg.size())));
    return Value(std::move(ok));
  });

  auto r = bus.invoke_callback("chat", "send", Value(std::string("hello")));
  KCD2S_CHECK(r.has_value());
  KCD2S_CHECK_EQ(calls, 1);
  KCD2S_CHECK_EQ(value_to_json(*r), std::string("{\"ok\":true,\"len\":5}"));

  // Unknown callback -> nullopt.
  KCD2S_CHECK(!bus.invoke_callback("chat", "missing", Value::null()).has_value());

  // clear_callbacks removes a resource's callbacks (crash isolation on stop).
  bus.clear_callbacks("chat");
  KCD2S_CHECK(!bus.invoke_callback("chat", "send", Value::null()).has_value());
}

KCD2S_TEST(nui, callback_raw_decodes_untrusted_body) {
  NuiBus bus;
  bus.register_callback("hud", "setHp", [](const Value& body) { return Value(body.as_int() * 2); });

  // Well-formed marshalled body.
  const auto good = kcd2s::sdk::encode(Value(std::int64_t{21}));
  auto r = bus.invoke_callback_raw("hud", "setHp", good.data(), good.size());
  KCD2S_CHECK(r.ok);
  KCD2S_CHECK_EQ(r.result.as_int(), std::int64_t{42});

  // Garbage body -> ok=false, no crash.
  const std::vector<std::uint8_t> junk = {0xFF, 0x01, 0x02, 0x03};
  KCD2S_CHECK(!bus.invoke_callback_raw("hud", "setHp", junk.data(), junk.size()).ok);
  // Empty body -> ok=false.
  KCD2S_CHECK(!bus.invoke_callback_raw("hud", "setHp", nullptr, 0).ok);
  // Valid body but unknown callback -> ok=false.
  KCD2S_CHECK(!bus.invoke_callback_raw("hud", "nope", good.data(), good.size()).ok);
}

KCD2S_TEST(nui, json_to_value_parses_shapes) {
  using kcd2s::sdk::json_to_value;
  auto r =
      json_to_value("  {\"k\":[1,2,3],\"s\":\"a\\u0041b\",\"f\":3.5,\"ok\":true,\"z\":null}  ");
  KCD2S_CHECK(r.ok);
  KCD2S_CHECK(r.value.tag() == kcd2s::sdk::Tag::Map);
  const auto& m = r.value.as_map();
  KCD2S_CHECK_EQ(m.size(), std::size_t{5});
  KCD2S_CHECK_EQ(m[0].first, std::string("k"));
  KCD2S_CHECK_EQ(m[0].second.as_array().size(), std::size_t{3});
  KCD2S_CHECK_EQ(m[1].second.as_str(), std::string("aAb"));  // \u0041 = 'A'
  KCD2S_CHECK(m[2].second.tag() == kcd2s::sdk::Tag::Double);
  KCD2S_CHECK(m[3].second.as_bool());
  KCD2S_CHECK(m[4].second.is_null());

  // Integers vs doubles.
  KCD2S_CHECK(json_to_value("42").value.tag() == kcd2s::sdk::Tag::Int);
  KCD2S_CHECK(json_to_value("-7").value.as_int() == -7);
  KCD2S_CHECK(json_to_value("3.14").value.tag() == kcd2s::sdk::Tag::Double);
  KCD2S_CHECK(json_to_value("1e3").value.tag() == kcd2s::sdk::Tag::Double);
}

KCD2S_TEST(nui, json_roundtrip_identity) {
  using kcd2s::sdk::EntityRef;
  using kcd2s::sdk::FuncRef;
  using kcd2s::sdk::json_to_value;
  Value::Map inner;
  inner.emplace_back("a", Value(std::int64_t{1}));
  inner.emplace_back("b", Value(std::string("x")));
  Value::Array arr;
  arr.push_back(Value::null());
  arr.push_back(Value(true));
  arr.push_back(Value(std::int64_t{-42}));
  arr.push_back(Value(3.5));
  arr.push_back(Value(std::string("hi \"q\"\n")));
  arr.push_back(Value(Value::Array{Value(std::int64_t{1}), Value(std::int64_t{2})}));
  arr.push_back(Value(std::move(inner)));
  arr.push_back(Value(EntityRef{9}));
  arr.push_back(Value(FuncRef{3, 4}));
  arr.push_back(Value::bytes({0, 255, 128}));
  const Value root(std::move(arr));

  const std::string json = value_to_json(root);
  auto back = json_to_value(json);
  KCD2S_CHECK(back.ok);
  KCD2S_CHECK(back.value == root);  // identity incl. wrappers (entity/func/bytes)

  // Integral double survives as a double (value_to_json appends .0).
  const std::string jd = value_to_json(Value(5.0));
  KCD2S_CHECK(json_to_value(jd).value.tag() == kcd2s::sdk::Tag::Double);
}

KCD2S_TEST(nui, json_rejects_malformed) {
  using kcd2s::sdk::json_to_value;
  for (const char* bad : {"", "{", "[1,2", "{\"k\":}", "nul", "123abc", "\"unterminated",
                          "\"\\ud800\"", "{\"k\" 1}", "[,]", "tru"}) {
    KCD2S_CHECK(!json_to_value(std::string(bad)).ok);
  }
  // Deeper than the depth guard -> rejected, no overflow.
  std::string deep;
  for (int i = 0; i < 200; ++i)
    deep.push_back('[');
  for (int i = 0; i < 200; ++i)
    deep.push_back(']');
  KCD2S_CHECK(!json_to_value(deep).ok);
}

KCD2S_TEST(nui, json_fuzz_never_crashes) {
  using kcd2s::sdk::json_to_value;
  std::uint64_t s = 0x1234ABCDu;
  auto nextb = [&]() -> char {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    // Bias toward JSON-ish bytes so the parser is exercised, not just rejected immediately.
    static const char alpha[] = "{}[]\":,0123456789truefalsn. \\\tx";
    return alpha[static_cast<unsigned>(s) % (sizeof(alpha) - 1)];
  };
  for (int i = 0; i < 8000; ++i) {
    const std::size_t n = static_cast<std::size_t>(nextb()) % 40 + 1;
    std::string buf;
    for (std::size_t j = 0; j < n; ++j)
      buf.push_back(nextb());
    auto r = json_to_value(buf);  // must not crash/hang (ASan)
    (void)r;
  }
}

KCD2S_TEST(nui, callback_via_json_body) {
  NuiBus bus;
  bus.register_callback("hud", "setName", [](const Value& body) {
    std::string name;
    if (body.tag() == kcd2s::sdk::Tag::Map)
      for (const auto& [k, v] : body.as_map())
        if (k == "name" && v.tag() == kcd2s::sdk::Tag::Str)
          name = v.as_str();
    return Value(static_cast<std::int64_t>(name.size()));
  });

  auto r = bus.invoke_callback_json("hud", "setName", "{\"name\":\"henry\"}");
  KCD2S_CHECK(r.ok);
  KCD2S_CHECK_EQ(r.result.as_int(), std::int64_t{5});

  // Malformed JSON body -> ok=false, no crash.
  KCD2S_CHECK(!bus.invoke_callback_json("hud", "setName", "{not json").ok);
  // Valid JSON but unknown callback -> ok=false.
  KCD2S_CHECK(!bus.invoke_callback_json("hud", "nope", "123").ok);
}

KCD2S_TEST(nui, focus_state_transitions) {
  NuiBus bus;
  KCD2S_CHECK(!bus.has_focus() && !bus.has_cursor());
  bus.set_focus(true, true);
  KCD2S_CHECK(bus.has_focus() && bus.has_cursor());
  bus.set_focus(true, false);  // focused, cursor hidden
  KCD2S_CHECK(bus.has_focus() && !bus.has_cursor());
  bus.set_focus(false, false);
  KCD2S_CHECK(!bus.has_focus() && !bus.has_cursor());
}
