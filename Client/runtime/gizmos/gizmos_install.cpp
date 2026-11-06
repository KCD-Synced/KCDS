// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "gizmos_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "gizmos.hpp"

namespace kcds::game_client {

void install_gizmos() {
  // The server's gizmos: the renderer's aux geometry as CScriptBind_System::DrawLine
  // reaches it, gEnv->pRenderer + 1592, then SetRenderFlags + 8 and DrawLine
  // + 64; and the label call CScriptBind_System::DrawLabel makes.
  kcds::runtime::gizmos::Natives gizmo_natives;
  gizmo_natives.renderer = static_cast<void* const*>(resolve_unique(
      "AuxGeomRenderer",
      "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 38 06 00 00 4C 8D 45 E8 48 C7 45 E8 00 00 00 00 BA "
      "01 00 00 00",
      3, 7));
  gizmo_natives.layout_verified =
      resolve_unique(
          "AuxGeomDrawLine",
          "48 8B 03 FF 50 08 F3 0F 10 05 ?? ?? ?? ?? 48 8D 4D 28 48 8B 03 4C 8D 4D D8 "
          "F3 0F 11 44 24 28 4C 8D 45 28 48 89 4C 24 20 48 8D 55 E8 48 8B CB FF 50 40") != nullptr;
  gizmo_natives.draw_label = resolve_unique(
      "DrawLabelEx",
      "48 83 EC 48 8B 41 08 48 8D 54 24 50 F2 0F 10 01 48 8D 4C 24 30 45 0F B6 C9 41 C1 E1 05 "
      "89 44 24 38 41 83 C9 05");
  log("GIZMOS %s",
      kcds::runtime::gizmos::install(gizmo_natives, log) ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
