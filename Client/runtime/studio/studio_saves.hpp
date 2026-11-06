// The Asset/Animation Studio's saves (as World Architect's Saves tab): the author
// keeps scenes under names in Documents\KCDS\Asset Studio\<name>.json on this PC and loads one
// back. The editor (Client/resources/studio/editor.cjs) sends:
//
//   { op: 'studioSaveAs', name, data }   writes the save (at most 60 KiB, one message), replacing
//                                        one so named; answered with the list
//   { op: 'studioSaveDelete', name }     deletes it; answered with the list
//   { op: 'studioSaveList' }             answered { op: 'studioSaves', saves: [{ name, bytes,
//                                        time }] }, time in seconds since 1970, newest first
//   { op: 'studioSaveLoad', name }       answered { op: 'studioSaveLoaded', name, data } (data ''
//                                        when it could not be read)
//
// A name is 1 to 48 letters, digits, spaces, '-' or '_' (nothing else reaches the file system).
// handle_runtime_message: any thread; tick: engine thread.
#pragma once

#include <functional>
#include <string>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::studio::saves {

bool handle_runtime_message(const kcd2s::sdk::Value& message);

void tick(const std::function<bool(const kcd2s::sdk::Value&)>& send);

// Whether a save may be named so.
bool save_name(const std::string& name);

}  // namespace kcds::runtime::studio::saves
