// Your own meshes in the studio: a mesh made in Blender and converted to the game's
// own model files (Dev/tools/assets/blender/kcds_export.py: a .cgf with its streams in it and its
// .mtl) is read from %LOCALAPPDATA%\KCDS\studio\meshes\, put in the game's file system at
// objects/kcds_studio/<name>.cgf in a pack from memory (native_ui mount_files), and shown in the
// studio as a static mesh entity the game loads itself (props/prop_entity spawn_mesh).
//
//   { op: 'studioMeshList' }          the editor asks which meshes there are: answered
//                                     { op: 'studioMeshes', names: [...] }
//   { op: 'studioMesh', name, at, turn, scale }
//                                     shows that mesh (the one before goes), at [x, y, z],
//                                     turned [x, y, z] degrees (z * y * x), scale times its size;
//                                     name '' takes it away
//   { op: 'studioMeshPlace', at, turn, scale }
//                                     moves, turns and sizes the mesh shown
//   { op: 'studioMeshWrite', name, model, material }
//                                     a shape the studio's prop menu made (studio/shapes.cjs):
// `model` the .cgf in base64 (CrCh), `material`
//                                     the .mtl's text, written as <name>.cgf and <name>.mtl into
//                                     the meshes folder; answered { op: 'studioMeshWritten',
//                                     name, ok, error }
//   { op: 'studioMeshBody', kind, mass }
//                                     the mesh shown (and each one shown after it) given a body
//                                    : 'none' (stands as placed, nothing hits it),
//                                     'solid' (stands, things hit it) or 'falls' (rigid, `mass`
//                                     kg: falls from where it is placed and rests); put back
//                                     where it was placed first; no body while cloth or held
//   joint (either op)     a joint of the character to hold the mesh in ('RightHand'
//                                     and 'LeftHand': the grip joints in the palms; '' or none:
//                                     free in the scene); held, at and turn are its place and turn
//                                     in the joint, it follows the joint every frame and is not
//                                     made cloth
//   { op: 'studioMeshCloth', on, mass }
//                                     the mesh shown (and each one shown after it) as cloth of
//                                     `mass` kg hanging from its highest vertices, or solid again
//                                     (physical_props/cloth.hpp); moved, a cloth is
//                                     solid until 0.5 s after the last move, then cloth anew
//
// handle_runtime_message: any thread; tick and where: engine thread.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::studio::mesh {

bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Every frame in the studio's world: does what the editor asked (send: to the resource
// runtime); a mesh held in a joint goes with that joint of `local_entity`, the character.
void tick(void* local_entity, const std::function<bool(const kcd2s::sdk::Value&)>& send);

// The mesh shown: its place, turn (x, y, z, w) and the box it takes in the world. False while no
// mesh is shown.
bool where(float place[3], float turn[4], float min[3], float max[3]);

// The mesh shown (its entity), or null.
void* shown_entity();

// The material the mesh came with (objects/kcds_studio/<name>), empty while none is shown.
std::string own_material();

// One of your meshes' files: its .cgf and its .mtl (empty when it has none); false without the
// mesh (the export). Any thread.
bool mesh_files(const std::string& name, std::vector<std::uint8_t>& model,
                std::vector<std::uint8_t>& material);

// A mesh's textures from Blender (kcds_export.py: <name>_<material>.dds beside it): their file
// names and bytes, at most 16. Any thread.
std::vector<std::pair<std::string, std::vector<std::uint8_t>>> mesh_textures(
    const std::string& name);

}  // namespace kcds::runtime::studio::mesh
