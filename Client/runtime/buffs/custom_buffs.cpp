#include "custom_buffs.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "player_buffs.hpp"

namespace kcds::runtime::buffs::custom {
namespace {

namespace integration = kcd2s::integration;
using integration::CustomBuff;

using integration::Guid;

// The game's CryString constructor: (string, text).
using StringFromText = void*(__fastcall*)(void* string, const char* text);
StringFromText string_from_text = nullptr;

void set_string(void* row, std::size_t offset, const char* text) {
  string_from_text(static_cast<std::uint8_t*>(row) + offset, text);
}

constexpr std::size_t kDefinitionSize = 152;
constexpr std::size_t kName = 16;
constexpr std::size_t kDescription = 24;
constexpr std::size_t kFamily = 36;
constexpr std::size_t kDuration = 48;
constexpr std::size_t kExclusivity = 52;
constexpr std::size_t kParams = 64;
constexpr std::size_t kUiName = 72;
constexpr std::size_t kAiTag = 84;
constexpr std::size_t kPersistent = 92;
constexpr std::size_t kVisualEffect = 96;
constexpr std::size_t kImplementationClass = 144;
constexpr std::size_t kInstanceId = 8;
constexpr std::size_t kInstanceDefinition = 16;
// C_TimedSoulBuffInstance (BasicTimed): its run, doubles in seconds.
constexpr std::size_t kTimedDuration = 216;
constexpr std::size_t kTimedElapsed = 224;
// More copies than a session's resources register (256 at once on the server).
constexpr std::size_t kMaxDefinitions = 1024;
constexpr ULONGLONG kSecondMs = 1000;
// A buff with less left than this is not put back: it ends on its own or by the server.
constexpr ULONGLONG kPutBackMarginMs = 500;

constexpr char kTimedImplementation[] = "BasicTimed";
constexpr char kNoText[] = "";

// One copy of a game buff's definition, made for one name, icon, label and duration.
struct Definition {
  alignas(16) std::array<std::uint8_t, kDefinitionSize> row{};
  std::string name;
  std::string label;
  Guid icon{};
  std::int64_t duration_ms = -1;
  std::string params;  // the game's own modifiers (buff_params text)
};

// A custom buff on the soul: its copy, its instance and when it ends (0: never).
struct Shown {
  const Definition* definition = nullptr;
  std::uint64_t instance = 0;
  ULONGLONG ends_at = 0;
};

Logger log_line = nullptr;
// A deque keeps every copy where it is.
std::deque<Definition> definitions;
std::set<const void*> rows;
std::map<std::string, Shown> shown;
void* current_soul = nullptr;

template <class T>
T field(const void* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
  return value;
}

template <class T>
void set_field(void* base, std::size_t offset, const T& value) {
  std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(T));
}

// The copy for the show: one made before, or a new one of the icon's definition. Null when the
// game has no such buff or there are too many copies.
const Definition* definition_for(const CustomBuff& buff) {
  for (const Definition& made : definitions) {
    if (made.name == buff.name && made.icon == buff.icon && made.label == buff.label &&
        made.duration_ms == buff.duration_ms && made.params == buff.params) {
      return &made;
    }
  }
  const std::uint8_t* icon = definition_of(buff.icon);
  if (!icon || definitions.size() >= kMaxDefinitions) {
    return nullptr;
  }
  Definition& made = definitions.emplace_back();
  made.name = buff.name;
  made.label = buff.label;
  made.icon = buff.icon;
  made.duration_ms = buff.duration_ms;
  made.params = buff.params;
  std::memcpy(made.row.data(), icon, kDefinitionSize);
  void* row = made.row.data();
  // The text columns are the game's CryStrings: each is made anew by the game's constructor (the
  // copied pointers were never counted, so nothing is released). The copies are never freed.
  set_string(row, kName, made.name.c_str());
  set_string(row, kDescription, kNoText);
  set_field<std::int32_t>(row, kFamily, -1);
  const float seconds = buff.duration_ms < 0 ? -1.0f : float(buff.duration_ms) / 1000.0f;
  set_field<float>(row, kDuration, seconds);
  set_field<std::int32_t>(row, kExclusivity, 0);
  // The game's own modifiers of the buff (its buff_params text): the server's native modifiers
  // none for a buff without them.
  set_string(row, kParams, made.params.c_str());
  set_string(row, kUiName, made.label.c_str());
  set_field<std::int32_t>(row, kAiTag, -1);
  set_field<std::uint8_t>(row, kPersistent, 0);
  set_string(row, kVisualEffect, kNoText);
  set_string(row, kImplementationClass, kTimedImplementation);
  rows.insert(row);
  return &made;
}

// Puts the copy's instance on the soul with `remaining_ms` of its run left (below zero: the
// whole run). Its id, 0 when the game made none.
std::uint64_t add(void* soul, const Definition& definition, std::int64_t remaining_ms) {
  void* instance = add_defined(soul, definition.icon, definition.row.data());
  if (!instance) {
    return 0;
  }
  // The run starts where the server's is: the elapsed part of a timed instance of the copy.
  const bool ours = field<const void*>(instance, kInstanceDefinition) == definition.row.data();
  if (ours && definition.duration_ms > 0 && remaining_ms >= 0 &&
      remaining_ms < definition.duration_ms) {
    const double duration = field<double>(instance, kTimedDuration);
    const double elapsed = double(definition.duration_ms - remaining_ms) / 1000.0;
    if (duration > 0 && elapsed < duration) {
      set_field<double>(instance, kTimedElapsed, elapsed);
    }
  }
  return field<std::uint64_t>(instance, kInstanceId);
}

void show(void* soul, const CustomBuff& buff) {
  const auto known = shown.find(buff.name);
  if (known != shown.end()) {
    remove_instance(known->second.instance);
    shown.erase(known);
  }
  const Definition* definition = definition_for(buff);
  if (!definition) {
    if (log_line) {
      log_line("CUSTOM BUFF %s: no definition of its icon %s", buff.name.c_str(),
               integration::guid_text(buff.icon).c_str());
    }
    return;
  }
  Shown entry;
  entry.definition = definition;
  entry.instance = add(soul, *definition, buff.remaining_ms);
  const std::int64_t left = buff.remaining_ms >= 0 ? buff.remaining_ms : buff.duration_ms;
  entry.ends_at = left >= 0 ? GetTickCount64() + ULONGLONG(left) : 0;
  shown[buff.name] = entry;
  if (log_line) {
    log_line("CUSTOM BUFF show %s icon=%s duration=%lld remaining=%lld instance=%llu params='%s'",
             buff.name.c_str(), integration::guid_text(buff.icon).c_str(),
             static_cast<long long>(buff.duration_ms), static_cast<long long>(buff.remaining_ms),
             static_cast<unsigned long long>(entry.instance), buff.params.c_str());
  }
}

void hide(const CustomBuff& buff) {
  const auto known = shown.find(buff.name);
  if (known == shown.end()) {
    return;
  }
  remove_instance(known->second.instance);
  shown.erase(known);
  if (log_line) {
    log_line("CUSTOM BUFF hide %s", buff.name.c_str());
  }
}

// Takes every custom buff off; the manager finds an instance by its id alone.
void remove_all() {
  for (const auto& [name, entry] : shown) {
    (void)name;
    remove_instance(entry.instance);
  }
  shown.clear();
}

}  // namespace

