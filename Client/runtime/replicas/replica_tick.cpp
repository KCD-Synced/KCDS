// Winsock before Windows.h (see client_internal.hpp).
#include "../voice/voice_client.hpp"
#include "../core/client_internal.hpp"
#include "../npcs/npc_attack.hpp"

#include "replica_tick.hpp"
#include "character_retirement.hpp"
#include "distance_lod.hpp"
#include "horse_riders.hpp"
#include "replica_drift.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <optional>
#include <string_view>

#include "../animation/animation_capture.hpp"
#include "../animation/animation_props.hpp"
#include "../animation/upper_body.hpp"
#include "../animation/player_animation.hpp"
#include "../animation/pointing.hpp"
#include "../pose_animation/clip_in_place.hpp"
#include "../appearance/components.hpp"
#include "../appearance/disguise.hpp"
#include "../appearance/grime.hpp"
#include "../appearance/outfit.hpp"
#include "../appearance/visibility.hpp"
#include "../buffs/replica_buffs.hpp"
#include "../carts/placed_carts.hpp"
#include "../carry/body_carry.hpp"
#include "../carry/carry_items.hpp"
#include "../combat/clinch_relay.hpp"
#include "../combat/combat_identity.hpp"
#include "../combat/defence.hpp"
#include "../combat/melee_contacts.hpp"
#include "../combat/native_actions.hpp"
#include "../combat/player_damage.hpp"
#include "../combat/reaction_window.hpp"
#include "../combat/sync_attacks.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../crafting/alchemy.hpp"
#include "../crafting/grinding.hpp"
#include "../crafting/smithing.hpp"
#include "../knockout/dropped_weapons.hpp"
#include "../knockout/knockout.hpp"
#include "../knockout/replica_wake_tick.hpp"
#include "../look/look_pose_layer.hpp"
#include "../look/native_look.hpp"
#include "../movement/ladder_clips.hpp"
#include "../progression/progression_orders.hpp"
#include "../progression/replica_soul.hpp"
#include "../physical_props/player_props.hpp"
#include "../pose_animation/player_animations.hpp"
#include "../props/held_props.hpp"
#include "../ranged/aim.hpp"
#include "../ranged/shots.hpp"
#include "../riding/lead_rope.hpp"
#include "../riding/native_horses.hpp"
#include "../appearance/brand.hpp"
#include "../characters/rested_updates.hpp"
#include "../nameplates/nameplates.hpp"
#include "../npcs/native_npcs.hpp"
#include "../props/prop_entity.hpp"
#include "../riding/trample.hpp"
#include "../seats/seat_collisions.hpp"
#include "../seats/seat_reservations.hpp"
#include "../stats/player_stats.hpp"
#include "../weapons/drawn_weapons.hpp"
#include "../weapons/held_weapon.hpp"
#include "kcd2s/integration/animation_props.hpp"
#include "kcd2s/integration/horse.hpp"
#include "kcd2s/integration/npc.hpp"
#include "kcd2s/integration/player_stats.hpp"
#include "kcd2s/integration/posture.hpp"
#include "kcd2s/integration/replica_buffs.hpp"
#include "kcd2s/integration/replica_soul.hpp"
#include "motion_capture.hpp"
#include "../core/game_layouts.hpp"
#include "remote.hpp"
#include "replica_autonomy.hpp"
#include "replica_fight.hpp"
#include "replica_look.hpp"
#include "replica_removal.hpp"
#include "deferred_moves.hpp"
#include "replica_spawn.hpp"
#include "replica_state.hpp"

