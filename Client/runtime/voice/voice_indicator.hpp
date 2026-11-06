// The player's voice indicator (the design canvas, In-game HUD: voice chat indicator):
// a pill right of the watermark with the microphone, the range's name and its pips. Quiet gold when
// idle; green with a pulsing ring and level bars that follow the microphone while the player is
// heard, amber on shout; with push-to-talk the key in place of the pips; red and crossed out when
// the server silenced the player. The label flashes once when the range changes.
//
// The server may turn it off (Voice.setIndicator) for a resource's own view, which reads the same
// state through LocalVoice. Set from the engine thread, painted on the render thread (the
// overlay's Present hook), right after the watermark.
#pragma once

#include <string>

struct ID2D1DeviceContext;

namespace kcds::runtime::voice::indicator {

struct State {
  bool shown = false;  // voice chat on and the server lets the game show it
  int tier = 1;        // 0 whisper, 1 normal, 2 shout
  bool muted = false;  // silenced by the server
  bool talking = false;
  bool push_to_talk = false;
  std::wstring key;    // the push-to-talk key, as shown ("V", "M4"); "" for none
  float level = 0.0f;  // the microphone's level, 0 to 1
};

// The newest state (engine thread).
void set(const State& state);

// Paints it onto the frame the canvas draws into (between BeginDraw and EndDraw), right of the
// watermark; nothing while the watermark is not painted. Render thread.
void paint(ID2D1DeviceContext* canvas, float width, float height);

}  // namespace kcds::runtime::voice::indicator
