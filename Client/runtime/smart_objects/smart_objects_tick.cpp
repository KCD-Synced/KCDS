// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "smart_objects_tick.hpp"

#include <mutex>
#include <vector>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "placed_smart_objects.hpp"

namespace kcds::game_client {

namespace {

// The script binding Game.SpawnPrefab's body: the prefab system's global (rel32 at +
// 71, the mov ending at + 75) and the call of the prefab manager's spawn (rel32 at + 110, ending
// at + 114).
constexpr const char* kSpawnPrefabBinding =
    "48 89 5C 24 08 57 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 40 48 8B FA 49 8B "
    "C9 48 8D 54 24 30 49 8B D8 E8 ?? ?? ?? ?? 84 C0 74 42 48 8B 0D ?? ?? ?? ?? 8B D3 48 8B 01 "
    "FF 50 70 48 85 C0 74 2E 48 8B 0D ?? ?? ?? ?? 48 8D 54 24 30 8A 84 24 88 00 00 00 44 8B C3 "
    "44 8B 8C 24 80 00 00 00 88 44 24 20 48 8B 89 50 01 00 00 E8";
// A prefab instance taken down: its child instances, entities and brushes.
constexpr const char* kClearInstance =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 48 8B 59 70 48 8B "
    "F1 48 3B 5E 78 74 13 48 8B 0B 48 85 C9 74 05 E8";

bool tried = false;

void install_once(void* entity_system) {
  if (tried || !entity_system) {
    return;
  }
  tried = true;
  kcds::runtime::smart_objects::Natives natives;
  natives.spawn_prefab = resolve_unique("PrefabSpawn", kSpawnPrefabBinding, 110, 114);
  natives.prefab_system =
      static_cast<void* const*>(resolve_unique("PrefabSystem", kSpawnPrefabBinding, 71, 75));
  natives.clear_instance = resolve_unique("PrefabInstanceClear", kClearInstance);
  natives.entity_system = entity_system;
  log("SMARTOBJECT %s",
      kcds::runtime::smart_objects::install(natives, log) ? "installed" : "unavailable");
}

}  // namespace

void tick_smart_objects(const Frame& frame) {
  std::vector<kcd2s::integration::SmartObjectOrder> orders;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (client) {
      orders = client->peer().take_smart_objects();
    }
  }
  if (orders.empty()) {
    return;
  }
  install_once(frame.entity_system);
  kcds::runtime::smart_objects::apply(orders);
}

}  // namespace kcds::game_client
