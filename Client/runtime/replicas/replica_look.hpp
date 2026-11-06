// The owner's look, aim and view on its replica.
#pragma once

#include <string>

#include "../../../Backend/integration/local/animation.hpp"
#include "remote.hpp"
#include "replica_state.hpp"

namespace kcds::game_client {

// The replica's look and whether the owner's look was applied to it.
struct AppliedLook {
  void* look{};
  core::Vec3 target{};
  bool valid{};
  bool enabled{};
};
template <class OwnerState>
AppliedLook apply_remote_look(Remote& remote, const OwnerState& e, void* entity, void* movement,
                              bool guard_alignment);
extern template AppliedLook apply_remote_look<RemoteState>(Remote& remote, const RemoteState& e,
                                                           void* entity, void* movement,
                                                           bool guard_alignment);
void log_replica_look(core::EntityId id, const char* posture, const AppliedLook& applied,
                      void* skeleton, const std::string& owner_animations);
void note_waiting_look_pose(core::EntityId id, Remote& remote,
                            const kcd2s::local::AnimationLayer& clip);
template <class OwnerState>
void apply_remote_aim(Remote& remote, const OwnerState& e, void* entity);
extern template void apply_remote_aim<RemoteState>(Remote& remote, const RemoteState& e,
                                                   void* entity);
void apply_remote_view(void* actor, const core::Vec3& direction);

}  // namespace kcds::game_client
