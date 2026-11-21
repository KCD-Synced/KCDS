// The light of a lantern carried on a body (Player.setLantern): the game's own lantern light.
//
// The game's NPCs carry their lamp tool lit with the light archetype Standard.Light_Lantern
// (Libs/EntityArchetypes/Lights.xml): a warm orange of diffuse multiplier 0.003, a bulb of 0.35 m,
// a radius of 6 m and the lantern's flicker (light style 49 at speed 0.35). A player's lantern was
// lit with a light of KCDS's own on the hand's joint, intensity 2 with a 0.1 m bulb: so near the
// body it flooded the face and clothes white. Every game now hangs a Light entity with the
// archetype's values on the lamp itself, when the server hangs a wearable lamp's model on a body.
// Its projector (a 175 degree cone of the lantern's pattern in the lamp's own facing) is left out:
// the lamp hangs turned otherwise than the game's own, and the cone left half the scene dark.
//
// Engine thread.
#pragma once

#include <cstdint>
#include <string>

namespace kcds::runtime::lanterns::light {

// Whether a model is one of the game's carried lamps (lamp_*_wearable*.cgf).
bool is_lantern(const std::string& model);

// Lights the lamp `lamp` (the entity showing prop `id`): the light hangs on it as its child.
// `attach_child` is CEntity::AttachChild. False when the light could not be made.
bool light(std::uint32_t id, void* lamp, void* attach_child);

// Puts the light of prop `id` out (it goes with its lamp); nothing when it has none.
void put_out(std::uint32_t id);

}  // namespace kcds::runtime::lanterns::light
