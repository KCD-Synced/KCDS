// The game's horse inspect screen and its "My horse" column,
// WHGame 1.5.5.
//
// The screen (C_UIHorseInspect, opened for the horse looked at) compares that horse
// with the player's own: the player's horse is the first of the player's horses (a
// container's front, index 0). A server horse given to the player (Horse.giveTo) is offered to the
// game as its player's horse (riding/owned_horse.hpp), so the column shows it as the game shows a
// horse of the player's.
//
// A client resource turns the comparison off (Hud.setHorseCompare(false), the runtime's
// { op: 'hudHorseCompare', enabled }): while the screen opens, the player's horse is then answered
// as none, so the column shows "-" as for a player without a horse. Engine thread (the runtime
// message on the network thread only stores the setting).
#pragma once

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::riding::horse_inspect {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* open = nullptr;   // (screen, horse entity id)
  void* front = nullptr;  // (container, index): the player's horse at index 0
};

void install(const Natives& natives, Logger log);

// { op: "hudHorseCompare", enabled }; true when the message was this module's.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

}  // namespace kcds::runtime::riding::horse_inspect
