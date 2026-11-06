#include "whistle.hpp"

#include <Windows.h>

#include <cstring>

#include "MinHook.h"

namespace kcds::runtime::riding::whistle {
namespace {

constexpr char kWhistleTrigger[] = "v_horse_whistle";
// The player's horse call component and the component's player.
constexpr std::size_t kPlayerHorseCall = 0xD68;
constexpr std::size_t kHorseCallPlayer = 32;
// The audio system's trigger id by name, the entity's proxy of a kind, the proxy's trigger.
constexpr std::size_t kAudioTriggerIdSlot = 7;
constexpr std::size_t kEntityProxySlot = 76;
constexpr int kAudioProxy = 3;
constexpr std::size_t kExecuteTriggerSlot = 30;

using CallHorse = void(__fastcall*)(void* component);
using WhistleSound = void(__fastcall*)(void* component);
using EmptyCallback = void*(__fastcall*)();
using TriggerId = void(__fastcall*)(void* audio, const char* name, std::uint32_t* id);
using EntityProxy = void*(__fastcall*)(void* entity, void* out, int kind);
using ExecuteTrigger = void(__fastcall*)(void* proxy, std::uint32_t id, int lip_sync, int proxy_id,
                                         void* callback);

Natives natives;
Logger log_line = nullptr;
WhistleSound original_whistle_sound = nullptr;
bool whistled = false;
std::uint32_t trigger_id = 0;

template <class T>
T slot(void* object, std::size_t index) {
  return reinterpret_cast<T>((*reinterpret_cast<void***>(object))[index]);
}

void __fastcall hooked_whistle_sound(void* component) {
  original_whistle_sound(component);
  void* player =
      component
          ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(component) + kHorseCallPlayer)
          : nullptr;
  if (player && natives.local_player && player == natives.local_player()) {
    whistled = true;
  }
}

// Lets go of the entity proxy's shared pointer {object, control block} as the game does: the
// strong count at + 8, the weak at + 12; the block's slot 0 destroys the object, slot 1 itself.
void release(void* control) {
  if (!control) {
    return;
  }
  auto counts = reinterpret_cast<volatile LONG*>(static_cast<std::uint8_t*>(control) + 8);
  if (InterlockedDecrement(&counts[0]) == 0) {
    slot<void(__fastcall*)(void*)>(control, 0)(control);
    if (InterlockedDecrement(&counts[1]) == 0) {
      slot<void(__fastcall*)(void*)>(control, 1)(control);
    }
  }
}

// Plays the whistle on an entity, at its position.
bool play_on(void* entity) {
  void* audio = natives.audio_system ? *natives.audio_system : nullptr;
  if (!entity || !audio || !natives.empty_callback) {
    return false;
  }
  if (trigger_id == 0) {
    slot<TriggerId>(audio, kAudioTriggerIdSlot)(audio, kWhistleTrigger, &trigger_id);
  }
  if (trigger_id == 0) {
    return false;
  }
  alignas(16) std::uint8_t pointer[16] = {};
  slot<EntityProxy>(entity, kEntityProxySlot)(entity, pointer, kAudioProxy);
  void* proxy = nullptr;
  void* control = nullptr;
  std::memcpy(&proxy, pointer, sizeof(proxy));
  std::memcpy(&control, pointer + 8, sizeof(control));
  if (proxy) {
    void* callback = reinterpret_cast<EmptyCallback>(natives.empty_callback)();
    slot<ExecuteTrigger>(proxy, kExecuteTriggerSlot)(proxy, trigger_id, 0, 1, callback);
  }
  release(control);
  return proxy != nullptr;
}

void whistle_local() {
  void* player = natives.local_player ? natives.local_player() : nullptr;
  void* component =
      player ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(player) + kPlayerHorseCall)
             : nullptr;
  if (component && natives.call_horse) {
    reinterpret_cast<CallHorse>(natives.call_horse)(component);
  }
  if (log_line) {
    log_line("WHISTLE the server's whistle %s", component ? "given" : "had no horse call");
  }
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  const bool hooked =
      natives.whistle_sound &&
      MH_CreateHook(natives.whistle_sound, reinterpret_cast<void*>(hooked_whistle_sound),
                    reinterpret_cast<void**>(&original_whistle_sound)) == MH_OK &&
      MH_EnableHook(natives.whistle_sound) == MH_OK;
  const bool ready = hooked && natives.call_horse && natives.empty_callback &&
                     natives.audio_system && natives.local_player && natives.replica_entity;
  if (log_line) {
    log_line("WHISTLE %s", ready ? "installed" : "unavailable");
  }
  return ready;
}

bool take_whistled() {
  const bool was = whistled;
  whistled = false;
  return was;
}

void apply(const std::vector<std::uint32_t>& whistles) {
  for (const std::uint32_t whistler : whistles) {
    if (whistler == 0) {
      whistle_local();
      continue;
    }
    void* entity = natives.replica_entity ? natives.replica_entity(whistler) : nullptr;
    const bool played = play_on(entity);
    if (log_line) {
      log_line("WHISTLE player %u whistled nearby: %s", whistler,
               played ? "heard" : "no body here");
    }
  }
}

}  // namespace kcds::runtime::riding::whistle
