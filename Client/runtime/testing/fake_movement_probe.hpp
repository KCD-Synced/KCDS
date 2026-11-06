// Test probe for issue 016 (KCDS_TEST_HOOKS only): who hands an NPC to the game's fake movement.
// The simulator's NPC bodies walked in jumps a second apart, without their walk, even 10 m in
// front of the camera: C_FakeMovementManager moved them. Its request (vtable
// slot 0, called as calls it) is hooked and each request is logged with the entity and
// the game's stack, so a run names the path that chose fake movement.
#pragma once

namespace kcds::runtime::testing::fake_movement {

// Hooks the manager's request; false when its class is not found.
bool install();

}  // namespace kcds::runtime::testing::fake_movement
