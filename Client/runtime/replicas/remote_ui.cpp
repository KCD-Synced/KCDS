#include "remote_ui.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "MinHook.h"
#include "remote_ui_role.hpp"

namespace kcds::game_client {
namespace {

// 1.5.5: (UI registry slot 6) consumes its by-value CryString argument
// through on every path. Suppression must perform that same cleanup exactly once.
using RegisterMask = void(__fastcall*)(void*, void*, const void*, void*);
using ReleaseName = void(__fastcall*)(void*);
RegisterMask original_register_mask = nullptr;
ReleaseName release_name = nullptr;

void __fastcall register_mask(void* registry, void* name, const void* mask, void* listener) {
  const bool registered = kcds::runtime::replicas::route_ui_registration(
      listener, [&] { original_register_mask(registry, name, mask, listener); },
      [&] { release_name(name); });
  if (!registered) {
    log("REMOTE UI local missile listener omitted listener=%p", listener);
  }
}

}  // namespace

bool install_remote_ui_role() {
  auto registration = resolve_unique(
      "UiStateRegisterMask",
      "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 20 4C 8B F1 49 8B F1 B9 20 00 00 00 "
      "49 8B E8 48 8B FA E8");
  release_name = reinterpret_cast<ReleaseName>(
      resolve_unique("UiStateRegistrationNameRelease",
                     "48 83 EC 28 48 8B 09 48 83 C1 F4 E8 ?? ?? ?? ?? 85 C0 78 ?? ?? ?? ?? ?? ?? "
                     "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? "
                     "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 48 8B C4 48"));
  if (!registration || !release_name) {
    log("REMOTE UI role signatures unavailable; adapter not started");
    return false;
  }
  if (MH_CreateHook(registration, reinterpret_cast<void*>(register_mask),
                    reinterpret_cast<void**>(&original_register_mask)) != MH_OK ||
      MH_EnableHook(registration) != MH_OK) {
    log("REMOTE UI role hook unavailable; adapter not started");
    return false;
  }
  log("REMOTE UI role installed; replicas do not register local missile listeners");
  return true;
}

}  // namespace kcds::game_client
