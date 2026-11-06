#include "kcd2s/scripting/host.hpp"

#include <string>
#include <vector>

#include "kcd2s/sdk/value.hpp"
#include "kcd2s/testkit.hpp"

using kcd2s::scripting::ResourceHost;
using kcd2s::sdk::Value;

KCD2S_TEST(host, emit_reaches_other_resource) {
  ResourceHost host;
  const auto a = host.add_resource("resA");  // e.g. a JS resource
  const auto b = host.add_resource("resB");  // e.g. a C# resource

  std::string got;
  host.on(b, "chat", [&](const Value::Array& args) {
    if (!args.empty())
      got = args[0].as_str();
  });

  const std::size_t n = host.emit("chat", {Value("hello from A")});
  KCD2S_CHECK_EQ(n, std::size_t{1});
  KCD2S_CHECK_EQ(got, std::string("hello from A"));
  (void)a;
}

KCD2S_TEST(host, handlers_run_in_registration_order) {
  ResourceHost host;
  const auto a = host.add_resource("a");
  const auto b = host.add_resource("b");
  std::vector<int> order;
  host.on(a, "tick", [&](const Value::Array&) { order.push_back(1); });
  host.on(b, "tick", [&](const Value::Array&) { order.push_back(2); });
  host.on(a, "tick", [&](const Value::Array&) { order.push_back(3); });
  host.emit("tick");
  KCD2S_CHECK_EQ(order.size(), std::size_t{3});
  KCD2S_CHECK(order[0] == 1 && order[1] == 2 && order[2] == 3);
}

KCD2S_TEST(host, unmatched_event_invokes_nothing) {
  ResourceHost host;
  const auto a = host.add_resource("a");
  host.on(a, "foo", [&](const Value::Array&) { KCD2S_CHECK(false); });
  KCD2S_CHECK_EQ(host.emit("bar"), std::size_t{0});
}

KCD2S_TEST(host, exports_cross_resource_by_name) {
  ResourceHost host;
  const auto bank = host.add_resource("core-economy");
  host.add_resource("job-courier");

  host.register_export(bank, "pay", [](const Value::Array& args) -> Value {
    const std::int64_t amount = args.empty() ? 0 : args[0].as_int();
    return Value(amount * 2);  // trivial deterministic transform
  });

  auto r = host.call_export("core-economy", "pay", {Value(std::int64_t{21})});
  KCD2S_CHECK(r.has_value());
  KCD2S_CHECK_EQ(r->as_int(), std::int64_t{42});

  KCD2S_CHECK(!host.call_export("core-economy", "missing").has_value());
  KCD2S_CHECK(!host.call_export("no-such-resource", "pay").has_value());
}

KCD2S_TEST(host, stop_removes_handlers_and_exports) {
  ResourceHost host;
  const auto a = host.add_resource("a");
  const auto b = host.add_resource("b");
  int b_calls = 0;
  host.on(b, "evt", [&](const Value::Array&) { ++b_calls; });
  host.register_export(a, "f", [](const Value::Array&) { return Value(std::int64_t{1}); });

  host.emit("evt");
  KCD2S_CHECK_EQ(b_calls, 1);
  KCD2S_CHECK(host.call_export("a", "f").has_value());

  host.stop_resource(a);
  KCD2S_CHECK(!host.call_export("a", "f").has_value());  // export gone
  KCD2S_CHECK_EQ(host.resource_count(), std::size_t{1});

  host.stop_resource(b);
  KCD2S_CHECK_EQ(host.emit("evt"), std::size_t{0});  // handler gone
  KCD2S_CHECK_EQ(b_calls, 1);                        // not invoked again
}

KCD2S_TEST(host, handler_stopping_a_resource_mid_emit_is_safe) {
  ResourceHost host;
  const auto a = host.add_resource("a");
  const auto b = host.add_resource("b");
  int b_calls = 0;
  // a's handler stops b during the same emit; b's handler must still have been snapshotted.
  host.on(a, "boom", [&](const Value::Array&) { host.stop_resource(b); });
  host.on(b, "boom", [&](const Value::Array&) { ++b_calls; });
  const std::size_t n = host.emit("boom");
  KCD2S_CHECK_EQ(n, std::size_t{2});  // both ran, no crash/invalidation
  KCD2S_CHECK_EQ(b_calls, 1);
  KCD2S_CHECK(!host.alive(b));
}
