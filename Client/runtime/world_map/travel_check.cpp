#include "travel_check.hpp"

#include <cstddef>
#include <cstdint>

namespace kcds::runtime::world_map::travel_check {
namespace {

// The way to C_FastTravel, as its Stop binding walks it.
constexpr std::size_t kServicesPlayerModule = 296;
constexpr std::size_t kPlayerFastTravel = 40;

// C_FastTravel's fields the check reads.
constexpr std::size_t kTravelActive = 72;
constexpr std::size_t kTravelFlags = 73;
constexpr std::size_t kTravelTraveller = 88;
constexpr std::size_t kTravelCarrier = 96;
constexpr std::size_t kTravelRouteBegin = 144;
constexpr std::size_t kTravelRouteEnd = 152;
constexpr std::uint8_t kFlagInterior = 2;
constexpr std::uint8_t kFlagForced = 4;
constexpr int kCodeNotNow = 1;

// services + 8, slot 512: the local player's actor.
constexpr std::size_t kServicesGame = 8;
constexpr std::size_t kGameLocalPlayer = 512;

// a rider is carried by their horse, whose entity id is at + 464 + 8; the riding state
// is 2 at slot 48 of the object at + 2448, and the id is looked up in services + 384,
// slot 24.
constexpr std::size_t kActorState = 2448;
constexpr std::size_t kStateMode = 48;
constexpr std::int8_t kModeRiding = 2;
constexpr std::size_t kActorHorseId = 472;
constexpr std::size_t kServicesActors = 384;
constexpr std::size_t kActorsById = 24;

// the object asked about the "interior" tag is services + 352, + 168, slot 16's
// answer's first slot.
constexpr std::size_t kServicesWorld = 352;
constexpr std::size_t kWorldPlace = 168;
constexpr std::size_t kPlaceHolder = 16;

// A route point; the check counts them and reads none.
struct RoutePoint {
  float x = 0;
  float y = 0;
  float z = 0;
};

using Check = std::uint8_t(__fastcall*)(void* travel);
using TagManager = void*(__fastcall*)();
using HasTag = bool(__fastcall*)(void* manager, void* object, void** tag);
using MakeString = void(__fastcall*)(void** out, const char* text);
using FreeString = void(__fastcall*)(void** text);

Natives natives;

template <typename T>
T& field(void* object, std::size_t offset) {
  return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset);
}

template <typename Function>
Function slot(void* object, std::size_t offset) {
  return *reinterpret_cast<Function*>(field<std::uint8_t*>(object, 0) + offset);
}

void* carrier_of(void* services, void* player) {
  void* state = field<void*>(player, kActorState);
  using Mode = std::int8_t(__fastcall*)(void*);
  if (slot<Mode>(state, kStateMode)(state) != kModeRiding) {
    return player;
  }
  const std::uint32_t horse = field<std::uint32_t>(player, kActorHorseId);
  if (horse == 0) {
    return nullptr;
  }
  void* actors = field<void*>(services, kServicesActors);
  using ById = void*(__fastcall*)(void*, std::uint32_t);
  return slot<ById>(actors, kActorsById)(actors, horse);
}

bool interior_now(void* services) {
  void* place = field<void*>(field<void*>(services, kServicesWorld), kWorldPlace);
  using Getter = void*(__fastcall*)(void*);
  void* holder = slot<Getter>(place, kPlaceHolder)(place);
  void* object = slot<Getter>(holder, 0)(holder);
  void* tag = nullptr;
  reinterpret_cast<MakeString>(natives.make_string)(&tag, "interior");
  const bool interior = reinterpret_cast<HasTag>(natives.has_tag)(
      reinterpret_cast<TagManager>(natives.tag_manager)(), object, &tag);
  reinterpret_cast<FreeString>(natives.free_string)(&tag);
  return interior;
}

}  // namespace

void install(const Natives& given) {
  natives = given;
}

bool available() {
  return natives.services && natives.check && natives.tag_manager && natives.has_tag &&
         natives.make_string && natives.free_string;
}

int ask() {
  if (!available()) {
    return -1;
  }
  void* services = natives.services();
  void* module = services ? field<void*>(services, kServicesPlayerModule) : nullptr;
  void* travel = module ? field<void*>(module, kPlayerFastTravel) : nullptr;
  void* game = services ? field<void*>(services, kServicesGame) : nullptr;
  if (!travel || !game) {
    return -1;
  }
  // A travel under way owns these fields, and the game's check answers "not now" to it.
  if (field<std::uint8_t>(travel, kTravelActive)) {
    return kCodeNotNow;
  }
  using LocalPlayer = void*(__fastcall*)(void*);
  void* player = slot<LocalPlayer>(game, kGameLocalPlayer)(game);
  if (!player || !field<void*>(player, kActorState)) {
    return -1;
  }
  RoutePoint route[2];
  auto& begin = field<RoutePoint*>(travel, kTravelRouteBegin);
  auto& end = field<RoutePoint*>(travel, kTravelRouteEnd);
  auto& flags = field<std::uint8_t>(travel, kTravelFlags);
  auto& traveller = field<void*>(travel, kTravelTraveller);
  auto& carrier = field<void*>(travel, kTravelCarrier);
  const auto saved_begin = begin;
  const auto saved_end = end;
  const auto saved_flags = flags;
  void* const saved_traveller = traveller;
  void* const saved_carrier = carrier;

  const bool interior = interior_now(services);
  begin = route;
  end = route + 2;
  flags = static_cast<std::uint8_t>((saved_flags & ~(kFlagInterior | kFlagForced)) |
                                    (interior ? kFlagInterior : 0));
  traveller = player;
  carrier = carrier_of(services, player);
  const int code = reinterpret_cast<Check>(natives.check)(travel);

  begin = saved_begin;
  end = saved_end;
  flags = saved_flags;
  traveller = saved_traveller;
  carrier = saved_carrier;
  return code;
}

}  // namespace kcds::runtime::world_map::travel_check
