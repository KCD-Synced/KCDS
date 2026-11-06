// The game's own prompts at its food stations (the server's Interactions.set and
// Player.setInteraction: cooking, smoking), WHGame 1.5.5.
//
// The player's interactor (C_PlayerInteractor) works out each update which entity
// the player aims at (its update, the focus at + 244), and hands it to its step
// (this, a reference to the entity, its id first), which makes the prompts and what the use key
// does. Hooked: a food trigger (C_FoodProcessingTrigger: foodProcessingType in its entity's
// Properties.Script.Misc, cooking, smoking or drying) the server has switched off is no focus, so
// it has no prompt and its key does nothing. Engine thread only.
#pragma once

namespace kcds::runtime::interactions::stations {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* interactor_step = nullptr;         // (interactor, entity reference)
  void* properties = nullptr;              // (holder of the entity, table out)
  void* sub_table = nullptr;               // (table, name, table out)
  void* (*entity)(unsigned id) = nullptr;  // the entity of an id, null when it has none
};

// False when a native is missing or the hook fails; the prompts are then the game's own.
bool install(const Natives& natives, Logger logger);

}  // namespace kcds::runtime::interactions::stations
