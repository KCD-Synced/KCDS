// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "world_start.hpp"

#include "load_progress.hpp"

#include <cstring>
#include <mutex>
#include <string>

#include "../core/client_state.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../discord/discord_presence.hpp"
#include "../gizmos/gizmos.hpp"
#include "../identity/nickname.hpp"
#include "../maps/custom_maps.hpp"
#include "../markers/blips.hpp"
#include "../native_ui/native_ui.hpp"
#include "../quests/quests.hpp"
#include "../world_map/resource_world_map.hpp"
#include "kcds/menu/compositor.hpp"
#include "profile_settings.hpp"
#include "world_reset.hpp"

namespace kcds::game_client {

namespace sdk = kcd2s::sdk;

// How long a world waits for the server's client resources before it loads without them.
constexpr double kResourcesWaitSeconds = 120;

// Whether the world may load: once the server's client resources run, so the level reads their
// replace/ files (a texture, a model) instead of the game's. The server sends the
// world when it admits the player, while the resources may still download. A resource runtime
// that failed, or resources not ready after kResourcesWaitSeconds, do not hold the world.
bool resources_let_world_load(std::uint32_t revision, double now) {
  static std::uint32_t waiting_revision = 0;
  static double waiting_since = 0;
  bool ready = false;
  bool failed = false;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    // World Architect's and the studio's local mode: no server, so no server resources to wait
    // for (it waited the whole kResourcesWaitSeconds before its world loaded).
    if (client->offline()) {
      return true;
    }
    ready = client->resources_ready();
    failed = client->resource_runtime_failed();
  }
  if (ready) {
    return true;
  }
  if (waiting_revision != revision) {
    waiting_revision = revision;
    waiting_since = now;
    log("SERVER WORLD revision=%u waits for the client resources", revision);
  }
  if (failed || now - waiting_since >= kResourcesWaitSeconds) {
    static std::uint32_t reported = 0;
    if (reported != revision) {
      reported = revision;
      log("SERVER WORLD revision=%u loads without the client resources (%s)", revision,
          failed ? "the resource runtime failed" : "not ready in time");
    }
    return true;
  }
  return false;
}

void request_server_world(void* framework, void* entity_system,
                          const kcd2s::integration::WorldConfig& world, double now) {
  if (!resources_let_world_load(world.revision, now)) {
    return;
  }
  // A custom map loads once the game lists it (it may still be coming from the server).
  if (kcds::runtime::maps::custom::is_custom(world.map) &&
      !kcds::runtime::maps::custom::listed(world.map)) {
    return;
  }
  // A fast travel that takes the player to another map ends, and its map screen closes, before
  // the level loads.
  if (kcds::runtime::world_map::hold_world_change()) {
    return;
  }
  reset_world_replicas(entity_system);
  // The client resources' drawings stay on their own level.
  if (kcds::runtime::gizmos::level(world.map)) {
    std::lock_guard<std::mutex> lock(network_mutex);
    client->send_to_resource_runtime(
        sdk::Value(sdk::Value::Map{{"op", sdk::Value("gizmosCleared")}}));
  }
  void* services = get_services();
  auto helper = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(services) + 208);
  if (!helper)
    return;
  auto console = *console_slot;
  if (!console)
    return;
  static double profile_wait_started = 0;
  if (!profile_settings_loaded.load()) {
    if (profile_wait_started == 0)
      profile_wait_started = now;
    if (original_load_profile_settings && now - profile_wait_started < kProfileSettingsWaitSeconds)
      return;
    static bool reported = false;
    if (!reported) {
      reported = true;
      log("PROFILE settings load not observed; starting with the current EULA value");
    }
  }
  // Start the debug world only after the game has loaded an existing EULA
  // confirmation (or the user completes the native overlay). Never accept it.
  auto eula_confirmed = slot<void*(__fastcall*)(void*, const char*)>(console, 23)(
      console, "wh_ui_eulaConfirmedVersion");
  auto eula_current = slot<void*(__fastcall*)(void*, const char*)>(console, 23)(
      console, "wh_ui_eulaCurrentVersion");
  if (!eula_confirmed || !eula_current)
    return;
  const auto confirmed = slot<int(__fastcall*)(void*)>(eula_confirmed, 2)(eula_confirmed);
  const auto required = slot<int(__fastcall*)(void*)>(eula_current, 2)(eula_current);
  static double next_eula_log = 0;
  if (now >= next_eula_log) {
    next_eula_log = now + 5;
    log("PROFILE EULA confirmed=%d required=%d", confirmed, required);
  }
  if (confirmed < required)
    return;
  // wh_sys_HideLoadingScreen ("Hides the loading screen when loading a saved game")
  // is left at its default so the native loading screen with its progress marker
  // shows while the multiplayer world loads.
  for (const auto* setting : {"wh_ui_DisableNewGameDebugFader"}) {
    auto var = slot<void*(__fastcall*)(void*, const char*)>(console, 23)(console, setting);
    if (!var) {
      log("Required debug cvar unavailable: %s", setting);
      failed = true;
      return;
    }
    slot<void(__fastcall*)(void*, int)>(var, 7)(var, 1);
  }
  if (auto tutorials = slot<void*(__fastcall*)(void*, const char*)>(console, 23)(
          console, "wh_ui_TutorialsEnabled"))
    slot<void(__fastcall*)(void*, int)>(tutorials, 7)(tutorials, 0);
  // Preserve the game's hostility and manual combat input rules. The
  // LockOnAllTargets development setting makes proximity trigger combat.
  alignas(8) std::uint8_t params[144]{};
  params_init(params);
  params[0] = 1;
  string_assign(params + 136, world.map.c_str());
  log("SERVER WORLD map=%s debug=%d quests=%d revision=%u", world.map.c_str(), world.debug,
      world.quests, world.revision);
  auto level_system = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + 1296);
  if (!level_system)
    return;
  load_counter_before =
      *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(level_system) + 96);
  requested_revision = world.revision;
  multiplayer_world.store(true);
  loading_down_since_request.store(false);
  kcds::runtime::session::load_progress::reset();
  // The resources' stream/ and replace/ packs go in before the level loads, so the level reads a
  // resource's texture or model in place of the game's. The runtime sends their
  // mount requests before it reports the resources ready, which this request waits for.
  kcds::runtime::native_ui::mount_pending();
  auto accepted = slot<char(__fastcall*)(void*, const void*)>(helper, 0)(helper, params);
  string_assign(params + 136, "");
  log("Native NewGameDebug request accepted=%d; waiting for local actor", accepted);
  // The world loads now: the page hides (it stays loaded for a Disconnected popup).
  kcds::menu::set_visible(false);
  world_requested.store(true);
  if (!accepted) {
    failed = true;
    log("Debug startup refused; no fallback to campaign");
  }
}

