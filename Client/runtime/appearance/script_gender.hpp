// The gender the game's scripts see of the local player, WHGame 1.5.5.
//
// A woman's player soul has the archetype Hero_female (appearance/components.cpp set_gender): its
// gender builds her female body. The game's scripts read it through soul:GetGender()
// (EntityUtils.IsFemale) and keep a woman from what the story never lets one do: an alchemy table
// and the grindstone (AlchemyTable.lua, Grindstone.lua IsUsable), mounting a horse (Horse.lua),
// grabbing a body and pickpocketing (BasicAIActions.lua). A player may do all of it whatever her
// body, so the binding (the soul script binding's GetGender: the handler's soul
// its archetype at + 3256 and the gender at + 28) answers male for a Hero_female soul:
// while it runs, the soul has the Hero archetype, the record next to it. NPC women stay women.
//
// Main thread (script calls).
#pragma once

namespace kcds::runtime::appearance::script_gender {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* get_gender = nullptr;    // the soul script binding's GetGender, hooked
  void* handler_soul = nullptr;  // the soul a script call is made on
};

// False when a native is missing: the scripts then see a woman.
bool install(const Natives& natives, Logger logger);

}  // namespace kcds::runtime::appearance::script_gender
