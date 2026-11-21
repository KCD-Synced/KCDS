#include "../core/client_internal.hpp"

#include "dialog_cancel.hpp"

#include <atomic>
#include <cstdint>

#include "../core/natives.hpp"
#include "MinHook.h"

namespace kcds::runtime::testing::dialog_cancel {
namespace {

// The dialogue screen's key registration (up, scroll_up, down, scroll_down, dialog_confirm,
// dialog_confirm_mouse, dialog_skip, dialog_cancel in the action map "dialog"), with the screen.
using RegisterKeys = std::int64_t(__fastcall*)(void* screen);
// The dialog_cancel handler (a std::function's call): reads only the screen captured at + 8.
using CancelHandler = std::int64_t(__fastcall*)(void* handler);

RegisterKeys original_register_keys = nullptr;
CancelHandler cancel_handler = nullptr;
std::atomic<void*> screen{nullptr};
Logger log_line = nullptr;

std::int64_t __fastcall hooked_register_keys(void* dialogue_screen) {
  screen.store(dialogue_screen);
  return original_register_keys(dialogue_screen);
}

}  // namespace

bool install(Logger logger) {
  log_line = logger;
  if (GetEnvironmentVariableW(L"KCDS_TEST_HOOKS", nullptr, 0) == 0) {
    return false;
  }
  void* register_keys = game_client::resolve_unique(
      "TestDialogueKeys",
      "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 70 20 55 48 8D 68 A1 48 81 EC A0 00 00 "
      "00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 47 48 8B F9 E8 ?? ?? ?? ?? 48 8D 4D EF 48 89 7D "
      "0F "
      "48 8B 90 28 01 00 00 48 8B 72 60");
  cancel_handler = reinterpret_cast<CancelHandler>(game_client::resolve_unique(
      "TestDialogueCancelKey",
      "48 83 EC 28 48 8B 49 08 48 8D 54 24 30 48 83 C1 08 C7 44 24 30 04 00 00 00 E8"));
  if (!register_keys || !cancel_handler) {
    return false;
  }
  return MH_CreateHook(register_keys, reinterpret_cast<void*>(&hooked_register_keys),
                       reinterpret_cast<void**>(&original_register_keys)) == MH_OK &&
         MH_EnableHook(register_keys) == MH_OK;
}

bool cancel() {
  void* dialogue_screen = screen.load();
  if (!dialogue_screen || !cancel_handler) {
    return false;
  }
  // The handler's std::function object: its captured screen at + 8.
  void* handler[2] = {nullptr, dialogue_screen};
  cancel_handler(handler);
  return true;
}

}  // namespace kcds::runtime::testing::dialog_cancel
