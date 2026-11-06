// The editors' script-step pipe for an AI assistant: the one MCP server of World
// Architect and the Asset/Animation Studio (Dev/tools/architect-mcp) drives the open editor's
// script steps through it, the Studio's tools above all. World Architect's own commands go over
// its AI bridge (ai_bridge.hpp, TCP on this PC with a token); this pipe is beside it.
//
// Only in the local mode (local_mode.hpp: the game's developer mode and the editor asked for):
// the game client opens a named pipe on this machine, \\.\pipe\kcds-editor-<process id>, for the
// user's own processes. There is no network and no server. Each line the MCP server writes is
// JSON { "id": number, "line": "a step of the open editor" }; the editor's answer comes back as
// one JSON line { "id": number, "text": "..." }. The steps are the editors' own script steps
// (architect/editor.cjs and studio/editor.cjs script()).
//
//   { op: 'editorReply', id, text }   from the resource runtime: an editor's answer
//
// start: once, when the local mode runs. take: engine thread. handle_runtime_message: any thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::architect::editor_pipe {

struct Step {
  std::int64_t id = 0;
  std::string line;
};

// Opens the pipe and serves it on its own thread; does nothing outside the local mode.
void start();

// The steps the MCP server sent since the last call.
std::vector<Step> take();

// An editor's answer to a step; false for any other message.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

}  // namespace kcds::runtime::architect::editor_pipe
