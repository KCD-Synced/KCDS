// The players' nameplates as the in-frame overlay paints them, in the owner's design
// of ("In-game HUD — component states"): an optional title above the name in EB
// Garamond italic, the name in Cinzel, and a health bar with diamond caps under it.
//
//   healthy   the bar's red fill in a dark track with a gold rim and gold caps
//   wounded   the fill empties from the right, the same red throughout
//   critical  under 25 %: a brighter red, red rim and caps, a glow pulsing once a second
//   speaking  a horn-mouth icon left of the name while the player talks (Player.setSpeaking)
//   far       past 30 m: the name alone, at 80 % and faded; nothing past 60 m
//
// Sizes are the design's at 1080 pixels high, scaled with the frame's height. The fonts are the
// menu's own (ui/menu/fonts next to the client DLL, WOFF2 unpacked by DirectWrite), the latin-ext
// subsets taking the letters the latin ones lack (Czech names); without them the system's Georgia
// draws. The overlay paints over the game's HUD, so a piece that would fall on the HUD's fixed
// places (the compass, the health and stamina bars) is left out, as if behind it.
//
// The engine thread publishes the frame's plates (nameplates.cpp); the render thread paints the
// newest ones (chat_overlay.cpp, before the watermark).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ID2D1DeviceContext;

namespace kcds::runtime::nameplates {

struct Plate {
  // Whose it is: a plate that is shown stays shown while it stays near enough.
  std::uint32_t id = 0;
  // From the camera, metres: the nearest plates paint last and in full.
  float distance = 0.0f;
  // Where the plate stands on the screen (its bottom middle, a point just above the head): x from
  // the screen's centre and y from its top, in screen heights (the same at any aspect).
  float x = 0.0f;
  float y = 0.0f;
  bool distant = false;           // the name alone, faded
  float size = 1.0f;              // the plate's size by distance, at most 1 (nameplates.cpp)
  float opacity = 1.0f;           // below 1 over a body on the ground
  std::wstring name;              // "" when the name is not shown
  std::wstring title;             // "" for none
  std::uint32_t title_color = 0;  // AARRGGBB; 0 for the design's gold
  float title_size = 0.0f;        // pixels at 1080 high; 0 for the design's
  std::uint32_t color = 0;        // the name's AARRGGBB; 0 for the design's
  float health = -1.0f;           // 0 to 1; below 0 for no bar
  bool speaking = false;
  bool voice_talking = false;  // on voice chat now: a microphone left of the name
  std::wstring label;          // a resource's line above the plate, "" for none
  std::uint32_t label_color = 0;
};

// This frame's plates (engine thread).
void publish(std::vector<Plate> plates);

// Paints the newest plates onto the frame the canvas draws into (between BeginDraw and EndDraw);
// width and height are the frame's size in pixels. Render thread.
void paint(ID2D1DeviceContext* canvas, float width, float height);

}  // namespace kcds::runtime::nameplates
