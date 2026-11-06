// Items on the ground, projectiles that landed and cooking pots: their natives
// found and installed at startup together, since the landings and the pots use the ground's
// entity natives.
#pragma once

#include "../book/resource_book.hpp"

namespace kcds::game_client {

// Called by initialize_adapter() with the book's natives (book/book_install.hpp), whose item
// descriptor and inventory calls the ground items share.
void install_ground(const kcds::runtime::book::Natives& natives);

}  // namespace kcds::game_client
