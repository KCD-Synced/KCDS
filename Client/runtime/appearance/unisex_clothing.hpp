// Clothing and armour on female bodies.
//
// The game made its clothing and armour for one gender each, almost all of it male. An item
// class's clothing component says which (WHGame 1.5.5):
//
//   class + 208       the clothing component; its race at + 16 (valid when + 20 is set), its gender
//                     at + 24 (valid when + 25 is set: 1 male, 2 female)
//   class slot 59     the wear check: the soul may put the item on when its race is the
//                     class's and it takes the class's gender (soul slots 40 and 42)
//         the character's clothing manager adds a component to the body it draws only
//                     when the component's race and gender are its owner's (owner slots 6 and 7)
//   soul slot 41      the soul's gender (its archetype's)
//
// Both hooks let a female soul wear and draw a male piece: for the length of the original call the
// male component says female, then male again. Male bodies and female pieces are left alone. The
// piece is drawn as it was made, on the female body.
//
// Engine thread only.
#pragma once

namespace kcds::runtime::appearance::unisex_clothing {

// Hooks the wear check and the clothing manager's add. False when either could not be installed.
bool install(void* wear_check, void* add_component);

}  // namespace kcds::runtime::appearance::unisex_clothing
