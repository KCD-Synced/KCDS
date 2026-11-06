// World Architect's saves: the author keeps setups under names
// in Documents\KCDS\World Architect\<name>.json on this PC, loads one back or merges several into
// the work. The editor (Client/resources/architect/editor.cjs) sends and asks:
//
//   architectSaveAs { name, data }: writes the save (at most 4 MiB), replacing one so named.
//   architectSaveDelete { name }: deletes it.
//   sync localArchitectSaves: [{ name, bytes, time }], time in seconds since 1970.
//   architectSaveAs { name, data, part, parts }: a larger save in parts (a message holds 64 KiB),
//     in order from 0, written once the last came.
//   sync localArchitectSaveRead { name, offset }: { text, next } the save's JSON text from offset
//     in pieces (next -1 at the end), or null.
//   sync localArchitectExport { folder, files: [{ name, text, append }], map }: writes an export
//     (Export for Blender, Export for server) into exports\<folder>\ (at most 64 MiB), answers
//     the folder's path or null. A file larger than a message comes in pieces, each after the
//     first with append set. `map`, a custom map's name (maps/custom_maps.hpp): its level.pak and
//     map.json are copied into maps\<map>\ of the export, for the server to send its players.
//
// A name is 1 to 48 letters, digits, spaces, '-' or '_' (nothing else reaches the file system).
// Local mode only (local_mode.hpp). handle_runtime_message: any thread; pump: network thread.
#pragma once

#include <filesystem>
#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::architect::saves {

// Documents\KCDS\World Architect on this PC (empty when Windows does not say where Documents is).
std::filesystem::path architect_folder();

// True when the message was one of the above (a sync request is answered at the next pump).
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Sends the answers made.
void pump(const std::function<bool(const kcd2s::sdk::Value&)>& send);

}  // namespace kcds::runtime::architect::saves
