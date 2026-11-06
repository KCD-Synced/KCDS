#include "max_column.hpp"

#include <Windows.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "../hud/game_texts.hpp"
#include "../native_ui/native_ui.hpp"
#include "MinHook.h"
#include "item_types.hpp"
#include "max_column_movie.hpp"
#include "stock_projection.hpp"

namespace kcds::runtime::inventory {
namespace {

namespace integration = kcd2s::integration;
namespace sdk = kcd2s::sdk;

constexpr char kElement[] = "ApseInventoryList";
// The container screen's two lists (wh::guimodule::C_UIItemTransfer: its
// C_UIItemTransferInventory lists at + 184 and + 1712, made with the element
// "ItemTransfer" and the fixed instances 0 and 1; no panel binding). Its movie draws its rows with
// the same ItemView as the inventory's list.
constexpr char kTransferElement[] = "ItemTransfer";
constexpr int kTransferInstances = 2;
constexpr char kClip[] = "kcdsMax";
constexpr char kMovieKey[] = "_kcds/inventory-max-5.swf";
constexpr char kMovieUrl[] = "/%USER%/kcds_ui/_kcds/inventory-max-5.swf";
// The localization key the movie's icon asks the game's tooltip for.
constexpr char kTooltipKey[] = "kcds_tip_max_carry";
// How often the list is looked at, and how long a load may take before it is tried again.
constexpr ULONGLONG kLookMs = 250;
constexpr ULONGLONG kLoadRetryMs = 3000;
// IEntity::GetId (vtable slot 1).
constexpr std::size_t kEntityGetId = 1;
// An item class's icon id (item_types.hpp).
constexpr std::size_t kClassIcon = 0x90;

using PanelBind = void*(__fastcall*)(void* binder, const char** element, void* entity,
                                     const char** panel, unsigned flags);
PanelBind original_bind = nullptr;
Logger log_line = nullptr;

// The instances the menu shows the list in (the inventory's, the shop's two, a container's): each
// its panel entity's id, added by the binding hook.
constexpr std::size_t kMaxLists = 8;
std::mutex bound_mutex;
std::vector<unsigned> bound;

// One list the column follows: its element and instance, what it was last handed and when its
// movie was last asked for.
struct List {
  const char* element = kElement;
  unsigned instance = 0;
  std::optional<std::string> handed;
  ULONGLONG load_started = 0;
};
std::vector<List> lists;

std::map<integration::Guid, std::uint32_t> limits;
bool movie_written = false;
ULONGLONG looked_at = 0;

void* __fastcall hooked_bind(void* binder, const char** element, void* entity, const char** panel,
                             unsigned flags) {
  const bool list = element && *element && entity && std::strcmp(*element, kElement) == 0;
  unsigned id = 0;
  if (list) {
    using GetId = unsigned(__fastcall*)(void* entity);
    id = (*reinterpret_cast<GetId**>(entity))[kEntityGetId](entity);
  }
  void* result = original_bind(binder, element, entity, panel, flags);
  if (list && result && id != 0) {
    std::lock_guard<std::mutex> lock(bound_mutex);
    if (std::find(bound.begin(), bound.end(), id) == bound.end() && bound.size() < kMaxLists) {
      bound.push_back(id);
      if (log_line) {
        log_line("INVENTORY list shown in instance %u", id);
      }
    }
  }
  return result;
}

// One NativeUI clip call on a shown list (below its _root).
sdk::Value clip_call(const List& list, sdk::Value::Array path, const char* action,
                     sdk::Value::Map fields = {}) {
  fields.emplace_back("op", sdk::Value("elementClip"));
  fields.emplace_back("name", sdk::Value(list.element));
  fields.emplace_back("instance", sdk::Value(std::int64_t(list.instance)));
  fields.emplace_back("path", sdk::Value(std::move(path)));
  fields.emplace_back("action", sdk::Value(action));
  return native_ui::request(sdk::Value(std::move(fields)));
}

bool movie_ready(const List& list) {
  const sdk::Value ready =
      clip_call(list, {sdk::Value(kClip)}, "get", {{"member", sdk::Value("ready")}});
  return ready.tag() == sdk::Tag::Bool && ready.as_bool();
}

// The class's icon id (+ 0x90, a CryString: its first field points at the characters), the name
// the list gives a row's icon; "" without one.
std::string icon_of(const integration::Guid& item) {
  const auto* item_class = static_cast<const std::uint8_t*>(game_item_class(item));
  const char* icon = nullptr;
  if (item_class) {
    std::memcpy(&icon, item_class + kClassIcon, sizeof(icon));
  }
  return icon ? std::string(icon) : std::string();
}

// The rows' limits as the movie takes them, "<key>=<max>|...": the local player's items of a
// limited class by the names the list gives them ("w" and the item's handle), then each limited
// class by "i" and its icon, which the movie asks for another inventory's rows (a trader's).
std::string row_limits() {
  std::string text;
  const auto add = [&text](const std::string& key, std::uint32_t max) {
    text += (text.empty() ? "" : "|") + key + "=" + std::to_string(max);
  };
  for (const auto& [handle, item_class] : local_item_classes()) {
    const auto limit = limits.find(item_class);
    if (limit == limits.end()) {
      continue;
    }
    char name[24] = {};
    std::snprintf(name, sizeof(name), "w%016llx", static_cast<unsigned long long>(handle));
    add(name, limit->second);
  }
  for (const auto& [item_class, max] : limits) {
    const std::string icon = icon_of(item_class);
    if (!icon.empty() && icon.find_first_of("|=") == std::string::npos) {
      add("i" + icon, max);
    }
  }
  return text;
}

}  // namespace

bool install_max_column(void* panel_bind, Logger log) {
  log_line = log;
  return panel_bind &&
         MH_CreateHook(panel_bind, reinterpret_cast<void*>(hooked_bind),
                       reinterpret_cast<void**>(&original_bind)) == MH_OK &&
         MH_EnableHook(panel_bind) == MH_OK;
}

void set_max_column_limits(std::map<integration::Guid, std::uint32_t> classes,
                           const std::string& tooltip) {
  limits = std::move(classes);
  for (List& list : lists) {
    list.handed.reset();
  }
  hud::set_text(kTooltipKey, tooltip);
}

// One list: its movie loaded when the server limits anything, then handed what it shows.
void keep_list(List& list, const std::string& wanted, ULONGLONG now, Logger log) {
  if (list.instance > unsigned(INT_MAX) ||
      !native_ui::element_instance(list.element, int(list.instance))) {
    list.handed.reset();
    return;
  }
  if (!movie_ready(list)) {
    list.handed.reset();
    // A server without limits leaves the list as the game has it.
    if (limits.empty() || now - list.load_started < kLoadRetryMs) {
      return;
    }
    if (!movie_written) {
      const std::vector<std::uint8_t> movie = max_column_movie();
      movie_written = !movie.empty() && native_ui::write_own_file(kMovieKey, movie);
    }
    list.load_started = now;
    clip_call(list, {}, "create", {{"child", sdk::Value(kClip)}});
    clip_call(list, {sdk::Value(kClip)}, "invoke",
              {{"method", sdk::Value("loadMovie")},
               {"args", sdk::Value(sdk::Value::Array{sdk::Value(kMovieUrl)})}});
    if (log) {
      log("INVENTORY max column movie loading in %s instance %u", list.element, list.instance);
    }
    return;
  }
  if (list.handed && *list.handed == wanted) {
    return;
  }
  clip_call(list, {sdk::Value(kClip)}, "invoke",
            {{"method", sdk::Value("setLimits")},
             {"args", sdk::Value(sdk::Value::Array{sdk::Value(wanted)})}});
  list.handed = wanted;
}

void tick_max_column(Logger log) {
  const ULONGLONG now = GetTickCount64();
  if (now - looked_at < kLookMs) {
    return;
  }
  looked_at = now;
  if (lists.empty()) {
    for (int instance = 0; instance < kTransferInstances; ++instance) {
      lists.push_back(List{kTransferElement, unsigned(instance)});
    }
  }
  {
    std::lock_guard<std::mutex> lock(bound_mutex);
    for (const unsigned instance : bound) {
      const bool known = std::any_of(lists.begin(), lists.end(), [instance](const List& list) {
        return list.element == kElement && list.instance == instance;
      });
      if (!known) {
        lists.push_back(List{kElement, instance});
      }
    }
  }
  if (lists.empty()) {
    return;
  }
  // "on" and the rows: the column shows while the server limits any class. The player's own
  // items name the rows; another inventory's rows (a trader's) find no limit and stay empty.
  const std::string wanted = (limits.empty() ? "0" : "1") + std::string(":") + row_limits();
  for (List& list : lists) {
    keep_list(list, wanted, now, log);
  }
}

}  // namespace kcds::runtime::inventory
