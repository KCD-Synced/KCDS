// Opt-in headless comparison of the exact same dense workload with and without UPDATE reuse.
// This measures snapshot construction only; it is not network or native game acceptance.
#include "kcd2s/replication/snapshot.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../../integration/local/movement.hpp"
#include "kcd2s/core/threading.hpp"

using namespace kcd2s::replication;

namespace {

constexpr std::size_t kPopulation = 1000;
constexpr unsigned kRepeats = 4;

enum class BuildMode { Legacy, Cached, Borrowed, Stamped };

struct Run {
  double milliseconds = 0;
  std::vector<std::vector<std::uint8_t>> snapshots;
  std::size_t hits = 0;
  std::size_t misses = 0;
  std::size_t fallbacks = 0;
  std::size_t busy = 0;
  std::size_t variants = 0;
  std::size_t memory = 0;
  std::size_t phase = 0;
};

Run run(const World& world, const std::vector<EntityId>& ids,
        const std::vector<ClientRepl>& initial, std::uint32_t budget,
        kcd2s::core::ThreadPool& workers, UpdateCache& cache, BuildMode mode,
        std::vector<RowMembership>& memberships, std::vector<std::vector<EntityId>>& interests) {
  Run result;
  result.snapshots.resize(initial.size());
  std::vector<SnapshotStats> stats(initial.size());
  BuildOptions opts;
  opts.byte_budget = budget;
  opts.allow_overdraw = true;
  opts.updates = mode == BuildMode::Legacy ? nullptr : &cache;
  const auto start = std::chrono::steady_clock::now();
  if (opts.updates) {
    cache.reset(world);
  }
  const auto chunks = memberships.size();
  const auto chunk_size = (initial.size() + chunks - 1) / chunks;
  workers.parallel_for(chunks, [&](std::size_t chunk) {
    for (auto viewer = chunk * chunk_size;
         viewer < std::min((chunk + 1) * chunk_size, initial.size()); ++viewer) {
      const auto& client = initial[viewer];
      const SnapshotView view{client.ackedBaseline, client.entityBaselines, client.inScope};
      if (mode == BuildMode::Stamped) {
        auto& membership = memberships[chunk];
        membership.begin(world);
        auto& ordered = interests[viewer];
        ordered.clear();
        for (const auto entity : ids) {
          if (membership.insert(entity)) {
            ordered.push_back(entity);
          }
        }
        std::sort(ordered.begin(), ordered.end());
        result.snapshots[viewer] = build_snapshot_view(
            world, world.tick(), SnapshotAOI{ordered, membership}, view, &stats[viewer], &opts);
      } else {
        // Include both current server allocations: interest_of deduplicates grid/owned/self,
        // then build_one reconstructs membership for pending-leave bookkeeping. The legacy
        // wrapper additionally copies baseline/scope maps and builds another AOI set.
        std::unordered_set<EntityId> admitted;
        std::vector<EntityId> ordered;
        for (const auto entity : ids) {
          if (admitted.insert(entity).second) {
            ordered.push_back(entity);
          }
        }
        const std::unordered_set<EntityId> interest(ordered.begin(), ordered.end());
        if (mode == BuildMode::Borrowed) {
          result.snapshots[viewer] =
              build_snapshot_view(world, world.tick(), interest, view, &stats[viewer], &opts);
        } else {
          ClientRepl temporary = client;
          result.snapshots[viewer] =
              build_snapshot(world, world.tick(), ordered, temporary, &stats[viewer], &opts);
        }
      }
    }
  });
  result.milliseconds =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  for (const auto& stat : stats) {
    result.hits += stat.update_cache_hits;
    result.misses += stat.update_cache_misses;
    result.fallbacks += stat.update_cache_fallbacks;
    result.busy += stat.update_cache_busy_fallbacks;
    result.variants += stat.update_cache_variant_fallbacks;
    result.memory += stat.update_cache_memory_fallbacks;
    result.phase += stat.update_cache_phase_fallbacks;
  }
  return result;
}

bool scenario(const std::string& name, kcd2s::core::ThreadPool& workers) {
  const auto schema = kcd2s::local::player_schema();
  World world(schema);
  const auto player = schema.get("NetworkPlayer")->id;
  std::vector<EntityId> ids;
  world.set_tick(1);
  for (std::size_t i = 0; i < kPopulation; ++i) {
    const auto entity = world.spawn(player);
    world.write(entity, kcd2s::local::Position, Vec3{float(i % 32), float(i / 32), 1});
    world.write(entity, kcd2s::local::Animations, std::string(84, 'a'));
    world.write(entity, kcd2s::local::BodyVisible, true);
    ids.push_back(entity);
  }
  std::vector<ClientRepl> clients(kPopulation);
  for (std::size_t viewer = 0; viewer < clients.size(); ++viewer) {
    auto& client = clients[viewer];
    client.ackedBaseline = static_cast<kcd2s::core::Tick>(96 + viewer % 4);
    if (name == "loss" && viewer >= 900) {
      client.ackedBaseline = static_cast<kcd2s::core::Tick>(88 + viewer % 8);
    }
    client.inScope.insert(ids.begin(), ids.end());
    for (const auto entity : ids) {
      client.entityBaselines[entity] = client.ackedBaseline;
    }
  }
  if (name != "idle") {
    world.set_tick(98);
    for (std::size_t i = 0; i < ids.size(); i += 10) {
      world.write(ids[i], kcd2s::local::Health, 75.0f);
    }
    world.set_tick(100);
    for (std::size_t i = 0; i < ids.size(); ++i) {
      world.write(ids[i], kcd2s::local::Position, Vec3{float(i % 32) + 0.1f, float(i / 32), 1});
      world.write(ids[i], kcd2s::local::MotionClock, std::int64_t{1600});
      if (name != "moving") {
        world.write(ids[i], kcd2s::local::Animations, std::string(84, 'b'));
      }
    }
  }
  world.set_tick(100);
  const std::uint32_t budget = name == "idle" || name == "moving" ? 0u : 4096u;
  UpdateCache cache;
  std::vector<RowMembership> memberships(std::min(workers.size(), clients.size()));
  std::vector<std::vector<EntityId>> interests(clients.size());
  double cached_ms = 0;
  double uncached_ms = 0;
  double borrowed_ms = 0;
  double stamped_ms = 0;
  std::size_t hits = 0, misses = 0, fallbacks = 0, bytes = 0;
  std::size_t busy = 0, variants = 0, memory = 0, phase = 0;
  for (unsigned repeat = 0; repeat < kRepeats; ++repeat) {
    // Rotate all four modes through each position. Scratch persists as it does in Server;
    // its first allocation remains inside the first stamped run's measured interval.
    Run runs[4];
    constexpr BuildMode modes[]{BuildMode::Legacy, BuildMode::Cached, BuildMode::Borrowed,
                                BuildMode::Stamped};
    for (unsigned order = 0; order < 4; ++order) {
      const auto index = (repeat + order) % 4;
      runs[index] =
          run(world, ids, clients, budget, workers, cache, modes[index], memberships, interests);
    }
    const auto& uncached = runs[0];
    const auto& cached = runs[1];
    const auto& borrowed = runs[2];
    const auto& stamped = runs[3];
    if (cached.snapshots != uncached.snapshots || borrowed.snapshots != uncached.snapshots ||
        stamped.snapshots != uncached.snapshots) {
      std::fprintf(stderr, "snapshot byte mismatch in %s\n", name.c_str());
      return false;
    }
    cached_ms += cached.milliseconds;
    uncached_ms += uncached.milliseconds;
    borrowed_ms += borrowed.milliseconds;
    stamped_ms += stamped.milliseconds;
    hits += stamped.hits;
    misses += stamped.misses;
    fallbacks += stamped.fallbacks;
    busy += stamped.busy;
    variants += stamped.variants;
    memory += stamped.memory;
    phase += stamped.phase;
    if (repeat == 0) {
      for (const auto& payload : cached.snapshots) {
        bytes += payload.size();
      }
    }
  }
  std::printf(
      "SNAPSHOT_CACHE_BENCH {\"workload\":\"%s\",\"entities\":%zu,\"viewers\":%zu,"
      "\"fields\":%zu,\"workers\":%zu,\"repeats\":%u,\"peer_budget\":%u,"
      "\"uncached_mean_ms\":%.3f,\"cached_mean_ms\":%.3f,\"speedup\":%.3f,"
      "\"borrowed_mean_ms\":%.3f,\"borrowed_speedup\":%.3f,\"server_setup_timed\":true,"
      "\"stamped_mean_ms\":%.3f,\"stamped_speedup\":%.3f,\"interest_dedup_timed\":true,"
      "\"snapshot_bytes\":%zu,\"hits_per_tick\":%zu,\"misses_per_tick\":%zu,"
      "\"fallbacks_per_tick\":%zu,\"busy_per_tick\":%zu,\"variants_per_tick\":%zu,"
      "\"memory_per_tick\":%zu,\"phase_per_tick\":%zu,\"retained_bytes\":%zu,\"byte_parity\":true}"
      "\n",
      name.c_str(), ids.size(), clients.size(), schema.get(player)->field_count(), workers.size(),
      kRepeats, budget, uncached_ms / kRepeats, cached_ms / kRepeats, uncached_ms / cached_ms,
      borrowed_ms / kRepeats, uncached_ms / borrowed_ms, stamped_ms / kRepeats,
      borrowed_ms / stamped_ms, bytes, hits / kRepeats, misses / kRepeats, fallbacks / kRepeats,
      busy / kRepeats, variants / kRepeats, memory / kRepeats, phase / kRepeats,
      cache.retained_bytes());
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  unsigned worker_count = 1;
  if (argc > 1) {
    const int requested = std::atoi(argv[1]);
    if (requested < 1 || requested > 64) {
      std::fprintf(stderr, "usage: bench_repl_snapshot [workers 1..64]\n");
      return 2;
    }
    worker_count = static_cast<unsigned>(requested);
  }
  kcd2s::core::ThreadPool workers(worker_count);
  for (const auto* name : {"idle", "moving", "animation", "loss"}) {
    if (!scenario(name, workers)) {
      return 1;
    }
  }
  return 0;
}
