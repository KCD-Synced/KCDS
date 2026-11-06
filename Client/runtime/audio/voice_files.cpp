#include "voice_files.hpp"

#include <Windows.h>

#include <cstddef>
#include <unordered_map>
#include <utility>

#include "../crafting/sounds.hpp"

// After windows.h.
#include "MinHook.h"

namespace kcds::runtime::audio::voices {
namespace {

namespace sounds = crafting::sounds;

// The audio system's slot for a switch's id by name (IAudioSystem).
constexpr std::size_t kSwitchId = 9;
// The audio component's slots.
constexpr std::size_t kPlayFile = 28;
constexpr std::size_t kSetSwitchState = 33;
// The component's own audio object, where the game plays its spoken files.
constexpr std::uint32_t kDefaultAuxObject = 1;
constexpr char kFilePurposeSwitch[] = "standalone_file_purpose";
// Files kept between two takes at most (a frame normally holds none or one).
constexpr std::size_t kMaxPlayed = 64;
// The longest file name kept.
constexpr std::size_t kMaxFile = 200;
// Components whose purpose is remembered; past it the memory starts anew (components come and go,
// and a freed one's address may be another's later).
constexpr std::size_t kMaxPurposes = 4096;

using NameId = bool(__fastcall*)(void* audio, const char* name, std::uint32_t* id);
using PlayFile = void(__fastcall*)(void* component, const char* file, float start,
                                   std::uint32_t aux_object, void* callback);
using SetSwitchState = void(__fastcall*)(void* component, std::uint32_t switch_id,
                                         std::uint32_t state, std::uint32_t aux_object);
using EmptyCallback = void*(__fastcall*)();

PlayFile original_play_file = nullptr;
SetSwitchState original_set_switch_state = nullptr;
bool tried = false;
bool hooked = false;
bool trace = false;
void* local_component = nullptr;
Logger log_line = nullptr;
std::uint32_t purpose_switch = 0;
EmptyCallback empty_callback = nullptr;

// The purpose state last set on each component's default audio object.
std::unordered_map<void*, std::uint32_t> purposes;
std::vector<Played> played;

// The file name's characters under a fault guard, at most kMaxFile; 0 when unreadable.
std::size_t copy_file(const char* file, char* out) {
  std::size_t length = 0;
  __try {
    while (length < kMaxFile && file[length] != '\0') {
      out[length] = file[length];
      ++length;
    }
  } __except (1) {
    length = 0;
  }
  return length;
}

void __fastcall on_set_switch_state(void* component, std::uint32_t switch_id, std::uint32_t state,
                                    std::uint32_t aux_object) {
  if (purpose_switch != 0 && switch_id == purpose_switch && aux_object == kDefaultAuxObject) {
    if (purposes.size() >= kMaxPurposes) {
      purposes.clear();
    }
    purposes[component] = state;
  }
  original_set_switch_state(component, switch_id, state, aux_object);
}

void __fastcall on_play_file(void* component, const char* file, float start,
                             std::uint32_t aux_object, void* callback) {
  char buffer[kMaxFile] = {};
  const std::size_t length = file ? copy_file(file, buffer) : 0;
  if (length > 0 && aux_object == kDefaultAuxObject && played.size() < kMaxPlayed) {
    Played entry;
    entry.component = component;
    entry.file.assign(buffer, length);
    const auto purpose = purposes.find(component);
    entry.purpose = purpose == purposes.end() ? 0 : purpose->second;
    entry.at = GetTickCount64();
    played.push_back(std::move(entry));
  }
  if (trace && log_line) {
    const bool local = component && component == local_component;
    log_line("AUDIO play file %s%s", length > 0 ? std::string(buffer, length).c_str() : "(none)",
             local ? " (local player)" : "");
  }
  original_play_file(component, file, start, aux_object, callback);
}

bool hook(void* target, void* detour, void** original) {
  return MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
}

}  // namespace

void set_empty_callback(void* get_empty_object) {
  empty_callback = reinterpret_cast<EmptyCallback>(get_empty_object);
}

void watch(void* component, void* local, Logger logger) {
  if (local) {
    local_component = local;
  }
  if (tried || !component) {
    return;
  }
  void* audio = sounds::audio_system();
  if (!audio) {
    return;
  }
  tried = true;
  log_line = logger;
  trace = GetEnvironmentVariableW(L"KCDS_TEST_HOOKS", nullptr, 0) != 0;
  const auto switch_id = reinterpret_cast<NameId>((*static_cast<void***>(audio))[kSwitchId]);
  if (!switch_id(audio, kFilePurposeSwitch, &purpose_switch)) {
    purpose_switch = 0;
  }
  void** table = *static_cast<void***>(component);
  hooked = hook(table[kSetSwitchState], reinterpret_cast<void*>(&on_set_switch_state),
                reinterpret_cast<void**>(&original_set_switch_state)) &&
           hook(table[kPlayFile], reinterpret_cast<void*>(&on_play_file),
                reinterpret_cast<void**>(&original_play_file));
  if (log_line) {
    log_line("AUDIO voice files %s (purpose switch %u)%s", hooked ? "watched" : "not hooked",
             purpose_switch, trace ? ", traced" : "");
  }
}

std::vector<Played> take_played() {
  return std::exchange(played, {});
}

bool play(void* component, const std::string& file, std::uint32_t purpose) {
  if (!hooked || !component || file.empty() || purpose_switch == 0 || !empty_callback) {
    return false;
  }
  void* callback = empty_callback();
  if (purpose != 0) {
    original_set_switch_state(component, purpose_switch, purpose, kDefaultAuxObject);
  }
  original_play_file(component, file.c_str(), 0.0f, kDefaultAuxObject, callback);
  return true;
}

}  // namespace kcds::runtime::audio::voices
