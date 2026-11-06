// The CharacterCreator's stage: the game side of
// Client/resources/character_creator.cjs.
//
// The stage is the pause screen's player page. The pause screen (C_UIApse, WHGame 1.5.5) is
// *(services + 432); vtable slot 1 opens it on a page (4 is the player page) with a context of
// the global and an empty std::function, as the game's own open_apse_player does
// slot 2 closes it, slot 3 is the current page and + 104 its open state machine.
// The pause screen is a 3D scene: its panels are Flash movies drawn on plates, and the player
// page's objects come from the scene's entity links (UIApseLinkNode):
//
//   C_UIApse + 8520   the character stage (vtable slot 14 is its init)
//                     + 32 the dummy actor (the paper-doll figure), + 424 the character panel
//                     entity (slots, stats and niche), + 432 the pedestal entity
//   C_UIApse + 2960   C_UIApsePlayer (its constructor): + 216 the PlayerList plate
//                     entity, + 688 the PlayerInfo plate entity
//
// The figure is filmed by a camera of the creator's own (camera/anchor_view.hpp) instead of the
// page's: in front of the figure, a little to its side so it fills the left half of the screen
// beside the panel and the page's niche is out of the frame; the whole figure for the body, its
// head close for the face, hair and beard, easing from one to the other.
//
// While a creator is open the two plates and the character panel are not drawn (CEntity vtable
// slot 65, not recursive: the pedestal stays), and the dummy wears the look the runtime sends,
// through the same component natives as the player's own body. The player's body is untouched;
// the game copies it into the dummy again the next time the page opens.
//
//   runtime -> game:  { op: "creatorOpen" }, { op: "creatorClose" },
//                     { op: "creatorLook", gender, body, head, hair, beard }
//                     { op: "creatorFrame", close } (close: the head, else the whole figure)
//   game -> runtime:  { op: "creatorStage", state } with state "open" once the stage is up (with
//                     keys: the names of the creator's keys on the player's layout),
//                     "unavailable" when it could not be put up, "left" when the player closed
//                     the pause screen or turned to another page, "interrupted" when the world
//                     went away under it
//                     { op: "creatorKey", key } for a press of the creator's keys (gender G,
//                     random R, reset Backspace, accept Enter, the arrows and Tab, by their place
//                     on the keyboard) while the stage is up and the game window in front
//
// The pause screen stays on the player page while the stage is up (NativeUI's page hold).
//
// Engine thread for frame, network thread for the rest.
#pragma once

#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::character_creator {

using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;
using Logger = void (*)(const char* format, ...);

struct Natives {
  void* (*window)() = nullptr;               // the game window
  void* (*framework)() = nullptr;            // gFramework, for the creator's camera
  void* (*entity_system)() = nullptr;        // IEntitySystem
  void* spawn_params_init = nullptr;         // SEntitySpawnParams::SEntitySpawnParams
  void* (*services)() = nullptr;             // the game services getter
  const void* open_context = nullptr;        // the global
  const void* stage_init = nullptr;          // the stage's vtable slot 14
  const void* apse_player_vtable = nullptr;  // C_UIApsePlayer's
};

bool install(const Natives& natives, Logger logger);

// For local::Client::set_local_runtime_handler: true when the message was the creator's.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Engine thread, every frame: opens, keeps and closes the stage.
void frame(bool in_world);

// Engine thread: whether the stage is open (its own camera films the body).
bool active();

// Network thread: sends the stage's changes to the runtime of session.
void pump(const void* session, const RuntimeSender& send);

}  // namespace kcds::runtime::character_creator
