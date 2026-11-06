// Whether the local player may open a crafting station, asked of the server,
// WHGame 1.5.5.
//
// The game opens a station the player uses through the player's minigame request
// (the player's component, the request it keeps at + 708: the station's id, the minigame's kind
// at + 4, 3 alchemy, 12 smithing; false when it did not start, and its caller then drops the
// request), and through its script bindings: the smithy's StartMinigame
// (C_ScriptBind_Blacksmithing) and the alchemy table's StartAlchemy, each
// (binding, script function handle, user, station) ending with the handle's slot 11
// EndFunction. All three are hooked:
// - a station another player works at (a replica's action shows it there) does not open;
// - while the client is in a server's world, the local player's start does not open at once: the
//   client asks (CRAFT_REPORT Ask, kcd2s/integration/crafting.hpp) and the start fails (the
//   script call ends); on the server's yes (CRAFT_VERDICT) the start runs again (the request put
//   back, or with a script handle of its own), on a no it stays closed;
// - any other start (offline, or another user) goes as the game does it.
//
// Engine thread only.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "kcd2s/integration/crafting.hpp"

namespace kcds::runtime::crafting::permits {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* start_minigame = nullptr;
  void* start_smithing = nullptr;
  void* start_alchemy = nullptr;
  Logger log = nullptr;
  // Whether another player works at the station standing there.
  std::function<bool(const float position[3])> smithy_taken;
  std::function<bool(const float position[3])> table_taken;
};

void install(const Natives& natives);

// Every frame: the local player's entity id, and whether the client is in a server's world (the
// starts go through the server only then).
void set_local(std::uint32_t local_entity_id, bool online);

// The asks to send since the last call.
std::vector<kcd2s::integration::CraftReport> take_asks();

// The server's answer: a yes opens the station asked about.
void apply_verdict(const kcd2s::integration::CraftVerdict& verdict);

}  // namespace kcds::runtime::crafting::permits
