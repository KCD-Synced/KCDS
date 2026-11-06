// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "animation_capture.hpp"

#include "../core/log.hpp"
#include "../book/resource_book.hpp"
#include "../combat/reaction_window.hpp"
#include "../core/natives.hpp"
#include "../dice/dice_table.hpp"
#include "../replicas/motion_capture.hpp"
#include "female_retarget.hpp"
#include "player_animation.hpp"
#include "../replicas/remote.hpp"
#include "human_overlay.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace kcds::game_client {

namespace replication = kcd2s::replication;

MovementRequest original_movement_request;
std::atomic<std::uint32_t> local_engine_id{0};
std::shared_mutex motion_mutex;
std::atomic<void*> local_skeleton{nullptr};
float motion_params[18]{};
std::uint32_t motion_mask = 0;
MotionParam original_motion_param;
local::AnimationState local_animations{};
std::atomic<bool> human_overlay_ready{false};
kcds::runtime::human_overlay::Natives human_overlay_natives;
std::atomic<bool> human_overlay_install_pending{false};
std::atomic<DWORD> engine_update_thread{0};
std::atomic<unsigned> human_overlay_pak_mutations{0};
std::atomic<std::uint64_t> human_overlay_pak_generation{0};
std::uint64_t observed_human_overlay_pak_generation = 0;
SRWLOCK human_overlay_pak_lock = SRWLOCK_INIT;
FacialPakChanged original_facial_pak_changed = nullptr;
std::optional<kcds::runtime::human_overlay::Membership> owner_overlay_membership;
local::HumanOverlay local_human_overlay;
void* human_overlay_owner_skeleton = nullptr;
std::optional<kcds::runtime::human_overlay::Scope> human_overlay_owner_scope;
bool human_overlay_fifo_stopped = false;
int unavailable_owner_overlay_clip = -1;
std::optional<PendingHumanOverlay> pending_human_overlay;
std::unordered_map<void*, HumanOverlayGuard> owned_human_overlay_skeletons;  // motion_mutex

class HumanOverlayRead {
 public:
  HumanOverlayRead() : acquired_(TryAcquireSRWLockShared(&human_overlay_pak_lock) != FALSE) {
  }

  ~HumanOverlayRead() {
    if (acquired_) {
      ReleaseSRWLockShared(&human_overlay_pak_lock);
    }
  }

  explicit operator bool() const {
    return acquired_;
  }

  HumanOverlayRead(const HumanOverlayRead&) = delete;
  HumanOverlayRead& operator=(const HumanOverlayRead&) = delete;

 private:
  bool acquired_ = false;
};
// The weapon in the local player's hands: its skeleton, and the clips its game started on it
// (a bow bending, a crossbow's string, a hand cannon's match and shot with their effects), which
// its replicas' weapons play too.
std::atomic<void*> local_weapon_skeleton{nullptr};
local::AnimationState local_weapon_animations{};
std::unordered_map<void*, local::AnimationStopFence> animation_stop_fences;
std::uint64_t animation_sequence = 0;
std::unordered_map<void*, HorseCapture> horse_captures;  // same motion_mutex as player capture
kcds::runtime::replicas::MotionIndex<RemoteInputs>
    remote_inputs;  // key: skeleton, protected by motion_mutex
bool native_neutral_attack_active(void* movement);
AnimationProgress native_animation_progress = nullptr;
thread_local bool replaying_animation = false;
bool native_pair_alignment(void* movement);
bool in_reaction_window(void* skeleton);
std::int64_t __fastcall hooked_facial_pak_changed(void* manager) {
  human_overlay_pak_mutations.fetch_add(1);
  AcquireSRWLockExclusive(&human_overlay_pak_lock);
  __try {
    return original_facial_pak_changed(manager);
  } __finally {
    human_overlay_pak_generation.fetch_add(1);
    ReleaseSRWLockExclusive(&human_overlay_pak_lock);
    human_overlay_pak_mutations.fetch_sub(1);
  }
}

bool managed_human_overlay_start_impl(void* skeleton, int clip, unsigned layer) {
  // Also inside a reaction window: the body layers then take the game's own reaction, but
  // the owner's game makes the same grunt or pain face and its clip comes on this layer; the
  // replica's own one played it twice (owner 303481 once, its replica twice in 31 ms) and
  // the mouth opened again.
  if (replaying_animation || !human_overlay_ready.load() || layer != local::kHumanOverlayLayer ||
      human_overlay_pak_mutations.load()) {
    return false;
  }
  std::shared_ptr<const kcds::runtime::human_overlay::Membership> membership;
  {
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    const auto guard = owned_human_overlay_skeletons.find(skeleton);
    if (guard == owned_human_overlay_skeletons.end() ||
        guard->second.generation != human_overlay_pak_generation.load()) {
      return false;
    }
    membership = guard->second.membership;
  }
  const auto scope = kcds::runtime::human_overlay::scope(skeleton);
  return membership && scope && membership->scope == *scope &&
         kcds::runtime::human_overlay::alias(skeleton, clip).has_value();
}

