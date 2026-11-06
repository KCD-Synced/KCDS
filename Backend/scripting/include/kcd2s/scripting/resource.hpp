// Resource manifest + registry.
//
// A resource is a folder with a manifest declaring its metadata, its client/server/shared scripts
// (each script's runtime is inferred from its extension: .js/.ts/.lua/.cs), extra files shipped to
// clients, the resources it depends on, and the names it exports. The server loads resources in a
// deterministic dependency order; the client receives a resource's files on join.
//
// This module is the host-agnostic model + a small hand-rolled manifest parser (no external deps).
// Actually *running* a script needs a concrete IScriptRuntime (V8/LuaJIT/CoreCLR), which is
// prepare-only; sandbox/crash-isolation is a documented runtime concern, not implemented here.
//
// Manifest format (fxmanifest-like, line/token based; values are quoted strings):
//   name "chat"
//   version "1.0.0"
//   author "someone"            # optional
//   description "..."           # optional
//   client_script "cl.lua"      # singular or plural; plural may use a { ... } block
//   client_scripts { "a.ts" "b.js" }
//   server_script "sv.js"
//   shared_script "shared.lua"
//   file "ui/index.html"        # arbitrary extension (shipped to clients, not a script)
//   dependency "base"
//   dependencies { "base" "auth" }
//   export "getName"
//   # and -- begin line comments (outside quotes). Unknown keys are ignored (forward-compatible).
#ifndef KCD2S_SCRIPTING_RESOURCE_HPP
#define KCD2S_SCRIPTING_RESOURCE_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace kcd2s::scripting {

enum class ScriptLang { Js, Ts, Lua, CSharp };
enum class ScriptSide { Client, Server, Shared };

// Classify a script path by extension. Returns nullopt for an unknown/again-unsupported extension.
std::optional<ScriptLang> lang_of_path(const std::string& path);
const char* lang_name(ScriptLang l);

struct ScriptDecl {
  std::string path;
  ScriptLang lang;
  ScriptSide side;
};

struct ResourceManifest {
  std::string name;
  std::string version;
  std::string author;
  std::string description;
  std::vector<ScriptDecl> scripts;        // client + server + shared, in declared order
  std::vector<std::string> files;         // arbitrary files shipped to clients
  std::vector<std::string> dependencies;  // other resource names
  std::vector<std::string> exports;       // names this resource exports
  std::string ui_page;                    // NUI HTML entry (fxmanifest-style `ui_page`), optional

  // Convenience: scripts filtered by side.
  std::vector<const ScriptDecl*> by_side(ScriptSide side) const;
};

struct ParseResult {
  bool ok = false;
  ResourceManifest manifest;
  std::string error;  // human-readable reason when !ok
};

// Parse + validate a manifest from text. Required: name, version, and every script has a known
// language. Untrusted-input safe: malformed input (unterminated quote/block, runaway size) yields
// ok=false with a reason, never a crash.
ParseResult parse_manifest(const std::string& text);

// A set of resources with dependency-ordered start resolution.
class ResourceRegistry {
 public:
  // Add a validated manifest. Fails (false + err) on a duplicate name.
  bool add(ResourceManifest m, std::string& err);

  const ResourceManifest* find(const std::string& name) const;
  std::size_t size() const {
    return resources_.size();
  }

  // Produce a deterministic topological start order (dependencies before dependents). Fails on an
  // unknown dependency or a dependency cycle, naming the offender in err.
  bool resolve_start_order(std::vector<std::string>& out_order, std::string& err) const;

 private:
  std::vector<ResourceManifest> resources_;  // insertion order; small N, linear lookups are fine
};

}  // namespace kcd2s::scripting

#endif  // KCD2S_SCRIPTING_RESOURCE_HPP
