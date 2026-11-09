#include "kcd2s/replication/snapshot.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <string>
#include <type_traits>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/core/seq.hpp"
#include "kcd2s/replication/field_codec.hpp"

namespace kcd2s::replication {

using core::BitReader;
using core::BitWriter;

namespace {

void encode_construct(BitWriter& w, const World& world, const SchemaRegistry& schema,
                      core::EntityId e) {
  const EntitySlot* s = world.slot(e);
  const TypeDef* td = schema.get(s->type);
  w.write_varuint(e);
  w.write_bits(s->type, 32);
  w.write_bits(s->stateEpoch, 8);
  for (const FieldDef& fd : td->fields) {
    const FieldValue* v = world.read(e, fd.id);
    encode_field(w, fd, v ? *v : FieldValue{});
  }
}

void encode_update(BitWriter& w, const World& world, const SchemaRegistry& schema, core::EntityId e,
                   const std::vector<core::FieldId>& changed) {
  const EntitySlot* s = world.slot(e);
  const TypeDef* td = schema.get(s->type);
  w.write_varuint(e);
  w.write_bits(s->stateEpoch, 8);
  std::vector<bool> mask(td->field_count(), false);
  for (core::FieldId f : changed)
    mask[f] = true;
  for (std::size_t f = 0; f < td->field_count(); ++f)
    w.write_bool(mask[f]);
  for (std::size_t f = 0; f < td->field_count(); ++f) {
    if (!mask[f])
      continue;
    const FieldValue* v = world.read(e, static_cast<core::FieldId>(f));
    encode_field(w, td->fields[f], v ? *v : FieldValue{});
  }
}

float priority_of(const BuildOptions* opts, core::EntityId e) {
  if (!opts)
    return 0.0f;
  if (opts->priority_of)
    return opts->priority_of(e);
  if (!opts->priority)
    return 0.0f;
  auto it = opts->priority->find(e);
  return it == opts->priority->end() ? 0.0f : it->second;
}

bool is_mandatory(const BuildOptions* opts, core::EntityId e) {
  return opts && opts->mandatory && opts->mandatory->count(e) != 0;
}

std::size_t varuint_bytes(std::uint64_t v) {
  std::size_t bytes = 1;
  while (v >= 0x80u) {
    v >>= 7;
    ++bytes;
  }
  return bytes;
}

// The CONSTRUCT encoding of `e`: the tick's shared one when there is a cache, otherwise (or while
// another build is encoding it) a copy appended to `own`. `own` must have room reserved so the
// pointers it hands out stay valid.
const BitWriter* construct_encoding_of(const World& world, const SchemaRegistry& schema,
                                       core::EntityId e, const BuildOptions* opts,
                                       std::vector<BitWriter>& own) {
  if (opts && opts->constructs) {
    if (const BitWriter* shared = opts->constructs->encoding(world, e))
      return shared;
  }
  own.emplace_back();
  encode_construct(own.back(), world, schema, e);
  return &own.back();
}

}  // namespace

void ConstructCache::reset(std::size_t rows) {
  if (rows > capacity_) {
    entries_ = std::make_unique<Entry[]>(rows);
    capacity_ = rows;
  } else {
    for (std::size_t i = 0; i < rows; ++i)
      entries_[i].state.store(kEmpty, std::memory_order_relaxed);
  }
  rows_ = rows;
  encoded_.store(0, std::memory_order_relaxed);
  reused_.store(0, std::memory_order_relaxed);
}

const BitWriter* ConstructCache::encoding(const World& world, core::EntityId e) {
  const std::uint32_t row = world.row_index(e);
  if (row == EntityLookup::kNotFound || row >= rows_)
    return nullptr;
  Entry& entry = entries_[row];
  if (entry.state.load(std::memory_order_acquire) == kReady) {
    reused_.fetch_add(1, std::memory_order_relaxed);
    return &entry.bits;
  }
  std::uint8_t expected = kEmpty;
  if (!entry.state.compare_exchange_strong(expected, kEncoding, std::memory_order_acquire)) {
    if (expected == kReady) {
      reused_.fetch_add(1, std::memory_order_relaxed);
      return &entry.bits;
    }
    return nullptr;  // another build is encoding it right now
  }
  entry.bits.clear();
  encode_construct(entry.bits, world, world.schema(), e);
  encoded_.fetch_add(1, std::memory_order_relaxed);
  entry.state.store(kReady, std::memory_order_release);
  return &entry.bits;
}

void UpdateCache::reset(const World& world) {
  const std::size_t rows = world.row_count();
  if (rows > capacity_) {
    entries_ = std::make_unique<Row[]>(rows);
    capacity_ = rows;
    retained_bytes_.store(0, std::memory_order_relaxed);
  } else {
    for (std::size_t row = 0; row < rows; ++row) {
      for (auto& entry : entries_[row].entries) {
        entry.key.store(kEmpty, std::memory_order_relaxed);
      }
    }
  }
  std::vector<core::Tick> changes;
  for (const auto entity : world.live_entities()) {
    const auto* slot = world.slot(entity);
    const auto* type = world.schema().get(slot->type);
    changes.clear();
    changes.reserve(type->field_count() + 1);
    for (const auto& field : type->fields) {
      changes.push_back(world.field_change_tick(entity, field.id));
    }
    changes.push_back(world.epoch_change_tick(entity));
    entries_[world.row_index(entity)].intervals.reset(changes);
  }
  rows_ = rows;
  world_ = &world;
  tick_ = world.tick();
}

const UpdateCache::Delta* UpdateCache::delta(const World& world, core::EntityId entity,
                                             core::Tick baseline, bool* reused,
                                             Fallback* fallback) {
  if (reused) {
    *reused = false;
  }
  if (fallback) {
    *fallback = Fallback::None;
  }
  if (world_ != &world || tick_ != world.tick()) {
    if (fallback) {
      *fallback = Fallback::Phase;
    }
    return nullptr;
  }
  const std::uint32_t row = world.row_index(entity);
  if (row == EntityLookup::kNotFound || row >= rows_) {
    if (fallback) {
      *fallback = Fallback::Phase;
    }
    return nullptr;
  }
  const std::uint64_t baseline_key = std::uint64_t{entries_[row].intervals.key(baseline)} << 2;
  for (auto& entry : entries_[row].entries) {
    auto key = entry.key.load(std::memory_order_acquire);
    if (key == kEmpty) {
      if (entry.key.compare_exchange_strong(key, baseline_key | kEncoding,
                                            std::memory_order_acq_rel)) {
        const auto changed = world.changed_since(entity, baseline);
        entry.value.changed =
            !changed.empty() || core::seq_greater(world.epoch_change_tick(entity), baseline);
        const std::size_t old_capacity = entry.value.bits.bytes().capacity();
        entry.value.bits.clear();
        if (entry.value.changed) {
          encode_update(entry.value.bits, world, world.schema(), entity, changed);
        }
        const std::size_t new_capacity = entry.value.bits.bytes().capacity();
        const std::size_t growth = new_capacity - old_capacity;
        const auto before = retained_bytes_.fetch_add(growth, std::memory_order_relaxed);
        if (new_capacity > byte_limit_ || before > byte_limit_ - growth) {
          entry.value.bits = BitWriter{};
          retained_bytes_.fetch_sub(new_capacity, std::memory_order_relaxed);
          entry.key.store(baseline_key | kUncached, std::memory_order_release);
          if (fallback) {
            *fallback = Fallback::Memory;
          }
          return nullptr;
        }
        entry.key.store(baseline_key | kReady, std::memory_order_release);
        return &entry.value;
      }
    }
    if ((key & ~std::uint64_t{3}) == baseline_key) {
      if ((key & 3) == kReady) {
        if (reused) {
          *reused = true;
        }
        return &entry.value;
      }
      if (fallback) {
        *fallback = (key & 3) == kUncached ? Fallback::Memory : Fallback::Busy;
      }
      return nullptr;
    }
  }
  if (fallback) {
    *fallback = Fallback::Variants;
  }
  return nullptr;
}

namespace {

template <typename AOI>
std::vector<std::uint8_t> build_snapshot_impl(const World& world, core::Tick snapTick,
                                              const AOI& aoi_set, const SnapshotView& client,
                                              SnapshotStats* stats, const BuildOptions* opts,
                                              std::vector<core::EntityId>* out_constructed,
                                              std::vector<core::EntityId>* out_updated,
                                              std::unordered_set<core::EntityId>* new_scope) {
  SnapshotTimings* timings = opts ? opts->timings : nullptr;
  std::chrono::steady_clock::time_point phase_started{};
  if (timings) {
    *timings = {};
    phase_started = std::chrono::steady_clock::now();
  }
  const auto finish_phase = [&](SnapshotPhase phase) {
    if (timings) {
      const auto now = std::chrono::steady_clock::now();
      timings->seconds[static_cast<std::size_t>(phase)] +=
          std::chrono::duration<double>(now - phase_started).count();
      phase_started = now;
    }
  };
  if (stats) {
    stats->update_cache_hits = 0;
    stats->update_cache_misses = 0;
    stats->update_cache_fallbacks = 0;
    stats->update_cache_busy_fallbacks = 0;
    stats->update_cache_variant_fallbacks = 0;
    stats->update_cache_memory_fallbacks = 0;
    stats->update_cache_phase_fallbacks = 0;
  }
  const SchemaRegistry& schema = world.schema();

  std::vector<core::EntityId> enter, stay, leave;
  for (core::EntityId e : aoi_set) {
    if (client.inScope.count(e))
      stay.push_back(e);
    else
      enter.push_back(e);
  }
  for (core::EntityId e : client.inScope)
    if (!aoi_set.contains(e))
      leave.push_back(e);

  // A stamped view already supplies canonical order. Public unordered input wrappers retain
  // their deduplication semantics and canonicalize here, independently of hash insertion order.
  if constexpr (!std::is_same_v<AOI, SnapshotAOI>) {
    std::sort(enter.begin(), enter.end());
    std::sort(stay.begin(), stay.end());
  }
  std::sort(leave.begin(), leave.end());
  finish_phase(SnapshotPhase::Scope);

  // Updates: entities in scope with changed fields or a newer forced-state epoch since the
  // client's baseline. An epoch-only change is an UPDATE with an empty field mask.
  std::vector<core::EntityId> updated;
  std::vector<std::vector<core::FieldId>> updated_fields;
  std::vector<const BitWriter*> update_encoding;
  for (core::EntityId e : stay) {
    auto baseline = client.entityBaselines.find(e);
    const core::Tick baseline_tick =
        baseline == client.entityBaselines.end() ? client.ackedBaseline : baseline->second;
    if (opts && opts->update_due && !opts->update_due(e)) {
      // Held unless something changed that may not wait.
      bool urgent = core::seq_greater(world.epoch_change_tick(e), baseline_tick);
      if (!urgent && opts->field_may_wait) {
        for (core::FieldId field : world.changed_since(e, baseline_tick)) {
          if (!opts->field_may_wait(e, field)) {
            urgent = true;
            break;
          }
        }
      }
      if (!urgent) {
        if (stats) {
          ++stats->updates_held;
        }
        continue;
      }
    }
    if (opts && opts->updates) {
      bool reused = false;
      UpdateCache::Fallback fallback;
      const auto* cached = opts->updates->delta(world, e, baseline_tick, &reused, &fallback);
      if (stats) {
        if (cached) {
          if (reused) {
            ++stats->update_cache_hits;
          } else {
            ++stats->update_cache_misses;
          }
        } else {
          ++stats->update_cache_fallbacks;
          switch (fallback) {
            case UpdateCache::Fallback::Busy:
              ++stats->update_cache_busy_fallbacks;
              break;
            case UpdateCache::Fallback::Variants:
              ++stats->update_cache_variant_fallbacks;
              break;
            case UpdateCache::Fallback::Memory:
              ++stats->update_cache_memory_fallbacks;
              break;
            case UpdateCache::Fallback::Phase:
              ++stats->update_cache_phase_fallbacks;
              break;
            case UpdateCache::Fallback::None:
              break;
          }
        }
      }
      if (cached) {
        if (cached->changed) {
          updated.push_back(e);
          updated_fields.emplace_back();
          update_encoding.push_back(&cached->bits);
        }
        continue;
      }
    }
    auto changed = world.changed_since(e, baseline_tick);
    const bool epoch_changed = core::seq_greater(world.epoch_change_tick(e), baseline_tick);
    if (changed.empty() && !epoch_changed)
      continue;
    updated.push_back(e);
    updated_fields.push_back(std::move(changed));
    update_encoding.push_back(nullptr);
  }
  finish_phase(SnapshotPhase::Delta);

  const std::uint32_t budget = opts ? opts->byte_budget : 0;

  // Decide inclusion. Without a budget, include everything. With one, order constructs+updates by
  // priority (desc) and greedily include until the running size would exceed the budget.
  std::vector<std::size_t> include_construct;  // indices into `enter`
  std::vector<std::size_t> include_update;     // indices into `updated`
  std::unordered_set<core::EntityId> constructed_now;
  // Budgeted builds keep each included entry's encoding (null: encode while writing).
  std::vector<BitWriter> encodings;
  std::vector<const BitWriter*> construct_encoding(enter.size(), nullptr);

  if (budget == 0) {
    for (std::size_t i = 0; i < enter.size(); ++i)
      include_construct.push_back(i);
    for (std::size_t i = 0; i < updated.size(); ++i)
      include_update.push_back(i);
    if (new_scope) {
      for (core::EntityId e : enter) {
        constructed_now.insert(e);
      }
    }
  } else {
    struct Cand {
      bool is_construct;
      std::size_t idx;
      float score;
      std::size_t bytes;
      const BitWriter* encoding;
    };
    // Every pending entry is encoded once to learn its size (a CONSTRUCT possibly by another
    // client's build: see ConstructCache); included entries are spliced into the snapshot from
    // these encodings instead of being encoded a second time. The reserve keeps the recorded
    // pointers valid.
    encodings.reserve(enter.size() + updated.size());
    std::vector<Cand> mandatory;
    std::vector<Cand> cands;
    cands.reserve(enter.size() + updated.size());
    for (std::size_t i = 0; i < enter.size(); ++i) {
      const BitWriter* encoding = construct_encoding_of(world, schema, enter[i], opts, encodings);
      const Cand cand{true, i, 0.0f, encoding->byte_size(), encoding};
      if (is_mandatory(opts, enter[i])) {
        mandatory.push_back(cand);
        continue;
      }
      cands.push_back(cand);
      cands.back().score = priority_of(opts, enter[i]);
    }
    for (std::size_t i = 0; i < updated.size(); ++i) {
      const BitWriter* encoding = update_encoding[i];
      if (!encoding) {
        encodings.emplace_back();
        encode_update(encodings.back(), world, schema, updated[i], updated_fields[i]);
        encoding = &encodings.back();
      }
      const Cand cand{false, i, 0.0f, encoding->byte_size(), encoding};
      if (is_mandatory(opts, updated[i])) {
        mandatory.push_back(cand);
        continue;
      }
      cands.push_back(cand);
      cands.back().score = priority_of(opts, updated[i]);
    }
    finish_phase(SnapshotPhase::Candidates);
    auto entity_of = [&](const Cand& c) { return c.is_construct ? enter[c.idx] : updated[c.idx]; };
    std::sort(cands.begin(), cands.end(), [&](const Cand& a, const Cand& b) {
      if (a.score != b.score) {
        return a.score > b.score;
      }
      if (a.is_construct != b.is_construct) {
        return a.is_construct;
      }
      return entity_of(a) < entity_of(b);
    });
    finish_phase(SnapshotPhase::Order);

    auto include = [&](const Cand& c) {
      if (c.is_construct) {
        include_construct.push_back(c.idx);
        construct_encoding[c.idx] = c.encoding;
        if (new_scope) {
          constructed_now.insert(enter[c.idx]);
        }
      } else {
        include_update.push_back(c.idx);
        update_encoding[c.idx] = c.encoding;
      }
    };

    // Header and count overhead estimate, then everything that is always sent.
    std::size_t running = 16;
    for (core::EntityId e : leave)
      running += varuint_bytes(e);
    for (const Cand& c : mandatory) {
      running += c.bytes;
      include(c);
    }
    // Once the caller authorizes overdraw, a large high-priority entry must not
    // keep losing to smaller updates that fit every tick. Send it alone among
    // optional entries, rather than adding it after already filling the budget.
    const bool overdrawn =
        !cands.empty() && opts->allow_overdraw && running + cands.front().bytes > budget;
    if (overdrawn) {
      include(cands.front());
    }
    std::vector<core::EntityId> deferred;
    for (std::size_t i = 0; i < cands.size(); ++i) {
      const Cand& c = cands[i];
      if (overdrawn) {
        if (i != 0) {
          deferred.push_back(entity_of(c));
        }
        continue;
      }
      if (running + c.bytes > budget) {
        deferred.push_back(entity_of(c));
        continue;  // skip (lower prio keeps being skipped too)
      }
      running += c.bytes;
      include(c);
    }
    if (stats) {
      stats->deferred_constructs = enter.size() - include_construct.size();
      stats->deferred_updates = updated.size() - include_update.size();
      stats->overdraw = overdrawn;
      stats->deferred = std::move(deferred);
    }
  }
  finish_phase(SnapshotPhase::Selection);

  BitWriter w;
  w.write_bits(snapTick, 32);
  w.write_bits(client.ackedBaseline, 32);

  w.write_varuint(include_construct.size());
  for (std::size_t i : include_construct) {
    const BitWriter* encoding = construct_encoding[i];
    if (!encoding && opts && opts->constructs)
      encoding = opts->constructs->encoding(world, enter[i]);
    if (!encoding || !w.append(*encoding))
      encode_construct(w, world, schema, enter[i]);
    if (out_constructed)
      out_constructed->push_back(enter[i]);  // only entities actually sent
  }

  w.write_varuint(include_update.size());
  for (std::size_t i : include_update) {
    if (!update_encoding[i] || !w.append(*update_encoding[i]))
      encode_update(w, world, schema, updated[i], updated_fields[i]);
    if (out_updated)
      out_updated->push_back(updated[i]);
  }

  w.write_varuint(leave.size());
  for (core::EntityId e : leave)
    w.write_varuint(e);

  // New scope = entities still in view and constructed: everything that stayed, plus the constructs
  // we actually emitted this tick (budget-deferred constructs are intentionally left out).
  if (new_scope) {
    for (core::EntityId e : stay) {
      new_scope->insert(e);
    }
    for (core::EntityId e : constructed_now) {
      new_scope->insert(e);
    }
  }

  if (stats) {
    stats->bytes = w.byte_size();
    stats->constructs = include_construct.size();
    stats->updates = include_update.size();
    stats->destructs = leave.size();
  }
  // The caller can time the whole function separately to expose the remaining return-buffer
  // copy and destruction of local scratch. Do not change those production paths to time them.
  finish_phase(SnapshotPhase::Serialization);
  return w.bytes();
}

}  // namespace

std::vector<std::uint8_t> build_snapshot_view(const World& world, core::Tick snapTick,
                                              const std::unordered_set<core::EntityId>& aoi,
                                              const SnapshotView& client, SnapshotStats* stats,
                                              const BuildOptions* opts,
                                              std::vector<core::EntityId>* out_constructed,
                                              std::vector<core::EntityId>* out_updated) {
  return build_snapshot_impl(world, snapTick, aoi, client, stats, opts, out_constructed,
                             out_updated, nullptr);
}

std::vector<std::uint8_t> build_snapshot_view(const World& world, core::Tick snapTick,
                                              const SnapshotAOI& aoi, const SnapshotView& client,
                                              SnapshotStats* stats, const BuildOptions* opts,
                                              std::vector<core::EntityId>* out_constructed,
                                              std::vector<core::EntityId>* out_updated) {
  assert(aoi.membership.belongs_to(world));
  assert(std::is_sorted(aoi.entities.begin(), aoi.entities.end()));
  assert(std::adjacent_find(aoi.entities.begin(), aoi.entities.end()) == aoi.entities.end());
  return build_snapshot_impl(world, snapTick, aoi, client, stats, opts, out_constructed,
                             out_updated, nullptr);
}

std::vector<std::uint8_t> build_snapshot(const World& world, core::Tick snapTick,
                                         const std::vector<core::EntityId>& aoi, ClientRepl& client,
                                         SnapshotStats* stats, const BuildOptions* opts,
                                         std::vector<core::EntityId>* out_constructed,
                                         std::vector<core::EntityId>* out_updated) {
  const std::unordered_set<core::EntityId> aoi_set(aoi.begin(), aoi.end());
  const SnapshotView view{client.ackedBaseline, client.entityBaselines, client.inScope};
  std::unordered_set<core::EntityId> new_scope;
  auto payload = build_snapshot_impl(world, snapTick, aoi_set, view, stats, opts, out_constructed,
                                     out_updated, &new_scope);
  client.inScope = std::move(new_scope);
  return payload;
}

bool Mirror::apply(const std::uint8_t* data, std::size_t len) {
  BitReader r(data, len);
  const core::Tick snapTick = static_cast<core::Tick>(r.read_bits(32));
  const core::Tick baselineTick = static_cast<core::Tick>(r.read_bits(32));
  if (r.overrun())
    return false;
  // Snapshots are carried on an unordered channel. After a newer DESTRUCT was applied, a late
  // CONSTRUCT must not resurrect its body and consume a slot the server has already released.
  // Keep the latest acknowledgement on duplicates too; compare across the 32-bit tick wrap.
  if (has_applied_ && !core::seq_greater(snapTick, last_applied_)) {
    return true;
  }

  const std::uint64_t nConstruct = r.read_varuint();
  for (std::uint64_t i = 0; i < nConstruct; ++i) {
    const core::EntityId e = static_cast<core::EntityId>(r.read_varuint());
    const core::TypeId type = static_cast<core::TypeId>(r.read_bits(32));
    const std::uint8_t epoch = static_cast<std::uint8_t>(r.read_bits(8));
    if (r.overrun())
      return false;
    const TypeDef* td = schema_.get(type);
    if (!td)
      return false;
    Entity ent;
    ent.type = type;
    ent.epoch = epoch;
    ent.fields.resize(td->field_count());
    for (const FieldDef& fd : td->fields)
      ent.fields[fd.id] = decode_field(r, fd);
    if (r.overrun())
      return false;
    ent.revision = ++next_revision_;
    entities_[e] = std::move(ent);
  }

  const std::uint64_t nUpdate = r.read_varuint();
  for (std::uint64_t i = 0; i < nUpdate; ++i) {
    const core::EntityId e = static_cast<core::EntityId>(r.read_varuint());
    const std::uint8_t epoch = static_cast<std::uint8_t>(r.read_bits(8));
    if (r.overrun())
      return false;
    auto it = entities_.find(e);
    if (it == entities_.end())
      return false;  // update before construct: malformed for this client
    Entity& ent = it->second;
    const TypeDef* td = schema_.get(ent.type);
    if (!td)
      return false;
    ent.epoch = epoch;
    std::vector<bool> mask(td->field_count(), false);
    for (std::size_t f = 0; f < td->field_count(); ++f)
      mask[f] = r.read_bool();
    if (r.overrun())
      return false;
    for (std::size_t f = 0; f < td->field_count(); ++f) {
      if (!mask[f])
        continue;
      ent.fields[f] = decode_field(r, td->fields[f]);
      ent.revision = ++next_revision_;
    }
    if (r.overrun())
      return false;
  }

  const std::uint64_t nDestruct = r.read_varuint();
  for (std::uint64_t i = 0; i < nDestruct; ++i) {
    const core::EntityId e = static_cast<core::EntityId>(r.read_varuint());
    entities_.erase(e);
  }
  if (r.overrun())
    return false;

  last_applied_ = snapTick;
  last_baseline_ = baselineTick;
  has_applied_ = true;
  return true;
}

}  // namespace kcd2s::replication
