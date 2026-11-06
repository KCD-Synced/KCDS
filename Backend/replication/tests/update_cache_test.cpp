#include "kcd2s/replication/snapshot.hpp"

#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <random>
#include <thread>
#include <type_traits>

#include "kcd2s/core/threading.hpp"
#include "kcd2s/testkit.hpp"

using namespace kcd2s::replication;

namespace {

SchemaRegistry cache_schema() {
  SchemaRegistry schema;
  schema.define("CacheCharacter")
      .field("visible", FieldKind::Bool)
      .field("position", FieldKind::Vec3, Priority::High, Authority::Owner, {-128, 128, 11})
      .field("health", FieldKind::F32)
      .field("state", FieldKind::Str)
      .field("action", FieldKind::I64);
  return schema;
}

std::vector<EntityId> cache_crowd(World& world, unsigned count) {
  std::vector<EntityId> ids;
  for (unsigned i = 0; i < count; ++i) {
    const auto entity = world.spawn(world.schema().get("CacheCharacter")->id);
    world.write(entity, 0, true);
    world.write(entity, 1, Vec3{float(i % 20), 2, 3});
    world.write(entity, 2, 100.0f);
    world.write(entity, 3, std::string(20 + i, 'a'));
    world.write(entity, 4, std::int64_t{i});
    ids.push_back(entity);
  }
  return ids;
}

bool same_build(const World& world, const std::vector<EntityId>& aoi, const ClientRepl& client,
                const BuildOptions& shared, SnapshotStats* observed = nullptr) {
  ClientRepl first = client;
  ClientRepl second = client;
  BuildOptions alone = shared;
  alone.updates = nullptr;
  SnapshotStats cached, uncached;
  std::vector<EntityId> first_constructs, first_updates, second_constructs, second_updates;
  const auto with = build_snapshot(world, world.tick(), aoi, first, &cached, &shared,
                                   &first_constructs, &first_updates);
  const auto without = build_snapshot(world, world.tick(), aoi, second, &uncached, &alone,
                                      &second_constructs, &second_updates);
  const std::unordered_set<EntityId> interest(aoi.begin(), aoi.end());
  const SnapshotView view{client.ackedBaseline, client.entityBaselines, client.inScope};
  SnapshotStats borrowed;
  std::vector<EntityId> borrowed_constructs, borrowed_updates;
  const auto borrowed_bytes = build_snapshot_view(world, world.tick(), interest, view, &borrowed,
                                                  &shared, &borrowed_constructs, &borrowed_updates);
  RowMembership membership;
  membership.begin(world);
  std::vector<EntityId> ordered;
  for (const auto entity : aoi) {
    if (membership.insert(entity)) {
      ordered.push_back(entity);
    }
  }
  std::sort(ordered.begin(), ordered.end());
  SnapshotStats stamped;
  std::vector<EntityId> stamped_constructs, stamped_updates;
  const auto stamped_bytes =
      build_snapshot_view(world, world.tick(), SnapshotAOI{ordered, membership}, view, &stamped,
                          &shared, &stamped_constructs, &stamped_updates);
  if (observed) {
    *observed = cached;
  }
  return with == without && with == borrowed_bytes && with == stamped_bytes &&
         first_constructs == stamped_constructs && first_updates == stamped_updates &&
         cached.deferred == stamped.deferred && cached.overdraw == stamped.overdraw &&
         cached.constructs == stamped.constructs && cached.updates == stamped.updates &&
         cached.destructs == stamped.destructs && first.inScope == second.inScope &&
         first.entityBaselines == second.entityBaselines && first_constructs == second_constructs &&
         first_constructs == borrowed_constructs && first_updates == borrowed_updates &&
         first_updates == second_updates && cached.bytes == uncached.bytes &&
         cached.constructs == uncached.constructs && cached.updates == uncached.updates &&
         cached.destructs == uncached.destructs &&
         cached.deferred_constructs == uncached.deferred_constructs &&
         cached.deferred_updates == uncached.deferred_updates &&
         cached.deferred == uncached.deferred && cached.overdraw == uncached.overdraw &&
         cached.deferred == borrowed.deferred && cached.overdraw == borrowed.overdraw &&
         cached.update_cache_fallbacks ==
             cached.update_cache_busy_fallbacks + cached.update_cache_variant_fallbacks +
                 cached.update_cache_memory_fallbacks + cached.update_cache_phase_fallbacks;
}

template <typename Tick>
std::uint64_t changed_mask(std::span<const Tick> changes, Tick baseline) {
  std::uint64_t mask = 0;
  for (std::size_t i = 0; i < changes.size(); ++i) {
    if (static_cast<std::make_signed_t<Tick>>(Tick(changes[i] - baseline)) > 0) {
      mask |= std::uint64_t{1} << i;
    }
  }
  return mask;
}

}  // namespace

