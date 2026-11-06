// Perks the player learns on the perk screen ask the server first:
// the server decides (playerPerkLearning, Progression.lockPerks), so a player cannot learn what
// the server does not allow, and the game cannot grant a perk the server blocks.
//
// The perk screen learns through a C_LearnPerkEffect, whose Apply calls the game's
// learn on the soul's perk points (soul + 1968, whose + 8 is the perk set at
// soul + 1880) with the perk's GUID. The learn checks the perk set's offered perks
// (its tree level, parent and exclusive partner), takes the perk's definition (on the
// definitions: its + 72 is the stat it is paid from, 10 for a skill, whose id is at
// + 76, 35 being the main level), spends one of that tree's points and adds the perk (AddPerk),
// refunding the point when the add fails (owned, or excluded by the game).
//
// The learn is hooked. On the local player's points, a learn the game would allow is not done:
// the perk is held back as a request, and the server lets it through with a LearnPerk order
// (learn() below, the game's own learn, the point spent) or turns it down with a RefusePerk
// order. A learn the game would refuse runs as the game's own and fails as it would.
//
// AddPerk is hooked too: on the local player's perk set, a perk the server blocks is not added
// unless the server itself grants it (ServerGrant), whatever in the game grants it.
//
// The hooks run on the thread the game applies RPG effects on (the engine thread).
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "kcd2s/integration/progression.hpp"

namespace kcds::runtime::progression::perk_gate {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* learn = nullptr;        // the game's learn, hooked
  void* add_perk = nullptr;     // the perk set's add, hooked
  void* offered = nullptr;      // whether the perk set offers a perk
  void* add_refused = nullptr;  // whether the perk set's add refuses a perk
  void* definition = nullptr;   // a perk's definition
  void* definitions = nullptr;  // the perk definitions it searches
};

// False when a native is missing or a hook fails.
bool install(const Natives& natives, Logger logger);

// Every frame: the local player's soul (nullptr outside a server world: nothing is held back and
// the block list is dropped).
void tick(void* soul);

// The perks the player asked for since the last call, oldest first.
std::vector<kcd2s::integration::PerkId> take_requests();

// The server let a requested perk through: the game's own learn on the soul, a point spent.
bool learn(void* soul, const kcd2s::integration::PerkId& perk);

// The server turned a requested perk down: it may be asked for again, and the refusal goes to
// the resource runtime (progressionPerkRefused).
void refused(const kcd2s::integration::PerkId& perk, kcd2s::integration::PerkRefusal refusal);

// The server's refusals since the last call, oldest first.
std::vector<std::pair<kcd2s::integration::PerkId, kcd2s::integration::PerkRefusal>> take_refusals();

// The server's whole block list.
void set_blocked(std::vector<kcd2s::integration::PerkId> perks);

// While one lives, the local player's perk set takes blocked perks: the server's own orders.
class ServerGrant {
 public:
  ServerGrant();
  ~ServerGrant();
  ServerGrant(const ServerGrant&) = delete;
  ServerGrant& operator=(const ServerGrant&) = delete;
};

}  // namespace kcds::runtime::progression::perk_gate
