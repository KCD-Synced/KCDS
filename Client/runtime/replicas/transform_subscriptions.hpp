// The game's transform manager: owners (actors, AI objects) whose position its update, on a
// worker thread, reads each frame for their subscribers.
#pragma once

#include <cstddef>

namespace kcds::game_client {

// Cancels every transform subscription whose owner lies in the `size` bytes at `object` (the
// object and the interfaces inside it), before the object is freed. Returns how many were
// cancelled.
std::size_t cancel_transform_subscriptions(const void* object, std::size_t size);

// The bytes of a C_Player (its actor creator's allocation).
constexpr std::size_t kPlayerBytes = 3584;

}  // namespace kcds::game_client