bool managed_human_overlay_start(void* skeleton, int clip, unsigned layer) {
  // Workers pass through: a model may have changed since the engine published its cache.
  if (replaying_animation || layer != local::kHumanOverlayLayer ||
      GetCurrentThreadId() != engine_update_thread.load() ||
      !TryAcquireSRWLockShared(&human_overlay_pak_lock)) {
    return false;
  }
  __try {
    return managed_human_overlay_start_impl(skeleton, clip, layer);
  } __finally {
    ReleaseSRWLockShared(&human_overlay_pak_lock);
  }
}

void note_remote_human_overlay_start(void* skeleton, unsigned layer) {
  if (replaying_animation || layer != local::kHumanOverlayLayer) {
    return;
  }
  std::lock_guard<std::shared_mutex> lock(motion_mutex);
  const auto guard = owned_human_overlay_skeletons.find(skeleton);
  if (guard != owned_human_overlay_skeletons.end()) {
    ++guard->second.native_starts;
  }
}

bool managed_layer(void* skeleton, unsigned layer) {
  if (replaying_animation || layer >= 16)
    return false;
  if (layer == local::kHumanOverlayLayer) {
    // Native layer-wide stop/clear also serves body and facial-pak cleanup. A verified facial
    // start is filtered separately; ownership never blocks a whole native layer here.
    return false;
  }
  if (in_reaction_window(skeleton))
    return false;
  std::lock_guard<std::shared_mutex> lock(motion_mutex);
  auto it = remote_inputs.find(skeleton);
  return it != remote_inputs.end() && it->second.animations[layer].sequence != 0;
}
ClearAnimation original_clear_animation;
StopAnimation original_stop_animation;

void stop_local_human_overlay(bool fence_fifo) {
  human_overlay_fifo_stopped = fence_fifo;
  if (local_human_overlay.state != local::HumanOverlayState::Stop) {
    local_human_overlay = {};
    local_human_overlay.state = local::HumanOverlayState::Stop;
    local_human_overlay.sequence = ++animation_sequence;
    log("HUMAN OVERLAY owner stop sequence=%llu", local_human_overlay.sequence);
  }
}

void reset_local_human_overlay(void* skeleton,
                               const std::optional<kcds::runtime::human_overlay::Scope>& scope) {
  if (human_overlay_owner_skeleton != skeleton || human_overlay_owner_scope != scope) {
    human_overlay_owner_skeleton = skeleton;
    human_overlay_owner_scope = scope;
    local_human_overlay = {};
    unavailable_owner_overlay_clip = -1;
    pending_human_overlay.reset();
    stop_local_human_overlay(false);
  }
}

// Called only by frame/FIFO capture on the engine update thread. Native hook callers merely
// record their scalar request below; audio/animation workers never resolve engine identities.
void resolve_owner_human_overlay(void* skeleton, int clip, const local::AnimationParams& params,
                                 bool from_fifo) {
  if (human_overlay_pak_mutations.load()) {
    return;
  }
  const auto alias = kcds::runtime::human_overlay::alias(skeleton, clip);
  if (!alias) {
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    if (pending_human_overlay) {
      return;
    }
    stop_local_human_overlay(human_overlay_fifo_stopped);
    if (unavailable_owner_overlay_clip != clip) {
      unavailable_owner_overlay_clip = clip;
      log("HUMAN OVERLAY owner identity unavailable clip=%d", clip);
    }
    return;
  }
  local::HumanOverlay captured;
  captured.sequence = 1;
  captured.alias_hash = *alias;
  captured.state = local::HumanOverlayState::Start;
  captured.params = params;
  if (!local::valid_human_overlay(captured)) {
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    if (!pending_human_overlay) {
      stop_local_human_overlay(human_overlay_fifo_stopped);
    }
    return;
  }
  std::lock_guard<std::shared_mutex> lock(motion_mutex);
  if (pending_human_overlay) {
    return;
  }
  if (from_fifo && (human_overlay_fifo_stopped ||
                    (local_human_overlay.state == local::HumanOverlayState::Start &&
                     local_human_overlay.alias_hash == *alias))) {
    return;
  }
  human_overlay_fifo_stopped = false;
  unavailable_owner_overlay_clip = -1;
  captured.sequence = ++animation_sequence;
  local_human_overlay = captured;
  log("HUMAN OVERLAY owner start alias=%llx clip=%d sequence=%llu", *alias, clip,
      captured.sequence);
}

// Whether the local player's face clips go to the replicas. Not sent: every clip the
// game starts on the face layer is a dialogue facial of FacialAnimations.img, the lip-sync of a
// voice line the local player speaks (in a fight its grunts and hit cries,
// Localization/dialog/open_world/boj_a_zraneni). Only this game plays the line, so on every other
// screen the replica moved its lips without a voice: it "talked" through the fight (each grunt's
// line is followed within 30 ms by a face clip 303479 to 303485).
// A replica's own faces stay refused (managed_human_overlay_start), so its face is
// still. A voice sync that plays the line on the replica could send its face with it again.
constexpr bool kSendOwnerFaces = false;

