// An animation's events in the studio: at a key's time the studio plays a particle
// effect of the game's catalogue at a joint of the character, or one of the game's sound triggers
// on the character, while the animation plays in the editor.
//
//   { op: 'studioEvent', kind: 'effect', name: 'Library.group.name', joint, scale }
//       a burst of the effect at the joint's place (the body's place for none) for two seconds,
//       as the client's Vfx.burst plays one (vfx/placed_effects.hpp)
//   { op: 'studioEvent', kind: 'sound', name }
//       the trigger on the character's entity, as the client's Audio.playFrom
//       (crafting/sounds.hpp play)
//
// handle_runtime_message: any thread; tick: engine thread.
#pragma once

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::studio::events {

bool handle_runtime_message(const kcd2s::sdk::Value& message);

void tick(void* local_entity);

}  // namespace kcds::runtime::studio::events
