// Whether an entity is a part of one of the server's carts (named kcds_cart_<id>[_<part>]).
//
// Rays that ask whether people see or hear each other pass through carts: a player seated on a
// cart had its planks and canopy between their eyes and everyone else.
//
// Engine thread.
#pragma once

#include <string>

#include "../props/prop_entity.hpp"

namespace kcds::runtime::carts {

// For physics::first_hit_passing: true for a cart part (`context` is not used).
inline bool is_cart_part(void* entity, void* /*context*/) {
  const std::string name = props::entity::name_of(entity, 64);
  return name.compare(0, 10, "kcds_cart_") == 0;
}

}  // namespace kcds::runtime::carts
