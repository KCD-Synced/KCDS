// Winsock before Windows.h (see client_internal.hpp).
#include "client_internal.hpp"

#include "frame.hpp"

#include <chrono>
#include <mutex>
#include <tuple>

#include "../attach/player_attach.hpp"
#include "../animation/animation_capture.hpp"
#include "../animation/player_animation.hpp"
#include "../animation/upper_body.hpp"
#include "../input/cuffs.hpp"
#include "../audio/resource_audio.hpp"
#include "../follow/follow.hpp"
#include "../animation/animation_orders_tick.hpp"
#include "../body_state/body_snapshot.hpp"
#include "../body_state/body_state_tick.hpp"
#include "../characters/frame_metrics.hpp"
#include "../branding/runtime_branding.hpp"
#include "../buffs/buffs_tick.hpp"
#include "../camera/free_look.hpp"
#include "../camera/photo_mode_tick.hpp"
#include "../carry/body_carry_tick.hpp"
#include "../carts/carts_tick.hpp"
#include "../chat/chat_tick.hpp"
#include "../combat/combat_relay_tick.hpp"
#include "../combat/defence.hpp"
#include "../combat/defence_timing.hpp"
#include "../combat/keep_lock.hpp"
#include "../combat/melee_contacts.hpp"
#include "../cookpot/cookpot_tick.hpp"
#include "../crafting/crafting_tick.hpp"
#include "../dice/dice_tick.hpp"
#include "../voice/voice_client.hpp"
#include "../voice/voice_settings_page.hpp"
#include "../gathering/gathering_tick.hpp"
#include "../doors/doors_tick.hpp"
#include "../graves/graves_tick.hpp"
#include "../smart_objects/smart_objects_tick.hpp"
#include "../ground/ground_tick.hpp"
#include "../architect/level_doors.hpp"
#include "../architect/level_sounds.hpp"
#include "../architect/navmesh_bake.hpp"
#include "../kcd1/kcd1_map.hpp"
#include "../kcd1/interior_light.hpp"
#include "../kcd1/map_screen.hpp"
#include "../maps/custom_maps.hpp"
#include "kcds/menu_native/main_menu.hpp"
#include "../architect/local_level_edits.hpp"
#include "../architect/local_mode.hpp"
#include "../architect/player_body.hpp"
#include "../architect/world_look.hpp"
#include "../level/level_edits.hpp"
#include "../studio/studio_rope.hpp"
#include "../pose_animation/clip_in_place.hpp"
#include "../studio/game_files.hpp"
#include "../studio/studio_game_clip.hpp"
#include "../studio/studio_hold.hpp"
#include "../studio/studio_mesh.hpp"
#include "../studio/studio_skin.hpp"
#include "../studio/studio_outfit.hpp"
#include "../studio/studio_pose.hpp"
#include "../studio/studio_prop_gizmo.hpp"
#include "../studio/studio_event.hpp"
#include "../studio/studio_floor.hpp"
#include "../studio/studio_grid.hpp"
#include "../studio/studio_saves.hpp"
#include "../studio/studio_sky.hpp"
#include "../studio/studio_texture.hpp"
#include "../studio/studio_skeleton.hpp"
#include "../studio/studio_whip.hpp"
#include "../studio/studio_scene.hpp"
#include "../branding/watermark.hpp"
#include "../hud/hud_calls.hpp"
#include "../input/input_gate.hpp"
#include "../input/scripted_input.hpp"
#include "../inventory/inventory_tick.hpp"
#include "../knockout/dropped_weapons_tick.hpp"
#include "../knockout/knockout.hpp"
#include "../knockout/knockout_tick.hpp"
#include "../local_player/local_state_tick.hpp"
#include "../look/local_look.hpp"
#include "../movement/ladders_tick.hpp"
#include "../movement/movement_mode_tick.hpp"
#include "../npcs/npc_suppression.hpp"
#include "../npcs/local_npcs.hpp"
#include "../npcs/native_npcs.hpp"
#include "../pickpocket/pickpocket_tick.hpp"
#include "../pillory/pillory_tick.hpp"
#include "../progression/progression_tick.hpp"
#include "../props/previews_tick.hpp"
#include "../ranged/shots_tick.hpp"
#include "../replicas/held_items.hpp"
#include "../replicas/replica_spawn.hpp"
#include "../replicas/character_retirement.hpp"
#include "../replicas/replica_tick.hpp"
#include "../riding/lead_rope.hpp"
#include "../riding/native_horses.hpp"
#include "../riding/rider_diagnostics.hpp"
#include "../riding/trample.hpp"
#include "../session/leave_world.hpp"
#include "../session/process_hooks.hpp"
#include "../session/server_browser.hpp"
#include "../session/world_start.hpp"
#include "../siege/siege_tick.hpp"
#include "../stash/body_containers.hpp"
#include "../stats/player_command_binding.hpp"
#include "../testing/combat_tests.hpp"
#include "../testing/test_hooks.hpp"
#include "../vfx/placed_objects_tick.hpp"
#include "../keybinds/keybinds.hpp"
#include "../lights/lights_tick.hpp"
#include "../weapon_hits/weapon_hits_tick.hpp"
#include "../world_map/map_reveal.hpp"
#include "../session/ui_time_scale.hpp"
#include "client_state.hpp"
#include "engine_loop.hpp"
#include "log.hpp"
#include "natives.hpp"

