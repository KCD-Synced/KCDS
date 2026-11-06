// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "studio_skin_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "studio_skin.hpp"

namespace kcds::game_client {

void install_studio_skin() {
  kcds::runtime::studio::skin::Natives skin;
  // The character manager (qword_185189BE0), read by its accessor as animation/human_overlay
  // finds it.
  skin.character_manager = static_cast<void* const*>(resolve_unique(
      "StudioSkinCharacterManager",
      "48 8B 05 ?? ?? ?? ?? 48 05 C8 01 00 00 C3 CC CC 48 8B 81 A0 03 00 00 C3", 3, 7));
  skin.allocate =
      resolve_unique("StudioSkinAllocate",
                     "40 53 48 83 EC 20 83 3D ?? ?? ?? ?? 00 48 8B D9 75 05 E8 ?? ?? ?? ?? 48 8B "
                     "05 ?? ?? ?? ?? 48 8D 54 24 38 45 33 C0 FF D0");
  // CSKINAttachment's constructor: its vtable, a reference slot, a small array.
  skin.skin_attachment = resolve_unique(
      "StudioSkinAttachment",
      "40 53 48 83 EC 20 BA 08 00 00 00 48 C7 41 08 00 00 00 00 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 "
      "89 01 4C 8D 0D ?? ?? ?? ?? 48 83 C1 10 44 8D 42 FE E8");
  if (!skin.character_manager || !skin.allocate || !skin.skin_attachment) {
    log("STUDIO skin functions missing: skins cannot be worn in the studio");
  }
  kcds::runtime::studio::skin::install(skin);
}

}  // namespace kcds::game_client