void capture_human_overlay_start(void* skeleton, int clip, const local::AnimationParams& params,
                                 bool from_fifo) {
  if (!human_overlay_ready.load() || skeleton != local_skeleton.load() ||
      params.layer != local::kHumanOverlayLayer) {
    return;
  }
  if (!kSendOwnerFaces) {
    return;
  }
  if (from_fifo) {
    resolve_owner_human_overlay(skeleton, clip, params, true);
    return;
  }
  std::lock_guard<std::shared_mutex> lock(motion_mutex);
  pending_human_overlay = PendingHumanOverlay{skeleton, true, clip, params};
  human_overlay_fifo_stopped = false;
}

void drain_owner_human_overlay(void* skeleton) {
  if (!human_overlay_ready.load() || skeleton != local_skeleton.load() ||
      human_overlay_pak_mutations.load()) {
    return;
  }
  const auto generation = human_overlay_pak_generation.load();
  const auto scope = kcds::runtime::human_overlay::scope(skeleton);
  if (!scope || !owner_overlay_membership || owner_overlay_membership->scope != *scope) {
    owner_overlay_membership =
        scope ? kcds::runtime::human_overlay::snapshot(skeleton) : std::nullopt;
  }
  if (human_overlay_pak_mutations.load() || human_overlay_pak_generation.load() != generation) {
    owner_overlay_membership.reset();
    return;
  }
  std::optional<PendingHumanOverlay> pending;
  {
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    reset_local_human_overlay(skeleton, scope);
    pending = std::exchange(pending_human_overlay, std::nullopt);
  }
  if (!pending || pending->skeleton != skeleton) {
    return;
  }
  if (pending->start) {
    resolve_owner_human_overlay(skeleton, pending->clip, pending->params, false);
  } else {
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    if (!pending_human_overlay) {
      stop_local_human_overlay(true);
    }
  }
}

void capture_stop(void* skeleton, unsigned layer, bool from_fifo = false) {
  if (layer >= 16)
    return;
  std::lock_guard<std::shared_mutex> lock(motion_mutex);
  if (skeleton == local_skeleton.load() && layer == local::kHumanOverlayLayer) {
    if (human_overlay_ready.load()) {
      if (from_fifo) {
        // An empty FIFO ends a stop fence; a later unobserved native start may seed again.
        if (!pending_human_overlay) {
          stop_local_human_overlay(false);
        }
      } else {
        pending_human_overlay = PendingHumanOverlay{skeleton};
        human_overlay_fifo_stopped = true;
      }
    }
    auto& legacy = local_animations[layer];
    if (legacy.sequence && legacy.clip >= 0) {
      animation_stop_fences[skeleton].stop(layer, legacy.clip);
      legacy.clip = -1;
      legacy.params.layer = layer;
      legacy.sequence = ++animation_sequence;
    }
    return;
  }
  if (skeleton == local_weapon_skeleton.load()) {
    auto& weapon = local_weapon_animations[layer];
    if (!(weapon.sequence && weapon.clip == -1)) {
      weapon.clip = -1;
      weapon.params.layer = layer;
      weapon.sequence = ++animation_sequence;
    }
    return;
  }
  auto h = horse_captures.find(skeleton);
  if (skeleton != local_skeleton.load() && (h == horse_captures.end() || !h->second.authority))
    return;
  auto& a =
      skeleton == local_skeleton.load() ? local_animations[layer] : h->second.animations[layer];
  if (a.sequence && a.clip == -1)
    return;
  animation_stop_fences[skeleton].stop(layer, a.clip);
  a.clip = -1;
  a.params.layer = layer;
  a.sequence = ++animation_sequence;
}
bool trample_fall_skeleton(void* skeleton);

std::int64_t __fastcall hooked_clear_animation(void* skeleton, unsigned layer) {
  if (trample_fall_skeleton(skeleton)) {
    log("COMBAT TRAMPLE replica fall clear layer=%u replaying=%u", layer,
        unsigned(replaying_animation));
  }
  if (managed_layer(skeleton, layer))
    return 0;
  auto result = original_clear_animation(skeleton, layer);
  capture_stop(skeleton, layer);
  return result;
}
char __fastcall hooked_stop_animation(void* skeleton, unsigned layer, float blend) {
  if (trample_fall_skeleton(skeleton)) {
    log("COMBAT TRAMPLE replica fall stop layer=%u blend=%.2f replaying=%u", layer, blend,
        unsigned(replaying_animation));
  }
  if (managed_layer(skeleton, layer))
    return 1;
  auto result = original_stop_animation(skeleton, layer, blend);
  if (result)
    capture_stop(skeleton, layer);
  return result;
}
void __fastcall hooked_motion_param(void* skeleton, int id, float value) {
  if (id >= 0 && id < 17) {
    // The engine's animation workers call this for every parameter of every body: they only read.
    std::shared_lock<std::shared_mutex> lock(motion_mutex);
    auto it = remote_inputs.find(skeleton);
    if (it != remote_inputs.end() && !native_pair_alignment(it->second.movement) &&
        (it->second.mask & (1u << id)))
      value = it->second.params[id];
  }
  original_motion_param(skeleton, id, value);
  bool captured_horse = false;
  if (id >= 0 && id < 17 && std::isfinite(value)) {
    std::shared_lock<std::shared_mutex> lock(motion_mutex);
    const auto h = horse_captures.find(skeleton);
    captured_horse = h != horse_captures.end() && h->second.authority;
  }
  if (captured_horse) {
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    auto h = horse_captures.find(skeleton);
    if (h != horse_captures.end() && h->second.authority) {
      h->second.params[id] = value;
      h->second.mask |= 1u << id;
    }
  }
  if (skeleton == local_skeleton.load() && id >= 0 && id < 17 && std::isfinite(value)) {
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    motion_params[id] = value;
    motion_mask |= 1u << id;
  }
}
StartAnimation original_start_animation;
// diagnostics: every clip start for a few seconds after a trample (both clients).
std::atomic<ULONGLONG> clip_trace_until{0};
// The shortest blend (seconds) a base-layer clip of the local player seated at a dice table is
// sent with.
constexpr float kDiceSeatedBlend = 0.3f;

