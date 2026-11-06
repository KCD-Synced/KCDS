#include "kcd2s/scripting/resource_manager.hpp"

#include <algorithm>

#include "kcd2s/sdk/value.hpp"

namespace kcd2s::scripting {

using sdk::Value;

ResourceHandle ResourceManager::handle_of(const std::string& name) const {
  auto it = handles_.find(name);
  return it == handles_.end() ? 0u : it->second;
}

bool ResourceManager::start_resource(const std::string& name) {
  const ResourceManifest* m = reg_.find(name);
  if (!m)
    return false;
  if (started(name))
    return true;

  host_.emit(kEvResourceStarting, Value::Array{Value(name)});
  const ResourceHandle h = host_.add_resource(name);
  handles_[name] = h;  // visible to the start hook via handle_of()

  // Load the resource's scripts into the runtime (if one is wired), then Start it.
  if (runtime_) {
    for (const ScriptDecl& s : m->scripts) {
      if (!runtime_->LoadResource(ResourceInfo{name, s.path}, h)) {
        host_.stop_resource(h);
        handles_.erase(name);
        return false;
      }
    }
    runtime_->Start(h);
  }

  if (start_hook_ && !start_hook_(*m, h)) {
    if (runtime_)
      runtime_->Stop(h);
    host_.stop_resource(h);
    handles_.erase(name);
    return false;
  }

  started_.push_back(name);
  host_.emit(kEvResourceStarted, Value::Array{Value(name)});
  return true;
}

void ResourceManager::stop_resource_named(const std::string& name) {
  auto it = handles_.find(name);
  if (it == handles_.end())
    return;
  const ResourceHandle h = it->second;
  const ResourceManifest* m = reg_.find(name);

  host_.emit(kEvResourceStopping, Value::Array{Value(name)});
  if (stop_hook_ && m)
    stop_hook_(*m, h);
  if (runtime_)
    runtime_->Stop(h);
  host_.stop_resource(h);  // removes this resource's handlers + exports (crash isolation)
  handles_.erase(it);
  started_.erase(std::remove(started_.begin(), started_.end(), name), started_.end());
  host_.emit(kEvResourceStopped, Value::Array{Value(name)});
}

bool ResourceManager::start_all(std::string& err) {
  std::vector<std::string> order;
  if (!reg_.resolve_start_order(order, err))
    return false;  // cycle/unknown dep -> start nothing

  const std::size_t mark = started_.size();
  for (const std::string& name : order) {
    if (started(name))
      continue;
    if (!start_resource(name)) {
      err = "failed to start resource '" + name + "'";
      // Roll back everything started in this call (reverse order). Copy the name: stop erases it.
      while (started_.size() > mark) {
        const std::string n = started_.back();
        stop_resource_named(n);
      }
      return false;
    }
  }
  return true;
}

void ResourceManager::stop_all() {
  while (!started_.empty()) {
    const std::string n = started_.back();
    stop_resource_named(n);
  }
}

void ResourceManager::transitive_deps(const std::string& name,
                                      std::vector<std::string>& out) const {
  const ResourceManifest* m = reg_.find(name);
  if (!m)
    return;
  std::vector<std::string> deps = m->dependencies;
  std::sort(deps.begin(), deps.end());  // deterministic
  for (const std::string& d : deps) {
    if (std::find(out.begin(), out.end(), d) != out.end())
      continue;
    transitive_deps(d, out);
    if (std::find(out.begin(), out.end(), d) == out.end())
      out.push_back(d);
  }
}

void ResourceManager::transitive_dependents(const std::string& name,
                                            std::vector<std::string>& out) const {
  // BFS over started resources whose dependency list contains a name in the frontier (so a
  // transitive dependent is included). Candidates are scanned in sorted order for determinism.
  std::vector<std::string> frontier{name};
  while (!frontier.empty()) {
    const std::string cur = frontier.back();
    frontier.pop_back();
    std::vector<std::string> candidates = started_;
    std::sort(candidates.begin(), candidates.end());
    for (const std::string& cand : candidates) {
      const ResourceManifest* m = reg_.find(cand);
      if (!m)
        continue;
      if (std::find(m->dependencies.begin(), m->dependencies.end(), cur) == m->dependencies.end())
        continue;
      if (std::find(out.begin(), out.end(), cand) != out.end())
        continue;
      out.push_back(cand);
      frontier.push_back(cand);
    }
  }
}

bool ResourceManager::start_one(const std::string& name, std::string& err) {
  if (!reg_.find(name)) {
    err = "unknown resource '" + name + "'";
    return false;
  }
  // Validate the whole graph resolves (catches cycles / unknown deps) before starting anything.
  std::vector<std::string> order;
  if (!reg_.resolve_start_order(order, err))
    return false;

  // Start name + its transitive deps, in the global topo order (so deps precede dependents).
  std::vector<std::string> needed;
  transitive_deps(name, needed);
  needed.push_back(name);

  const std::size_t mark = started_.size();
  for (const std::string& n : order) {
    if (std::find(needed.begin(), needed.end(), n) == needed.end())
      continue;
    if (started(n))
      continue;
    if (!start_resource(n)) {
      err = "failed to start resource '" + n + "'";
      while (started_.size() > mark) {
        const std::string back = started_.back();
        stop_resource_named(back);
      }
      return false;
    }
  }
  return true;
}

void ResourceManager::stop_one(const std::string& name) {
  if (!started(name))
    return;
  std::vector<std::string> dependents;
  transitive_dependents(name, dependents);
  // Collect dependents in reverse start order BEFORE stopping (stop mutates started_).
  std::vector<std::string> ordered;
  for (std::size_t i = started_.size(); i-- > 0;) {
    const std::string& n = started_[i];
    if (std::find(dependents.begin(), dependents.end(), n) != dependents.end())
      ordered.push_back(n);
  }
  for (const std::string& n : ordered)
    stop_resource_named(n);
  stop_resource_named(name);
}

}  // namespace kcd2s::scripting
