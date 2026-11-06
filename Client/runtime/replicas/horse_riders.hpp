#pragma once

#include <cstdint>
#include <unordered_set>
#include <variant>

#include "kcd2s/core/ids.hpp"
#include "kcd2s/integration/horse.hpp"

namespace kcds::runtime::replicas {

// Use the same frame snapshot as player replay, including horses whose native bodies do not
// exist yet. Signed IDs preserve the wire comparison without aliasing malformed large IDs.
template <class Entities>
std::unordered_set<std::int64_t> snapshot_horse_riders(const Entities& entities) {
  std::unordered_set<std::int64_t> riders;
  const auto horse_type = kcd2s::core::type_id("NetworkHorse");
  for (const auto& [id, horse] : entities) {
    if (horse.type == horse_type && horse.fields.size() > kcd2s::integration::HorseRider) {
      riders.insert(std::get<std::int64_t>(horse.fields[kcd2s::integration::HorseRider]));
    }
  }
  return riders;
}

}  // namespace kcds::runtime::replicas
