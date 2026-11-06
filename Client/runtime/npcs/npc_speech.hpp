// What an NPC says (Npc.say and LocalNpc.say): the text in the game's
// own speech bubble over its head (hud/speech_bubbles.hpp) for as long as the speech lasts, as the
// game's dialogue shows one: within wh_dlg_ChatBubblesDistance (12 m) of the camera, scaled down
// to wh_dlg_ChatBubblesMinScale (0.4) far away, over a standing head (about 1.85 m) by
// wh_dlg_ChatBubblesMinOffset to MaxOffset. Engine thread only.
#pragma once

#include <cstdint>
#include <string>

namespace kcds::runtime::npcs {

struct Speech {
  // The speech text last taken ("<sequence>|<milliseconds>|<text>", "" for none).
  std::string field;
  std::string speaker;
  std::string text;
  double until{};
  unsigned bubble{};
  // The game's own line of the NPC (a bark its simulator reported), and whether the text is a
  // localization key.
  bool native = false;
  bool localize = false;
  // The voice file the simulator's game played with its own line (game-root relative, empty for
  // none) and the standalone_file_purpose state it played it with.
  std::string voice;
  std::uint32_t purpose = 0;
  // Whether this game shows the NPC's own line as a subtitle too (issue 016: every game but its
  // simulator's, whose game shows its own), and whether it is showing it now.
  bool subtitle_wanted = false;
  bool subtitled = false;
};

// Takes the speech text when it differs from the last one; true when it starts a new speech.
bool take_speech(Speech& speech, const std::string& field, const std::string& speaker, double now);

// Shows the bubble over the body (an IEntity) while the speech lasts and the camera is near, and
// takes it away otherwise. `view` is the view camera's matrix (its position at 3, 7 and 11), null
// when there is none.
void show_speech(Speech& speech, void* entity, const float* view, double now);

// Takes the bubble away.
void end_speech(Speech& speech);

}  // namespace kcds::runtime::npcs
