#include "kcd2s/replication/snapshot.hpp"

#include <cmath>
#include <limits>
#include <random>
#include <string>
#include <unordered_set>
#include <variant>

#include "kcd2s/replication/schema.hpp"
#include "kcd2s/replication/world.hpp"
#include "kcd2s/testkit.hpp"

using namespace kcd2s::replication;

// A schema whose fields are RAW (quant.bits==0) so encode/decode is lossless -> exact parity.
static SchemaRegistry raw_schema() {
  SchemaRegistry reg;
  reg.define("Human")
      .field("position", FieldKind::Vec3, Priority::High, Authority::Owner)
      .field("health", FieldKind::F32, Priority::Med, Authority::Server)
      .field("level", FieldKind::I64, Priority::Low, Authority::Server)
      .field("alive", FieldKind::Bool, Priority::Low, Authority::Server)
      .field("name", FieldKind::Str, Priority::Low, Authority::Server);
  return reg;
}

static bool field_eq(const FieldValue& a, const FieldValue& b) {
  return a == b;
}

// Assert the mirror holds exactly the live server entities with equal field values.
static bool mirror_matches_server(const World& w, const Mirror& m,
                                  const std::vector<EntityId>& live) {
  if (m.size() != live.size())
    return false;
  for (EntityId e : live) {
    const Mirror::Entity* me = m.get(e);
    if (!me)
      return false;
    const EntitySlot* s = w.slot(e);
    if (me->type != s->type || me->epoch != s->stateEpoch)
      return false;
    const TypeDef* td = w.schema().get(s->type);
    for (const FieldDef& fd : td->fields) {
      const FieldValue* sv = w.read(e, fd.id);
      if (!field_eq(me->fields[fd.id], *sv))
        return false;
    }
  }
  return true;
}

KCD2S_TEST(snapshot, construct_then_apply_parity) {
  SchemaRegistry reg = raw_schema();
  World w(reg);
  const TypeId human = reg.get("Human")->id;

  w.set_tick(1);
  const EntityId a = w.spawn(human);
  w.write(a, 0, Vec3{1, 2, 3});
  w.write(a, 1, 0.5f);
  w.write(a, 2, std::int64_t{7});
  w.write(a, 3, true);
  w.write(a, 4, std::string("Henry"));

  Mirror mirror(reg);
  ClientRepl client;  // baseline 0, empty scope
  SnapshotStats stats;
  auto buf = build_snapshot(w, /*snapTick=*/1, {a}, client, &stats);
  KCD2S_CHECK(mirror.apply(buf));
  KCD2S_CHECK_EQ(stats.constructs, std::size_t{1});
  KCD2S_CHECK(mirror_matches_server(w, mirror, {a}));
  KCD2S_CHECK_EQ(mirror.last_applied(), kcd2s::core::Tick{1});
}

KCD2S_TEST(snapshot, late_construct_cannot_resurrect_a_removed_character) {
  SchemaRegistry schema = raw_schema();
  World world(schema);
  world.set_tick(10);
  const EntityId old_character = world.spawn(schema.get("Human")->id);
  const EntityId replacement = world.spawn(schema.get("Human")->id);
  ClientRepl client;
  Mirror mirror(schema);
  const auto construct = build_snapshot(world, 10, {old_character}, client);
  KCD2S_CHECK(mirror.apply(construct));
  client.ackedBaseline = 10;
  const auto remove = build_snapshot(world, 11, {}, client);
  KCD2S_CHECK(mirror.apply(remove));
  client.ackedBaseline = 11;
  KCD2S_CHECK(mirror.apply(build_snapshot(world, 12, {replacement}, client)));
  KCD2S_CHECK(mirror.apply(construct));
  KCD2S_CHECK(mirror.apply(remove));
  KCD2S_CHECK(mirror.get(old_character) == nullptr);
  KCD2S_CHECK(mirror.get(replacement) != nullptr);
  KCD2S_CHECK_EQ(mirror.size(), std::size_t{1});
  KCD2S_CHECK_EQ(mirror.last_applied(), kcd2s::core::Tick{12});
  KCD2S_CHECK_EQ(mirror.last_baseline(), kcd2s::core::Tick{11});
}

