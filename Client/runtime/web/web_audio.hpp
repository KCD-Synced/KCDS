// The sound of web views. The browser plays it in a helper process of the game's own
// (kcds-web-helper.exe), so its level is set on that process's sessions in Windows' mixer, every
// second (a session appears when a page starts playing): the player's Browser volume setting
// (Settings > Sound, saved in %LOCALAPPDATA%\KCDS\web-audio.json) times the server's share for
// this player (Player.webVolume, Player.webMuted; web_audio_level.hpp).
#pragma once

namespace kcds::runtime::web::audio {

using Logger = void (*)(const char* format, ...);

// Starts applying the level (once).
void start(Logger logger);

// The player's own setting, 0 to 1 (1 by default); setting it saves it and applies it at once.
float setting();
void set_setting(float value);

// The server's share for this player: volume 0 to 100, muted (runtime op webAudio).
void set_server(float volume, bool muted);

}  // namespace kcds::runtime::web::audio
