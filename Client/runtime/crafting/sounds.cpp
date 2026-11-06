#include "sounds.hpp"

#include <windows.h>

#include <array>
#include <cstddef>

#include "MinHook.h"

namespace kcds::runtime::crafting::sounds {
namespace {

// A shared_ptr as the entity's component getter hands it out.
struct SharedComponent {
  void* object = nullptr;
  void* control = nullptr;
};
// CryAudio's request user data: owner, data, data owner, flags (none: no callbacks).
struct AudioUserData {
  void* owner = nullptr;
  void* data = nullptr;
  void* data_owner = nullptr;
  std::uint32_t flags = 0;
};
using EntityComponent = SharedComponent*(__fastcall*)(void* entity, SharedComponent* out, int type);
using TriggerId = bool(__fastcall*)(void* audio, const char* name, std::uint32_t* id);
// IEntityAudioComponent::ExecuteTrigger says whether the trigger started; the callers
// rely on it: the smithing minigame starts its overburning loop again in every frame it is told the
// start failed (the hook returned nothing, the minigame started a new FMOD event instance
// every frame and FMOD Studio's command buffer grew to 64 MiB, out of memory). StopTrigger
// passes on its own result.
using ExecuteTrigger = bool(__fastcall*)(void* component, std::uint32_t trigger, std::uint32_t zero,
                                         std::uint32_t aux_object, const AudioUserData* data);
using StopTrigger = std::uint64_t(__fastcall*)(void* component, std::uint32_t trigger,
                                               std::uint32_t aux_object);
// IEntityAudioComponent::SetParameter, as the minigames call it: the value in a float
// register, the default aux object.
using SetParameter = std::uint64_t(__fastcall*)(void* component, std::uint32_t parameter,
                                                float value, std::uint32_t aux_object);
constexpr int kAudioComponent = 3;
constexpr std::size_t kGetTriggerId = 7;
constexpr std::size_t kExecuteTrigger = 30;
constexpr std::size_t kStopTrigger = 31;
constexpr std::size_t kSetParameter = 34;
constexpr std::uint32_t kDefaultAuxObject = 1;
// Components watched at once (the oldest makes room), and what one keeps until it is taken.
constexpr std::size_t kWatchedMax = 64;
constexpr std::size_t kHeardMax = 64;
constexpr std::size_t kParametersMax = 16;
// Triggers started on components nobody watches yet, kept this long for a watch that begins right
// after (the grindstone's minigame starts its whetstone loop as it opens, in the same
// frame and before the grinding watch begins; the loop then never reached the other players).
constexpr std::size_t kRecentMax = 32;
constexpr ULONGLONG kRecentMilliseconds = 1000;

struct Watched {
  void* component = nullptr;
  std::vector<Heard> heard;
  std::vector<Parameter> parameters;
};

struct Recent {
  void* component = nullptr;
  Heard heard;
  ULONGLONG at = 0;
};

Natives natives;
ExecuteTrigger original_execute = nullptr;
StopTrigger original_stop = nullptr;
SetParameter original_parameter = nullptr;
bool hooked = false;
std::array<Watched, kWatchedMax> watched{};
std::size_t next_slot = 0;
std::array<Recent, kRecentMax> recent{};
std::size_t next_recent = 0;
SRWLOCK lock = SRWLOCK_INIT;

template <class Fn>
Fn slot_of(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

// Releases the getter's shared_ptr (its control block's counts at + 8 and + 12).
void release(const SharedComponent& pointer) {
  if (!pointer.control) {
    return;
  }
  auto counts = reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(pointer.control) + 8);
  if (_InterlockedExchangeAdd(counts, -1) == 1) {
    void** vtable = *static_cast<void***>(pointer.control);
    reinterpret_cast<void(__fastcall*)(void*)>(vtable[0])(pointer.control);
    if (_InterlockedExchangeAdd(counts + 1, -1) == 1) {
      reinterpret_cast<void(__fastcall*)(void*)>(vtable[1])(pointer.control);
    }
  }
}

void note(void* component, std::uint32_t trigger, bool start) {
  if (!component) {
    return;
  }
  AcquireSRWLockExclusive(&lock);
  bool known = false;
  for (Watched& one : watched) {
    if (one.component == component) {
      if (one.heard.size() < kHeardMax) {
        one.heard.push_back(Heard{trigger, start});
      }
      known = true;
      break;
    }
  }
  if (!known) {
    recent[next_recent] = Recent{component, Heard{trigger, start}, GetTickCount64()};
    next_recent = (next_recent + 1) % kRecentMax;
  }
  ReleaseSRWLockExclusive(&lock);
}

bool __fastcall hooked_execute(void* component, std::uint32_t trigger, std::uint32_t zero,
                               std::uint32_t aux_object, const AudioUserData* data) {
  const bool started = original_execute(component, trigger, zero, aux_object, data);
  note(component, trigger, true);
  return started;
}

std::uint64_t __fastcall hooked_stop(void* component, std::uint32_t trigger,
                                     std::uint32_t aux_object) {
  const std::uint64_t result = original_stop(component, trigger, aux_object);
  note(component, trigger, false);
  return result;
}

std::uint64_t __fastcall hooked_parameter(void* component, std::uint32_t parameter, float value,
                                          std::uint32_t aux_object) {
  const std::uint64_t result = original_parameter(component, parameter, value, aux_object);
  if (!component) {
    return result;
  }
  AcquireSRWLockExclusive(&lock);
  for (Watched& one : watched) {
    if (one.component != component) {
      continue;
    }
    bool known = false;
    for (Parameter& kept : one.parameters) {
      if (kept.id == parameter) {
        kept.value = value;
        known = true;
      }
    }
    if (!known && one.parameters.size() < kParametersMax) {
      one.parameters.push_back(Parameter{parameter, value});
    }
    break;
  }
  ReleaseSRWLockExclusive(&lock);
  return result;
}

}  // namespace

void install(const Natives& given) {
  natives = given;
}

void* audio_of(void* thing) {
  SharedComponent component;
  if (thing && natives.entity_component) {
    reinterpret_cast<EntityComponent>(natives.entity_component)(thing, &component, kAudioComponent);
  }
  void* object = component.object;
  release(component);
  return object;
}

void watch(void* component) {
  if (!component) {
    return;
  }
  if (!hooked) {
    hooked = true;
    void** table = *static_cast<void***>(component);
    void* execute = table[kExecuteTrigger];
    void* stop = table[kStopTrigger];
    void* parameter = table[kSetParameter];
    const bool done = MH_CreateHook(execute, reinterpret_cast<void*>(&hooked_execute),
                                    reinterpret_cast<void**>(&original_execute)) == MH_OK &&
                      MH_EnableHook(execute) == MH_OK &&
                      MH_CreateHook(stop, reinterpret_cast<void*>(&hooked_stop),
                                    reinterpret_cast<void**>(&original_stop)) == MH_OK &&
                      MH_EnableHook(stop) == MH_OK;
    const bool parameters_done =
        MH_CreateHook(parameter, reinterpret_cast<void*>(&hooked_parameter),
                      reinterpret_cast<void**>(&original_parameter)) == MH_OK &&
        MH_EnableHook(parameter) == MH_OK;
    if (!parameters_done) {
      original_parameter = nullptr;
    }
    if (natives.log) {
      natives.log("SOUNDS %s (%p %p), parameters %s (%p)", done ? "watched" : "not watched",
                  execute, stop, parameters_done ? "watched" : "not watched", parameter);
    }
  }
  AcquireSRWLockExclusive(&lock);
  bool present = false;
  for (const Watched& one : watched) {
    present = present || one.component == component;
  }
  if (!present) {
    Watched& one = watched[next_slot];
    one = Watched{component, {}, {}};
    next_slot = (next_slot + 1) % kWatchedMax;
    // What the component played just before, oldest first.
    const ULONGLONG now = GetTickCount64();
    for (std::size_t i = 0; i < kRecentMax; ++i) {
      Recent& earlier = recent[(next_recent + i) % kRecentMax];
      if (earlier.component == component && now - earlier.at <= kRecentMilliseconds &&
          one.heard.size() < kHeardMax) {
        one.heard.push_back(earlier.heard);
      }
      if (earlier.component == component) {
        earlier.component = nullptr;
      }
    }
  }
  ReleaseSRWLockExclusive(&lock);
}

void take(void* component, std::vector<Heard>& out) {
  out.clear();
  if (!component) {
    return;
  }
  AcquireSRWLockExclusive(&lock);
  for (Watched& one : watched) {
    if (one.component == component) {
      out.swap(one.heard);
      break;
    }
  }
  ReleaseSRWLockExclusive(&lock);
}

void parameters(void* component, std::vector<Parameter>& out) {
  out.clear();
  if (!component) {
    return;
  }
  AcquireSRWLockShared(&lock);
  for (const Watched& one : watched) {
    if (one.component == component) {
      out = one.parameters;
      break;
    }
  }
  ReleaseSRWLockShared(&lock);
}

void* audio_system() {
  return natives.audio_system ? *natives.audio_system : nullptr;
}

std::uint32_t trigger_id(const char* name) {
  void* audio = natives.audio_system ? *natives.audio_system : nullptr;
  std::uint32_t id = 0;
  if (!audio || !name || !slot_of<TriggerId>(audio, kGetTriggerId)(audio, name, &id)) {
    return 0;
  }
  return id;
}

void play(void* thing, std::uint32_t trigger, bool start) {
  void* component = trigger != 0 ? audio_of(thing) : nullptr;
  if (!component) {
    return;
  }
  // Played through the originals, so this client's own watched components do not hear them.
  if (start) {
    const AudioUserData data;
    ExecuteTrigger execute =
        original_execute ? original_execute : slot_of<ExecuteTrigger>(component, kExecuteTrigger);
    execute(component, trigger, 0, kDefaultAuxObject, &data);
  } else {
    StopTrigger stop =
        original_stop ? original_stop : slot_of<StopTrigger>(component, kStopTrigger);
    stop(component, trigger, kDefaultAuxObject);
  }
}

void set_parameter(void* thing, std::uint32_t parameter, float value) {
  void* component = parameter != 0 ? audio_of(thing) : nullptr;
  if (!component) {
    return;
  }
  // Through the original, so this client's own watched components do not keep it.
  SetParameter set =
      original_parameter ? original_parameter : slot_of<SetParameter>(component, kSetParameter);
  set(component, parameter, value, kDefaultAuxObject);
}

}  // namespace kcds::runtime::crafting::sounds