KCD2S_TEST(snapshot, duplicate_keeps_current_state_and_acknowledgement) {
  SchemaRegistry schema = raw_schema();
  World world(schema);
  world.set_tick(1);
  const EntityId character = world.spawn(schema.get("Human")->id);
  world.write(character, 1, 100.0f);
  ClientRepl client;
  Mirror mirror(schema);
  KCD2S_CHECK(mirror.apply(build_snapshot(world, 1, {character}, client)));
  client.ackedBaseline = 1;
  world.set_tick(2);
  world.write(character, 1, 75.0f);
  const auto update = build_snapshot(world, 2, {character}, client);
  KCD2S_CHECK(mirror.apply(update));
  KCD2S_CHECK(mirror.apply(update));
  KCD2S_CHECK_EQ(std::get<float>(mirror.get(character)->fields[1]), 75.0f);
  KCD2S_CHECK_EQ(mirror.last_applied(), kcd2s::core::Tick{2});
  KCD2S_CHECK_EQ(mirror.last_baseline(), kcd2s::core::Tick{1});
}

KCD2S_TEST(snapshot, freshness_accepts_wrap_and_rejects_late_pre_wrap_packets) {
  SchemaRegistry schema = raw_schema();
  World world(schema);
  const auto before_wrap = std::numeric_limits<kcd2s::core::Tick>::max();
  world.set_tick(before_wrap);
  const EntityId character = world.spawn(schema.get("Human")->id);
  ClientRepl client;
  Mirror mirror(schema);
  const auto construct = build_snapshot(world, before_wrap, {character}, client);
  KCD2S_CHECK(mirror.apply(construct));
  KCD2S_CHECK(mirror.apply(build_snapshot(world, 0, {}, client)));
  KCD2S_CHECK(mirror.get(character) == nullptr);
  KCD2S_CHECK(mirror.apply(construct));
  KCD2S_CHECK(mirror.get(character) == nullptr);
  KCD2S_CHECK_EQ(mirror.last_applied(), kcd2s::core::Tick{0});
  KCD2S_CHECK(mirror.apply(build_snapshot(world, 1, {character}, client)));
  KCD2S_CHECK(mirror.get(character) != nullptr);
  KCD2S_CHECK_EQ(mirror.last_applied(), kcd2s::core::Tick{1});
}

KCD2S_TEST(snapshot, shared_constructs_give_the_same_bytes) {
  SchemaRegistry reg = raw_schema();
  World w(reg);
  const TypeId human = reg.get("Human")->id;
  w.set_tick(1);
  std::vector<EntityId> crowd;
  for (int i = 0; i < 6; ++i) {
    const EntityId e = w.spawn(human);
    w.write(e, 0, Vec3{float(i), 2, 3});
    w.write(e, 4, std::string(40 + i * 13, char('a' + i)));
    crowd.push_back(e);
  }
  ConstructCache cache;
  cache.reset(w.row_count());
  BuildOptions shared;
  shared.constructs = &cache;
  BuildOptions budgeted = shared;
  budgeted.byte_budget = 160;  // some constructs deferred: sizes come from the shared entries
  bool same = true;
  for (int viewer = 0; viewer < 4; ++viewer) {
    // Every viewer sees a different part of the crowd.
    const std::vector<EntityId> aoi(crowd.begin() + viewer, crowd.end());
    for (const BuildOptions* opts : {&shared, &budgeted}) {
      BuildOptions alone = *opts;
      alone.constructs = nullptr;
      ClientRepl first;
      ClientRepl second;
      const auto with_cache = build_snapshot(w, 1, aoi, first, nullptr, opts);
      const auto without = build_snapshot(w, 1, aoi, second, nullptr, &alone);
      if (with_cache != without || first.inScope != second.inScope)
        same = false;
    }
  }
  KCD2S_CHECK(same);
  KCD2S_CHECK_EQ(cache.encoded(), std::size_t{6});  // each entity encoded once
  KCD2S_CHECK(cache.reused() > 6);
}

