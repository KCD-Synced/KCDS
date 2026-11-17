// WHGame 1.5.5 development client. Engine work runs on CCryAction::Update, never Present.
// The server selects the map; the native debug helper starts it without loading a save.
// This file is the client's startup: DllMain, and initialize_adapter() installing every feature
// in order. The engine frame is core/frame.cpp.
// Winsock before Windows.h (see client_internal.hpp).
#include "core/client_internal.hpp"
#include "voice/voice_client.hpp"
#include "voice/voice_settings_page_install.hpp"

#include "MinHook.h"
#include "core/signature_store.hpp"
#include "characters/remote_target_scan.hpp"
#include "characters/body_collisions.hpp"
#include "characters/rested_updates.hpp"
#include "action_hints/action_hints_install.hpp"
#include "animation/animation_targets.hpp"
#include "animation/player_animation_install.hpp"
#include "beds/stay_in_bed.hpp"
#include "body_state/body_state_install.hpp"
#include "book/book_install.hpp"
#include "branding/runtime_branding.hpp"
#include "character_creator/creator_install.hpp"
#include "characters/skinning_capacity.hpp"
#include "characters/material_capacity.hpp"
#include "characters/skinning_metrics.hpp"
#include "characters/pipeline_metrics.hpp"
#include "chat/chat_install.hpp"
#include "combat/combat_natives_install.hpp"
#include "combat/guard_opponent.hpp"
#include "combat/combat_targets.hpp"
#include "core/client_state.hpp"
#include "core/engine_loop.hpp"
#include "core/engine_natives.hpp"
#include "core/natives.hpp"
#include "core/game_module.hpp"
#include "appearance/brand.hpp"
#include "core/log.hpp"
#include "core/natives.hpp"
#include "core/startup_targets.hpp"
#include "crash/crash_reporting.hpp"
#include "audio/audio_install.hpp"
#include "follow/follow_install.hpp"
#include "gizmos/gizmos_install.hpp"
#include "nameplates/nameplates_install.hpp"
#include "hud/hud_checks.hpp"
#include "hud/resource_hud_install.hpp"
#include "hud/tutorial_hint.hpp"
#include "input/input_install.hpp"
#include "instances/multiple_instances.hpp"
#include "knockout/knockout_install.hpp"
#include "look/look_install.hpp"
#include "markers/markers_install.hpp"
#include "movement/movement_install.hpp"
#include "native_ui/native_ui_install.hpp"
#include "keybinds/keybinds_install.hpp"
#include "noclip/noclip_install.hpp"
#include "npcs/npc_suppression.hpp"
#include "progression/progression_install.hpp"
#include "lights/lights_tick.hpp"
#include "props/props_install.hpp"
#include "replicas/replica_guards_install.hpp"
#include "replicas/replica_role_install.hpp"
#include "replicas/replica_targets.hpp"
#include "riding/rider_install.hpp"
#include "seats/seats_install.hpp"
#include "session/early_hooks.hpp"
#include "session/network_session.hpp"
#include "session/process_hooks.hpp"
#include "session/server_browser.hpp"
#include "session/session_guards_install.hpp"
#include "stats/stats_install.hpp"
#include "testing/test_hooks.hpp"
#include "video/bink_skip.hpp"
#include "weapons/weapons_install.hpp"
#include "world/single_player_screens_install.hpp"
#include "world_map/map_reveal.hpp"
#include "world_map/world_map_install.hpp"

