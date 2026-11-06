#include "emitters.hpp"

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "MinHook.h"

namespace kcds::runtime::crafting::emitters {
namespace {

constexpr std::size_t kParticleEmitter = 104;
constexpr std::size_t kSetSpawnParams = 81;
constexpr std::size_t kParamsSize = 128;  // more than SSpawnParams' 64 bytes
// The emitters kept at once (watched ones and the last the game gave parameters); the oldest makes
// room for a new one.
constexpr std::size_t kWatchedMax = 64;

using EmitterOf = void*(__fastcall*)(void* entity, int slot);
using SetSpawnParams = void(__fastcall*)(void* emitter, const void* params);
using ParamsInit = void*(__fastcall*)(void* params);

struct Watched {
  void* emitter = nullptr;
  bool known = false;
  Given given;
};

Natives natives;
SetSpawnParams original_set = nullptr;
bool hooked = false;
std::array<Watched, kWatchedMax> watched{};
std::size_t next_slot = 0;
SRWLOCK lock = SRWLOCK_INIT;

float read_float(const void* params, std::size_t offset) {
  float value = 0.0f;
  std::memcpy(&value, static_cast<const std::uint8_t*>(params) + offset, sizeof(value));
  return value;
}

void __fastcall hooked_set(void* emitter, const void* params) {
  original_set(emitter, params);
  if (!emitter || !params) {
    return;
  }
  const Given given{read_float(params, kCountScale), read_float(params, kStrength)};
  AcquireSRWLockExclusive(&lock);
  bool kept = false;
  for (Watched& one : watched) {
    if (one.emitter == emitter) {
      one.known = true;
      one.given = given;
      kept = true;
      break;
    }
  }
  // An emitter the game made anew (a stroke restarts the sparks with a new one) takes the oldest
  // place.
  if (!kept) {
    watched[next_slot] = Watched{emitter, true, given};
    next_slot = (next_slot + 1) % kWatchedMax;
  }
  ReleaseSRWLockExclusive(&lock);
}

}  // namespace

void install(const Natives& given_natives) {
  natives = given_natives;
}

void* of(void* thing) {
  if (!thing) {
    return nullptr;
  }
  auto emitter_of = reinterpret_cast<EmitterOf>((*static_cast<void***>(thing))[kParticleEmitter]);
  return emitter_of(thing, 0);
}

void watch(void* emitter) {
  if (!emitter) {
    return;
  }
  if (!hooked) {
    hooked = true;
    void* target = (*static_cast<void***>(emitter))[kSetSpawnParams];
    const bool done = MH_CreateHook(target, reinterpret_cast<void*>(&hooked_set),
                                    reinterpret_cast<void**>(&original_set)) == MH_OK &&
                      MH_EnableHook(target) == MH_OK;
    if (natives.log) {
      natives.log("EMITTERS spawn parameters %s (%p)", done ? "watched" : "not watched", target);
    }
  }
  AcquireSRWLockExclusive(&lock);
  bool present = false;
  for (const Watched& one : watched) {
    present = present || one.emitter == emitter;
  }
  if (!present) {
    watched[next_slot] = Watched{emitter, false, Given{}};
    next_slot = (next_slot + 1) % kWatchedMax;
  }
  ReleaseSRWLockExclusive(&lock);
}

bool given(void* emitter, Given& out) {
  bool found = false;
  AcquireSRWLockShared(&lock);
  for (const Watched& one : watched) {
    if (emitter && one.emitter == emitter && one.known) {
      out = one.given;
      found = true;
      break;
    }
  }
  ReleaseSRWLockShared(&lock);
  return found;
}

void give(void* thing, const Given& parameters) {
  give_emitter(of(thing), parameters);
}

void give_emitter(void* emitter, const Given& parameters) {
  if (!emitter || !natives.spawn_params_init) {
    return;
  }
  alignas(16) std::uint8_t params[kParamsSize] = {};
  reinterpret_cast<ParamsInit>(natives.spawn_params_init)(params);
  std::memcpy(params + kCountScale, &parameters.count, sizeof(float));
  std::memcpy(params + kStrength, &parameters.strength, sizeof(float));
  auto set = reinterpret_cast<SetSpawnParams>((*static_cast<void***>(emitter))[kSetSpawnParams]);
  set(emitter, params);
}

}  // namespace kcds::runtime::crafting::emitters
