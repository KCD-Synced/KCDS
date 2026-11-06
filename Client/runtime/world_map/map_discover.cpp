#include "map_discover.hpp"

#include "MinHook.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace kcds::runtime::world_map::discover {
namespace {

constexpr std::size_t kRpgModule = 304;
constexpr std::size_t kLocationManager = 184;
constexpr std::size_t kHolderRefCount = 8;
constexpr std::size_t kHolderPlace = 16;
constexpr std::size_t kPlaceStatus = 10;
constexpr std::size_t kPlaceSetStatus = 11;
constexpr int kDiscovered = 2;
constexpr std::size_t kGuidTextLength = 36;
// C_RPGLocationManager slot 32: DiscoverLocation(guid, ...); slot 33: DiscoverPOI(guid,
// ..).
constexpr std::size_t kDiscoverLocation = 32;
// Slot 49: a point of interest's status changed (from the point's setter),
// where the game's walking discovery of a point shows its toast.
constexpr std::size_t kPoiStatusChanged = 49;
// The points of interest: a hash map of the manager by GUID, its list's head at + 40 (a node: the
// next at + 0, the GUID at + 16, the holder at + 32); a point's status at + 140, which its setter
// writes and only then tells the manager's listeners when asked to.
constexpr std::size_t kPoiListHead = 40;
constexpr std::size_t kNodeKey = 16;
constexpr std::size_t kNodeHolder = 32;
constexpr std::size_t kPoiStatus = 140;
constexpr std::size_t kMaxPoiNodes = 65536;

using FindPlace = void**(__fastcall*)(void* manager, void** out, const void* guid);
using GetStatus = int(__fastcall*)(void* place);
using SetStatus = void(__fastcall*)(void* place, int status);
using DeleteHolder = void(__fastcall*)(void* holder, int flags);
using DiscoverLocation = std::int64_t(__fastcall*)(void* manager, const void* guid,
                                                   std::int64_t flag, char known_only);

Natives natives;
DiscoverLocation original_discover = nullptr;
// The key of the location the game discovers on this thread right now, else null.
thread_local const std::uint8_t* discovering = nullptr;
// The point of interest whose status the game changes on this thread right now, else null.
using PoiStatusChanged = std::int64_t(__fastcall*)(void* manager, void* poi, std::int64_t before,
                                                   std::int64_t after);
PoiStatusChanged original_poi_changed = nullptr;
thread_local void* changing_poi = nullptr;

template <class T>
T field(const void* object, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

int hex_digit(char c) {
  static constexpr char kDigits[] = "0123456789abcdef";
  const char folded = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
  for (int digit = 0; digit < 16; ++digit) {
    if (kDigits[digit] == folded) {
      return digit;
    }
  }
  return -1;
}

// The GUID's 16 bytes as the game keeps them, from its text; false for anything else.
bool guid_bytes(const std::string& text, std::array<std::uint8_t, 16>& out) {
  if (text.size() != kGuidTextLength) {
    return false;
  }
  std::array<std::uint8_t, 16> digits{};
  std::size_t count = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (text[i] != '-') {
        return false;
      }
      continue;
    }
    const int high = hex_digit(text[i]);
    const int low = i + 1 < text.size() ? hex_digit(text[i + 1]) : -1;
    if (high < 0 || low < 0 || count >= digits.size()) {
      return false;
    }
    digits[count++] = std::uint8_t(high * 16 + low);
    ++i;
  }
  if (count != digits.size()) {
    return false;
  }
  // The first three groups are little-endian numbers in memory, the last eight bytes as written.
  out = {digits[3],  digits[2],  digits[1],  digits[0], digits[5],  digits[4],
         digits[7],  digits[6],  digits[8],  digits[9], digits[10], digits[11],
         digits[12], digits[13], digits[14], digits[15]};
  return true;
}

void* location_manager() {
  void* services = natives.services ? natives.services() : nullptr;
  void* module = services ? field<void*>(services, kRpgModule) : nullptr;
  void* manager = module ? field<void*>(module, kLocationManager) : nullptr;
  if (!manager || field<const void*>(manager, 0) != natives.manager_vtable) {
    return nullptr;
  }
  return manager;
}

std::int64_t __fastcall hooked_discover(void* manager, const void* guid, std::int64_t flag,
                                        char known_only) {
  const std::uint8_t* outer = discovering;
  discovering = static_cast<const std::uint8_t*>(guid);
  const std::int64_t result = original_discover(manager, guid, flag, known_only);
  discovering = outer;
  return result;
}

