// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "menu_pages.hpp"

#include <cstdio>
#include <map>

#include "../core/natives.hpp"

namespace kcds::runtime::native_ui::menu_pages {
namespace {

constexpr std::size_t kServicesInventoryScreen = 432;
constexpr std::size_t kScreenInterface = 16;

using KeyCallback = void(__fastcall*)(void* callback);
using Close = void(__fastcall*)(void* screen, std::uint64_t how);

// A page's key callback: the shared prologue with the page in its mov dl. 68 bytes: shorter, the
// prologue is shared with other key callbacks.
KeyCallback callback_of(std::uint8_t page) {
  static std::map<std::uint8_t, KeyCallback> known;
  const auto found = known.find(page);
  if (found != known.end()) {
    return found->second;
  }
  char pattern[512];
  std::snprintf(
      pattern, sizeof(pattern),
      "4C 8B DC 49 89 5B 10 57 48 81 EC D0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 C0 "
      "00 00 00 48 8B 79 08 49 8D 53 98 48 8D 4C 24 20 48 8B 47 10 49 C7 43 D8 00 00 00 00 48 8B "
      "58 08 48 8B 05 ?? ?? ?? 03 49 89 43 98 E8 ?? ?? ?? FF 4C 8B C0 48 8D 4F 10 B2 %02X FF D3 48 "
      "8D 4C 24 78 E8 ?? ?? 1E 00 48 8B",
      unsigned(page));
  char name[32];
  std::snprintf(name, sizeof(name), "MenuPageKey%u", unsigned(page));
  auto callback = reinterpret_cast<KeyCallback>(kcds::game_client::resolve_unique(name, pattern));
  known[page] = callback;
  return callback;
}

std::uint8_t* screen() {
  void* services = kcds::game_client::get_services ? kcds::game_client::get_services() : nullptr;
  return services ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(services) +
                                                      kServicesInventoryScreen)
                  : nullptr;
}

}  // namespace

void* screen_owner() {
  std::uint8_t* at = screen();
  return at ? at - kScreenInterface : nullptr;
}

bool open(std::uint8_t page) {
  const KeyCallback callback = callback_of(page);
  void* owner = screen_owner();
  if (!callback || !owner) {
    return false;
  }
  // The std::function object the callback is called on: its vtable, then the captured owner.
  void* function[2] = {nullptr, owner};
  callback(function);
  return true;
}

bool close() {
  std::uint8_t* at = screen();
  if (!at) {
    return false;
  }
  // The screen's close key ("close" of the apse map): its callback calls the screen's method 2
  // with 0.
  (*reinterpret_cast<Close**>(at))[2](at, 0);
  return true;
}

}  // namespace kcds::runtime::native_ui::menu_pages
