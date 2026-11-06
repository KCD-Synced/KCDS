#include "seat_reservations.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../core/natives.hpp"
#include "../level/level_edits.hpp"
#include "MinHook.h"
#include "kcd2s/integration/seats.hpp"

namespace kcds::runtime::seats {
namespace {

using kcd2s::integration::Seat;

using Reserve = char(__fastcall*)(void* smart_object, std::uint64_t wuid, std::uint16_t resource);
using Release = std::int64_t(__fastcall*)(void* smart_object, std::uint64_t wuid,
                                          std::uint16_t resource);
using Holds = bool(__fastcall*)(void* table, std::uint64_t wuid, std::uint16_t resource);
using FindSmartObject = void*(__fastcall*)(void* unused, std::uint64_t wuid);
using FindNpc = std::uint8_t**(__fastcall*)(void* records, std::uint8_t** found,
                                            const std::uint32_t* entity_id);

// Smart object (and AI object): its WUID, reservation table and capacity per resource.
constexpr std::size_t kWuid = 8;
constexpr std::size_t kReservations = 240;
constexpr std::size_t kCapacityBegin = 296;
constexpr std::size_t kCapacityEnd = 304;
// NPC manager: the local player's AI object and the records { entity id, actor, AI object }.
constexpr std::size_t kManagerPlayerAi = 104;
constexpr std::size_t kManagerRecords = 48;
constexpr std::size_t kRecordSize = 24;
constexpr std::size_t kRecordAiObject = 16;

// What a replica holds: the seats of its owner's field it reserved (each owner's seat with the
// seat of this client it is: a placed smart object's WUID differs), with the WUID it holds
// them with, and the field they came from.
struct Replica {
  std::uint64_t wuid = 0;
  std::string field;
  std::map<Seat, Seat> held;
};

// A placed smart object found by its place: this many WUIDs either side of the owner's, as far
// from the owner's place (each client builds the same objects, in an order of its own).
constexpr std::uint64_t kPlaceSearch = 128;
constexpr long kPlaceReachCm = 10;

Natives natives;
Logger log_line = nullptr;
Reserve original_reserve = nullptr;

std::mutex local_mutex;
std::set<Seat> local_reserved;
// When the game last gave each AI object a seat, by its WUID.
std::unordered_map<std::uint64_t, std::uint64_t> seats_taken;
// What every actor reserved (its WUID -> the smart objects' WUIDs and resources), kept until it is
// found released (holds_seat, issue 016).
std::unordered_map<std::uint64_t, std::vector<std::pair<std::uint64_t, std::uint16_t>>>
    actor_reserved;
std::unordered_map<std::uint32_t, Replica> replicas;

template <class T>
T read(const void* object, std::size_t offset) {
  T value;
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

void* npc_manager() {
  return natives.npc_manager ? *natives.npc_manager : nullptr;
}

std::uint64_t local_wuid() {
  void* manager = npc_manager();
  void* ai_object = manager ? read<void*>(manager, kManagerPlayerAi) : nullptr;
  return ai_object ? read<std::uint64_t>(ai_object, kWuid) : 0;
}

// The WUID of the AI object the NPC manager made for an entity, 0 without one.
std::uint64_t wuid_of_entity(std::uint32_t entity_id) {
  void* manager = npc_manager();
  if (!manager) {
    return 0;
  }
  auto* records = static_cast<std::uint8_t*>(manager) + kManagerRecords;
  std::uint8_t* found = nullptr;
  reinterpret_cast<FindNpc>(natives.find_npc)(records, &found, &entity_id);
  auto* begin = read<std::uint8_t*>(records, 0);
  const auto count = read<std::size_t>(records, 8);
  if (!found || found == begin + count * kRecordSize) {
    return 0;
  }
  void* ai_object = read<void*>(found, kRecordAiObject);
  return ai_object ? read<std::uint64_t>(ai_object, kWuid) : 0;
}

// The smart object of a seat, null when it is not there or has no such resource.
void* smart_object_of(const Seat& seat) {
  void* object =
      reinterpret_cast<FindSmartObject>(natives.find_smart_object)(nullptr, seat.smart_object);
  if (!object) {
    return nullptr;
  }
  const auto begin = read<std::uintptr_t>(object, kCapacityBegin);
  const auto end = read<std::uintptr_t>(object, kCapacityEnd);
  if (!begin || end < begin || seat.resource >= (end - begin) / sizeof(std::int32_t)) {
    return nullptr;
  }
  return object;
}

bool holds(void* object, std::uint64_t wuid, std::uint16_t resource) {
  void* table = read<void*>(object, kReservations);
  return table && reinterpret_cast<Holds>(natives.holds)(table, wuid, resource);
}

// Where a smart object stands: the object it points to at + 24 keeps its place at + 8 (found live,
// issue 016: a sitting NPC's bench seat 1.4 m from its feet).
constexpr std::size_t kSmartObjectSpot = 24;
constexpr std::size_t kSpotPlace = 8;
constexpr float kMapExtent = 8192.0f;

bool smart_object_place(void* smart_object, float place[3]) {
  const auto* spot = read<const std::uint8_t*>(smart_object, kSmartObjectSpot);
  if (!spot) {
    return false;
  }
  std::memcpy(place, spot + kSpotPlace, 3 * sizeof(float));
  return std::isfinite(place[0]) && std::isfinite(place[1]) && std::isfinite(place[2]) &&
         place[0] > 1.0f && place[1] > 1.0f && place[0] < kMapExtent && place[1] < kMapExtent;
}

// Smart objects shown as taken because an edit took their object away, told once each.
std::set<void*> refused_told;

// The game's check whether a smart object's resource is free for an actor (its
// capacity is more than the actors holding it), which the AI asks before it chooses a seat
// and the reservation asks again. A seat, bench or work place whose object an edit of
// the world hid or moved away answers taken (issue 016: NPCs and dogs sat on benches that were no
// longer there), so no one chooses it. Never inside a reservation: one of the game's callers
// counts a reservation as made whatever it answers and releases it later, and a release of one
// never made crashed the game.
using Available = char(__fastcall*)(void* smart_object, std::uint16_t resource, std::uint64_t wuid);
Available original_available = nullptr;
thread_local bool in_reservation = false;

char __fastcall hooked_available(void* smart_object, std::uint16_t resource, std::uint64_t wuid) {
  if (!in_reservation && smart_object) {
    float place[3] = {};
    if (smart_object_place(smart_object, place) &&
        kcds::runtime::level::blocks_smart_object(place)) {
      std::lock_guard<std::mutex> lock(local_mutex);
      if (refused_told.insert(smart_object).second && log_line) {
        log_line(
            "SEATS smart object %p at %.1f,%.1f,%.1f shown taken: an edit took its object "
            "away",
            smart_object, place[0], place[1], place[2]);
      }
      return 0;
    }
  }
  return original_available(smart_object, resource, wuid);
}

char __fastcall hooked_reserve(void* smart_object, std::uint64_t wuid, std::uint16_t resource) {
  in_reservation = true;
  const char reserved = original_reserve(smart_object, wuid, resource);
  in_reservation = false;
  const std::uint64_t local = local_wuid();
  if (reserved && wuid) {
    std::lock_guard<std::mutex> lock(local_mutex);
    seats_taken[wuid] = GetTickCount64();
    if (smart_object) {
      auto& held = actor_reserved[wuid];
      const std::pair<std::uint64_t, std::uint16_t> seat{read<std::uint64_t>(smart_object, kWuid),
                                                         resource};
      if (std::find(held.begin(), held.end(), seat) == held.end()) {
        held.push_back(seat);
      }
    }
  }
  if (reserved && smart_object && local && wuid == local) {
    std::lock_guard<std::mutex> lock(local_mutex);
    local_reserved.insert({read<std::uint64_t>(smart_object, kWuid), resource});
  }
  return reserved;
}

// Where a smart object stands, in whole centimetres; false when it is not known.
bool place_cm(void* smart_object, long out[3]) {
  float place[3] = {};
  if (!smart_object_place(smart_object, place)) {
    return false;
  }
  for (int axis = 0; axis < 3; ++axis) {
    out[axis] = std::lround(double(place[axis]) * 100.0);
  }
  return true;
}

bool same_place(const long a[3], const long b[3]) {
  for (int axis = 0; axis < 3; ++axis) {
    if (std::labs(a[axis] - b[axis]) > kPlaceReachCm) {
      return false;
    }
  }
  return true;
}

// The seat of this client an owner's seat is: the same WUID when it stands where the owner's
// does (the level's smart objects), else the smart object a few WUIDs away that stands there (a
// placed one). Its WUID 0 when there is none.
Seat local_seat_of(const Seat& owner) {
  void* object = smart_object_of(owner);
  if (!owner.has_place) {
    return object ? owner : Seat{};
  }
  long here[3] = {};
  if (object && place_cm(object, here) && same_place(here, owner.place)) {
    return owner;
  }
  for (std::uint64_t step = 1; step <= kPlaceSearch; ++step) {
    for (const std::uint64_t wuid : {owner.smart_object + step, owner.smart_object - step}) {
      // The WUID's kind is its high byte: a step never leaves it.
      if ((wuid >> 56) != (owner.smart_object >> 56)) {
        continue;
      }
      const Seat candidate{wuid, owner.resource};
      void* other = smart_object_of(candidate);
      if (other && place_cm(other, here) && same_place(here, owner.place)) {
        return candidate;
      }
    }
  }
  return Seat{};
}

void release_held(Replica& replica) {
  for (const auto& [owner, seat] : replica.held) {
    (void)owner;
    void* object = smart_object_of(seat);
    // Released only while it is still held: the smart object may have been made again since.
    if (object && holds(object, replica.wuid, seat.resource)) {
      reinterpret_cast<Release>(natives.release)(object, replica.wuid, seat.resource);
    }
  }
  replica.held.clear();
}

bool hook(void* target, void* detour, void** original) {
  return MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  if (!natives.reserve || !natives.release || !natives.holds || !natives.find_smart_object ||
      !natives.npc_manager || !natives.find_npc) {
    return false;
  }
  if (!hook(natives.reserve, reinterpret_cast<void*>(hooked_reserve),
            reinterpret_cast<void**>(&original_reserve))) {
    return false;
  }
  // The availability check is the reservation's first call (its rel32 at + 0x20, the instruction
  // ending at + 0x24): E8 there, as in WHGame 1.5.5.
  constexpr std::size_t kAvailableCall = 0x1F;
  const auto* reserve = static_cast<const std::uint8_t*>(natives.reserve);
  if (reserve[kAvailableCall] == 0xE8) {
    std::int32_t offset = 0;
    std::memcpy(&offset, reserve + kAvailableCall + 1, sizeof(offset));
    void* available = const_cast<std::uint8_t*>(reserve + kAvailableCall + 5 + offset);
    const bool hooked = hook(available, reinterpret_cast<void*>(hooked_available),
                             reinterpret_cast<void**>(&original_available));
    if (log_line) {
      log_line("SEATS availability check %s", hooked ? "hooked" : "not hooked");
    }
  } else if (log_line) {
    log_line("SEATS availability check not found");
  }
  return true;
}

std::uint64_t seat_taken_at(std::uint32_t entity_id) {
  const std::uint64_t wuid = wuid_of_entity(entity_id);
  std::lock_guard<std::mutex> lock(local_mutex);
  const auto found = seats_taken.find(wuid);
  return found == seats_taken.end() ? 0 : found->second;
}

bool holds_seat(std::uint32_t entity_id) {
  const std::uint64_t wuid = wuid_of_entity(entity_id);
  if (!wuid) {
    return false;
  }
  std::vector<std::pair<std::uint64_t, std::uint16_t>> held;
  {
    std::lock_guard<std::mutex> lock(local_mutex);
    const auto found = actor_reserved.find(wuid);
    if (found == actor_reserved.end()) {
      return false;
    }
    held = found->second;
  }
  std::vector<std::pair<std::uint64_t, std::uint16_t>> still;
  for (const auto& [object_wuid, resource] : held) {
    void* object = smart_object_of(Seat{object_wuid, resource});
    if (object && holds(object, wuid, resource)) {
      still.push_back({object_wuid, resource});
    }
  }
  std::lock_guard<std::mutex> lock(local_mutex);
  if (still.empty()) {
    actor_reserved.erase(wuid);
  } else {
    actor_reserved[wuid] = still;
  }
  return !still.empty();
}

std::string local_seats() {
  const std::uint64_t local = local_wuid();
  std::lock_guard<std::mutex> lock(local_mutex);
  std::set<Seat> published;
  for (auto it = local_reserved.begin(); it != local_reserved.end();) {
    void* object = local ? smart_object_of(*it) : nullptr;
    if (object && holds(object, local, it->resource)) {
      // With its place, so the other clients find a placed one whose WUID differs there.
      Seat seat = *it;
      seat.has_place = place_cm(object, seat.place);
      published.insert(seat);
      ++it;
    } else {
      it = local_reserved.erase(it);
    }
  }
  return kcd2s::integration::seats_field(published);
}

void keep_replica(std::uint32_t player, std::uint32_t entity_id, const std::string& field) {
  const std::uint64_t wuid = wuid_of_entity(entity_id);
  Replica& replica = replicas[player];
  if (replica.field == field && replica.wuid == wuid) {
    return;
  }
  std::set<Seat> wanted;
  if (!kcd2s::integration::parse_seats_field(field, wanted)) {
    wanted.clear();
    if (log_line) {
      log_line("SEATS player=%u field refused: %s", player, field.c_str());
    }
  }
  if (replica.wuid != wuid) {
    release_held(replica);
    replica.wuid = wuid;
  }
  replica.field = field;
  for (auto it = replica.held.begin(); it != replica.held.end();) {
    if (wanted.count(it->first)) {
      ++it;
      continue;
    }
    const Seat& seat = it->second;
    void* object = smart_object_of(seat);
    if (object && holds(object, wuid, seat.resource)) {
      reinterpret_cast<Release>(natives.release)(object, wuid, seat.resource);
    }
    if (log_line) {
      log_line("SEATS player=%u left %016llX:%u", player,
               static_cast<unsigned long long>(seat.smart_object), unsigned(seat.resource));
    }
    it = replica.held.erase(it);
  }
  if (!wuid) {
    return;
  }
  for (const Seat& owner : wanted) {
    if (replica.held.count(owner)) {
      continue;
    }
    const Seat seat = local_seat_of(owner);
    void* object = seat.smart_object ? smart_object_of(seat) : nullptr;
    const bool taken = object && original_reserve(object, wuid, seat.resource);
    if (taken) {
      replica.held.emplace(owner, seat);
    }
    if (log_line) {
      log_line("SEATS player=%u %s %016llX:%u (theirs %016llX)", player,
               taken ? "holds" : (object ? "found it taken" : "has no such seat here"),
               static_cast<unsigned long long>(seat.smart_object), unsigned(seat.resource),
               static_cast<unsigned long long>(owner.smart_object));
    }
  }
}

void forget(std::uint32_t player) {
  auto it = replicas.find(player);
  if (it == replicas.end()) {
    return;
  }
  release_held(it->second);
  replicas.erase(it);
}

void reset() {
  for (auto& [player, replica] : replicas) {
    (void)player;
    release_held(replica);
  }
  replicas.clear();
  std::lock_guard<std::mutex> lock(local_mutex);
  local_reserved.clear();
  seats_taken.clear();
  actor_reserved.clear();
  refused_told.clear();
}

}  // namespace kcds::runtime::seats
