// Server world state — data-oriented, no per-entity/per-connection diff
// objects. Entities are slots + columns; every field write that changes the encoded value stamps
// fieldChangeTick, so a delta for a client with baseline B is exactly the fields whose change tick
// > B. This single stamp means no diff state is kept per connection.
//
// Prototype storage note: field values are held as a tagged variant per (entity, field) for clarity
// at prototype scale. The interface (write/read/changed_since) is already column-shaped, so the
// backing store can be swapped for packed per-type columns later without touching callers.
#ifndef KCD2S_REPLICATION_WORLD_HPP
#define KCD2S_REPLICATION_WORLD_HPP

#include <unordered_set>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "kcd2s/core/ids.hpp"
#include "kcd2s/core/math.hpp"
#include "kcd2s/replication/entity_lookup.hpp"
#include "kcd2s/replication/schema.hpp"

namespace kcd2s::replication {

using core::EntityId;
using core::FieldId;
using core::PeerId;
using core::Quat;
using core::Tick;
using core::TypeId;
using core::Vec3;

// Canonical stored value for a replicated field.
using FieldValue = std::variant<std::monostate, float, Vec3, Quat, std::int64_t, bool, std::string>;

enum EntityFlags : std::uint8_t {
  kFlagAlive = 1 << 0,
  kFlagViewer = 1 << 1,
  kFlagAlwaysVisible = 1 << 2,
  kFlagVisible = 1 << 3,
};

inline constexpr std::uint32_t kGlobalWorld = 0xffffffffu;

struct EntitySlot {
  EntityId id = core::kInvalidEntity;  // public id of the live entity in this slot
  TypeId type = 0;
  std::uint8_t flags = 0;
  PeerId owner = 0;  // 0 = server-owned
  std::uint8_t stateEpoch = 0;
  std::uint32_t dimension = kGlobalWorld;
  PeerId visibleTo = 0;  // zero clears the per-connection restriction
  Vec3 position;         // hot: used by interest every tick
  float range = 0;       // interest radius
};

class World {
 public:
  // Entity ids start at `first_id` (default 1) and increase by one per spawn for the lifetime of
  // this world. A different first id exists only so tests can reach id exhaustion quickly.
  explicit World(const SchemaRegistry& schema, EntityId first_id = core::kFirstEntityId)
      : schema_(schema),
        next_id_(first_id == core::kInvalidEntity ? core::kFirstEntityId : first_id) {
  }

  // At most this many entities can be alive at once (dense slot storage bound).
  static constexpr std::size_t kMaxLiveEntities = 0xFFFFu;

  // Advance/sets the current tick (the value stamped by subsequent writes).
  void set_tick(Tick t) {
    tick_ = t;
  }
  Tick tick() const {
    return tick_;
  }

  // Create an entity of a registered type and give it the next sequential id. Ids are never
  // reused, so a stale id can never resolve to a newer entity. Returns kInvalidEntity if the type
  // is unknown, kMaxLiveEntities are alive, or every id up to kLastEntityId has been handed out
  // (fail closed: spawning stays refused for the rest of this world's lifetime).
  EntityId spawn(TypeId type);
  // Hand out `count` consecutive ids that only spawn_reserved() may use, so another process (the
  // resource host) can name an entity before the world creates it. Like every id they are never
  // handed out again. Returns the first id, or kInvalidEntity if not that many ids are left.
  EntityId reserve_ids(std::uint32_t count);
  // Create an entity under a reserved, still unused id. Returns kInvalidEntity if the id was not
  // reserved, is already used, the type is unknown or kMaxLiveEntities are alive.
  EntityId spawn_reserved(TypeId type, EntityId id);
  // Free an entity. Its slot is reused by a later spawn; its id is never handed out again.
  void despawn(EntityId e);
  // Id the next successful spawn will return, or kInvalidEntity once ids are exhausted.
  EntityId next_entity_id() const {
    return next_id_ > core::kLastEntityId ? core::kInvalidEntity : static_cast<EntityId>(next_id_);
  }

  bool alive(EntityId e) const;
  const EntitySlot* slot(EntityId e) const;
  EntitySlot* slot(EntityId e);

  // Write a field value. fieldChangeTick is stamped with the current tick only when the value's
  // wire encoding differs from the stored one (see same_encoding in field_codec.hpp); rewriting an
  // identical value stores it but leaves the field out of later deltas. Returns true when the
  // write was accepted, whether or not the value changed. No-op (returns false) if the entity is
  // dead or the field is out of range. Writing the type's "position" field also updates the slot's
  // hot position (interest input).
  bool write(EntityId e, FieldId field, FieldValue value);
  const FieldValue* read(EntityId e, FieldId field) const;

  Tick field_change_tick(EntityId e, FieldId field) const;
  // Fields whose change tick is strictly newer than `baseline` (the delta set).
  std::vector<FieldId> changed_since(EntityId e, Tick baseline) const;

  // Advance the entity's forced-state epoch (wraps at 256) and stamp the current tick as its epoch
  // change tick. Every UPDATE carries the epoch, but an UPDATE is only sent for an entity with a
  // changed field or a newer epoch, so an epoch fence must be stamped like a field change to reach
  // observers whose fields did not change. Returns false (no-op) for a dead entity.
  bool bump_state_epoch(EntityId e);
  // Tick of the last bump_state_epoch() (0 if never bumped or the entity is dead).
  Tick epoch_change_tick(EntityId e) const;

  std::size_t entity_count() const {
    return live_count_;
  }
  // Storage row of a live entity: below row_count() and stable until the next spawn or despawn;
  // EntityLookup::kNotFound for an entity that is not live.
  std::uint32_t row_index(EntityId e) const {
    return slot_of_.find(e);
  }
  std::size_t row_count() const {
    return rows_.size();
  }
  // All currently-live entity ids (unordered).
  std::vector<EntityId> live_entities() const;

  const SchemaRegistry& schema() const {
    return schema_;
  }

 private:
  struct Row {
    EntitySlot slot;
    std::vector<FieldValue> values;
    std::vector<Tick> change;  // fieldChangeTick, parallel to values
    Tick epoch_change = 0;     // tick of the last stateEpoch bump
  };

  // Fill a free slot with a new entity of a registered type under `id`.
  EntityId spawn_at(TypeId type, EntityId id);
  // Row of a live entity, or nullptr for an unknown, despawned or invalid id.
  Row* live_row(EntityId e);
  const Row* live_row(EntityId e) const;

  const SchemaRegistry& schema_;
  Tick tick_ = 0;
  std::vector<Row> rows_;            // dense slot storage
  std::vector<std::uint32_t> free_;  // reusable slot indices
  EntityLookup slot_of_;             // live entity id -> slot index
  std::uint64_t next_id_;            // wider than EntityId so exhaustion is representable
  std::size_t live_count_ = 0;
  std::unordered_set<EntityId> reserved_;  // reserved ids not yet used by spawn_reserved
};

}  // namespace kcd2s::replication

#endif  // KCD2S_REPLICATION_WORLD_HPP
