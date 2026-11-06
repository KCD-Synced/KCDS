// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "lights_tick.hpp"

#include <vector>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../vfx/placed_objects_tick.hpp"
#include "lights.hpp"

namespace kcds::game_client {

void install_lights() {
  kcds::runtime::lights::Natives natives;
  // CDLight's constructor and the entity script binding LoadLight's own load into a
  // slot.
  natives.make_light = resolve_unique("LightConstructor",
                                      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 "
                                      "33 ED C7 41 08 FF FF FF FF 48 89 29 BE 00 00 80 3F");
  natives.load_light = resolve_unique("EntityLoadLight",
                                      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 8B F2 49 8B F8 "
                                      "33 D2 48 8B D9 E8 ?? ?? ?? ?? 4C 8D 48 B0 48 F7 D8 48 1B C0 "
                                      "49 85 C1 74");
  natives.log = log;
  // A light on a body: the body's entity, and AttachChild as the held props resolve
  // it.
  natives.body_of = &placed_body_entity;
  natives.attach_child = resolve_unique(
      "EntityAttachChild",
      "48 89 5C 24 20 55 56 57 48 8D 6C 24 B9 48 81 EC F0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 "
      "C4 48 89 45 37 49 8B");
  log("LIGHTS %s", kcds::runtime::lights::install(natives) ? "installed" : "unavailable");
}

void tick_lights() {
  std::vector<kcd2s::integration::LightOrder> orders;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    orders = client->peer().take_light_orders();
  }
  for (const auto& order : orders) {
    kcds::runtime::lights::apply(order);
  }
  kcds::runtime::lights::tick();
}

}  // namespace kcds::game_client
