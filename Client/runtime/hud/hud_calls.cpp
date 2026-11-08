#include "hud_calls.hpp"

#include "../core/client_internal.hpp"
#include "../action_hints/resource_action_hints.hpp"
#include "../architect/cursor_pick.hpp"
#include "../architect/level_plants.hpp"
#include "../architect/level_properties.hpp"
#include "../architect/user_assets.hpp"
#include "../book/resource_book.hpp"
#include "../camera/photo_mode.hpp"
#include "../camera/third_person.hpp"
#include "../character_creator/creator_stage.hpp"
#include "../chat/chat_overlay.hpp"
#include "../crafting/alchemy.hpp"
#include "../crafting/grinding.hpp"
#include "../crafting/smithing.hpp"
#include "../core/log.hpp"
#include "../dialogue/dialogue_list.hpp"
#include "../input/cuffs.hpp"
#include "../attach/player_attach.hpp"
#include "../input/frozen.hpp"
#include "../lobby/lobby.hpp"
#include "../input/interaction_rules.hpp"
#include "../riding/native_horses.hpp"
#include "../riding/whistle.hpp"
#include "../interactions/world_interactions.hpp"
#include "../knockout/knockout.hpp"
#include "../native_ui/native_ui.hpp"
#include "../needs/needs.hpp"
#include "../noclip/free_camera.hpp"
#include "../npcs/npc_use.hpp"
#include "../physics/local_world.hpp"
#include "../pillory/pillory.hpp"
#include "../props/ghosts.hpp"
#include "../props/held_props.hpp"
#include "../props/placed_props.hpp"
#include "../props/placer.hpp"
#include "../props/prop_entity.hpp"
#include "../stats/player_stats.hpp"
#include "../vendors/vendors_tick.hpp"
#include "../world_map/resource_world_map.hpp"
#include "../world/trespass.hpp"
#include "engine_script.hpp"
#include "resource_hud.hpp"
#include "tutorial_hint.hpp"
#include "../prefabs/local_prefabs.hpp"
#include "../collision/local_collision.hpp"
#include "../props/local_props.hpp"
#include "../markers/local_decals.hpp"
#include "../terrain/terrain_ops.hpp"
#include "../world/navmesh_holes.hpp"
#include "../vegetation/local_vegetation.hpp"
#include "../testing/prefab_probe.hpp"
#include "../testing/terrain_probe.hpp"

#include <vector>

