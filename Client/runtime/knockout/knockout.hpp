// Knock-out instead of death in a server world.
//
// A player never dies in a server world: the client keeps the game's own hidden immortality buff
// ("immortality_nonpersistent" in rpg/buff.xml: no UI name, no icon, UI visibility None) on the
// local player's soul, so a lethal hit knocks it out with its health at the immortality minimum
// instead of killing it, and it stays down (the game does not end that knock-out by itself).
// Knock-outs that end by themselves, such as in a fist fight, are untouched. The client reports
// the knock-out to the server (the resources hear playerDied) and gets the player up when a
// resource revives it. A resource can let the knocked-out player see the world dimmed instead of
// a black screen (Player.setKnockoutVision). Everything runs through the game's natives;
// no script is involved.
//
// The game's own exhaustion faint (faint.hpp) is no knock-out: the player is not
// reported, kept down, dimmed or kept from the game's own screen, and the game wakes it by itself.
// Its client reports the faint in the player's posture, and its replicas lie down and get up by
// their own games.
#ifndef KCDS_RUNTIME_KNOCKOUT_HPP
#define KCDS_RUNTIME_KNOCKOUT_HPP

#include <cstdint>

#include "body_state.hpp"

namespace kcds::runtime::knockout {

using Logger = void (*)(const char* format, ...);

// The buff manager getter, the soul's state setter (soul, state,
// value; the script binding's SetState), gEnv->pRenderer, the game's singleton getter,
// which leads to the UI that shows the knock-out's seal, and gEnv->pConsole. The
// unconscious buff instance's vtable (faint.hpp) tells a faint from a knock-out; without it every
// unconsciousness counts as a knock-out.
struct Natives {
  void* buff_manager = nullptr;
  void* set_state = nullptr;
  void** renderer = nullptr;
  void* game = nullptr;
  void** console_slot = nullptr;
  void* unconscious_vtable = nullptr;
};

// Keeps the natives. False when a required one is missing (the unconscious vtable is not).
bool install(const Natives& natives, Logger logger);

// Engine thread, every frame: the local player's soul (nullptr outside a server world) and how
// much of the world it sees while knocked out (percent). Keeps the immortality on it and returns
// true once when the player has just been knocked out (a faint is not; a lethal blow during a
// faint is a knock-out then). A knocked-out player who faints stays knocked out.
bool tick(void* local_soul, unsigned vision_percent);

// Engine thread: requests full native health and removal of unconsciousness once. True means
// the removal buff was admitted, not that native wake has completed; observe_body must confirm.
bool revive();

// Engine thread, for every other player's replica: while its owner is knocked out
// (owner_down), its soul holds the game's own permanent unconsciousness, so this game sees the
// body as unconscious (its own lying pose, and the grab it offers over a body that is down); when
// the owner gets up the game's own end of a knock-out takes it away. `soul` is the replica's.
// `owner_fainted`: the owner lies in the exhaustion faint (its posture); once a down
// replica is a faint's, it stays one until it is up again.
void keep_replica_down(std::uint32_t player, void* soul, bool owner_down, bool owner_fainted);

// Whether this game holds player's replica down (keep_replica_down): the game owns its body then
// (its unconscious pose, a grab, a carry), so nothing else moves or animates it.
bool replica_down(std::uint32_t player);

// Whether player's replica is held down for its owner's faint: this game lays the
// body down itself, through its own ragdoll action, and not by the owner's ragdoll pose (a faint
// has no blow, and the owner's body may never become a ragdoll to send one).
bool replica_fainted(std::uint32_t player);

// The replica of player is gone.
void forget_replica(std::uint32_t player);

// Creatures (horses): the same game natives on any other soul, engine thread.
// Gives the soul the game's hidden immortality when it lacks it, so a lethal blow knocks it out
// instead of killing it (the caller checks about once a second: the buffs are searched).
void keep_creature_immortal(void* soul);

// World Architect's local mode:
// the game's hidden immortality and full health kept on the local player's
// soul, about twice a second; engine thread.
void keep_untouchable(void* soul);

// Whether the soul lies unconscious (the game's own check).
bool creature_unconscious(void* soul);

// Keeps the soul down with the game's permanent unconsciousness, when it does not hold it yet.
void hold_creature_down(void* soul);

// Full health and the game's own end of an unconsciousness: the creature gets up.
void wake_creature(void* soul);

// Engine thread: whether the local player lies knocked out (as of the last tick); a faint is not
// a knock-out.
bool knocked_out();

// When the local player was last revived (GetTickCount64), 0 for never.
std::uint64_t revived_at();

// Engine thread: whether the local player lies in the exhaustion faint (as of the last tick).
bool fainted();

// Each frame with the local body: whether it is in the air (body_state_tick).
void note_in_air(bool in_air);

// Each frame: true once when a landing took the local player's last health. The caller then deals
// the game's own lethal damage, and tick() sees the knock-out that follows.
bool fall_took_last_health(void* soul, std::uint64_t now_ms);

// Engine thread: whether the local player is kept down after a knock-out until a revive, also in
// the moment the game itself ends the unconsciousness (other players see it lying throughout).
bool kept_down_now();

}  // namespace kcds::runtime::knockout

#endif  // KCDS_RUNTIME_KNOCKOUT_HPP
