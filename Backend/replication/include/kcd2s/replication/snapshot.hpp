// Snapshot & delta. The server builds a per-client channel-1 payload from
// the DO-world against the client's acked baseline tick; the client applies it to a local mirror.
//
// Wire layout (§5.3):
//   u32 snapTick, u32 baselineTick
//   varuint nConstruct : per { varuint entityId, u32 type, u8 epoch, <all fields> }
//   varuint nUpdate    : per { varuint entityId, u8 epoch, <fieldMask: nFields bits>, <changed> }
//   varuint nDestruct  : per { varuint entityId }
//
// Field encoding is driven by the schema (FieldKind + Quant). With Quant.bits==0 a field is raw
// (lossless); with bits>0 it is quantized (Backend/core/quant). Snapshots are unreliable by design:
// each carries its baselineTick so a drop costs one tick, not a resync.
#ifndef KCD2S_REPLICATION_SNAPSHOT_HPP
#define KCD2S_REPLICATION_SNAPSHOT_HPP

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/core/ids.hpp"
#include "kcd2s/replication/baseline_intervals.hpp"
#include "kcd2s/replication/row_membership.hpp"
#include "kcd2s/replication/schema.hpp"
#include "kcd2s/replication/world.hpp"

namespace kcd2s::replication {

struct SnapshotStats {
  std::size_t bytes = 0;
  std::size_t constructs = 0;
  std::size_t updates = 0;
  std::size_t destructs = 0;
  // Byte budget outcome (all zero/empty without a budget).
  std::size_t deferred_constructs = 0;
  std::size_t deferred_updates = 0;
  bool overdraw = false;                 // one entry was sent although it exceeded the budget
  std::vector<core::EntityId> deferred;  // entities whose CONSTRUCT or UPDATE was deferred
  // Per-build counters avoid a contended shared counter on every cache hit.
  std::size_t update_cache_hits = 0;
  std::size_t update_cache_misses = 0;
  std::size_t update_cache_fallbacks = 0;
  std::size_t update_cache_busy_fallbacks = 0;
  std::size_t update_cache_variant_fallbacks = 0;
  std::size_t update_cache_memory_fallbacks = 0;
  std::size_t update_cache_phase_fallbacks = 0;
};

// Opt-in diagnostic timing for one synchronous builder call. Values are elapsed seconds,
// including scheduling delays, not CPU time. The caller exclusively owns this output and each
// call resets it. Null BuildOptions::timings leaves every diagnostic clock call disabled.
enum class SnapshotPhase : std::size_t {
  Scope,
  Delta,
  Candidates,
  Order,
  Selection,
  Serialization,
  Count
};
struct SnapshotTimings {
  std::array<double, static_cast<std::size_t>(SnapshotPhase::Count)> seconds{};
};

// Per-client replication state the builder reads and updates (the subset §5.1 needs for a build).
struct ClientRepl {
  std::unordered_map<core::EntityId, core::Tick> entityBaselines;
  core::Tick ackedBaseline = 0;                // newest snapshot tick this client confirmed
  std::unordered_set<core::EntityId> inScope;  // entities currently constructed on the client
};

// Read-only state borrowed for one synchronous build. The caller retains ownership and may not
// mutate these containers until the build returns. Useful when confirmed scope is ACK-managed
// externally and the builder's speculative new scope would otherwise be immediately discarded.
struct SnapshotView {
  core::Tick ackedBaseline;
  const std::unordered_map<core::EntityId, core::Tick>& entityBaselines;
  const std::unordered_set<core::EntityId>& inScope;
};

// A unique, ascending sequence of live IDs and matching membership, borrowed for one build.
// Both must describe this same immutable world phase and outlive the synchronous call.
struct SnapshotAOI {
  std::span<const core::EntityId> entities;
  const RowMembership& membership;

