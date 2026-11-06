// Winsock before Windows.h (see client_internal.hpp).
#include "client_internal.hpp"

#include "engine_natives.hpp"

#include <cstdint>
#include <cstring>

#include "client_state.hpp"
#include "engine_loop.hpp"
#include "log.hpp"
#include "natives.hpp"

namespace kcds::game_client {

void resolve_engine_natives() {
  update_target = resolve_unique(
      "CCryAction::Update",
      "48 8B C4 4C 89 40 18 48 89 50 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 40 41 8B E9");
  framework_slot = static_cast<void**>(resolve_unique(
      "gFramework", "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 68 04 00 00 48 85 C0 74 66 0F B7 55", 3,
      7));
  map_callback = reinterpret_cast<MapCallback>(resolve_unique(
      "MapCallback",
      "4C 8B DC 49 89 5B 10 49 89 73 18 57 41 54 41 55 41 56 41 57 48 81 EC D0 01 00 00"));
  // Signature of the native NewGameDebug std::function invoke, whose CALL resolves the services
  // getter.
  get_services = reinterpret_cast<ServicesGetter>(
      resolve_unique("GameServicesGetter",
                     "40 53 48 83 EC 20 48 8D 59 08 48 8B 83 90 00 00 00 C6 40 72 00 E8 ?? ?? ?? "
                     "?? 48 8B D3 48 8B 88 D0 00 00 00",
                     22, 26));
  // The failed-load path's main-menu return: services, mov rcx,[rax+0E8h],
  // GetLoadingScreen, mov rcx,rax, GoToMainMenu.
  constexpr const char* kMainMenuReturnSignature =
      "E8 ?? ?? ?? ?? 48 8B 88 E8 00 00 00 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B CB E8";
  native_loading_screen = reinterpret_cast<GetLoadingScreen>(
      resolve_unique("GetLoadingScreen", kMainMenuReturnSignature, 13, 17));
  native_go_to_main_menu = reinterpret_cast<GoToMainMenu>(
      resolve_unique("GoToMainMenu", kMainMenuReturnSignature, 21, 25));
  log("MENU main-menu return %s", native_go_to_main_menu ? "resolved" : "unavailable");
  params_init = reinterpret_cast<ParamsInit>(
      resolve_unique("NewGameParamsInit",
                     "40 53 48 83 EC 20 33 C0 48 8B D9 88 01 48 89 41 40 48 89 81 80 00 00 00 E8 "
                     "?? ?? ?? ?? 48 83 C0 0C 48 89 83 88 00 00 00"));
  string_assign = reinterpret_cast<StringAssign>(
      resolve_unique("CryStringAssign",
                     "40 53 48 83 EC 20 48 8B D9 4C 8B CA 48 8B CA E8 ?? ?? ?? ?? 4C 8B C0 49 8B "
                     "D1 48 8B CB E8 ?? ?? ?? ?? 48 8B C3 48 83 C4 20 5B C3"));
  // Map callback's console reader at +0x2d, verified in.
  if (map_callback) {
    auto reader = reinterpret_cast<std::uint8_t*>(map_callback) + 0x2d;
    if (reader[0] == 0x4c && reader[1] == 0x8b && reader[2] == 0x25) {
      std::int32_t displacement;
      std::memcpy(&displacement, reader + 3, 4);
      console_slot = reinterpret_cast<void**>(reader + 7 + displacement);
    }
  }
}

}  // namespace kcds::game_client
