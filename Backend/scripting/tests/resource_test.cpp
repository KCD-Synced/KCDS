#include "kcd2s/scripting/resource.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "kcd2s/testkit.hpp"

using kcd2s::scripting::parse_manifest;
using kcd2s::scripting::ResourceManifest;
using kcd2s::scripting::ResourceRegistry;
using kcd2s::scripting::ScriptLang;
using kcd2s::scripting::ScriptSide;

// Helper: parse a manifest the tests know is valid (validity itself is checked in the parse tests).
static ResourceManifest must_parse(const std::string& text) {
  return parse_manifest(text).manifest;
}

KCD2S_TEST(resource, parses_well_formed_manifest) {
  const char* text = R"(
    # a chat resource
    name "chat"
    version "1.2.0"
    author "someone"
    description "in-game chat"
    client_scripts { "cl_main.lua" "cl_ui.ts" }
    server_script "sv_main.js"
    shared_script "shared.lua"
    files { "html/index.html" "html/app.js" }
    dependency "base"
    exports { "sendMessage" "clear" }
  )";
  auto r = parse_manifest(text);
  KCD2S_CHECK(r.ok);
  const ResourceManifest& m = r.manifest;
  KCD2S_CHECK_EQ(m.name, std::string("chat"));
  KCD2S_CHECK_EQ(m.version, std::string("1.2.0"));
  KCD2S_CHECK_EQ(m.author, std::string("someone"));
  KCD2S_CHECK_EQ(m.scripts.size(), std::size_t{4});  // 2 client + 1 server + 1 shared
  KCD2S_CHECK_EQ(m.files.size(), std::size_t{2});
  KCD2S_CHECK_EQ(m.dependencies.size(), std::size_t{1});
  KCD2S_CHECK_EQ(m.exports.size(), std::size_t{2});

  // Per-script language + side classification.
  KCD2S_CHECK_EQ(m.by_side(ScriptSide::Client).size(), std::size_t{2});
  KCD2S_CHECK_EQ(m.by_side(ScriptSide::Server).size(), std::size_t{1});
  KCD2S_CHECK_EQ(m.by_side(ScriptSide::Shared).size(), std::size_t{1});
  KCD2S_CHECK(m.by_side(ScriptSide::Server)[0]->lang == ScriptLang::Js);
  KCD2S_CHECK(m.by_side(ScriptSide::Shared)[0]->lang == ScriptLang::Lua);
  bool saw_ts = false;
  for (auto* s : m.by_side(ScriptSide::Client))
    if (s->lang == ScriptLang::Ts)
      saw_ts = true;
  KCD2S_CHECK(saw_ts);
}

KCD2S_TEST(resource, parses_ui_page) {
  auto m = must_parse(R"(name "hud" version "1" ui_page "nui/index.html")");
  KCD2S_CHECK_EQ(m.ui_page, std::string("nui/index.html"));
  // Absent ui_page stays empty.
  KCD2S_CHECK(must_parse(R"(name "r" version "1")").ui_page.empty());
}

KCD2S_TEST(resource, singular_and_plural_forms) {
  auto m = must_parse(R"(name "r" version "1" client_script "a.lua" dependency "x")");
  KCD2S_CHECK_EQ(m.scripts.size(), std::size_t{1});
  KCD2S_CHECK_EQ(m.dependencies.size(), std::size_t{1});
  KCD2S_CHECK_EQ(m.dependencies[0], std::string("x"));
}

KCD2S_TEST(resource, lang_classification_and_unknown_ext_rejected) {
  KCD2S_CHECK(kcd2s::scripting::lang_of_path("a.cs").value() == ScriptLang::CSharp);
  KCD2S_CHECK(kcd2s::scripting::lang_of_path("a.JS").value() ==
              ScriptLang::Js);  // case-insensitive
  KCD2S_CHECK(!kcd2s::scripting::lang_of_path("a.py").has_value());
  KCD2S_CHECK(!kcd2s::scripting::lang_of_path("noext").has_value());

  auto r = parse_manifest(R"(name "r" version "1" server_script "main.py")");
  KCD2S_CHECK(!r.ok);  // unknown script language
}

KCD2S_TEST(resource, missing_required_fields_rejected) {
  KCD2S_CHECK(!parse_manifest(R"(version "1")").ok);          // no name
  KCD2S_CHECK(!parse_manifest(R"(name "r")").ok);             // no version
  KCD2S_CHECK(parse_manifest(R"(name "r" version "1")").ok);  // minimal valid
}

