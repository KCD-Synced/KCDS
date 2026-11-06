// The game's Search and Gut on a dead animal, WHGame 1.5.5.
//
// Scripts/Entities/actor/BasicAnimal.lua AddAnimalLootAction offers neither while the soul has the
// script context animal_disableLootButcherActions. The soul's script binding HasScriptContext
// (binding, handler, the context's name) is hooked: a server NPC's body whose carcass
// the server closed (NpcCarcassClosed) answers yes to that context; everything else is the game's
// own answer. Engine thread.
#pragma once

namespace kcds::runtime::npcs::carcass_prompts {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* has_script_context = nullptr;  // the binding
  void* soul_of_handler = nullptr;     // the handler's soul
  void* return_bool = nullptr;         // the handler's Return(bool)
};

// False when a native is missing or the hook fails; the prompts are then the game's own.
bool install(const Natives& natives, Logger logger);

}  // namespace kcds::runtime::npcs::carcass_prompts
