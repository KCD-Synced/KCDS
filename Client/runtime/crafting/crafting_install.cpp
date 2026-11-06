// Winsock before Windows.h (see client_internal.hpp).
#include "../animation/hand_props.hpp"
#include "../core/client_internal.hpp"
#include "../lockpicking/lockpicking.hpp"

#include "crafting_install.hpp"

#include <cstdint>
#include <cstring>

#include "../animation/animation_capture.hpp"
#include "../core/client_state.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../preparing/preparing.hpp"
#include "../props/prop_entity.hpp"
#include "../replicas/motion_capture.hpp"
#include "alchemy.hpp"
#include "emitters.hpp"
#include "grinding.hpp"
#include "permits.hpp"
#include "smithing.hpp"
#include "sounds.hpp"
#include "station_clips.hpp"

namespace kcds::game_client {

void install_crafting(const kcds::runtime::props::Natives& natives) {
  // Smithing seen by everyone: the minigame's update is found by its RTTI name.
  kcds::runtime::crafting::smithing::Natives smithing;
  smithing.image = image;
  smithing.image_size = image_size;
  smithing.attach_child = natives.attach_child;
  // The station's effects: a particle component switched on or off and an
  // entity's particle component.
  smithing.particle_enable = resolve_unique(
      "ParticleComponentEnable",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F9 40 8A F2 48 8B 49 28 8B 57 68 48 8B "
      "01 FF 90 40 03 00 00");
  smithing.particle_component = resolve_unique(
      "ParticleComponentOf",
      "48 83 EC 28 48 8B 02 48 8B CA BA 12 00 00 00 FF 90 50 02 00 00 48 8B C8 48 83 C4 28 E9");
  smithing.particle_activate = resolve_unique(
      "ParticleComponentActivate",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 83 79 68 00 48 8B D9 7D 05 E8 ?? ?? ?? ?? 83 "
      "7B 68 00 7C ?? 80 7B 6E 00");
  smithing.particle_restart = resolve_unique(
      "ParticleComponentRestart",
      "40 53 48 83 EC 20 83 79 68 00 48 8B D9 7C 05 E8 ?? ?? ?? ?? 80 7B 6D 00 74 08 48 8B CB E8");
  smithing.allocate =
      resolve_unique("GameAllocate",
                     "40 53 48 83 EC 20 83 3D ?? ?? ?? ?? 00 48 8B D9 75 05 E8 ?? ?? ?? ?? 48 8B "
                     "05 ?? ?? ?? ?? 48 8D 54 24 38 45 33 C0 FF D0");
  smithing.hand_take =
      resolve_unique("HandSlotTake",
                     "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 60 20 55 41 56 41 57 48 "
                     "8B EC 48 83 EC 70 4D 8B E1 49 63 D8 44 8B FA");
  smithing.hand_release =
      resolve_unique("HandSlotRelease",
                     "48 8B C4 48 89 58 20 55 56 57 41 56 41 57 48 8D 6C 24 90 48 81 EC 70 01 00 "
                     "00 0F 29 70 C8 0F 29 78 B8 44 0F 29 40 A8 44");
  // CCryAction::GetIActorSystem is the framework + 1304; GetActor is its slot 3.
  smithing.actor_of = [](std::uint32_t entity) -> void* {
    void* framework = framework_slot ? *framework_slot : nullptr;
    void* actors = framework
                       ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + 1304)
                       : nullptr;
    if (!actors) {
      return nullptr;
    }
    using GetActor = void*(__fastcall*)(void* actors, std::uint32_t entity);
    return reinterpret_cast<GetActor>((*static_cast<void***>(actors))[3])(actors, entity);
  };
  smithing.entity_attachment =
      resolve_unique("EntityAttachment",
                     "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 4C 8B F1 B8 "
                     "00 00 80 3F 33 C9 41 8A D9 49 8B F0 48 8B FA 41 89 4E 08 41 89 46 0C");
  // The workpiece's glow: a material of its own for a part of an entity's character
  smithing.zone_glow = resolve_unique(
      "BlacksmithZoneGlow",
      "48 8B C4 55 53 56 57 41 54 41 56 41 57 48 8D 68 A1 48 81 EC 90 00 00 00 0F 29 70 B8 4C");
  smithing.part_material = resolve_unique(
      "CharacterPartMaterial",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 48 81 EC 90 00 00 00 49 8B E9 48 8B FA 48 "
      "85 D2 0F 84 ?? ?? ?? ?? 48 8B 02 48 8B CF 33 D2 FF 90");
  // The stations' sounds: an entity's component by type (as the physics props use
  // it) and the audio system, from the entity binding's ExecuteAudioTrigger (+ 0xF0).
  kcds::runtime::crafting::sounds::Natives station_sounds;
  station_sounds.entity_component = natives.physics_component;
  station_sounds.audio_system = static_cast<void* const*>(resolve_unique(
      "AudioSystem",
      "48 8B 0D ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 41 B9 02 00 00 00 C7 44 24 20 04 00 00 00 4D 8B "
      "C6 48 8B 01 FF 50 20 41 C6 86 80 03 00 00 01",
      3, 7));
  station_sounds.log = log;
  kcds::runtime::crafting::sounds::install(station_sounds);
  // One smith at a station: whether a user may smith there.
  smithing.can_use = resolve_unique(
      "SmitheryCanUse",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 8B F2 8B D1 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF "
      "50 70 48 8B 0D ?? ?? ?? ?? 8B D6 48 8B F8");
  // The recipe book's close action.
  smithing.close_recipe_book = resolve_unique(
      "SmithingCloseBook",
      "40 53 48 83 EC 20 48 8B 59 08 E8 ?? ?? ?? ?? 48 8B 43 68 48 8D 4B 68 FF 50 60 B2 03 48 8D "
      "88 80 00 00 00 48 83 C4 20 5B E9");
  // A stroke's sparks, an overheated iron's smoke, an alchemy table's fire: the emitters' spawn
  // parameters (SSpawnParams' constructor).
  kcds::runtime::crafting::emitters::Natives emitters;
  emitters.spawn_params_init = resolve_unique(
      "SpawnParamsInit",
      "40 53 48 83 EC 20 48 8B D9 B8 00 00 80 3F 33 C9 89 0B 66 C7 43 04 00 01 C6 43 06 01 89 43 "
      "08 89 43 0C 89 43 10");
  emitters.log = log;
  kcds::runtime::crafting::emitters::install(emitters);
  // The bellows' own clip and pull (smithing.hpp): the clip functions' originals past the hooks
  // (the bag is neither a player nor a replica), read when called, as they are hooked later.
  smithing.start_clip = [](void* skeleton, int clip, std::uint32_t flags, float blend,
                           float key_time) {
    local::AnimationParams params;
    params.transition = blend;
    params.normalized_time = key_time;
    params.layer = 0;
    params.flags = flags;
    return original_start_animation && original_start_animation(skeleton, clip, &params) != 0;
  };
  smithing.clear_clips = [](void* skeleton) {
    if (original_clear_animation) {
      original_clear_animation(skeleton, 0);
    }
  };
  smithing.stop_clips = [](void* skeleton) {
    if (original_stop_animation) {
      original_stop_animation(skeleton, 0, kRemoteStopBlendSeconds);
    }
  };
  smithing.log = log;
  // The same clip natives for every station's animated things (station_clips.hpp).
  kcds::runtime::crafting::clips::Natives clip_natives;
  clip_natives.start_clip = smithing.start_clip;
  clip_natives.stop_clips = smithing.stop_clips;
  clip_natives.clear_clips = smithing.clear_clips;
  kcds::runtime::crafting::clips::install(clip_natives);
  log("SMITHING %s",
      kcds::runtime::crafting::smithing::install(smithing) ? "installed" : "unavailable");
  // The tools the game itself puts in the local player's hands, on the same hand
  // slot functions, hooked.
  kcds::runtime::animation::hand_props::Natives hands;
  kcds::runtime::lockpicking::Natives lockpicking;
  lockpicking.update = reinterpret_cast<void*>(
      resolve_unique("LockpickingUpdate",
                     "48 89 5C 24 08 57 48 83 EC 30 48 8D B9 88 00 00 00 0F 29 74 24 20 8A 07 0F "
                     "28 F1 48 8B D9 3C 03 74 06 2C 04"));
  log("LOCKPICK %s",
      kcds::runtime::lockpicking::install(lockpicking, log) ? "installed" : "unavailable");
  hands.hand_take = smithing.hand_take;
  hands.hand_release = smithing.hand_release;
  hands.local_hands = []() -> void* {
    void* actor = local_combat_actor.load();
    return actor ? slot<void*(__fastcall*)(void*)>(actor, 194)(actor) : nullptr;
  };
  hands.local_entity = []() -> void* {
    auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
    return actor ? *reinterpret_cast<void**>(actor + 56) : nullptr;
  };
  log("HAND props %s",
      kcds::runtime::animation::hand_props::install(hands, log) ? "installed" : "unavailable");
  // Alchemy at an alchemy table: the table's things as the minigame shows them.
  kcds::runtime::crafting::alchemy::Natives alchemy;
  alchemy.image = smithing.image;
  alchemy.image_size = smithing.image_size;
  alchemy.particle_enable = smithing.particle_enable;
  alchemy.particle_component = smithing.particle_component;
  alchemy.particle_activate = smithing.particle_activate;
  alchemy.log = log;
  log("ALCHEMY %s",
      kcds::runtime::crafting::alchemy::install(alchemy) ? "installed" : "unavailable");
  // Sharpening at a grindstone: its wheel, sparks and sound; the particle manager
  // read where the minigame's effect at a character joint uses it.
  constexpr char kSpawnEffect[] =
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 81 EC B0 00 00 00 49 8B F9 "
      "41 8B D8 48 8B EA E8 ?? ?? ?? ?? 8B D3 48 8B 88 90 01 00 00 48 8B 01 FF 50 70 45 33 FF 48 "
      "8B F0 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 0D";
  kcds::runtime::crafting::grinding::Natives grinding;
  grinding.image = smithing.image;
  grinding.image_size = smithing.image_size;
  grinding.particle_manager =
      static_cast<void* const*>(resolve_unique("ParticleManager", kSpawnEffect, 74, 78));
  grinding.log = log;
  log("GRINDING %s",
      kcds::runtime::crafting::grinding::install(grinding) ? "installed" : "unavailable");
  // Opening a smithy or an alchemy table: one player at a station, and the server asked first
  // the script bindings' StartMinigame and StartAlchemy.
  kcds::runtime::crafting::permits::Natives permits;
  permits.start_minigame = resolve_unique(
      "StartMinigameRequest",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? 48 8B CB 48 "
      "8B F0 E8 ?? ?? ?? ?? 44 0F B6 4F 04 41 83 E9 02");
  permits.start_smithing = resolve_unique(
      "StartBlacksmithing",
      "48 89 5C 24 08 57 48 83 EC 30 48 8B 49 60 49 8B C0 49 8B D9 C6 44 24 20 00 48 8B FA 41 B1 "
      "01 8B D0 41 B0 0C E8");
  permits.start_alchemy = resolve_unique(
      "StartAlchemy",
      "48 89 5C 24 08 57 48 83 EC 30 48 8B 49 60 49 8B C0 49 8B D9 C6 44 24 20 00 48 8B FA 41 B1 "
      "01 8B D0 41 B0 03 E8");
  permits.smithy_taken = [](const float position[3]) {
    return kcds::runtime::crafting::smithing::station_taken(position);
  };
  permits.table_taken = [](const float position[3]) {
    return kcds::runtime::crafting::alchemy::table_taken(position);
  };
  permits.log = log;
  kcds::runtime::crafting::permits::install(permits);