KCD2S_TEST(resource, malformed_is_rejected_not_crash) {
  KCD2S_CHECK(!parse_manifest(R"(name "unterminated)").ok);                   // unterminated string
  KCD2S_CHECK(!parse_manifest("name \"r\" version \"1\" deps { \"a\" ").ok);  // unterminated block
  KCD2S_CHECK(!parse_manifest("} name \"r\"").ok);                            // stray brace
  KCD2S_CHECK(!parse_manifest("\"orphan\"").ok);                              // value with no key
}

KCD2S_TEST(resource, unknown_keys_ignored) {
  auto r = parse_manifest(R"(name "r" version "1" fx_version "cerulean" game "gta5")");
  KCD2S_CHECK(r.ok);  // unknown keys tolerated for forward-compat
  KCD2S_CHECK_EQ(r.manifest.name, std::string("r"));
}

KCD2S_TEST(resource, registry_rejects_duplicate) {
  ResourceRegistry reg;
  std::string err;
  KCD2S_CHECK(reg.add(must_parse(R"(name "a" version "1")"), err));
  KCD2S_CHECK(!reg.add(must_parse(R"(name "a" version "2")"), err));  // duplicate name
  KCD2S_CHECK(!err.empty());
}

KCD2S_TEST(resource, dependency_topological_order) {
  ResourceRegistry reg;
  std::string err;
  // c depends on b, b depends on a.
  KCD2S_CHECK(reg.add(must_parse(R"(name "c" version "1" dependency "b")"), err));
  KCD2S_CHECK(reg.add(must_parse(R"(name "b" version "1" dependency "a")"), err));
  KCD2S_CHECK(reg.add(must_parse(R"(name "a" version "1")"), err));

  std::vector<std::string> order;
  KCD2S_CHECK(reg.resolve_start_order(order, err));
  KCD2S_CHECK_EQ(order.size(), std::size_t{3});
  auto idx = [&](const std::string& n) {
    return std::find(order.begin(), order.end(), n) - order.begin();
  };
  KCD2S_CHECK(idx("a") < idx("b"));  // deps start first
  KCD2S_CHECK(idx("b") < idx("c"));
}

KCD2S_TEST(resource, determinism_of_order) {
  // Diamond: d -> {b, c} -> a. Order must be stable and deps-first.
  ResourceRegistry reg;
  std::string err;
  KCD2S_CHECK(reg.add(must_parse(R"(name "d" version "1" dependencies { "b" "c" })"), err));
  KCD2S_CHECK(reg.add(must_parse(R"(name "b" version "1" dependency "a")"), err));
  KCD2S_CHECK(reg.add(must_parse(R"(name "c" version "1" dependency "a")"), err));
  KCD2S_CHECK(reg.add(must_parse(R"(name "a" version "1")"), err));

  std::vector<std::string> o1, o2;
  KCD2S_CHECK(reg.resolve_start_order(o1, err));
  KCD2S_CHECK(reg.resolve_start_order(o2, err));
  KCD2S_CHECK(o1 == o2);  // deterministic
  auto idx = [&](std::vector<std::string>& o, const std::string& n) {
    return std::find(o.begin(), o.end(), n) - o.begin();
  };
  KCD2S_CHECK(idx(o1, "a") < idx(o1, "b"));
  KCD2S_CHECK(idx(o1, "a") < idx(o1, "c"));
  KCD2S_CHECK(idx(o1, "b") < idx(o1, "d"));
  KCD2S_CHECK(idx(o1, "c") < idx(o1, "d"));
}

KCD2S_TEST(resource, cycle_detected) {
  ResourceRegistry reg;
  std::string err;
  KCD2S_CHECK(reg.add(must_parse(R"(name "a" version "1" dependency "b")"), err));
  KCD2S_CHECK(reg.add(must_parse(R"(name "b" version "1" dependency "a")"), err));
  std::vector<std::string> order;
  KCD2S_CHECK(!reg.resolve_start_order(order, err));  // cycle
  KCD2S_CHECK(!err.empty());
}

KCD2S_TEST(resource, unknown_dependency_error) {
  ResourceRegistry reg;
  std::string err;
  KCD2S_CHECK(reg.add(must_parse(R"(name "a" version "1" dependency "ghost")"), err));
  std::vector<std::string> order;
  KCD2S_CHECK(!reg.resolve_start_order(order, err));
  KCD2S_CHECK(err.find("ghost") != std::string::npos);
}
