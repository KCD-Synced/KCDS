// The local player held out of the world by the server (LOBBY, Player.setLobby), while the
// server makes their character or logs them in. While held the game makes no sound: its audio
// sessions on the default output (the world, and voice chat, which plays in this process) are
// muted through Windows' own session volume, so nothing of the world is heard however the game
// mixes it. A server's page runs in its own process and keeps its sound. The resource runtime
// hears it too, and keeps a server's loading page up (Client/resources/loading_screen.cjs).
#pragma once

namespace kcds::runtime::lobby {

// Holds the player out of the world, or lets them in. Any thread; a change mutes or unmutes the
// game's sound at once. True when it changed.
bool set(bool held);

// Whether the player is held now.
bool held();

// Any thread, often (every frame is fine): while held, mutes again about once a second, for the
// game opens sound sessions of its own later (a sound bank, voice chat's output).
void keep_muted();

}  // namespace kcds::runtime::lobby
