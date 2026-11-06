// The game's own speech bubbles over a character's head, as it shows the barks of its NPCs
// (wh_ui_BubblesEnabled), WHGame 1.5.5. Used for Npc.say.
//
// The HUD's bubbles are wh::guimodule::C_UIHudBubbles behind its interface
// wh::framework::I_UIHudBubbles, which the entity module holds: *(*(services + 224) + 384) (found
// + 376 is C_UIHudEvents, the item transfer item_notice uses). Its vtable:
//   slot 2 CreateBubble() -> id, a hidden bubble of the HUD's "bubble<id>" clips
//   slot 3 ReleaseBubble(id)
//   slot 4 SetBubbleText(id, const CryString& text, const CryString& speaker,
//                      bool localize, bool); the bubble shows the first
//   slot 5 SetBubblePosition(id, const Vec3& world, float scale); the HUD projects it
//                      onto the screen every frame and shows it once placed
// The game's dialogue shows a bubble within wh_dlg_ChatBubblesDistance (12 m) of the camera,
// scaled down to wh_dlg_ChatBubblesMinScale (0.4) with the distance, wh_dlg_ChatBubblesMinOffset
// to MaxOffset (0.12 to 0.16 m) over the head. Engine thread only.
#pragma once

#include <string>

namespace kcds::runtime::hud {

struct BubbleNatives {
  void* (*services)() = nullptr;     // the game services getter
  void* string_from_text = nullptr;  // CryString from a text
  void* string_free = nullptr;
  // SetText of the game's subtitles, resolved when the client starts: a scan of the
  // game during play held a frame for 17 s.
  void* subtitle_set_text = nullptr;
  // C_UISubtitles' constructor: the module the game makes, its interface at + 0x58.
  void* subtitles_constructor = nullptr;
  void (*log)(const char* format, ...) = nullptr;
};

void install_bubbles(const BubbleNatives& natives);

// A bubble with the speaker's name and the text, not yet placed; 0 when the HUD has none.
// `localize`: the text is the game's localization key.
unsigned create_bubble(const std::string& speaker, const std::string& text, bool localize = false);

// The HUD's bubbles interface (C_UIHudBubbles), null while the HUD has none.
void* hud_bubbles();

// Whether create_bubble is setting its own bubble's text right now (the text hook leaves it).
bool own_text();

// Puts the bubble over a world point at a scale (1 near the camera).
void place_bubble(unsigned id, const float position[3], float scale);

void release_bubble(unsigned id);

// The game's subtitle line at the bottom of the screen (issue 016: a remote NPC's line showed in a
// bubble but not as a subtitle). wh::guimodule::C_UISubtitles behind its dialog interface (at +
// 88 of it): slot 1 SetText(const CryString& text, const CryString& name, bool isPlayer, bool
// force, bool isLocalizable) (shows it only while the player has subtitles on, or
// with force; localizes the texts with isLocalizable) and slot 2 Clear. The interface
// is taken where the game makes its subtitles module or calls its SetText. False while the game has
// made none.
bool show_subtitle(const std::string& speaker, const std::string& text, bool localize);
void clear_subtitle();

}  // namespace kcds::runtime::hud