  auto begin() const {
    return entities.begin();
  }
  auto end() const {
    return entities.end();
  }
  bool contains(core::EntityId entity) const {
    return membership.contains(entity);
  }
};

// CONSTRUCT entries shared by every client's build of one tick. An entity's CONSTRUCT (id, type,
// epoch, every field) depends only on the world, so the first build that needs it encodes it and
// the others splice the same bits. Safe for builds running in parallel over a world nobody writes
// to; reset() before each tick's builds. A build that finds an entry still being encoded by
// another thread encodes its own copy instead of waiting.
class ConstructCache {
 public:
  // Forget every entry and make room for `rows` world rows (World::row_count()).
  void reset(std::size_t rows);
  // The CONSTRUCT encoding of live entity `e`, or nullptr when another build is encoding it now.
  const core::BitWriter* encoding(const World& world, core::EntityId e);
  // Entries encoded and entries reused since the last reset (instrumentation).
  std::size_t encoded() const {
    return encoded_.load(std::memory_order_relaxed);
  }
  std::size_t reused() const {
    return reused_.load(std::memory_order_relaxed);
  }

 private:
  enum : std::uint8_t { kEmpty = 0, kEncoding = 1, kReady = 2 };
  struct Entry {
    std::atomic<std::uint8_t> state{kEmpty};
    core::BitWriter bits;
  };
  std::unique_ptr<Entry[]> entries_;
  std::size_t capacity_ = 0;
  std::size_t rows_ = 0;
  std::atomic<std::size_t> encoded_{0};
  std::atomic<std::size_t> reused_{0};
};

// UPDATE decisions and encodings shared by builds over one immutable world phase. Each row
// retains at most four baseline intervals, including the result "nothing changed". Baselines
// share a delta only when every modular field/epoch comparison is identical. Busy/full entries
// use the ordinary uncached path;
// neither cache pressure nor parallel scheduling changes snapshot bytes or budget decisions.
class UpdateCache {
 public:
  static constexpr std::size_t kVariantsPerRow = 4;
  static constexpr std::size_t kMaxRetainedBytes = 64 * 1024 * 1024;

  explicit UpdateCache(std::size_t byte_limit = kMaxRetainedBytes)
      : byte_limit_(byte_limit < kMaxRetainedBytes ? byte_limit : kMaxRetainedBytes) {
  }

  struct Delta {
    bool changed = false;
    core::BitWriter bits;
  };
  enum class Fallback { None, Busy, Variants, Memory, Phase };

  // Call after all world writes and before starting parallel builds. No reset or world mutation
  // may overlap a build; even same-tick writes require another reset. Returned pointers remain
  // valid through this build phase. Retained encoding buffers share a fixed memory ceiling.
  void reset(const World& world);
  const Delta* delta(const World& world, core::EntityId entity, core::Tick baseline,
                     bool* reused = nullptr, Fallback* fallback = nullptr);
  std::size_t retained_bytes() const {
    return retained_bytes_.load(std::memory_order_relaxed);
  }