KCD2S_TEST(snapshot, delta_update_only_changed_fields) {
  SchemaRegistry reg = raw_schema();
  World w(reg);
  const TypeId human = reg.get("Human")->id;

  w.set_tick(1);
  const EntityId a = w.spawn(human);
  w.write(a, 0, Vec3{0, 0, 0});
  w.write(a, 1, 1.0f);

  Mirror mirror(reg);
  ClientRepl client;
  mirror.apply(build_snapshot(w, 1, {a}, client, nullptr));  // CONSTRUCT, scope now {a}

  // Simulate client acking baseline tick 1.
  client.ackedBaseline = 1;

  // Server changes only health at tick 2.
  w.set_tick(2);
  w.write(a, 1, 0.25f);

  SnapshotStats stats;
  auto buf = build_snapshot(w, 2, {a}, client, &stats);
  KCD2S_CHECK_EQ(stats.constructs, std::size_t{0});
  KCD2S_CHECK_EQ(stats.updates, std::size_t{1});
  KCD2S_CHECK(mirror.apply(buf));
  KCD2S_CHECK(mirror_matches_server(w, mirror, {a}));
  // position unchanged still matches
  KCD2S_CHECK((std::get<Vec3>(mirror.get(a)->fields[0]) == Vec3{0, 0, 0}));
  KCD2S_CHECK_EQ(std::get<float>(mirror.get(a)->fields[1]), 0.25f);
}

KCD2S_TEST(snapshot, update_not_due_is_held_and_carries_everything_later) {
  SchemaRegistry reg = raw_schema();
  World w(reg);
  const TypeId human = reg.get("Human")->id;

  w.set_tick(1);
  const EntityId near = w.spawn(human);
  const EntityId far = w.spawn(human);
  w.write(near, 0, Vec3{0, 0, 0});
  w.write(far, 0, Vec3{0, 0, 0});
  w.write(far, 1, 1.0f);

  Mirror mirror(reg);
  ClientRepl client;
  KCD2S_CHECK(mirror.apply(build_snapshot(w, 1, {near, far}, client, nullptr)));
  client.ackedBaseline = 1;

  // Both change at tick 2; only the near one is due.
  w.set_tick(2);
  w.write(near, 0, Vec3{1, 0, 0});
  w.write(far, 0, Vec3{2, 0, 0});
  BuildOptions held;
  held.update_due = [&](EntityId e) { return e != far; };
  SnapshotStats stats;
  KCD2S_CHECK(mirror.apply(build_snapshot(w, 2, {near, far}, client, &stats, &held)));
  KCD2S_CHECK_EQ(stats.updates, std::size_t{1});
  KCD2S_CHECK_EQ(stats.updates_held, std::size_t{1});
  KCD2S_CHECK((std::get<Vec3>(mirror.get(far)->fields[0]) == Vec3{0, 0, 0}));

  // The far one changes again; once due, its UPDATE carries both changes since its baseline.
  w.set_tick(3);
  w.write(far, 1, 0.5f);
  SnapshotStats due_stats;
  KCD2S_CHECK(mirror.apply(build_snapshot(w, 3, {near, far}, client, &due_stats)));
  KCD2S_CHECK_EQ(due_stats.updates_held, std::size_t{0});
  KCD2S_CHECK(mirror_matches_server(w, mirror, {near, far}));
}

KCD2S_TEST(snapshot, no_change_produces_empty_update) {
  SchemaRegistry reg = raw_schema();
  World w(reg);
  const TypeId human = reg.get("Human")->id;
  w.set_tick(1);
  const EntityId a = w.spawn(human);
  w.write(a, 1, 1.0f);

  ClientRepl client;
  (void)build_snapshot(w, 1, {a}, client, nullptr);
  client.ackedBaseline = 1;

  SnapshotStats stats;
  (void)build_snapshot(w, 2, {a}, client, &stats);  // nothing changed since tick 1
  KCD2S_CHECK_EQ(stats.updates, std::size_t{0});
  KCD2S_CHECK_EQ(stats.constructs, std::size_t{0});
}