namespace kcds::game_client {

namespace integration = kcd2s::integration;

// Whether the owner's body parts are a woman's (local.hpp BodyParts, its last name the gender).
bool body_is_female(const RemoteState& state) {
  if (state.fields.size() <= local::BodyParts) {
    return false;
  }
  integration::BodyParts parts;
  const auto& field = std::get<std::string>(state.fields[local::BodyParts]);
  return integration::parse_body_parts_field(field, parts) && !parts.empty() &&
         parts.back() == "female";
}

// A body with no animation on its base layer is drawn in its default pose, arms out
// (not reproduced): logged once when it lasts a second, and when it ends, so a report holds it.
// ISkeletonAnim slot 15: how many animations a layer holds.
void watch_base_layer(core::EntityId id, Remote& remote) {
  constexpr std::size_t kLayerCountSlot = 15;
  constexpr std::uint64_t kLogAfterMs = 1000;
  if (!remote.skeleton) {
    return;
  }
  using LayerCount = unsigned(__fastcall*)(void*, std::uint32_t);
  const unsigned count = slot<LayerCount>(remote.skeleton, kLayerCountSlot)(remote.skeleton, 0);
  const std::uint64_t now = GetTickCount64();
  if (count != 0) {
    if (remote.no_base_clip_logged) {
      log("REMOTE base layer player=%u has an animation again after %llu ms", unsigned(id),
          static_cast<unsigned long long>(now - remote.no_base_clip_since));
    }
    remote.no_base_clip_since = 0;
    remote.no_base_clip_logged = false;
    return;
  }
  if (remote.no_base_clip_since == 0) {
    remote.no_base_clip_since = now;
    return;
  }
  if (!remote.no_base_clip_logged && now - remote.no_base_clip_since >= kLogAfterMs) {
    remote.no_base_clip_logged = true;
    log("REMOTE base layer player=%u has had no animation for %llu ms: drawn in its default pose",
        unsigned(id), static_cast<unsigned long long>(now - remote.no_base_clip_since));
  }
}

// Where a body's plate stands: its feet, and over the head wherever the pose puts it (crouching,
// sitting): the top of the head from the skeleton's HeadEnd joint, or from its Head joint.
void place_plate(void* body, kcds::runtime::nameplates::Seen& seen) {
  kcds::runtime::props::entity::world_position(body, seen.feet);
  float rotation[4];
  seen.has_head = kcds::runtime::props::entity::joint_pose(body, "HeadEnd", seen.head, rotation);
  if (seen.has_head) {
    seen.head[2] -= kcds::runtime::nameplates::kHeadEndAboveTop;
  } else if (kcds::runtime::props::entity::joint_pose(body, "Head", seen.head, rotation)) {
    seen.has_head = true;
    seen.head[2] += kcds::runtime::nameplates::kHeadJointToTop;
  }
}

// The nameplates of the NPCs a resource gave one (Npc.nameplate): drawn as a player's,
// from the NPC's name (or the tag's text) and the server's health.
void keep_npc_nameplates(const MirrorEntities& entities, void* entity_system) {
  for (const auto& [id, latest] : entities) {
    if (latest.type != core::type_id("NetworkNpc") ||
        latest.fields.size() <= integration::NpcNameplate) {
      continue;
    }
    const auto& field = std::get<std::string>(latest.fields[integration::NpcNameplate]);
    if (field.empty() || !std::get<bool>(latest.fields[integration::NpcAlive])) {
      continue;
    }
    const auto npc = native_npcs.find(id);
    void* body =
        npc == native_npcs.end() ? nullptr : get_entity(entity_system, npc->second.engine_id);
    if (!body) {
      continue;
    }
    kcds::runtime::nameplates::Seen seen;
    place_plate(body, seen);
    seen.nickname = std::get<std::string>(latest.fields[integration::NpcName]);
    seen.field = field;
    seen.health = std::clamp(std::get<float>(latest.fields[integration::NpcHealth]), 0.0f,
                             integration::kNpcMaxHealth) /
                  integration::kNpcMaxHealth * 100.0f;
    kcds::runtime::nameplates::keep(id, seen);
  }
}

// The players' nameplates this frame: where each body stands, its name, what the
// server said about its tag and its health.
// Player.brand on every body, the local player's too. A branded body is asked again
// now and then, as a change of clothes or hair builds its head anew.
void keep_brands(const MirrorEntities& entities, core::EntityId own, void* local_actor) {
  constexpr unsigned kLookEveryFrames = 15;
  constexpr double kAskAgainSeconds = 5.0;
  struct Shown {
    void* actor = nullptr;
    std::int64_t brand = 0;
    double at = 0.0;
  };
  static std::unordered_map<core::EntityId, Shown> shown;
  static unsigned frames = 0;
  if (++frames % kLookEveryFrames != 0) {
    return;
  }
  const double now = local::clock_seconds();
  const auto player_type = core::type_id("NetworkPlayer");
  for (const auto& [id, latest] : entities) {
    if (latest.type != player_type || latest.fields.size() <= local::Brand) {
      continue;
    }
    const auto remote = remotes.find(id);
    void* actor = id == own                 ? local_actor
                  : remote == remotes.end() ? nullptr
                                            : remote->second.actor;
    if (!actor) {
      continue;
    }
    const auto brand = std::get<std::int64_t>(latest.fields[local::Brand]);
    Shown& body = shown[id];
    const bool changed = body.actor != actor || body.brand != brand;
    if (!changed && (brand == 0 || now - body.at < kAskAgainSeconds)) {
      continue;
    }
    // A body that never had one has nothing to take off.
    if (brand == 0 && body.brand == 0) {
      body.actor = actor;
      continue;
    }
    if (kcds::runtime::appearance::brand::show(actor, std::uint8_t(brand))) {
      body = {actor, brand, now};
    }
  }
}

// The players' physical props (Player.setPhysicalProp): every frame, on the
// local body and on every replica (physical_props/player_props.hpp).
void keep_physical_props(const MirrorEntities& entities, core::EntityId own, void* local_actor) {
  const auto player_type = core::type_id("NetworkPlayer");
  std::vector<std::uint32_t> shown;
  for (const auto& [id, latest] : entities) {
    if (latest.type != player_type || latest.fields.size() <= local::PhysicalProp) {
      continue;
    }
    const auto remote = remotes.find(id);
    void* actor = id == own                 ? local_actor
                  : remote == remotes.end() ? nullptr
                                            : remote->second.actor;
    const auto& field = std::get<std::string>(latest.fields[local::PhysicalProp]);
    if (field.empty() && !actor) {
      continue;
    }
    kcds::runtime::physical_props::players::show(std::uint32_t(id), actor, field);
    shown.push_back(std::uint32_t(id));
  }
  kcds::runtime::physical_props::players::keep_only(shown);
}

// The players' lead ropes (Player.setLeadRope), drawn by riding/lead_rope.hpp.
void keep_lead_ropes(const MirrorEntities& entities) {
  const auto player_type = core::type_id("NetworkPlayer");
  std::vector<PlayerLeadRope> ropes;
  for (const auto& [id, latest] : entities) {
    if (latest.type != player_type || latest.fields.size() <= local::LeadRope) {
      continue;
    }
    const auto& field = std::get<std::string>(latest.fields[local::LeadRope]);
    if (const auto rope = field.empty() ? std::nullopt : kcd2s::integration::lead_rope_of(field)) {
      ropes.push_back({std::uint32_t(id), *rope});
    }
  }
  set_player_lead_ropes(std::move(ropes));
}

// The players' pose animations (Player.setPoseAnimation): every frame, on the local
// body and on every replica (pose_animation/player_animations.hpp).
void keep_pose_animations(const MirrorEntities& entities, core::EntityId own, void* local_actor) {
  const auto player_type = core::type_id("NetworkPlayer");
  std::vector<std::uint32_t> shown;
  for (const auto& [id, latest] : entities) {
    if (latest.type != player_type || latest.fields.size() <= local::PoseAnimation) {
      continue;
    }
    const auto remote = remotes.find(id);
    void* actor = id == own                 ? local_actor
                  : remote == remotes.end() ? nullptr
                                            : remote->second.actor;
    const auto& field = std::get<std::string>(latest.fields[local::PoseAnimation]);
    if (field.empty() && !actor) {
      continue;
    }
    kcds::runtime::pose_animation::players::show(std::uint32_t(id), actor, field);
    shown.push_back(std::uint32_t(id));
  }
  kcds::runtime::pose_animation::players::keep_only(shown);
}

void keep_nameplates(const MirrorEntities& entities, core::EntityId own, void* local_entity,
                     void* entity_system) {
  for (const auto& [id, latest] : entities) {
    if (latest.type != core::type_id("NetworkPlayer") || latest.fields.size() <= local::Nameplate) {
      continue;
    }
    kcds::runtime::nameplates::Seen seen;
    seen.own = id == own;
    void* body = local_entity;
    if (!seen.own) {
      const auto remote = remotes.find(id);
      body =
          remote == remotes.end() ? nullptr : get_entity(entity_system, remote->second.engine_id);
    }
    if (!body) {
      continue;
    }
    place_plate(body, seen);
    seen.nickname = std::get<std::string>(latest.fields[local::Nickname]);
    seen.field = std::get<std::string>(latest.fields[local::Nameplate]);
    integration::PlayerVitals vitals;
    integration::PlayerAbilities abilities;
    if (integration::parse_vitals_field(std::get<std::string>(latest.fields[local::Vitals]),
                                        vitals) &&
        integration::parse_abilities_field(std::get<std::string>(latest.fields[local::Abilities]),
                                           abilities) &&
        abilities.maxima[0] > 0.0f) {
      seen.health = std::clamp(vitals.values[0] / abilities.maxima[0] * 100.0f, 0.0f, 100.0f);
    }
    // The owner's talking style, after the speed and upper-body marks of its
    // animation field: the plate's speaking icon.
    if (latest.fields.size() > local::AnimationProps) {
      std::string mark;
      std::string field;
      integration::split_speed_field(std::get<std::string>(latest.fields[local::AnimationProps]),
                                     mark, field);
      std::string after_pointing;
      integration::split_pointing_field(field, mark, after_pointing);
      field = after_pointing;
      bool upper_body = false;
      std::string talking;
      integration::split_upper_body_field(field, upper_body, talking);
      std::string style;
      std::string rest;
      seen.speaking = integration::split_talking_field(talking, style, rest) && !style.empty();
    }
    kcds::runtime::nameplates::keep(id, seen);
  }
}

// Retire every departed player before any character type enters its spawn phase.
void retire_remote_players(const Frame& frame) {
  const auto& entities = frame.entities;
  void* const entity_system = frame.entity_system;
  void* const local_actor = frame.local_actor;
  // CEntitySystem::RemoveEntity slot 19 confirmed, including its diagnostic string.
  for (auto it = remotes.begin(); it != remotes.end();) {
    // A body of the other gender's skeleton goes, and the next frame makes it anew.
    const auto found = entities.find(it->first);
    if (found != entities.end() && body_is_female(found->second) == it->second.female) {
      ++it;
      continue;
    }
    discard_held_hits("opponent removed", it->first);
    it->second.wake.clear();
    it->second.wake_scope.reset();
    it->second.wake_presentation_held = false;
    retain_local_view();
    diagnose_view("before remote remove");
    log_local_feedback("before remote remove", it->second.actor);
    // These cleanup paths can call the engine and reenter native health handling.
    kcds::runtime::props::forget(it->first);
    kcds::runtime::animation_props::forget(it->first);
    kcds::runtime::crafting::smithing::forget(it->first);
    kcds::runtime::crafting::alchemy::forget(it->first);
    kcds::runtime::crafting::grinding::forget(it->first);
    kcds::runtime::player_animation::forget_replica_clip(it->first);
    kcds::runtime::player_animation::forget_replica_talking(it->first);
    kcds::runtime::player_animation::forget_replica_speed(it->first);
    kcds::runtime::appearance::outfit::forget(it->first);
    kcds::runtime::appearance::grime::forget(it->first);
    kcds::runtime::knockout::forget_replica(it->first);
    kcds::runtime::appearance::components::forget(it->first);
    kcds::runtime::appearance::visibility::forget(it->first);
    kcds::runtime::appearance::disguise::forget(it->first);
    kcds::runtime::seats::forget(it->first);
    kcds::runtime::seats::collisions::forget(it->first);
    kcds::runtime::weapons::forget(it->first);
    kcds::runtime::knockout::dropped_weapons::forget(it->first);
    ladder_climbers.erase(it->first);
    ladder_clip_times.erase(it->first);
    ladder_held_clips.erase(it->first);
    ladder_hold_rules.erase(it->first);
    kcds::runtime::carry::forget(it->first);
    {
      std::lock_guard<std::mutex> lock(combat_identity_mutex);
      auto soul = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(it->second.actor) + 1640);
      pending_sync_hits.erase(it->first);
      remote_ui_blocking.erase(it->first);
      remote_combat_souls.erase(soul);
      remote_native_targets.erase(soul);
      remote_combat_movements.erase(soul);
      ignored_health_writes.erase(it->first);
      physical_contact_listeners.clear();
      if (local_requested_target.load() == it->first)
        local_requested_target.store(0);
    }
    {
      std::lock_guard<std::shared_mutex> lock(motion_mutex);
      remote_inputs.erase(it->second.skeleton);
      animation_stop_fences.erase(it->second.skeleton);
      animation_stop_fences.erase(it->second.weapon_skeleton);
      remote_looks.erase(it->second.look);
      owned_human_overlay_skeletons.erase(it->second.skeleton);
    }
    {
      std::lock_guard<std::mutex> lock(reaction_clips_mutex);
      reaction_clips.erase(it->second.skeleton);
    }
    kcds::runtime::ranged::forget_replica_aim(it->second.aim);
    const auto local_ui = capture_local_ui(local_actor);
    // The replica's own action in its fight (28: none) as it goes,'s evidence.
    const unsigned fight_action = native_action_type(
        *reinterpret_cast<void**>(static_cast<std::uint8_t*>(it->second.actor) + 632));
    unsigned animations_left_out = 0;
    if (get_entity(entity_system, it->second.engine_id)) {
      silence_remote_ui_state_listener(it->second.actor, local_actor);
      removing_remote_replica = true;
      animations_left_out =
          remove_replica_entity(entity_system, it->second.engine_id, it->second.actor, it->first);
      removing_remote_replica = false;
    }
    restore_local_ui(local_ui, local_actor);
    retain_local_view();
    diagnose_view("after remote remove");
    log_local_feedback("after remote remove", nullptr);
    log("REMOTE removed player=%u fight_action=%u combat_animations_left_out=%u", it->first,
        fight_action, animations_left_out);
    replica_timelines.erase(it->first);
    replica_shown.erase(it->first);
    forget_decoded_animation(it->first);
    replica_out_of_fight_since.erase(it->first);
    clinch_handshake.forget(it->first);
    replica_contacts.erase(it->first);
    for (auto hit = replica_hits.begin(); hit != replica_hits.end();) {
      if (hit->first == it->first || hit->second.attacker == it->first) {
        hit = replica_hits.erase(hit);
      } else {
        ++hit;
      }
    }
    {
      std::lock_guard<std::mutex> lock(replica_autonomy_mutex);
      replica_autonomy_owners.erase(it->second.autonomy_context);
    }
    it = remotes.erase(it);
  }
}

