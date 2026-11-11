#include "view_camera.hpp"

#include "../core/client_internal.hpp"
#include "../core/natives.hpp"
#include "../hud/resource_hud.hpp"
#include "../web/resource_web.hpp"

#include <algorithm>

namespace kcds::game_client {

// Snapshots the view camera for Hud.worldToScreen: CXConsole + 0x200 is the ISystem.
// The same camera places the web views anchored to world points (Web.setViewWorldPosition).
void clear_view_camera() {
  kcds::runtime::hud::clear_camera();
  kcds::runtime::web::set_view_camera(false, nullptr, 0);
}

void read_view_camera(bool in_world) {
  view_matrix_valid = false;
  if (!in_world || !view_camera_verified.load() || !console_slot || !*console_slot) {
    clear_view_camera();
    return;
  }
  void* system = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(*console_slot) + 0x200);
  if (!system) {
    clear_view_camera();
    return;
  }
  using GetViewCamera = const float*(__fastcall*)(void*);
  auto camera = slot<GetViewCamera>(system, 0x438 / 8)(system);
  if (!camera) {
    clear_view_camera();
    return;
  }
  kcds::runtime::hud::set_camera(camera, camera[12]);
  // Views placed in the world (anchored to a point, or on a screen there) are part of the world:
  // not drawn over the game's menus and screens (Escape, the inventory, the map).
  kcds::runtime::web::set_view_camera(!game_menu_open(), camera, camera[12]);
  std::copy(camera, camera + 12, view_matrix.begin());
  view_fov = camera[12];
  view_matrix_valid = true;
}

}  // namespace kcds::game_client