bool in_reaction_window(void* skeleton) {
  std::lock_guard<std::mutex> lock(reaction_clips_mutex);
  auto it = reaction_windows.find(skeleton);
  return it != reaction_windows.end() && GetTickCount64() < it->second;
}
// diagnostics: the seated local rider sometimes switches to the on-foot clips
// (idle 10672, walk 16560-16562) and stands on the horse. The native caller of such a start is
// logged to find which system picks the on-foot fragment.
std::atomic<bool> local_rider_seated{false};

bool on_foot_clip(int id) {
  return (id >= 10670 && id <= 10677) || (id >= 16560 && id <= 16562) || id == 16442;
}

// Every local clip start for a few seconds after the local player starts mounting is logged with
// its native caller too (a bad mount switched layer 15 to the on-foot 15951 94 ms in).
std::atomic<ULONGLONG> mount_trace_until{0};
// The skeletons of trampled replicas while they fall: the game's own fall reaction (fragment
// HitDeath, clip 3920 on layer 8, then its ragdoll) runs on the replica, but the replica's
// locomotion started its idle (10670 on layer 0, 15953 on layer 15) 235 ms in and cut the fall
// off (live burst: the replica stayed upright and slid beside the horse). Until it gets up, only
// the fall layer starts natively on it (kTrampleFallLayer, core/client_internal.hpp).
std::mutex trample_fall_mutex;
std::unordered_set<void*> trample_fall_skeletons;

bool trample_fall_guarded(void* skeleton, unsigned layer) {
  if (replaying_animation || layer == kTrampleFallLayer) {
    return false;
  }
  std::lock_guard<std::mutex> lock(trample_fall_mutex);
  return trample_fall_skeletons.count(skeleton) != 0;
}

bool trample_fall_skeleton(void* skeleton) {
  std::lock_guard<std::mutex> lock(trample_fall_mutex);
  return trample_fall_skeletons.count(skeleton) != 0;
}