void set_logger(Logger logger) {
  log_line = logger;
}

void set_string_from_text(void* native) {
  string_from_text = reinterpret_cast<StringFromText>(native);
}

void tick(void* soul) {
  if (soul == current_soul) {
    return;
  }
  if (!soul) {
    remove_all();
  }
  // A new body: keep() puts the buffs on it.
  current_soul = soul;
}

void apply(void* soul, const std::vector<CustomBuff>& orders) {
  if (!soul) {
    return;
  }
  // The copied definition row holds the game's own strings (CryString, a reference count before
  // the text); plain C strings in it made the game's string copy fault (0.2.1). Without the
  // game's string constructor a custom buff is not shown; the server still applies it.
  if (!string_from_text) {
    for (const CustomBuff& order : orders) {
      if (log_line && order.kind == CustomBuff::Kind::Show) {
        log_line("CUSTOM BUFF %s: not shown (no string constructor)", order.name.c_str());
      }
    }
    return;
  }
  for (const CustomBuff& order : orders) {
    if (order.kind == CustomBuff::Kind::Show) {
      show(soul, order);
    } else {
      hide(order);
    }
  }
}

void keep(void* soul) {
  if (!soul || shown.empty()) {
    return;
  }
  const std::vector<std::uint64_t> carried = instance_ids(soul);
  const ULONGLONG now = GetTickCount64();
  for (auto it = shown.begin(); it != shown.end();) {
    Shown& entry = it->second;
    const bool on_soul = entry.instance != 0 &&
                         std::find(carried.begin(), carried.end(), entry.instance) != carried.end();
    if (on_soul) {
      ++it;
      continue;
    }
    if (entry.ends_at != 0 && now + kPutBackMarginMs >= entry.ends_at) {
      // Its run is over here; the server's hide follows.
      it = shown.erase(it);
      continue;
    }
    const std::int64_t remaining = entry.ends_at != 0 ? std::int64_t(entry.ends_at - now) : -1;
    entry.instance = add(soul, *entry.definition, remaining);
    if (log_line) {
      log_line("CUSTOM BUFF %s put back instance=%llu", it->first.c_str(),
               static_cast<unsigned long long>(entry.instance));
    }
    if (entry.instance == 0) {
      // The game takes it no more; the next show tries again.
      it = shown.erase(it);
      continue;
    }
    ++it;
  }
}

bool owns(const void* definition) {
  return rows.count(definition) != 0;
}

}  // namespace kcds::runtime::buffs::custom
