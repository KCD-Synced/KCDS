// Windows/CryEngine engine glue for the client bridge. This is the ONLY part
// that touches WHGame.dll. It is compiled solely in a Windows client build (see CMake gate) and
// injected into the running game; it cannot be built or run on the headless host. The header stays
// free of Windows/D3D types so the rest of the client can reference the interface.
//
// Pipeline (real): on load -> resolve WHGame functions/globals by signature (Resolver) -> acquire
// gEnv (ISystem::GetGlobalEnvironment / RIP-relative global) -> hook IDXGISwapChain::Present. Each
// Present: pull the latest Mirror transforms (bridge/apply) and write them into CEntity objects via
// the resolved engine entry points, then call the original Present.
#ifndef KCD2S_BRIDGE_ENGINE_WIN_HPP
#define KCD2S_BRIDGE_ENGINE_WIN_HPP

#include "kcd2s/bridge/apply.hpp"
#include "kcd2s/replication/schema.hpp"
#include "kcd2s/replication/snapshot.hpp"

namespace kcd2s::bridge {

// Maps our dense replication EntityId to the engine's entity id, established at CONSTRUCT time so
// transform applies and destructs can find the live CEntity. (Implementation in engine_win.cpp.)
class EngineBridge {
 public:
  // Resolve engine symbols, acquire gEnv, install the Present hook. Returns false if any required
  // symbol fails to resolve (logged); the caller may retry on a later frame.
  bool initialize();

  // Remove hooks and release engine references. Safe to call if not initialized.
  void shutdown();

  bool ready() const {
    return ready_;
  }

  // Called from the frame hook: write the mirror's transforms into the engine this frame. The
  // schema tells extract_transforms which fields are position/rotation.
  void apply_frame(const replication::SchemaRegistry& schema, const replication::Mirror& mirror);

  // Associate / forget an engine entity for one of our ids (called on snapshot CONSTRUCT/DESTRUCT).
  void bind_entity(core::EntityId ours, std::uint64_t engine_id);
  void unbind_entity(core::EntityId ours);

 private:
  bool ready_ = false;
};

}  // namespace kcd2s::bridge

#endif  // KCD2S_BRIDGE_ENGINE_WIN_HPP
