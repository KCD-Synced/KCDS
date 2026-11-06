// The players' appearance (outfits, grime, visor and preset guards, unisex clothing, character
// components, disguises): its natives found and installed at startup.
#pragma once

#include "../book/resource_book.hpp"

namespace kcds::game_client {

// Called by initialize_adapter() with the book's natives (book/book_install.hpp): the outfits
// and grime use its inventory calls, the components its CryString calls.
void install_outfit(const kcds::runtime::book::Natives& natives);
void install_appearance_guards(const kcds::runtime::book::Natives& natives);

}  // namespace kcds::game_client
