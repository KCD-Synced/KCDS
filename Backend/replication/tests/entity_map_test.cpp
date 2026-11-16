#include "kcd2s/replication/entity_map.hpp"

#include <cstddef>
#include <cstdint>
#include <random>
#include <unordered_map>

#include "kcd2s/testkit.hpp"

using kcd2s::core::EntityId;
using kcd2s::replication::EntityMap;

KCD2S_TEST(entity_map, empty_map_finds_nothing) {
  const EntityMap<std::uint32_t> map;
  KCD2S_CHECK(map.find(1) == nullptr);
  KCD2S_CHECK(map.find(kcd2s::core::kInvalidEntity) == nullptr);
  KCD2S_CHECK(map.empty());
}

KCD2S_TEST(entity_map, every_value_is_a_value_including_the_largest) {
  EntityMap<std::uint32_t> map;
  map[7] = 0xFFFFFFFFu;
  map[8] = 0;
  KCD2S_CHECK(map.find(7) && *map.find(7) == 0xFFFFFFFFu);
  KCD2S_CHECK(map.find(8) && *map.find(8) == 0);
  KCD2S_CHECK_EQ(map.size(), std::size_t{2});
}

KCD2S_TEST(entity_map, the_invalid_entity_is_a_key_beside_the_table) {
  EntityMap<std::uint32_t> map;
  map[kcd2s::core::kInvalidEntity] = 5;
  map[1] = 6;
  KCD2S_CHECK(map.find(kcd2s::core::kInvalidEntity) && *map.find(kcd2s::core::kInvalidEntity) == 5);
  KCD2S_CHECK_EQ(map.size(), std::size_t{2});
  KCD2S_CHECK(map.erase(kcd2s::core::kInvalidEntity));
  KCD2S_CHECK(map.find(kcd2s::core::kInvalidEntity) == nullptr);
  KCD2S_CHECK(!map.erase(kcd2s::core::kInvalidEntity));
  KCD2S_CHECK_EQ(map.size(), std::size_t{1});
}

KCD2S_TEST(entity_map, equal_contents_compare_equal_however_they_were_built) {
  EntityMap<std::uint32_t> forward;
  EntityMap<std::uint32_t> backward;
  for (EntityId id = 1; id <= 200; ++id) {
    forward[id] = id * 3;
  }
  for (EntityId id = 200; id >= 1; --id) {
    backward[id] = id * 3;
  }
  backward[300] = 1;
  backward.erase(300);
  KCD2S_CHECK(forward == backward);
  backward[17] = 0;
  KCD2S_CHECK(forward != backward);
}

// Random writes, overwrites, removals and lookups behave as std::unordered_map does, through
// growth and through removals inside long probe chains (ids that crowd the same homes).
KCD2S_TEST(entity_map, matches_unordered_map_under_random_operations) {
  std::mt19937_64 random(12345);
  EntityMap<std::uint32_t> map;
  std::unordered_map<EntityId, std::uint32_t> reference;
  for (int step = 0; step < 200000; ++step) {
    // Mostly a small range, so ids collide and chains form; now and then a large id.
    const EntityId id = (random() % 16 == 0) ? EntityId(random()) : EntityId(random() % 3000);
    const auto operation = random() % 10;
    if (operation < 5) {
      const auto value = std::uint32_t(random());
      map[id] = value;
      reference[id] = value;
    } else if (operation < 8) {
      KCD2S_CHECK_EQ(map.erase(id), reference.erase(id) == 1);
    } else {
      const auto found = reference.find(id);
      const std::uint32_t* value = map.find(id);
      KCD2S_CHECK_EQ(value != nullptr, found != reference.end());
      if (value && found != reference.end()) {
        KCD2S_CHECK_EQ(*value, found->second);
      }
    }
    if (step % 10000 == 0) {
      KCD2S_CHECK_EQ(map.size(), reference.size());
    }
  }
  KCD2S_CHECK_EQ(map.size(), reference.size());
  for (const auto& [id, value] : reference) {
    KCD2S_CHECK(map.find(id) && *map.find(id) == value);
  }
  map.clear();
  KCD2S_CHECK(map.empty());
  KCD2S_CHECK(map.find(1) == nullptr);
}

KCD2S_TEST(entity_map, retain_if_keeps_exactly_the_wanted_entries) {
  EntityMap<std::uint32_t> map;
  for (EntityId id = 0; id <= 500; ++id) {
    map[id] = id;
  }
  map.retain_if([](EntityId id, std::uint32_t value) { return id % 3 == 0 && value == id; });
  KCD2S_CHECK_EQ(map.size(), std::size_t{167});
  for (EntityId id = 0; id <= 500; ++id) {
    KCD2S_CHECK_EQ(map.contains(id), id % 3 == 0);
  }
  map.retain_if([](EntityId, std::uint32_t) { return false; });
  KCD2S_CHECK(map.empty());
}

KCD2S_TEST(entity_map, a_cleared_map_is_refilled_as_new) {
  EntityMap<std::uint32_t> map;
  for (int round = 0; round < 3; ++round) {
    for (EntityId id = 1; id <= 300; ++id) {
      map[id * 7] = id + EntityId(round);
    }
    KCD2S_CHECK_EQ(map.size(), std::size_t{300});
    KCD2S_CHECK(map.find(7) && *map.find(7) == 1 + std::uint32_t(round));
    map.clear();
    KCD2S_CHECK(map.empty());
    KCD2S_CHECK(map.find(7) == nullptr);
  }
}
