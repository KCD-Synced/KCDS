// The collision a game model's file holds, for a placed prop whose model the game loads without
// it (WHGame 1.5.5).
//
// The larger buildings and structures (an oven, a shed, a well) are files of several root meshes:
// a shadow mesh and merged meshes, with their physics proxies under the merged ones. The game
// loads them with no physics geometry at all (the model object's list at + 296 stays empty) and
// the level stands separate colliders beside them, so a prop of such a model had no collision.
//
// The proxies are in the model's .cgf (CrCh 0x746): a mesh chunk (0x1000) names its physics data
// chunk (0x1018) at + 92; the data after the chunk's 24-byte head is what the game's own model
// loader hands its geometry manager. KCDS reads the file through the game's ICryPak (+ 288
// FOpen, + 304 FReadRaw, + 368 FGetSize, + 440 FClose), loads each proxy with the geometry
// manager's LoadPhysGeometry (gEnv->pPhysicalWorld slot 4 the manager, its slot 14; a stream with
// its buffer at + 8 and the read place at + 20, no vertices or indices of the caller) and adds it
// to the prop's body with AddGeometry (IPhysicalEntity slot 8; pe_geomparams: position + 12,
// rotation + 24, scale + 40, surface + 64, flags + 68, collider flags + 72, minimum contact
// distance + 76, breakable material + 80, bounds again + 108), at its node's place in the model.
//
// Engine thread.
#pragma once

#include <string>

namespace kcds::runtime::props::model_collision {

using Logger = void (*)(const char* format, ...);

// physical_world: the global holding gEnv->pPhysicalWorld.
void install(void* const* physical_world, Logger logger);

// Adds the collision of `model` (a mesh path as the prop has it) to `body` (the prop's static
// IPhysicalEntity, at the prop's own scale `scale`). The number of proxies added; 0 when the file
// has none or something is missing.
int add(void* body, const std::string& model, float scale);

}  // namespace kcds::runtime::props::model_collision
