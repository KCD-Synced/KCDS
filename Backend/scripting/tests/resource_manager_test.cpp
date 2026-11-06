#include "kcd2s/scripting/resource_manager.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "kcd2s/scripting/host.hpp"
#include "kcd2s/scripting/resource.hpp"
#include "kcd2s/scripting/runtime.hpp"
#include "kcd2s/sdk/value.hpp"
#include "kcd2s/testkit.hpp"

using kcd2s::scripting::parse_manifest;
using kcd2s::scripting::ResourceHandle;
using kcd2s::scripting::ResourceHost;
using kcd2s::scripting::ResourceInfo;
using kcd2s::scripting::ResourceManager;
using kcd2s::scripting::ResourceManifest;
using kcd2s::scripting::ResourceRegistry;
using kcd2s::scripting::RuntimeStats;
using kcd2s::sdk::Value;

static ResourceManifest manifest(const std::string& text) {
  return parse_manifest(text).manifest;
}

// Records Load/Start/Stop calls so tests can assert the runtime was driven in order.
struct FakeRuntime : kcd2s::scripting::IScriptRuntime {
  std::vector<std::string> log;
  bool LoadResource(const ResourceInfo& info, ResourceHandle) override {
    log.push_back("load:" + info.name + ":" + info.entry);
    return true;
  }
  void Start(ResourceHandle h) override {
    log.push_back("start:" + std::to_string(h));
  }
  void Stop(ResourceHandle h) override {
    log.push_back("stop:" + std::to_string(h));
  }
  void Tick(ResourceHandle, double) override {
  }
  Value InvokeExport(ResourceHandle, const std::string&, const Value::Array&) override {
    return Value::null();
  }
  void DispatchEvent(ResourceHandle, const std::string&, const Value::Array&) override {
  }
  RuntimeStats Stats(ResourceHandle) override {
    return {};
  }
};

KCD2S_TEST(resource_manager, start_all_follows_topo_order) {
  ResourceRegistry reg;
  std::string err;
  reg.add(manifest(R"(name "c" version "1" dependency "b")"), err);
  reg.add(manifest(R"(name "b" version "1" dependency "a")"), err);
  reg.add(manifest(R"(name "a" version "1")"), err);

  ResourceHost host;
  ResourceManager mgr(reg, host);
  KCD2S_CHECK(mgr.start_all(err));

  const auto& order = mgr.start_order();
  KCD2S_CHECK_EQ(order.size(), std::size_t{3});
  auto idx = [&](const std::string& n) {
    return std::find(order.begin(), order.end(), n) - order.begin();
  };
  KCD2S_CHECK(idx("a") < idx("b"));
  KCD2S_CHECK(idx("b") < idx("c"));
  KCD2S_CHECK(mgr.started("a") && mgr.started("b") && mgr.started("c"));
}

KCD2S_TEST(resource_manager, exports_and_handlers_live_only_between_start_and_stop) {
  ResourceRegistry reg;
  std::string err;
  reg.add(manifest(R"(name "svc" version "1" export "ping")"), err);

  ResourceHost host;
  ResourceManager mgr(reg, host);
  // The start hook binds the resource's export + an event handler.
  int hits = 0;
  mgr.set_start_hook([&](const ResourceManifest& m, ResourceHandle h) {
    host.register_export(h, "ping", [](const Value::Array&) { return Value(std::int64_t{42}); });
    host.on(h, "tick", [&hits](const Value::Array&) { ++hits; });
    (void)m;
    return true;
  });

  // Before start: no export, no handler.
  KCD2S_CHECK(!host.call_export("svc", "ping").has_value());
  KCD2S_CHECK_EQ(host.emit("tick"), std::size_t{0});

  KCD2S_CHECK(mgr.start_all(err));
  auto r = host.call_export("svc", "ping");
  KCD2S_CHECK(r.has_value() && r->as_int() == 42);
  KCD2S_CHECK_EQ(host.emit("tick"), std::size_t{1});
  KCD2S_CHECK_EQ(hits, 1);

  // After stop: export + handler removed (crash isolation).
  mgr.stop_all();
  KCD2S_CHECK(!host.call_export("svc", "ping").has_value());
  KCD2S_CHECK_EQ(host.emit("tick"), std::size_t{0});
  KCD2S_CHECK(!mgr.started("svc"));
}

