// Winsock before Windows.h (see client_internal.hpp).
#include "client_internal.hpp"

#include "game_module.hpp"

#include <psapi.h>

#include <cstdint>

#include "../branding/runtime_branding.hpp"
#include "client_state.hpp"
#include "log.hpp"
#include "natives.hpp"
#include "signature_store.hpp"

namespace kcds::game_client {

DWORD find_game_module() {
  HMODULE game = nullptr;
  for (unsigned i = 0; i < 1200 && !game; ++i) {
    game = GetModuleHandleW(L"WHGame.dll");
    if (!game)
      Sleep(50);
  }
  if (!game) {
    log("WHGame.dll not loaded");
    return 1;
  }
  kcds::runtime::branding::ensure_installed();
  MODULEINFO mi{};
  if (!GetModuleInformation(GetCurrentProcess(), game, &mi, sizeof(mi)))
    return 1;
  image = static_cast<std::uint8_t*>(mi.lpBaseOfDll);
  image_size = mi.SizeOfImage;
  open_signature_store();
  return 0;
}

}  // namespace kcds::game_client
