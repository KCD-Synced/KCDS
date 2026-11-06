#include "kcd2s/replication/world.hpp"

#include <limits>
#include <string>
#include <variant>

#include "kcd2s/replication/schema.hpp"
#include "kcd2s/testkit.hpp"

using namespace kcd2s::replication;

static SchemaRegistry human_schema() {
  SchemaRegistry reg;
  reg.define("Human")
      .field("position", FieldKind::Vec3, Priority::High, Authority::Owner)
      .field("health", FieldKind::F32, Priority::Med, Authority::Server)
      .field("name", FieldKind::Str, Priority::Low, Authority::Server);
  return reg;
}

KCD2S_TEST(world, spawn_alive_despawn) {
  SchemaRegistry reg = human_schema();
  World w(reg);
  const EntityId e = w.spawn(reg.get("Human")->id);
  KCD2S_CHECK(e != kcd2s::core::kInvalidEntity);
  KCD2S_CHECK(w.alive(e));
  KCD2S_CHECK_EQ(w.entity_count(), std::size_t{1});
  w.despawn(e);
  KCD2S_CHECK(!w.alive(e));
  KCD2S_CHECK_EQ(w.entity_count(), std::size_t{0});
}

KCD2S_TEST(world, spawn_unknown_type_fails) {
  SchemaRegistry reg = human_schema();
  World w(reg);
  KCD2S_CHECK_EQ(w.spawn(12345u), kcd2s::core::kInvalidEntity);
}

KCD2S_TEST(world, ids_are_sequential_from_one) {
  SchemaRegistry reg = human_schema();
  World w(reg);
  const TypeId human = reg.get("Human")->id;
  KCD2S_CHECK_EQ(w.next_entity_id(), EntityId{1});
  const EntityId first = w.spawn(human);
  const EntityId second = w.spawn(human);
  const EntityId third = w.spawn(human);
  KCD2S_CHECK_EQ(first, EntityId{1});
  KCD2S_CHECK_EQ(second, EntityId{2});
  KCD2S_CHECK_EQ(third, EntityId{3});
  KCD2S_CHECK_EQ(w.next_entity_id(), EntityId{4});
}

KCD2S_TEST(world, despawned_id_is_never_reused) {
  SchemaRegistry reg = human_schema();
  World w(reg);
  const TypeId human = reg.get("Human")->id;
  const EntityId first = w.spawn(human);
  const EntityId other = w.spawn(human);
  w.despawn(first);
  const EntityId respawned = w.spawn(human);  // reuses first's storage slot, never its id
  KCD2S_CHECK_EQ(respawned, EntityId{3});
  KCD2S_CHECK(!w.alive(first));  // old handle resolves to nothing
  KCD2S_CHECK(w.slot(first) == nullptr);
  KCD2S_CHECK(w.read(first, 1) == nullptr);
  KCD2S_CHECK(!w.write(first, 1, 0.25f));
  KCD2S_CHECK(!w.bump_state_epoch(first));
  KCD2S_CHECK(w.alive(respawned));
  KCD2S_CHECK(w.alive(other));
  const EntitySlot* slot = w.slot(respawned);
  KCD2S_CHECK(slot != nullptr);
  KCD2S_CHECK_EQ(slot->id, respawned);
  // Despawning the stale id again must not touch the entity now stored in its old slot.
  w.despawn(first);
  KCD2S_CHECK(w.alive(respawned));
  KCD2S_CHECK_EQ(w.entity_count(), std::size_t{2});
}

KCD2S_TEST(world, invalid_id_never_resolves) {
  SchemaRegistry reg = human_schema();
  World w(reg);
  w.spawn(reg.get("Human")->id);
  KCD2S_CHECK(!w.alive(kcd2s::core::kInvalidEntity));
  KCD2S_CHECK(w.slot(kcd2s::core::kInvalidEntity) == nullptr);
  KCD2S_CHECK(!w.alive(EntityId{2}));  // not handed out yet
}

KCD2S_TEST(world, write_stamps_change_tick) {
  SchemaRegistry reg = human_schema();
  World w(reg);
  const EntityId e = w.spawn(reg.get("Human")->id);

  w.set_tick(5);
  KCD2S_CHECK(w.write(e, 1, 0.75f));  // health
  KCD2S_CHECK_EQ(w.field_change_tick(e, 1), kcd2s::core::Tick{5});

  const FieldValue* v = w.read(e, 1);
  KCD2S_CHECK(v != nullptr);
  KCD2S_CHECK(std::holds_alternative<float>(*v));
  KCD2S_CHECK_EQ(std::get<float>(*v), 0.75f);
}

