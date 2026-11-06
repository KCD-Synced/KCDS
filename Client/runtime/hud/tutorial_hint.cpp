#include "tutorial_hint.hpp"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "MinHook.h"
#include "../native_ui/native_ui.hpp"

namespace kcds::runtime::hud::tutorial_hint {
namespace {

namespace sdk = kcd2s::sdk;

// IUIElement (vtable byte offsets): GetFunctionDesc(name), CallFunction(desc, SUIArguments*,
// return, 0) and CallFunction by name (name, SUIArguments*, return, 0).
constexpr std::size_t kFunctionDesc = 472;
constexpr std::size_t kCallByDesc = 544;
constexpr std::size_t kCallByName = 552;
constexpr std::size_t kSetVisible = 224;
using SetVisible = void(__fastcall*)(void* element, bool visible);
using FunctionDesc = const void*(__fastcall*)(void* element, const char* name);
using CallByDesc = bool(__fastcall*)(void* element, const void* desc, const void* args,
                                     void* result, void* unused);
using CallByName = bool(__fastcall*)(void* element, const char* name, const void* args,
                                     void* result, void* unused);

// SUIArguments: a DynArray of 32-byte entries, each a TUIData at + 8 (its type at + 16; 4 is a
// string whose pointer is at + 0); the count is at data - 4.
constexpr std::size_t kArgumentData = 8;
constexpr std::size_t kDataType = 16;
constexpr std::uint8_t kString = 4;
constexpr std::uint8_t kBool = 6;

const char* const kWatched[] = {"ShowTutorial", "HideTutorial", "HideCurrentTutorial",
                                "HideAllTutorials", "SetPlayingTutorialEvent"};
constexpr std::size_t kWatchedCount = sizeof(kWatched) / sizeof(kWatched[0]);

struct Call {
  std::string name;
  std::string id;
};

Logger log_line = nullptr;
CallByName original_call = nullptr;
CallByDesc original_call_desc = nullptr;
SetVisible original_set_visible = nullptr;
void* hooked_target = nullptr;
// The HUD's descriptors of the watched functions.
const void* watched_descs[kWatchedCount] = {};
// The HUD element instance the hook reports calls of.
void* hud = nullptr;

std::mutex calls_mutex;
std::vector<Call> calls;

// The first argument when it is a string; empty otherwise.
std::string first_string(const void* args) {
  auto data = args ? *static_cast<const std::uint8_t* const*>(args) : nullptr;
  if (!data || *reinterpret_cast<const std::int32_t*>(data - 4) < 1) {
    return {};
  }
  const std::uint8_t* first = data + kArgumentData;
  if (first[kDataType] != kString) {
    return {};
  }
  const char* text = *reinterpret_cast<const char* const*>(first);
  return text ? std::string(text) : std::string();
}

// The first argument when it is a bool; true otherwise.
bool first_flag(const void* args) {
  auto data = args ? *static_cast<const std::uint8_t* const*>(args) : nullptr;
  if (!data || *reinterpret_cast<const std::int32_t*>(data - 4) < 1) {
    return true;
  }
  const std::uint8_t* first = data + kArgumentData;
  return first[kDataType] != kBool || first[0] != 0;
}

// The game's tutorial calls; SetPlayingTutorialEvent counts only when it stops the tutorial
// events (a menu or the pause screen coming up), which takes the box down for good.
void report(const char* name, const void* args) {
  if (std::strcmp(name, "SetPlayingTutorialEvent") == 0) {
    if (first_flag(args)) {
      return;
    }
    name = "TutorialsStopped";
  }
  std::lock_guard<std::mutex> lock(calls_mutex);
  calls.push_back({name, first_string(args)});
}

bool __fastcall watched_call(void* element, const char* name, const void* args, void* result,
                             void* unused) {
  if (element == hud && name) {
    for (const char* watched : kWatched) {
      if (std::strcmp(name, watched) == 0) {
        report(watched, args);
        break;
      }
    }
  }
  return original_call(element, name, args, result, unused);
}

// The same calls through a function descriptor (the runtime's own hints come this way too; it
// tells its ids apart).
bool __fastcall watched_call_desc(void* element, const void* desc, const void* args, void* result,
                                  void* unused) {
  if (element == hud && desc) {
    for (std::size_t index = 0; index < kWatchedCount; ++index) {
      if (desc == watched_descs[index]) {
        report(kWatched[index], args);
        break;
      }
    }
  }
  return original_call_desc(element, desc, args, result, unused);
}

// The HUD going away (a menu, the pause screen) takes the tutorial box down with it.
void __fastcall watched_set_visible(void* element, bool visible) {
  if (element == hud && !visible) {
    std::lock_guard<std::mutex> lock(calls_mutex);
    calls.push_back({"HudHidden", std::string()});
  }
  original_set_visible(element, visible);
}

}  // namespace

void install(Logger logger) {
  log_line = logger;
}

void frame(bool in_world) {
  if (!in_world) {
    return;
  }
  void* element = native_ui::element_instance("HUD", 0);
  if (!element) {
    return;
  }
  hud = element;
  if (hooked_target) {
    return;
  }
  // Every element shares the class's implementation; the hooks report the HUD's calls only.
  void** vtable = *static_cast<void***>(element);
  for (std::size_t index = 0; index < kWatchedCount; ++index) {
    watched_descs[index] = reinterpret_cast<FunctionDesc>(vtable[kFunctionDesc / sizeof(void*)])(
        element, kWatched[index]);
  }
  void* target = vtable[kCallByName / sizeof(void*)];
  void* target_desc = vtable[kCallByDesc / sizeof(void*)];
  void* target_visible = vtable[kSetVisible / sizeof(void*)];
  const bool hooked = MH_CreateHook(target, reinterpret_cast<void*>(watched_call),
                                    reinterpret_cast<void**>(&original_call)) == MH_OK &&
                      MH_EnableHook(target) == MH_OK &&
                      MH_CreateHook(target_desc, reinterpret_cast<void*>(watched_call_desc),
                                    reinterpret_cast<void**>(&original_call_desc)) == MH_OK &&
                      MH_EnableHook(target_desc) == MH_OK &&
                      MH_CreateHook(target_visible, reinterpret_cast<void*>(watched_set_visible),
                                    reinterpret_cast<void**>(&original_set_visible)) == MH_OK &&
                      MH_EnableHook(target_visible) == MH_OK;
  hooked_target = target;
  if (log_line) {
    log_line("TUTORIAL HINT game calls %s", hooked ? "watched" : "unavailable");
  }
}

void pump(const void* session, const RuntimeSender& send) {
  (void)session;
  std::vector<Call> taken;
  {
    std::lock_guard<std::mutex> lock(calls_mutex);
    taken.swap(calls);
  }
  for (const Call& call : taken) {
    sdk::Value::Map message{{"op", sdk::Value(std::string("tutorialHintGame"))},
                            {"call", sdk::Value(call.name)},
                            {"id", sdk::Value(call.id)}};
    send(sdk::Value(std::move(message)));
  }
}

}  // namespace kcds::runtime::hud::tutorial_hint
