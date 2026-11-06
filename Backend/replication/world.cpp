#include "kcd2s/replication/world.hpp"

#include <string>

#include "kcd2s/core/seq.hpp"
#include "kcd2s/replication/field_codec.hpp"

namespace kcd2s::replication {

namespace {
// Concrete zero value per field kind, so an unwritten field still has a defined value that encodes
// and decodes identically on server and client (no monostate on the wire).
FieldValue default_value(FieldKind k) {
  switch (k) {
    case FieldKind::F32:
      return FieldValue{0.0f};
    case FieldKind::Vec3:
      return FieldValue{Vec3{}};
    case FieldKind::Quat:
      return FieldValue{Quat{}};
    case FieldKind::I64:
      return FieldValue{std::int64_t{0}};
    case FieldKind::Bool:
      return FieldValue{false};
    case FieldKind::Str:
      return FieldValue{std::string{}};
  }
  return FieldValue{};
}
}  // namespace

World::Row* World::live_row(EntityId e) {
  const std::uint32_t index = slot_of_.find(e);
  if (index == EntityLookup::kNotFound) {
    return nullptr;
  }
  return &rows_[index];
}

const World::Row* World::live_row(EntityId e) const {
  const std::uint32_t index = slot_of_.find(e);
  if (index == EntityLookup::kNotFound) {
    return nullptr;
  }
  return &rows_[index];
}

EntityId World::spawn(TypeId type) {
  if (!schema_.get(type))
    return core::kInvalidEntity;
  if (next_id_ > core::kLastEntityId)
    return core::kInvalidEntity;  // every id of this session was handed out; never wrap
  if (live_count_ >= kMaxLiveEntities)
    return core::kInvalidEntity;
  const EntityId id = static_cast<EntityId>(next_id_);
  ++next_id_;
  return spawn_at(type, id);
}

EntityId World::reserve_ids(std::uint32_t count) {
  if (count == 0 || next_id_ + count - 1 > core::kLastEntityId)
    return core::kInvalidEntity;
  const EntityId first = static_cast<EntityId>(next_id_);
  for (std::uint32_t i = 0; i < count; ++i)
    reserved_.insert(static_cast<EntityId>(first + i));
  next_id_ += count;
  return first;
}

EntityId World::spawn_reserved(TypeId type, EntityId id) {
  if (!schema_.get(type) || live_count_ >= kMaxLiveEntities)
    return core::kInvalidEntity;
  if (reserved_.erase(id) == 0)
    return core::kInvalidEntity;  // never reserved, or already used: an id names one entity only
  return spawn_at(type, id);
}

EntityId World::spawn_at(TypeId type, EntityId id) {
  const TypeDef* td = schema_.get(type);
  std::uint32_t index;
  if (!free_.empty()) {
    index = free_.back();
    free_.pop_back();
  } else {
    index = static_cast<std::uint32_t>(rows_.size());
    rows_.emplace_back();
  }

  Row& row = rows_[index];
  row.slot.id = id;
  row.slot.type = type;
  row.slot.flags = kFlagAlive | kFlagVisible;
  row.slot.owner = 0;
  row.slot.stateEpoch = 0;
  row.slot.dimension = kGlobalWorld;
  row.slot.visibleTo = 0;
  row.slot.position = Vec3{};
  row.values.clear();
  row.values.reserve(td->field_count());
  for (const FieldDef& fd : td->fields)
    row.values.push_back(default_value(fd.kind));
  row.change.assign(td->field_count(), 0);
  row.epoch_change = 0;

  slot_of_.insert(id, index);
  ++live_count_;
  return id;
}

void World::despawn(EntityId e) {
  const std::uint32_t index = slot_of_.find(e);
  if (index == EntityLookup::kNotFound)
    return;  // unknown, already despawned or invalid id

  slot_of_.erase(e);
  Row& row = rows_[index];
  row.slot.id = core::kInvalidEntity;
  row.slot.flags = 0;
  row.values.clear();
  row.change.clear();
  free_.push_back(index);
  --live_count_;
}

bool World::alive(EntityId e) const {
  return live_row(e) != nullptr;
}

const EntitySlot* World::slot(EntityId e) const {
  const Row* row = live_row(e);
  return row ? &row->slot : nullptr;
}
EntitySlot* World::slot(EntityId e) {
  Row* row = live_row(e);
  return row ? &row->slot : nullptr;
}

bool World::write(EntityId e, FieldId field, FieldValue value) {
  Row* live = live_row(e);
  if (!live)
    return false;
  Row& row = *live;
  if (field >= row.values.size())
    return false;

  const TypeDef* td = schema_.get(row.slot.type);

  // Stamp the change tick only when the client-visible encoding changes. Rewriting an identical
  // value (owner input re-sends every field) must not put the field into every delta. The new
  // value is still stored, so server-side reads see the latest write; it differs from the
  // previously sent value at most below the field's quantization step.
  bool changed = true;
  if (td && field < td->fields.size())
    changed = !same_encoding(td->fields[field], row.values[field], value);

  row.values[field] = std::move(value);
  if (changed)
    row.change[field] = tick_;

  // Keep the hot interest position in sync when the type's position field is written.
  if (td && td->position_field == field) {
    if (const Vec3* p = std::get_if<Vec3>(&row.values[field]))
      row.slot.position = *p;
  }
  return true;
}

const FieldValue* World::read(EntityId e, FieldId field) const {
  const Row* live = live_row(e);
  if (!live)
    return nullptr;
  const Row& row = *live;
  if (field >= row.values.size())
    return nullptr;
  return &row.values[field];
}

Tick World::field_change_tick(EntityId e, FieldId field) const {
  const Row* live = live_row(e);
  if (!live)
    return 0;
  const Row& row = *live;
  if (field >= row.change.size())
    return 0;
  return row.change[field];
}

std::vector<FieldId> World::changed_since(EntityId e, Tick baseline) const {
  std::vector<FieldId> out;
  const Row* live = live_row(e);
  if (!live)
    return out;
  const Row& row = *live;
  for (std::size_t f = 0; f < row.change.size(); ++f)
    if (core::seq_greater(row.change[f], baseline))
      out.push_back(static_cast<FieldId>(f));
  return out;
}

bool World::bump_state_epoch(EntityId e) {
  Row* live = live_row(e);
  if (!live)
    return false;
  Row& row = *live;
  ++row.slot.stateEpoch;
  row.epoch_change = tick_;
  return true;
}

Tick World::epoch_change_tick(EntityId e) const {
  const Row* live = live_row(e);
  if (!live)
    return 0;
  return live->epoch_change;
}

std::vector<EntityId> World::live_entities() const {
  std::vector<EntityId> out;
  out.reserve(live_count_);
  for (const Row& row : rows_)
    if (row.slot.flags & kFlagAlive)
      out.push_back(row.slot.id);
  return out;
}

}  // namespace kcd2s::replication