KCD2S_TEST(world, changed_since_is_the_delta_set) {
  SchemaRegistry reg = human_schema();
  World w(reg);
  const EntityId e = w.spawn(reg.get("Human")->id);

  w.set_tick(10);
  w.write(e, 0, Vec3{1, 2, 3});  // position
  w.write(e, 1, 1.0f);           // health
  w.set_tick(20);
  w.write(e, 1, 0.5f);  // health again, newer

  // Baseline 10: only fields changed after tick 10 -> just health (field 1).
  auto delta = w.changed_since(e, 10);
  KCD2S_CHECK_EQ(delta.size(), std::size_t{1});
  KCD2S_CHECK_EQ(delta[0], kcd2s::core::FieldId{1});

  // Baseline 0: everything written so far.
  auto full = w.changed_since(e, 0);
  KCD2S_CHECK_EQ(full.size(), std::size_t{2});
}

KCD2S_TEST(world, position_write_updates_slot) {
  SchemaRegistry reg = human_schema();
  World w(reg);
  const EntityId e = w.spawn(reg.get("Human")->id);
  w.write(e, 0, Vec3{10, 20, 30});  // position field
  const EntitySlot* s = w.slot(e);
  KCD2S_CHECK(s != nullptr);
  KCD2S_CHECK((s->position == Vec3{10, 20, 30}));  // hot interest copy kept in sync
}

KCD2S_TEST(world, write_dead_entity_fails) {
  SchemaRegistry reg = human_schema();
  World w(reg);
  const EntityId e = w.spawn(reg.get("Human")->id);
  w.despawn(e);
  KCD2S_CHECK(!w.write(e, 1, 1.0f));
}

KCD2S_TEST(world, live_entities_lists_all) {
  SchemaRegistry reg = human_schema();
  World w(reg);
  const TypeId human = reg.get("Human")->id;
  const EntityId a = w.spawn(human);
  const EntityId b = w.spawn(human);
  const EntityId c = w.spawn(human);
  w.despawn(b);
  auto live = w.live_entities();
  KCD2S_CHECK_EQ(live.size(), std::size_t{2});
  bool has_a = false, has_c = false;
  for (EntityId e : live) {
    if (e == a)
      has_a = true;
    if (e == c)
      has_c = true;
  }
  KCD2S_CHECK(has_a && has_c);
}

KCD2S_TEST(world, slot_churn_never_aliases_an_old_id) {
  // Replaces the former generation-retirement test: one storage slot is recycled far beyond the old
  // 16-bit generation range and no earlier id ever becomes live again.
  SchemaRegistry reg = human_schema();
  World world(reg);
  const auto type = reg.get("Human")->id;
  const EntityId first = world.spawn(type);
  EntityId previous = first;
  bool sequential = true;
  bool stale_rejected = true;
  for (unsigned round = 0; round < 0x10010u; ++round) {
    world.despawn(previous);
    const EntityId current = world.spawn(type);
    sequential = sequential && current == previous + 1;
    stale_rejected = stale_rejected && !world.alive(previous) && !world.alive(first);
    previous = current;
  }
  KCD2S_CHECK(sequential);
  KCD2S_CHECK(stale_rejected);
  KCD2S_CHECK_EQ(previous, EntityId{0x10011u});
  KCD2S_CHECK_EQ(world.entity_count(), std::size_t{1});
}

KCD2S_TEST(world, id_exhaustion_fails_closed) {
  SchemaRegistry reg = human_schema();
  World world(reg, EntityId{0xFFFFFFFEu});
  const auto type = reg.get("Human")->id;
  const EntityId second_to_last = world.spawn(type);
  const EntityId last = world.spawn(type);
  KCD2S_CHECK_EQ(second_to_last, EntityId{0xFFFFFFFEu});
  KCD2S_CHECK_EQ(last, kcd2s::core::kLastEntityId);
  KCD2S_CHECK_EQ(world.next_entity_id(), kcd2s::core::kInvalidEntity);
  KCD2S_CHECK_EQ(world.spawn(type), kcd2s::core::kInvalidEntity);
  // Freeing entities does not bring ids back: the counter never wraps to 0 or to 1.
  world.despawn(second_to_last);
  world.despawn(last);
  KCD2S_CHECK_EQ(world.spawn(type), kcd2s::core::kInvalidEntity);
  KCD2S_CHECK_EQ(world.entity_count(), std::size_t{0});
}

