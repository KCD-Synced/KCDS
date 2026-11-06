// The pace rule a server puts on the local player
// (Player.setPace).
//
// Walking enforced holds the engine's own run and sprint permissions: the actor's vtable slot 222
// (the script binding's SetMovementRestriction(bAllowSprint, bAllowRun), which the
// actor gets as (allowRun, allowSprint)) keeps them at actor + 1034 (run) and + 1033 (sprint),
// passes them to the actor's AI movement ability and steps a pace the player may no longer keep
// down (sprint to run, run to walk). The game clears the same pair itself while the player carries
// a body.
//
// While the rule holds, the setter is hooked: whatever the game asks for (the end of carrying a
// body, a scripted restriction) is remembered and the permissions stay off; the game writing the
// bytes itself is caught the next frame. When the rule is lifted, the game gets back what it last
// asked for, through the same setter.
//
// Engine thread, except the hook (the thread the game sets the restriction on).
#pragma once

namespace kcds::runtime::movement {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* restriction = nullptr;  // the actor's movement restriction, hooked
};

// False when the native is missing or the hook fails.
bool install(const Natives& natives, Logger logger);

// Every frame: the local actor (nullptr outside a server world) and whether the server forbids it
// running and sprinting.
void tick(void* actor, bool walk_enforced);

}  // namespace kcds::runtime::movement
