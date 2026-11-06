// How much of what the local player wears and holds equipped counts toward their load (issue),
// WHGame 1.5.5.
//
// The soul works its derived values out in (derived values, soul), its inventory's
// weight among them: every item of the inventory adds its class's weight times its amount
// through a lambda (the slot 2 of its std::function: derived values at + 8 of the
// lambda, the weight at + 28 of them). Both are hooked: while the local player's soul works its
// values out with a share under 100 %, each item it wears (appearance/outfit.hpp worn) adds only
// that share of what the game would add. The encumbrance (speed, stamina, the inventory's weight)
// follows from that weight; a new share marks the soul's values stale (item_overrides.hpp).
//
// Engine thread only.
#pragma once

#include <cstdint>
#include <vector>

namespace kcds::runtime::inventory::equipped_weight {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* derive = nullptr;      // the soul's derived values worked out
  void* add_weight = nullptr;  // an item's weight added to them
  void* (*local_actor)() = nullptr;
  std::vector<void*> (*worn)(void* actor) = nullptr;
  Logger log = nullptr;
};

void install(const Natives& natives);

// The share the server gives (0 to 100); true when it changed.
bool set_percent(std::uint8_t percent);

}  // namespace kcds::runtime::inventory::equipped_weight