KCD2S_TEST(world, live_entity_cap_refuses_spawn_without_consuming_an_id) {
  SchemaRegistry reg = human_schema();
  World world(reg);
  const auto type = reg.get("Human")->id;
  for (std::size_t i = 0; i < World::kMaxLiveEntities; ++i)
    world.spawn(type);
  KCD2S_CHECK_EQ(world.entity_count(), World::kMaxLiveEntities);
  KCD2S_CHECK_EQ(world.spawn(type), kcd2s::core::kInvalidEntity);
  world.despawn(EntityId{10});
  KCD2S_CHECK_EQ(world.spawn(type), EntityId{0x10000u});
}

// phase 1: a write stamps the change tick only when the encoded value differs.

static SchemaRegistry delta_schema() {
  SchemaRegistry reg;
  reg.define("Delta")
      .field("position", FieldKind::Vec3, Priority::High, Authority::Owner)
      .field("health", FieldKind::F32, Priority::Med, Authority::Server)
      .field("animations", FieldKind::Str, Priority::High, Authority::Owner)
      .field("level", FieldKind::I64, Priority::Low, Authority::Server)
      .field("grounded", FieldKind::Bool, Priority::Low, Authority::Owner)
      .field("rotation", FieldKind::Quat, Priority::High, Authority::Owner)
      .field("coarse", FieldKind::F32, Priority::Low, Authority::Server, Quant{0.0f, 100.0f, 8});
  return reg;
}

KCD2S_TEST(world, identical_write_keeps_change_tick) {
  SchemaRegistry reg = delta_schema();
  World w(reg);
  const EntityId e = w.spawn(reg.get("Delta")->id);
  const std::string animation(1220, 'a');

  w.set_tick(5);
  w.write(e, 0, Vec3{1, 2, 3});
  w.write(e, 1, 0.5f);
  w.write(e, 2, animation);
  w.write(e, 3, std::int64_t{9});
  w.write(e, 4, true);
  w.write(e, 5, Quat{0, 0, 0.6f, 0.8f});

  w.set_tick(9);
  KCD2S_CHECK(w.write(e, 0, Vec3{1, 2, 3}));  // accepted, but not a change
  KCD2S_CHECK(w.write(e, 1, 0.5f));
  KCD2S_CHECK(w.write(e, 2, animation));
  KCD2S_CHECK(w.write(e, 3, std::int64_t{9}));
  KCD2S_CHECK(w.write(e, 4, true));
  KCD2S_CHECK(w.write(e, 5, Quat{0, 0, 0.6f, 0.8f}));
  for (FieldId f = 0; f <= 5; ++f)
    KCD2S_CHECK_EQ(w.field_change_tick(e, f), kcd2s::core::Tick{5});
  KCD2S_CHECK(w.changed_since(e, 5).empty());
}

KCD2S_TEST(world, changed_write_stamps_only_that_field) {
  SchemaRegistry reg = delta_schema();
  World w(reg);
  const EntityId e = w.spawn(reg.get("Delta")->id);
  w.set_tick(5);
  w.write(e, 1, 0.5f);
  w.write(e, 2, std::string(1220, 'a'));

  w.set_tick(6);
  w.write(e, 1, 0.5f);
  std::string animation(1220, 'a');
  animation[1219] = 'b';  // a one-byte change at the end of a large payload
  w.write(e, 2, animation);
  auto delta = w.changed_since(e, 5);
  KCD2S_CHECK_EQ(delta.size(), std::size_t{1});
  KCD2S_CHECK_EQ(delta[0], FieldId{2});
  KCD2S_CHECK(std::get<std::string>(*w.read(e, 2)) == animation);
}

KCD2S_TEST(world, raw_float_compares_bit_patterns) {
  SchemaRegistry reg = delta_schema();
  World w(reg);
  const EntityId e = w.spawn(reg.get("Delta")->id);  // health defaults to +0.0f

  w.set_tick(3);
  w.write(e, 1, -0.0f);  // equal as a float, different on the wire
  KCD2S_CHECK_EQ(w.field_change_tick(e, 1), kcd2s::core::Tick{3});

  const float nan = std::numeric_limits<float>::quiet_NaN();
  w.set_tick(4);
  w.write(e, 1, nan);
  KCD2S_CHECK_EQ(w.field_change_tick(e, 1), kcd2s::core::Tick{4});
  w.set_tick(5);
  w.write(e, 1, nan);  // same bit pattern: not a change even though NaN != NaN
  KCD2S_CHECK_EQ(w.field_change_tick(e, 1), kcd2s::core::Tick{4});
}

