#include "horse_riders.hpp"

#include <limits>
#include <unordered_map>

#include "kcd2s/replication/snapshot.hpp"
#include "kcd2s/testkit.hpp"

namespace {

using Entity = kcd2s::replication::Mirror::Entity;
using Entities = std::unordered_map<kcd2s::core::EntityId, Entity>;
using kcds::runtime::replicas::snapshot_horse_riders;

Entity horse(std::int64_t rider) {
  Entity result;
  result.type = kcd2s::core::type_id("NetworkHorse");
  result.fields.resize(kcd2s::integration::HorseRider + 1);
  result.fields[kcd2s::integration::HorseRider] = rider;
  return result;
}

bool legacy_awaiting_mount(const Entities& entities, kcd2s::core::EntityId player) {
  bool awaiting = false;
  for (const auto& [id, entity] : entities) {
    if (entity.type == kcd2s::core::type_id("NetworkHorse") &&
        entity.fields.size() > kcd2s::integration::HorseRider &&
        std::get<std::int64_t>(entity.fields[kcd2s::integration::HorseRider]) ==
            std::int64_t(player)) {
      awaiting = true;
    }
  }
  return awaiting;
}

}  // namespace

KCD2S_TEST(horse_riders, preserves_signed_ids_types_short_rows_and_duplicate_riders) {
  Entities entities;
  entities[10] = horse(1);
  entities[11] = horse(1);
  entities[12] = horse(-1);
  entities[13] = horse(std::int64_t(std::numeric_limits<std::uint32_t>::max()) + 2);
  entities[14] = horse(0);
  entities[15] = horse(2);
  entities[15].type = kcd2s::core::type_id("NetworkNpc");
  entities[16] = horse(3);
  entities[16].fields.resize(kcd2s::integration::HorseRider);
  const auto riders = snapshot_horse_riders(entities);
  for (const std::uint32_t player : {0u, 1u, 2u, 3u, 0xffffffffu}) {
    KCD2S_CHECK(riders.contains(std::int64_t(player)) == legacy_awaiting_mount(entities, player));
  }
  KCD2S_CHECK(riders.contains(1));
  KCD2S_CHECK(!riders.contains(0xffffffffLL));
  KCD2S_CHECK(!riders.contains(2));
}

KCD2S_TEST(horse_riders, snapshot_mount_reassignment_and_world_replacement_leave_no_stale_rider) {
  Entities entities;
  // There is deliberately no native horse table: the server's mount arrives first.
  entities[30] = horse(4);
  KCD2S_CHECK(snapshot_horse_riders(entities).contains(4));
  entities[30].fields[kcd2s::integration::HorseRider] = std::int64_t(5);
  const auto reassigned = snapshot_horse_riders(entities);
  KCD2S_CHECK(!reassigned.contains(4));
  KCD2S_CHECK(reassigned.contains(5));
  entities.erase(30);
  KCD2S_CHECK(snapshot_horse_riders(entities).empty());
  entities[30] = horse(6);
  const auto next_world = snapshot_horse_riders(entities);
  KCD2S_CHECK(!next_world.contains(5));
  KCD2S_CHECK(next_world.contains(6));
}

KCD2S_TEST(horse_riders, thousand_player_snapshot_matches_legacy_membership) {
  Entities entities;
  for (std::uint32_t player = 1; player <= 1000; ++player) {
    entities[player].type = kcd2s::core::type_id("NetworkPlayer");
  }
  for (std::uint32_t npc = 1001; npc <= 1032; ++npc) {
    entities[npc].type = kcd2s::core::type_id("NetworkNpc");
  }
  for (std::uint32_t index = 0; index < 8; ++index) {
    entities[1033 + index] = horse(1 + index * 137);
  }
  const auto riders = snapshot_horse_riders(entities);
  KCD2S_CHECK(riders.size() == 8);
  for (std::uint32_t player = 1; player <= 1000; ++player) {
    KCD2S_CHECK(riders.contains(player) == legacy_awaiting_mount(entities, player));
  }
}
