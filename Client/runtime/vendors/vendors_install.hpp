// The server's vendors on the game's own shop screen: their natives found
// and installed at startup.
#pragma once

#include "../book/resource_book.hpp"

namespace kcds::game_client {

// Called by initialize_adapter() with the book's natives (book/book_install.hpp).
void install_vendors(const kcds::runtime::book::Natives& natives);

}  // namespace kcds::game_client
