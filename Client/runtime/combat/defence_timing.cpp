// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "defence_timing.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../replicas/remote.hpp"

#include <algorithm>
#include <unordered_map>

namespace kcds::game_client {

std::int64_t defence_timing_ms() {
  const ULONGLONG elapsed = GetTickCount64() - remote_attack_started;
  if (!remote_attack_started || elapsed >= kAttackAnswerMs || elapsed > 5000) {
    return -1;
  }
  return std::int64_t(elapsed);
}

// The defence timings players sent with their block presses: player -> {ms after the attack
// started on their screen, when it arrived} (DefenceTiming, defence_timing.hpp).
std::unordered_map<core::EntityId, DefenceTiming> defence_timings;
// A timing older than this when the press arrives is not the press's, in ms.
constexpr ULONGLONG kDefenceTimingMs = 500;
// A lag below this is not made up, in ms (about a frame).
constexpr ULONGLONG kMinCatchUpMs = 15;

void* running_attacker_entity(core::EntityId player) {
  if (remote_attack_source == player ||
      GetTickCount64() - remote_attack_started >= kAttackAnswerMs) {
    return nullptr;
  }
  const auto attacker = remotes.find(remote_attack_source);
  if (attacker == remotes.end() || !attacker->second.actor) {
    return nullptr;
  }
  // The actor's entity (+ 56), as the game's target setter takes it.
  return *reinterpret_cast<void**>(static_cast<std::uint8_t*>(attacker->second.actor) + 56);
}

// A defender's block reaches its attacker a round trip after the defender saw the swing coming
// to the attacker the defender's blade came up that much later in the swing than it did
// for the defender, and the swing landed before the guard. A watcher sees it one network
// hop late (the defender answered the swing as it reached the defender). The replica of a
// defender whose block press arrives while someone attacks plays four times as fast until it has
// made up that lag, so its guard stands where its owner's stood against the swing.
struct DefenceCatchUp {
  void* skeleton = nullptr;
  ULONGLONG until = 0;
  float scales[16] = {};
};
std::unordered_map<core::EntityId, DefenceCatchUp> defence_catch_ups;
constexpr float kDefenceCatchUpScale = 4.0f;
// The last measured contact-to-answer time of a local swing, in ms (47-110).
ULONGLONG combat_round_trip_ms = 80;
constexpr ULONGLONG kMinRoundTripMs = 30;
constexpr ULONGLONG kMaxRoundTripMs = 200;

void start_defence_catch_up(core::EntityId player) {
  const auto replica = remotes.find(player);
  if (replica == remotes.end() || !replica->second.skeleton || defence_catch_ups.count(player)) {
    return;
  }
  const ULONGLONG round_trip =
      (std::min)((std::max)(combat_round_trip_ms, kMinRoundTripMs), kMaxRoundTripMs);
  const ULONGLONG now = GetTickCount64();
  auto local_actor = static_cast<std::uint8_t*>(local_combat_actor.load());
  auto local_movement = local_actor ? *reinterpret_cast<void**>(local_actor + 632) : nullptr;
  // How long after the running attack started this screen shows the press, and the estimate of
  // the lag when the defender sent no timing (a round trip on the attacker, half on a watcher).
  ULONGLONG shown_after = 0;
  ULONGLONG estimate = 0;
  if ((local_movement && native_action_type(local_movement) == kNativeAttackAction) ||
      now - local_attack_started < kAttackAnswerMs) {
    shown_after = now - local_attack_started;
    estimate = round_trip;
  } else if (remote_attack_source != player && now - remote_attack_started < kAttackAnswerMs) {
    shown_after = now - remote_attack_started;
    estimate = round_trip / 2;
  } else {
    return;
  }
  ULONGLONG lag = estimate;
  const auto timing = defence_timings.find(player);
  if (timing != defence_timings.end() && now - timing->second.received < kDefenceTimingMs) {
    // The defender's own screen: the press this long after the attack started.
    lag = shown_after > timing->second.after_attack ? shown_after - timing->second.after_attack : 0;
    defence_timings.erase(timing);
  }
  lag = (std::min)(lag, kMaxRoundTripMs);
  if (lag < kMinCatchUpMs) {
    return;
  }
  DefenceCatchUp catch_up;
  catch_up.skeleton = replica->second.skeleton;
  // At four times the speed the replica gains three times as much as it plays for.
  catch_up.until = GetTickCount64() + lag / 3;
  for (unsigned layer = 0; layer < 16; ++layer) {
    catch_up.scales[layer] = layer_playback_scale(catch_up.skeleton, layer);
  }
  defence_catch_ups[player] = catch_up;
  log("COMBAT DEFENCE catch-up player=%u making up %llu ms", player, lag);
}

// Engine thread, every frame: holds the catch-up speed (the game resets a layer's speed when its
// fragment changes) and puts the speeds back when it ends.
void tick_defence_catch_ups() {
  const ULONGLONG now = GetTickCount64();
  for (auto entry = defence_catch_ups.begin(); entry != defence_catch_ups.end();) {
    const auto replica = remotes.find(entry->first);
    const bool alive =
        replica != remotes.end() && replica->second.skeleton == entry->second.skeleton;
    const bool done = now >= entry->second.until;
    if (alive) {
      for (unsigned layer = 0; layer < 16; ++layer) {
        layer_playback_scale(entry->second.skeleton, layer) =
            done ? entry->second.scales[layer] : kDefenceCatchUpScale;
      }
    }
    if (done || !alive) {
      entry = defence_catch_ups.erase(entry);
    } else {
      ++entry;
    }
  }
}

}  // namespace kcds::game_client
