// The local player's body on the skeleton of its gender, WHGame 1.5.5.
//
// The local player is the game's Player: Henry's male skeleton (male.cdf). A woman's body parts
// on it stretched her fingers, so a woman's body is put on the player woman's skeleton
// (female_skeleton.hpp kPlayerModel), and a man's back on male.cdf, when a resource changes the
// gender (Player.setAppearance).
//
// The character is swapped the way BasicActor.SetActorModel does (CEntity::LoadCharacter,
// as the entity binding LoadCharacter calls it, slot 0), but the body's
// action controller is flushed first (CActionController slot 3): its running fragments
// keep procedural clips that point into the old character (CProceduralClipLookingBlendBone,
// reached it after the swap through the scope context change and crashed in the
// game's abort stub). The new character stands in its bind pose until the actor is
// revived as the actor binding Revive(true) does (C_ScriptBindActor: IActor slot 154,
// reason 4), a few frames later once the new character is in; that starts its animation graph
// again. What the body wore goes with the old character; the inventory puts it back on after the
// revive (inventory_tick.hpp hold_local_inventory, redress_local_body). The script's Properties
// fileModel and clientFileModel take the model too, so the game's own later resets load the same
// one.
//
// Engine thread.
#pragma once

namespace kcds::runtime::appearance::player_skeleton {

using Logger = void (*)(const char* format, ...);

// CEntity::LoadCharacter; false when it was not found (no swap then).
bool install(void* load_character, Logger logger);

// Puts the body of this actor on the skeleton of that gender when it is on the other one; true
// when it swapped.
bool keep(void* actor, bool female);

// Each frame: revives the actor a few frames after keep() swapped its body, once the new
// character is in; true on the frame it did (what the body wore is off then), or found the
// player on another actor.
bool settle(void* actor);

}  // namespace kcds::runtime::appearance::player_skeleton
