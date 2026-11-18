// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "skin_binding_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "skin_binding.hpp"

namespace kcds::game_client {

namespace {

// CSKINAttachment's constructor: its vtable, a reference slot, a small array.
constexpr const char* kSkinAttachmentConstructor =
    "40 53 48 83 EC 20 BA 08 00 00 00 48 C7 41 08 00 00 00 00 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 "
    "89 01 4C 8D 0D ?? ?? ?? ?? 48 83 C1 10 44 8D 42 FE E8";

}  // namespace

void install_skin_binding() {
  kcds::runtime::appearance::skin_binding::Natives skin;
  // The character manager (qword_185189BE0), read by its accessor as animation/human_overlay
  // finds it.
  skin.character_manager = static_cast<void* const*>(resolve_unique(
      "SkinBindingCharacterManager",
      "48 8B 05 ?? ?? ?? ?? 48 05 C8 01 00 00 C3 CC CC 48 8B 81 A0 03 00 00 C3", 3, 7));
  skin.allocate =
      resolve_unique("SkinBindingAllocate",
                     "40 53 48 83 EC 20 83 3D ?? ?? ?? ?? 00 48 8B D9 75 05 E8 ?? ?? ?? ?? 48 8B "
                     "05 ?? ?? ?? ?? 48 8D 54 24 38 45 33 C0 FF D0");
  skin.skin_attachment = resolve_unique("SkinBindingAttachment", kSkinAttachmentConstructor);
  // The vtable the constructor puts in (its lea, the operand at + 22).
  skin.skin_object_table =
      resolve_unique("SkinBindingAttachmentTable", kSkinAttachmentConstructor, 22, 26);
  if (!skin.character_manager || !skin.allocate || !skin.skin_attachment) {
    log("SKIN binding functions missing: no skins or crowns can be worn");
  }
  if (!skin.skin_object_table) {
    log("SKIN binding: the skin attachment's table was not found, so no hair loses its crown");
  }
  kcds::runtime::appearance::skin_binding::install(skin);
}

}  // namespace kcds::game_client
