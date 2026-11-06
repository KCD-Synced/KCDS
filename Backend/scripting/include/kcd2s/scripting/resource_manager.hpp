// ResourceManager — starts/stops resources into a ResourceHost in dependency order (/§6).
// It ties the parsed manifests (ResourceRegistry) to the running host: each
// started resource gets a ResourceHost handle, its scripts are (optionally) loaded into an
// IScriptRuntime, and a start hook binds its exports/handlers. Resources start in topological
// dependency order and stop in reverse, so a dependency is always live while its dependents run and
// teardown never leaks a stopped resource's handlers/exports (crash isolation, enforced by
// ResourceHost::stop_resource).
//
// The concrete IScriptRuntime adapters are prepare-only; this manager drives whatever runtime it is
// given (or none), and tests pass a stub. Lifecycle events are emitted on the host bus so resources
// can react to each other coming up/down.
#ifndef KCD2S_SCRIPTING_RESOURCE_MANAGER_HPP
#define KCD2S_SCRIPTING_RESOURCE_MANAGER_HPP

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "kcd2s/scripting/host.hpp"
#include "kcd2s/scripting/resource.hpp"
#include "kcd2s/scripting/runtime.hpp"

namespace kcd2s::scripting {

// Lifecycle events emitted on the host bus (args: [ Str resourceName ]).
inline constexpr char kEvResourceStarting[] = "resource:starting";
inline constexpr char kEvResourceStarted[] = "resource:started";
inline constexpr char kEvResourceStopping[] = "resource:stopping";
inline constexpr char kEvResourceStopped[] = "resource:stopped";

class ResourceManager {
 public:
  // Invoked after a resource's host handle is created (and its scripts loaded into the runtime, if
  // any), so the owner can bind the resource's exports/event handlers. Return false to abort that
  // resource's start.
  using StartHook = std::function<bool(const ResourceManifest&, ResourceHandle)>;
  using StopHook = std::function<void(const ResourceManifest&, ResourceHandle)>;

  // `runtime` is optional (nullptr = none); when set, each script is LoadResource'd and the
  // resource is Start()/Stop()'d on it.
  ResourceManager(ResourceRegistry& reg, ResourceHost& host, IScriptRuntime* runtime = nullptr)
      : reg_(reg), host_(host), runtime_(runtime) {
  }

  void set_start_hook(StartHook h) {
    start_hook_ = std::move(h);
  }
  void set_stop_hook(StopHook h) {
    stop_hook_ = std::move(h);
  }

  // Start all resources in topological dependency order. On an unresolvable order (cycle / unknown
  // dependency) nothing is started and false+err is returned. If a start hook or runtime load fails
  // mid-way, everything started in this call is rolled back (stopped in reverse).
  bool start_all(std::string& err);

  // Stop every started resource in reverse start order.
  void stop_all();

  // Start one resource and any of its (transitive) dependencies not yet started, in topo order.
  bool start_one(const std::string& name, std::string& err);
  // Stop one resource and any started resources that (transitively) depend on it, dependents first.
  void stop_one(const std::string& name);

  bool started(const std::string& name) const {
    return handles_.count(name) != 0;
  }
  // Host handle of a started resource, or 0 if not started.
  ResourceHandle handle_of(const std::string& name) const;
  const std::vector<std::string>& start_order() const {
    return started_;
  }

 private:
  bool start_resource(const std::string& name);  // assumes deps already started
  void stop_resource_named(const std::string& name);
  void transitive_deps(const std::string& name, std::vector<std::string>& out) const;
  void transitive_dependents(const std::string& name, std::vector<std::string>& out) const;

  ResourceRegistry& reg_;
  ResourceHost& host_;
  IScriptRuntime* runtime_;
  StartHook start_hook_;
  StopHook stop_hook_;
  std::vector<std::string> started_;  // in start order
  std::unordered_map<std::string, ResourceHandle> handles_;
};

}  // namespace kcd2s::scripting

#endif  // KCD2S_SCRIPTING_RESOURCE_MANAGER_HPP
