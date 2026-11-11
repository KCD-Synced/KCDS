// Winsock before Windows.h (see client_internal.hpp).
#include "client_internal.hpp"

#include "startup_targets.hpp"

#include <cstdlib>

#include "../combat/clinch_relay.hpp"
#include "../combat/defence.hpp"
#include "../combat/hit_feedback.hpp"
#include "../combat/melee_contacts.hpp"
#include "../combat/native_effects.hpp"
#include "../combat/sync_attacks.hpp"
#include "../replicas/remote_lifecycle.hpp"
#include "client_state.hpp"
#include "engine_loop.hpp"
#include "log.hpp"

namespace kcds::game_client {

DWORD check_startup_targets(const StartupTargets& targets) {
  // The targets under the names initialize_adapter() gave them.
  const bool menu_hooks_installed = targets.menu_hooks_installed;
  void* const clear_animation = targets.clear_animation;
  void* const stop_animation = targets.stop_animation;
  void* const start_animation = targets.start_animation;
  void* const motion_param = targets.motion_param;
  void* const movement_request = targets.movement_request;
  void* const ai_object_ctor = targets.ai_object_ctor;
  void* const ai_object_dtor = targets.ai_object_dtor;
  void* const set_combat_target = targets.set_combat_target;
  void* const combat_input = targets.combat_input;
  void* const combat_eligibility = targets.combat_eligibility;
  void* const produce_melee = targets.produce_melee;
  void* const player_hit_feedback = targets.player_hit_feedback;
  void* const hit_reaction = targets.hit_reaction;
  void* const choose_sync_attack = targets.choose_sync_attack;
  void* const start_clinch = targets.start_clinch;
  void* const make_attack = targets.make_attack;
  void* const make_sync_hit = targets.make_sync_hit;
  void* const queue_combat_query = targets.queue_combat_query;
  void* const perfect_block_window = targets.perfect_block_window;
  void* const submit_rpg_event = targets.submit_rpg_event;
  void* const destroy_rpg_event = targets.destroy_rpg_event;
  void* const melee_hit = targets.melee_hit;
  void* const combat_damage = targets.combat_damage;
  void* const set_native_characteristic = targets.set_native_characteristic;
  void* const player_ctor = targets.player_ctor;
  void* const modify_native_event = targets.modify_native_event;
  void* const queue_native_hit = targets.queue_native_hit;
  void* const create_collision_hit_event = targets.create_collision_hit_event;
  void* const interactive_state = targets.interactive_state;
  void* const move_to_start = targets.move_to_start;
  void* const move_to_end = targets.move_to_end;
  void* const enable_action_map = targets.enable_action_map;
  // Combat between players: a function of it missing (a game patch) turns combat off, and the
  // client starts without it.
  if (!queue_native_hit || !create_collision_hit_event || !modify_native_event ||
      !create_injured_params || !create_morale_params || !native_injured_params_type ||
      !native_morale_params_type || !native_backoff_attack_type || !create_native_cause ||
      !create_native_event || !create_health_effect || !create_stamina_effect ||
      !create_buff_effect || !create_unconscious_params || !produce_native_hit ||
      !complete_physical_attack_power || !set_native_contact_target || !native_health_effect_type ||
      !native_stamina_effect_type || !native_buff_effect_type || !native_unconscious_params_type ||
      !native_sync_attack_database || !choose_sync_attack || !construct_sync_attack ||
      !make_attack || !start_clinch || !native_clinch_ready || !native_clinch_kinds[0] ||
      !native_clinch_kinds[1] || !native_sync_hit_database || !make_sync_hit ||
      !queue_combat_query || !perfect_block_window || !submit_rpg_event || !destroy_rpg_event ||
      !set_combat_target || !produce_melee || !player_hit_feedback || !base_actor_hit_feedback ||
      !hit_reaction || !rpg_module_slot || !melee_hit || !combat_input || !combat_eligibility ||
      !combat_damage || !set_native_characteristic || std::getenv("KCDS_TEST_COMBAT_OFF")) {
    combat_available.store(false);
    log("COMBAT off: a combat native of this game build is missing; no combat between players");
  }
  if (!interactive_state || !move_to_start || !move_to_end || !enable_action_map) {
    log("Interactive state signatures missing; hooks not installed");
    return 1;
  }
  if (!ai_object_ctor || !ai_object_dtor || !cancel_transform || !transform_manager_slot ||
      !clear_animation || !stop_animation || !start_animation || !motion_param ||
      !movement_request || !actor_hit || !actor_deactivate || !menu_hooks_installed ||
      !actor_update || !actor_tick || !actor_reset || !player_ctor || !actor_init ||
      !actor_post_init || !actor_release || !actor_event || !actor_activate) {
    log("Remote lifecycle signature missing; no hooks installed");
    return 1;
  }
  if (!update_target || !framework_slot || !map_callback || !get_services || !params_init ||
      !string_assign || !console_slot) {
    log("Required engine signature missing; no hook installed");
    return 1;
  }
  return 0;
}

}  // namespace kcds::game_client