KCD2S_TEST(update_cache, profiling_preserves_budgeted_scope_loss_and_cached_bytes) {
  auto schema = cache_schema();
  World world(schema);
  world.set_tick(1);
  const auto ids = cache_crowd(world, 12);
  ClientRepl base;
  const std::vector<EntityId> initial(ids.begin(), ids.begin() + 8);
  (void)build_snapshot(world, 1, initial, base);
  base.ackedBaseline = 1;
  world.set_tick(3);
  world.write(ids[0], 3, std::string(700, 'z'));
  world.write(ids[1], 2, 40.0f);
  world.bump_state_epoch(ids[2]);
  // Last two confirmed entities depart; four fresh constructs compete with changed updates.
  std::vector<EntityId> interest = ids;
  interest.erase(interest.begin() + 6, interest.begin() + 8);
  const std::unordered_set<EntityId> mandatory{ids[1]};
  ConstructCache constructs;
  UpdateCache updates;
  for (const std::uint32_t budget : {0u, 80u, 320u}) {
    for (const bool overdraw : {false, true}) {
      constructs.reset(world.row_count());
      updates.reset(world);
      BuildOptions options;
      options.byte_budget = budget;
      options.allow_overdraw = overdraw;
      options.mandatory = &mandatory;
      options.constructs = &constructs;
      options.updates = &updates;
      options.priority_of = [&](EntityId entity) { return entity == ids[0] ? 10.0f : 0.0f; };
      auto unprofiled = base;
      SnapshotStats expected;
      std::vector<EntityId> expected_constructs, expected_updates;
      const auto bytes = build_snapshot(world, 3, interest, unprofiled, &expected, &options,
                                        &expected_constructs, &expected_updates);
      constructs.reset(world.row_count());
      updates.reset(world);
      auto profiled = base;
      SnapshotStats observed;
      SnapshotTimings timings;
      timings.seconds.fill(-1.0);
      options.timings = &timings;
      std::vector<EntityId> observed_constructs, observed_updates;
      KCD2S_CHECK(bytes == build_snapshot(world, 3, interest, profiled, &observed, &options,
                                          &observed_constructs, &observed_updates));
      KCD2S_CHECK(expected_constructs == observed_constructs);
      KCD2S_CHECK(expected_updates == observed_updates);
      KCD2S_CHECK(expected.deferred == observed.deferred);
      KCD2S_CHECK_EQ(expected.overdraw, observed.overdraw);
      KCD2S_CHECK_EQ(observed.destructs, std::size_t{2});
      KCD2S_CHECK(profiled.inScope == unprofiled.inScope);
      KCD2S_CHECK(profiled.entityBaselines == unprofiled.entityBaselines);
      KCD2S_CHECK_EQ(expected.update_cache_misses, observed.update_cache_misses);
      for (const double seconds : timings.seconds) {
        KCD2S_CHECK(std::isfinite(seconds) && seconds >= 0);
      }
      if (budget == 0) {
        KCD2S_CHECK_EQ(timings.seconds[std::size_t(SnapshotPhase::Candidates)], 0.0);
        KCD2S_CHECK_EQ(timings.seconds[std::size_t(SnapshotPhase::Order)], 0.0);
      }
      // The same opt-in output also leaves borrowed/stamped wrappers and cache fallback unchanged.
      KCD2S_CHECK(same_build(world, interest, base, options));
    }
  }
}