KCD2S_TEST(snapshot, destruct_removes_from_mirror) {
  SchemaRegistry reg = raw_schema();
  World w(reg);
  const TypeId human = reg.get("Human")->id;
  w.set_tick(1);
  const EntityId a = w.spawn(human);
  w.write(a, 1, 1.0f);

  Mirror mirror(reg);
  ClientRepl client;
  mirror.apply(build_snapshot(w, 1, {a}, client, nullptr));
  KCD2S_CHECK_EQ(mirror.size(), std::size_t{1});

  // Entity leaves AOI -> DESTRUCT.
  SnapshotStats stats;
  auto buf = build_snapshot(w, 2, {}, client, &stats);
  KCD2S_CHECK_EQ(stats.destructs, std::size_t{1});
  KCD2S_CHECK(mirror.apply(buf));
  KCD2S_CHECK_EQ(mirror.size(), std::size_t{0});
}

// a respawn in a recycled slot reaches the mirror as DESTRUCT(old id) + CONSTRUCT(new
// id). The old id never resolves on the client again, and small sequential ids travel as one-byte
// varuints in the unchanged wire layout.
KCD2S_TEST(snapshot, respawn_in_recycled_slot_replaces_the_mirror_entry_under_a_new_id) {
  SchemaRegistry reg = raw_schema();
  World w(reg);
  const TypeId human = reg.get("Human")->id;
  w.set_tick(1);
  const EntityId first = w.spawn(human);
  w.write(first, 4, std::string("Henry"));

  Mirror mirror(reg);
  ClientRepl client;
  KCD2S_CHECK(mirror.apply(build_snapshot(w, 1, {first}, client, nullptr)));
  KCD2S_CHECK_EQ(first, EntityId{1});
  KCD2S_CHECK(mirror.get(first) != nullptr);

  w.set_tick(2);
  w.despawn(first);
  const EntityId second = w.spawn(human);  // same storage slot, new id
  w.write(second, 4, std::string("Hans"));
  KCD2S_CHECK_EQ(second, EntityId{2});
  SnapshotStats stats;
  KCD2S_CHECK(mirror.apply(build_snapshot(w, 2, {second}, client, &stats)));
  KCD2S_CHECK_EQ(stats.destructs, std::size_t{1});
  KCD2S_CHECK_EQ(stats.constructs, std::size_t{1});
  KCD2S_CHECK(mirror.get(first) == nullptr);
  KCD2S_CHECK(mirror_matches_server(w, mirror, {second}));
}

KCD2S_TEST(snapshot, full_32_bit_ids_roundtrip) {
  // Ids are opaque 32-bit values on the wire; the top of the range must survive the varuint path.
  SchemaRegistry reg = raw_schema();
  World w(reg, EntityId{0xFFFFFFFEu});
  const TypeId human = reg.get("Human")->id;
  w.set_tick(1);
  const EntityId high = w.spawn(human);
  const EntityId last = w.spawn(human);
  w.write(last, 1, 0.25f);
  Mirror mirror(reg);
  ClientRepl client;
  KCD2S_CHECK(mirror.apply(build_snapshot(w, 1, {high, last}, client, nullptr)));
  KCD2S_CHECK_EQ(last, kcd2s::core::kLastEntityId);
  KCD2S_CHECK(mirror_matches_server(w, mirror, {high, last}));
}