void log_seated_on_foot_start(void* skeleton, int id, unsigned layer) {
  if (skeleton != local_skeleton.load()) {
    return;
  }
  const bool mounting = GetTickCount64() < mount_trace_until.load();
  if (!mounting && (layer != 0 || !local_rider_seated.load() || !on_foot_clip(id))) {
    return;
  }
  void* stack[16]{};
  const auto count = CaptureStackBackTrace(1, 16, stack, nullptr);
  log("RIDER ON-FOOT clip=%d layer=%u %s", id, layer, mounting ? "while mounting" : "while seated");
  for (unsigned i = 0; i < count; ++i) {
    const auto at = reinterpret_cast<std::uintptr_t>(stack[i]);
    const auto base = reinterpret_cast<std::uintptr_t>(image);
    if (at >= base && at - base < image_size) {
      log("RIDER ON-FOOT caller frame=%u ida=%llx", i, 0x180000000ULL + at - base);
    }
  }
}
// Defined with the swing hold.
void local_clip_started(void* skeleton, int id, unsigned layer);
char __fastcall hooked_start_animation(void* skeleton, int id, const void* params) {
  local::AnimationParams values;
  std::memcpy(&values, params, sizeof(values));
  if (GetTickCount64() < clip_trace_until.load()) {
    log("TRAMPLE TRACE clip start skeleton=%p local=%u clip=%d layer=%u flags=0x%x managed=%u",
        skeleton, unsigned(skeleton == local_skeleton.load()), id, values.layer, values.flags,
        unsigned(managed_layer(skeleton, values.layer)));
  }
  if (managed_human_overlay_start(skeleton, id, values.layer) ||
      managed_layer(skeleton, values.layer))
    return 1;
  log_seated_on_foot_start(skeleton, id, values.layer);
  if (trample_fall_guarded(skeleton, values.layer)) {
    log("COMBAT TRAMPLE replica fall kept clip=%d layer=%u refused", id, values.layer);
    return 1;
  }
  // Test hook: a clip the game starts itself on a replica (a gardener's
  // replica jumped between poses while she hoed).
  static const bool replica_trace =
      GetEnvironmentVariableW(L"KCDS_TEST_REPLICA_TRACE", nullptr, 0) > 0;
  if (replica_trace && !replaying_animation && skeleton != local_skeleton.load()) {
    bool replica = false;
    {
      std::lock_guard<std::shared_mutex> lock(motion_mutex);
      replica = remote_inputs.find(skeleton) != remote_inputs.end();
    }
    if (replica) {
      log("REPLICA NATIVE clip=%d layer=%u skeleton=%p transition=%.2f", id, values.layer, skeleton,
          values.transition);
    }
  }
  auto result = original_start_animation(skeleton, id, params);
  if (result) {
    note_remote_human_overlay_start(skeleton, values.layer);
    local_clip_started(skeleton, id, values.layer);
    capture_human_overlay_start(skeleton, id, values, false);
  }
  if (result) {
    kcds::runtime::animation::female_retarget::note_clip_start(skeleton, id, values.layer);
  }
  if (result && values.layer < 16) {
    // A start ends the layer's stop fence: the clip that was fading out is started anew, so the
    // capture takes it as a new start (the same clip played again after a stop).
    // Our own starts that reach this hook (player and NPC clips) never hold motion_mutex.
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    animation_stop_fences[skeleton].start(values.layer);
  }
  if (result && skeleton == local_skeleton.load() && values.layer == local::kHumanOverlayLayer) {
    return result;
  }
  if (result && values.layer < 16 && skeleton != local_skeleton.load() &&
      in_reaction_window(skeleton)) {
    void* movement = nullptr;
    {
      std::lock_guard<std::shared_mutex> lock(motion_mutex);
      auto replica = remote_inputs.find(skeleton);
      if (replica != remote_inputs.end()) {
        movement = replica->second.movement;
      }
    }
    const unsigned action = movement ? native_action_type(movement) : 0;
    const bool variant = variant_reaction_action(action);
    std::lock_guard<std::mutex> lock(reaction_clips_mutex);
    reaction_clips[skeleton][values.layer] = ReactionClip{id, GetTickCount64(), variant};
    log("COMBAT REACTION replica clip=%d layer=%u started by the game (action %u)", id,
        values.layer, action);
  }
  // A clip a resource ships has an id of this client's own: the replicas play it by name from the
  // field "animationProps", so its id is not sent.
  if (result && skeleton == local_skeleton.load() &&
      kcds::runtime::player_animation::starting_shipped_clip()) {
    log("LOCAL ANIMATION clip=%d layer=%u is a shipped clip: sent by name", id, values.layer);
    return result;
  }
  if (result && values.layer < 16) {
    local::AnimationLayer captured;
    captured.clip = id;
    captured.params = values;
    captured.sequence = 1;
    // At a dice table the game cuts from the seated idle to a throw with no blend (9215 after
    // 9206, transition 0): unseen in the player's own view, but the other games' replicas jumped
    // from one seated pose to the other. Theirs blend in instead. The server's NPC
    // opponent is seated in this game too, so its clips go out the same way.
    const bool seated_at_dice =
        skeleton == local_skeleton.load() || kcds::runtime::dice::seated_npc(skeleton);
    if (seated_at_dice && values.layer == 0 && values.transition < kDiceSeatedBlend &&
        kcds::runtime::dice::at_table()) {
      captured.params.transition = kDiceSeatedBlend;
    }
    if (local::valid_animation(captured, values.layer)) {
      std::lock_guard<std::shared_mutex> lock(motion_mutex);
      if (skeleton == local_weapon_skeleton.load()) {
        captured.sequence = ++animation_sequence;
        local_weapon_animations[values.layer] = captured;
        log("LOCAL WEAPON ANIMATION clip=%d layer=%u sequence=%llu", id, values.layer,
            captured.sequence);
        return result;
      }
      auto h = horse_captures.find(skeleton);
      if (skeleton != local_skeleton.load() && (h == horse_captures.end() || !h->second.authority))
        return result;
      if (skeleton == local_skeleton.load() && kcds::runtime::book::holding_reading_pose()) {
        log("LOCAL ANIMATION clip=%d layer=%u kept from others until the book's put-away", id,
            values.layer);
        return result;
      }
      animation_stop_fences[skeleton].start(values.layer);
      captured.sequence = ++animation_sequence;
      (skeleton == local_skeleton.load() ? local_animations : h->second.animations)[values.layer] =
          captured;
      log("%s ANIMATION clip=%d layer=%u flags=0x%x transition=%.2f sequence=%llu",
          skeleton == local_skeleton.load() ? "LOCAL" : "SIMULATED", id, values.layer, values.flags,
          values.transition, captured.sequence);
    }
  }
  return result;
}
// SkeletonAnim v15: FIFO count; v18: 120-byte CAnimation entry.
// Seed layers already running when the debug player became ready; hook handles subsequent starts.
// Set every frame, before the capture, from the local player's native action: a plain attack (3).
std::atomic<bool> local_plain_attack{false};
void capture_existing_animations_impl(void* skeleton, bool current_progress) {
  drain_owner_human_overlay(skeleton);
  for (unsigned layer = 0; layer < 16; ++layer) {
    auto count = slot<unsigned(__fastcall*)(void*, unsigned)>(skeleton, 15)(skeleton, layer);
    if (count > 16)
      continue;
    if (!count) {
      capture_stop(skeleton, layer, true);
      continue;
    }
    auto raw = slot<const std::uint8_t*(__fastcall*)(void*, unsigned, unsigned)>(skeleton, 18)(
        skeleton, layer, count - 1);
    const int clip = *reinterpret_cast<const int*>(raw + 16);
    local::AnimationLayer a;
    a.sequence = 1;
    a.clip = clip;
    a.params.layer = layer;
    std::memcpy(&a.params.transition, raw + 32, 4);
    // Human combat retains the native start parameter and paired-action lifecycle.
    // Horse transition capture can opt into the verified whole-clip progress getter.
    std::memcpy(&a.params.normalized_time, raw + 28, 4);
    // an attack clip starts part-way in on the owner (already at 0.23 when the attack
    // began) while its start parameter says 0, so a receiver started the swing from the beginning
    // and showed it about 200 ms late. During a plain attack the local combat layer relays its real
    // clip position, so the receiver starts the swing where the owner's is. Paired actions keep
    // their native start parameter.
    const bool attack_progress =
        layer == 0 && skeleton == local_skeleton.load() && local_plain_attack.load();
    if (current_progress || attack_progress) {
      if ((*reinterpret_cast<void***>(skeleton))[28] !=
          reinterpret_cast<void*>(native_animation_progress)) {
        failed = true;
        return;
      }
      a.params.normalized_time = native_animation_progress(skeleton, raw);
    }
    std::memcpy(&a.params.speed, raw + 44, 4);
    std::memcpy(&a.params.token, raw + 48, 4);
    std::memcpy(&a.params.flags, raw + 76, 4);
    std::memcpy(&a.params.user_token, raw + 84, 4);
    std::memcpy(a.params.user_data, raw + 88, 32);
    if (skeleton == local_skeleton.load() && layer == local::kHumanOverlayLayer) {
      if (!human_overlay_ready.load() || human_overlay_pak_mutations.load() ||
          !owner_overlay_membership || clip < 0 ||
          static_cast<std::size_t>(clip) >= owner_overlay_membership->clips.size()) {
        continue;
      }
      if (owner_overlay_membership->contains(clip)) {
        capture_human_overlay_start(skeleton, clip, a.params, true);
        // Retire a previous ordinary body clip from the legacy layer before facial ownership.
        std::lock_guard<std::shared_mutex> lock(motion_mutex);
        auto& legacy = local_animations[layer];
        if (legacy.sequence && legacy.clip >= 0) {
          legacy.clip = -1;
          legacy.params.layer = layer;
          legacy.sequence = ++animation_sequence;
        }
        continue;
      }
      // A non-facial body clip keeps the existing KSA1 contract. Facial metadata carries Stop.
      {
        std::lock_guard<std::shared_mutex> lock(motion_mutex);
        stop_local_human_overlay(false);
      }
    }
    if (!local::valid_animation(a, layer))
      continue;
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    auto h = horse_captures.find(skeleton);
    if (skeleton != local_skeleton.load() && (h == horse_captures.end() || !h->second.authority))
      continue;
    // The idle the game starts at the end of a book's reading (hooked_start_animation).
    if (skeleton == local_skeleton.load() && kcds::runtime::book::holding_reading_pose())
      continue;
    auto& captured = skeleton == local_skeleton.load() ? local_animations : h->second.animations;
    if (animation_stop_fences[skeleton].fading(layer, clip))
      continue;
    if (captured[layer].sequence && captured[layer].clip == clip) {
      // Keep native progress current without creating another start sequence.
      // A transition receiver can reseed a skipped clip at its current time.
      captured[layer].params.normalized_time = a.params.normalized_time;
      continue;
    }
    animation_stop_fences[skeleton].start(layer);
    a.sequence = ++animation_sequence;
    captured[layer] = a;
    log("LOCAL FIFO clip=%d layer=%u flags=0x%x sequence=%llu", clip, layer, a.params.flags,
        a.sequence);
  }
}

