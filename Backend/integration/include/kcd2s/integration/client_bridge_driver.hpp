// Client bridge driver (-11): the headless glue between the replicated Mirror and
// the engine. Each tick it diffs the mirror to emit entity bind/unbind (CONSTRUCT/DESTRUCT) and
// feeds the current transforms to a sink. On Windows the sink forwards to EngineBridge
// (bind_entity/unbind_entity + apply); in tests a spy sink records the calls. This keeps the entire
// client data path — snapshot -> mirror -> bind/unbind + transforms — verifiable without the
// engine.
#ifndef KCD2S_INTEGRATION_CLIENT_BRIDGE_DRIVER_HPP
#define KCD2S_INTEGRATION_CLIENT_BRIDGE_DRIVER_HPP

#include <unordered_set>
#include <vector>

#include "kcd2s/bridge/apply.hpp"
#include "kcd2s/core/ids.hpp"
#include "kcd2s/replication/schema.hpp"
#include "kcd2s/replication/snapshot.hpp"

namespace kcd2s::integration {

// What the driver drives. The engine implementation lives in the Windows bridge; tests use a spy.
struct ITransformSink {
  virtual ~ITransformSink() = default;
  virtual void on_bind(core::EntityId e) = 0;    // entity now present on the client (CONSTRUCT)
  virtual void on_unbind(core::EntityId e) = 0;  // entity gone (DESTRUCT)
  virtual void apply(const std::vector<bridge::TransformApply>& transforms) = 0;
};

class ClientBridgeDriver {
 public:
  ClientBridgeDriver(const replication::SchemaRegistry& schema, ITransformSink& sink)
      : schema_(schema), sink_(sink) {
  }

  // Call after the ClientPeer has applied this tick's snapshots. Emits bind/unbind for entities
  // that entered/left the mirror since the last sync, then applies the current transform set.
  void sync(const replication::Mirror& mirror);

  std::size_t tracked() const {
    return known_.size();
  }

 private:
  const replication::SchemaRegistry& schema_;
  ITransformSink& sink_;
  std::unordered_set<core::EntityId> known_;
};

}  // namespace kcd2s::integration

#endif  // KCD2S_INTEGRATION_CLIENT_BRIDGE_DRIVER_HPP
