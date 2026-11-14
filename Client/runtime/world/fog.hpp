// Fog over the weather's own (World.setFog): the game's volumetric fog made thicker everywhere,
// up to a few metres of sight.
//
// Every frame the time of day updates, it hands the 3D engine its fog through the engine's
// SetGlobalParameter (vtable slot 224, + 0x700). KCDS hooks that call: with a fog strength above
// 0 the fog's extinction, density and ramp it passes on are at least the strength's, so a thicker
// weather fog still shows, and the time of day's own values come back as soon as the strength is
// 0 again. (Warhorse's console variable wh_r_VolumetricFogGlobalDensityOverride is registered but
// read by nothing.)
//
// Engine thread.
#pragma once

namespace kcds::runtime::world::fog {

// The fog from now on (0 to 1; anything else is taken as 0).
void want(float strength);

// Hooks the 3D engine's SetGlobalParameter the first time it is there (engine: the engine
// object, null while there is none), and gives it a new strength at once. Every frame in a world.
void frame(void* engine);

}  // namespace kcds::runtime::world::fog
