#include "recipe_tables.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <utility>

#include "../hud/game_texts.hpp"
#include "kcd2s/bridge/sigscan.hpp"

namespace kcds::runtime::crafting::recipe_tables {
namespace {

// The database's sort of its rows by key (its vtable + 128;for the smithy's).
constexpr std::size_t kSortSlot = 128 / 8;
// The folder of the recipe tables and the tables the client loads patches of.
constexpr const char* kTableRoot = "libs/tables/";
constexpr std::size_t kTableCount = 2;
constexpr const char* kRecipeTables[kTableCount] = {"BlacksmithRecipes", "AlchemyRecipe"};
// The databases' classes (RTTI type names).
constexpr const char* kDatabaseTypes[kTableCount] = {
    ".?AVC_BlacksmithRecipeDatabase@playermodule@wh@@",
    ".?AVC_AlchemyRecipeDatabase@playermodule@wh@@"};

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
  void* database = find_database(table);
  if (!database) {
    if (natives.log) {
      natives.log("RECIPES %s: no loaded database %s", path.c_str(), table.c_str());
    }
    return false;
  }
  // The game's table load waits for work of the engine thread, so it runs on a thread of its own.
  std::thread([database, path, table]() {
    void* text = nullptr;
    reinterpret_cast<StringCall>(natives.string_init)(&text);
    natives.string_assign(&text, path.c_str());
    const bool loaded =
        reinterpret_cast<LoadTableFile>(natives.load_table_file)(database, &text, true);
    reinterpret_cast<StringCall>(natives.string_free)(&text);
    if (loaded) {
      auto sort = reinterpret_cast<Sort>((*static_cast<void***>(database))[kSortSlot]);
      sort(database);
    }
    if (natives.log) {
      natives.log("RECIPES %s %s into %s", path.c_str(), loaded ? "loaded" : "not loaded",
                  table.c_str());
    }
  }).detach();
  return true;
}

}  // namespace kcds::runtime::crafting::recipe_tables