KCD2S_TEST(world, quantized_sub_step_write_is_not_a_change) {
  SchemaRegistry reg = delta_schema();
  World w(reg);
  const EntityId e = w.spawn(reg.get("Delta")->id);
  w.set_tick(2);
  w.write(e, 6, 50.0f);
  KCD2S_CHECK_EQ(w.field_change_tick(e, 6), kcd2s::core::Tick{2});

  // 8 bits over [0, 100]: a step is about 0.39. A 0.01 change encodes identically.
  w.set_tick(3);
  w.write(e, 6, 50.01f);
  KCD2S_CHECK_EQ(w.field_change_tick(e, 6), kcd2s::core::Tick{2});
  KCD2S_CHECK_EQ(std::get<float>(*w.read(e, 6)), 50.01f);  // latest value is still stored

  w.set_tick(4);
  w.write(e, 6, 60.0f);
  KCD2S_CHECK_EQ(w.field_change_tick(e, 6), kcd2s::core::Tick{4});
}

KCD2S_TEST(world, first_write_of_default_value_is_not_a_change) {
  SchemaRegistry reg = delta_schema();
  World w(reg);
  const EntityId e = w.spawn(reg.get("Delta")->id);
  w.set_tick(7);
  w.write(e, 3, std::int64_t{0});  // same as the spawn default
  w.write(e, 2, std::string{});
  KCD2S_CHECK_EQ(w.field_change_tick(e, 3), kcd2s::core::Tick{0});
  KCD2S_CHECK_EQ(w.field_change_tick(e, 2), kcd2s::core::Tick{0});
}

KCD2S_TEST(world, bump_state_epoch_stamps_epoch_change) {
  SchemaRegistry reg = delta_schema();
  World w(reg);
  const EntityId e = w.spawn(reg.get("Delta")->id);
  KCD2S_CHECK_EQ(w.epoch_change_tick(e), kcd2s::core::Tick{0});
  w.set_tick(11);
  KCD2S_CHECK(w.bump_state_epoch(e));
  KCD2S_CHECK_EQ(w.slot(e)->stateEpoch, std::uint8_t{1});
  KCD2S_CHECK_EQ(w.epoch_change_tick(e), kcd2s::core::Tick{11});
  KCD2S_CHECK(w.changed_since(e, 0).empty());  // no field changed
  w.despawn(e);
  KCD2S_CHECK(!w.bump_state_epoch(e));
}

KCD2S_TEST(world, reserved_ids_name_exactly_one_entity_and_are_never_reused) {
  SchemaRegistry reg = human_schema();
  World w(reg);
  const auto type = reg.get("Human")->id;
  const EntityId before = w.spawn(type);
  const EntityId first = w.reserve_ids(3);
  KCD2S_CHECK_EQ(first, EntityId(before + 1));
  // An ordinary spawn never takes a reserved id.
  const EntityId after = w.spawn(type);
  KCD2S_CHECK_EQ(after, EntityId(first + 3));
  // A reserved id spawns once, in any order; a second use, an unreserved id or a live id fail.
  KCD2S_CHECK_EQ(w.spawn_reserved(type, EntityId(first + 2)), EntityId(first + 2));
  KCD2S_CHECK(w.alive(EntityId(first + 2)));
  KCD2S_CHECK_EQ(w.spawn_reserved(type, EntityId(first + 2)), kcd2s::core::kInvalidEntity);
  KCD2S_CHECK_EQ(w.spawn_reserved(type, after), kcd2s::core::kInvalidEntity);
  KCD2S_CHECK_EQ(w.spawn_reserved(type, EntityId(after + 1)), kcd2s::core::kInvalidEntity);
  KCD2S_CHECK_EQ(w.spawn_reserved(kcd2s::replication::TypeId(999), first),
                 kcd2s::core::kInvalidEntity);
  // A despawned reserved id stays used.
  w.despawn(EntityId(first + 2));
  KCD2S_CHECK_EQ(w.spawn_reserved(type, EntityId(first + 2)), kcd2s::core::kInvalidEntity);
  KCD2S_CHECK_EQ(w.spawn_reserved(type, first), first);
  // Zero or more ids than are left reserve nothing.
  KCD2S_CHECK_EQ(w.reserve_ids(0), kcd2s::core::kInvalidEntity);
  World tight(reg, kcd2s::core::kLastEntityId - 1);
  KCD2S_CHECK_EQ(tight.reserve_ids(3), kcd2s::core::kInvalidEntity);
  KCD2S_CHECK_EQ(tight.reserve_ids(2), EntityId(kcd2s::core::kLastEntityId - 1));
  KCD2S_CHECK_EQ(tight.spawn(type), kcd2s::core::kInvalidEntity);
}
