// The max column's own movie (max_column.hpp): a SWF 8 movie of ActionScript 2 the client
// writes itself, loaded into the game's inventory list.
#ifndef KCDS_RUNTIME_INVENTORY_MAX_COLUMN_MOVIE_HPP
#define KCDS_RUNTIME_INVENTORY_MAX_COLUMN_MOVIE_HPP

#include <cstdint>
#include <vector>

namespace kcds::runtime::inventory {

// The movie's bytes.
std::vector<std::uint8_t> max_column_movie();

}  // namespace kcds::runtime::inventory

#endif