// A C_Actor's entity, and how often a calm body takes its combat update.
constexpr std::size_t kActorEntity = kcds::layouts::Actor::kEntity;
constexpr unsigned kCombatUpdateEvery = 8;

void tick_replicas(const Frame& frame, bool& finished) {
  tick_remote_distance_lod(frame);
  // The frame's values under the names frame() gave them.
  const auto now = frame.now;
  const auto own = frame.own;
  const auto& entities = frame.entities;
  const auto horse_riders = kcds::runtime::replicas::snapshot_horse_riders(entities);
  const auto actor_system = frame.actor_system;
  const auto entity_system = frame.entity_system;
  const auto game_interface = frame.game_interface;
  const auto local_actor = frame.local_actor;
  const auto local_entity = frame.local_entity;
  const auto local_look = frame.local_look;
  replica_rest_new_frame();
  begin_rested_frame();
  const auto& look_target = frame.look_target;
  const auto look_enabled = frame.look_enabled;
  const auto look_valid = frame.look_valid;
  // Spawn through the engine's actor factory with channel 0; CreateActor explicitly
  // supports that branch. Do not establish local camera/action-map/authority CCETs.
  // Class name comes from the real local entity class, never from a guessed NPC name.
  engine_stage = "local entity class";
  auto klass = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(local_entity) + 32);
  const char* class_name = slot<const char*(__fastcall*)(void*)>(klass, 2)(klass);
  for (const auto& [id, latest] : entities) {
    if (id == own)
      continue;
    if (latest.type != core::type_id("NetworkPlayer"))
      continue;
    if (!std::get<bool>(latest.fields[local::PoseValid]))
      continue;
    const RemoteState& e = shown_remote_state(id, latest, now);
    auto rp = std::get<core::Vec3>(e.fields[local::Position]);
    auto rq = std::get<core::Quat>(e.fields[local::Rotation]);
    auto it = remotes.find(id);
    if (it == remotes.end()) {
      if (!character_spawns_ready()) {
        continue;
      }
      // A woman's body is built on the female skeleton (replica_spawn.cpp): the female body parts
      // on the male skeleton stretched her fingers.
      const bool female = body_is_female(latest);
      const char* spawn_class = class_name;
      char name[64];
      sprintf_s(name, "kcd2s_remote_%u", id);
      core::Vec3 scale{1, 1, 1};
      using CreateActor =
          void*(__fastcall*)(void*, unsigned short, const char*, const char*, const core::Vec3*,
                             const core::Quat*, const core::Vec3*, std::uint32_t);
      engine_stage = "remote CreateActor";
      log("REMOTE create attempt player=%u class=%s pos=(%.2f %.2f %.2f)", id, spawn_class, rp.x,
          rp.y, rp.z);
      diagnose_view("before remote spawn");
      log_local_feedback("before remote spawn", nullptr);
      spawning_remote = true;
      spawning_female_remote = female;
      void* actor = slot<CreateActor>(actor_system, 5)(actor_system, 0, name, spawn_class, &rp, &rq,
                                                       &scale, 0);
      spawning_remote = false;
      spawning_female_remote = false;
      diagnose_view("after remote spawn");
      retain_local_view();
      diagnose_view("after local context restore");
      log_local_feedback("after remote spawn", actor);
      retain_local_input(local_actor);
      log("REMOTE retained local combat/input movement selectors");
      auto retained_local = slot<void*(__fastcall*)(void*)>(game_interface, 64)(game_interface);
      if (retained_local != local_actor) {
        log("Remote startup replaced local player; adapter stopped");
        failed = true;
        return;
      }
      if (!actor) {
        log("Remote C_Player factory refused id=%u class=%s", id, spawn_class);
        continue;
      }
      // CActorSystem::CreateActor script return path proves actor->entity at +56.
      engine_stage = "remote entity";
      void* entity = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 56);
      if (!entity) {
        log("Remote actor has no entity");
        failed = true;
        return;
      }
      auto engine_id = *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(entity) + 12);
      record_character_created(engine_id, entity);
      it = remotes.emplace(id, Remote{engine_id, actor}).first;
      it->second.female = female;
      if (!bind_replica_autonomy(id, it->second)) {
        log("Remote autonomy identity unavailable player=%u; adapter stopped", id);
        failed = true;
        return;
      }
      if (auto soul = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 1640)) {
        std::lock_guard<std::mutex> lock(combat_identity_mutex);
        remote_combat_souls[soul] = id;
        remote_native_targets[soul] = 0;
        remote_combat_movements[soul] =
            *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 632);
        log("COMBAT remote C_Player native Soul=%p network=%u", soul, id);
      }
      auto character = slot<void*(__fastcall*)(void*, unsigned)>(entity, 101)(entity, 0);
      it->second.skeleton =
          character ? slot<void*(__fastcall*)(void*)>(character, 5)(character) : nullptr;
      auto movement = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 632);
      auto movement_core =
          movement ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(movement) + 528)
                   : nullptr;
      log("REMOTE player=%u engine=0x%x class=%s channel=0 character=%p movement=%p core=%p", id,
          engine_id, class_name, character, movement, movement_core);
    }
    const float body_at[3] = {rp.x, rp.y, rp.z};
    const bool rests = replica_can_rest(id, it->second, latest, &e == &latest, body_at);
    if (it->second.actor) {
      void* body_entity =
          *reinterpret_cast<void**>(static_cast<std::uint8_t*>(it->second.actor) + kActorEntity);
      if (it->second.rest_calm && !replica_turn(id, kCombatUpdateEvery)) {
        mark_calm(body_entity);
      }
      if (rests && replica_off_screen(body_at)) {
        mark_rested(body_entity);
      }
    }
    if (rests) {
      continue;
    }
    if (it->second.actor && replica_light_frame(id, it->second, latest, body_at)) {
      if (void* light_entity = get_entity(entity_system, it->second.engine_id)) {
        set_transform(light_entity, rp, replica_heading(id, rq), 0);
      }
      continue;
    }
    if (!get_entity(entity_system, it->second.engine_id)) {
      {
        std::lock_guard<std::mutex> lock(replica_autonomy_mutex);
        replica_autonomy_owners.erase(it->second.autonomy_context);
      }
      it->second.autonomy_context = nullptr;
      release_remote_human_overlay(it->second);
      continue;
    }
    if (latest.fields.size() > local::Vitals && it->second.actor) {
      // Vitals belong to the owner in every locomotion state, including a seated rider.
      constexpr std::size_t kHealth = 0;
      constexpr std::size_t kConsciousness = 6;
      static_assert(std::string_view(integration::kVitalNames[kConsciousness]) == "consciousness");
      integration::PlayerVitals vitals;
      if (integration::parse_vitals_field(std::get<std::string>(latest.fields[local::Vitals]),
                                          vitals)) {
        const bool down = vitals.values[kHealth] > 0.0f && vitals.values[kConsciousness] <= 0.0f;
        void* soul = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(it->second.actor) + 1640);
        // Whether that is its owner's exhaustion faint, from the same report as the
        // consciousness: the owner publishes both in the frame it faints.
        kcd2s::integration::PlayerPosture owner_posture;
        const bool fainted =
            down && latest.fields.size() > local::Posture &&
            kcd2s::integration::parse_posture_field(
                std::get<std::int64_t>(latest.fields[local::Posture]), owner_posture) &&
            owner_posture.fainted;
        kcds::runtime::knockout::keep_replica_down(id, soul, down, fainted);
        // The NPCs this client simulates leave a knocked-out player be (npc_attack.hpp forgive).
        if (down && soul) {
          kcds::runtime::npcs::attack::forgive(
              *reinterpret_cast<const std::uint64_t*>(static_cast<std::uint8_t*>(soul) + 48));
        }
        {
          ReplicaVitalsWrite owner_write(soul);
          kcds::runtime::stats::keep_replica_vitals(soul, vitals);
        }
        observe_replica_wake_owner(id, it->second, down);
      }
    }
    if (latest.fields.size() > local::Hands && it->second.actor) {
      kcds::runtime::weapons::observe_replica_items(
          id, it->second.actor, std::get<std::string>(latest.fields[local::Hands]));
    }
    engine_stage = "remote skeleton identity";
    if (auto native_entity = get_entity(entity_system, it->second.engine_id)) {
      auto character =
          slot<void*(__fastcall*)(void*, unsigned)>(native_entity, 101)(native_entity, 0);
      auto skeleton =
          character ? slot<void*(__fastcall*)(void*)>(character, 5)(character) : nullptr;
      if (skeleton != it->second.skeleton) {
        std::lock_guard<std::shared_mutex> lock(motion_mutex);
        owned_human_overlay_skeletons.erase(it->second.skeleton);
        remote_inputs.erase(it->second.skeleton);
        animation_stop_fences.erase(it->second.skeleton);
        it->second.skeleton = skeleton;
        it->second.human_overlay_sequence = 0;
        it->second.attempted_overlay_sequence = 0;
        it->second.observed_overlay_sequence = 0;
        it->second.human_overlay_scope.reset();
        it->second.human_overlay_membership.reset();
        it->second.human_overlay_clip = -1;
        it->second.unavailable_overlay_alias = 0;
        std::fill(std::begin(it->second.animation_sequences),
                  std::end(it->second.animation_sequences), 0);
      }
    }
    engine_stage = "remote native wake lifecycle";
    auto& wake_remote = it->second;
    const bool wake_active =
        tick_replica_wake(id, wake_remote, get_entity(entity_system, wake_remote.engine_id));
    const bool wake_completed =
        wake_remote.wake_scope && wake_remote.wake.take_completion(*wake_remote.wake_scope);
    if (wake_completed) {
      log("KNOCKOUT replica native get-up completed player=%u epoch=%u", id,
          unsigned(wake_remote.wake_scope->epoch));
    }
    const bool consume_wake_clips =
        wake_active || wake_completed || wake_remote.wake_presentation_held;
    wake_remote.wake_presentation_held = wake_active;
    if (consume_wake_clips) {
      // Identity refresh precedes this fence. Also consume the completion edge if native
      // callbacks finished GetUp before this frame, so its old owner clip cannot replay.
      const auto clips =
          local::decode_animation(std::get<std::string>(latest.fields[local::Animations]));
      if (clips) {
        for (unsigned layer = 0; layer < 16; ++layer) {
          wake_remote.animation_sequences[layer] =
              std::max(wake_remote.animation_sequences[layer], (*clips)[layer].sequence);
        }
      }
      continue;
    }
    engine_stage = "remote human overlay";
    // Native RiderSync owns the paired roots and animation queues while mounting
    // or dismounting. Replaying owner locomotion here fights that native action.
    if (e.fields.size() > local::NativeRiderState)
      synchronize_native_rider_completion(id, it->second.actor,
                                          std::get<std::string>(e.fields[local::NativeRiderState]));
    if (failed)
      return;
    // Native RiderSync supplies the attachment, and during mounting or dismounting the rider's
    // animation too: owner on-foot requests/clips must not replace that native pose. Once seated,
    // the replica's horse plays its owner's clips rather than the riding fragments, so the
    // rider's own riding clips are replayed as well (without them the seated replica stood
    // on the horse in its idle pose).
    const bool transition = native_rider_transition(it->second.actor);
    auto overlay_movement =
        *reinterpret_cast<void**>(static_cast<std::uint8_t*>(it->second.actor) + 632);
    kcds::runtime::carry::Guid overlay_carried_item{};
    const bool native_overlay_body =
        transition || native_human_mounted(it->second.actor) ||
        kcds::runtime::carry::carried(it->second.actor, overlay_carried_item) ||
        kcds::runtime::carry::carried_entity(it->second.actor) != 0 ||
        kcds::runtime::carry::body_held(id, it->second.engine_id, local_actor) ||
        in_reaction_window(it->second.skeleton) || kcds::runtime::knockout::replica_down(id) ||
        trample_falling(id) || trample_ragdoll_physics(id) ||
        (latest.fields.size() > local::RagdollPose &&
         valid_ragdoll_pose(std::get<std::string>(latest.fields[local::RagdollPose]))) ||
        (overlay_movement && paired_action(native_action_type(overlay_movement)));
    bool overlay_owned = false;
    if (native_overlay_body) {
      release_remote_human_overlay(it->second);
    } else {
      overlay_owned = keep_remote_human_overlay(id, it->second, latest);
    }
    if (transition || native_human_mounted(it->second.actor)) {
      const auto& clips = decoded_animation(id, std::get<std::string>(e.fields[local::Animations]));
      auto skeleton = it->second.skeleton;
      if (clips)
        for (unsigned layer = 0; layer < 16; ++layer) {
          if (overlay_owned && layer == local::kHumanOverlayLayer) {
            continue;
          }
          const auto& clip = (*clips)[layer];
          auto& applied = it->second.animation_sequences[layer];
          // The owner's look poses for the riding (or, dismounting, the standing) posture start
          // while RiderSync mounts or dismounts the replica; they play once it is done.
          const auto step =
              kcds::runtime::look::owner_clip_step(layer, clip.sequence, applied, transition);
          if (step == kcds::runtime::look::OwnerClipStep::Wait) {
            note_waiting_look_pose(id, it->second, clip);
            continue;
          }
          if (step == kcds::runtime::look::OwnerClipStep::Play && skeleton) {
            engine_stage = "remote mounted animation";
            log("REMOTE MOUNTED ANIMATION player=%u clip=%d layer=%u sequence=%llu", id, clip.clip,
                layer, clip.sequence);
            replaying_animation = true;
            if (clip.clip < 0) {
              original_clear_animation(skeleton, layer);
            } else {
              original_start_animation(skeleton, clip.clip, &clip.params);
            }
            replaying_animation = false;
          }
          applied = std::max(applied, clip.sequence);
        }
      {
        std::lock_guard<std::shared_mutex> lock(motion_mutex);
        remote_inputs.erase(it->second.skeleton);
      }
      // A seated rider still turns its head where its owner looks (the owner saw its own
      // head turn, the other player did not). While it mounts or dismounts RiderSync owns it.
      auto rider_entity = get_entity(entity_system, it->second.engine_id);
      if (!transition && rider_entity && e.fields.size() > local::LookTuning) {
        engine_stage = "remote mounted look IK";
        auto movement =
            *reinterpret_cast<void**>(static_cast<std::uint8_t*>(it->second.actor) + 632);
        const AppliedLook applied = apply_remote_look(it->second, e, rider_entity, movement, false);
        log_replica_look(id, "riding", applied, skeleton,
                         std::get<std::string>(e.fields[local::Animations]));
      } else {
        std::lock_guard<std::shared_mutex> lock(motion_mutex);
        remote_looks.erase(it->second.look);
      }
      // A rider draws and holsters on the horse too: the replica's hands follow its
      // owner's while it rides, as below for a body on foot (this branch skips the rest).
      if (rider_entity && latest.fields.size() > local::Hands) {
        engine_stage = "remote mounted hands";
        kcds::runtime::weapons::keep_replica(id, it->second.actor, rider_entity,
                                             std::get<std::string>(latest.fields[local::Hands]));
      }
      // So does what it wears (a rider who changed weapons or clothes on the horse kept its
      // old sheathed sword and belt on the other screens until it got off).
      if (rider_entity && latest.fields.size() > local::Outfit) {
        engine_stage = "remote mounted outfit";
        kcds::runtime::appearance::outfit::apply(
            id, it->second.actor, rider_entity,
            std::get<std::string>(latest.fields[local::Outfit]));
      }
      if (e.fields.size() > local::Grime) {
        engine_stage = "remote mounted grime";
        kcds::runtime::appearance::grime::apply(id, it->second.actor,
                                                std::get<std::string>(e.fields[local::Grime]));
      }
      if (e.fields.size() > local::BodyParts) {
        engine_stage = "remote mounted body parts";
        kcds::runtime::appearance::components::show(
            id, it->second.actor, std::get<std::string>(e.fields[local::BodyParts]));
      }
      if (rider_entity && e.fields.size() > local::Disguise) {
        engine_stage = "remote mounted disguise";
        kcds::runtime::appearance::disguise::keep_replica(
            id, rider_entity, std::get<std::string>(e.fields[local::Disguise]));
      }
      if (rider_entity && e.fields.size() > local::BodyVisible) {
        engine_stage = "remote mounted body visibility";
        kcds::runtime::appearance::visibility::keep_replica(
            id, rider_entity, std::get<bool>(e.fields[local::BodyVisible]));
      }
      continue;
    }
    // A knocked-out player's replica held by a carry in this game is the game's body to
    // move: none of its owner's position, movement or animation is put on it while it is picked
    // up and carried (a pick-up of a replica still driven crashed in the movement request, live).
    // Down but not carried, it lies with its owner's animation.
    if (kcds::runtime::knockout::replica_down(id) &&
        kcds::runtime::carry::body_held(id, it->second.engine_id, local_actor)) {
      {
        // Its layers go back to the game, so the carry's own pick-up and carried clips play on
        // the body (the replication kept refusing them: the body lay through the pick-up and then
        // jumped onto the carrier's shoulder, live).
        std::lock_guard<std::shared_mutex> lock(motion_mutex);
        remote_inputs.erase(it->second.skeleton);
      }
      // The carry moves the body; its owner's pose is laid out on it again once it is down.
      applied_ragdoll_poses.erase(id);
      const std::uint32_t carried = kcds::runtime::carry::carried_entity(local_actor);
      static std::uint32_t logged_carried = 0;
      if (carried == it->second.engine_id && logged_carried != id) {
        logged_carried = id;
        log("CARRY local player carries the body of player=%u", id);
      }
      continue;
    }
    engine_stage = "remote trample ragdoll";
    const bool trample_ragdolled_now = tick_trample_ragdoll(
        id, decoded_animation(id, std::get<std::string>(e.fields[local::Animations])),
        it->second.animation_sequences[0], it->second.actor, it->second.skeleton,
        get_entity(entity_system, it->second.engine_id), rp);
    // Held down for its owner's faint, the replica is its own game's to lay down and hold (issue):
    // none of its owner's pose, position, movement or clips is put on it meanwhile.
    const bool laid_by_game = replica_laid_by_game(id);
    const bool owner_ragdoll =
        !laid_by_game && e.fields.size() > local::RagdollPose &&
        valid_ragdoll_pose(std::get<std::string>(e.fields[local::RagdollPose]));
    const bool lying =
        !laid_by_game &&
        tick_owner_ragdoll(id, it->second.actor, owner_ragdoll, it->second.animation_sequences[0],
                           get_entity(entity_system, it->second.engine_id));
    const bool ragdolled = trample_ragdolled_now || lying || laid_by_game;
    const bool ragdoll_physics = trample_ragdoll_physics(id) || lying || laid_by_game;
    {
      // A remote player on a ladder holds it in this game too (movement/shared_ladders.hpp).
      kcd2s::integration::PlayerPosture posture;
      if (e.fields.size() > local::Posture &&
          kcd2s::integration::parse_posture_field(std::get<std::int64_t>(e.fields[local::Posture]),
                                                  posture) &&
          posture.on_ladder) {
        ladder_climbers[id] = {rp.x, rp.y, rp.z};
        if (kcd2s::integration::ladder_clip_held(posture.ladder_progress)) {
          ladder_clip_times[id] = kcd2s::integration::ladder_time_of(posture.ladder_progress);
        } else {
          ladder_clip_times.erase(id);
        }
      } else {
        ladder_climbers.erase(id);
        ladder_clip_times.erase(id);
      }
    }
    {
      // A rider sits where this game shows their seat, whatever their own game's cart reports.
      float seat_position[3] = {rp.x, rp.y, rp.z};
      float seat_rotation[4] = {rq.x, rq.y, rq.z, rq.w};
      if (kcds::runtime::carts::placed::rider_pose(id, seat_position, seat_rotation)) {
        rp = {seat_position[0], seat_position[1], seat_position[2]};
        rq = {seat_rotation[0], seat_rotation[1], seat_rotation[2], seat_rotation[3]};
      }
    }
    engine_stage = "remote SetWorldTM";
    if (auto entity = get_entity(entity_system, it->second.engine_id)) {
      trace_lying_body("replica", id, entity);
      core::Quat replica_turn{};
      auto replica_movement =
          *reinterpret_cast<void**>(static_cast<std::uint8_t*>(it->second.actor) + 632);
      const bool guard_alignment = native_pair_alignment(replica_movement);
      if (!guard_alignment && !ragdoll_physics && !native_human_mounted(it->second.actor)) {
        // A jump far beyond one frame of movement is a teleport (script position, /tp). A
        // standing replica does not follow a plain SetWorldTM until it moves again (live 0.1.1
        // regression), so the jump uses the script SetWorldPos reason like the owner's teleport.
        const float* current = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
        unsigned reason = 0;
        if (current) {
          const float dx = rp.x - current[3];
          const float dy = rp.y - current[7];
          const float dz = rp.z - current[11];
          // A body that drifted from it (stuck indoors on its own movement, a floor apart) is
          // put there the same way (replica_drift.hpp). A ladder climber keeps its ladder clip.
          static std::unordered_map<core::EntityId, kcds::runtime::replicas::ReplicaDrift> drifts;
          const double seconds =
              std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
                  .count();
          if (dx * dx + dy * dy + dz * dz > kReplicaTeleportMeters * kReplicaTeleportMeters) {
            reason = kScriptSetWorldPosReason;
            log("REMOTE teleport player=%u pos=(%.2f %.2f %.2f)", id, rp.x, rp.y, rp.z);
          } else if (!ladder_climbers.count(id) && drifts[id].should_place(dx, dy, dz, seconds)) {
            reason = kScriptSetWorldPosReason;
            static LogBudget budget;
            unsigned left_out = 0;
            if (log_budget(budget, 10, left_out)) {
              log("REMOTE drift placed player=%u off=(%.2f %.2f %.2f) pos=(%.2f %.2f %.2f) (%u "
                  "like "
                  "it left out)",
                  id, dx, dy, dz, rp.x, rp.y, rp.z, left_out);
            }
          }
        }
        // The turn is the animated character's own (its movement request, as for any NPC): it
        // turns the body and places what the replica holds and carries with it. A rotation set on
        // the entity from outside turned the body only, and the bow, the sword in hand and the
        // holsters kept the old heading until the next base clip (live probe).
        core::Quat current_heading;
        const core::Quat& heading = replica_heading(id, rq);
        if (!reason && current && heading_of(current, current_heading)) {
          // While the body gets up from lying the animated character does apply the request's
          // turn, on top of the heading set here: the replica stood at twice the turn one frame
          // and at its old heading the next, so its getting up started from a body turned the
          // wrong way and swung its legs into the air (live trace). The entity's heading
          // alone turns it then.
          if (!replica_getting_up(id)) {
            replica_turn = turn_between(current_heading, heading);
          }
          // The animated character never applied that turn to a replica (live probe: the request
          // held the right turn every frame, the body kept its heading), so the entity takes its
          // owner's heading as well, before the next update places what it carries.
          defer_replica_move(it->second.engine_id, rp, heading);
        } else {
          set_transform(entity, rp, heading, reason);
        }
      } else if (ragdoll_physics && !laid_by_game && e.fields.size() > local::RagdollPose &&
                 valid_ragdoll_pose(std::get<std::string>(e.fields[local::RagdollPose]))) {
        // Its owner's ragdoll pose: the parts are relative to the entity, so the entity takes its
        // owner's position and rotation too. Until the replica's own physics is a ragdoll (the
        // profile takes effect some frames later) it stays where it is: the owner's position
        // follows its ragdoll, and the still animated body floated up in front of the camera on
        // the way there.
        const std::string& pose = std::get<std::string>(e.fields[local::RagdollPose]);
        auto& applied = applied_ragdoll_poses[id];
        unsigned parts = 0;
        if (void* physics = articulated_physics(entity)) {
          if (physics != applied.physics || pose != applied.pose) {
            set_transform(entity, rp, rq, 0);
            parts = apply_ragdoll_pose(entity, pose);
            if (parts) {
              applied.physics = physics;
              applied.pose = pose;
              ++applied.updates;
            }
          }
        }
        static ULONGLONG next_pose_log = 0;
        if (GetTickCount64() >= next_pose_log) {
          next_pose_log = GetTickCount64() + 500;
          log("COMBAT TRAMPLE replica ragdoll pose player=%u parts=%u of %zu updates=%u", id, parts,
              (pose.size() - 4) / kPartTransformSize, applied.updates);
          applied.updates = 0;
        }
      } else if (ragdoll_physics && !laid_by_game) {
        // The owner's position follows its own ragdoll (live probe every 30 ms), so the replica's
        // ragdoll is kept on it: after a fast hit the rider's horse had thrown the replica 3.5 m
        // away from where its owner lay, and it got up there with a jump. Its orientation
        // stays its own: forcing its owner's on the ragdoll every frame spun it (live burst).
        const float* current = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
        if (current) {
          const float dx = rp.x - current[3];
          const float dy = rp.y - current[7];
          const float dz = rp.z - current[11];
          if (dx * dx + dy * dy + dz * dz >
              kTrampleRagdollFollowMeters * kTrampleRagdollFollowMeters) {
            set_transform(entity, rp, rotation(current), 0);
          }
        }
      }
      static std::unordered_map<core::EntityId, bool> last_alignment;
      if (last_alignment[id] != guard_alignment) {
        last_alignment[id] = guard_alignment;
        log("COMBAT native pair alignment player=%u active=%u", id, unsigned(guard_alignment));
      }
      static std::unordered_map<core::EntityId, unsigned> last_action;
      const auto action_type = native_action_type(replica_movement);
      if (!last_action.count(id) || last_action[id] != action_type) {
        last_action[id] = action_type;
        log("COMBAT replica native action player=%u type=%u owns_animation=%u owns_root=%u", id,
            action_type, 0u, unsigned(guard_alignment));
      }
      if (e.fields.size() > local::UiBlocking) {
        std::lock_guard<std::mutex> lock(combat_identity_mutex);
        if (std::get<bool>(e.fields[local::UiBlocking]))
          remote_ui_blocking.insert(id);
        else
          remote_ui_blocking.erase(id);
      }
      if (latest.fields.size() > local::RiposteAbility) {
        const auto ability = std::get<std::int64_t>(latest.fields[local::RiposteAbility]);
        auto replica_soul =
            *reinterpret_cast<void**>(static_cast<std::uint8_t*>(it->second.actor) + 1640);
        if (replica_soul && (ability == 0 || ability == 1) &&
            (it->second.riposte_ability_soul != replica_soul ||
             it->second.riposte_ability_epoch != latest.epoch ||
             it->second.attempted_riposte_ability != (ability == 1))) {
          // One attempt per received state/body/epoch, including explicit removal.
          // A native refusal is logged; it is not retried every engine frame.
          it->second.riposte_ability_soul = replica_soul;
          it->second.riposte_ability_epoch = latest.epoch;
          it->second.attempted_riposte_ability = ability == 1;
          const bool applied =
              kcds::runtime::progression::mirror_riposte_ability(replica_soul, ability == 1);
          log("COMBAT replica riposte ability player=%u epoch=%u enabled=%u applied=%u", id,
              unsigned(latest.epoch), unsigned(ability == 1), unsigned(applied));
        }
      }
      if (auto model = owner_models.find(id); model != owner_models.end()) {
        auto movement_bytes = static_cast<std::uint8_t*>(replica_movement);
        auto replica_core =
            movement_bytes ? *reinterpret_cast<std::uint8_t**>(movement_bytes + 528) : nullptr;
        // The stance and the guard are kept; core + 96 the game itself resets on a watcher's
        // replicas every frame (about 30 corrections in half a second), so it is only given
        // to a relayed pair start (take_owner_models).
        for (std::size_t index = 0; replica_core && index < kKeptModelProperties; ++index) {
          const unsigned owner_value = model->second[index];
          const std::size_t property = kCoreModelProperties[index];
          if (property == kCoreGuard && native_clinch_owns_guard(replica_movement)) {
            continue;
          }
          if (owner_value <= kMaxDefenceMode &&
              *reinterpret_cast<unsigned*>(replica_core + property + 8) != owner_value) {
            take_owner_property(replica_movement, property, owner_value);
          }
        }
      }
      if (latest.fields.size() > local::ReplicaSoul) {
        // The owner's levels and perks on the replica's soul, which the game's combat math here
        // reads when the replica is the attacker. Once per report and soul.
        const auto& report = std::get<std::string>(latest.fields[local::ReplicaSoul]);
        auto replica_soul =
            *reinterpret_cast<void**>(static_cast<std::uint8_t*>(it->second.actor) + 1640);
        kcd2s::integration::ReplicaSoul owner;
        if (replica_soul &&
            (it->second.replica_soul_applied_to != replica_soul ||
             it->second.replica_soul_applied != report) &&
            kcd2s::integration::parse_replica_soul_field(report, owner)) {
          it->second.replica_soul_applied_to = replica_soul;
          it->second.replica_soul_applied = report;
          const bool applied = kcds::runtime::progression::replica_soul::apply(replica_soul, owner);
          log("REPLICA SOUL player=%u strength=%u sword=%u perks=%zu applied=%u", id,
              unsigned(owner.levels[15]), unsigned(owner.levels[8]), owner.perks.size(),
              unsigned(applied));
        }
      }
      if (latest.fields.size() > local::Abilities && latest.fields.size() > local::Vitals) {
        const auto& vitals = std::get<std::string>(latest.fields[local::Vitals]);
        if (it->second.voice_vitals != vitals) {
          it->second.voice_vitals = vitals;
        }
        const auto& abilities = std::get<std::string>(latest.fields[local::Abilities]);
        if (it->second.voice_abilities != abilities) {
          it->second.voice_abilities = abilities;
        }
      }
      if (latest.fields.size() > local::VisorClosed) {
        it->second.voice_visor_closed = std::get<bool>(latest.fields[local::VisorClosed]);
      }
      if (latest.fields.size() > local::NativeRiderState) {
        it->second.voice_riding =
            !std::get<std::string>(latest.fields[local::NativeRiderState]).empty();
      }
      if (latest.fields.size() > local::ReplicaBuffs) {
        // The owner's potions, drinks and injuries on the replica's soul: once per report
        // and soul, and every few seconds again, since a buff runs its own course here.
        constexpr ULONGLONG kRecheckMs = 5000;
        const auto& report = std::get<std::string>(latest.fields[local::ReplicaBuffs]);
        auto replica_soul =
            *reinterpret_cast<void**>(static_cast<std::uint8_t*>(it->second.actor) + 1640);
        const ULONGLONG now_ms = GetTickCount64();
        std::vector<kcd2s::integration::ReplicaBuff> owner_buffs;
        if (replica_soul &&
            (it->second.replica_buffs_applied_to != replica_soul ||
             it->second.replica_buffs_applied != report ||
             now_ms - it->second.replica_buffs_applied_at >= kRecheckMs) &&
            kcd2s::integration::parse_replica_buffs_field(report, owner_buffs)) {
          const bool reported = it->second.replica_buffs_applied != report;
          if (reported || it->second.replica_buffs_applied_to != replica_soul) {
            it->second.replica_buffs_mirror = {};
          }
          it->second.replica_buffs_applied_to = replica_soul;
          it->second.replica_buffs_applied = report;
          it->second.replica_buffs_applied_at = now_ms;
          const unsigned changed = kcds::runtime::buffs::replica::apply(
              replica_soul, owner_buffs, it->second.replica_buffs_mirror);
          if (reported || changed > 0) {
            log("REPLICA BUFFS player=%u owner=%zu changed=%u", id, owner_buffs.size(), changed);
          }
        }
      }
      if (e.fields.size() > local::HeldProp) {
        kcds::runtime::props::observe(id, std::get<std::int64_t>(e.fields[local::HeldProp]));
      }
      if (e.fields.size() > local::AnimationProps) {
        engine_stage = "remote animation props";
        // The field's props and the shipped clip the owner's body plays: a shipped clip has
        // an id of each client's own, so the replica plays it by name from here; its start goes
        // past the replica's synced clip layer as the owner's own animations do.
        // The owner's talking style leads the field; its face clip loops by name too.
        std::optional<kcd2s::integration::ShippedClip> shipped;
        std::string props_field;
        std::string talking;
        std::string field = std::get<std::string>(e.fields[local::AnimationProps]);
        std::string rest;
        std::string speed;
        kcd2s::integration::split_speed_field(field, speed, rest);
        field = rest;
        std::string pointing;
        kcd2s::integration::split_pointing_field(field, pointing, rest);
        field = rest;
        kcds::runtime::animation::pointing::keep_replica(entity, pointing);
        bool upper_body = false;
        kcd2s::integration::split_upper_body_field(field, upper_body, rest);
        field = rest;
        if (kcd2s::integration::split_talking_field(field, talking, rest)) {
          field = rest;
        }
        if (!kcd2s::integration::split_animation_field(field, shipped, props_field)) {
          props_field = field;
        }
        kcds::runtime::animation_props::keep(id, entity, props_field);
        const bool previous_replay = replaying_animation;
        replaying_animation = true;
        kcds::runtime::player_animation::keep_replica_clip(id, entity, shipped);
        // A player talking on voice chat moves their mouth on every screen that hears them, as
        // a script's Player.setSpeaking does; the script's own style wins.
        if (talking.empty() && kcds::runtime::voice::is_talking(id)) {
          talking = "neutral";
        }
        kcds::runtime::player_animation::keep_replica_talking(id, entity, talking);
        replaying_animation = previous_replay;
        // The owner's script clip layer plays the cuffed pose: it moves only the upper body, the
        // legs walk (animation/upper_body.hpp).
        constexpr unsigned kClipLayer = kcds::runtime::player_animation::kScriptClipLayer;
        const auto& layers =
            decoded_animation(id, std::get<std::string>(e.fields[local::Animations]));
        // The owner's upper-body request (its field's "U"): the clip moves the upper body
        // only, the legs walk.
        if (upper_body && layers && (*layers)[kClipLayer].clip >= 0) {
          kcds::runtime::animation::upper_body::keep_legs(entity, kClipLayer);
        }
        if (layers && (*layers)[kClipLayer].clip >= 0 &&
            kcds::runtime::player_animation::is_cuff_pose_clip(entity,
                                                               (*layers)[kClipLayer].clip)) {
          kcds::runtime::animation::upper_body::keep_legs(entity, kClipLayer);
        }
        // The owner's request at a speed of its own or from a start time.
        float mark_speed = 1.0f;
        unsigned mark_layer = 0;
        float mark_place = 0.0f;
        int owner_clip = -1;
        if (layers &&
            kcd2s::integration::parse_speed_mark(speed, mark_speed, mark_layer, mark_place)) {
          owner_clip = (*layers)[mark_layer].clip;
        }
        kcds::runtime::player_animation::keep_replica_speed(id, entity, speed, owner_clip);
      }
      if (e.fields.size() > local::StationAction) {
        engine_stage = "remote station action";
        // The replica's smithing: the station's tools and workpiece where the owner's game
        // holds them.
        const std::string& station_action = std::get<std::string>(e.fields[local::StationAction]);
        // The bellows' lever is pulled with the owner's motion parameters (smithing.hpp).
        kcds::runtime::crafting::smithing::BodyMotion body_motion;
        body_motion.mask =
            std::uint32_t(std::get<std::int64_t>(e.fields[local::MotionMask])) & 0x1ffff;
        for (unsigned i = 0; i < 17; ++i) {
          const auto values = std::get<core::Vec3>(e.fields[local::MotionParams0 + i / 3]);
          body_motion.values[i] = i % 3 == 0 ? values.x : (i % 3 == 1 ? values.y : values.z);
        }
        kcds::runtime::crafting::smithing::keep(id, entity, station_action, local::clock_seconds(),
                                                body_motion);
        kcds::runtime::crafting::alchemy::keep(id, entity, station_action, local::clock_seconds());
        kcds::runtime::crafting::grinding::keep(id, entity, station_action, local::clock_seconds());
      }
      if (latest.fields.size() > local::Outfit) {
        engine_stage = "remote outfit";
        kcds::runtime::appearance::outfit::apply(
            id, it->second.actor, entity, std::get<std::string>(latest.fields[local::Outfit]));
      }
      if (e.fields.size() > local::Grime) {
        engine_stage = "remote grime";
        kcds::runtime::appearance::grime::apply(id, it->second.actor,
                                                std::get<std::string>(e.fields[local::Grime]));
      }
      if (e.fields.size() > local::BodyParts) {
        engine_stage = "remote body parts";
        kcds::runtime::appearance::components::show(
            id, it->second.actor, std::get<std::string>(e.fields[local::BodyParts]));
      }
      if (e.fields.size() > local::Disguise) {
        engine_stage = "remote disguise";
        kcds::runtime::appearance::disguise::keep_replica(
            id, entity, std::get<std::string>(e.fields[local::Disguise]));
      }
      if (latest.fields.size() > local::Hands) {
        engine_stage = "remote hands";
        kcds::runtime::weapons::keep_replica(id, it->second.actor, entity,
                                             std::get<std::string>(latest.fields[local::Hands]));
      }
      if (e.fields.size() > local::Carry) {
        engine_stage = "remote carry";
        const float feet[3] = {rp.x, rp.y, rp.z};
        kcds::runtime::carry::keep_replica(id, it->second.actor, feet,
                                           std::get<std::string>(e.fields[local::Carry]));
      }
      if (e.fields.size() > local::Seats) {
        engine_stage = "remote seats";
        const auto& seats = std::get<std::string>(e.fields[local::Seats]);
        kcds::runtime::seats::keep_replica(id, it->second.engine_id, seats);
        // Seated players do not push or block each other, nor the partner of a
        // paired animation.
        const bool partner = kcds::runtime::player_animation::running_partner() == id;
        // Riders of a cart too: two capsules on neighbouring seats overlap, and the replica was
        // then not drawn at all.
        const bool on_cart = kcds::runtime::carts::placed::rider_on_cart(id);
        // A client script took the body out of the collisions (Player.setCollision).
        const bool scripted = kcds::runtime::seats::collisions::script_passes_through(id);
        kcds::runtime::seats::collisions::keep(id, it->second.engine_id,
                                               !seats.empty() || partner || on_cart || scripted,
                                               !kcds::runtime::seats::local_seats().empty() ||
                                                   kcds::runtime::carts::placed::local_held());
      }
      if (e.fields.size() > local::BodyVisible) {
        engine_stage = "remote body visibility";
        kcds::runtime::appearance::visibility::keep_replica(
            id, entity, std::get<bool>(e.fields[local::BodyVisible]));
      }
      if (e.fields.size() > local::AimEnabled) {
        engine_stage = "remote aim pose";
        apply_remote_aim(it->second, e, entity);
      }
      if (e.fields.size() > local::WeaponState && !ragdolled) {
        engine_stage = "remote weapon state";
        kcds::runtime::ranged::keep_weapon_state(
            it->second.actor, std::uint8_t(std::get<std::int64_t>(e.fields[local::WeaponState])));
      }
      if (e.fields.size() > local::LookTuning) {
        engine_stage = "remote native look IK";
        const AppliedLook applied =
            apply_remote_look(it->second, e, entity, replica_movement, guard_alignment);
        void* look = applied.look;
        const auto& target = applied.target;
        const bool valid_look = applied.valid;
        const bool enabled = applied.enabled;
        static double next_look_log = 0;
        if (now > next_look_log) {
          next_look_log = now + 3;
          log("LOOK native source=%p valid=%u enabled=%u target=(%.2f %.2f %.2f) replica=%p "
              "valid=%u enabled=%u target=(%.2f %.2f %.2f)",
              local_look, unsigned(look_valid), unsigned(look_enabled), look_target.x,
              look_target.y, look_target.z, look, unsigned(valid_look), unsigned(enabled), target.x,
              target.y, target.z);
        }
        const bool seated = e.fields.size() > local::Seats &&
                            !std::get<std::string>(e.fields[local::Seats]).empty();
        const std::int64_t posture =
            e.fields.size() > local::Posture ? std::get<std::int64_t>(e.fields[local::Posture]) : 0;
        log_replica_look(id, look_posture_name(false, seated, posture), applied,
                         it->second.skeleton,
                         std::get<std::string>(latest.fields[local::Animations]));
      }
      if (!ragdolled) {
        engine_stage = "remote base layer check";
        watch_base_layer(id, it->second);
      }
      if (e.fields.size() > local::LookDirection && !ragdolled) {
        engine_stage = "remote view rotation";
        apply_remote_view(it->second.actor, std::get<core::Vec3>(e.fields[local::LookDirection]));
      }
      if (std::get<bool>(e.fields[local::MotionValid])) {
        const auto type = std::get<std::int64_t>(e.fields[local::MotionType]);
        const auto flags = std::get<std::int64_t>(e.fields[local::MotionFlags]);
        NativeMovementRequest request;
        request.type = std::uint32_t(type);
        request.velocity = std::get<core::Vec3>(e.fields[local::MotionVelocity]);
        request.rotation = replica_turn;
        request.allow_strafe = (flags & 1) != 0;
        request.jumping = (flags & 2) != 0;
        if (type >= 0 && type <= 6) {
          engine_stage = "remote AnimatedCharacter movement request";
          auto animated = slot<void*(__fastcall*)(void*)>(it->second.actor, 90)(it->second.actor);
          auto skeleton = it->second.skeleton;
          // The owner's clips play as soon as they arrive, not on the smoothed timeline the
          // position follows: an attack, block or parry shown 100 ms late made the swords
          // miss each other on the other player's screen.
          const auto& clips =
              decoded_animation(id, std::get<std::string>(latest.fields[local::Animations]));
          RemoteInputs remote;
          remote.movement = replica_movement;
          remote.animated = animated;
          remote.motion = request;
          remote.valid = true;
          remote.mask =
              std::uint32_t(std::get<std::int64_t>(e.fields[local::MotionMask])) & 0x1ffff;
          for (unsigned i = 0; i < 6; ++i) {
            const auto values = std::get<core::Vec3>(e.fields[local::MotionParams0 + i]);
            remote.params[i * 3] = values.x;
            remote.params[i * 3 + 1] = values.y;
            remote.params[i * 3 + 2] = values.z;
          }
          if (clips)
            remote.animations = *clips;
          if (skeleton) {
            std::lock_guard<std::shared_mutex> lock(motion_mutex);
            // While it falls, the trampled replica is not managed by its owner's animation: its
            // own knockdown reaction starts clips on the layers the replication otherwise owns
            // (managed_layer), which were refused (the reaction was queued, no fall seen).
            if (trample_falling(id) || ragdolled) {
              remote_inputs.erase(skeleton);
            } else {
              remote_inputs.assign(skeleton, remote);
            }
          }
          if (animated && !guard_alignment && !ragdolled)
            original_movement_request(animated, &request);
          // The server already seats this player on a horse, but the replica is not natively
          // mounted yet (tick_native_riders mounts it after this loop). Riding clips started on the
          // on-foot replica crashed native animation (live 0.1.1: a mount accepted late, after the
          // horse's simulator left), so they are consumed without playing, as while mounted.
          const bool awaiting_mount = horse_riders.contains(std::int64_t(id));
          // The owner's riding look poses wait for the mount (look/look_pose_layer.hpp).
          if (skeleton && clips && awaiting_mount) {
            for (unsigned layer = 0; layer < 16; ++layer) {
              const auto& clip = (*clips)[layer];
              auto& applied = it->second.animation_sequences[layer];
              const auto step =
                  kcds::runtime::look::owner_clip_step(layer, clip.sequence, applied, true);
              if (step == kcds::runtime::look::OwnerClipStep::Wait) {
                note_waiting_look_pose(id, it->second, clip);
                continue;
              }
              applied = std::max(applied, clip.sequence);
            }
          }
          if (skeleton && clips && !awaiting_mount && !ragdolled)
            return_reaction_layers(skeleton, *clips, it->second.animation_sequences);
          if (!awaiting_mount && !ragdolled)
            keep_replica_in_fight(id, it->second.actor, replica_movement);
          // A pair (SYNC_PERFECT_BLOCK 9 and its hit 10) runs 2-4 s on the game's own clips: the
          // game animates the replica's half for all of it, not only the first reaction window
          // (both halves froze mid-pair once the window closed, on the struck player's
          // and on a watcher's screen).
          if (replica_movement && !ragdolled) {
            const unsigned action = native_action_type(replica_movement);
            if (action == 9 || action == 10)
              open_reaction_window(id);
          }
          // The owner's body is a ragdoll (2) or asleep (3), as its posture field says.
          kcd2s::integration::PlayerPosture owner_body;
          const bool owner_limp =
              latest.fields.size() > local::Posture &&
              kcd2s::integration::parse_posture_field(
                  std::get<std::int64_t>(latest.fields[local::Posture]), owner_body) &&
              (owner_body.physics_profile == 2 || owner_body.physics_profile == 3);
          if (skeleton && clips && !awaiting_mount && !ragdolled)
            for (unsigned layer = 0; layer < 16; ++layer) {
              if (overlay_owned && layer == local::kHumanOverlayLayer) {
                continue;
              }
              const auto& clip = (*clips)[layer];
              auto& applied = it->second.animation_sequences[layer];
              if (!clip.sequence || clip.sequence <= applied)
                continue;
              // A knocked-out owner's layers empty as its body turns into a ragdoll; the replica
              // keeps playing its own game's collapse until its ragdoll profile takes effect
              // (tick_owner_ragdoll): stopping it showed the bind pose, a body standing with its
              // arms out, until then.
              // So does a body its owner's game made a ragdoll or put to sleep (a paired kill's
              // victim): its clip ends as the ragdoll takes over.
              if (clip.clip < 0 && (trample_falling(id) ||
                                    kcds::runtime::knockout::replica_down(id) || owner_limp)) {
                applied = clip.sequence;
                continue;
              }
              {
                // The game already plays this clip from a replayed combat reaction.
                std::lock_guard<std::mutex> lock(reaction_clips_mutex);
                auto played = reaction_clips.find(skeleton);
                // The owner's clip that follows on the same layer is the same clip, or, after a
                // defence or pair the game started, its own version of the same move (the game
                // picks its own variant here: 4829 for the owner's 4843).
                const ReactionClip* reaction =
                    played != reaction_clips.end() ? &played->second[layer] : nullptr;
                if (reaction && reaction->clip >= 0 &&
                    GetTickCount64() - reaction->at < kReactionClipMs &&
                    (reaction->clip == clip.clip || reaction->variant)) {
                  played->second[layer] = ReactionClip{};
                  applied = clip.sequence;
                  log("REMOTE ANIMATION player=%u clip=%d layer=%u already played by the reaction",
                      id, clip.clip, layer);
                  continue;
                }
              }
              engine_stage = "remote native animation";
              replaying_animation = true;
              // appends to the native FIFO with the supplied
              // transition/queue flags. Clearing before every start cuts a hit
              // reaction as soon as its queued return-to-idle arrives.
              if (clip.clip < 0 && applied) {
                // The owner's game ended the clip with a blend (a block let go: the weapon goes
                // back from the guard smoothly); a clear put it back at once.
                original_stop_animation(skeleton, layer, kRemoteStopBlendSeconds);
              } else if (!applied || clip.clip < 0) {
                original_clear_animation(skeleton, layer);
              }
              char accepted = 1;
              if (clip.clip >= 0)
                accepted = original_start_animation(skeleton, clip.clip, &clip.params);
              replaying_animation = false;
              // Rejected native assets can finish streaming later. Retry without consuming the
              // sequence.
              if (accepted) {
                applied = clip.sequence;
                static LogBudget budget;
                unsigned left_out = 0;
                if (log_budget(budget, 20, left_out)) {
                  log("REMOTE ANIMATION player=%u clip=%d layer=%u sequence=%llu accepted=%d (%u "
                      "like it left out)",
                      id, clip.clip, layer, clip.sequence, accepted, left_out);
                }
                // A cutscene's clip on the script clip layer: the body is kept where its player
                // stands (pose_animation/clip_in_place.hpp).
                if (layer == kcds::runtime::player_animation::kScriptClipLayer && clip.clip >= 0) {
                  kcds::runtime::pose_animation::clip_in_place::watch(it->second.engine_id);
                }
                // A paired request's slave places itself by its master's clip.
                if (layer == 0) {
                  kcds::runtime::player_animation::replica_base_clip(
                      id, get_entity(entity_system, it->second.engine_id), clip.clip);
                }
              }
            }
          // Its owner's getting up is on it now: its ragdoll turns into a living body.
          finish_owner_get_up(id, it->second.actor, it->second.animation_sequences[0]);
          if (skeleton) {
            // A replica its owner's clips do not animate (waiting for its horse, lying) holds no
            // climb clip either.
            const bool owner_animated = clips && !awaiting_mount && !ragdolled;
            hold_ladder_clip(id, skeleton, owner_animated ? (*clips)[0].clip : -1);
          }
          // The owner's weapon clips on the weapon the replica holds; a weapon it takes
          // into its hands plays the owner's latest ones.
          if (e.fields.size() > local::WeaponAnimations && !ragdolled) {
            void* weapon = held_weapon_skeleton(it->second.actor, entity_system);
            if (weapon != it->second.weapon_skeleton) {
              it->second.weapon_skeleton = weapon;
              std::fill(std::begin(it->second.weapon_sequences),
                        std::end(it->second.weapon_sequences), 0);
            }
            const auto& weapon_clips = decoded_animation(
                id, std::get<std::string>(e.fields[local::WeaponAnimations]), true);
            if (weapon && weapon_clips) {
              for (unsigned layer = 0; layer < 16; ++layer) {
                const auto& clip = (*weapon_clips)[layer];
                auto& applied = it->second.weapon_sequences[layer];
                if (!clip.sequence || clip.sequence <= applied) {
                  continue;
                }
                engine_stage = "remote weapon animation";
                replaying_animation = true;
                if (clip.clip < 0) {
                  original_clear_animation(weapon, layer);
                }
                const char accepted =
                    clip.clip >= 0 ? original_start_animation(weapon, clip.clip, &clip.params) : 1;
                replaying_animation = false;
                if (accepted) {
                  applied = clip.sequence;
                  log("REMOTE WEAPON ANIMATION player=%u clip=%d layer=%u sequence=%llu", id,
                      clip.clip, layer, clip.sequence);
                }
              }
            }
          }
          if (skeleton && !guard_alignment) {
            auto remote_mask = std::get<std::int64_t>(e.fields[local::MotionMask]);
            for (unsigned i = 0; i < 17 && skeleton; ++i)
              if (remote_mask & (1LL << i)) {
                const auto values = std::get<core::Vec3>(e.fields[local::MotionParams0 + i / 3]);
                const float value = i % 3 == 0 ? values.x : (i % 3 == 1 ? values.y : values.z);
                if (std::isfinite(value))
                  original_motion_param(skeleton, int(i), value);
              }
          }
          static double next = 0;
          if (now > next) {
            next = now + 3;
            log("REMOTE MOTION player=%u type=%lld velocity=(%.2f %.2f %.2f) mask=0x%llx", id, type,
                request.velocity.x, request.velocity.y, request.velocity.z,
                std::get<std::int64_t>(e.fields[local::MotionMask]));
          }
        }
      }
    }
  }
  tick_native_riders(entities, own, local_actor, actor_system, now);
  engine_stage = "nameplates";
  keep_nameplates(entities, own, local_entity, entity_system);
  keep_brands(entities, own, local_combat_actor.load());
  keep_physical_props(entities, own, local_combat_actor.load());
  keep_lead_ropes(entities);
  keep_pose_animations(entities, own, local_combat_actor.load());
  keep_npc_nameplates(entities, entity_system);
  finished = true;
}

}  // namespace kcds::game_client
