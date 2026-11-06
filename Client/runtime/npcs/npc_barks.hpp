// What a server NPC says on its own (its barks, its lines in a fight) shown to every player
// (only the player its simulator's game showed it to saw the bubble), WHGame 1.5.5.
//
// The game shows an NPC's line in the HUD's bubbles (C_UIHudBubbles, hud/speech_bubbles.hpp):
// SetBubbleText (slot 4: id, the shown text, the speaker, whether the text is a localization key)
// and then SetBubblePosition (slot 5: id, the world point over the head, scale) every frame. Both
// are hooked, and ReleaseBubble (slot 3) too. A bubble that is not KCDS's own (Npc.say), placed for
// the first time, is handed out with its text and place; the simulator matches it to the server
// NPC under it and reports it (NPC_BARK), and every other client shows it from the NPC's speech
// field. Engine thread.
#pragma once

#include <string>
#include <vector>

namespace kcds::runtime::npcs::barks {

using Logger = void (*)(const char* format, ...);

// Hooks the HUD's bubble calls once it has them (null: not yet). Cheap to call every frame.
void watch(void* hud_bubbles, Logger logger);

struct Placed {
  std::string text;
  bool localize = false;
  float position[3] = {};
};

// The game's own bubbles placed for the first time since the last call.
std::vector<Placed> take_placed();

}  // namespace kcds::runtime::npcs::barks