KCD2S_TEST(update_cache, shuffled_duplicate_interest_has_canonical_budget_ties_and_departures) {
  auto schema = cache_schema();
  World world(schema);
  world.set_tick(1);
  const auto ids = cache_crowd(world, 5);
  ClientRepl client;
  Mirror mirror(schema);
  KCD2S_CHECK(mirror.apply(build_snapshot(world, 1, {ids[0], ids[3], ids[4]}, client)));
  client.ackedBaseline = 1;
  world.set_tick(2);
  world.write(ids[0], 2, 30.0f);
  UpdateCache cache;
  cache.reset(world);
  BuildOptions opts;
  opts.updates = &cache;
  opts.byte_budget = 1;
  opts.allow_overdraw = true;
  // The only UPDATE has a lower ID than either entering body. Equal scores still admit the
  // lowest-ID CONSTRUCT first, and neither AOI insertion order nor duplicate IDs may change it.
  const std::vector<EntityId> shuffled{ids[2], ids[0], ids[1], ids[2], ids[0]};
  KCD2S_CHECK(same_build(world, shuffled, client, opts));
  SnapshotStats stats;
  std::vector<EntityId> constructed, updated;
  KCD2S_CHECK(mirror.apply(
      build_snapshot(world, 2, shuffled, client, &stats, &opts, &constructed, &updated)));
  KCD2S_CHECK(constructed == std::vector<EntityId>{ids[1]});
  KCD2S_CHECK(updated.empty());
  KCD2S_CHECK(stats.deferred == std::vector<EntityId>({ids[2], ids[0]}));
  KCD2S_CHECK_EQ(stats.destructs, std::size_t{2});
  KCD2S_CHECK(mirror.get(ids[3]) == nullptr);
  KCD2S_CHECK(mirror.get(ids[4]) == nullptr);
  KCD2S_CHECK(mirror.get(ids[1]) != nullptr);
  KCD2S_CHECK(mirror.get(ids[2]) == nullptr);
  // The wrappers preserve their scope mutation contract; only actually delivered constructs join.
  KCD2S_CHECK(client.inScope == std::unordered_set<EntityId>({ids[0], ids[1]}));
}

KCD2S_TEST(update_cache, stamped_recycled_row_constructs_new_identity_and_destructs_old_identity) {
  auto schema = cache_schema();
  World world(schema);
  world.set_tick(1);
  const auto old = cache_crowd(world, 1).front();
  ClientRepl client;
  Mirror mirror(schema);
  KCD2S_CHECK(mirror.apply(build_snapshot(world, 1, {old}, client)));
  client.ackedBaseline = 1;
  RowMembership membership;
  membership.begin(world);
  KCD2S_CHECK(membership.insert(old));
  const auto row = world.row_index(old);
  world.despawn(old);
  world.set_tick(2);
  const auto replacement = cache_crowd(world, 1).front();
  KCD2S_CHECK_EQ(world.row_index(replacement), row);
  membership.begin(world);
  KCD2S_CHECK(membership.insert(replacement));
  const std::array<EntityId, 1> aoi{replacement};
  const SnapshotView view{client.ackedBaseline, client.entityBaselines, client.inScope};
  SnapshotStats stats;
  KCD2S_CHECK(
      mirror.apply(build_snapshot_view(world, 2, SnapshotAOI{aoi, membership}, view, &stats)));
  KCD2S_CHECK_EQ(stats.constructs, std::size_t{1});
  KCD2S_CHECK_EQ(stats.destructs, std::size_t{1});
  KCD2S_CHECK(mirror.get(old) == nullptr);
  KCD2S_CHECK(mirror.get(replacement) != nullptr);
  // The borrowed API never speculatively changes the caller's ACK-managed scope.
  KCD2S_CHECK(client.inScope == std::unordered_set<EntityId>{old});
}

KCD2S_TEST(update_cache, intervals_match_exhaustive_eight_bit_modular_comparisons) {
  for (unsigned seed = 0; seed < 256; ++seed) {
    const std::array<std::uint8_t, 5> changes{0, 255, std::uint8_t(seed * 17),
                                              std::uint8_t(seed * 43), std::uint8_t(seed * 71)};
    BaselineIntervals<std::uint8_t> intervals;
    intervals.reset(changes);
    std::unordered_map<std::size_t, std::uint64_t> masks;
    for (unsigned baseline = 0; baseline < 256; ++baseline) {
      const auto key = intervals.key(std::uint8_t(baseline));
      const auto mask = changed_mask<std::uint8_t>(changes, std::uint8_t(baseline));
      const auto [found, inserted] = masks.emplace(key, mask);
      KCD2S_CHECK(inserted || found->second == mask);
    }
    KCD2S_CHECK(intervals.boundary_count() <= 2 * changes.size());
  }
}