  // Cooking, smoking and drying asked of the server: the interrupt node's start,
  // C_PrepareItem's start and, through the interrupt node's own calls, a node's
  // context (+ 0x1F) and string attribute (+ 0x11B).
  constexpr char kInterruptNode[] =
      "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E0 48 81 EC 20 01 00 00 4C 8B "
      "F9 E8 ?? ?? ?? ?? 4C 8B E8 48 8D 48 18 E8";
  kcds::runtime::preparing::Natives preparing;
  preparing.interrupt_node = resolve_unique("InterruptNodeStart", kInterruptNode);
  preparing.prepare_item =
      resolve_unique("PrepareItemStart",
                     "48 89 5C 24 08 55 56 57 41 56 41 57 48 8D 6C 24 C9 48 81 EC B0 00 00 00 48 "
                     "8B F9 E8 ?? ?? ?? ?? 48 8B F0 48 8D 58 18 48 8B 13 48 8B CB FF 52 38");
  preparing.node_context = resolve_unique("InterruptNodeContext", kInterruptNode, 0x20, 0x24);
  preparing.string_attribute = resolve_unique("InterruptNodeString", kInterruptNode, 0x11C, 0x120);
  preparing.log = log;
  preparing.local_position = [](float position[3]) {
    auto* actor = static_cast<std::uint8_t*>(local_combat_actor.load());
    void* body = actor ? *reinterpret_cast<void**>(actor + 56) : nullptr;
    if (!body) {
      return false;
    }
    kcds::runtime::props::entity::world_position(body, position);
    return true;
  };
  kcds::runtime::preparing::install(preparing);
}

}  // namespace kcds::game_client
