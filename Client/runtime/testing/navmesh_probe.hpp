// Test-only probe (KCDS_TEST_HOOKS, World Architect's custom-map navmesh, issue list C4): logs
// whether the game loads a level's navmesh and from which file, so a custom map's can be compared
// with the game's own levels before the mod writes navmeshes:
//
//   NAVPROBE agent load        C_Navigation's load of one agent navigation
//   NAVPROBE index <path> ok   the tile index (.hdr) read, with its path and result
//   NAVPROBE tiles ok          the main .nav read and its tiles added
#pragma once

namespace kcds::runtime::testing::navmesh_probe {

// Hooks the three functions; with KCDS_TEST_HOOKS only (called by install_test_hooks).
void install();

}  // namespace kcds::runtime::testing::navmesh_probe