KCD2S_TEST(update_cache, intervals_preserve_32_bit_wrap_and_half_range_boundaries) {
  using Tick = kcd2s::core::Tick;
  std::mt19937 random(487);
  for (unsigned round = 0; round < 64; ++round) {
    const std::array<Tick, 9> changes{0,
                                      1,
                                      0xffffffffu,
                                      0xfffffffeu,
                                      0x80000000u,
                                      0x7fffffffu,
                                      Tick(random()),
                                      Tick(random()),
                                      Tick(random())};
    BaselineIntervals<Tick> intervals;
    intervals.reset(changes);
    std::unordered_map<std::size_t, std::uint64_t> masks;
    auto check = [&](Tick baseline) {
      const auto key = intervals.key(baseline);
      const auto mask = changed_mask<Tick>(changes, baseline);
      const auto [found, inserted] = masks.emplace(key, mask);
      KCD2S_CHECK(inserted || found->second == mask);
    };
    for (const Tick changed : changes) {
      for (const Tick boundary : {changed, Tick(changed - 0x80000000u + 1)}) {
        check(boundary - 1);
        check(boundary);
        check(boundary + 1);
      }
    }
    for (unsigned sample = 0; sample < 1000; ++sample) {
      check(random());
    }
  }
}

KCD2S_TEST(update_cache, exact_baselines_budget_overdraw_and_scope_match_uncached_bytes) {
  auto schema = cache_schema();
  World world(schema);
  world.set_tick(1);
  auto ids = cache_crowd(world, 12);
  world.set_tick(2);
  world.write(ids[0], 2, 75.0f);
  world.write(ids[5], 2, 75.0f);
  world.set_tick(3);
  world.write(ids[1], 1, Vec3{10, 11, 12});
  world.write(ids[5], 1, Vec3{10, 11, 12});
  world.set_tick(4);
  world.write(ids[2], 3, std::string(200, 'b'));
  world.write(ids[5], 3, std::string(100, 'c'));
  world.set_tick(5);
  world.bump_state_epoch(ids[3]);
  world.bump_state_epoch(ids[5]);
  world.set_tick(6);
  world.write(ids[4], 4, std::int64_t{-999});
  world.write(ids[5], 4, std::int64_t{-999});

  UpdateCache cache;
  cache.reset(world);
  const std::unordered_set<EntityId> mandatory{ids[2]};
  BuildOptions opts;
  opts.updates = &cache;
  opts.mandatory = &mandatory;
  opts.priority_of = [&](EntityId entity) { return entity == ids[3] ? 10.0f : 0.0f; };
  ClientRepl client;
  client.inScope.insert(ids.begin(), ids.end() - 1);
  const EntityId departed = ids.front();
  ids.erase(ids.begin());  // Same snapshot includes a DESTRUCT and a new CONSTRUCT.
  std::size_t fallbacks = 0;
  for (const auto budget : {0u, 1u, 64u, 1024u}) {
    opts.byte_budget = budget;
    opts.allow_overdraw = budget == 1;
    for (unsigned baseline = 0; baseline <= 7; ++baseline) {
      client.ackedBaseline = baseline;
      // This entity was deferred while other entities advanced, as on a lossy/budgeted peer.
      client.entityBaselines[ids[1]] = 1;
      SnapshotStats stats;
      KCD2S_CHECK(same_build(world, ids, client, opts, &stats));
      KCD2S_CHECK_EQ(stats.destructs, std::size_t{1});
      fallbacks += stats.update_cache_fallbacks;
      KCD2S_CHECK(client.inScope.count(departed) != 0);
    }
  }
  KCD2S_CHECK(fallbacks > 0);  // More distinct field sets than cache slots must remain correct.
}

KCD2S_TEST(update_cache, overdraw_with_fitting_lower_priority_updates_keeps_three_way_parity) {
  auto schema = cache_schema();
  World world(schema);
  world.set_tick(1);
  const auto ids = cache_crowd(world, 2);
  ClientRepl client;
  client.inScope.insert(ids[0]);
  client.ackedBaseline = 1;
  world.set_tick(2);
  world.write(ids[0], 2, 55.0f);
  world.write(ids[1], 3, std::string(300, 'n'));
  UpdateCache cache;
  cache.reset(world);
  BuildOptions opts;
  opts.updates = &cache;
  opts.byte_budget = 64;
  opts.allow_overdraw = true;
  opts.priority_of = [&](EntityId entity) { return entity == ids[1] ? 10.0f : 0.0f; };
  SnapshotStats stats;
  KCD2S_CHECK(same_build(world, ids, client, opts, &stats));
  KCD2S_CHECK(stats.overdraw);
  KCD2S_CHECK_EQ(stats.constructs, std::size_t{1});
  KCD2S_CHECK_EQ(stats.updates, std::size_t{0});
  KCD2S_CHECK(stats.deferred == std::vector<EntityId>{ids[0]});
}

