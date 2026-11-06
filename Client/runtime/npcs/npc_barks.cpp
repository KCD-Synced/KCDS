#include "npc_barks.hpp"

#include <cstddef>
#include <cstring>
#include <unordered_map>
#include <utility>

#include "../hud/speech_bubbles.hpp"
#include "MinHook.h"

namespace kcds::runtime::npcs::barks {
namespace {

constexpr std::size_t kReleaseSlot = 3;
constexpr std::size_t kSetTextSlot = 4;
constexpr std::size_t kPlaceSlot = 5;
// The longest line kept (NPC_BARK's limit).
constexpr std::size_t kMaxText = 240;

using Release = void(__fastcall*)(void* bubbles, unsigned id);
using SetText = void(__fastcall*)(void* bubbles, unsigned id, void* const* text,
                                  void* const* speaker, bool localize, bool flag);
using Place = void(__fastcall*)(void* bubbles, unsigned id, const float* position, float scale);

Release original_release = nullptr;
SetText original_set_text = nullptr;
Place original_place = nullptr;
bool hooked = false;
bool failed = false;
Logger log_line = nullptr;

struct Pending {
  std::string text;
  bool localize = false;
  bool placed = false;
};
// The game's own bubbles by id, until released.
std::unordered_map<unsigned, Pending> pending;
std::vector<Placed> placed;

// Copies up to kMaxText characters into `out` under a fault guard; their count, 0 when unreadable.
std::size_t copy_chars(const char* chars, char* out) {
  std::size_t length = 0;
  __try {
    while (length < kMaxText && chars[length] != '\0') {
      out[length] = chars[length];
      ++length;
    }
  } __except (1) {
    length = 0;
  }
  return length;
}

// A CryString's characters (the string holds a pointer to them), empty when unreadable.
std::string text_of(void* const* string) {
  if (!string || !*string) {
    return {};
  }
  char buffer[kMaxText] = {};
  const std::size_t length = copy_chars(static_cast<const char*>(*string), buffer);
  return std::string(buffer, length);
}

void __fastcall hooked_set_text(void* bubbles, unsigned id, void* const* text, void* const* speaker,
                                bool localize, bool flag) {
  original_set_text(bubbles, id, text, speaker, localize, flag);
  if (hud::own_text()) {
    return;
  }
  Pending entry;
  entry.text = text_of(text);
  entry.localize = localize;
  if (!entry.text.empty()) {
    pending[id] = entry;
  }
}

void __fastcall hooked_place(void* bubbles, unsigned id, const float* position, float scale) {
  original_place(bubbles, id, position, scale);
  auto it = pending.find(id);
  if (it == pending.end() || it->second.placed || !position) {
    return;
  }
  it->second.placed = true;
  Placed one;
  one.text = it->second.text;
  one.localize = it->second.localize;
  std::memcpy(one.position, position, sizeof(one.position));
  placed.push_back(one);
}

void __fastcall hooked_release(void* bubbles, unsigned id) {
  pending.erase(id);
  original_release(bubbles, id);
}

bool hook(void* target, void* detour, void** original) {
  return target && MH_CreateHook(target, detour, original) == MH_OK &&
         MH_EnableHook(target) == MH_OK;
}

}  // namespace

void watch(void* hud_bubbles, Logger logger) {
  if (hooked || failed || !hud_bubbles) {
    return;
  }
  log_line = logger;
  void** table = *static_cast<void***>(hud_bubbles);
  hooked = hook(table[kSetTextSlot], reinterpret_cast<void*>(&hooked_set_text),
                reinterpret_cast<void**>(&original_set_text)) &&
           hook(table[kPlaceSlot], reinterpret_cast<void*>(&hooked_place),
                reinterpret_cast<void**>(&original_place)) &&
           hook(table[kReleaseSlot], reinterpret_cast<void*>(&hooked_release),
                reinterpret_cast<void**>(&original_release));
  failed = !hooked;
  if (log_line) {
    log_line("NPC barks: the HUD's bubbles %s", hooked ? "watched" : "could not be hooked");
  }
}

std::vector<Placed> take_placed() {
  return std::exchange(placed, {});
}

}  // namespace kcds::runtime::npcs::barks
