// Story cutscenes: Hud.setCutscenes and storyCutsceneSuppressed.
//
// A quest plays a cutscene through its concept graph's CutsceneHandler node (registered in):
// the node builds the cutscene instance and queues it with the cutscene player
// (wh::guimodule::C_CutscenePlayer, called only from the node's enqueue).
// The player tells its listeners how the cutscene goes: 0 queued, 1 about to play,
// 2 played, 3 finished, 4 skipped; the node fires its OnQueued, BeforePlay, AfterPlay and
// OnFinished ports from them. A cutscene puts the player into the game's cutscene
// state, can stop the world clock and pulls in quest scenery, so multiplayer suppresses it by
// default: the hook does not queue it, tells the listeners 0 to 3 at once (so the quest goes on as
// if it had played) and reports its name (the instance's + 24). Fast travel, sleeping and waiting
// the player starts do not go through the node and always play.
//
// The instance (shared_ptr's object): + 24 its name (const char*), + 32 the cutscene it plays
// (the player queues nothing without one).
#pragma once

#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::hud::story_cutscenes {

using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;
using Logger = void (*)(const char* format, ...);

struct Natives {
  void* queue = nullptr;   // C_CutscenePlayer's queue of a quest's cutscene, hooked
  void* notify = nullptr;  // its listeners' notification
};

// False when a native is missing: story cutscenes then play.
bool install(const Natives& natives, Logger logger);

// { op: "hudStoryCutscenes", enabled }; true when the message was this module's.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Network thread: reports suppressed cutscenes as { op: "hudStoryCutsceneSuppressed", cutscene }
// to the runtime of session; a new session suppresses them again.
void pump(const void* session, const RuntimeSender& send);

}  // namespace kcds::runtime::hud::story_cutscenes