KCD2S_TEST(snapshot, byte_budget_defers_low_priority) {
  SchemaRegistry reg = raw_schema();
  World w(reg);
  const TypeId human = reg.get("Human")->id;
  w.set_tick(1);

  std::vector<EntityId> ids;
  std::unordered_map<EntityId, float> prio;
  for (int i = 0; i < 20; ++i) {
    const EntityId e = w.spawn(human);
    w.write(e, 0, Vec3{static_cast<float>(i), 0, 0});
    w.write(e, 4, std::string("npc"));
    ids.push_back(e);
    prio[e] = static_cast<float>(i);  // higher index = higher priority
  }

  Mirror mirror(reg);
  ClientRepl client;
  BuildOptions opts;
  opts.priority = &prio;

  // One full build to learn the per-entity cost, then pick a budget that admits only a few.
  ClientRepl probe;
  SnapshotStats fullstats;
  (void)build_snapshot(w, 1, ids, probe, &fullstats, nullptr);
  opts.byte_budget = static_cast<std::uint32_t>(fullstats.bytes / 4);  // ~quarter -> a few entities

  SnapshotStats s1;
  auto buf1 = build_snapshot(w, 1, ids, client, &s1, &opts);
  KCD2S_CHECK(s1.constructs > 0);
  KCD2S_CHECK(s1.constructs < 20);  // budget deferred the rest
  KCD2S_CHECK(mirror.apply(buf1));
  KCD2S_CHECK_EQ(mirror.size(), s1.constructs);
  client.ackedBaseline = 1;  // client acks -> no redundant UPDATE resends next tick
  // The highest-priority entity (index 19) must be among those sent.
  KCD2S_CHECK(mirror.get(ids[19]) != nullptr);
  // A low-priority one (index 0) should have been deferred.
  KCD2S_CHECK(mirror.get(ids[0]) == nullptr);

  // Subsequent ticks drain the backlog; all 20 eventually constructed.
  for (int t = 2; t <= 10 && mirror.size() < 20; ++t) {
    w.set_tick(static_cast<kcd2s::core::Tick>(t));
    auto buf = build_snapshot(w, static_cast<kcd2s::core::Tick>(t), ids, client, nullptr, &opts);
    KCD2S_CHECK(mirror.apply(buf));
    client.ackedBaseline = static_cast<kcd2s::core::Tick>(t);
  }
  KCD2S_CHECK_EQ(mirror.size(), std::size_t{20});
}

KCD2S_TEST(snapshot, synthetic_1k_world_full_parity) {
  SchemaRegistry reg = raw_schema();
  World w(reg);
  const TypeId human = reg.get("Human")->id;
  std::mt19937 rng(42);

  const int N = 1000;
  std::vector<EntityId> ids;
  w.set_tick(1);
  for (int i = 0; i < N; ++i) {
    const EntityId e = w.spawn(human);
    ids.push_back(e);
    std::uniform_real_distribution<float> pos(-1000, 1000);
    w.write(e, 0, Vec3{pos(rng), pos(rng), pos(rng)});
    w.write(e, 1, static_cast<float>(i) / N);
    w.write(e, 2, std::int64_t{i});
    w.write(e, 3, (i % 2) == 0);
    w.write(e, 4, std::string("npc-") + std::to_string(i));
  }

  Mirror mirror(reg);
  ClientRepl client;
  SnapshotStats full;
  mirror.apply(build_snapshot(w, 1, ids, client, &full));
  KCD2S_CHECK_EQ(full.constructs, static_cast<std::size_t>(N));
  KCD2S_CHECK(mirror_matches_server(w, mirror, ids));
  client.ackedBaseline = 1;

  // Tick 2: move 100 random entities. Only those should ride the delta.
  w.set_tick(2);
  std::uniform_int_distribution<int> pick(0, N - 1);
  std::unordered_set<int> moved;
  while (moved.size() < 100)
    moved.insert(pick(rng));
  for (int idx : moved) {
    std::uniform_real_distribution<float> pos(-1000, 1000);
    w.write(ids[idx], 0, Vec3{pos(rng), pos(rng), pos(rng)});
  }

  SnapshotStats delta;
  auto buf = build_snapshot(w, 2, ids, client, &delta);
  KCD2S_CHECK_EQ(delta.constructs, std::size_t{0});
  KCD2S_CHECK_EQ(delta.updates, std::size_t{100});  // exactly the moved ones
  KCD2S_CHECK(mirror.apply(buf));
  KCD2S_CHECK(mirror_matches_server(w, mirror, ids));

  // A delta touching 10% of entities must be far smaller than a full snapshot.
  KCD2S_CHECK(delta.bytes * 3 < full.bytes);
  std::fprintf(stderr, "    [bytes] full=%zu delta(100/1000 moved)=%zu\n", full.bytes, delta.bytes);
}