void capture_existing_animations(void* skeleton, bool current_progress) {
  if (GetCurrentThreadId() != engine_update_thread.load() ||
      !TryAcquireSRWLockShared(&human_overlay_pak_lock)) {
    return;
  }
  __try {
    if (!human_overlay_pak_mutations.load()) {
      capture_existing_animations_impl(skeleton, current_progress);
    }
  } __finally {
    ReleaseSRWLockShared(&human_overlay_pak_lock);
  }
}
void clear_remote_human_overlay(Remote& remote) {
  if (remote.skeleton) {
    const bool previous = replaying_animation;
    replaying_animation = true;
    original_clear_animation(remote.skeleton, local::kHumanOverlayLayer);
    replaying_animation = previous;
  }
  remote.human_overlay_clip = -1;
}

void release_remote_human_overlay(Remote& remote) {
  std::lock_guard<std::shared_mutex> lock(motion_mutex);
  owned_human_overlay_skeletons.erase(remote.skeleton);
  remote.human_overlay_sequence = 0;
  remote.attempted_overlay_sequence = 0;
  remote.human_overlay_clip = -1;
  remote.human_overlay_scope.reset();
  remote.human_overlay_membership.reset();
  remote.stopping_overlay_count = 0;
}

bool keep_remote_human_overlay_impl(core::EntityId player, Remote& remote,
                                    const replication::Mirror::Entity& latest) {
  if (!human_overlay_ready.load() || !remote.skeleton ||
      latest.fields.size() <= local::HumanOverlayAnimation) {
    return false;
  }
  const auto scope = kcds::runtime::human_overlay::scope(remote.skeleton);
  if (!scope) {
    release_remote_human_overlay(remote);
    return false;
  }
  if (scope != remote.human_overlay_scope || !remote.human_overlay_membership) {
    auto membership = kcds::runtime::human_overlay::snapshot(remote.skeleton);
    if (!membership || membership->scope != *scope) {
      release_remote_human_overlay(remote);
      return false;
    }
    remote.human_overlay_membership =
        std::make_shared<const kcds::runtime::human_overlay::Membership>(std::move(*membership));
    remote.human_overlay_scope = scope;
    remote.human_overlay_sequence = 0;
    remote.attempted_overlay_sequence = 0;
    remote.human_overlay_clip = -1;
    remote.unavailable_overlay_alias = 0;
    remote.stopping_overlay_count = 0;
  }
  // Layer 11 is not universally facial. Never clear or suppress another native body queue.
  const auto fifo_count = slot<unsigned(__fastcall*)(void*, unsigned)>(remote.skeleton, 15)(
      remote.skeleton, local::kHumanOverlayLayer);
  if (fifo_count > 16) {
    release_remote_human_overlay(remote);
    return false;
  }
  std::array<int, 16> fifo_clips{};
  for (unsigned index = 0; index < fifo_count; ++index) {
    const auto entry = slot<const std::uint8_t*(__fastcall*)(void*, unsigned, unsigned)>(
        remote.skeleton, 18)(remote.skeleton, local::kHumanOverlayLayer, index);
    if (!entry ||
        !remote.human_overlay_membership->contains(*reinterpret_cast<const int*>(entry + 16))) {
      release_remote_human_overlay(remote);
      return false;
    }
    fifo_clips[index] = *reinterpret_cast<const int*>(entry + 16);
  }
  const auto state = local::decode_human_overlay(
      std::get<std::string>(latest.fields[local::HumanOverlayAnimation]));
  if (!state) {
    if (remote.human_overlay_clip >= 0) {
      clear_remote_human_overlay(remote);
    }
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    return owned_human_overlay_skeletons.count(remote.skeleton) != 0;
  }
  if (state->state == local::HumanOverlayState::Unset) {
    bool owned = false;
    {
      std::lock_guard<std::shared_mutex> lock(motion_mutex);
      owned = owned_human_overlay_skeletons.count(remote.skeleton) != 0;
    }
    if (owned) {
      clear_remote_human_overlay(remote);
    }
    release_remote_human_overlay(remote);
    remote.observed_overlay_sequence = 0;
    return false;
  }
  if (state->sequence < remote.observed_overlay_sequence) {
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    return owned_human_overlay_skeletons.count(remote.skeleton) != 0;
  }
  remote.observed_overlay_sequence = state->sequence;
  if (latest.fields.size() > local::Animations) {
    const auto body =
        local::decode_animation(std::get<std::string>(latest.fields[local::Animations]));
    if (body) {
      const auto& layer = (*body)[local::kHumanOverlayLayer];
      if (layer.clip >= 0 && !remote.human_overlay_membership->contains(layer.clip)) {
        release_remote_human_overlay(remote);
        return false;
      }
    }
  }
  bool first = false;
  std::uint64_t native_starts = 0;
  {
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    auto [guard, inserted] = owned_human_overlay_skeletons.try_emplace(remote.skeleton);
    first = inserted;
    guard->second.membership = remote.human_overlay_membership;
    guard->second.generation = human_overlay_pak_generation.load();
    native_starts = guard->second.native_starts;
  }
  if (first) {
    // An explicit owner state replaces any autonomous overlay that predated ownership.
    clear_remote_human_overlay(remote);
  }
  if (state->state == local::HumanOverlayState::Stop) {
    if (state->sequence > remote.human_overlay_sequence) {
      remote.stopping_overlay_clips = fifo_clips;
      remote.stopping_overlay_count = fifo_count;
      const bool previous = replaying_animation;
      replaying_animation = true;
      original_stop_animation(remote.skeleton, local::kHumanOverlayLayer, kRemoteStopBlendSeconds);
      replaying_animation = previous;
      remote.human_overlay_sequence = state->sequence;
      remote.applied_overlay_native_starts = native_starts;
      remote.human_overlay_clip = -1;
      remote.unavailable_overlay_alias = 0;
      log("HUMAN OVERLAY replica stop player=%u sequence=%llu", player, state->sequence);
    } else if (fifo_count != 0) {
      // A worker may have queued a facial clip before the ownership cache was published.
      // Its provenance was checked above; no body queue is removed here.
      const bool unexpected =
          std::any_of(fifo_clips.begin(), fifo_clips.begin() + fifo_count, [&](int clip) {
            return std::find(remote.stopping_overlay_clips.begin(),
                             remote.stopping_overlay_clips.begin() + remote.stopping_overlay_count,
                             clip) ==
                   remote.stopping_overlay_clips.begin() + remote.stopping_overlay_count;
          });
      if (unexpected || remote.applied_overlay_native_starts != native_starts) {
        clear_remote_human_overlay(remote);
        remote.applied_overlay_native_starts = native_starts;
      }
    }
    return true;
  }
  // Resolve against the current set even for an already applied sequence. Facial-pak changes
  // can renumber local indices; no prior entry pointer is reused.
  const auto clip = kcds::runtime::human_overlay::resolve(remote.skeleton, state->alias_hash);
  if (!clip) {
    if (remote.human_overlay_clip >= 0 || remote.unavailable_overlay_alias != state->alias_hash) {
      clear_remote_human_overlay(remote);
    }
    if (remote.unavailable_overlay_alias != state->alias_hash) {
      remote.unavailable_overlay_alias = state->alias_hash;
      log("HUMAN OVERLAY replica alias unavailable player=%u alias=%llx", player,
          state->alias_hash);
    }
    return true;
  }
  const bool owner_clip_present = std::find(fifo_clips.begin(), fifo_clips.begin() + fifo_count,
                                            *clip) != fifo_clips.begin() + fifo_count;
  if (state->sequence == remote.human_overlay_sequence && *clip == remote.human_overlay_clip &&
      native_starts == remote.applied_overlay_native_starts &&
      (fifo_count == 0 || owner_clip_present)) {
    return true;
  }
  if (!kcds::runtime::human_overlay::ready_for_start(remote.skeleton, *clip)) {
    if (remote.unavailable_overlay_alias != state->alias_hash) {
      remote.unavailable_overlay_alias = state->alias_hash;
      log("HUMAN OVERLAY replica CAF not resident player=%u alias=%llx", player, state->alias_hash);
    }
    return true;
  }
  if (remote.attempted_overlay_sequence == state->sequence &&
      remote.attempted_overlay_native_starts == native_starts) {
    return true;
  }
  remote.attempted_overlay_sequence = state->sequence;
  remote.attempted_overlay_native_starts = native_starts;
  remote.stopping_overlay_count = 0;
  const bool previous = replaying_animation;
  replaying_animation = true;
  const char accepted = original_start_animation(remote.skeleton, *clip, &state->params);
  replaying_animation = previous;
  if (accepted) {
    remote.human_overlay_sequence = state->sequence;
    remote.applied_overlay_native_starts = native_starts;
    remote.human_overlay_clip = *clip;
    remote.unavailable_overlay_alias = 0;
    remote.stopping_overlay_count = 0;
    log("HUMAN OVERLAY replica start player=%u alias=%llx clip=%d sequence=%llu", player,
        state->alias_hash, *clip, state->sequence);
  } else if (remote.human_overlay_clip >= 0) {
    clear_remote_human_overlay(remote);
  }
  if (!accepted) {
    log("HUMAN OVERLAY replica start refused player=%u alias=%llx sequence=%llu", player,
        state->alias_hash, state->sequence);
  }
  return true;
}

