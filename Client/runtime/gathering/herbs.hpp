// Herb gathering through the server, WHGame 1.5.5.
//
// A plant is picked through the shared Herb_InteractorEntity (PickableArea) and the herb gathering
// minigame (C_HerbGathering). At the picking animation's event the minigame calls the
// respawn manager's update (wh::environmentmodule::C_RespawnManager, the environment module at
// game services + 312, its slot 11; the manager's slot 2), which runs the manager's
// pick for the plant under the crosshair the manager keeps at + 96: the pick takes
// that kind's shown plants within a radius (the player's own when none is given: game services
// + 304 → + 152, slot 18, of the player's soul), hides them (the render node's Hide, flag 0x100)
// and records them with a regrowth deadline in game milliseconds (+ 160), returning how many it
// took (C_HerbGathering + 152). The minigame then gives the herb (its item class at
// + 112, the amount from the same object's slot 17, plants x factor + 128).
//
// In a server's world:
// - the local pick runs as the game's, with a deadline that never comes (the server decides when
//   plants grow back), and is remembered;
// - the herb is not given: the client asks the server (GATHER_REPORT) and ends the minigame as
//   the game does after giving; the server gives the herb itself, or refuses and the pick's
//   plants come back (the manager's respawn, for that position, radius and kind);
// - the server's picks in the player's world (GATHER_PLANTS) run the same pick at their place,
//   and their regrowth the respawn.
// A pick's records stay in the manager (+ 160, a hashed list: end node at + 8, a 48-byte node's
// record at + 16) and hide the plants that stream in later inside them (on a render
// node added); a pick that grows back has its records erased with the respawn.
// Offline, everything goes as the game does it.
//
// Engine thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "kcd2s/integration/gathering.hpp"

namespace kcds::runtime::gathering {

using Logger = void (*)(const char* format, ...);

struct Natives {
  std::function<void*()> services;     // the game services
  std::function<void*()> local_actor;  // the local player's actor (soul at + 1640)
  void* pick = nullptr;
  void* respawn = nullptr;
  void* erase_record = nullptr;
  void* grant = nullptr;
  void* collect = nullptr;  // the plants of a kind within a radius
  void* reserve = nullptr;  // its list's room
  Logger log = nullptr;
};

bool install(const Natives& natives);

// Every frame: whether the client is in a server's world (picks go through the server only then).
void set_online(bool online);

// The local player's picks to ask the server about since the last call.
std::vector<kcd2s::integration::GatherReport> take_reports();

// The server's answer to a pick, and the plants picked or grown back in the player's world.
void apply_verdict(const kcd2s::integration::GatherVerdict& verdict);
void apply_plants(const kcd2s::integration::GatherPlants& plants);

// Writes every plant the respawn manager holds now (its grid: the level's pickable plants that
// have streamed in around the player) to the file, one "kind,x,y,z" line each, for a sweep of a
// level (the test hook KCD2S.HerbDump, Dev/tools/gathering/build_herbs.py). How many; -1 when there
// is no manager or the file could not be written.
long dump_plants(const wchar_t* path);

}  // namespace kcds::runtime::gathering
