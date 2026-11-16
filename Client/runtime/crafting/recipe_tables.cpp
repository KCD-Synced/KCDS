#include "recipe_tables.hpp"

#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include "../hud/game_texts.hpp"
#include "kcd2s/bridge/sigscan.hpp"

namespace kcds::runtime::crafting::recipe_tables {
namespace {

// The database's sort of its rows by key (its vtable + 128;for the smithy's).
constexpr std::size_t kSortSlot = 128 / 8;
// The folder of the recipe tables and the tables the client loads patches of.
constexpr const char* kTableRoot = "libs/tables/";
constexpr std::size_t kTableCount = 3;
constexpr const char* kRecipeTables[kTableCount] = {"BlacksmithRecipes", "AlchemyRecipe",
                                                    "BlacksmithWorkpieces"};
// The databases' classes (RTTI type names), as the loaded databases have them.
constexpr const char* kDatabaseTypes[kTableCount] = {
    ".?AVC_BlacksmithRecipeDatabase@playermodule@wh@@",
    ".?AVC_AlchemyRecipeDatabase@playermodule@wh@@",
    ".?AVC_BlacksmithWorkpieceDatabase@playermodule@wh@@"};

using LoadTableFile = bool(__fastcall*)(void* database, void* path, bool patch);
using StringCall = void(__fastcall*)(void* string);
using Sort = bool(__fastcall*)(void* database);

Natives natives;
const void* vtables[kTableCount] = {};

template <class T>
T at(const void* object, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

std::string lower(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') {
      c = char(c - 'A' + 'a');
    }
  }
  return text;
}

// The loaded database of this table, or null: the game's static database, checked by its vtable.
void* find_database(const std::string& table) {
  for (std::size_t i = 0; i < kTableCount; ++i) {
    if (lower(table) != lower(kRecipeTables[i])) {
      continue;
    }
    void* database = natives.databases[i];
    if (database && vtables[i] && at<const void*>(database, 0) == vtables[i]) {
      return database;
    }
  }
  return nullptr;
}

// The value of an attribute in an XML element's text, or "".
std::string attribute(const std::string& element, const char* name) {
  const std::string key = std::string(" ") + name + "=\"";
  const std::size_t start = element.find(key);
  if (start == std::string::npos) {
    return {};
  }
  const std::size_t from = start + key.size();
  const std::size_t to = element.find('"', from);
  return to == std::string::npos ? std::string() : element.substr(from, to - from);
}

// The game's XML escapes in an attribute value, as plain text.
std::string unescape(std::string value) {
  const std::pair<const char*, const char*> escapes[] = {
      {"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}};
  for (const auto& [from, to] : escapes) {
    for (std::size_t at = value.find(from); at != std::string::npos; at = value.find(from, at)) {
      value.replace(at, std::strlen(from), to);
      at += std::strlen(to);
    }
  }
  return value;
}

// The models of the workpieces resources defined (Crafting.defineWorkpiece), by id: every client
// loads the same patches, so a smith's custom workpiece can be shown on the others' screens.
std::mutex workpieces_mutex;
std::unordered_map<std::string, std::string> workpiece_models;

// Every BlacksmithWorkpiece row's Id and Model.
void note_workpieces(const std::string& text) {
  const std::string tag = "<BlacksmithWorkpiece ";
  std::lock_guard<std::mutex> lock(workpieces_mutex);
  for (std::size_t at = text.find(tag); at != std::string::npos; at = text.find(tag, at + 1)) {
    const std::size_t end = text.find('>', at);
    if (end == std::string::npos) {
      return;
    }
    const std::string element = text.substr(at, end - at);
    const std::string id = unescape(attribute(element, "Id"));
    const std::string model = unescape(attribute(element, "Model"));
    if (!id.empty() && !model.empty()) {
      workpiece_models[id] = model;
    }
  }
}

// Every element with StringName="kcds_..." and a Text: the game's text of that key.
void give_texts(const std::string& text) {
  for (std::size_t at = text.find('<'); at != std::string::npos; at = text.find('<', at + 1)) {
    const std::size_t end = text.find('>', at);
    if (end == std::string::npos) {
      return;
    }
    const std::string element = text.substr(at, end - at);
    const std::string key = attribute(element, "StringName");
    const std::string value = attribute(element, "Text");
    if (key.rfind("kcds_", 0) == 0 && !value.empty()) {
      kcds::runtime::hud::set_text(key, unescape(value));
    }
  }
}

// The table loads waiting, in the order they came, and the one thread that runs them. The game's
// table load waits for work of the engine thread, so it cannot run there; a thread of its own for
// each file let a server's dozen recipe patches load and sort the same database at once, which
// broke it and the game's heap (crashes in the smithy and in CryPak right after the loads).
struct Load {
  void* database = nullptr;
  std::string path;
  std::string table;
};
std::mutex loads_mutex;
std::condition_variable loads_ready;
std::deque<Load> loads;
bool loader_started = false;

// One table patch into its database, then the database sorted by key again.
void load_now(const Load& load) {
  void* text = nullptr;
  reinterpret_cast<StringCall>(natives.string_init)(&text);
  natives.string_assign(&text, load.path.c_str());
  const bool loaded =
      reinterpret_cast<LoadTableFile>(natives.load_table_file)(load.database, &text, true);
  reinterpret_cast<StringCall>(natives.string_free)(&text);
  if (loaded) {
    auto sort = reinterpret_cast<Sort>((*static_cast<void***>(load.database))[kSortSlot]);
    sort(load.database);
  }
  if (natives.log) {
    natives.log("RECIPES %s %s into %s", load.path.c_str(), loaded ? "loaded" : "not loaded",
                load.table.c_str());
  }
}

void loader_loop() {
  for (;;) {
    Load next;
    {
      std::unique_lock<std::mutex> lock(loads_mutex);
      loads_ready.wait(lock, [] { return !loads.empty(); });
      next = std::move(loads.front());
      loads.pop_front();
    }
    load_now(next);
  }
}

void queue_load(Load load) {
  std::lock_guard<std::mutex> lock(loads_mutex);
  loads.push_back(std::move(load));
  if (!loader_started) {
    loader_started = true;
    std::thread(loader_loop).detach();
  }
  loads_ready.notify_one();
}

}  // namespace

void install(const Natives& given) {
  natives = given;
  const auto base = reinterpret_cast<std::uintptr_t>(natives.image);
  for (std::size_t i = 0; i < kTableCount && natives.image; ++i) {
    const std::size_t offset =
        kcd2s::bridge::rtti_vtable(natives.image, natives.image_size, base, kDatabaseTypes[i]);
    vtables[i] = offset == kcd2s::bridge::knpos ? nullptr : natives.image + offset;
  }
  if (natives.log) {
    const bool ready = natives.load_table_file && natives.string_init && natives.string_free &&
                       natives.string_assign;
    natives.log("RECIPES table patches %s", ready ? "installed" : "unavailable");
    // A table whose database or its class the client did not find takes no patches.
    for (std::size_t i = 0; i < kTableCount; ++i) {
      if (!natives.databases[i] || !vtables[i]) {
        natives.log("RECIPES %s: %s not found", kRecipeTables[i],
                    natives.databases[i] ? "the database's class" : "the database");
      }
    }
  }
}

bool apply(const std::string& path, const std::string& text) {
  if (!natives.load_table_file || !natives.string_init || !natives.string_free ||
      !natives.string_assign) {
    return false;
  }
  // libs/tables/<folder>/<Table>__<suffix>.xml
  const std::string lowered = lower(path);
  const std::string root = kTableRoot;
  if (lowered.rfind(root, 0) != 0 || lowered.size() < 4 ||
      lowered.compare(lowered.size() - 4, 4, ".xml") != 0) {
    return false;
  }
  const std::size_t slash = path.find('/', root.size());
  if (slash == std::string::npos || path.find('/', slash + 1) != std::string::npos) {
    return false;
  }
  const std::string file = path.substr(slash + 1);
  const std::size_t patch = file.find("__");
  if (patch == std::string::npos || patch == 0) {
    return false;
  }
  const std::string table = file.substr(0, patch);
  bool known = false;
  for (const char* name : kRecipeTables) {
    known = known || lower(table) == lower(name);
  }
  if (!known) {
    return false;
  }
  give_texts(text);
  if (lower(table) == lower("BlacksmithWorkpieces")) {
    note_workpieces(text);
  }
  void* database = find_database(table);
  if (!database) {
    if (natives.log) {
      natives.log("RECIPES %s: no loaded database %s", path.c_str(), table.c_str());
    }
    return false;
  }
  // Loaded on the loader thread, one table after another (queue_load).
  queue_load(Load{database, path, table});
  return true;
}

std::string workpiece_model(const std::string& id) {
  std::lock_guard<std::mutex> lock(workpieces_mutex);
  const auto found = workpiece_models.find(id);
  return found == workpiece_models.end() ? std::string() : found->second;
}

}  // namespace kcds::runtime::crafting::recipe_tables