bool keep_remote_human_overlay(core::EntityId player, Remote& remote,
                               const replication::Mirror::Entity& latest) {
  if (GetCurrentThreadId() != engine_update_thread.load() ||
      !TryAcquireSRWLockShared(&human_overlay_pak_lock)) {
    release_remote_human_overlay(remote);
    return false;
  }
  __try {
    if (human_overlay_pak_mutations.load()) {
      release_remote_human_overlay(remote);
      return false;
    }
    return keep_remote_human_overlay_impl(player, remote, latest);
  } __finally {
    // SEH can leave the guarded engine adapter before C++ destructors run (/EHsc).
    ReleaseSRWLockShared(&human_overlay_pak_lock);
  }
}

void invalidate_human_overlays_after_pak_change() {
  HumanOverlayRead pak_read;
  if (!pak_read || human_overlay_pak_mutations.load()) {
    return;
  }
  const auto generation = human_overlay_pak_generation.load();
  if (generation == observed_human_overlay_pak_generation) {
    return;
  }
  observed_human_overlay_pak_generation = generation;
  owner_overlay_membership.reset();
  {
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    owned_human_overlay_skeletons.clear();
    human_overlay_owner_scope.reset();
    pending_human_overlay.reset();
    local_human_overlay = {};
    stop_local_human_overlay(false);
  }
  for (auto& [id, remote] : remotes) {
    remote.human_overlay_scope.reset();
    remote.human_overlay_membership.reset();
    remote.human_overlay_sequence = 0;
    remote.attempted_overlay_sequence = 0;
    remote.human_overlay_clip = -1;
    remote.unavailable_overlay_alias = 0;
  }
  log("HUMAN OVERLAY facial pak generation=%llu; native identities discarded", generation);
}

void install_pending_human_overlay() {
  if (human_overlay_install_pending.exchange(false)) {
    const bool installed = kcds::runtime::human_overlay::install(human_overlay_natives);
    human_overlay_ready.store(installed);
    log("HUMAN OVERLAY native identity adapter=%u", unsigned(installed));
  }
}

}  // namespace kcds::game_client
