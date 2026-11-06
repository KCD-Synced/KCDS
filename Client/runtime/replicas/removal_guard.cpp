#include "removal_guard.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "MinHook.h"

namespace kcds::runtime::replicas {
namespace {

using QueueCombatAnimation = std::uintptr_t(__fastcall*)(void* holder, void** animation,
                                                         float blend);
using ReleaseAnimation = std::uintptr_t(__fastcall*)(void* animation);

// releases the caller's reference to the animation at + 0x5F: E8 and a rel32
// (the Mannequin action's release).
constexpr std::size_t kReleaseCall = 0x5F;
// The holder's first field is its combat actor; the combat actor's actor is at + 504
// (reads the same chain to reach the actor's animated character).
constexpr std::size_t kCombatActorActor = 504;
// The image base the addresses in the log are relative to.
constexpr std::uintptr_t kIdaBase = 0x180000000;

QueueCombatAnimation original_queue = nullptr;
ReleaseAnimation release_animation = nullptr;
RemovalLogger write_log = nullptr;
const std::uint8_t* module_base = nullptr;

std::atomic<void*> removing_actor{nullptr};
std::atomic<unsigned> removing_player{0};
std::atomic<unsigned> left_out{0};

void* holder_actor(void* holder) {
  if (!holder) {
    return nullptr;
  }
  auto* combat_actor = *static_cast<std::uint8_t**>(holder);
  if (!combat_actor) {
    return nullptr;
  }
  return *reinterpret_cast<void**>(combat_actor + kCombatActorActor);
}

// The address of an object's class (its vtable), 0 for none.
unsigned long long class_address(const void* object) {
  if (!object || !module_base) {
    return 0;
  }
  const auto* table = *static_cast<const std::uint8_t* const*>(object);
  return static_cast<unsigned long long>(table - module_base + kIdaBase);
}

std::uintptr_t __fastcall guarded_queue(void* holder, void** animation, float blend) {
  void* removing = removing_actor.load();
  if (!removing || holder_actor(holder) != removing) {
    return original_queue(holder, animation, blend);
  }
  void* left = animation ? *animation : nullptr;
  left_out.fetch_add(1);
  if (write_log) {
    write_log("REMOTE removal player=%u combat animation not queued animation=%p classIDA=0x%llx",
              removing_player.load(), left, class_address(left));
  }
  if (left) {
    release_animation(left);
  }
  return 0;
}

}  // namespace

bool install_removal_guard(void* queue_combat_animation, const void* module, RemovalLogger logger) {
  write_log = logger;
  module_base = static_cast<const std::uint8_t*>(module);
  if (!queue_combat_animation) {
    return false;
  }
  auto* code = static_cast<std::uint8_t*>(queue_combat_animation);
  if (code[kReleaseCall] != 0xE8) {
    return false;
  }
  std::int32_t relative = 0;
  std::memcpy(&relative, code + kReleaseCall + 1, sizeof(relative));
  release_animation = reinterpret_cast<ReleaseAnimation>(code + kReleaseCall + 5 + relative);
  return MH_CreateHook(queue_combat_animation, reinterpret_cast<void*>(guarded_queue),
                       reinterpret_cast<void**>(&original_queue)) == MH_OK &&
         MH_EnableHook(queue_combat_animation) == MH_OK;
}

void begin_replica_removal(void* actor, unsigned player) {
  left_out.store(0);
  removing_player.store(player);
  removing_actor.store(actor);
}

unsigned end_replica_removal() {
  removing_actor.store(nullptr);
  removing_player.store(0);
  return left_out.exchange(0);
}

}  // namespace kcds::runtime::replicas
