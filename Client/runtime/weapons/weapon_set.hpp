// The client resources' Weapons: the local player draws, holsters or toggles the selected weapon
// set and the torch with the game's own actions and animations (WHGame 1.5.5).
//
// The weapon quick access manager (the soul's items, slot 480 / 8; their quick access, 392 / 8;
// its weapon manager, 128 / 8) keeps the selected slot at + 240 (slot 96 / 8 reads it) and each
// slot's two items at + 48 + 48 * slot. Its slot 120 / 8 says whether a slot may be used now, and
// slot 72 / 8 runs the selected slot's action, as the HUD's toggle_holster_qam_weapon key does:
// the human's animated draw when the slot's item is not in a hand, its animated holster when it
// is. The weapon stays equipped either way. It takes a std::function called with the action's
// finish reason (0 when it ran to its end) and answers false when the game's action manager
// refused to start it (another action, a restriction of the actor).
//
// The torch: the player input's draw_holster_torch handler (TorchToggle) on the local player's
// input (actor + 2952). The torch is drawn while the left hand holds an item of the torch kind
// (IsTorchItem). Its toggle tells nothing back: the hand is watched for up to kTorchSeconds.
//
// The calls are synchronous requests ({ op: 'syncRequest', id, request: { op: 'weapons...' } },
// Client/resources/weapons.cjs), answered on the engine frame; an action's end goes to the
// runtime as { op: 'weaponsEvent', action, ok, reason }.
#pragma once

#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::weapons::set {

using Logger = void (*)(const char* format, ...);
using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;

struct Natives {
  void* torch_toggle = nullptr;  // (player input)
  void* is_torch = nullptr;      // (item) -> bool
  // The slot's action under the manager's slot 72 / 8: (slot, result, actor, animated, selected,
  // done), its result's first byte 0 once started, else the game's reason. Hooked to read it.
  void* run_slot = nullptr;
  Logger log = nullptr;
};

void install(const Natives& natives);

// Whether a hand of the actor (the local player or an NPC) holds an item of its selected weapon
// set: a weapon drawn, not a tool or a spoon. Engine thread.
bool drawn(void* actor);

// Whether the actor's left hand holds a torch. Engine thread.
bool torch_drawn(void* actor);

// The actor's selected weapon set (0 to 3), -1 without its weapon manager; and selecting one, so a
// replica draws the set its simulator's body drew. Engine thread.
int selected_slot(void* actor);
bool select_slot(void* actor, unsigned index);

// A resource runtime message; false when it is not a Weapons request. Any thread.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Answers the requests and watches a running action (engine thread, in a world).
void frame(void* local_actor);

// An action of the local player's input that a resource did not hold off (engine thread). The
// game draws or holsters only when its draw key (toggle_holster_qam_weapon, B) is held past the
// key's hold delay, so a tap of it did nothing. A tap (released within the hold delay) is
// carried out here as Weapons.toggle, unless the game itself drew or holstered by
// kKeyWaitSeconds after the release.
void on_action(void* local_actor, const char* action, unsigned mode);

// Sends the answers and the actions' ends to the runtime (network_mutex held).
void pump(const RuntimeSender& send);

}  // namespace kcds::runtime::weapons::set
