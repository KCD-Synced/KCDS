#include "dice_sit.hpp"

#include <cstring>

namespace kcds::runtime::dice::sit {
namespace {

// The table's two chairs: the player's sit-down takes the first (seat1_SO); the second
// (seat2_SO) is the gambler's, whose interrupt sits its actor there with the same stance
// (so_diceTable_new.xml: dice_interrupt_playerSit, dice_interrupt_playDiceMinigame).
constexpr char kSitInterrupt[] = "interrupt_playerSit";
constexpr char kSecondSeatInterrupt[] = "interrupt_playDiceMinigame";
constexpr char kLeaveInterrupt[] = "interrupt_instantlyLeaveDiceTable";
// What the table's behaviour gives both gamblers when a game ends (so_diceTable_new.xml, the tree
// dice_interrupt_instantlyLeaveDiceTable): it takes them out of their seat.
constexpr std::uint8_t kPriority = 201;
// The scheduler refuses a request no higher than what runs or waits. A match the server
// ended leaves the minigame's interrupt running at 201, so getting up asks above it.
constexpr std::uint8_t kLeavePriority = 202;
// AddInterrupt's bytes after the priority: the pending check, urgency (0 Default), aliveness
// (0 Alive), privileged.
constexpr std::uint8_t kUrgencyDefault = 0;
constexpr std::size_t kManagerLocalNpc = 104;
constexpr std::size_t kHostId = 8;
constexpr std::size_t kSchedulerSlot = 0x138 / 8;
// The request and its moved copy, 48 bytes each.
constexpr std::size_t kRequestBytes = 64;

using MakeString = void*(__fastcall*)(void* string, const char* text);
using FreeString = void(__fastcall*)(void* string);
using BehaviorId = void*(__fastcall*)(void* unused, std::uint32_t* id, void* name);
using HostOf = void*(__fastcall*)(void* unused, std::uint64_t id);
using BuildRequest = void*(__fastcall*)(void* request, std::uint32_t behavior, std::uint64_t host,
                                        std::uint8_t priority, std::uint8_t pending,
                                        std::uint8_t urgency, std::uint8_t aliveness,
                                        std::uint8_t privileged);
using MoveRequest = void*(__fastcall*)(void* into, void* request);
using Enqueue = std::uint8_t(__fastcall*)(void* scheduler, void* request);
using DestroyRequest = void(__fastcall*)(void* request);
using Scheduler = void*(__fastcall*)(void* npc);

Natives natives;

template <class T>
T read(const void* object, std::size_t offset) {
  T value{};
  if (object) {
    std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  }
  return value;
}

// The behaviour's id the game knows the interrupt's name by; 0 when it does not.
std::uint32_t behavior_id(const char* interrupt) {
  void* name = nullptr;
  reinterpret_cast<MakeString>(natives.make_string)(&name, interrupt);
  std::uint32_t id = 0;
  reinterpret_cast<BehaviorId>(natives.behavior_id)(nullptr, &id, &name);
  reinterpret_cast<FreeString>(natives.free_string)(&name);
  return id;
}

}  // namespace

bool install(const Natives& given) {
  natives = given;
  return natives.make_string && natives.free_string && natives.behavior_id && natives.host_of &&
         natives.build_request && natives.move_request && natives.enqueue &&
         natives.destroy_request && natives.npc_manager && natives.wuid_of;
}

namespace {

// Adds one of the table's interrupts to an NPC agent (the local player's when npc is null), as the
// behaviour's AddInterrupt does.
bool add_interrupt(void* table_behavior, const char* interrupt, void* npc,
                   std::uint8_t priority = kPriority) {
  if (!natives.enqueue || !table_behavior) {
    return false;
  }
  std::uint64_t wuid = 0;
  if (!natives.wuid_of(table_behavior, wuid)) {
    return false;
  }
  const std::uint32_t behavior = behavior_id(interrupt);
  void* host = reinterpret_cast<HostOf>(natives.host_of)(nullptr, wuid);
  if (!npc) {
    npc = read<void*>(*natives.npc_manager, kManagerLocalNpc);
  }
  if (behavior == 0 || !host || !npc) {
    if (natives.log) {
      natives.log("DICE %s unavailable (behaviour %u, host %p, player %p)", interrupt, behavior,
                  host, npc);
    }
    return false;
  }
  alignas(16) std::uint8_t request[kRequestBytes] = {};
  alignas(16) std::uint8_t moved[kRequestBytes] = {};
  reinterpret_cast<BuildRequest>(natives.build_request)(
      request, behavior, read<std::uint64_t>(host, kHostId), priority, 0, kUrgencyDefault, 0, 0);
  void* scheduler = (*reinterpret_cast<Scheduler**>(npc))[kSchedulerSlot](npc);
  reinterpret_cast<MoveRequest>(natives.move_request)(moved, request);
  const bool queued = reinterpret_cast<Enqueue>(natives.enqueue)(scheduler, moved) != 0;
  reinterpret_cast<DestroyRequest>(natives.destroy_request)(request);
  if (natives.log) {
    natives.log("DICE %s %s", interrupt, queued ? "asked" : "refused");
  }
  return queued;
}

}  // namespace

bool sit_down(void* table_behavior, std::uint8_t seat) {
  return add_interrupt(table_behavior, seat == 1 ? kSecondSeatInterrupt : kSitInterrupt, nullptr);
}

bool seat_npc(void* table_behavior, void* npc_agent, std::uint8_t seat) {
  if (!npc_agent) {
    return false;
  }
  return add_interrupt(table_behavior, seat == 1 ? kSecondSeatInterrupt : kSitInterrupt, npc_agent);
}

bool leave_table(void* table_behavior) {
  return add_interrupt(table_behavior, kLeaveInterrupt, nullptr, kLeavePriority);
}

bool npc_leave_table(void* table_behavior, void* npc_agent) {
  if (!npc_agent) {
    return false;
  }
  return add_interrupt(table_behavior, kLeaveInterrupt, npc_agent, kLeavePriority);
}

}  // namespace kcds::runtime::dice::sit