namespace kcds::game_client {
// The local actor's soul (C_Actor + 1640, as tick_knockout reads it).
constexpr std::size_t kLocalActorSoul = 1640;

namespace local = kcd2s::local;

// The client's frame on the engine update thread (core/engine_loop.cpp calls it after the
// game's update): the world started and the local player found, then every feature's tick in
// order, sharing one Frame.
void frame() {
  namespace metrics = kcds::runtime::characters::frame_metrics;
  metrics::Phases phases;
  engine_update_thread.store(GetCurrentThreadId());
  install_pending_human_overlay();
  invalidate_human_overlays_after_pak_change();
  engine_stage = "frame start";
  tick_swing_hold();
  tick_defence_catch_ups();
  tick_pending_perfect_blocks();
  quit_if_requested();
  leave_world_if_requested();
  // Resource actions on the game's key binding screen, in the menus too.
  engine_stage = "key bindings";
  kcds::runtime::keybinds::frame();
  // The game's own settings, asked by the server browser's gear; a test kit's way to the KCDS
  // page (KCDS_TEST_HOOKS).
  engine_stage = "menu settings";
  if (signaled(kcds_settings_event)) {
    kcds::menu_native::open_settings();
    kcds::menu_native::request_kcds_page();
  }
  kcds::menu_native::frame();
  // Custom maps a server's resource brought: listed while the join waits for them, before the
  // world loads.
  if (void* framework = *framework_slot) {
    kcds::runtime::maps::custom::frame(framework);
  }
  kcds::runtime::voice::frame();
  kcds::runtime::voice::settings_page::frame();
  // The server browser picks the server first; the engine thread waits without blocking.
  if (!client_ready.load()) {
    kcds::runtime::branding::watermark::set_in_world(false);
    return;
  }
  tick_chat();
  sync_chat_input_gate();
  sync_web_pointer();
  // The frame's start, not this moment after the game's update: the update's length varies by
  // tens of milliseconds in a crowd, and the bodies placed for this frame moved by that much more
  // or less than the frame really lasted.
  const double now = engine_frame_started > 0.0 ? engine_frame_started : local::clock_seconds();
  phases.mark(metrics::Phase::Setup);
  // The frame's own copy of the mirror: only what a snapshot changed is copied, since a whole copy
  // of a crowd's strings each frame cost more than some of the crowd's game work.
  static MirrorEntities entities;
  auto [config, world_ready, own] = [&] {
    std::lock_guard<std::mutex> lock(network_mutex);
    auto& peer = client->peer();
    const auto& mirrored = peer.mirror().entities();
    for (auto it = entities.begin(); it != entities.end();) {
      if (mirrored.find(it->first) == mirrored.end()) {
        it = entities.erase(it);
      } else {
        ++it;
      }
    }
    for (const auto& [id, entity] : mirrored) {
      auto [mine, added] = entities.try_emplace(id, entity);
      if (!added && mine->second.revision != entity.revision) {
        mine->second = entity;
      }
    }
    // The watermark's server line: the server's name when it tells it, the ping.
    if (auto name = peer.take_server_name()) {
      kcds::runtime::branding::watermark::set_server_name(*name);
    }
    kcds::runtime::branding::watermark::set_ping(peer.rtt_seconds());
    return std::make_tuple(peer.world_config(), peer.world_ready(), client->own_avatar());
  }();
  phases.mark(metrics::Phase::Mirror);
  // World Architect's local mode: no server, so the client names its own world and the local
  // player is in it without a server avatar.
  const bool local_mode = kcds::runtime::architect::local_mode::enabled();
  if (local_mode) {
    config = kcds::runtime::architect::local_mode::world();
    world_ready = kcds::runtime::architect::local_mode::world_ready();
  }
  const bool in_world = world_ready && (own != core::kInvalidEntity || local_mode);
  run_hud_calls(in_world);
  // The watermark is for the loaded world only: not the menus over it, nor the game's loading
  // screen, which stays up for seconds after the world is ready and comes back with a teleport
  // (its "loading" action map, loading_down_since_request).
  kcds::runtime::branding::watermark::set_in_world(in_world && multiplayer_world_ready.load() &&
                                                   loading_down_since_request.load() &&
                                                   !page_over_world.load());
  if (local_mode) {
    // The editor opens under the loading screen, so the player lands in it.
    const bool ready = in_world;
    std::lock_guard<std::mutex> lock(network_mutex);
    const unsigned runtime = client->resource_runtime_generation();
    kcds::runtime::architect::local_mode::frame(
        ready, runtime,
        [](const kcd2s::sdk::Value& message) { return client->send_to_resource_runtime(message); });
    kcds::runtime::architect::world_look::frame(ready);
  }
  // The editor's Leave: back to the main menu and its server list (session/server_browser.hpp).
  if (local_mode && kcds::runtime::architect::local_mode::take_leave()) {
    leave_tool();
  }
  static bool first = true;
  if (first) {
    log("GAME UPDATE thread=%lu", GetCurrentThreadId());
    kcds::runtime::branding::report_applied_settings();
    first = false;
  }
  void* framework = *framework_slot;
  if (!framework)
    return;
  // CCryAction::GetIActorSystem (vtable 25) returns this+1304, confirmed in.
  auto actor_system = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + 1304);
  if (!actor_system)
    return;
  auto entity_system = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor_system) + 48);
  bool spawn_hook_finished = false;
  install_spawn_hook(entity_system, spawn_hook_finished);
  if (!spawn_hook_finished)
    return;
  if (!config)
    return;
  const auto world = *config;
  // The level's own people (issue 016), before the level spawns them.
  kcds::runtime::npcs::set_level_people(world.level_people);
  // NPCs far from the player walk with their animation in every world (the camera observes).
  kcds::runtime::npcs::keep_camera_observer();
  // The world keeps its pace while the player is in the inventory or another game screen.
  kcds::runtime::session::keep_ui_time_scale();
  if (requested_revision != world.revision) {
    // World Architect's local mode loads its world straight away, with no main menu of ours. The
    // game's own first-start pages (EULA, telemetry) open when it enters its main menu: the world
    // waits until the player has answered them (nothing is answered for the player), or the
    // pages would stay open over the level, their buttons under the editor's panels.
    if (local_mode && kcds::menu_native::first_time_overlay_open()) {
      return;
    }
    request_server_world(framework, entity_system, world, now);
    return;
  }
  // obtains the WH local C_Player via GameInterface slot 64.
  // CGameObjectExtensionHelper<C_Actor,IActor,64> stores engine entity at +56.
  // CryActorSystem's client IDs remain zero in this debug startup and are not
  // the authoritative WH local-player reference.
  if (!entity_system)
    return;
  auto services = get_services();
  auto game_interface = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(services) + 8);
  if (!game_interface)
    return;
  auto local_actor = slot<void*(__fastcall*)(void*)>(game_interface, 64)(game_interface);
  auto local_entity = local_actor
                          ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(local_actor) + 56)
                          : nullptr;
  auto local_id =
      local_entity
          ? *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(local_entity) + 12)
          : 0;
  static double last_diag = 0;
  if (now - last_diag > 5) {
    log("READINESS WH player=%p entity=%p id=0x%x", local_actor, local_entity, local_id);
    last_diag = now;
  }
  if (!local_entity || get_entity(entity_system, local_id) != local_entity)
    return;
  if (local_mode) {
    kcds::runtime::architect::player_body::frame(local_entity);
  }
  if (!world_ready) {
    bool world_loaded = false;
    finish_world_load(framework, services, world, now, last_diag, local_id, world_loaded);
    if (!world_loaded)
      return;
    // The KCD1 map has none of KCD2's spawn points: the player starts at Rataje.
    kcds::runtime::kcd1::stage_player(world.map, local_entity);
    // Nor a story to switch its layers: they start as a new KCD1 game has them.
    kcds::runtime::kcd1::start_layers(world.map);
    // Nor a record in the game's level table: the map screen shows KCD1's world map.
    kcds::runtime::kcd1::map_screen::show(world.map, services);
    // Nor global illumination the game can read: it builds it from the level's geometry.
    kcds::runtime::kcd1::interior_light::apply(world.map);
    if (local_mode) {
      kcds::runtime::architect::local_mode::mark_world_ready();
      if (kcds::runtime::studio::requested()) {
        kcds::runtime::studio::stage_player(local_entity);
      } else {
        // A custom map has no spawn point: the player goes to the middle of its ground.
        kcds::runtime::maps::custom::stage_player(kcds::runtime::architect::local_mode::world().map,
                                                  local_entity);
      }
    }
  }
  if (local_mode && kcds::runtime::studio::requested()) {
    kcds::runtime::studio::keep_light();
    kcds::runtime::studio::whip::tick(local_entity);
    kcds::runtime::studio::rope::tick(local_entity, [](const kcd2s::sdk::Value& message) {
      std::lock_guard<std::mutex> lock(network_mutex);
      return client->send_to_resource_runtime(message);
    });
    // A clip of the game's own on the character (studio/studio_game_clip.hpp).
    const auto to_resources = [](const kcd2s::sdk::Value& message) {
      std::lock_guard<std::mutex> lock(network_mutex);
      return client->send_to_resource_runtime(message);
    };
    kcds::runtime::studio::game_clip::tick(local_entity, to_resources);
    // Searches in the game's own objects and clips (studio/game_files.hpp).
    kcds::runtime::studio::game_files::tick(to_resources);
    // The hand that holds the prop closes round it.
    kcds::runtime::studio::hold::grip(
        local_entity, kcds::runtime::studio::rope::made() || kcds::runtime::studio::whip::made());
    kcds::runtime::studio::pose::tick(local_entity);
    kcds::runtime::studio::outfit::tick(local_actor, local_entity);
    kcds::runtime::studio::sky::tick();
    kcds::runtime::studio::skin::tick(local_entity);
    kcds::runtime::studio::mesh::tick(local_entity, [](const kcd2s::sdk::Value& message) {
      std::lock_guard<std::mutex> lock(network_mutex);
      return client->send_to_resource_runtime(message);
    });
    kcds::runtime::studio::skeleton::tick(local_entity, [](const kcd2s::sdk::Value& message) {
      std::lock_guard<std::mutex> lock(network_mutex);
      return client->send_to_resource_runtime(message);
    });
    kcds::runtime::studio::prop_gizmo::tick(local_entity, [](const kcd2s::sdk::Value& message) {
      std::lock_guard<std::mutex> lock(network_mutex);
      return client->send_to_resource_runtime(message);
    });
    kcds::runtime::studio::textures::tick([](const kcd2s::sdk::Value& message) {
      std::lock_guard<std::mutex> lock(network_mutex);
      return client->send_to_resource_runtime(message);
    });
    kcds::runtime::studio::saves::tick([](const kcd2s::sdk::Value& message) {
      std::lock_guard<std::mutex> lock(network_mutex);
      return client->send_to_resource_runtime(message);
    });
    kcds::runtime::studio::events::tick(local_entity);
    kcds::runtime::studio::floor::tick(to_resources);
    kcds::runtime::studio::grid::tick();
    // Local NPCs (LocalNpc, npcs/local_npcs.hpp) in the studio's world too: the frame returns
    // before their tick without a network player (someone to whip, a body in a lash's way).
    kcds::runtime::npcs::local::frame(actor_system, entity_system, nullptr, now);
  }
  report_gameplay_ready(world, now);
  reveal_server_map(services, world);
  local_engine_id.store(local_id);
  local_combat_actor.store(local_actor);
  local_network_id.store(own);
  bind_player_command_body(local_actor, world.revision, own);
  retain_local_input(local_actor);
  local_rider_seated.store(native_human_mounted && native_human_mounted(local_actor) != 0);
  local_rider_mounted.store(local_rider_seated.load());
  tick_knockout(local_actor);
  tick_movement_mode(local_actor);
  tick_progression_orders(local_actor);
  tick_progression(local_actor);
  tick_buffs(local_actor);
  tick_inventory(local_actor);
  tick_crafting(local_entity);
  tick_dice();
  tick_gathering();
  tick_cookpot_looks();
  tick_placed_objects();
  tick_lights();
  tick_carts();
  tick_previews(local_entity, now);
  // The client resources' sounds.
  kcds::runtime::audio::frame(entity_system, local_entity);
  // The client resources' Follow.
  kcds::runtime::follow::frame(entity_system, local_actor, local_entity, local_rider_seated.load());
  tick_engine_ids();
  tick_body_tags(local_actor);
  if (combat_available.load()) {
    tick_native_block_test(local_actor, now);
    tick_master_strike_test(local_actor);
    tick_keep_lock(local_actor);
  }
  tick_book_probe();
  if (combat_available.load()) {
    tick_native_attack_test(local_actor, now);
  }
  tick_scripted_input(local_actor, now);
  tick_trample_replay();
  LocalLookReading look;
  read_local_look(local_entity, look);
  // Free look: the head looks where the camera does, on every screen.
  float free_look_target[3];
  if (kcds::runtime::camera::free_look::target(free_look_target)) {
    look.look_target = {free_look_target[0], free_look_target[1], free_look_target[2]};
    look.look_enabled = true;
    look.look_valid = true;
  }
  // The values the frame's feature ticks share, all known from here on.
  const Frame frame{
      .now = now,
      .own = own,
      .entities = entities,
      .world = world,
      .actor_system = actor_system,
      .entity_system = entity_system,
      .game_interface = game_interface,
      .local_actor = local_actor,
      .local_entity = local_entity,
      .local_id = local_id,
      .local_look = look.local_look,
      .local_aim = look.local_aim,
      .look_target = look.look_target,
      .look_tuning = look.look_tuning,
      .look_enabled = look.look_enabled,
      .look_valid = look.look_valid,
  };
  note_crowd_trace_local(local_entity);
  local_plain_attack.store(native_action_type(*reinterpret_cast<void**>(
                               static_cast<std::uint8_t*>(local_actor) + 632)) == 3);
  if (auto skeleton = local_skeleton.load())
    capture_existing_animations(skeleton);
  if (own == core::kInvalidEntity) {
    // World Architect's local mode has no server avatar, so the local state tick (which applies
    // level edits) never runs: its own level edits (hidden trees, bushes, houses) are applied here
    // (Del and Clear nature left the level's objects standing).
    if (kcds::runtime::architect::local_mode::enabled()) {
      // Its door and gate preview (open / close them from Details) adds the gates' level edits.
      auto level_edits = kcds::runtime::architect::level_edits::take();
      kcds::runtime::architect::level_doors::frame(true, level_edits);
      // Its placed sounds (Effects > Sounds).
      kcds::runtime::architect::level_sounds::frame(true);
      // Bake navmesh (custom maps).
      kcds::runtime::architect::navmesh_bake::frame(true);
      // Its NPC preview: local NPCs placed and sent walking (the navmesh seen at work).
      kcds::runtime::npcs::local::frame(actor_system, entity_system,
                                        view_matrix_valid ? view_matrix.data() : nullptr, now);
      kcds::runtime::npcs::local::take_walks();
      kcds::runtime::level::frame(level_edits);
      // Nobody dies in World Architect, in the editor or playing as the player.
      void* soul =
          local_actor
              ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(local_actor) + kLocalActorSoul)
              : nullptr;
      kcds::runtime::knockout::keep_untouchable(soul);
    }
    return;
  }
  bool local_state_sent = false;
  phases.mark(metrics::Phase::Local);
  send_local_state(frame, local_state_sent);
  phases.mark(metrics::Phase::LocalState);
  if (!local_state_sent)
    return;
  begin_character_frame(frame);
  detach_retiring_riders(entities);
  retire_remote_players(frame);
  retire_native_npcs(entities, entity_system);
  retire_native_horses(entities, entity_system);
  phases.mark(metrics::Phase::Retirement);
  // With riding off no horse is made, so none is retired or ridden either.
  if (riding_available.load()) {
    tick_native_horses(entities, actor_system, entity_system, now);
  }
  phases.mark(metrics::Phase::Horses);
  tick_native_npcs(entities, actor_system, entity_system,
                   view_matrix_valid ? view_matrix.data() : nullptr, now);
  // Led horses' ropes, once both ends' bodies moved this frame (Horse.follow with rope).
  tick_lead_ropes(now);
  // The two NPC steps after the network NPCs are timed too (frames of 16 and
  // 19 s in the NPC phase).
  const auto bodies_started = std::chrono::steady_clock::now();
  tick_npc_bodies();
  const auto local_started = std::chrono::steady_clock::now();
  kcds::runtime::npcs::local::frame(actor_system, entity_system,
                                    view_matrix_valid ? view_matrix.data() : nullptr, now);
  const auto local_ended = std::chrono::steady_clock::now();
  const double bodies_ms =
      std::chrono::duration<double, std::milli>(local_started - bodies_started).count();
  const double local_ms =
      std::chrono::duration<double, std::milli>(local_ended - local_started).count();
  if (bodies_ms > 300.0 || local_ms > 300.0) {
    log("NPC slow frame npc bodies %.0f ms, level people %.0f ms", bodies_ms, local_ms);
  }
  // Bodies under a cutscene's clip kept where their players stand (pose_animation/clip_in_place).
  kcds::runtime::pose_animation::clip_in_place::tick(local_entity);
  phases.mark(metrics::Phase::Npcs);
  tick_ground_items(entities);
  tick_body_containers(frame);
  tick_shots(entity_system);
  tick_pickpocket_orders(entity_system);
  tick_pickpocket(entity_system, local_actor, local_entity);
  tick_siege(local_actor, local_entity);
  tick_body_carry(entity_system, local_actor, local_entity);
  tick_dropped_weapons(local_actor);
  tick_ladders(frame);
  tick_photo_mode_permission();
  tick_animation_orders(frame);
  tick_pillory(frame);
  // The cuffed pose moves only the upper body: the legs walk (animation/upper_body.hpp).
  if (kcds::runtime::input::cuffs::cuffed()) {
    kcds::runtime::animation::upper_body::keep_legs(
        local_entity, kcds::runtime::player_animation::kScriptClipLayer);
  }
  tick_doors(frame);
  tick_graves(frame);
  tick_smart_objects(frame);
  if (failed)
    return;
  bool replicas_finished = false;
  phases.mark(metrics::Phase::Features);
  tick_replicas(frame, replicas_finished);
  phases.mark(metrics::Phase::Players);
  if (!replicas_finished)
    return;
  // After the replicas: an attached body goes where its target is this frame.
  kcds::runtime::attach::frame(true);
  // Without combat's functions (a game patch) there is no combat between players to relay.
  if (combat_available.load()) {
    tick_combat_relay(frame);
  }
  tick_weapon_hits(frame);
  retain_local_input(local_actor);
  retain_local_view();
  engine_stage = "test native body snapshot";
  tick_native_body_snapshot(entity_system, local_actor, local_id, own, world.revision);
  phases.mark(metrics::Phase::Combat);
  // Send native animation inputs; transforms remain server-replicated corrections.
  // What runs from here on is the game's own frame, not this step: a crash report must not blame
  // it.
  engine_stage = "game frame";
}

}  // namespace kcds::game_client
