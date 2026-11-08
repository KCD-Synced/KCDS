// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "world_reset.hpp"

#include "../animation/animation_capture.hpp"
#include "../animation/animation_props.hpp"
#include "../animation/no_gravity.hpp"
#include "../animation/player_animation.hpp"
#include "../animation/pointing.hpp"
#include "../appearance/components.hpp"
#include "../appearance/disguise.hpp"
#include "../appearance/grime.hpp"
#include "../appearance/outfit.hpp"
#include "../appearance/visibility.hpp"
#include "../branding/watermark.hpp"
#include "../camera/free_look.hpp"
#include "../camera/third_person.hpp"
#include "../carry/carry_items.hpp"
#include "../carts/cart_horses.hpp"
#include "../carts/placed_carts.hpp"
#include "../combat/attack_factory.hpp"
#include "../combat/clinch_relay.hpp"
#include "../combat/combat_identity.hpp"
#include "../combat/combat_input.hpp"
#include "../combat/defence.hpp"
#include "../combat/melee_contacts.hpp"
#include "../combat/player_damage.hpp"
#include "../combat/sync_attacks.hpp"
#include "../core/log.hpp"
#include "../crafting/alchemy.hpp"
#include "../crafting/grinding.hpp"
#include "../crafting/smithing.hpp"
#include "../dialogue/dialogue_list.hpp"
#include "../doors/doors.hpp"
#include "../graves/graves.hpp"
#include "../smart_objects/placed_smart_objects.hpp"
#include "../gates/gates.hpp"
#include "../gizmos/gizmos.hpp"
#include "../nameplates/nameplates.hpp"
#include "../ground/ground_items.hpp"
#include "../input/cuffs.hpp"
#include "../attach/player_attach.hpp"
#include "../input/frozen.hpp"
#include "../noclip/free_camera.hpp"
#include "../input/interaction_rules.hpp"
#include "../interactions/world_interactions.hpp"
#include "../knockout/dropped_weapons.hpp"
#include "../knockout/knockout.hpp"
#include "../level/level_edits.hpp"
#include "../look/native_look.hpp"
#include "../markers/blips.hpp"
#include "../npcs/local_npcs.hpp"
#include "../npcs/native_npcs.hpp"
#include "../pillory/pillory.hpp"
#include "../props/ghosts.hpp"
#include "../physical_props/player_props.hpp"
#include "../props/held_props.hpp"
#include "../props/placed_props.hpp"
#include "../prefabs/local_prefabs.hpp"
#include "../collision/local_collision.hpp"
#include "../architect/level_doors.hpp"
#include "../architect/level_sounds.hpp"
#include "../audio/resource_audio.hpp"
#include "../props/local_props.hpp"
#include "../architect/editor_overlay.hpp"
#include "../branding/editor_chrome.hpp"
#include "../terrain/terrain_edits.hpp"
#include "../terrain/terrain_ops.hpp"
#include "../vegetation/local_vegetation.hpp"
#include "../props/placer.hpp"
#include "../ranged/landings.hpp"
#include "../replicas/remote.hpp"
#include "../riding/lead_rope.hpp"
#include "../riding/native_horses.hpp"
#include "../seats/seat_collisions.hpp"
#include "../seats/seat_reservations.hpp"
#include "../siege/siege_engines.hpp"
#include "../siege/siege_ladders.hpp"
#include "../siege/stone_piles.hpp"
#include "../testing/combat_tests.hpp"
#include "../vendors/vendor_screen.hpp"
#include "../vfx/placed_effects.hpp"
#include "../lights/lights.hpp"
#include "../weapons/drawn_weapons.hpp"
#include "../replicas/replica_autonomy.hpp"
#include "../replicas/replica_fight.hpp"
#include "../replicas/replica_removal.hpp"
#include "../replicas/replica_spawn.hpp"
#include "../replicas/replica_state.hpp"
#include "../stats/player_command_binding.hpp"
#include "../pose_animation/clip_in_place.hpp"
#include "../world/trespass.hpp"
#include "../world/world_weather.hpp"

