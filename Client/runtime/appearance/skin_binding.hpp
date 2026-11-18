// Skinned meshes worn on a body the game's own way, as its clothing puts on a piece: the character
// manager's LoadModelSKIN (ICharacterManager slot 6), a CSKINAttachment (64 bytes of the game's
// allocator, its constructor) holding the attachment's skin interface (IAttachment slot 29) with a
// reference, and IAttachment AddBinding (slot 26: object, skin, flags 2048). An attachment of our
// own is made once with IAttachmentManager CreateAttachment (slot 2, name and type 2 for a skin,
// as the attachment list loader does) and found again by name (slot 8); taking a skin off is its
// ClearBinding (slot 30).
//
// A bound skin's attachment object (IAttachment slot 28) draws each of its six levels of detail
// with a replacement material when one is set (CSKINAttachment slot 15, set; slot 14, the one
// drawn: the replacement, else the skin's own).
//
// Engine thread.
#pragma once

#include <cstdint>
#include <string>

namespace kcds::runtime::appearance::skin_binding {

struct Natives {
  void* const* character_manager = nullptr;  // the game's character manager
  void* allocate = nullptr;                  // the game's allocator: memory of `size` bytes
  void* skin_attachment = nullptr;           // CSKINAttachment constructor
  void* skin_object_table = nullptr;         // CSKINAttachment's vtable, which it puts in
};

// The levels of detail a skin attachment object draws.
inline constexpr int kLods = 6;

void install(const Natives& natives);
bool installed();

// The attachment manager of the body's character (entity slot 0), or null.
void* attachment_manager(void* entity);

// The body's skin attachment named `name`, made the first time; null when it cannot be made.
void* own_attachment(void* manager, const char* name);

// The body's attachment named `name`, or null when it has none.
void* named_attachment(void* manager, const char* name);

// The manager's attachment number `index`, or null past the last one.
void* attachment_at(void* manager, std::uint32_t index);

// The attachment object bound to an attachment (a CSKINAttachment for a skin), or null.
void* bound_object(void* attachment);

// Whether an attachment object is a skin's (a CSKINAttachment): only those draw with a replacement
// material.
bool skin_object(void* object);

// The game's skin at `path` (loaded once by the character manager, which keeps it), or null.
void* load_skin(const std::string& path);

// Puts the skin on the attachment; false when the game refused it.
bool bind(void* attachment, void* skin);

// Takes the attachment's skin off.
void clear(void* attachment);

// The material an attachment object draws level `lod` with (its replacement, else the skin's
// own), or null.
void* drawn_material(void* object, int lod);

// The replacement material level `lod` is drawn with (the object's own field), or null when it
// draws the skin's own.
void* replacement(void* object, int lod);

// Draws level `lod` with `material`, which the object takes a reference to; null goes back to the
// skin's own.
void replace_material(void* object, int lod, void* material);

}  // namespace kcds::runtime::appearance::skin_binding
