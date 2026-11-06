// Test-only probe (KCDS_TEST_HOOKS, World Architect terraforming): sets the terrain in a square to
// one height while the game runs and has the game rebuild it, so the native path for flattening
// ground under a placed building can be proven before it is built into the mod.
//
// The kit writes "<x> <y> <half size> <height>" to %TEMP%\kcds-terrain-<pid>.txt and sets the
// named event Local\KCD2S.ProbeTerrain.<pid>. Engine thread.
#pragma once

namespace kcds::runtime::testing::terrain_probe {

// Creates the probe's event; with KCDS_TEST_HOOKS only (called by install_test_hooks).
void install();

// Runs the probe once its event is set and the player is in the world.
void tick(bool in_world);

}  // namespace kcds::runtime::testing::terrain_probe
