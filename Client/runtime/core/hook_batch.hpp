// Hooks enabled together while the client starts. MinHook stops every thread of the game for each
// hook it enables, which cost the hundreds of the client's start several seconds of every join; in
// a batch they go live a few at a time, within kHookBatchMilliseconds.
//
// CMake force-includes this header into every source of the game client, so a call of
// MH_EnableHook on the initialization thread joins the batch; on any other thread it is immediate.
#pragma once

#define MH_EnableHook(target) kcds_enable_hook(target)

namespace kcds::game_client {

// From here on the initialization thread's hooks go live in batches.
void begin_hook_batch();

// Everything asked for is live when this returns, and later hooks go live at once.
void end_hook_batch();

}  // namespace kcds::game_client