 private:
  enum : std::uint64_t { kEmpty = 0, kEncoding = 1, kReady = 2, kUncached = 3 };
  struct Entry {
    // Interval and publication state are claimed together, so a busy interval can be recognized
    // without racing its writer or occupying all four slots with copies of the same request.
    std::atomic<std::uint64_t> key{kEmpty};
    Delta value;
  };
  struct Row {
    BaselineIntervals<core::Tick> intervals;
    std::array<Entry, kVariantsPerRow> entries;
  };
  std::unique_ptr<Row[]> entries_;
  std::size_t capacity_ = 0;
  std::size_t rows_ = 0;
  const World* world_ = nullptr;
  core::Tick tick_ = 0;
  const std::size_t byte_limit_;
  std::atomic<std::size_t> retained_bytes_{0};
};

// Optional budget/priority for a build. With byte_budget>0 the builder emits CONSTRUCT/UPDATE
// entries highest-priority-first and stops once the budget is (softly) reached; deferred CONSTRUCTs
// are NOT marked in scope, so they retry next tick (no popping — distant/idle entities just update
// less often). A deferred UPDATE loses nothing: the entity's baseline does not advance, so its next
// UPDATE carries every field changed since then. DESTRUCTs and mandatory entries are always
// emitted and count against the budget first.
struct BuildOptions {
  std::uint32_t byte_budget = 0;  // 0 = unlimited
  // Higher value = sent first. Entities absent from the map score 0. Null => all equal priority.
  const std::unordered_map<core::EntityId, float>* priority = nullptr;
  // When set, used instead of `priority`. Called only for entities with a pending CONSTRUCT or
  // UPDATE, so callers need not score every entity in view.
  std::function<float(core::EntityId)> priority_of;
  // Entities emitted regardless of the budget (for example the viewer's own avatar).
  const std::unordered_set<core::EntityId>* mandatory = nullptr;
  // If the highest-priority optional entry does not fit, emit that single entry anyway instead
  // of filling the budget with smaller, lower-priority entries. The caller authorizes this after
  // enough credit or waiting time and accounts for the overdraw; no other optional entry is sent.
  bool allow_overdraw = false;
  // CONSTRUCT entries shared with the other builds of this tick (null: encode every one here).
  ConstructCache* constructs = nullptr;
  // Baseline-equivalent UPDATE results shared with other builds of this immutable world phase.
  UpdateCache* updates = nullptr;
  // Phase measurements only; never changes inclusion, ordering or wire encoding.
  SnapshotTimings* timings = nullptr;
};

// Build a channel-1 snapshot payload for one client. `aoi` is the set of entities that should be in
// scope this tick (interest result; the caller may pass all live entities). Updates client.inScope.
// Entities in aoi but not in scope -> CONSTRUCT; in scope but not in aoi -> DESTRUCT; in both ->
// UPDATE of fields changed since client.ackedBaseline.
std::vector<std::uint8_t> build_snapshot(const World& world, core::Tick snapTick,
                                         const std::vector<core::EntityId>& aoi, ClientRepl& client,
                                         SnapshotStats* stats = nullptr,
                                         const BuildOptions* opts = nullptr,
                                         std::vector<core::EntityId>* out_constructed = nullptr,
                                         std::vector<core::EntityId>* out_updated = nullptr);

// Same encoding, budget and output-list semantics, without changing or copying the borrowed
// replication state. Reuses the caller's AOI set and does not materialize a speculative new scope.
std::vector<std::uint8_t> build_snapshot_view(
    const World& world, core::Tick snapTick, const std::unordered_set<core::EntityId>& aoi,
    const SnapshotView& client, SnapshotStats* stats = nullptr, const BuildOptions* opts = nullptr,
    std::vector<core::EntityId>* out_constructed = nullptr,
    std::vector<core::EntityId>* out_updated = nullptr);

// Same snapshot semantics using reusable row membership instead of per-viewer hash nodes.
// Entry classes start in ascending EntityId order; budgeted optional priorities prefer constructs
// on equal scores, then ascending EntityId. The other wrappers use the same ordering.
std::vector<std::uint8_t> build_snapshot_view(
    const World& world, core::Tick snapTick, const SnapshotAOI& aoi, const SnapshotView& client,
    SnapshotStats* stats = nullptr, const BuildOptions* opts = nullptr,
    std::vector<core::EntityId>* out_constructed = nullptr,
    std::vector<core::EntityId>* out_updated = nullptr);

// Client-side mirror: the applied view of the world (same schema). Read-mostly; the bridge reads
// it.
class Mirror {
 public:
  struct Entity {
    core::TypeId type = 0;
    std::uint8_t epoch = 0;
    std::vector<FieldValue> fields;
    // Changes whenever a snapshot writes this entity, so a reader can skip an unchanged one.
    std::uint64_t revision = 0;
  };

  explicit Mirror(const SchemaRegistry& schema) : schema_(schema) {
  }

  // Apply a snapshot payload. Returns false if the payload is malformed/truncated. Older and
  // duplicate snapshots are ignored successfully, retaining the newest tick for acknowledgements.
  bool apply(const std::uint8_t* data, std::size_t len);
  bool apply(const std::vector<std::uint8_t>& buf) {
    return apply(buf.data(), buf.size());
  }

  const Entity* get(core::EntityId e) const {
    auto it = entities_.find(e);
    return it == entities_.end() ? nullptr : &it->second;
  }
  const std::unordered_map<core::EntityId, Entity>& entities() const {
    return entities_;
  }
  std::size_t size() const {
    return entities_.size();
  }
  core::Tick last_applied() const {
    return last_applied_;
  }
  bool has_applied() const {
    return has_applied_;
  }
  core::Tick last_baseline() const {
    return last_baseline_;
  }

 private:
  const SchemaRegistry& schema_;
  std::unordered_map<core::EntityId, Entity> entities_;
  std::uint64_t next_revision_ = 0;
  bool has_applied_ = false;
  core::Tick last_applied_ = 0;
  core::Tick last_baseline_ = 0;
};

}  // namespace kcd2s::replication

#endif  // KCD2S_REPLICATION_SNAPSHOT_HPP
