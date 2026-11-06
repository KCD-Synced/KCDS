// Resource host: the local event bus + export registry that lets resources (any language) talk to
// each other within one peer. A resource emits an event; handlers in other resources
// run, in registration order (determinism, §7). A resource declares exports callable by name from
// any other resource. Stopping a resource removes its handlers and exports (crash isolation: one
// resource's teardown never leaks into another).
//
// This is host-agnostic and headless-testable; a concrete IScriptRuntime (runtime.hpp) wires a real
// language in by registering C-function-backed handlers/exports here. Tests use plain C++
// callbacks.
#ifndef KCD2S_SCRIPTING_HOST_HPP
#define KCD2S_SCRIPTING_HOST_HPP

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "kcd2s/scripting/runtime.hpp"
#include "kcd2s/sdk/value.hpp"

namespace kcd2s::scripting {

class ResourceHost {
 public:
  using EventHandler = std::function<void(const sdk::Value::Array& args)>;
  using ExportFn = std::function<sdk::Value(const sdk::Value::Array& args)>;

  // Register/stop a resource. The name is how other resources address its exports.
  ResourceHandle add_resource(std::string name);
  void stop_resource(ResourceHandle h);  // removes all its handlers + exports
  bool alive(ResourceHandle h) const {
    return resources_.count(h) != 0;
  }
  std::size_t resource_count() const {
    return resources_.size();
  }

  // Local event bus (cross-resource, cross-language within this peer).
  void on(ResourceHandle h, const std::string& event, EventHandler cb);
  // Emit to all live handlers of `event`, in registration order. Returns the number invoked.
  std::size_t emit(const std::string& event, const sdk::Value::Array& args = {});

  // Exports: a resource publishes functions callable by (resourceName, fn) from any other.
  void register_export(ResourceHandle h, const std::string& fn, ExportFn impl);
  std::optional<sdk::Value> call_export(const std::string& resource_name, const std::string& fn,
                                        const sdk::Value::Array& args = {});

 private:
  struct Handler {
    ResourceHandle owner;
    std::string event;
    EventHandler cb;
  };
  struct Export {
    ResourceHandle owner;
    ExportFn impl;
  };

  ResourceHandle next_handle_ = 1;
  std::unordered_map<ResourceHandle, std::string> resources_;  // handle -> name
  std::vector<Handler> handlers_;                              // ordered for determinism
  std::unordered_map<std::string, Export> exports_;            // "resource\0fn" -> export
};

}  // namespace kcd2s::scripting

#endif  // KCD2S_SCRIPTING_HOST_HPP
