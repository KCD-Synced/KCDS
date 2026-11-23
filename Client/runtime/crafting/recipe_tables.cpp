#include "recipe_tables.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../hud/game_texts.hpp"
#include "../loader_link/payload_link.hpp"
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
using FindWorkpiece = void*(__fastcall*)(void* unused, const char** id);

// A database's rows: a std::vector of the row objects at + 48 (begin) and + 56 (end).
constexpr std::size_t kRowsBegin = 48;
constexpr std::size_t kRowsEnd = 56;
// A workpiece row's Id (a CryString) and the index of the workpieces' database in natives.
constexpr std::size_t kWorkpieceId = 8;
constexpr std::size_t kWorkpieces = 2;

Natives natives;
const void* vtables[kTableCount] = {};

// A recipe holds a plain pointer to its workpiece row (WorkpieceId resolved at the recipe's load).
// The rows live in the workpieces' vector, so a workpiece patch (new rows, then the sort) moves
// them and every recipe pointed at freed memory: the smithy's grab of the workpiece then read it
// and the game crashed. After such a load each recipe is pointed again at its workpiece by id.
struct WorkpieceLinks {
  FindWorkpiece find = nullptr;  // the game's lookup of a workpiece row by id
  std::size_t offset = 0;        // the recipe's workpiece pointer
  std::size_t recipe_size = 0;   // one recipe row in the recipes' vector
};
WorkpieceLinks links;

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

// The multiplicative inverse of an odd number modulo 2^64 (Newton's iteration).
std::uint64_t inverse(std::uint64_t odd) {
  std::uint64_t value = odd;
  for (int step = 0; step < 6; ++step) {
    value *= 2 - odd * value;
  }
  return value;
}

// Every offset in the image where the pattern matches, in order.
std::vector<std::size_t> matches(const char* pattern) {
  std::vector<std::size_t> found;
  const auto signature = kcd2s::bridge::Signature::parse(pattern);
  if (!signature || !natives.image) {
    return found;
  }
  std::size_t from = 0;
  while (from < natives.image_size) {
    const std::size_t offset =
        kcd2s::bridge::scan(natives.image + from, natives.image_size - from, *signature);
    if (offset == kcd2s::bridge::knpos) {
      break;
    }
    found.push_back(from + offset);
    from += offset + 1;
  }
  return found;
}

// The game's pieces for the recipes' workpiece pointers, WHGame 1.5.5:
// - the lookup of a workpiece by id: one of the databases' identical lookups, the one that reads
//   the workpieces' vector (its mov rdx, [rip+...] of the vector's begin);
// - the recipe's pointer: WorkpieceId's setter calls that lookup and stores the result at
//   [rbx + offset];
// - a recipe row's size: the recipes' sort divides the vector's length by it (movabs of its
//   inverse, the size / 8 being odd).
void find_links() {
  void* workpieces = natives.databases[kWorkpieces];
  if (!natives.image || !workpieces || !vtables[0]) {
    return;
  }
  const auto base = reinterpret_cast<std::uintptr_t>(natives.image);
  const std::size_t rows = reinterpret_cast<std::uintptr_t>(workpieces) + kRowsBegin - base;
  std::size_t find = kcd2s::bridge::knpos;
  for (const std::size_t match :
       matches("48 8B C4 48 89 58 10 48 89 70 18 48 89 48 08 57 48 83 EC 30 48 8B FA 48 8D 48 08 "
               "4C 8B CA 33 F6 89 74 24 40 4C 8B 05 ?? ?? ?? ?? 48 8B 15 ?? ?? ?? ?? E8")) {
    if (kcd2s::bridge::rip_target(natives.image, natives.image_size, match + 46, match + 50) ==
        rows) {
      find = match;
      break;
    }
  }
  if (find == kcd2s::bridge::knpos) {
    return;
  }
  std::size_t offset = 0;
  for (const std::size_t match :
       matches("48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? 48 89 43")) {
    if (kcd2s::bridge::rip_target(natives.image, natives.image_size, match + 17, match + 21) ==
        find) {
      offset = natives.image[match + 24];
      break;
    }
  }
  const auto* sort = static_cast<const std::uint8_t*>(at<const void*>(vtables[0], kSortSlot * 8));
  const std::uint8_t sort_start[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x51, 0x38, 0x48, 0xB8};
  std::size_t recipe_size = 0;
  if (sort && std::memcmp(sort, sort_start, sizeof(sort_start)) == 0) {
    const std::uint64_t multiplier = at<std::uint64_t>(sort, sizeof(sort_start));
    if (multiplier & 1) {
      recipe_size = std::size_t(inverse(multiplier) * 8);
    }
  }
  if (offset == 0 || recipe_size == 0 || offset + 8 > recipe_size || recipe_size > 4096) {
    return;
  }
  links.find = reinterpret_cast<FindWorkpiece>(const_cast<std::uint8_t*>(natives.image + find));
  links.offset = offset;
  links.recipe_size = recipe_size;
}

