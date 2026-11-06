// The players' animations as this client captures and replays them: the
// local player's clips and motion parameters, its human overlay (the face, layer 11) owned and
// sent, the game's clip start, stop and clear hooks that keep replicas on their owners' clips, the
// clips already running when a body became ready, and the replicas' own human overlay.
#pragma once

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

#include "../../../Backend/integration/local/animation.hpp"
#include "../../../Backend/integration/local/human_overlay.hpp"
#include "../replicas/remote.hpp"
#include "human_overlay.hpp"

namespace kcds::game_client {

namespace local = kcd2s::local;

using FacialPakChanged = std::int64_t(__fastcall*)(void*);
struct PendingHumanOverlay {
  void* skeleton = nullptr;  // compared to the current owner; never dereferenced after replacement
  bool start = false;
  int clip = -1;
  local::AnimationParams params{.layer = local::kHumanOverlayLayer};
};
struct HumanOverlayGuard {
  std::shared_ptr<const kcds::runtime::human_overlay::Membership> membership;
  std::uint64_t generation = 0;
  std::uint64_t native_starts = 0;
};
using StopAnimation = char(__fastcall*)(void*, unsigned, float);
// The blend an owner's stopped clip ends with on its replica, in seconds.
constexpr float kRemoteStopBlendSeconds = 0.25f;
// How long a reaction's clip stands in for its owner's same clip, in ms.
constexpr ULONGLONG kReactionClipMs = 500;

// The local player's clips, motion parameters and weapon clips as captured this frame.
extern local::AnimationState local_animations;
extern float motion_params[18];
extern std::uint32_t motion_mask;
extern std::atomic<void*> local_weapon_skeleton;
extern local::AnimationState local_weapon_animations;
// Set every frame, before the capture, from the local player's native action: a plain attack (3).
extern std::atomic<bool> local_plain_attack;

// The human overlay (the face): its natives, readiness, the owner's state and the replicas' guards.
extern std::atomic<bool> human_overlay_ready;
extern kcds::runtime::human_overlay::Natives human_overlay_natives;
extern std::atomic<bool> human_overlay_install_pending;
// Called by frame() every frame: the human overlay installed on the engine thread once its
// natives are found (human_overlay_install_pending).
void install_pending_human_overlay();
extern FacialPakChanged original_facial_pak_changed;
std::int64_t __fastcall hooked_facial_pak_changed(void* manager);
extern std::optional<kcds::runtime::human_overlay::Membership> owner_overlay_membership;
extern local::HumanOverlay local_human_overlay;
extern void* human_overlay_owner_skeleton;
extern std::optional<kcds::runtime::human_overlay::Scope> human_overlay_owner_scope;
extern bool human_overlay_fifo_stopped;
extern int unavailable_owner_overlay_clip;
extern std::optional<PendingHumanOverlay> pending_human_overlay;
extern std::unordered_map<void*, HumanOverlayGuard> owned_human_overlay_skeletons;

// The game's clip start, stop, clear and motion parameter (hooked: the originals).
extern StopAnimation original_stop_animation;
std::int64_t __fastcall hooked_clear_animation(void* skeleton, unsigned layer);
char __fastcall hooked_stop_animation(void* skeleton, unsigned layer, float blend);
void __fastcall hooked_motion_param(void* skeleton, int id, float value);
char __fastcall hooked_start_animation(void* skeleton, int id, const void* params);
// A replica's reaction window: the game animates it itself for a moment.
bool in_reaction_window(void* skeleton);

// The clips already running on a skeleton, captured when its body became ready.
void capture_existing_animations(void* skeleton, bool current_progress);
// A replica's human overlay kept on its owner's, and the identities dropped after a pak change.
void clear_remote_human_overlay(Remote& remote);
bool keep_remote_human_overlay(core::EntityId player, Remote& remote,
                               const kcd2s::replication::Mirror::Entity& latest);
void invalidate_human_overlays_after_pak_change();

}  // namespace kcds::game_client
