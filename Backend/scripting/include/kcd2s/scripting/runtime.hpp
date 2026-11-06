// IScriptRuntime — the uniform host interface every language runtime implements, so
// the platform treats JS/TS, Lua and C# identically (load/start/stop/tick/invoke/dispatch + stats).
//
// PREPARE-ONLY for the concrete adapters: the V8/Node, LuaJIT and CoreCLR implementations live
// under Backend/scripting/runtimes/ and are built only where those embed libraries exist (see
// a). This interface + the host (host.hpp) are the contract they implement;
// a headless FakeRuntime in the tests exercises the host against this same interface.
#ifndef KCD2S_SCRIPTING_RUNTIME_HPP
#define KCD2S_SCRIPTING_RUNTIME_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/sdk/value.hpp"

namespace kcd2s::scripting {

using ResourceHandle = std::uint32_t;

struct ResourceInfo {
  std::string name;
  std::string entry;  // entry module/assembly path, runtime-specific
};

struct RuntimeStats {
  std::uint64_t events_dispatched = 0;
  std::uint64_t exports_invoked = 0;
};

// A language runtime hosts one or more crash-isolated resources (one isolate/ALC/lua_State each).
struct IScriptRuntime {
  virtual ~IScriptRuntime() = default;

  virtual bool LoadResource(const ResourceInfo& info,
                            ResourceHandle h) = 0;  // create isolate, load
  virtual void Start(ResourceHandle h) = 0;
  virtual void Stop(ResourceHandle h) = 0;             // tear down, free all refs
  virtual void Tick(ResourceHandle h, double dt) = 0;  // drive timers/microtasks
  virtual sdk::Value InvokeExport(ResourceHandle h, const std::string& fn,
                                  const sdk::Value::Array& args) = 0;
  virtual void DispatchEvent(ResourceHandle h, const std::string& event,
                             const sdk::Value::Array& args) = 0;
  virtual RuntimeStats Stats(ResourceHandle h) = 0;

  // TODO(env): concrete adapters (Backend/scripting/runtimes/{js,lua,csharp}) require V8/libnode,
  // LuaJIT and CoreCLR hostfxr respectively — not present in the headless CI image. Implement
  // there.
};

}  // namespace kcd2s::scripting

#endif  // KCD2S_SCRIPTING_RUNTIME_HPP
