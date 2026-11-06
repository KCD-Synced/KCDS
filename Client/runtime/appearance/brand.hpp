// A brand on a body's face, as the game's own punishment shows it. C_BrandingBuff
// (WHGame 1.5.5: its state 0 none, 1 and 2 the two marks) asks the actor's human
// (actor + 1672; its vtable + 248) to show a variant of the character part "head_face". This module
// asks the same on any body.
//
// Engine thread.
#pragma once

#include <cstdint>

namespace kcds::runtime::appearance::brand {

using Logger = void (*)(const char* format, ...);

// False when the game's function was not found: bodies then show no brand.
bool install(void* branding_state, Logger logger);

// Shows state 0 (none), 1 or 2 on the body; false when it cannot now (no human yet).
bool show(void* actor, std::uint8_t state);

}  // namespace kcds::runtime::appearance::brand
