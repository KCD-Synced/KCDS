// The game functions initialize_adapter() finds before MinHook is initialized and hooks after it
// Each resolve_* function of a feature fills its part, check_startup_targets()
// stops the adapter when a required one is missing, and each install_*_hooks function hooks its
// part, in the order initialize_adapter() always hooked them.
#pragma once

#include <windows.h>

namespace kcds::game_client {

struct StartupTargets {
  // Whether the main menu's hooks were installed (the menu is built soon after WHGame.dll loads).
  bool menu_hooks_installed = false;
  // Horses: the local player's mount and dismount (riding/).
  void* mount_target = nullptr;
  void* dismount_target = nullptr;
  // Animation: the skeleton's layers, the facial pak and the motion parameters (animation/).
  void* clear_animation = nullptr;
  void* stop_animation = nullptr;
  void* start_animation = nullptr;
  void* facial_pak_changed = nullptr;
  void* motion_param = nullptr;
  // The replicas' movement requests and AI objects (replicas/).
  void* movement_request = nullptr;
  void* ai_object_ctor = nullptr;
  void* ai_object_dtor = nullptr;
  // Combat: targets, input, hits, sync attacks, clinches, RPG events and the player's actor
  // (combat/).
  void* set_combat_target = nullptr;
  void* combat_input = nullptr;
  void* combat_eligibility = nullptr;
  void* produce_melee = nullptr;
  void* player_hit_feedback = nullptr;
  void* hit_reaction = nullptr;
  void* choose_sync_attack = nullptr;
  void* start_clinch = nullptr;
  void* make_attack = nullptr;
  void* make_sync_hit = nullptr;
  void* queue_combat_query = nullptr;
  void* perfect_block_window = nullptr;
  void* submit_rpg_event = nullptr;
  void* destroy_rpg_event = nullptr;
  void* melee_hit = nullptr;
  void* combat_damage = nullptr;
  void* set_native_characteristic = nullptr;
  void* player_ctor = nullptr;
  void* modify_native_event = nullptr;
  void* queue_native_hit = nullptr;
  void* create_collision_hit_event = nullptr;
  // The interactive state and the action maps of the local player (input/).
  void* interactive_state = nullptr;
  void* move_to_start = nullptr;
  void* move_to_end = nullptr;
  void* enable_action_map = nullptr;
  // Neutral combat: the combat entry, the attack lifecycle, the denials and the movement
  // restriction, sync attacks, the HUD's combat input and the map concept (combat/).
  void* combat_entry = nullptr;
  void* attack_start = nullptr;
  void* attack_exit = nullptr;
  void* master_riposte_start = nullptr;
  void* actor_has_channel = nullptr;
  void* concept_map_marker = nullptr;
  void* attack_space = nullptr;
  void* show_denial_attack = nullptr;
  void* denied_ui_emit = nullptr;
  void* restriction_setter = nullptr;
  void* sync_attack_update = nullptr;
  void* sync_attack_exit = nullptr;
  void* hud_combat_input = nullptr;
  void* denied_action_feedback = nullptr;
  // The map's clouds, codex toasts, quest markers and discovery HUD (world_map/).
  void* map_cloud_array = nullptr;
  void* codex_toast = nullptr;
  void* quest_markers = nullptr;
  void* quest_marker_visibility = nullptr;
  void* discovery_hud = nullptr;
  void* map_marker_visible = nullptr;
  // The local player's input actions, hooked for the chat (input/, chat/).
  void* player_input_action = nullptr;
};

// Called by initialize_adapter() once the targets and the engine natives are resolved. Returns
// 0, or 1 when a required one is missing and the adapter must not start (its exit code).
DWORD check_startup_targets(const StartupTargets& targets);

}  // namespace kcds::game_client
