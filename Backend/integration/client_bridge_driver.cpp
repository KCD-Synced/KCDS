#include "kcd2s/integration/client_bridge_driver.hpp"

namespace kcd2s::integration {

void ClientBridgeDriver::sync(const replication::Mirror& mirror) {
  // Current set of entities the client holds.
  std::unordered_set<core::EntityId> current;
  current.reserve(mirror.size());
  for (const auto& [id, ent] : mirror.entities()) {
    (void)ent;
    current.insert(id);
    if (!known_.count(id))
      sink_.on_bind(id);  // entered since last sync
  }
  // Entities that left the mirror.
  for (core::EntityId id : known_)
    if (!current.count(id))
      sink_.on_unbind(id);

  known_ = std::move(current);

  sink_.apply(bridge::extract_transforms(schema_, mirror));
}

}  // namespace kcd2s::integration
