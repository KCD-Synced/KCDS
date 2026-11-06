#include "kcd2s/replication/entity_lookup.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "kcd2s/testkit.hpp"

using kcd2s::core::EntityId;
using kcd2s::replication::EntityLookup;

KCD2S_TEST(entity_lookup, empty_table_finds_nothing) {
  const EntityLookup lookup;
  KCD2S_CHECK_EQ(lookup.find(1), EntityLookup::kNotFound);
  KCD2S_CHECK_EQ(lookup.find(kcd2s::core::kInvalidEntity), EntityLookup::kNotFound);
  KCD2S_CHECK_EQ(lookup.size(), std::size_t{0});
}

KCD2S_TEST(entity_lookup, insert_find_erase) {
  EntityLookup lookup;
  lookup.insert(1, 0);
  lookup.insert(2, 7);
  KCD2S_CHECK_EQ(lookup.find(1), std::uint32_t{0});
  KCD2S_CHECK_EQ(lookup.find(2), std::uint32_t{7});
  KCD2S_CHECK_EQ(lookup.find(3), EntityLookup::kNotFound);
  KCD2S_CHECK(lookup.erase(1));
  KCD2S_CHECK(!lookup.erase(1));
  KCD2S_CHECK_EQ(lookup.find(1), EntityLookup::kNotFound);
  KCD2S_CHECK_EQ(lookup.find(2), std::uint32_t{7});
  KCD2S_CHECK_EQ(lookup.size(), std::size_t{1});
}

KCD2S_TEST(entity_lookup, matches_reference_map_through_growth_and_churn) {
  // Sequential ids with random despawns, as a long server session produces them. Every id that
  // was ever inserted is checked after each phase, so backward-shift deletion can never strand an
  // entry behind a hole or leave an erased id resolvable.
  EntityLookup lookup;
  std::unordered_map<EntityId, std::uint32_t> reference;
  std::vector<EntityId> live;
  std::uint32_t state = 12345u;
  EntityId next_id = 1;
  for (int round = 0; round < 40; ++round) {
    for (int spawn = 0; spawn < 300; ++spawn) {
      const EntityId id = next_id;
      ++next_id;
      const std::uint32_t slot = static_cast<std::uint32_t>(live.size()) * 3u + 1u;
      lookup.insert(id, slot);
      reference[id] = slot;
      live.push_back(id);
    }
    for (int despawn = 0; despawn < 250 && !live.empty(); ++despawn) {
      state = state * 1664525u + 1013904223u;
      const std::size_t victim = (state >> 8) % live.size();
      const EntityId id = live[victim];
      live[victim] = live.back();
      live.pop_back();
      KCD2S_CHECK(lookup.erase(id));
      reference.erase(id);
    }
    KCD2S_CHECK_EQ(lookup.size(), reference.size());
    bool all_match = true;
    for (EntityId id = 1; id < next_id; ++id) {
      const auto expected = reference.find(id);
      const std::uint32_t found = lookup.find(id);
      if (expected == reference.end()) {
        all_match = all_match && found == EntityLookup::kNotFound;
      } else {
        all_match = all_match && found == expected->second;
      }
    }
    KCD2S_CHECK(all_match);
  }
}

KCD2S_TEST(entity_lookup, ids_near_the_top_of_the_range) {
  EntityLookup lookup;
  lookup.insert(0xFFFFFFFFu, 4);
  lookup.insert(0xFFFFFFFEu, 5);
  KCD2S_CHECK_EQ(lookup.find(0xFFFFFFFFu), std::uint32_t{4});
  KCD2S_CHECK_EQ(lookup.find(0xFFFFFFFEu), std::uint32_t{5});
  KCD2S_CHECK(lookup.erase(0xFFFFFFFFu));
  KCD2S_CHECK_EQ(lookup.find(0xFFFFFFFFu), EntityLookup::kNotFound);
  KCD2S_CHECK_EQ(lookup.find(0xFFFFFFFEu), std::uint32_t{5});
}
