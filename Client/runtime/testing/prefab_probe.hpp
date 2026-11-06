// Test-only probe (KCDS_TEST_HOOKS, World Architect): spawns one of the game's prefab
// templates (a house) at run time the way the RuntimePrefab entity class does, and logs each step,
// so the native path for placing a whole house can be proven before it is built into the mod.
//
// The kit writes "<prefab GUID> <x> <y> <z> [RndFlags]" to %TEMP%\kcds-prefab-<pid>.txt and sets
// the named event Local\KCD2S.ProbePrefab.<pid>. Engine thread.
#pragma once

namespace kcds::runtime::testing::prefab_probe {

// Creates the probe's event; with KCDS_TEST_HOOKS only (called by install_test_hooks).
void install();

// Runs the probe once its event is set and the player is in the world.
void tick(bool in_world);

}  // namespace kcds::runtime::testing::prefab_probe
