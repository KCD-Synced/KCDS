// Dice badges in a match between two players, WHGame 1.5.5.
//
// A player's badge in the game's dice minigame is an item of class DiceBadge (item class type 7:
// its tier at class slot 10, its effect at slot 11) that the player's badge (C_DiceBadge, C_Dice +
// 1232 + 16 x the player's index) holds at + 24; + 32 holds the other player's badge item (the
// Defence badge reads its tier). The game binds both when it has made the players' models (C_Dice
// phase 4): the local player's from the item selection before the match, the
// opponent's from its inventory by the start's badge word, and puts each badge on the table at
// its tag point. It then plays them by its own rules (C_DiceBadge's table).
//
// In a match between two players the badges are the server's: both are made as items in the other
// player's replica's inventory before the match starts (the local player's inventory stays as the
// server has it) and bound in place of the game's own choice, none for a player without one.
//
// Engine thread only.
#pragma once

#include <cstdint>
#include <functional>

#include "kcd2s/integration/dice.hpp"

namespace kcds::runtime::dice::badges {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* bind = nullptr;   // C_Dice binding both players' badges
  void* place = nullptr;  // a badge put on the table at a tag point
  // An item of the class made in an entity's inventory (C_Item*), or null.
  std::function<void*(std::uint32_t entity, const kcd2s::integration::Guid& item)> create_item;
  // The local player's dice minigame of the current match (C_Dice), or null.
  std::function<void*()> match_minigame;
  Logger log = nullptr;
};

bool install(const Natives& natives);

// The badges of the match about to start (the local player's and the other player's), made in the
// other player's replica's inventory; the game binds them when it has made the players' models.
void prepare(std::uint32_t match, std::uint32_t opponent, const kcd2s::integration::DiceBadge& own,
             const kcd2s::integration::DiceBadge& theirs);

// The match is over: its badges are not bound again.
void forget(std::uint32_t match);

}  // namespace kcds::runtime::dice::badges
