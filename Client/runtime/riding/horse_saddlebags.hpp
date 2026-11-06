// A server horse's saddlebags, held by the server.
//
// The server holds what a server horse carries (Horse#storage, a Stash) and sends its rows to the
// horse's owner in a STASH_SHOW that names the horse. The owner's game shows them in the horse's
// own inventory: its inventory screen's Horse tab and Move to horse, as for any horse of theirs.
//
//   - the rows are put into the horse's inventory (the entity's inventory, as the game finds it)
//     with the containers' fill (inventory/stock_projection.hpp fill_container), again whenever
//     the server sends them or the horse's body is made anew;
//   - the game's move of an item into another inventory (target inventory, item,
//     units or 0 for all, flag; the item's own inventory at + 144) is hooked: a move between the
//     local player's inventory and such a horse's does not happen in the game, and is asked of
//     the server instead as the container's put (the player's row) or take (the horse's row);
//     the server's answer is the next stock of both. The hook returns the item where it is (a
//     null answer, the game's own refusal, left the inventory screen waiting and stopped the
//     game's update, live); the request goes out at once (send).
//
// A closed show (the horse went to someone else) empties the horse's inventory here.
//
// Engine thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/integration/stash.hpp"

namespace kcds::runtime::riding::saddlebags {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* move_item = nullptr;  // hooked
  // The body of a horse by its network id here, or null.
  void* (*horse_entity)(std::uint32_t horse) = nullptr;
  // Its actor, whose worn tack stays in its inventory when the rows are put in (the tack lives in
  // the horse's own inventory: emptied with the rest, the game took it off), or null.
  void* (*horse_actor)(std::uint32_t horse) = nullptr;
  // Sends a take (true) or put of a horse's saddlebags to the server; false when it could not.
  bool (*send)(bool take, std::uint32_t stash, const std::string& row,
               std::uint32_t units) = nullptr;
};

// Hooks the move; false when it is missing or the hook fails.
bool install(const Natives& natives, Logger logger);

// A STASH_SHOW that names a horse.
void show(const kcd2s::integration::StashShow& shown);

// Puts the server's rows into the horses' inventories where they changed or the body is new.
void tick();

// The connection ended: no horse's saddlebags are shown any more.
void reset();

// The row of a horse's saddlebags an item of its inventory shows, by the item's handle; false for
// none (riding/horse_tack.hpp: a piece put on the horse from its saddlebags).
bool row_of(std::uint32_t horse, std::uint64_t handle, std::string& row);

// Test hooks only: the game's own move of one unit of the first item of a horse's saddlebags into
// the local player's inventory, as the inventory screen's Move does (it goes through the hook).
// False when no horse's saddlebags hold anything.
bool test_take_first();

}  // namespace kcds::runtime::riding::saddlebags
