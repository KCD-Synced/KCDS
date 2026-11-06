// World Architect's projects: what the author built on a map, kept on this machine
// between sessions as %LOCALAPPDATA%\KCDS\architect\projects\<map>.json. The editor
// (Client/resources/architect/editor.cjs) writes it after its changes; the local mode
// (local_mode.hpp) hands it back when the editor opens.
//
//   { op: 'architectSave', map, data }   the project's JSON text, written whole (at most 4 MiB)
//   { op: 'architectSave', map, data, part, parts }   a larger project in parts (a message holds
//                                        64 KiB), in order from 0: written once the last came
//
// handle_runtime_message: any thread.
#pragma once

#include <string>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::architect::projects {

// True when the message was an architectSave one (written in the local mode only).
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// The project kept for a map, empty when there is none.
std::string load(const std::string& map);

}  // namespace kcds::runtime::architect::projects