namespace kcds::game_client {

// The resources' Hud, native UI and props through the game's natives each frame; the test probe's
// Lua (KCDS_TEST_HOOKS only) in the engine's script system.
void run_hud_calls(bool in_world) {
  read_view_camera(in_world);
  // The script action key counts only with the game in front and no chat, page or menu typing.
  const bool keys_open = in_world && !ui_owns_input() && !page_over_world.load() &&
                         !game_menu_open() &&
                         GetForegroundWindow() == static_cast<HWND>(kcd2s::native_chat::window());
  // Nothing to loot or pick up while the player lies unconscious.
  kcds::runtime::action_hints::hide_world_hints(kcds::runtime::knockout::knocked_out() ||
                                                kcds::runtime::knockout::fainted());
  kcds::runtime::action_hints::frame(in_world, keys_open);
  kcds::runtime::world_map::frame(in_world);
  kcds::runtime::book::frame(in_world);
  kcds::runtime::native_ui::frame(in_world);
  kcds::runtime::character_creator::frame(in_world);
  kcds::runtime::hud::tutorial_hint::frame(in_world);
  engine_stage = "NoClip";
  kcds::runtime::noclip::frame(in_world, local::clock_seconds());
  engine_stage = "server rays and camera shots";
  run_server_rays_and_shots(in_world);
  // The client scripts' own rays, spheres and camera pose, after the view camera.
  kcds::runtime::physics::answer_local_world(in_world);
  kcds::runtime::vegetation::answer(in_world);
  // The use key on a server NPC.
  kcds::runtime::npcs::use::frame(in_world);
  // Off while the player lies unconscious, knocked out or in the exhaustion faint.
  kcds::runtime::camera::tick_photo_mode(
      in_world, local::clock_seconds(),
      kcds::runtime::knockout::knocked_out() || kcds::runtime::knockout::fainted());
  engine_stage = "resource HUD";
  std::optional<bool> combat_marker;
  std::optional<bool> needs;
  std::optional<bool> restricted_area_marker;
  std::optional<std::uint8_t> third_person;
  std::optional<bool> cuffs;
  std::optional<bool> frozen;
  std::optional<bool> lobby;
  std::vector<kcd2s::integration::PlayerAttach> player_attaches;
  std::optional<bool> noclip_allowed;
  std::vector<kcd2s::integration::MountOrder> mount_orders;
  bool dismount_order = false;
  bool rear_order = false;
  std::vector<kcd2s::integration::PullDownOrder> pull_downs;
  std::optional<std::uint16_t> interaction_rules;
  std::vector<std::uint32_t> whistles;
  std::optional<std::vector<kcd2s::integration::Interaction>> interactions;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (client) {
      combat_marker = client->peer().take_combat_marker();
      needs = client->peer().take_needs();
      restricted_area_marker = client->peer().take_restricted_area_marker();
      third_person = client->peer().take_third_person();
      cuffs = client->peer().take_cuffs();
      frozen = client->peer().take_frozen();
      lobby = client->peer().take_lobby();
      // The resource runtime hears it too: a server's loading page stays while the player is held.
      if (lobby) {
        using kcd2s::sdk::Value;
        client->send_to_resource_runtime(
            Value(Value::Map{{"op", Value(std::string("lobby"))}, {"held", Value(*lobby)}}));
      }
      player_attaches = client->peer().take_player_attaches();
      noclip_allowed = client->peer().take_noclip_allowed();
      mount_orders = client->peer().take_mount_orders();
      dismount_order = client->peer().take_dismount_order();
      rear_order = client->peer().take_rear_order();
      pull_downs = client->peer().take_pull_down_orders();
      interaction_rules = client->peer().take_interaction_rules();
      whistles = client->peer().take_whistles();
      // The local player whistled: the players near them hear it.
      if (kcds::runtime::riding::whistle::take_whistled()) {
        client->peer().send_whistle_report();
      }
      // The local player began a fast travel: the server decides whether it leads to another of
      // its maps.
      if (const auto target = kcds::runtime::world_map::take_travel_start()) {
        client->peer().send_travel_target((*target)[0], (*target)[1], (*target)[2]);
      }
      interactions = client->peer().take_interactions();
      // The world interactions the player used.
      for (const std::uint32_t id : kcds::runtime::interactions::take_fired()) {
        client->peer().send_interaction_fired(id);
      }
    }
  }
  if (interactions) {
    // The server's World.addInteraction list for the local player.
    kcds::runtime::interactions::set_list(std::move(*interactions));
  }
  // No world interaction for a player in a pillory or in cuffs, nor while a server conversation's
  // list shows: its use key is the list's pick (the groom's talk prompt took
  // the E of his own list).
  const bool can_interact = in_world && !ui_owns_input() && !page_over_world.load() &&
                            !kcds::runtime::pillory::in_stocks() &&
                            !kcds::runtime::input::cuffs::cuffed() &&
                            !kcds::runtime::dialogue::showing();
  kcds::runtime::interactions::frame(view_matrix_valid ? view_matrix.data() : nullptr,
                                     can_interact);
  if (third_person) {
    // The server's ThirdPerson.setMode.
    kcds::runtime::camera::third_person::set_mode(
        static_cast<kcds::runtime::camera::third_person::Mode>(*third_person));
  }
  // The server's whistles: the local player's own (Player.whistle) and nearby players'.
  kcds::runtime::riding::whistle::apply(whistles);
  if (interaction_rules) {
    // The game's own interactions the server allows (Interactions.set, Player.setInteraction).
    kcds::runtime::input::interaction_rules::set_allowed(*interaction_rules,
                                                         console_slot ? *console_slot : nullptr);
    const bool trespass = (*interaction_rules & kcd2s::integration::kAllowTrespass) != 0;
    kcds::runtime::hud::set_trespass_allowed(trespass);
    kcds::runtime::world::trespass::set_allowed(trespass);
    log("INTERACTIONS allowed 0x%x", unsigned(*interaction_rules));
  }
  // The server's player.mount.
  for (const auto& order : mount_orders) {
    if (in_world) {
      mount_ordered_horse(order.horse, order.instant);
    }
  }
  if (dismount_order && in_world) {
    dismount_ordered();
  }
  if (rear_order && in_world) {
    rear_ordered();
  }
  if (in_world) {
    for (const auto& order : pull_downs) {
      pull_down_ordered(order.attacker, order.rider);
    }
    tick_pull_down();
  }
  if (cuffs) {
    // The server's Player.setCuffed.
    kcds::runtime::input::cuffs::set_cuffed(*cuffs);
    log("CUFFS %s", *cuffs ? "on" : "off");
  }
  // The server's Player.attachTo.
  for (const auto& order : player_attaches) {
    kcds::runtime::attach::apply_order(order);
    log("ATTACH order player %u target %u flags %u", unsigned(order.player), unsigned(order.target),
        unsigned(order.flags));
  }
  if (frozen) {
    // The server's Player.frozen.
    kcds::runtime::input::frozen::set_frozen(*frozen);
    log("FROZEN %s", *frozen ? "on" : "off");
  }
  if (lobby) {
    // The server's Player.setLobby: the game's sound goes or comes back.
    kcds::runtime::lobby::set(*lobby);
  }
  // A sound the game starts while the player is held stays silent too.
  kcds::runtime::lobby::keep_muted();
  if (noclip_allowed) {
    // The server's Player.noclipAllowed.
    kcds::runtime::noclip::set_allowed(*noclip_allowed);
    log("NOCLIP %s by the server", *noclip_allowed ? "allowed" : "forbidden");
  }
  if (in_world) {
    DWORD foreground_pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
    // The game's screens and its crafting minigames are drawn for the first person.
    const bool game_screen = game_menu_open() || kcds::runtime::crafting::alchemy::own_running() ||
                             kcds::runtime::crafting::smithing::own_running() ||
                             kcds::runtime::crafting::grinding::own_running();
    static bool game_screen_logged = false;
    if (game_screen != game_screen_logged) {
      game_screen_logged = game_screen;
      log("CAMERA game screen %s (menu=%u)", game_screen ? "open" : "closed",
          unsigned(game_menu_open()));
    }
    kcds::runtime::camera::third_person::frame(foreground_pid == GetCurrentProcessId(),
                                               ui_owns_input() || page_over_world.load(),
                                               game_screen);
  }
  if (combat_marker) {
    // The server's CombatMarker.setEnabled.
    kcds::runtime::hud::set_combat_marker_shown(*combat_marker);
  }
  if (restricted_area_marker) {
    // The server's RestrictedAreaMarker.setEnabled.
    kcds::runtime::hud::set_restricted_area_marker_shown(*restricted_area_marker);
  }
  void* needs_actor = in_world ? local_combat_actor.load() : nullptr;
  void* needs_soul = nullptr;
  if (needs_actor) {
    needs_soul = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(needs_actor) + 1640);
  }
  bool body_awake = false;
  if (needs_soul) {
    const auto body =
        kcds::runtime::knockout::observe_body(needs_soul, needs_soul, native_soul_is_unconscious);
    body_awake = body == kcds::runtime::knockout::BodyState::Awake;
  }
  if (needs) {
    // The server's Needs.setEnabled.
    kcds::runtime::needs::set_enabled(*needs, body_awake);
  }
  if (kcds::runtime::needs::tick(in_world, body_awake)) {
    // The fill a knocked-out or fainted body missed when the needs went off, now that it is up:
    // exhaust and hunger (writable states 2 and 3, kVitalNames) to their maximum, as fillStat.
    constexpr std::uint8_t kExhaustState = 2;
    constexpr std::uint8_t kHungerState = 3;
    const std::vector<kcd2s::integration::StatRequest> fill = {
        {kExhaustState, kcd2s::integration::kMaxStatValue, false},
        {kHungerState, kcd2s::integration::kMaxStatValue, false}};
    kcds::runtime::stats::apply(needs_soul, fill, false, queue_scripted_health_reduction);
  }
  game_in_dialogue = in_world && read_game_dialogue();
  tick_dialogue(in_world);
  tick_vendors(in_world);
  kcds::runtime::hud::frame(in_world && hud_natives_found.load());
  std::vector<std::string> chunks;
  if (in_world) {
    kcds::runtime::props::frame();
    kcds::runtime::prefabs::local::frame(in_world);
    kcds::runtime::architect::level_plants::frame(in_world);
    kcds::runtime::architect::user_assets::frame(in_world);
    kcds::runtime::architect::level_properties::frame(in_world);
    kcds::runtime::architect::cursor_pick::frame(in_world);
    kcds::runtime::collision::local::frame(in_world);
    kcds::runtime::props::local::frame(in_world);
    kcds::runtime::terrain::ops::frame(in_world);
    kcds::runtime::world::navmesh_holes::frame(in_world);
    kcds::runtime::markers::local_decals::frame(in_world);
  }
  kcds::runtime::testing::prefab_probe::tick(in_world);
  kcds::runtime::testing::terrain_probe::tick(in_world);
  if (std::string probe = take_probe_lua(); !probe.empty()) {
    chunks.push_back(std::move(probe));
  }
  if (!in_world || !engine_scripts_verified.load() || !console_slot || chunks.empty()) {
    return;
  }
  if (!kcds::runtime::hud::run_lua(*console_slot, chunks)) {
    log("PROBE script system unavailable; %zu chunks dropped", chunks.size());
  }
}
}  // namespace kcds::game_client
