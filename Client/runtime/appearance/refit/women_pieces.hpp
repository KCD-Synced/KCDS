// Men's clothing and armour fitted to a woman's body when she wears them (body_fit.hpp is the
// math), WHGame 1.5.5.
//
// A piece is a clothing component (C_Clothing, clothing@entitymodule@wh): its file path (a CryString
// at + 32, under Objects/characters/), its gender (+ 24, set when + 25: 1 male) and its elements (a
// vector of shared_ptr at + 80). A skin element (C_SkinElement, 56 bytes) holds its equipment part
// (+ 8: 2 torso, 3 arms, 4 hands, 5 legs, 6 feet, 11 waist, 12 belt), its model and material (each
// an optional CryString: the string at + 24 / + 40, set when + 32 / + 48 is). The game draws the
// file "Objects/characters/" + file path + model.
//
// For a woman, a man's piece is put on as a copy of its own (the component's clone, vtable slot 1)
// whose body-part skins are copies of the originals' elements with a model of their own beside the
// original file: kcds_female_<name>.skin, the original moved onto her body. Those files are written
// once as small packs in %TEMP%\kcds-characters\refit (kept for the next start) and mounted as the
// female skeleton's pack is. A head piece's skin is not refitted but has the joints her skeleton
// lacks folded away (joint_fold.hpp): some men's helmets with such joints were not drawn on a
// woman at all. The copies are kept for the rest of the game; a man still wears the piece itself.
// A piece whose skin cannot be read or changed stays as it is.
//
// Engine thread only.
#pragma once

#include <cstddef>
#include <cstdint>

namespace kcds::runtime::appearance::refit::pieces {

using Logger = void (*)(const char* format, ...);

struct Natives {
  const std::uint8_t* image = nullptr;  // WHGame.dll as loaded, for the classes' vtables (RTTI)
  std::size_t image_size = 0;
  void* (*allocate)(std::size_t size) = nullptr;   // the game's allocator
  void (*string_init)(void* string) = nullptr;     // an empty CryString in place
  void* (*string_assign)(void* string, const char* text) = nullptr;
};

// False when a class's vtable or a native is missing: women then wear the pieces as made.
bool install(const Natives& natives, Logger logger);

// The game's CryPak and the binding root the mod packs are opened with, once it has them.
void set_pak(void* pak, const char* binding_root);

// For the clothing add of a woman's body: the shared_ptr (two pointers) of the fitted copy of the
// component `component` points to, or null to add it as it is.
void* const* fitted_for_woman(void* const* component);

}  // namespace kcds::runtime::appearance::refit::pieces
