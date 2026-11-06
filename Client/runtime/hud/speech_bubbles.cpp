#include "speech_bubbles.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include "../core/natives.hpp"
#include "MinHook.h"

namespace kcds::runtime::hud {
namespace {

// The game services' entity module, and its HUD bubbles interface.
constexpr std::size_t kServicesEntityModule = 224;
constexpr std::size_t kEntityModuleBubbles = 384;
constexpr std::size_t kCreateSlot = 2;
constexpr std::size_t kReleaseSlot = 3;
constexpr std::size_t kSetTextSlot = 4;
constexpr std::size_t kPlaceSlot = 5;

using Create = unsigned(__fastcall*)(void* bubbles);
using Release = void(__fastcall*)(void* bubbles, unsigned id);
using SetText = void(__fastcall*)(void* bubbles, unsigned id, void* const* speaker,
                                  void* const* text, bool localize, bool flag);
using Place = void(__fastcall*)(void* bubbles, unsigned id, const float* position, float scale);
using StringFromText = void*(__fastcall*)(void* string, const char* text);
using StringFree = void(__fastcall*)(void* string);

BubbleNatives natives;

template <class Function>
Function slot(void* object, std::size_t index) {
  return reinterpret_cast<Function>((*static_cast<void***>(object))[index]);
}

void* field(void* object, std::size_t offset) {
  void* value = nullptr;
  std::memcpy(&value, static_cast<std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

void* bubbles() {
  void* services = natives.services ? natives.services() : nullptr;
  void* module = services ? field(services, kServicesEntityModule) : nullptr;
  return module ? field(module, kEntityModuleBubbles) : nullptr;
}

}  // namespace

bool setting_own_text = false;

void* hud_bubbles() {
  return bubbles();
}

bool own_text() {
  return setting_own_text;
}

unsigned create_bubble(const std::string& speaker, const std::string& text, bool localize) {
  void* hud = bubbles();
  if (!hud || !natives.string_from_text || !natives.string_free) {
    return 0;
  }
  const unsigned id = slot<Create>(hud, kCreateSlot)(hud);
  if (!id) {
    return 0;
  }
  void* name = nullptr;
  void* line = nullptr;
  reinterpret_cast<StringFromText>(natives.string_from_text)(&name, speaker.c_str());
  reinterpret_cast<StringFromText>(natives.string_from_text)(&line, text.c_str());
  setting_own_text = true;
  slot<SetText>(hud, kSetTextSlot)(hud, id, &line, &name, localize, false);
  setting_own_text = false;
  reinterpret_cast<StringFree>(natives.string_free)(&name);
  reinterpret_cast<StringFree>(natives.string_free)(&line);
  return id;
}

void place_bubble(unsigned id, const float position[3], float scale) {
  void* hud = bubbles();
  if (hud && id) {
    slot<Place>(hud, kPlaceSlot)(hud, id, position, scale);
  }
}

namespace {

constexpr std::size_t kSubtitleSetTextSlot = 1;
constexpr std::size_t kSubtitleClearSlot = 2;
// The interface sits at the module's + 0x58 (SetText: lea rsi, [rcx - 58h]).
constexpr std::size_t kModuleInterface = 0x58;

using SubtitleSetText = void(__fastcall*)(void* subtitles, void* const* text, void* const* name,
                                          bool is_player, bool force, bool localize);
using SubtitleClear = void(__fastcall*)(void* subtitles);
using SubtitlesConstructor = void*(__fastcall*)(void* module);

// The game's subtitles interface, taken where the game makes its subtitles module (C_UISubtitles'
// constructor) or uses it (its SetText): no search through memory (a search held a
// frame for 17 s, then 0.64 s, and found nothing).
std::atomic<void*> captured{nullptr};
SubtitlesConstructor original_constructor = nullptr;
SubtitleSetText original_set_text = nullptr;

void note_interface(void* subtitles) {
  if (subtitles && captured.exchange(subtitles) != subtitles && natives.log) {
    natives.log("SUBTITLES interface %p", subtitles);
  }
}

void* __fastcall hooked_constructor(void* module) {
  void* made = original_constructor(module);
  if (made) {
    note_interface(static_cast<std::uint8_t*>(made) + kModuleInterface);
  }
  return made;
}

void __fastcall hooked_set_text(void* subtitles, void* const* text, void* const* name,
                                bool is_player, bool force, bool localize) {
  note_interface(subtitles);
  original_set_text(subtitles, text, name, is_player, force, localize);
}

void* subtitles_interface() {
  return captured.load();
}

}  // namespace

void install_bubbles(const BubbleNatives& given) {
  natives = given;
  if (natives.subtitles_constructor &&
      MH_CreateHook(natives.subtitles_constructor, reinterpret_cast<void*>(&hooked_constructor),
                    reinterpret_cast<void**>(&original_constructor)) == MH_OK) {
    MH_EnableHook(natives.subtitles_constructor);
  }
  if (natives.subtitle_set_text &&
      MH_CreateHook(natives.subtitle_set_text, reinterpret_cast<void*>(&hooked_set_text),
                    reinterpret_cast<void**>(&original_set_text)) == MH_OK) {
    MH_EnableHook(natives.subtitle_set_text);
  }
}

bool show_subtitle(const std::string& speaker, const std::string& text, bool localize) {
  void* subtitles = subtitles_interface();
  if (!subtitles || !natives.string_from_text || !natives.string_free || text.empty()) {
    return false;
  }
  void* name = nullptr;
  void* line = nullptr;
  reinterpret_cast<StringFromText>(natives.string_from_text)(&name, speaker.c_str());
  reinterpret_cast<StringFromText>(natives.string_from_text)(&line, text.c_str());
  slot<SubtitleSetText>(subtitles, kSubtitleSetTextSlot)(subtitles, &line, &name, false, false,
                                                         localize);
  reinterpret_cast<StringFree>(natives.string_free)(&name);
  reinterpret_cast<StringFree>(natives.string_free)(&line);
  return true;
}

void clear_subtitle() {
  void* subtitles = subtitles_interface();
  if (subtitles) {
    slot<SubtitleClear>(subtitles, kSubtitleClearSlot)(subtitles);
  }
}

void release_bubble(unsigned id) {
  void* hud = bubbles();
  if (hud && id) {
    slot<Release>(hud, kReleaseSlot)(hud, id);
  }
}

}  // namespace kcds::runtime::hud