KCD2S_TEST(resource_manager, stop_all_is_reverse_order) {
  ResourceRegistry reg;
  std::string err;
  reg.add(manifest(R"(name "c" version "1" dependency "b")"), err);
  reg.add(manifest(R"(name "b" version "1" dependency "a")"), err);
  reg.add(manifest(R"(name "a" version "1")"), err);

  ResourceHost host;
  // Record stop order via the stop hook.
  std::vector<std::string> stopped;
  ResourceManager mgr(reg, host);
  mgr.set_stop_hook([&](const ResourceManifest& m, ResourceHandle) { stopped.push_back(m.name); });

  KCD2S_CHECK(mgr.start_all(err));
  const std::vector<std::string> start_order = mgr.start_order();  // a, b, c
  mgr.stop_all();

  std::vector<std::string> expected(start_order.rbegin(), start_order.rend());
  KCD2S_CHECK(stopped == expected);  // reverse of start order
}

KCD2S_TEST(resource_manager, drives_runtime_load_start_stop) {
  ResourceRegistry reg;
  std::string err;
  reg.add(manifest(R"(name "r" version "1" client_script "a.lua" server_script "b.js")"), err);

  ResourceHost host;
  FakeRuntime rt;
  ResourceManager mgr(reg, host, &rt);
  KCD2S_CHECK(mgr.start_all(err));
  const ResourceHandle h = mgr.handle_of("r");
  // Both scripts loaded, then started.
  KCD2S_CHECK(std::find(rt.log.begin(), rt.log.end(), "load:r:a.lua") != rt.log.end());
  KCD2S_CHECK(std::find(rt.log.begin(), rt.log.end(), "load:r:b.js") != rt.log.end());
  KCD2S_CHECK(std::find(rt.log.begin(), rt.log.end(), "start:" + std::to_string(h)) !=
              rt.log.end());
  mgr.stop_all();
  KCD2S_CHECK(std::find(rt.log.begin(), rt.log.end(), "stop:" + std::to_string(h)) != rt.log.end());
}

KCD2S_TEST(resource_manager, start_all_fails_cleanly_on_cycle) {
  ResourceRegistry reg;
  std::string err;
  reg.add(manifest(R"(name "a" version "1" dependency "b")"), err);
  reg.add(manifest(R"(name "b" version "1" dependency "a")"), err);

  ResourceHost host;
  ResourceManager mgr(reg, host);
  KCD2S_CHECK(!mgr.start_all(err));                          // cycle
  KCD2S_CHECK_EQ(mgr.start_order().size(), std::size_t{0});  // nothing started
  KCD2S_CHECK_EQ(host.resource_count(), std::size_t{0});
}

KCD2S_TEST(resource_manager, start_one_pulls_in_dependencies) {
  ResourceRegistry reg;
  std::string err;
  reg.add(manifest(R"(name "app" version "1" dependency "lib")"), err);
  reg.add(manifest(R"(name "lib" version "1" dependency "base")"), err);
  reg.add(manifest(R"(name "base" version "1")"), err);
  reg.add(manifest(R"(name "unrelated" version "1")"), err);

  ResourceHost host;
  ResourceManager mgr(reg, host);
  KCD2S_CHECK(mgr.start_one("app", err));
  KCD2S_CHECK(mgr.started("app") && mgr.started("lib") && mgr.started("base"));
  KCD2S_CHECK(!mgr.started("unrelated"));  // not pulled in
  const auto& o = mgr.start_order();
  auto idx = [&](const std::string& n) { return std::find(o.begin(), o.end(), n) - o.begin(); };
  KCD2S_CHECK(idx("base") < idx("lib"));
  KCD2S_CHECK(idx("lib") < idx("app"));
}

KCD2S_TEST(resource_manager, stop_one_stops_dependents_first) {
  ResourceRegistry reg;
  std::string err;
  reg.add(manifest(R"(name "app" version "1" dependency "lib")"), err);
  reg.add(manifest(R"(name "lib" version "1")"), err);

  ResourceHost host;
  std::vector<std::string> stopped;
  ResourceManager mgr(reg, host);
  mgr.set_stop_hook([&](const ResourceManifest& m, ResourceHandle) { stopped.push_back(m.name); });
  KCD2S_CHECK(mgr.start_all(err));

  mgr.stop_one("lib");  // app depends on lib -> app must stop first
  KCD2S_CHECK(!mgr.started("lib"));
  KCD2S_CHECK(!mgr.started("app"));
  KCD2S_CHECK_EQ(stopped.size(), std::size_t{2});
  KCD2S_CHECK_EQ(stopped[0], std::string("app"));  // dependent first
  KCD2S_CHECK_EQ(stopped[1], std::string("lib"));
}
