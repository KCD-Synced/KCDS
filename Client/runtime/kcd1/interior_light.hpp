// The light inside the KCD1 map's buildings. The game lights a room with global illumination from
// voxels of the level's geometry, which a KCD2 level ships precomputed (its svo.pak, read from
// disk). KCD1 ships its own in an older format the game cannot read, so the KCD1 map had none and
// its rooms were lit flat. On the KCD1 map the game builds the voxels itself from the level's
// geometry, as CryEngine can: e_svoStreamVoxels 0 (voxelize, do not stream from disk),
// e_svoTI_UpdateGeometry 1 and e_svoLoadTree 1 (start building). Leaving for a KCD2 map puts the
// game's own values back.
//
// Engine thread.
#pragma once

#include <string>

namespace kcds::runtime::kcd1::interior_light {

// The world `map` is ready: switches the game's voxel global illumination to building it from
// the level on the KCD1 map, and back on any other.
void apply(const std::string& map);

}  // namespace kcds::runtime::kcd1::interior_light