KCD2S_TEST(snapshot, identical_rewrite_produces_no_update) {
  SchemaRegistry reg = raw_schema();
  World w(reg);
  const TypeId human = reg.get("Human")->id;
  w.set_tick(1);
  const EntityId a = w.spawn(human);
  w.write(a, 0, Vec3{1, 2, 3});
  w.write(a, 4, std::string(1220, 'x'));

  ClientRepl client;
  Mirror mirror(reg);
  KCD2S_CHECK(mirror.apply(build_snapshot(w, 1, {a}, client, nullptr)));
  client.ackedBaseline = 1;

  // An owner re-sends every field with the same values on every input.
  for (kcd2s::core::Tick t = 2; t <= 5; ++t) {
    w.set_tick(t);
    w.write(a, 0, Vec3{1, 2, 3});
    w.write(a, 4, std::string(1220, 'x'));
    SnapshotStats stats;
    KCD2S_CHECK(mirror.apply(build_snapshot(w, t, {a}, client, &stats)));
    KCD2S_CHECK_EQ(stats.updates, std::size_t{0});
    KCD2S_CHECK_EQ(stats.bytes, std::size_t{11});  // header and three zero counts only
  }
  KCD2S_CHECK(mirror_matches_server(w, mirror, {a}));
}

KCD2S_TEST(snapshot, epoch_only_change_sends_empty_mask_update) {
  SchemaRegistry reg = raw_schema();
  World w(reg);
  const TypeId human = reg.get("Human")->id;
  w.set_tick(1);
  const EntityId a = w.spawn(human);
  w.write(a, 1, 1.0f);

  ClientRepl client;
  Mirror mirror(reg);
  KCD2S_CHECK(mirror.apply(build_snapshot(w, 1, {a}, client, nullptr)));
  client.ackedBaseline = 1;

  w.set_tick(2);
  KCD2S_CHECK(w.bump_state_epoch(a));
  SnapshotStats stats;
  KCD2S_CHECK(mirror.apply(build_snapshot(w, 2, {a}, client, &stats)));
  KCD2S_CHECK_EQ(stats.updates, std::size_t{1});
  KCD2S_CHECK_EQ(mirror.get(a)->epoch, std::uint8_t{1});
  KCD2S_CHECK(mirror_matches_server(w, mirror, {a}));

  // Once the client confirms tick 2, the epoch is no longer re-sent.
  client.ackedBaseline = 2;
  w.set_tick(3);
  KCD2S_CHECK(mirror.apply(build_snapshot(w, 3, {a}, client, &stats)));
  KCD2S_CHECK_EQ(stats.updates, std::size_t{0});
}

// ---- Byte budget prioritization (phase 2) ----

namespace {

std::vector<EntityId> spawn_named(World& w, int count, std::size_t name_bytes) {
  const TypeId human = w.schema().get("Human")->id;
  std::vector<EntityId> ids;
  for (int i = 0; i < count; ++i) {
    const EntityId e = w.spawn(human);
    w.write(e, 0, Vec3{static_cast<float>(i), 0, 0});
    w.write(e, 4, std::string(name_bytes, 'n'));
    ids.push_back(e);
  }
  return ids;
}

}  // namespace

KCD2S_TEST(snapshot, mandatory_entries_ignore_the_budget) {
  SchemaRegistry reg = raw_schema();
  World w(reg);
  w.set_tick(1);
  const auto ids = spawn_named(w, 10, 200);
  const std::unordered_set<EntityId> mandatory{ids[3]};
  BuildOptions opts;
  opts.byte_budget = 32;  // smaller than any single entity
  opts.mandatory = &mandatory;

  ClientRepl client;
  SnapshotStats stats;
  Mirror mirror(reg);
  KCD2S_CHECK(mirror.apply(build_snapshot(w, 1, ids, client, &stats, &opts)));
  KCD2S_CHECK_EQ(stats.constructs, std::size_t{1});
  KCD2S_CHECK(mirror.get(ids[3]) != nullptr);
  KCD2S_CHECK_EQ(stats.deferred_constructs, std::size_t{9});
  KCD2S_CHECK_EQ(stats.deferred.size(), std::size_t{9});
  KCD2S_CHECK(!stats.overdraw);
}