void finish_world_load(void* framework, void* services,
                       const kcd2s::integration::WorldConfig& world, double now, double last_diag,
                       std::uint32_t local_id, bool& finished) {
  auto level_system = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + 1296);
  if (!level_system)
    return;
  auto count = *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(level_system) + 96);
  auto name = *reinterpret_cast<const char**>(static_cast<std::uint8_t*>(level_system) + 72);
  if (count == load_counter_before || !name)
    return;
  if (now - last_diag < 0.1)
    log("READINESS loaded level=%s", name);
  std::string loaded(name);
  auto slash = loaded.find_last_of("/\\");
  if (slash != std::string::npos)
    loaded.erase(0, slash + 1);
  if (_stricmp(loaded.c_str(), world.map.c_str()) != 0)
    return;
  auto new_game_helper = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(services) + 208);
  engine_stage = "finish native debug startup";
  slot<void(__fastcall*)(void*)>(new_game_helper, 1)(new_game_helper);
  log("Native debug startup completed; NewGameStart(debug=1) dispatched");
  // The quests of the world before are gone with it; the server sends this world's when the
  // world is ready.
  kcds::runtime::quests::forget_all();
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    client->peer().mark_world_ready(world.revision);
    // The name the player connects under; the server keeps the first one.
    client->peer().send_nickname(kcds::runtime::identity::local_nickname());
    // What this game build gave the client: the functions found and those lost to a patch.
    const NativeLookups lookups = native_lookups();
    kcd2s::integration::ClientNatives natives;
    natives.found = std::uint32_t(lookups.found);
    natives.combat = combat_available.load();
    natives.riding = riding_available.load();
    for (const std::string& name : lookups.missing) {
      if (!name.empty() && name.size() <= kcd2s::integration::kMaxClientNativeNameBytes &&
          natives.missing.size() < kcd2s::integration::kMaxClientNativeNames) {
        natives.missing.push_back(name);
      }
    }
    client->peer().send_client_natives(natives);
  }
  multiplayer_world_ready.store(true);
  kcds::runtime::markers::redraw_own_blips();
  log("WORLD READY local actor=0x%x", local_id);
  finished = true;
}

void report_gameplay_ready(const kcd2s::integration::WorldConfig& world, double now) {
  kcds::runtime::discord::entered_world();
  // playerReady: report once per world when the game took its loading screen down. If that is not
  // observed for this world, report 10 s after world-ready instead.
  static std::uint32_t reported_revision = 0;
  static double world_ready_at = 0;
  if (reported_revision != world.revision) {
    if (world_ready_at == 0)
      world_ready_at = now;
    const bool screen_down = loading_down_since_request.load();
    const bool fallback = !screen_down && now - world_ready_at > 10;
    if (screen_down || fallback) {
      bool sent;
      {
        std::lock_guard<std::mutex> lock(network_mutex);
        sent = client->peer().mark_gameplay_started(world.revision);
      }
      if (sent) {
        reported_revision = world.revision;
        world_ready_at = 0;
        log("GAMEPLAY READY revision=%u source=%s", world.revision,
            screen_down ? "loading screen" : "fallback");
      }
    }
  }
}

}  // namespace kcds::game_client
