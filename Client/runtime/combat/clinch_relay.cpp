// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "clinch_relay.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../replicas/remote.hpp"
#include "clinch_handshake.hpp"

#include <mutex>
#include <vector>

namespace kcds::game_client {

namespace integration = kcd2s::integration;

// starts every clinch: it gives both guards their GuardSyncMovement (90BED0) and
// marks the initiator's clinch component as the one holding the initiative, which alone may choose
// the clinch attacks. A new clinch comes from the approach check C5F9A0 (from
// 72C918, C5F81C and the attack factory 939310), decided from the facing, distance and closing
// speed this client sees (72E234, 72C918). For a replica that view lags its owner, so one client
// could start a clinch the other never did and lock its player for about 1.5 s. A new clinch
// between the local player and a replica therefore starts only through a handshake
// (combat/clinch_handshake.hpp): the initiator's client asks, the other client starts it and
// confirms (or refuses it at once), and only then does the initiator start it and tell every
// other client, whose replicas of the two take the same clinch. A guard start
// continues a clinch after its transitions and pairs from the clinch state they left, in the same
// frame, on every screen: that start stays the game's own. Clinches with NPCs stay native.
StartClinch original_start_clinch;
// Native clinch kinds: 0 from a guard start, 1 from an approach (C5F9A0).
const std::uint32_t* native_clinch_kinds[2] = {};
// the approach precondition on a clinch component (movement+1008): both actors in
// combat and targeting each other, facing each other and within the clinch distance. Read only.
ClinchReady native_clinch_ready = nullptr;
bool applying_relayed_clinch = false;
kcds::runtime::combat::ClinchHandshake clinch_handshake;
// whether a fighter's clinch state (core + 96) is a clinch state (the state table's
// + 77). The guard start continues a clinch only then, and the contact completion (D53778) takes
// a hit from a fighter in one as handled by the clinch. Read only.
MovementCheck native_in_clinch_state = nullptr;
// The combat core's int property with the clinch state a clinch was entered from
// (sets it with the state at core + 96; reads it when the initiator is in a clinch).
constexpr std::size_t kCoreClinchFrom = 144;
// The native action type of a guard (0 GUARD); a clinch hold is a guard owning its
// GuardSyncMovement (has_guard_sync_movement).
constexpr unsigned kNativeGuardType = 0;
// A fighter's clinch state (core + 96) and guard zone (core + 240), values at + 8; -1 for none.
int combat_property_value(void* movement, std::size_t property) {
  auto core = movement
                  ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 528)
                  : nullptr;
  return core ? *reinterpret_cast<int*>(core + property + 8) : -1;
}
// The native checks choose a clinch only between actors that target each other.
bool locked_on_each_other(void* first, void* second) {
  auto first_core = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(first) + 528);
  auto second_core = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(second) + 528);
  return first_core && second_core && *reinterpret_cast<void**>(first_core + 3320) == second &&
         *reinterpret_cast<void**>(second_core + 3320) == first;
}
// The game's own clinch start for a clinch the handshake (or a relayed start) decided. The start
// sets both fighters' guards from its rows (90BED0 ->); a guard that changes there
// makes the clinch component start a transition right after (->), so the
// guards before and after are logged to compare the screens.
char start_decided_clinch(unsigned kind, void* initiator, void* other, void* guard,
                          void* other_guard) {
  const int guard_before = combat_property_value(initiator, kCoreGuard);
  const int other_guard_before = combat_property_value(other, kCoreGuard);
  applying_relayed_clinch = true;
  const char started = original_start_clinch(kind, initiator, other, guard, other_guard);
  applying_relayed_clinch = false;
  log("COMBAT clinch start guards initiator=%u other=%u guards=%d/%d->%d/%d started=%u",
      remote_combat_id(initiator), remote_combat_id(other), guard_before, other_guard_before,
      combat_property_value(initiator, kCoreGuard), combat_property_value(other, kCoreGuard),
      unsigned(started));
  return started;
}
int native_clinch_code(unsigned kind) {
  for (int code = 0; code < 2; ++code) {
    if (native_clinch_kinds[code] && *native_clinch_kinds[code] == kind)
      return code;
  }
  return -1;
}
void send_clinch(core::EntityId target, std::uint8_t mode, int code) {
  integration::CombatActionData data{integration::CombatAction::Clinch, mode, float(code)};
  bool sent;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    sent = client->peer().send_combat_action(local_network_id.load(), target, data);
  }
  const char* what = "start";
  if (mode == 1) {
    what = "request";
  } else if (mode == 2) {
    what = "accept";
  } else if (mode == 4) {
    what = "refusal";
  }
  log("COMBAT clinch %s queue source=%u target=%u kind=%d sent=%u", what, local_network_id.load(),
      target, code, unsigned(sent));
}
// The local game started the clinch the local player initiates against `other` (after its accept):
// every other client's replicas of the two take it too (apply_watched_clinch).
void relay_started_clinch(core::EntityId other, int code) {
  std::vector<core::EntityId> watchers;
  {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    for (const auto& entry : remote_combat_souls) {
      if (entry.second != other) {
        watchers.push_back(entry.second);
      }
    }
  }
  for (const auto watcher : watchers) {
    send_clinch(watcher, 3, code);
  }
}
// A guard start continuing a clinch (calls only while the fighter's clinch
// state is a clinch state, and someone holds the initiative): the game's own on every
// screen, the local player's, its replicas' and a watcher's two replicas alike. Each screen plays
// the clinch's transitions and pairs for both fighters and ends them in the same frame, and the
// start goes on from the clinch state they left there. Answered by a handshake a round trip later
// the guard had already been built plain, its row set the clinch state to 0 and the
// clinch ended after every transition, on the initiator's screen first.
char continue_native_clinch(unsigned kind, void* initiator, void* other, void* guard,
                            void* other_guard) {
  const int state = combat_property_value(initiator, kCoreMode);
  const int guard_before = combat_property_value(initiator, kCoreGuard);
  const int other_guard_before = combat_property_value(other, kCoreGuard);
  const char started = original_start_clinch(kind, initiator, other, guard, other_guard);
  auto own = static_cast<std::uint8_t*>(local_combat_actor.load());
  auto own_move = own ? *reinterpret_cast<void**>(own + 632) : nullptr;
  log("COMBAT clinch continued natively initiator=%u other=%u local_initiator=%u state=%d "
      "guards=%d/%d->%d/%d started=%u",
      remote_combat_id(initiator), remote_combat_id(other), unsigned(initiator == own_move), state,
      guard_before, other_guard_before, combat_property_value(initiator, kCoreGuard),
      combat_property_value(other, kCoreGuard), unsigned(started));
  return started;
}
char __fastcall hooked_start_clinch(unsigned kind, void* initiator, void* other, void* guard,
                                    void* other_guard) {
  if (applying_relayed_clinch)
    return original_start_clinch(kind, initiator, other, guard, other_guard);
  auto own = static_cast<std::uint8_t*>(local_combat_actor.load());
  auto own_move = own ? *reinterpret_cast<void**>(own + 632) : nullptr;
  const bool remote_initiator = remote_combat_id(initiator) != 0;
  const bool remote_other = remote_combat_id(other) != 0;
  const bool involves_player = remote_initiator || remote_other ||
                               (own_move && (initiator == own_move || other == own_move));
  if (involves_player && native_clinch_code(kind) == kcds::runtime::combat::kGuardClinch) {
    return continue_native_clinch(kind, initiator, other, guard, other_guard);
  }
  // A watcher's two replicas lock onto each other like their owners, and its game would
  // clinch them by itself when they met: a clinch the owners never made, and the local player got
  // the clinch's F prompt. Only the owners decide; the watcher takes their relayed clinch.
  if (remote_initiator && remote_other) {
    static ULONGLONG next_log = 0;
    if (GetTickCount64() >= next_log) {
      next_log = GetTickCount64() + 500;
      log("COMBAT clinch between replicas suppressed source=%u other=%u kind=%u",
          remote_combat_id(initiator), remote_combat_id(other), kind);
    }
    return 0;
  }
  if (!own_move || (initiator != own_move && other != own_move))
    return original_start_clinch(kind, initiator, other, guard, other_guard);
  const auto replica = initiator == own_move ? other : initiator;
  const auto id = remote_combat_id(replica);
  if (!id)
    return original_start_clinch(kind, initiator, other, guard, other_guard);
  const int code = native_clinch_code(kind);
  if (code < 0) {
    log("COMBAT clinch unknown kind=%u kept native player=%u", kind, id);
    return original_start_clinch(kind, initiator, other, guard, other_guard);
  }
  if (initiator != own_move) {
    // The replica's owner decides its own new clinches and asks this client.
    static ULONGLONG next_log = 0;
    if (GetTickCount64() >= next_log) {
      next_log = GetTickCount64() + 500;
      log("COMBAT clinch by replica suppressed source=%u kind=%d", id, code);
    }
    return 0;
  }
  if (clinch_handshake.request(id, code, GetTickCount64())) {
    send_clinch(id, 1, code);
  }
  return 0;
}
// A watcher: the sender's game started the clinch it initiates against the player its lock
// is on. The watcher's replica of the sender and its replica of that player take the same native
// clinch, so what follows is replayed on a clinch here as on the owners' screens: the push (F,
// attack_special, which the game takes in a locked fight only in a clinch: ->
// needs the clinch's push time on the pusher's clinch component, + 32) and the clinch
// attacks. Not while either replica is still in a paired action here: a start stops the running
// action, which would cut the pair's hit, fall or get-up still playing here.
void apply_watched_clinch(core::EntityId source, void* initiator, void* own_move, int code,
                          unsigned kind) {
  auto core = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(initiator) + 528);
  auto other = core ? *reinterpret_cast<void**>(core + 3320) : nullptr;
  const auto other_id = remote_combat_id(other);
  const bool locked =
      other && other != own_move && other_id && locked_on_each_other(initiator, other);
  const bool pair_over = locked && !paired_action(native_action_type(initiator)) &&
                         !paired_action(native_action_type(other));
  // The initiator's clinch state and the state it came from, which the start reads.
  const int state = combat_property_value(initiator, kCoreMode);
  const int from = combat_property_value(initiator, kCoreClinchFrom);
  char started = 0;
  if (pair_over) {
    started = start_decided_clinch(kind, initiator, other, nullptr, nullptr);
  }
  log("COMBAT clinch start watched source=%u other=%u kind=%d locked=%u pair_over=%u state=%d "
      "from=%d started=%u",
      source, other_id, code, unsigned(locked), unsigned(pair_over), state, from,
      unsigned(started));
}
void apply_relayed_clinch(core::EntityId source, void* replica_actor, void* local_actor,
                          const integration::CombatActionData& data) {
  using kcds::runtime::combat::ClinchAnswer;
  auto replica_move =
      *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(replica_actor) + 632);
  auto own_move = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(local_actor) + 632);
  const int code = int(data.value);
  if (!replica_move || !own_move || !native_clinch_kinds[code])
    return;
  const unsigned kind = *native_clinch_kinds[code];
  if (data.mode == 3) {
    apply_watched_clinch(source, replica_move, own_move, code, kind);
    return;
  }
  const bool targeted = locked_on_each_other(own_move, replica_move);
  const auto now = GetTickCount64();
  if (data.mode == 4) {
    // The other client could not start the clinch the local player asked for. The local game asks
    // again (after the repeat time) only while it still wants that clinch.
    const bool answered = clinch_handshake.refused(source, code);
    log("COMBAT clinch refusal applied source=%u kind=%d answered=%u", source, code,
        unsigned(answered));
    return;
  }
  if (data.mode == 1) {
    // A new clinch (the approach; a guard start continues a clinch natively and asks nothing). This
    // client must see the players close and facing each other and both in their guards, as the
    // native approach does, so a replica that lags its owner never pulls this
    // player into a clinch on its own. A clinch this client still holds is started again:
    // the requester's game asks for an approach only out of a clinch (kept, the two
    // screens stayed apart and the request was renewed for 20 s).
    auto own_component = *reinterpret_cast<void**>(own_move + kMovementClinch);
    const bool approach = code == kcds::runtime::combat::kApproachClinch;
    const bool guards = native_action_type(own_move) == kNativeGuardType &&
                        native_action_type(replica_move) == kNativeGuardType;
    const bool ready = approach && guards && own_component && native_clinch_ready(own_component);
    const bool held = guards && native_pair_alignment(replica_move);
    const auto answer =
        clinch_handshake.answer(local_network_id.load(), source, targeted && ready, now);
    if (answer == ClinchAnswer::Ignore) {
      log("COMBAT clinch request ignored source=%u kind=%d: own request wins", source, code);
      return;
    }
    char started = 0;
    if (answer == ClinchAnswer::Start) {
      started = start_decided_clinch(kind, replica_move, own_move, nullptr, nullptr);
    }
    log("COMBAT clinch request applied source=%u kind=%d targeted=%u ready=%u held=%u started=%u",
        source, code, unsigned(targeted), unsigned(ready), unsigned(held), unsigned(started));
    send_clinch(source, started ? 2 : 4, code);
    return;
  }
  if (!clinch_handshake.accepted(source, code, now)) {
    log("COMBAT clinch accept ignored source=%u kind=%d: no matching request", source, code);
    return;
  }
  char started = 0;
  if (targeted) {
    started = start_decided_clinch(kind, own_move, replica_move, nullptr, nullptr);
  }
  log("COMBAT clinch accept applied source=%u kind=%d targeted=%u started=%u", source, code,
      unsigned(targeted), unsigned(started));
  if (started) {
    // Before the combat-model values this start changed (relay_combat_model, next frame): the
    // watcher's start reads the initiator's clinch state (core + 96, then + 144)
    // as it was before the start. Given the new state (24) first, the watcher's start took the
    // clinch for one already held and refused it (started=0 twice).
    relay_started_clinch(source, code);
  }
}

}  // namespace kcds::game_client
