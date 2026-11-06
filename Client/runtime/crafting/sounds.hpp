// The sounds a crafting station's minigame plays, WHGame
// 1.5.5: the smith's body (the forge's roar, the bellows, the hammer), an alchemy table and its
// things (the fire, the boiling, the mortar).
//
// An entity's audio component (a shared_ptr) has slot 30
// ExecuteTrigger(trigger, 0, aux object, user data), which the entity binding's
// ExecuteAudioTrigger calls, and slot 31 StopTrigger(trigger, aux object); every
// entity's audio component shares them. The owner's client hooks both and keeps each start and
// stop the game made on the components it watches; every other client starts and stops the same
// triggers on its own entities. Slot 34 SetParameter(parameter, value, aux object) sets an audio
// parameter (a grindstone's speed); the owner's client keeps the last value of each on
// the components it watches, and every other client sets the same. A parameter's id is the audio
// system's too (slot 8 GetAudioParameterID). A trigger's id is the audio system's (IAudioSystem,
// slot 7 GetAudioTriggerID(name, out)), the same on every client.
//
// Engine thread only, but for the hooks, which may run on any thread.
#pragma once

#include <cstdint>
#include <vector>

namespace kcds::runtime::crafting::sounds {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* entity_component = nullptr;
  void* const* audio_system = nullptr;  // IAudioSystem
  Logger log = nullptr;
};

void install(const Natives& natives);

// An entity's audio component, or null while it has none (it lives as long as its entity).
void* audio_of(void* thing);

// Keeps the starts and stops the game makes on this component from now on (hooks once).
void watch(void* component);

// One start or stop of a trigger.
struct Heard {
  std::uint32_t trigger = 0;
  bool start = true;
};

// The starts and stops on a watched component since the last call, in order.
void take(void* component, std::vector<Heard>& out);

// An audio parameter the game set on a watched component and its last value.
struct Parameter {
  std::uint32_t id = 0;
  float value = 0.0f;
};

// The audio parameters the game set on a watched component since it was first watched, each with
// its last value, in the order they were first set.
void parameters(void* component, std::vector<Parameter>& out);

// The audio system (IAudioSystem), null before it is up.
void* audio_system();

// A trigger's id by its name, 0 while the audio system has none.
std::uint32_t trigger_id(const char* name);

// Starts or stops a trigger on an entity, as the entity binding's ExecuteAudioTrigger and
// StopAudioTrigger do.
void play(void* thing, std::uint32_t trigger, bool start);

// Sets an audio parameter on an entity, as the game's minigames do.
void set_parameter(void* thing, std::uint32_t parameter, float value);

}  // namespace kcds::runtime::crafting::sounds
