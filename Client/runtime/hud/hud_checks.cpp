// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "hud_checks.hpp"

#include <cstdint>
#include <cstring>

#include "../core/log.hpp"
#include "../core/natives.hpp"

namespace kcds::game_client {

namespace {

// CScriptBind_System::GetViewCameraFov: ISystem vtable + 0x438 is
// GetViewCamera, the CCamera's vertical field of view at + 0x30 after its Matrix34.
constexpr const char* kViewCameraFovSignature =
    "40 53 48 83 EC 20 48 8B 49 60 48 8B DA 48 8B 01 FF 90 38 04 00 00 48 8D 54 24 30 48 8B CB "
    "F3 0F 10 40 30 F3 0F 11 44 24 30 E8";
constexpr const char* kIsInDialogSignature =
    "48 83 B8 68 06 00 00 00 74 ?? E8 ?? ?? ?? ?? 48 8B 88 50 01 00 00 48 8B 59 10 48 8B 8F 68 06 "
    "00 00 48 8B 01 FF 10 41 B1 01 48 8B CB 45 8A C1 48 8B 10 E8 ?? ?? ?? ??";
// The console's ExecuteStringInternal: its '#' path reads ISystem at +0x200, GetIScriptSystem
// at +0x278 and ExecuteBuffer at +0x30, the offsets hud/engine_script.cpp uses.
constexpr const char* kExecuteStringSignature =
    "48 8B 8F 00 02 00 00 48 8B 01 FF 90 F8 03 00 00 84 C0 74 ?? 48 8B 8F 00 02 00 00 48 8B 01 FF "
    "90 78 02 00 00 48 85 C0 74 ?? 48 8B 8F 00 02 00 00 48 8B 01 FF 90 78 02 00 00 48 8B 55 ?? 4C "
    "8D 0D ?? ?? ?? ?? 48 C7 44 24 20 00 00 00 00 48 8B 08 4C 63 42 F8 4C 8B 51 30";

}  // namespace

void verify_engine_scripts() {
  engine_scripts_verified =
      resolve_unique("ExecuteStringInternal", kExecuteStringSignature) != nullptr;
  log("HUD engine scripts %s", engine_scripts_verified.load() ? "verified" : "unavailable");
}

void resolve_dialogue_and_view_camera() {
  if (auto is_in_dialog =
          static_cast<std::uint8_t*>(resolve_unique("HumanIsInDialog", kIsInDialogSignature))) {
    std::int32_t displacement;
    std::memcpy(&displacement, is_in_dialog + 51, 4);
    dialogue_lookup = reinterpret_cast<DialogueLookup>(is_in_dialog + 55 + displacement);
  }
  log("HUD dialogue state %s", dialogue_lookup ? "resolved" : "unavailable");
  view_camera_verified = resolve_unique("GetViewCameraFov", kViewCameraFovSignature) != nullptr;
  log("HUD view camera %s", view_camera_verified.load() ? "verified" : "unavailable");
}

}  // namespace kcds::game_client
