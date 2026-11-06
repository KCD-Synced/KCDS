// Test probe (KCDS_TEST_HOOKS only): the navigation module's listener set that a
// crash found holding a listener already freed. Logs its size and each listener's
// vtable when they change, so a run shows whether removing NPC bodies leaves one behind.
#pragma once

namespace kcds::runtime::testing::nav_listeners {

// Hooks the navigation module's update to learn the module; false when its code is not found.
bool install();

}  // namespace kcds::runtime::testing::nav_listeners
