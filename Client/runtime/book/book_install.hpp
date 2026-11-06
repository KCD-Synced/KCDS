// Book: the game's reading of an inventory document, its natives found and installed
// at startup. The other startup installs share these natives: the services, the
// local entity, the inventory, the item descriptors and the CryString calls.
#pragma once

#include "resource_book.hpp"

namespace kcds::game_client {

// Called by initialize_adapter(). Returns the book's natives; installed tells whether the book
// itself installed.
kcds::runtime::book::Natives install_book(bool& installed);
// Called by initialize_adapter(): the book, then in their order the installs that share its
// natives (outfits, stock, ground items, notices, carries, replica guards, game texts, aim,
// pickpocket, physics, stashes, vendors, world clock, markers, quests, item uses, appearance).
void install_book_and_shared_natives_users();

}  // namespace kcds::game_client
