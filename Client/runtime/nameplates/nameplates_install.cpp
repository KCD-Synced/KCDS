// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "nameplates_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "presented_frame.hpp"

namespace kcds::game_client {

void install_nameplates() {
  // The engine and render threads' sync: the renderer's global, then the frame number it hands the
  // render thread written at renderer + 0x29C0, and the renderer's own call after it.
  auto* renderer = static_cast<void* const*>(resolve_unique(
      "PresentedFrameRenderer",
      "48 8B 0D ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 89 81 C0 29 00 00 48 8B 01 FF 90 10 0A 00 00", 3, 7));
  kcds::runtime::nameplates::presented_frame::install(renderer);
  log("NAMEPLATES presented frame %s", renderer ? "found" : "unavailable");
}

}  // namespace kcds::game_client