KCD2S_TEST(update_cache, many_ack_ticks_share_only_equivalent_field_sets) {
  auto schema = cache_schema();
  World world(schema);
  world.set_tick(1);
  const auto ids = cache_crowd(world, 1);
  world.set_tick(200);
  world.write(ids[0], 2, 80.0f);
  world.set_tick(300);
  world.write(ids[0], 3, std::string(100, 'q'));
  world.set_tick(400);
  UpdateCache cache;
  cache.reset(world);
  BuildOptions opts;
  opts.updates = &cache;
  ClientRepl client;
  client.inScope.insert(ids[0]);
  std::size_t hits = 0, misses = 0, fallbacks = 0;
  for (unsigned baseline = 50; baseline < 400; ++baseline) {
    client.ackedBaseline = baseline;
    SnapshotStats stats;
    KCD2S_CHECK(same_build(world, ids, client, opts, &stats));
    hits += stats.update_cache_hits;
    misses += stats.update_cache_misses;
    fallbacks += stats.update_cache_fallbacks;
  }
  KCD2S_CHECK_EQ(misses, std::size_t{3});
  KCD2S_CHECK_EQ(hits, std::size_t{347});
  KCD2S_CHECK_EQ(fallbacks, std::size_t{0});
}

KCD2S_TEST(update_cache, empty_deltas_are_reused_and_epochs_still_reach_the_mirror) {
  auto schema = cache_schema();
  World world(schema);
  world.set_tick(1);
  const auto ids = cache_crowd(world, 8);
  ClientRepl client;
  Mirror mirror(schema);
  KCD2S_CHECK(mirror.apply(build_snapshot(world, 1, ids, client)));
  client.ackedBaseline = 1;
  world.set_tick(2);
  UpdateCache cache;
  cache.reset(world);
  BuildOptions opts;
  opts.updates = &cache;
  SnapshotStats first, second;
  KCD2S_CHECK(same_build(world, ids, client, opts, &first));
  KCD2S_CHECK(same_build(world, ids, client, opts, &second));
  KCD2S_CHECK_EQ(first.update_cache_misses, ids.size());
  KCD2S_CHECK_EQ(second.update_cache_hits, ids.size());
  KCD2S_CHECK_EQ(second.updates, std::size_t{0});
  KCD2S_CHECK_EQ(second.bytes, std::size_t{11});
  world.bump_state_epoch(ids[0]);
  cache.reset(world);  // Same-tick mutation also starts a new immutable build phase.
  KCD2S_CHECK(mirror.apply(build_snapshot(world, 2, ids, client, &second, &opts)));
  KCD2S_CHECK_EQ(second.updates, std::size_t{1});
  KCD2S_CHECK_EQ(second.update_cache_hits, std::size_t{0});
  KCD2S_CHECK_EQ(second.update_cache_misses, ids.size());
  KCD2S_CHECK_EQ(mirror.get(ids[0])->epoch, std::uint8_t{1});
}

KCD2S_TEST(update_cache, loss_and_deferred_fields_converge_from_each_acknowledged_baseline) {
  auto schema = cache_schema();
  World world(schema);
  world.set_tick(1);
  const auto ids = cache_crowd(world, 2);
  ClientRepl client;
  Mirror mirror(schema);
  KCD2S_CHECK(mirror.apply(build_snapshot(world, 1, ids, client)));
  client.ackedBaseline = 1;
  UpdateCache cache;
  BuildOptions opts;
  opts.updates = &cache;
  world.set_tick(2);
  world.write(ids[0], 2, 30.0f);
  cache.reset(world);
  const auto lost = build_snapshot(world, 2, ids, client, nullptr, &opts);
  KCD2S_CHECK(!lost.empty());  // Lost packet does not advance an entity baseline.
  world.set_tick(3);
  world.write(ids[0], 4, std::int64_t{7});
  world.write(ids[1], 2, 80.0f);
  cache.reset(world);
  opts.byte_budget = 30;
  opts.priority_of = [&](EntityId entity) { return entity == ids[1] ? 10.0f : 0.0f; };
  std::vector<EntityId> delivered;
  SnapshotStats stats;
  KCD2S_CHECK(
      mirror.apply(build_snapshot(world, 3, ids, client, &stats, &opts, nullptr, &delivered)));
  KCD2S_CHECK_EQ(delivered.size(), std::size_t{1});
  KCD2S_CHECK_EQ(stats.deferred_updates, std::size_t{1});
  for (auto entity : delivered) {
    client.entityBaselines[entity] = 3;
  }
  client.entityBaselines[ids[0]] = 1;
  client.ackedBaseline = 3;
  world.set_tick(4);
  cache.reset(world);
  opts.byte_budget = 0;
  KCD2S_CHECK(same_build(world, ids, client, opts));
  KCD2S_CHECK(mirror.apply(build_snapshot(world, 4, ids, client, nullptr, &opts)));
  KCD2S_CHECK_EQ(std::get<float>(mirror.get(ids[0])->fields[2]), 30.0f);
  KCD2S_CHECK_EQ(std::get<std::int64_t>(mirror.get(ids[0])->fields[4]), std::int64_t{7});
  KCD2S_CHECK_EQ(std::get<float>(mirror.get(ids[1])->fields[2]), 80.0f);
}

