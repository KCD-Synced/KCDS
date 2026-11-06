// Snapshot -> engine transform extraction (-11). The engine-agnostic half of the
// client bridge: turn the replicated Mirror into a flat list of transform commands the Windows glue
// then writes into WHGame CEntity objects (SetWorldTM / physics owner). Keeping this here means the
// only untested part on a non-Windows host is the raw engine call itself.
#ifndef KCD2S_BRIDGE_APPLY_HPP
#define KCD2S_BRIDGE_APPLY_HPP

#include <vector>

#include "kcd2s/core/ids.hpp"
#include "kcd2s/core/math.hpp"
#include "kcd2s/replication/schema.hpp"
#include "kcd2s/replication/snapshot.hpp"

namespace kcd2s::bridge {

struct TransformApply {
  core::EntityId entity = 0;
  core::Vec3 position;
  core::Quat rotation;
  bool has_position = false;
  bool has_rotation = false;
};

// Extract transforms for every mirror entity whose type declares a Vec3 "position" and/or Quat
// "rotation" field. Order is unspecified (mirror is a hash map); the caller applies per entity.
std::vector<TransformApply> extract_transforms(const replication::SchemaRegistry& schema,
                                               const replication::Mirror& mirror);

}  // namespace kcd2s::bridge

#endif  // KCD2S_BRIDGE_APPLY_HPP