std::int64_t __fastcall hooked_poi_changed(void* manager, void* poi, std::int64_t before,
                                           std::int64_t after) {
  void* outer = changing_poi;
  changing_poi = poi;
  const std::int64_t result = original_poi_changed(manager, poi, before, after);
  changing_poi = outer;
  return result;
}

template <class Fn>
void hook(std::size_t slot, void* detour, Fn* original) {
  void* target = static_cast<void* const*>(natives.manager_vtable)[slot];
  if (MH_CreateHook(target, detour, reinterpret_cast<void**>(original)) == MH_OK) {
    MH_EnableHook(target);
  }
}

// The key of the point of interest at `poi` (a holder's + 16), or null.
const std::uint8_t* poi_key(void* manager, const void* poi) {
  void* head = field<void*>(manager, kPoiListHead);
  void* node = head ? field<void*>(head, 0) : nullptr;
  for (std::size_t count = 0; node && node != head && count < kMaxPoiNodes; ++count) {
    auto* holder = field<std::uint8_t*>(node, kNodeHolder);
    if (holder && holder + kHolderPlace == poi) {
      return static_cast<const std::uint8_t*>(node) + kNodeKey;
    }
    node = field<void*>(node, 0);
  }
  return nullptr;
}

// The point of interest of that GUID's status, or null when the level has none.
int* poi_status(void* manager, const std::array<std::uint8_t, 16>& guid) {
  void* head = field<void*>(manager, kPoiListHead);
  void* node = head ? field<void*>(head, 0) : nullptr;
  for (std::size_t count = 0; node && node != head && count < kMaxPoiNodes; ++count) {
    if (std::memcmp(static_cast<const std::uint8_t*>(node) + kNodeKey, guid.data(), 16) == 0) {
      auto* holder = field<std::uint8_t*>(node, kNodeHolder);
      return holder ? reinterpret_cast<int*>(holder + kHolderPlace + kPoiStatus) : nullptr;
    }
    node = field<void*>(node, 0);
  }
  return nullptr;
}

std::string text_of(const std::uint8_t* k) {
  char text[kGuidTextLength + 1];
  std::snprintf(text, sizeof(text),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", k[3], k[2],
                k[1], k[0], k[5], k[4], k[7], k[6], k[8], k[9], k[10], k[11], k[12], k[13], k[14],
                k[15]);
  return text;
}

void release(void* holder) {
  auto* count = reinterpret_cast<int*>(static_cast<std::uint8_t*>(holder) + kHolderRefCount);
  if ((*count)-- == 1) {
    (*static_cast<DeleteHolder**>(holder))[0](holder, 1);
  }
}

}  // namespace

void install(const Natives& given) {
  natives = given;
  if (!natives.manager_vtable) {
    return;
  }
  hook(kDiscoverLocation, reinterpret_cast<void*>(&hooked_discover), &original_discover);
  hook(kPoiStatusChanged, reinterpret_cast<void*>(&hooked_poi_changed), &original_poi_changed);
}

std::string discovering_key(bool location) {
  if (location) {
    return discovering ? text_of(discovering) : std::string();
  }
  void* manager = changing_poi ? location_manager() : nullptr;
  const std::uint8_t* key = manager ? poi_key(manager, changing_poi) : nullptr;
  return key ? text_of(key) : std::string();
}

Outcome place(const std::string& guid) {
  std::array<std::uint8_t, 16> bytes{};
  void* manager = natives.find_place && natives.manager_vtable ? location_manager() : nullptr;
  if (!manager || !guid_bytes(guid, bytes)) {
    return Outcome::Unknown;
  }
  void* holder = nullptr;
  reinterpret_cast<FindPlace>(natives.find_place)(manager, &holder, bytes.data());
  if (!holder) {
    // A point of interest: its status written as its setter does without telling anyone.
    int* status = poi_status(manager, bytes);
    if (!status) {
      return Outcome::Unknown;
    }
    if (*status == kDiscovered) {
      return Outcome::Already;
    }
    *status = kDiscovered;
    return Outcome::DiscoveredPoi;
  }
  void* place = static_cast<std::uint8_t*>(holder) + kHolderPlace;
  void** table = *static_cast<void***>(place);
  Outcome outcome = Outcome::Already;
  if (reinterpret_cast<GetStatus>(table[kPlaceStatus])(place) != kDiscovered) {
    reinterpret_cast<SetStatus>(table[kPlaceSetStatus])(place, kDiscovered);
    outcome = Outcome::Discovered;
  }
  release(holder);
  return outcome;
}

}  // namespace kcds::runtime::world_map::discover
