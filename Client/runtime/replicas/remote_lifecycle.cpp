// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "remote_lifecycle.hpp"
#include "remote_interactor_lifetime.hpp"
#include "remote_ui_role.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "replica_spawn.hpp"

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace kcds::game_client {

// C_AIObject owns its transform provider at +24. Channel-0 Player
// construction can leave transform subscribers alive after that provider dies.
// Cancel each native registration before its native destructor clears the owner.
AIObjectCtor original_ai_object_ctor;
AIObjectDtor original_ai_object_dtor;
CancelTransform cancel_transform;
void** transform_manager_slot;
// The AI module (its global) keeps every AIObject in a spatial grid at + 112
// (C_RegularGridDynamic<C_AIObject*>), which the module's update walks each frame for the actors
// around the player (->). A replica's AIObject is taken out of it before it
// is destroyed (safe for one not in the grid): the game's own despawn does that, our
// replica removal did not, and the next frame read the freed object.
constexpr std::size_t kAIModuleGrid = 112;
std::uint8_t** ai_module_slot = nullptr;
AIGridRemove ai_grid_remove = nullptr;
std::mutex ai_lifetime_mutex;
std::unordered_map<void*, void*> remote_ai_objects;
void* __fastcall hooked_ai_object_ctor(void* self, const void* id, void* owner) {
  auto result = original_ai_object_ctor(self, id, owner);
  if (spawning_remote && result) {
    std::lock_guard<std::mutex> lock(ai_lifetime_mutex);
    remote_ai_objects[result] = owner;
    log("REMOTE native AIObject=%p transform owner=%p", result, owner);
  }
  return result;
}
std::int64_t __fastcall hooked_ai_object_dtor(void* self) {
  bool remote = false;
  {
    std::lock_guard<std::mutex> lock(ai_lifetime_mutex);
    remote = remote_ai_objects.erase(self) != 0;
  }
  if (remote && transform_manager_slot)
    if (auto manager = static_cast<std::uint8_t*>(*transform_manager_slot)) {
      struct Registration {
        int kind;
        std::uint32_t reserved{};
        std::uint8_t callback[32];
      };
      std::vector<Registration> registrations;
      // Native sorted owner entries are 32 bytes; subscriber records are 56 bytes.
      // Snapshot before invoking removal because the engine compacts its vectors.
      for (unsigned offset : {8u, 32u}) {
        auto entries = *reinterpret_cast<std::uint8_t**>(manager + offset);
        auto count = *reinterpret_cast<std::size_t*>(manager + offset + 8);
        for (std::size_t i = 0; i < count; ++i) {
          auto entry = entries + i * 32;
          if (*reinterpret_cast<void**>(entry) != self)
            continue;
          auto begin = *reinterpret_cast<std::uint8_t**>(entry + 8);
          auto end = *reinterpret_cast<std::uint8_t**>(entry + 16);
          for (auto record = begin; record != end; record += 56) {
            Registration r;
            r.kind = *reinterpret_cast<int*>(record);
            std::memcpy(r.callback, record + 8, 32);
            registrations.push_back(r);
          }
        }
      }
      for (const auto& registration : registrations)
        cancel_transform(manager, self, registration.kind, registration.callback);
      log("REMOTE native transform cleanup object=%p subscriptions=%zu updating=%u", self,
          registrations.size(), unsigned(manager[56]));
    }
  void* grid = nullptr;
  if (remote && ai_module_slot && ai_grid_remove) {
    std::uint8_t* module = *ai_module_slot;
    grid = module ? *reinterpret_cast<void**>(module + kAIModuleGrid) : nullptr;
    if (grid) {
      ai_grid_remove(grid, self);
      log("REMOTE native AIObject=%p left the AI grid", self);
    }
  }
  const std::int64_t result = original_ai_object_dtor(self);
  // The destructor's own work can move the object once more, and the grid's update puts a moved
  // object back into its new cell: taken out again after it, or the grid kept the destroyed
  // object and a walk of it (a time skip's search for smart objects) called into it. The removal
  // reads only the grid and the object's address, so it is safe after the destructor too.
  if (grid) {
    ai_grid_remove(grid, self);
  }
  return result;
}

PlayerCtor original_player_ctor;
void* actor_hit;
void* actor_deactivate;
void* actor_update;
void* actor_tick;
void* actor_reset;
void* actor_init;
void* actor_post_init;
void* actor_release;
void* actor_event;
void* actor_activate;
// shows 289 primary C_Player entries before the next RTTI locator.
// Keep C_Player's RTTI and body factory; only remote lifecycle uses the shared
// C_Actor implementations instead of singleton UI/input registration.
void* remote_player_table[290]{};

namespace {

void* construct_player_with_ui_scope(void* self) {
  const auto* previous =
      kcds::runtime::replicas::begin_remote_player_construction(self, spawning_remote);
  void* player = nullptr;
  __try {
    player = original_player_ctor(self);
  } __finally {
    // The engine adapter catches native SEH outside this call. Restore the previous TLS scope
    // on that unwind too; /EHsc alone would leave a C++ RAII guard active after an engine fault.
    kcds::runtime::replicas::end_remote_player_construction(previous);
  }
  return player;
}

}  // namespace

void* __fastcall hooked_player_ctor(void* self) {
  auto player = construct_player_with_ui_scope(self);
  if (spawning_remote && player) {
    auto original = *reinterpret_cast<void***>(player);
    if (!remote_player_table[1]) {
      std::memcpy(remote_player_table, original - 1, sizeof(remote_player_table));
      remote_player_table[1 + 1] = actor_event;
      remote_player_table[1 + 7] = actor_init;
      remote_player_table[1 + 8] = actor_post_init;
      remote_player_table[1 + 14] = remote_interactor_release_target();
      remote_player_table[1 + 187] = actor_activate;
      remote_player_table[1 + 188] = actor_deactivate;
      remote_player_table[1 + 225] = actor_hit;
      remote_player_table[1 + 154] = actor_reset;
      remote_player_table[1 + 217] = actor_tick;
      remote_player_table[1 + 22] = actor_update;
    }
    *reinterpret_cast<void***>(player) = remote_player_table + 1;
    log("REMOTE C_Player role installed body=%p; local lifecycle unchanged", player);
  }
  return player;
}

}  // namespace kcds::game_client