// The recipes' rows: their first byte and how many, or none.
std::pair<std::uint8_t*, std::size_t> recipe_rows() {
  void* recipes = find_database(kRecipeTables[0]);
  if (!recipes || !links.find) {
    return {nullptr, 0};
  }
  auto* begin = at<std::uint8_t*>(recipes, kRowsBegin);
  auto* end = at<std::uint8_t*>(recipes, kRowsEnd);
  if (!begin || end < begin || std::size_t(end - begin) % links.recipe_size != 0) {
    return {nullptr, 0};
  }
  return {begin, std::size_t(end - begin) / links.recipe_size};
}

// Each recipe's workpiece id (by the recipe's index), read while the pointers are still good.
std::vector<std::pair<std::size_t, std::string>> workpiece_ids() {
  std::vector<std::pair<std::size_t, std::string>> ids;
  const auto [rows, count] = recipe_rows();
  for (std::size_t i = 0; i < count; ++i) {
    const auto* workpiece = at<const std::uint8_t*>(rows + i * links.recipe_size, links.offset);
    const char* id = workpiece ? at<const char*>(workpiece, kWorkpieceId) : nullptr;
    if (id && *id) {
      ids.emplace_back(i, id);
    }
  }
  return ids;
}

// Every recipe pointed at its workpiece again after the workpieces' rows moved.
void relink(const std::vector<std::pair<std::size_t, std::string>>& ids) {
  const auto [rows, count] = recipe_rows();
  void* workpieces = natives.databases[kWorkpieces];
  const auto* first = at<const std::uint8_t*>(workpieces, kRowsBegin);
  const auto* last = at<const std::uint8_t*>(workpieces, kRowsEnd);
  std::size_t moved = 0;
  std::size_t missing = 0;
  for (const auto& [index, id] : ids) {
    if (index >= count) {
      continue;
    }
    std::uint8_t* recipe = rows + index * links.recipe_size;
    const auto* before = at<const std::uint8_t*>(recipe, links.offset);
    const char* text = id.c_str();
    void* workpiece = links.find(nullptr, &text);
    if (before < first || before >= last || before != workpiece) {
      moved += 1;
    }
    if (!workpiece) {
      missing += 1;
    }
    std::memcpy(recipe + links.offset, &workpiece, sizeof(workpiece));
  }
  if (natives.log) {
    natives.log(
        "RECIPES %zu recipes pointed at their workpieces again (%zu had moved, %zu not found)",
        ids.size(), moved, missing);
  }
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

// One table patch into its database, then the database sorted by key again. Workpiece rows move
// in that load, so the recipes are pointed at them again after it.
void load_now(const Load& load) {
  const bool workpieces = load.database == natives.databases[kWorkpieces];
  std::vector<std::pair<std::size_t, std::string>> ids;
  if (workpieces) {
    ids = workpiece_ids();
  }
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
  if (workpieces) {
    relink(ids);
  }
}

void loader_loop() {
  const kcds::runtime::loader_link::ThreadScope scope("recipe tables");
  // Ends when the client is about to be unloaded (the KCDS loader's swap).
  while (!kcds::runtime::loader_link::stopping()) {
    Load next;
    {
      std::unique_lock<std::mutex> lock(loads_mutex);
      if (!loads_ready.wait_for(lock, std::chrono::milliseconds(250),
                                [] { return !loads.empty(); })) {
        continue;
      }
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
  find_links();
  if (natives.log) {
    const bool ready = natives.load_table_file && natives.string_init && natives.string_free &&
                       natives.string_assign;
    natives.log("RECIPES table patches %s", ready ? "installed" : "unavailable");
    if (links.find) {
      natives.log("RECIPES recipes' workpiece pointer at +%zu, recipe rows of %zu bytes",
                  links.offset, links.recipe_size);
    } else {
      natives.log("RECIPES recipes' workpiece pointer not found: no workpiece patches");
    }
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
    // Without the recipes' pointers to fix after the load, new workpiece rows would leave every
    // recipe pointing at freed memory.
    if (!links.find) {
      if (natives.log) {
        natives.log("RECIPES %s: not loaded, the recipes' workpiece pointer is unknown",
                    path.c_str());
      }
      return false;
    }
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
