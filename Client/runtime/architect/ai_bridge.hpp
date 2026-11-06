// World Architect for AI assistants.
// The game client listens on this PC only
// (127.0.0.1, port KCDS_ARCHITECT_AI_PORT, 47870 when unset), and only in World Architect's local
// mode. The MCP server (Dev/tools/architect-mcp) connects and speaks lines of text:
//
//   1. its first line is the token the game wrote to %LOCALAPPDATA%\KCDS\architect\ai-token
//      (made new each start); the game answers "ok", or closes on anything else;
//   2. then each line is one JSON request { id, command, args }, passed to the editor as
//      architectCommand { text }; the editor's architectReply { text } goes back as one line.
//
// The game reads no JSON here: the editor (Client/resources/architect/ai_commands.cjs) does.
// One connection at a time; a request line is at most 1 MiB.
// handle_runtime_message: any thread; pump: network thread.
#pragma once

#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::architect::ai_bridge {

// True when the message was the editor's architectReply (written to the connection).
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Starts listening once (local mode only) and passes the requests read since to the editor.
void pump(const std::function<bool(const kcd2s::sdk::Value&)>& send);

}  // namespace kcds::runtime::architect::ai_bridge
