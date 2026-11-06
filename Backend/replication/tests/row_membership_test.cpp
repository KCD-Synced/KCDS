#include "kcd2s/replication/row_membership.hpp"
#include "kcd2s/testkit.hpp"

#include <array>
#include <limits>

using namespace kcd2s::replication;

namespace {
SchemaRegistry membership_schema() {
  SchemaRegistry schema;
  schema.define("Member").field("value", FieldKind::I64);
  return schema;
}
}  // namespace

KCD2S_TEST(row_membership, deduplicates_live_ids_and_starts_each_viewer_empty) {
  const auto schema = membership_schema();
  World world(schema);
  const auto type = schema.get("Member")->id;
  const auto first = world.spawn(type);
  const auto second = world.spawn(type);
  RowMembership membership;
  KCD2S_CHECK(!membership.insert(first));
  KCD2S_CHECK(!membership.contains(first));
  membership.begin(world);
  KCD2S_CHECK(membership.belongs_to(world));
  KCD2S_CHECK(membership.insert(first));
  KCD2S_CHECK(!membership.insert(first));
  KCD2S_CHECK(!membership.insert(kcd2s::core::kInvalidEntity));
  KCD2S_CHECK(!membership.insert(std::numeric_limits<EntityId>::max()));
  KCD2S_CHECK(membership.contains(first));
  KCD2S_CHECK(!membership.contains(second));
  const auto retained = membership.retained_rows();
  membership.begin(world);
  KCD2S_CHECK_EQ(membership.retained_rows(), retained);
  KCD2S_CHECK(!membership.contains(first));
  KCD2S_CHECK(membership.insert(second));
  KCD2S_CHECK(!membership.contains(first));
}

KCD2S_TEST(row_membership, recycled_rows_and_other_worlds_never_inherit_membership) {
  const auto schema = membership_schema();
  World world(schema, 1000000);
  const auto type = schema.get("Member")->id;
  const auto first = world.spawn(type);
  const auto row = world.row_index(first);
  RowMembership membership;
  membership.begin(world);
  KCD2S_CHECK(membership.insert(first));
  world.despawn(first);
  const auto replacement = world.spawn(type);
  KCD2S_CHECK_EQ(world.row_index(replacement), row);
  KCD2S_CHECK(!membership.contains(first));
  KCD2S_CHECK(!membership.contains(replacement));
  membership.begin(world);
  KCD2S_CHECK(membership.insert(replacement));
  KCD2S_CHECK_EQ(membership.retained_rows(), std::size_t{1});
  World other(schema, replacement);
  KCD2S_CHECK_EQ(other.spawn(type), replacement);
  KCD2S_CHECK(!membership.belongs_to(other));
  membership.begin(other);
  KCD2S_CHECK(!membership.belongs_to(world));
  KCD2S_CHECK(!membership.contains(replacement));
  KCD2S_CHECK(membership.insert(replacement));
}

KCD2S_TEST(row_membership, generation_wrap_matches_a_fresh_reference_set_after_every_begin) {
  const auto schema = membership_schema();
  World world(schema);
  const auto type = schema.get("Member")->id;
  std::array<EntityId, 17> ids{};
  for (auto& id : ids) {
    id = world.spawn(type);
  }
  BasicRowMembership<std::uint8_t> membership;
  for (unsigned phase = 0; phase < 1024; ++phase) {
    membership.begin(world);
    for (const auto entity : ids) {
      KCD2S_CHECK(!membership.contains(entity));
    }
    for (unsigned i = 0; i < ids.size(); ++i) {
      if ((i + phase) % 3 == 0) {
        KCD2S_CHECK(membership.insert(ids[i]));
        KCD2S_CHECK(!membership.insert(ids[i]));
      }
    }
    for (unsigned i = 0; i < ids.size(); ++i) {
      KCD2S_CHECK_EQ(membership.contains(ids[i]), (i + phase) % 3 == 0);
    }
  }
  KCD2S_CHECK_EQ(membership.retained_rows(), ids.size());
}

KCD2S_TEST(row_membership, storage_follows_dense_rows_after_long_public_id_churn) {
  const auto schema = membership_schema();
  World world(schema, 1000000000);
  const auto type = schema.get("Member")->id;
  RowMembership membership;
  EntityId previous = 0;
  for (unsigned round = 0; round < 4096; ++round) {
    const auto entity = world.spawn(type);
    membership.begin(world);
    KCD2S_CHECK(membership.insert(entity));
    KCD2S_CHECK(!membership.contains(previous));
    KCD2S_CHECK_EQ(membership.retained_rows(), std::size_t{1});
    world.despawn(entity);
    previous = entity;
  }
  std::array<EntityId, 65> ids{};
  for (auto& entity : ids) {
    entity = world.spawn(type);
  }
  membership.begin(world);
  for (const auto entity : ids) {
    KCD2S_CHECK(membership.insert(entity));
  }
  KCD2S_CHECK_EQ(membership.retained_rows(), ids.size());
}
