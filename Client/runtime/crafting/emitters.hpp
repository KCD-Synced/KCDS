// The spawn parameters of the particle emitters a crafting station's effects use (milestone
// 0.2.3), WHGame 1.5.5: a stroke's sparks get a count scale, an overheated
// iron's smoke and an alchemy table's fire a strength.
//
// An entity's emitter is its CEntity slot 104 (GetParticleEmitter(slot)); the emitter's slot 81
// SetSpawnParams(params) takes SSpawnParams (its constructor; the count scale at + 8,
// the strength at + 12). The owner's client hooks SetSpawnParams (every emitter shares it) once a
// module watches an emitter, and keeps what the game last gave the latest emitters; every other
// client gives its own emitters the same.
//
// Engine thread only, but for the hook, which may run on any thread.
#pragma once

#include <cstddef>

namespace kcds::runtime::crafting::emitters {

using Logger = void (*)(const char* format, ...);

inline constexpr std::size_t kCountScale = 8;
inline constexpr std::size_t kStrength = 12;

struct Natives {
  void* spawn_params_init = nullptr;  // SSpawnParams' constructor
  Logger log = nullptr;
};

void install(const Natives& natives);

// A particle effect's emitter (its entity's first slot), or null while it has none.
void* of(void* thing);

// Keeps what the game gives this emitter from now on (hooks SetSpawnParams once).
void watch(void* emitter);

struct Given {
  float count = 1.0f;
  float strength = 1.0f;
};

// What the game last gave a watched emitter; false when nothing yet.
bool given(void* emitter, Given& out);

// Gives an effect's emitter the game's default spawn parameters with these two set.
void give(void* thing, const Given& parameters);

// The same for an emitter itself (one the game made at a character's attachment).
void give_emitter(void* emitter, const Given& parameters);

}  // namespace kcds::runtime::crafting::emitters
