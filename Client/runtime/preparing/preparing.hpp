// Cooking, smoking and drying at the game's own fires, smokehouses and drying racks, asked of the
// server, WHGame 1.5.5.
//
// The game starts the player's preparing from the station's smart object: its behaviour adds an
// interrupt to the player ("player_cooking" at a fire, so_fireplace.xml; "player_useSmokehouse" at
// a smokehouse or drying rack, so_smokehouse.xml) once the player chose what to prepare. The
// player's behaviour plays the station's animation and then converts the items, one
// PrepareFood (Cook, Smoke, Dry) or DryHerb node per item; both start in C_PrepareItem's start
// (slot 50 of their node wrappers).
//
// Two hooks:
// - the interrupt node's start: for those two interrupts, while the client is in a
//   server's world, the client asks (CRAFT_REPORT Prepare, kcd2s/integration/crafting.hpp) and the
//   interrupt goes on as the game makes it (a node cannot be held: its context goes back to the
//   tree as soon as its start returns, live);
// - C_PrepareItem's start: after a refusal (CRAFT_VERDICT no) the conversions of the next
//   kRefusedMs fail (the node's status 6), so the game converts nothing and takes nothing; the
//   animation has played. With no answer yet, or a yes, the game converts as it does alone.
//
// Engine thread only.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "kcd2s/integration/crafting.hpp"

namespace kcds::runtime::preparing {

using Logger = void (*)(const char* format, ...);

// The requests of preparing asks count from here, apart from the station asks' own.
inline constexpr std::uint32_t kFirstRequest = 0x40000000u;

struct Natives {
  void* interrupt_node = nullptr;    // the interrupt node's start
  void* prepare_item = nullptr;      // C_PrepareItem's start
  void* node_context = nullptr;      // a node's context, (through)
  void* string_attribute = nullptr;  // a node's string attribute, read
  Logger log = nullptr;
  // Where the local player stands; false when there is no local player.
  std::function<bool(float position[3])> local_position;
};

void install(const Natives& natives);

// Whether the client is in a server's world (asks are made only then).
void set_online(bool online);

// The asks to send since the last call.
std::vector<kcd2s::integration::CraftReport> take_asks();

// The server's answer; false when the request is not a preparing ask.
bool apply_verdict(const kcd2s::integration::CraftVerdict& verdict);

}  // namespace kcds::runtime::preparing
