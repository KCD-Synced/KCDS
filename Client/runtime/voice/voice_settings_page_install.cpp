// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "voice_settings_page_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "voice_settings_page.hpp"

namespace kcds::game_client {

void install_voice_settings_page() {
  // The game's Settings > Sound page and its two row events (voice_settings_page.hpp).
  kcds::runtime::voice::settings_page::Natives natives;
  // mov dl,0Eh (page 14) into the page preparation right after the prologue.
  natives.sound_page = resolve_unique(
      "SoundSettingsPage",
      "48 8B C4 48 89 58 18 48 89 78 20 55 48 8D 68 A1 48 81 EC B0 00 00 00 48 8B D9 0F 29 70 E8 "
      "48 8B 89 30 03 00 00 B2 0E");
  // the three volume variables at +258h, +260h and +250h.
  natives.read_volumes = resolve_unique(
      "SoundSettingsVolumes",
      "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B FA 48 8B 89 58 02 00 00 48 8B 01 FF 50 20 48 "
      "8B 8B 60 02 00 00 F3 0F 11 47 04");
  // keeps the value (xmm2) in xmm6 before the page state check.
  natives.value_event = resolve_unique(
      "SettingsInteractiveValue",
      "48 89 5C 24 10 57 48 83 EC 30 48 8B D9 0F 29 74 24 20 48 8B 89 30 03 00 00 0F 28 F2 48 83 "
      "C1 58 48 8B FA");
  // keeps the choice (r8d) in esi before the page state check.
  natives.choice_event = resolve_unique(
      "SettingsInteractiveChoice",
      "40 56 57 41 56 48 83 EC 20 48 8B F9 41 8B F0 48 8B 89 30 03 00 00 4C 8B F2 48 83 C1 58 48 "
      "8B 01 FF 50 48 3C 05 75");
  // stores the three volumes at +230h first.
  natives.apply_volumes = resolve_unique(
      "SoundSettingsApplyVolumes",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 F2 0F 10 02 41 8A D8 F2 0F 11 81 30 02 00 00 "
      "48 8B FA 8B 42 08 48 8B F1 89 81 38 02 00 00");
  natives.log = log;
  const bool installed = kcds::runtime::voice::settings_page::install(natives);
  log("VOICE settings page %s", installed ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
