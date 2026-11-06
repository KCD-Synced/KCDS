// The studio's export to a resource: the studio's editor makes the files of a
// server resource from its scene (Client/resources/studio/editor.cjs exportResource) and the
// game client writes them on this machine, in the local mode only:
//
//   { op: 'studioExport', name, files: [[relative path, text], ...], mesh? }
//     -> %LOCALAPPDATA%\KCDS\studio\exports\<name>\<relative path>
//   mesh: { name, texture?, tiling? }, your mesh in the resource's stream folder:
//     stream/objects/kcds/<name>/<mesh>.cgf with its material path rewritten to the resource's
//     (cgf_patch.hpp), its .mtl, and with a texture <mesh>_tex.dds (studio_texture.hpp) and a
//     material that shows it. Players get them through the resource download like any stream file.
//
// The name is 1 to 64 of a-z, 0-9 and _; a path is relative, of a-z A-Z 0-9 _ - . and /, with
// no "..", at most 16 files of 64 KiB. Anything else is refused and nothing is written.
//
// handle_runtime_message: any thread.
#pragma once

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::studio::exporter {

// True for a studioExport message (written or refused).
bool handle_runtime_message(const kcd2s::sdk::Value& message);

}  // namespace kcds::runtime::studio::exporter