#include <mutex>

namespace kcds::game_client {

// Drops this world's replicas and per-world state: the server sent another world, or the player
// leaves the world back to the main menu.
void reset_world_replicas(void* entity_system) {
  // The client resources' blips stay, drawn again in the next world.
  kcds::runtime::markers::own_blips_dropped();
  // Retire observers before cleanup can reenter restriction hooks on a dying body.
  for (auto& [id, remote] : remotes) {
    remote.wake.clear();
    remote.wake_scope.reset();
    remote.wake_presentation_held = false;
  }
  reset_player_command_binding();
  discard_held_hits("world reset");
  discard_player_damage("world reset");
  kcds::runtime::player_animation::reset();
  kcds::runtime::animation::no_gravity::reset();
  kcds::runtime::animation::pointing::reset();
  kcds::runtime::camera::free_look::reset();
  local_skeleton.store(nullptr);
  owner_overlay_membership.reset();
  pending_combat_soul = nullptr;
  {
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    owned_human_overlay_skeletons.clear();
    human_overlay_owner_skeleton = nullptr;
    human_overlay_owner_scope.reset();
    local_human_overlay = {};
    human_overlay_fifo_stopped = false;
    pending_human_overlay.reset();
    unavailable_owner_overlay_clip = -1;
  }
  authoritative_replica_soul = nullptr;
  swing_hold = {};
  contacted_attack_generation.clear();
  local_attack_action = nullptr;
  local_attack_started = 0;
  remote_attack_source = 0;
  remote_attack_started = 0;
  replica_contacts.clear();
  native_attack_started.clear();
  pending_perfect_blocks.clear();
  multiplayer_world_ready.store(false);
  kcds::runtime::branding::watermark::set_in_world(false);
  kcds::runtime::props::reset();
  kcds::runtime::physical_props::players::reset();
  kcds::runtime::animation_props::reset();
  kcds::runtime::crafting::smithing::reset();
  kcds::runtime::crafting::alchemy::reset();
  kcds::runtime::crafting::grinding::reset();
  kcds::runtime::seats::reset();
  kcds::runtime::seats::collisions::reset();
  kcds::runtime::weapons::reset();
  kcds::runtime::knockout::dropped_weapons::reset();
  kcds::runtime::carry::reset();
  kcds::runtime::ground::reset_world();
  kcds::runtime::ranged::reset_landings();
  // First person and the mode allowed again.
  kcds::runtime::camera::third_person::reset();
  kcds::runtime::pose_animation::clip_in_place::reset();
  // The cuffs come off; the server cuffs the player again in the next world.
  kcds::runtime::input::cuffs::set_cuffed(false);
  // So does a freeze.
  kcds::runtime::input::frozen::set_frozen(false);
  kcds::runtime::attach::clear();
  // And NoClip is allowed until the next server forbids it.
  kcds::runtime::noclip::set_allowed(true);
  // None of the game's own interactions until the next server allows them.
  kcds::runtime::world::trespass::set_allowed(false);
  kcds::runtime::input::interaction_rules::set_allowed(0, console_slot ? *console_slot : nullptr,
                                                       false);
  // A server conversation ends with the world; the server knows.
  kcds::runtime::dialogue::reset();
  // So does a vendor's screen; the server knows.
  kcds::runtime::vendors::reset(false);
  // And the world interactions; the server gives them again in the next world.
  kcds::runtime::interactions::reset();
  // And the siege engines and their stones; the server spawns them again when the next world is
  // ready.
  kcds::runtime::siege::reset();
  // And the siege ladders and stone piles, spawned again in the next world too.
  kcds::runtime::siege::ladders::reset();
  kcds::runtime::siege::stones::reset();
  // The stay in a pillory is forgotten; the server orders it again in the next world.
  kcds::runtime::pillory::reset();
  // The doors reported are forgotten; the next world reports its own.
  kcds::runtime::doors::reset();
  kcds::runtime::graves::reset();
  kcds::runtime::smart_objects::reset();
  // The server's props and effects are gone with the level; it sends them again.
  kcds::runtime::props::placed::reset();
  kcds::runtime::prefabs::local::reset();
  kcds::runtime::collision::local::reset();
  kcds::runtime::props::local::reset();
  kcds::runtime::architect::level_doors::reset();
  kcds::runtime::architect::level_sounds::reset();
  // The resources' kept sounds (Audio.loopAt) end with the world.
  kcds::runtime::audio::stop_resource_loops();
  kcds::runtime::branding::editor_chrome::reset();
  kcds::runtime::architect::overlay::reset();
  kcds::runtime::terrain::reset();
  kcds::runtime::terrain::ops::reset();
  kcds::runtime::vegetation::reset();
  kcds::runtime::vfx::placed::reset();
  // And its lights; the server sends its own again, the scripts hear theirs went.
  kcds::runtime::lights::reset();
  // So are the player's previews, placement and gizmos; the server sends the first two again
  kcds::runtime::props::ghosts::reset();
  kcds::runtime::props::placer::reset();
  kcds::runtime::gizmos::reset();
  kcds::runtime::nameplates::reset();
  // The next world is a new weather system; the server sends its weather again.
  kcds::runtime::world::forget();
  kcds::runtime::carts::placed::reset();
  reset_cart_horses();
  // The level edits were this world's; the server sends the next world's when it is ready
  kcds::runtime::level::reset();
  kcds::runtime::gates::reset();
  clear_lead_ropes();
  clear_native_horses(entity_system);
  clear_native_npcs(entity_system);
  kcds::runtime::npcs::local::clear(entity_system);
  for (const auto& [id, remote] : remotes) {
    if (entity_system && get_entity(entity_system, remote.engine_id)) {
      // As a single replica's removal does: the game's own handler of a removed player would
      // unbind the local player from the game UI, and the level switch that follows (a trip to
      // another map) read the unbound player and crashed.
      removing_remote_replica = true;
      remove_replica_entity(entity_system, remote.engine_id, remote.actor, id);
      removing_remote_replica = false;
    }
    // These keep each replica's actor until they forget it, as a single replica's removal does:
    // after a leave and a rejoin the outfit's preset guard read a destroyed actor.
    kcds::runtime::appearance::outfit::forget(id);
    kcds::runtime::appearance::grime::forget(id);
    kcds::runtime::appearance::components::forget(id);
    kcds::runtime::appearance::visibility::forget(id);
    kcds::runtime::appearance::disguise::forget(id);
    kcds::runtime::knockout::forget_replica(id);
  }
  {
    std::lock_guard<std::mutex> lock(replica_autonomy_mutex);
    replica_autonomy_owners.clear();
  }
  {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    remote_combat_souls.clear();
    remote_combat_movements.clear();
    ignored_health_writes.clear();
    physical_contact_listeners.clear();
    pending_sync_hits.clear();
    remote_native_targets.clear();
    local_requested_target.store(0);
  }
  {
    std::lock_guard<std::mutex> lock(pending_melee_mutex);
    pending_melee.clear();
  }
  native_block_test_until = 0;
  native_attack_test_until = 0;
  native_attack_test_release = 0;
  native_attack_test_next = 0;
  local_combat_actor.store(nullptr);
  local_network_id.store(core::kInvalidEntity);
  native_block_held.store(false);
  native_prepare_held.store(false);
  remotes.clear();
  replica_timelines.clear();
  replica_shown.clear();
  replica_out_of_fight_since.clear();
  clinch_handshake.reset();
  local_view_wrapper = nullptr;
  local_view_body = nullptr;
  local_stealth_watcher = nullptr;
  local_stealth_player = nullptr;
  {
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    remote_inputs.clear();
    remote_looks.clear();
  }
}

}  // namespace kcds::game_client