KCD2S_TEST(snapshot, overdraw_sends_the_top_entry_when_nothing_fits) {
  SchemaRegistry reg = raw_schema();
  World w(reg);
  w.set_tick(1);
  const auto ids = spawn_named(w, 5, 300);
  BuildOptions opts;
  opts.byte_budget = 64;
  opts.allow_overdraw = true;
  opts.priority_of = [&](EntityId e) { return e == ids[2] ? 10.0f : 0.0f; };

  ClientRepl client;
  SnapshotStats stats;
  Mirror mirror(reg);
  KCD2S_CHECK(mirror.apply(build_snapshot(w, 1, ids, client, &stats, &opts)));
  KCD2S_CHECK(stats.overdraw);
  KCD2S_CHECK_EQ(stats.constructs, std::size_t{1});
  KCD2S_CHECK(mirror.get(ids[2]) != nullptr);  // the highest priority one
  KCD2S_CHECK_EQ(stats.deferred.size(), std::size_t{4});
  bool top_not_deferred = true;
  for (EntityId e : stats.deferred) {
    if (e == ids[2])
      top_not_deferred = false;
  }
  KCD2S_CHECK(top_not_deferred);

  // Without the overdraw allowance nothing optional is sent.
  ClientRepl other;
  SnapshotStats strict;
  opts.allow_overdraw = false;
  (void)build_snapshot(w, 1, ids, other, &strict, &opts);
  KCD2S_CHECK_EQ(strict.constructs, std::size_t{0});
  KCD2S_CHECK_EQ(strict.deferred.size(), std::size_t{5});
}

KCD2S_TEST(snapshot, authorized_overdraw_rescues_large_construct_before_small_updates) {
  SchemaRegistry reg = raw_schema();
  World world(reg);
  world.set_tick(1);
  const auto small = spawn_named(world, 1, 4).front();
  ClientRepl client;
  Mirror mirror(reg);
  KCD2S_CHECK(mirror.apply(build_snapshot(world, 1, {small}, client)));
  client.ackedBaseline = 1;
  world.set_tick(2);
  const auto large = spawn_named(world, 1, 300).front();
  world.write(small, 1, 55.0f);
  BuildOptions opts;
  opts.byte_budget = 64;
  opts.priority_of = [&](EntityId entity) { return entity == large ? 10.0f : 0.0f; };
  auto strict_client = client;
  SnapshotStats strict;
  (void)build_snapshot(world, 2, {small, large}, strict_client, &strict, &opts);
  KCD2S_CHECK_EQ(strict.constructs, std::size_t{0});
  KCD2S_CHECK_EQ(strict.updates, std::size_t{1});
  KCD2S_CHECK(strict.deferred == std::vector<EntityId>{large});

  opts.allow_overdraw = true;
  SnapshotStats rescued;
  KCD2S_CHECK(mirror.apply(build_snapshot(world, 2, {small, large}, client, &rescued, &opts)));
  KCD2S_CHECK(rescued.overdraw);
  KCD2S_CHECK_EQ(rescued.constructs, std::size_t{1});
  KCD2S_CHECK_EQ(rescued.updates, std::size_t{0});
  KCD2S_CHECK(rescued.deferred == std::vector<EntityId>{small});
  KCD2S_CHECK(mirror.get(large) != nullptr);
  // The skipped small update keeps its old baseline and converges next time.
  client.entityBaselines[small] = 1;
  client.entityBaselines[large] = 2;
  world.set_tick(3);
  KCD2S_CHECK(mirror.apply(build_snapshot(world, 3, {small, large}, client, nullptr, &opts)));
  KCD2S_CHECK(mirror_matches_server(world, mirror, {small, large}));
}

