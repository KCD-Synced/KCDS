// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "rested_updates.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "MinHook.h"

namespace kcds::game_client {

namespace {

// The event every character's components and C_Actor update on.
constexpr int kUpdateEvent = 33;
// Both receivers hold their entity here.
constexpr std::size_t kEntity = 56;
// A power of two, well above the bodies a client can hold.
constexpr std::size_t kSlots = 8192;

using Table = std::array<std::atomic<void*>, kSlots>;

Table tables[2];
Table calm_tables[2];
// The table the workers read; the engine thread fills the other.
std::atomic<int> readable{0};

// The combat update's receiver holds the movement object here; its +504 is the C_Actor.
constexpr std::size_t kCombatMovement = 24;
constexpr std::size_t kMovementActor = 504;

using CombatUpdate = std::int64_t(__fastcall*)(void* combat, float seconds);
CombatUpdate original_combat_update = nullptr;

using ComponentEvent = void(__fastcall*)(void* component, const int* event);
using ActorEvent = void(__fastcall*)(void* actor, const int* event);

ComponentEvent original_component_event = nullptr;
ActorEvent original_actor_event = nullptr;

std::size_t slot_of(const void* entity) {
  const auto value = reinterpret_cast<std::uintptr_t>(entity);
  return ((value >> 4) * 0x9E3779B97F4A7C15ull) >> 40 & (kSlots - 1);
}

bool in_table(const Table& table, const void* entity) {
  for (std::size_t slot = slot_of(entity);; slot = (slot + 1) & (kSlots - 1)) {
    const void* held = table[slot].load(std::memory_order_relaxed);
    if (!held) {
      return false;
    }
    if (held == entity) {
      return true;
    }
  }
}

bool rested(const void* receiver) {
  const void* entity =
      *reinterpret_cast<void* const*>(static_cast<const std::uint8_t*>(receiver) + kEntity);
  return entity && in_table(tables[readable.load(std::memory_order_acquire)], entity);
}

std::int64_t __fastcall hooked_combat_update(void* combat, float seconds) {
  const auto* bytes = static_cast<const std::uint8_t*>(combat);
  const auto* movement = *reinterpret_cast<const std::uint8_t* const*>(bytes + kCombatMovement);
  const auto* actor =
      movement ? *reinterpret_cast<const std::uint8_t* const*>(movement + kMovementActor) : nullptr;
  const void* entity = actor ? *reinterpret_cast<void* const*>(actor + kEntity) : nullptr;
  if (entity && in_table(calm_tables[readable.load(std::memory_order_acquire)], entity)) {
    return 0;
  }
  return original_combat_update(combat, seconds);
}

void __fastcall hooked_component_event(void* component, const int* event) {
  if (*event == kUpdateEvent && rested(component)) {
    return;
  }
  original_component_event(component, event);
}

void __fastcall hooked_actor_event(void* actor, const int* event) {
  if (*event == kUpdateEvent && rested(actor)) {
    return;
  }
  original_actor_event(actor, event);
}

}  // namespace

void begin_rested_frame() {
  const int filled = 1 - readable.load(std::memory_order_relaxed);
  readable.store(filled, std::memory_order_release);
  for (auto& slot : tables[1 - filled]) {
    slot.store(nullptr, std::memory_order_relaxed);
  }
  for (auto& slot : calm_tables[1 - filled]) {
    slot.store(nullptr, std::memory_order_relaxed);
  }
}

static void add_to_table(Table& table, void* entity) {
  std::size_t slot = slot_of(entity);
  // Half full at most: past that a body just keeps its update.
  for (std::size_t tried = 0; tried < kSlots / 2; ++tried) {
    void* held = table[slot].load(std::memory_order_relaxed);
    if (!held) {
      table[slot].store(entity, std::memory_order_relaxed);
      return;
    }
    if (held == entity) {
      return;
    }
    slot = (slot + 1) & (kSlots - 1);
  }
}

void mark_rested(void* entity) {
  add_to_table(tables[1 - readable.load(std::memory_order_relaxed)], entity);
}

void mark_calm(void* entity) {
  add_to_table(calm_tables[1 - readable.load(std::memory_order_relaxed)], entity);
}

bool install_rested_updates() {
  void* component_event = resolve_unique("CharacterComponentEvent",
                                         "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 83 3A 21 48 "
                                         "8B F2 48 8B F9 75 30 48 8B 0D ?? ?? "
                                         "?? ?? 48 8B 01 FF 90 98 00 00 00 84 C0 75 2C 48 8B");
  void* actor_event = resolve_unique("CharacterActorEvent",
                                     "48 89 5C 24 18 48 89 6C 24 20 56 57 41 56 48 81 EC A0 00 00 "
                                     "00 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
                                     "48 89 84 24 90 00 00 00 48 8B F9 48 8B DA 48 8B 89");
  void* combat_update = resolve_unique("CharacterCombatUpdate",
                                       "48 89 5C 24 08 57 48 83 EC 30 48 8B F9 0F 29 74 24 20 48 "
                                       "8B 49 28 0F 28 F1 E8 ?? ?? ?? ?? 48 8B 4F 20 0F 28 CE E8");
  if (!component_event || !actor_event || !combat_update) {
    log("CHARACTERS rested updates: a function was not found, every body keeps its update");
    return false;
  }
  const bool hooked =
      MH_CreateHook(component_event, reinterpret_cast<void*>(hooked_component_event),
                    reinterpret_cast<void**>(&original_component_event)) == MH_OK &&
      MH_CreateHook(actor_event, reinterpret_cast<void*>(hooked_actor_event),
                    reinterpret_cast<void**>(&original_actor_event)) == MH_OK &&
      MH_CreateHook(combat_update, reinterpret_cast<void*>(hooked_combat_update),
                    reinterpret_cast<void**>(&original_combat_update)) == MH_OK &&
      MH_EnableHook(component_event) == MH_OK && MH_EnableHook(actor_event) == MH_OK &&
      MH_EnableHook(combat_update) == MH_OK;
  log("CHARACTERS rested updates %s", hooked ? "installed" : "failed");
  return hooked;
}

}  // namespace kcds::game_client