KCD2S_TEST(update_cache, wrap_world_mismatch_and_recycled_rows_cannot_reuse_stale_bits) {
  auto schema = cache_schema();
  World world(schema);
  const auto last = std::numeric_limits<kcd2s::core::Tick>::max();
  world.set_tick(last - 1);
  const auto ids = cache_crowd(world, 1);
  ClientRepl client;
  client.inScope.insert(ids[0]);
  client.ackedBaseline = last - 1;
  world.set_tick(last);
  world.write(ids[0], 2, 80.0f);
  UpdateCache cache;
  cache.reset(world);
  BuildOptions opts;
  opts.updates = &cache;
  KCD2S_CHECK(same_build(world, ids, client, opts));
  world.set_tick(0);
  world.write(ids[0], 4, std::int64_t{99});
  SnapshotStats stale;
  KCD2S_CHECK(same_build(world, ids, client, opts, &stale));
  KCD2S_CHECK_EQ(stale.update_cache_fallbacks, std::size_t{1});
  cache.reset(world);
  KCD2S_CHECK(same_build(world, ids, client, opts));
  client.ackedBaseline = last;
  KCD2S_CHECK(same_build(world, ids, client, opts));
  World other(schema);
  other.set_tick(0);
  const auto other_ids = cache_crowd(other, 1);
  KCD2S_CHECK(cache.delta(other, other_ids[0], last) == nullptr);
  const auto row = world.row_index(ids[0]);
  world.despawn(ids[0]);
  const auto replacement = cache_crowd(world, 1);
  KCD2S_CHECK_EQ(world.row_index(replacement[0]), row);
  world.write(replacement[0], 2, 45.0f);
  cache.reset(world);
  client.inScope.clear();
  client.inScope.insert(replacement[0]);
  KCD2S_CHECK(same_build(world, replacement, client, opts));
}

KCD2S_TEST(update_cache, concurrent_readers_and_cache_pressure_preserve_exact_results) {
  auto schema = cache_schema();
  World world(schema);
  world.set_tick(1);
  const auto ids = cache_crowd(world, 64);
  world.set_tick(9);
  for (const auto entity : ids) {
    world.write(entity, 3, std::string(512 + entity, 'z'));
  }
  UpdateCache cache(4096);
  cache.reset(world);
  BuildOptions opts;
  opts.updates = &cache;
  opts.byte_budget = 2048;
  opts.allow_overdraw = true;
  kcd2s::core::ThreadPool workers(8);
  std::atomic<unsigned> failures{0};
  std::atomic<std::size_t> fallbacks{0};
  workers.parallel_for(128, [&](std::size_t viewer) {
    ClientRepl client;
    client.ackedBaseline = static_cast<kcd2s::core::Tick>(1 + viewer % 8);
    client.inScope.insert(ids.begin(), ids.end());
    SnapshotStats stats;
    if (!same_build(world, ids, client, opts, &stats)) {
      ++failures;
    }
    fallbacks += stats.update_cache_fallbacks;
  });
  KCD2S_CHECK_EQ(failures.load(), 0u);
  KCD2S_CHECK(fallbacks.load() > 0);
  KCD2S_CHECK(cache.retained_bytes() <= 4096);
  for (unsigned phase = 10; phase < 20; ++phase) {
    world.set_tick(phase);
    world.write(ids[phase], 3, std::string(4000, 'q'));
    cache.reset(world);
    for (const auto entity : ids) {
      (void)cache.delta(world, entity, 1);
    }
    KCD2S_CHECK(cache.retained_bytes() <= 4096);
  }
}