KCD2S_TEST(snapshot, deferred_update_still_delivers_every_change) {
  // An UPDATE deferred by the budget does not advance the entity baseline, so a later UPDATE
  // carries all fields changed in between and the mirror converges to the server values.
  SchemaRegistry reg = raw_schema();
  World w(reg);
  w.set_tick(1);
  const auto ids = spawn_named(w, 2, 4);
  ClientRepl client;
  Mirror mirror(reg);
  KCD2S_CHECK(mirror.apply(build_snapshot(w, 1, ids, client)));
  client.ackedBaseline = 1;

  w.set_tick(2);
  w.write(ids[0], 1, 55.0f);  // health
  w.write(ids[1], 1, 66.0f);
  BuildOptions opts;
  // The header estimate is 16 bytes and each health UPDATE is 7 (1-byte id, epoch, 5-bit mask,
  // f32), so 26 bytes leave room for exactly one of the two.
  opts.byte_budget = 26;
  opts.priority_of = [&](EntityId e) { return e == ids[1] ? 1.0f : 0.0f; };
  SnapshotStats stats;
  KCD2S_CHECK(mirror.apply(build_snapshot(w, 2, ids, client, &stats, &opts)));
  KCD2S_CHECK_EQ(stats.updates, std::size_t{1});
  KCD2S_CHECK_EQ(stats.deferred_updates, std::size_t{1});
  KCD2S_CHECK(std::get<float>(mirror.get(ids[1])->fields[1]) == 66.0f);
  KCD2S_CHECK(std::get<float>(mirror.get(ids[0])->fields[1]) == 0.0f);  // still the old value
  // The client only learns entity baselines for what it received; ids[0] stays at tick 1.
  client.entityBaselines[ids[1]] = 2;
  client.entityBaselines[ids[0]] = 1;
  client.ackedBaseline = 2;

  w.set_tick(3);
  w.write(ids[0], 2, std::int64_t{7});  // level, a second change on the deferred entity
  KCD2S_CHECK(mirror.apply(build_snapshot(w, 3, ids, client)));
  KCD2S_CHECK(std::get<float>(mirror.get(ids[0])->fields[1]) == 55.0f);
  KCD2S_CHECK(std::get<std::int64_t>(mirror.get(ids[0])->fields[2]) == 7);
}

KCD2S_TEST(snapshot, held_entity_with_a_field_that_may_not_wait_is_sent) {
  SchemaRegistry reg = raw_schema();
  World w(reg);
  const TypeId human = reg.get("Human")->id;
  w.set_tick(1);
  const EntityId far = w.spawn(human);
  w.write(far, 0, Vec3{0, 0, 0});
  w.write(far, 1, 1.0f);
  Mirror mirror(reg);
  ClientRepl client;
  KCD2S_CHECK(mirror.apply(build_snapshot(w, 1, {far}, client, nullptr)));
  client.ackedBaseline = 1;

  // Not due; field 0 (position) may wait, field 1 (health) may not.
  BuildOptions held;
  held.update_due = [](EntityId) { return false; };
  held.urgent_change_tick = [&](EntityId e) { return w.field_change_tick(e, 1); };

  w.set_tick(2);
  w.write(far, 0, Vec3{3, 0, 0});
  SnapshotStats moved;
  KCD2S_CHECK(mirror.apply(build_snapshot(w, 2, {far}, client, &moved, &held)));
  KCD2S_CHECK_EQ(moved.updates_held, std::size_t{1});

  w.set_tick(3);
  w.write(far, 1, 0.0f);
  SnapshotStats died;
  KCD2S_CHECK(mirror.apply(build_snapshot(w, 3, {far}, client, &died, &held)));
  KCD2S_CHECK_EQ(died.updates_held, std::size_t{0});
  KCD2S_CHECK_EQ(died.updates, std::size_t{1});
  KCD2S_CHECK(mirror_matches_server(w, mirror, {far}));
}
