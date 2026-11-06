#include "kcd2s/scripting/host.hpp"

namespace kcd2s::scripting {

namespace {
std::string export_key(const std::string& resource, const std::string& fn) {
  std::string k = resource;
  k.push_back('\0');
  k += fn;
  return k;
}
}  // namespace

ResourceHandle ResourceHost::add_resource(std::string name) {
  const ResourceHandle h = next_handle_++;
  resources_.emplace(h, std::move(name));
  return h;
}

void ResourceHost::stop_resource(ResourceHandle h) {
  auto it = resources_.find(h);
  if (it == resources_.end())
    return;
  const std::string& name = it->second;

  // Remove this resource's event handlers.
  for (std::size_t i = handlers_.size(); i-- > 0;)
    if (handlers_[i].owner == h)
      handlers_.erase(handlers_.begin() + static_cast<std::ptrdiff_t>(i));

  // Remove this resource's exports.
  for (auto eit = exports_.begin(); eit != exports_.end();) {
    if (eit->second.owner == h)
      eit = exports_.erase(eit);
    else
      ++eit;
  }
  (void)name;
  resources_.erase(it);
}

void ResourceHost::on(ResourceHandle h, const std::string& event, EventHandler cb) {
  if (!alive(h))
    return;
  handlers_.push_back(Handler{h, event, std::move(cb)});
}

std::size_t ResourceHost::emit(const std::string& event, const sdk::Value::Array& args) {
  // Snapshot matching handlers first so a handler that stops a resource mid-emit can't invalidate
  // the iteration (determinism + safety).
  std::vector<EventHandler> to_run;
  for (const Handler& hd : handlers_)
    if (hd.event == event)
      to_run.push_back(hd.cb);
  for (auto& cb : to_run)
    cb(args);
  return to_run.size();
}

void ResourceHost::register_export(ResourceHandle h, const std::string& fn, ExportFn impl) {
  auto it = resources_.find(h);
  if (it == resources_.end())
    return;
  exports_[export_key(it->second, fn)] = Export{h, std::move(impl)};
}

std::optional<sdk::Value> ResourceHost::call_export(const std::string& resource_name,
                                                    const std::string& fn,
                                                    const sdk::Value::Array& args) {
  auto it = exports_.find(export_key(resource_name, fn));
  if (it == exports_.end())
    return std::nullopt;
  return it->second.impl(args);
}

}  // namespace kcd2s::scripting
