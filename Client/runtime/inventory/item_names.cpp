#include "item_names.hpp"

#include <atomic>
#include <cstring>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "../branding/runtime_branding.hpp"

namespace kcds::runtime::inventory {
namespace {

using StringAssign = void*(__fastcall*)(void* string, const char* text);

Logger log_line = nullptr;
StringAssign assign = nullptr;

// Key (without the '@') -> name.
std::shared_mutex names_mutex;
std::unordered_map<std::string, std::string> names;
// Whether any name is set, so the game's own keys skip the lock while none is.
std::atomic<bool> any_name{false};

bool item_name(const char* label, void* out) {
  if (!any_name.load(std::memory_order_acquire) || label[0] != '@') {
    return false;
  }
  std::shared_lock<std::shared_mutex> lock(names_mutex);
  const auto found = names.find(label + 1);
  if (found == names.end()) {
    return false;
  }
  assign(out, found->second.c_str());
  return true;
}

}  // namespace

bool install_item_names(void* string_assign, Logger logger) {
  log_line = logger;
  assign = reinterpret_cast<StringAssign>(string_assign);
  if (!assign) {
    return false;
  }
  return branding::add_label_override(item_name);
}

void apply_item_names(const kcd2s::integration::ItemNames& update) {
  std::unique_lock<std::shared_mutex> lock(names_mutex);
  if (update.full) {
    names.clear();
  }
  for (const auto& [key, name] : update.names) {
    if (name.empty()) {
      names.erase(key);
    } else {
      names[key] = name;
    }
  }
  any_name.store(!names.empty(), std::memory_order_release);
  if (log_line) {
    log_line("INVENTORY item names: %zu set (%s, %zu in this update)", names.size(),
             update.full ? "full" : "change", update.names.size());
  }
}

bool item_name_of(const std::string& key, std::string& name) {
  if (!any_name.load(std::memory_order_acquire)) {
    return false;
  }
  std::shared_lock<std::shared_mutex> lock(names_mutex);
  const auto found = names.find(key);
  if (found == names.end()) {
    return false;
  }
  name = found->second;
  return true;
}

}  // namespace kcds::runtime::inventory
