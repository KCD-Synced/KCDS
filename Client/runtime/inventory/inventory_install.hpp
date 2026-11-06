// The inventory (item uses, names and types): its natives found and installed at startup
#pragma once

#include "../book/resource_book.hpp"

namespace kcds::game_client {

// Called by initialize_adapter() with the book's natives (book/book_install.hpp), whose item
// descriptor and inventory calls the stock projection and the uses share.
void install_stock(const kcds::runtime::book::Natives& natives);
void install_item_uses(const kcds::runtime::book::Natives& natives);

}  // namespace kcds::game_client
