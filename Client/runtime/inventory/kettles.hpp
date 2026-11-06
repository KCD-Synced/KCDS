// Which cooking pot a player eats from, WHGame 1.5.5.
//
// A pot is eaten from through its trigger (an entity of the KettleActionTrigger class), whose
// script calls the soul's EatItem with the serving's class only. The level names each trigger
// after the fireplace it belongs to, with the fireplace's level GUID in brackets
// ("KettleActionTrigger1[Fireplace/fireplace_home5_29410bf0-15c5-...]"), the same on every client.
// The client keeps the triggers the level spawns and their fireplaces; a pot meal is the nearest
// trigger's, within reach of the local player.
//
// kettle_spawned on any thread; the rest on the engine thread.
#ifndef KCDS_RUNTIME_INVENTORY_KETTLES_HPP
#define KCDS_RUNTIME_INVENTORY_KETTLES_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace kcds::runtime::inventory {

struct KettleNatives {
  // Where an entity is; false when there is no such entity.
  bool (*entity_position)(std::uint32_t entity, float* position) = nullptr;
  // Where the local player is; false without one.
  bool (*player_position)(float* position) = nullptr;
};

void install_kettles(const KettleNatives& natives);

// The fireplace GUID a trigger's level name holds (lower case, dashed), or "" for none.
std::string fireplace_of_name(const char* name);

// An entity of the KettleActionTrigger class was spawned with this level name.
void kettle_spawned(std::uint32_t entity, const char* name);

// The fireplace GUID of the pot the local player stands at (its trigger within reach), or
// nothing.
std::optional<std::string> pot_at_player();

}  // namespace kcds::runtime::inventory

#endif  // KCDS_RUNTIME_INVENTORY_KETTLES_HPP
