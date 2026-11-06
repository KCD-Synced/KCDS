// Code generators: emit per-language bindings from the one IDL model so
// the API never drifts across languages. TypeScript .d.ts is the primary, first-class surface; Lua
// EmmyLua and C# stubs follow. Output is deterministic (stable ordering) so it can be snapshot/diff
// checked in CI.
#ifndef KCD2S_SDK_GEN_HPP
#define KCD2S_SDK_GEN_HPP

#include <string>

#include "kcd2s/sdk/idl.hpp"

namespace kcd2s::sdk::gen {

// Emit a TypeScript declaration file (.d.ts) for the model.
std::string emit_typescript(const idl::ApiModel& model);

// Emit Lua EmmyLua/LuaLS annotations (---@class/---@field + function stubs with ---@param/@return).
std::string emit_lua(const idl::ApiModel& model);

// Emit C# binding stubs (partial classes, properties, method signatures, events as
// System.Action<>).
std::string emit_csharp(const idl::ApiModel& model);

// Emit the API reference as JSON (schema 1) for the website's docs: every type and member with its
// TypeScript signature, side, stability, since, doc summary, parameters, errors, raised events and
// examples. Read by the kcds-website repository (server/src/docs/api-reference.ts).
std::string emit_docs_json(const idl::ApiModel& model);

}  // namespace kcd2s::sdk::gen

#endif  // KCD2S_SDK_GEN_HPP