namespace kcds::game_client {

DWORD WINAPI initialize_adapter() {
  const std::wstring dir = module_directory();
  // Outside loader lock: this is the DLL's own initialization thread.
  log("%s", kcds::runtime::crash::start_crash_reporting(dir, client_log_path()).c_str());
  if (find_game_module() != 0)
    return 1;
  if (!kcds::runtime::characters::materials::install(image, image_size, log)) {
    log("[materials] capacity override unavailable; retaining native buffer capacity");
  }
  kcds::runtime::characters::materials::report_status();
  if (!kcds::runtime::characters::skinning::install(image, image_size, log)) {
    log("[skinning] capacity override unavailable; retaining native buffer capacities");
  }
  kcds::runtime::characters::skinning::report_status();
  kcds::runtime::characters::skinning::metrics::install(image, image_size, log);
  StartupTargets targets;
  install_early_hooks(targets);
  // Before the main menu's video plays: its frame catch-up could hang the world load.
  kcds::runtime::video::bink_skip::install(log);
  if (resolve_map_reveal_natives() != 0)
    return 1;
  if (resolve_horse_targets(targets) != 0)
    return 1;
  resolve_engine_natives();
  if (resolve_animation_targets(targets) != 0)
    return 1;
  resolve_replica_targets(targets);
  resolve_combat_targets(targets);
  install_perfect_block_hooks();
  install_dodge_hooks();
  kcds::runtime::combat::guard_opponent::install(log);
  kcds::runtime::appearance::brand::install(
      resolve_unique(
          "BrandingState",
          "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B FA 48 8B D9 39 91 E8 00 00 00 "
          "0F 84 A9 00 00 00 48 8B 89 88 00 00 00 E8"),
      log);
  resolve_contact_natives();
  resolve_item_effect_natives();
  if (check_startup_targets(targets) != 0)
    return 1;
  if (open_network() != 0)
    return 1;
  if (install_test_hooks() != 0)
    return 1;
  AddVectoredExceptionHandler(1, observe_engine_exception);
  create_trample_replay_event();
  if (install_replica_autonomy() != 0)
    return 1;
  if (resolve_neutral_combat_targets(targets) != 0)
    return 1;
  if (resolve_rider_completion_natives() != 0)
    return 1;
  if (resolve_map_ui_targets(targets) != 0)
    return 1;
  if (resolve_look_natives() != 0)
    return 1;
  auto mh = MH_Initialize();
  if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED)
    return 1;
  begin_hook_batch();
  if (install_knockout_and_world_hooks() != 0)
    return 1;
  if (install_progression() != 0)
    return 1;
  install_body_state();
  install_player_animation();
  if (install_movement() != 0)
    return 1;
  if (install_look_hooks() != 0)
    return 1;
  if (install_neutral_attack_hooks(targets) != 0)
    return 1;
  if (install_interactive_state_hooks(targets) != 0)
    return 1;
  install_player_state_guard();
  if (install_neutral_combat_hooks(targets) != 0)
    return 1;
  if (install_map_ui_hooks(targets) != 0)
    return 1;
  if (install_player_input_hook(targets) != 0)
    return 1;
  if (install_single_player_screen_hooks() != 0)
    return 1;
  verify_engine_scripts();
  install_resource_hud();
  resolve_dialogue_and_view_camera();
  install_action_hints();
  install_world_map();
  install_character_creator();
  // The tutorial hint: the game's tutorial calls on the HUD.
  kcds::runtime::hud::tutorial_hint::install(log);
  // The game's NPCs stay out until NPCs are synced (0.2.0): hooked_spawn refuses them.
  kcds::runtime::npcs::install(log);
  install_replica_role();
  install_remote_target_scan();
  install_rested_updates();
  install_body_collisions();
  if (failed) {
    return 1;
  }
  install_seats();
  // In a bed the player stays lying until Get up (no sleep in a multiplayer world).
  kcds::runtime::beds::install(log);
  install_weapons();
  install_noclip();
  // Resource actions in the game's key binding settings.
  install_keybinds();
  install_book_and_shared_natives_users();
  install_native_ui();
  // Voice chat rows in the game's own Settings > Sound page.
  install_voice_settings_page();
  install_props_and_crafting();
  // Point lights the server and the client scripts place.
  install_lights();
  install_gizmos();
  install_nameplates();
  install_audio();
  install_follow();
  if (install_native_chat(targets) != 0)
    return 1;
  if (install_animation_layer_hooks(targets) != 0)
    return 1;
  install_body_position_hooks();
  if (install_animation_hooks(targets) != 0)
    return 1;
  if (install_horse_hooks(targets) != 0)
    return 1;
  if (install_seated_riders() != 0)
    return 1;
  if (install_horse_reservations() != 0)
    return 1;
  install_player_horses();
  if (install_replica_hooks(targets) != 0)
    return 1;
  // Combat's hooks: with combat off (check_startup_targets) every hook whose function this game
  // build still has goes in all the same, since the replicas' own setup leans on them; a failure
  // then only leaves that hook out. The per-frame combat work stays off (frame.cpp).
  const bool combat = combat_available.load();
  if (install_native_hit_hooks(targets) != 0 && combat)
    return 1;
  install_hit_pricing();
  install_trample_diagnostics();
  if (install_stats_and_npcs() != 0)
    return 1;
  if (install_combat_hooks(targets) != 0 && combat)
    return 1;
  install_optional_combat_guards();
  if (install_combat_input_hooks(targets) != 0 && combat)
    return 1;
  if (install_engine_loop_hook() != 0)
    return 1;
  kcds::runtime::characters::pipeline_metrics::install();
  install_session_guards();
  install_departure_hooks();
  end_hook_batch();
  hooks_installed.store(true);
  save_signature_store();
  return start_network_worker();
}
}  // namespace kcds::game_client
using namespace kcds::game_client;
// The adapter's initialization thread. A failed start is reported to the server browser, which
// otherwise waits for the network before it dials a server.
DWORD WINAPI initialize(void*) {
  const DWORD result = initialize_adapter();
  if (result != 0) {
    end_hook_batch();
    // Every step but the network worker's start (2) fails for a game function that was not found
    // or not hooked: the game's build is not one this release supports, and nothing joins
    // The steps that failed are in the log above.
    game_unsupported.store(result == 1);
    adapter_failed.store(true);
    log("ADAPTER start failed (%s); no server can be joined",
        result == 1 ? "a game function was not found: unsupported game version"
                    : "the network worker did not start");
  }
  return result;
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    module = h;
    DisableThreadLibraryCalls(h);
    open_log();
    // Before the game's main thread resumes: the exe's single-instance check runs first thing,
    // and the loader watch must see WHGame.dll being loaded.
    kcds::runtime::instances::allow_multiple_instances(log);
    kcds::runtime::branding::start(module_directory(), log);
    auto t = CreateThread(nullptr, 0, initialize, nullptr, 0, nullptr);
    if (t)
      CloseHandle(t);
  } else if (reason == DLL_PROCESS_DETACH && reserved) {
    // ExitProcess has terminated other threads and holds the loader lock.
    // Do not run thread-joining destructors here. Windows closes our job handles
    // during process teardown and kills the owned resource-process tree.
    // Tell the server this player left so others remove the avatar at once instead of after the
    // inactivity timeout. disconnect() is lock-free, allocation-free and non-blocking; it must not
    // take network_mutex here because a terminated thread may still own it.
    // The game may close before the client exists (a store copy that starts itself again).
    if (client) {
      kcds::runtime::voice::session_ended();
      client->disconnect();
    }
    client.release();
    winsock.release();
  }
  return TRUE;
}
