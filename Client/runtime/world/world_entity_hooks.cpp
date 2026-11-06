#include "world_entity_hooks.hpp"

#include "../attach/player_attach.hpp"
#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../input/interaction_rules.hpp"
#include "../interactions/station_prompts.hpp"
#include "../interactions/talk_prompts.hpp"
#include "../interactions/world_interactions.hpp"
#include "../npcs/carcass_prompts.hpp"
#include "../npcs/local_npcs.hpp"
#include "../npcs/npc_body.hpp"
#include "../npcs/npc_health.hpp"
#include "../props/placed_props.hpp"
#include "../replicas/remote.hpp"
#include "../vfx/placed_objects_tick.hpp"
#include "kcd2s/integration/interaction_rules.hpp"
#include "../riding/whistle.hpp"
#include "../siege/siege_engines.hpp"
#include "../siege/siege_ladders.hpp"
#include "../siege/stone_piles.hpp"

namespace kcds::game_client {

void install_world_entity_hooks() {
  // The bodies of the server's NPCs (npcs/npc_body.hpp): the game's own NPC entity
  // with the soul the server names.
  kcds::runtime::npcs::BodyNatives bodies;
  bodies.entity_system = []() -> void* {
    void* framework = framework_slot ? *framework_slot : nullptr;
    auto actors = framework
                      ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + 1304)
                      : nullptr;
    return actors ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actors) + 48) : nullptr;
  };
  bodies.spawn_params_init = resolve_unique(
      "EntitySpawnParams", "48 89 5C 24 08 57 48 83 EC 20 33 FF 48 8D 05 ?? ?? ?? ?? 48 89 41 28");
  kcds::runtime::npcs::install_bodies(bodies);
  log("NPC bodies %s", bodies.spawn_params_init ? "installed" : "unavailable");
  // The client resources' NPCs only this game shows (LocalNpc).
  kcds::runtime::npcs::local::install(log);
  // The resources' interactions on things in the world.
  kcds::runtime::interactions::Natives interaction_natives;
  interaction_natives.entity_system = bodies.entity_system;
  interaction_natives.component =
      resolve_unique("EntityComponent", "4C 8B 81 A8 00 00 00 45 33 D2 49 8B C8");
  // A body (player, NPC, horse) or a server prop by its network id.
  interaction_natives.body_of = [](std::uint32_t network_id) -> void* {
    if (void* body = placed_body_entity(network_id)) {
      return body;
    }
    return kcds::runtime::props::placed::entity_of(network_id);
  };
  kcds::runtime::interactions::install(interaction_natives, log);
  // The game's own Talk and chat prompts on people, as the server allows them.
  kcds::runtime::interactions::talk::Natives talk_natives;
  talk_natives.can_talk =
      resolve_unique("HumanCanTalk",
                     "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 40 "
                     "48 8B 01 4D");
  talk_natives.can_chat = resolve_unique(
      "HumanCanChat", "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 40 48 8B 01 44");
  talk_natives.chat_request = resolve_unique(
      "DialogueChatRequest",
      "40 53 48 83 EC 20 48 8B D9 48 8B 89 A0 00 00 00 48 85 C9 75 08 33 C0 48 83 C4 20 5B C3 48 "
      "83 C1 18 48 8B 01 FF 90 80 00 00 00 84 C0 75 E7");
  talk_natives.accepted_chat = resolve_unique(
      "DialogueAcceptedChat",
      "48 89 5C 24 08 57 48 83 EC 20 E8 ?? ?? ?? ?? 48 8B 88 50 01 00 00 48 8B 41 10 48 8B 88 90 "
      "00 00 00 48 8D 41 18 48 F7 D9 48 1B DB 48 23 D8 75 0D");
  talk_natives.npc_entity_closed = [](std::uint32_t entity) {
    const auto body = kcds::runtime::npcs::health::body_of_entity(entity);
    if (!body.has_value()) {
      return false;
    }
    return !kcds::runtime::input::interaction_rules::allowed(kcd2s::integration::kAllowNpcChat) ||
           !kcds::runtime::npcs::health::interactable(body->npc);
  };
  talk_natives.npc_soul = [](const void* soul) {
    return kcds::runtime::npcs::health::body_of_soul(soul).has_value();
  };
  talk_natives.npc_closed = [](const void* soul) {
    const auto body = kcds::runtime::npcs::health::body_of_soul(soul);
    return body.has_value() && !kcds::runtime::npcs::health::interactable(body->npc);
  };
  talk_natives.replica_soul = [](const void* soul) {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    return remote_combat_souls.count(const_cast<void*>(soul)) != 0;
  };
  kcds::runtime::interactions::talk::install(talk_natives, log);
  // The game's own prompts at its fires, smokehouses and drying racks, as the server allows them.
  kcds::runtime::interactions::stations::Natives station_natives;
  station_natives.interactor_step =
      resolve_unique("PlayerInteractorStep",
                     "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 30 8B 3A 48 "
                     "8B F1 48 8B CA 4C "
                     "8B F2 45 32 FF 32 DB 45 32 E4 E8");
  station_natives.properties = resolve_unique(
      "EntityScriptProperties",
      "48 89 5C 24 10 57 48 83 EC 20 48 8B 49 08 48 8B FA 48 C7 44 24 30 00 00 00 00 E8");
  station_natives.sub_table = resolve_unique("ScriptTableSubTable",
                                             "48 8B C4 48 89 58 08 48 89 70 10 57 48 83 EC 40 48 "
                                             "8B D9 C7 40 D8 06 00 00 00 49 8B 08 49 8B "
                                             "F8 48 89 48 E0 48 8B F2");
  static void* (*entity_system)() = nullptr;
  entity_system = bodies.entity_system;
  station_natives.entity = [](unsigned id) -> void* {
    void* system = entity_system();
    return system ? get_entity(system, id) : nullptr;
  };
  kcds::runtime::interactions::stations::install(station_natives, log);
  // The game's Search and Gut on a dead animal the server closed. The binding's own
  // calls give the handler's soul (the E8 at + 24) and its Return(bool).
  kcds::runtime::npcs::carcass_prompts::Natives carcass_natives;
  constexpr char kHasScriptContext[] =
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B CA 49 8B F0 48 8B DA E8 ?? ?? ?? ?? 48 "
      "8B F8 48 85 C0 0F 84";
  carcass_natives.has_script_context = resolve_unique("SoulHasScriptContext", kHasScriptContext);
  carcass_natives.soul_of_handler = resolve_unique("HandlerSoul", kHasScriptContext, 25, 29);
  carcass_natives.return_bool = resolve_unique(
      "HandlerReturnBool",
      "88 44 24 48 48 8D 54 24 48 48 8B CB E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 8B 74 24 38", 13, 17);
  kcds::runtime::npcs::carcass_prompts::install(carcass_natives, log);
  // The players' whistles for their horses, heard by the players near them.
  kcds::runtime::riding::whistle::Natives whistle_natives;
  whistle_natives.call_horse =
      resolve_unique("HorseCallWhistle", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 80 79 30");
  whistle_natives.whistle_sound =
      resolve_unique("HorseWhistleSound",
                     "48 89 5C 24 08 57 48 83 EC 50 0F 29 74 24 40 48 8B F9 E8 ?? ?? ?? ?? 48 "
                     "8B 90 50 01 00 00 48 8B 42 10 48 83 B8 90 00 00 00 00 75");
  whistle_natives.empty_callback = resolve_unique(
      "AudioEmptyCallback",
      "48 83 EC 28 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA 18 00 00 00 48 8B 04 C8 8B 04 "
      "02 39 05 ?? ?? ?? ?? 7F ?? 48 8D 05 ?? ?? ?? ?? 48 83 C4 28 C3 48 8D 0D ?? ?? ?? ?? E8 ?? "
      "?? ?? ?? 83 3D ?? ?? ?? ?? FF 75 ?? 0F 57 C0 48");
  whistle_natives.audio_system = static_cast<void**>(resolve_unique(
      "AudioSystem", "89 45 ?? 85 C0 75 ?? 48 8B 0D ?? ?? ?? ?? 4C 8D 45 ?? 48 8B 01 FF 50 38", 10,
      14));
  whistle_natives.local_player = []() -> void* { return local_combat_actor.load(); };
  whistle_natives.replica_entity = [](std::uint32_t player) -> void* {
    const auto found = remotes.find(core::EntityId(player));
    if (found == remotes.end() || !found->second.actor) {
      return nullptr;
    }
    return *reinterpret_cast<void**>(static_cast<std::uint8_t*>(found->second.actor) + 56);
  };
  kcds::runtime::riding::whistle::install(whistle_natives, log);
  // One player attached to another (Player.attachTo).
  kcds::runtime::attach::Natives attach_natives;
  attach_natives.physics_component = resolve_unique(
      "EntityPhysicsComponentAttach",
      "48 89 5C 24 10 55 56 57 41 56 41 57 48 8B EC 48 83 EC 40 48 8D 99 A8 00 00 00");
  attach_natives.local_entity = []() -> void* {
    void* actor = local_combat_actor.load();
    return actor ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 56) : nullptr;
  };
  attach_natives.replica_entity = whistle_natives.replica_entity;
  kcds::runtime::attach::install(attach_natives, log);
  // The server's siege engines, drawn as props.
  kcds::runtime::siege::install(log);
  // The server's siege ladders (the game's Ladder class) and stone piles.
  kcds::runtime::siege::ladders::install(log);
  kcds::runtime::siege::stones::install(log);
}

}  // namespace kcds::game_client
