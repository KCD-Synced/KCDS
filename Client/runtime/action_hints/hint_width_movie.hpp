// The hint width movie (hint_width.hpp): a SWF 8 movie of ActionScript 2 the client writes
// itself, loaded into the game's HUD.
#ifndef KCDS_RUNTIME_ACTION_HINTS_HINT_WIDTH_MOVIE_HPP
#define KCDS_RUNTIME_ACTION_HINTS_HINT_WIDTH_MOVIE_HPP

#include <cstdint>
#include <vector>

namespace kcds::runtime::action_hints {

// The movie's bytes.
std::vector<std::uint8_t> hint_width_movie();

}  // namespace kcds::runtime::action_hints

#endif
