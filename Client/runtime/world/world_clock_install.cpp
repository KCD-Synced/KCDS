// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "world_clock_install.hpp"

#include <cstdint>
#include <cstring>

#include "../branding/loading_tail.hpp"
#include "../core/client_state.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../session/load_progress.hpp"
#include "../winter/roof_occluders.hpp"
#include "../winter/shader_compile.hpp"
#include "../winter/snow_stage.hpp"
#include "interior.hpp"
#include "trespass.hpp"
#include "world_clock.hpp"
#include "world_weather.hpp"

namespace kcds::game_client {

namespace {

// Where the game's code sets the snow pass's stencil test (winter/snow_stage.hpp): its stencil
// setter called with ref and mask 0x80, write mask 0xFF and the test 6 (equal).
void* snow_stencil_site() {
  return resolve_unique("SnowPassStencil",
                        "41 B1 80 C6 44 24 20 FF 45 8A C1 BA 06 00 00 00 48 8B CB E8 ?? ?? ?? ?? "
                        "48 8B 86 48 03 00 00");
}

}  // namespace

void install_world_clock() {
  // The world clock: the game's calendar (the global object + 0x88) and the calls
  // its own Calendar.SetWorldTime / SetWorldTimeRatio script functions make.
  kcds::runtime::world::Natives world_clock;
  world_clock.game_globals = static_cast<void* const*>(
      resolve_unique("CalendarGlobals",
                     "40 53 48 83 EC 20 83 7A 18 00 75 38 48 8B 05 ?? ?? ?? ?? 48 8B 98 88 00 00 "
                     "00",
                     15, 19));
  world_clock.set_time = resolve_unique(
      "CalendarSetTime",
      "48 89 5C 24 08 57 48 83 EC 20 48 8B 79 48 48 8B D9 48 3B D7 7C 66 48 89 51 48 B2 01 C7 "
      "81 88 00 00 00 00 00 00 00 E8");
  world_clock.set_ratio = resolve_unique(
      "CalendarSetRatio",
      "40 53 48 83 EC 60 0F 29 74 24 50 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 48 0F 28 F1 "
      "48 8B D9 F3 0F 11 71 60 48 8B 05 ?? ?? ?? ?? 48 8B 90 A8 00 00 00");
  log("WORLD clock %s",
      kcds::runtime::world::install(world_clock, log) ? "installed" : "unavailable");
  // The world weather: the environment module's getter, as the call at the start
  // of Weather.ChangeWeather's implementation reaches it (E8 at + 16), and the
  // 3D engine the HUD block found.
  kcds::runtime::world::WeatherNatives world_weather;
  world_weather.get_environment = resolve_unique(
      "EnvironmentModule",
      "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 E8 ?? ?? ?? ?? 48 8B 88 38 01 00 00 48 "
      "8B 01 FF 50 40",
      17, 21);
  world_weather.engine_3d_slot = engine_3d_slot;
  // The game's own percentage of a level's load, for a server's loading page.
  kcds::runtime::session::load_progress::install(
      resolve_unique("LoadingSectionSetProgress",
                     "48 83 EC 28 66 0F 7E C8 87 41 10 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 13 48 8B "
                     "01 48 8D 15"),
      log);
  // The level loading screen keeps its spinner and hint to the end (branding/loading_tail.hpp).
  kcds::runtime::branding::loading_tail::install(log);
  log("WORLD weather %s",
      kcds::runtime::world::install_weather(world_weather, log) ? "installed" : "unavailable");
  // Snow: the renderer's deferred snow stage's update (winter/snow_stage.hpp).
  log("WINTER snow %s",
      kcds::runtime::winter::snow::install(
          resolve_unique("SnowStageUpdate",
                         "48 8B 41 28 83 B8 E0 8E 00 00 01 0F 84 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? "
                         "4C 8D 15 ?? ?? ?? ?? 41 B9 02 00 00 00"),
          // The stage's is-active check: r_snow (+ 940) above 0 and post effects on; the jump
          // to its second test is part of the pattern (another stage's check reads the same).
          resolve_unique("SnowStageActive",
                         "48 8B 51 20 33 C0 39 82 AC 03 00 00 0F 8F 98 7E A7 01"),
          // The settings' r_snow question, as the stage's init calls it first (E8 at + 17).
          resolve_unique("SnowSettingsEnabled",
                         "48 89 5C 24 10 57 48 83 EC 20 48 8B D9 48 8B 49 20 E8 ?? ?? ?? ?? 84 C0 "
                         "0F 84 D7 00 00 00",
                         18, 22),
          snow_stencil_site(), console_slot, log)
          ? "installed"
          : "unavailable");
  // The game's own DXC compile (winter/shader_compile.hpp): its prologue, then its
  // DxcCreateInstance call.
  log("WINTER shader compile %s",
      kcds::runtime::winter::shader_compile::install(
          resolve_unique("ShaderDxcCompile",
                         "48 89 5C 24 08 48 89 74 24 10 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC "
                         "70 48 8B 7D 50 45 33 E4 4D 89 21 49 8B F1 49 8B D8 4C 8B F2 4C 8B F9 48 "
                         "85 FF 74 03 4C 89 27"),
          log)
          ? "installed"
          : "unavailable");
  // Snow under no roof: the renderer's rain occlusion update (winter/roof_occluders.hpp).
  log("WINTER roof occlusion %s",
      kcds::runtime::winter::roof_occluders::install(
          resolve_unique("RainOcclusionUpdate",
                         "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B F1 "
                         "E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8D 2D "
                         "?? ?? ?? ?? 8B 81 A4 2E 00 00"),
          log)
          ? "installed"
          : "unavailable");
  // The players a server allows in private areas: the game's own "no reactions to
  // trespass" context query, as the reaction node reaches it (the second E8).
  log("WORLD trespass %s",
      kcds::runtime::world::trespass::install(
          resolve_unique("TrespassReactionsDisabled",
                         "E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 0F 85 97 03 00 00 84 DB 0F 85 8F 03 "
                         "00 00 E8",
                         6, 10),
          log)
          ? "installed"
          : "unavailable");
  // Whether the local player is in a building, for the voice rooms (world/interior.hpp): the
  // calls of the horse call's "interior" check (+ 0x60), each the E8 at its offset.
  constexpr const char* kInteriorCheck =
      "E8 ?? ?? ?? ?? 48 8B 98 60 01 00 00 E8 ?? ?? ?? ?? 48 8B 8B A8 00 00 00 48 8B F0 48 8B 11 "
      "FF 52 10 48 8B 08 48 8B 11 48 8B C8 FF D2 48 8D 15 ?? ?? ?? ?? 48 8B E8 48 8D 4C 24 ?? "
      "E8 ?? ?? ?? ?? 4C 8D 44 24 ?? 48 8B D5 48 8B CE E8 ?? ?? ?? ?? 48 8D 4C 24 ?? 0F B6 D8 E8 "
      "?? ?? ?? ??";
  kcds::runtime::world::interior::Natives interior;
  interior.get_environment = resolve_unique("InteriorEnvironment", kInteriorCheck, 1, 5);
  interior.contexts = resolve_unique("InteriorContexts", kInteriorCheck, 13, 17);
  interior.make_string = resolve_unique("InteriorMakeString", kInteriorCheck, 60, 64);
  interior.has_context = resolve_unique("InteriorHasContext", kInteriorCheck, 76, 80);
  interior.free_string = resolve_unique("InteriorFreeString", kInteriorCheck, 89, 93);
  kcds::runtime::world::interior::install(interior, log);
}

}  // namespace kcds::game_client
