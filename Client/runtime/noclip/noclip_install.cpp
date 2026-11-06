// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "noclip_install.hpp"

#include "../camera/camera_shot.hpp"
#include "../camera/free_look.hpp"
#include "../render/render_view.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../chat/chat_overlay.hpp"
#include "free_camera.hpp"

namespace kcds::game_client {

namespace native_chat = kcd2s::native_chat;

void install_noclip() {
  // NoClip: the free camera, a view of the engine's own view system.
  kcds::runtime::noclip::Natives natives;
  natives.framework = [] { return framework_slot ? *framework_slot : nullptr; };
  natives.services = get_services;
  natives.window = native_chat::window;
  natives.entity_system = []() -> void* {
    void* framework = framework_slot ? *framework_slot : nullptr;
    auto actors = framework
                      ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + 1304)
                      : nullptr;
    return actors ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actors) + 48) : nullptr;
  };
  natives.spawn_params_init = resolve_unique(
      "EntitySpawnParams", "48 89 5C 24 08 57 48 83 EC 20 33 FF 48 8D 05 ?? ?? ?? ?? 48 89 41 28");
  natives.physics_component = resolve_unique(
      "EntityPhysicsComponent",
      "48 89 5C 24 10 55 56 57 41 56 41 57 48 8B EC 48 83 EC 40 48 8D 99 A8 00 00 00");
  natives.local_entity = []() -> void* {
    auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
    return actor ? *reinterpret_cast<void**>(actor + 56) : nullptr;
  };
  const bool noclip = kcds::runtime::noclip::install(natives, log);
  log("NOCLIP %s", noclip ? "installed" : "unavailable");
  // The server's camera shots: a view of the same view system, from the same
  // framework, entity system and spawn parameters.
  kcds::runtime::camera::ShotNatives shots;
  shots.view.framework = natives.framework;
  shots.view.entity_system = natives.entity_system;
  shots.view.spawn_params_init = natives.spawn_params_init;
  shots.send_ended = [](kcd2s::integration::CameraShotEnd reason) {
    std::lock_guard<std::mutex> lock(network_mutex);
    return client && client->peer().send_camera_shot_ended(reason);
  };
  kcds::runtime::camera::install_shots(shots);
  // Free look (playAnimation lockRotation): a view of the same view system at the eyes.
  kcds::runtime::camera::free_look::FreeLookNatives free_look;
  free_look.view = shots.view;
  free_look.services = get_services;
  free_look.window = native_chat::window;
  free_look.mouse_free = [] {
    return !ui_owns_input() && !page_over_world.load() && !game_menu_open();
  };
  kcds::runtime::camera::free_look::install(free_look, log);
  // Render.view's camera is a view of the same view system (render).
  kcds::runtime::render::install(shots.view, log);
  log("CAMERA SHOTS %s", shots.view.spawn_params_init ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
