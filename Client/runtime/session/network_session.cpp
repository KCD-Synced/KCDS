// Winsock before Windows.h (see client_internal.hpp).
#include "../voice/voice_client.hpp"
#include "../core/client_internal.hpp"

#include "network_session.hpp"
#include "resource_cache.hpp"
#include "resource_files.hpp"

#include "../branding/watermark.hpp"
#include "../architect/ai_bridge.hpp"
#include "../architect/cursor_pick.hpp"
#include "../architect/editor_overlay.hpp"
#include "../architect/editor_pipe.hpp"
#include "../architect/level_doors.hpp"
#include "../architect/level_layers.hpp"
#include "../architect/level_plants.hpp"
#include "../architect/level_properties.hpp"
#include "../architect/level_sounds.hpp"
#include "../architect/navmesh_bake.hpp"
#include "../architect/user_assets.hpp"
#include "../architect/asset_files.hpp"
#include "../studio/studio_rope.hpp"
#include "../studio/studio_export.hpp"
#include "../studio/studio_hold.hpp"
#include "../studio/game_files.hpp"
#include "../studio/studio_game_clip.hpp"
#include "../pose_animation/player_animations.hpp"
#include "../studio/studio_mesh.hpp"
#include "../studio/studio_skin.hpp"
#include "../studio/studio_outfit.hpp"
#include "../studio/studio_pose.hpp"
#include "../studio/studio_prop_gizmo.hpp"
#include "../studio/studio_event.hpp"
#include "../studio/studio_floor.hpp"
#include "../studio/studio_grid.hpp"
#include "../studio/studio_saves.hpp"
#include "../studio/studio_texture.hpp"
#include "../studio/studio_sky.hpp"
#include "../studio/studio_skeleton.hpp"
#include "../studio/studio_whip.hpp"
#include "../architect/local_level_edits.hpp"
#include "../architect/local_mode.hpp"
#include "../architect/player_body.hpp"
#include "../architect/project_store.hpp"
#include "../architect/saves.hpp"
#include "../architect/world_look.hpp"

#include "../action_hints/resource_action_hints.hpp"
#include "../book/resource_book.hpp"
#include "../character_creator/creator_stage.hpp"
#include "../chat/resource_chat.hpp"
#include "../crash/crash_context.hpp"
#include "../camera/camera_shot.hpp"
#include "../vfx/local_effects.hpp"
#include "../lights/lights.hpp"
#include "../render/render_view.hpp"
#include "../chat/chat_overlay.hpp"
#include "../core/client_state.hpp"
#include "../core/log.hpp"
#include "../gizmos/gizmos.hpp"
#include "../audio/resource_audio.hpp"
#include "../combat/targetable.hpp"
#include "../seats/seat_collisions.hpp"
#include "../follow/follow.hpp"
#include "../physics/local_world.hpp"
#include "../props/local_previews.hpp"
#include "../prefabs/local_prefabs.hpp"
#include "../collision/local_collision.hpp"
#include "../props/local_props.hpp"
#include "../maps/custom_maps.hpp"
#include "../branding/editor_chrome.hpp"
#include "../terrain/terrain_ops.hpp"
#include "../world/navmesh_holes.hpp"
#include "../vegetation/local_vegetation.hpp"
#include "../discord/discord_presence.hpp"
#include "../identity/hardware_id.hpp"
#include "../identity/licence_pass.hpp"
#include "../identity/sha256.hpp"
#include "../identity/nickname.hpp"
#include "../nameplates/nameplates.hpp"
#include "../hud/level_up_notice.hpp"
#include "../hud/monologue.hpp"
#include "../hud/character_stats/character_stats.hpp"
#include "../hud/story_cutscenes.hpp"
#include "../riding/horse_inspect.hpp"
#include "../hud/resource_hud.hpp"
#include "../hud/tutorial_hint.hpp"
#include "../input/resource_input.hpp"
#include "../keybinds/keybinds.hpp"
#include "../inventory/inventory_use.hpp"
#include "../inventory/item_types.hpp"
#include "../markers/blips.hpp"
#include "../markers/local_decals.hpp"
#include "../native_ui/native_ui.hpp"
#include "../noclip/free_camera.hpp"
#include "../npcs/local_npcs.hpp"
#include "../vendors/vendor_screen.hpp"
#include "../web/resource_web.hpp"
#include "../weapon_hits/weapon_hits_tick.hpp"
#include "../world_map/resource_world_map.hpp"
#include "MinHook.h"
#include "server_address.hpp"
#include "server_browser.hpp"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <ctime>

namespace kcds::game_client {

namespace native_chat = kcd2s::native_chat;

// Without KCDS_SERVER_ADDRESS the player picks the server in the in-game browser first.
bool browser_mode = false;
// What the modules tell a new resource runtime by: one for each client and each runtime it
// started. A runtime started anew gets what the modules sent the old one again.
const void* runtime_session(const local::Client* current) {
  struct Session {
    const local::Client* client;
    std::uint32_t runtime;
  };
  static std::unique_ptr<Session> session;
  const std::uint32_t runtime = current->resource_runtime_generation();
  if (!session || session->client != current || session->runtime != runtime) {
    // Made while the old one lives, so its address is another.
    auto made = std::make_unique<Session>(Session{current, runtime});
    session = std::move(made);
  }
  return session.get();
}

// Hands the web views' and keys' reports to the current client's resource runtime
// (network_mutex held).
void pump_web_views() {
  local::Client* current = client.get();
  if (!current) {
    return;
  }
  const void* session = runtime_session(current);
  auto send = [current](const kcd2s::sdk::Value& message) {
    return current->send_to_resource_runtime(message);
  };
  kcds::runtime::web::pump(session, send);
  kcds::runtime::input::Gate gate;
  gate.window = native_chat::window();
  // In World Architect's local mode there is no server avatar: the local world is the world.
  const bool own = current->own_avatar() != core::kInvalidEntity || current->offline();
  const bool ready = current->offline() ? kcds::runtime::architect::local_mode::world_ready()
                                        : current->peer().world_ready();
  gate.in_world = ready && own;
  const bool chat_editing = native_chat::editing();
  const bool page_focused = kcds::runtime::web::has_input_focus();
  const bool page_over = page_over_world.load();
  const bool menu_open = game_menu_open();
  gate.captured = chat_editing || page_focused || page_over || menu_open;
  // Why the resources' key binds stop or start again, once for each change (a player flying with
  // NoClip got none, a server team said).
  static int last_gate = -1;
  const int now_gate = (gate.in_world ? 1 : 0) | (chat_editing ? 2 : 0) | (page_focused ? 4 : 0) |
                       (page_over ? 8 : 0) | (menu_open ? 16 : 0);
  if (now_gate != last_gate) {
    last_gate = now_gate;
    log("KEYS gate in_world=%d chat=%d page_focus=%d page_over=%d menu=%d", gate.in_world ? 1 : 0,
        chat_editing ? 1 : 0, page_focused ? 1 : 0, page_over ? 1 : 0, menu_open ? 1 : 0);
  }
  gate.pointer = kcds::runtime::web::has_input_focus() || kcds::runtime::native_ui::has_focus();
  kcds::runtime::input::pump(session, send, gate);
  kcds::runtime::keybinds::pump(session, send);
  kcds::runtime::voice::pump(send);
  kcds::runtime::chat::pump(session, send);
  kcds::runtime::action_hints::pump(session, send);
  kcds::runtime::world_map::pump(session, send);
  kcds::runtime::book::pump(session, send);
  kcds::runtime::vendors::pump(send);
  kcds::runtime::native_ui::pump(session, send);
  kcds::runtime::character_creator::pump(session, send);
  kcds::runtime::hud::tutorial_hint::pump(session, send);
  kcds::runtime::discord::pump(session, send);
  kcds::runtime::audio::pump(send);
  kcds::runtime::follow::pump(send);
  kcds::runtime::physics::pump_local_world(send);
  kcds::runtime::vegetation::pump(send);
  kcds::runtime::prefabs::local::pump(send);
  kcds::runtime::architect::level_plants::pump(send);
  kcds::runtime::architect::level_layers::pump(send);
  kcds::runtime::architect::user_assets::pump(send);
  kcds::runtime::architect::asset_files::pump(send);
  kcds::runtime::architect::level_properties::pump(send);
  kcds::runtime::architect::saves::pump(send);
  kcds::runtime::architect::ai_bridge::pump(send);
  kcds::runtime::props::local::pump(send);
  kcds::runtime::architect::level_doors::pump(send);
  kcds::runtime::architect::level_sounds::pump(send);
  kcds::runtime::architect::navmesh_bake::pump(send);
  kcds::runtime::npcs::local::pump(send);
  kcds::runtime::architect::cursor_pick::pump(send);
  pump_weapon_hits(send);
  kcds::runtime::props::local_previews::pump(send);
  kcds::runtime::noclip::pump(session, send);
  kcds::runtime::camera::pump(send);
  kcds::runtime::render::pump(send);
  kcds::runtime::lights::pump(send);
  kcds::runtime::inventory::pump_types(session, send);
  kcds::runtime::inventory::pump_uses(send);
  kcds::runtime::hud::monologue::pump(session, send);
  kcds::runtime::hud::xp_notices_session(session);
  kcds::runtime::hud::story_cutscenes::pump(session, send);
  kcds::runtime::hud::pump(session, send, gate.in_world && hud_natives_found.load(),
                           game_in_dialogue.load());
}
DWORD WINAPI network_worker(void*) {
  // With the browser, the browser thread creates the client and runs its handshake.
  while (!client_ready.load()) {
    Sleep(20);
  }
  network_worker_running.store(true);
  // A failing tick is logged (the first, then every 500th in a row) and the worker goes on: it
  // used to stop for good, so that session and every later join lost its server after 10 s
  // until the game restarted.
  std::uint64_t failures_in_a_row = 0;
  for (;;) {
    try {
      std::lock_guard<std::mutex> lock(network_mutex);
      // The worker outlives a session: after the player left a server, a cancelled or refused
      // join closes the next client while no world runs.
      if (client) {
        client->tick(local::clock_seconds());
        pump_web_views();
      }
      failures_in_a_row = 0;
    } catch (const std::exception& e) {
      if (failures_in_a_row % 500 == 0) {
        log("Network worker tick failed (%llu in a row): %s",
            static_cast<unsigned long long>(failures_in_a_row + 1), e.what());
      }
      failures_in_a_row += 1;
    }
    Sleep(10);
  }
}
// Connects the KSTP client to the chosen server and starts its resource runtime.
bool open_client(const std::string& address, const std::string& listing,
                 const std::string& password, bool kcd1) {
  std::string server_ip;
  std::uint16_t server_port = 0;
  if (!resolve_server_address(address, server_ip, server_port)) {
    return false;
  }
  log("Game server %s:%u", server_ip.c_str(), unsigned(server_port));
  // The watermark's server line: a server on this computer is "localhost", without a
  // ping; another one's name comes once the world is ready.
  // Test hook: KCDS_TEST_WATERMARK_REMOTE shows a kit server on this computer as another one's.
  const bool test_remote =
      std::getenv("KCDS_TEST_HOOKS") && std::getenv("KCDS_TEST_WATERMARK_REMOTE");
  kcds::runtime::branding::watermark::set_server(L"",
                                                 server_ip.rfind("127.", 0) == 0 && !test_remote);
  // The player's key bindings of this server's resource actions.
  kcds::runtime::keybinds::set_server(
      listing.empty() ? server_ip + ":" + std::to_string(server_port) : listing);
  try {
    auto created = std::make_unique<local::Client>(server_ip.c_str(), server_port);
    // Obtain the platform proof only after the server supplies its handshake nonce.
    created->set_identity(kcds::runtime::identity::local_nickname(),
                          kcds::runtime::identity::steam_id(), kcds::runtime::discord::user_id(),
                          kcds::runtime::identity::hardware_id());
    // The launcher's join token goes only to the server the launcher joins
    // directly, never to one picked in the browser.
    const std::string join_token = launcher_join_token();
    if (!join_token.empty() && address == launcher_server_address()) {
      created->set_join_token(join_token);
      log("Join token handed to %s", address.c_str());
    }
    // Every server, localhost included, lets in only a legally owned game.
    created->set_licence_provider([master = master_url(),
                                   endpoint = server_ip + ":" + std::to_string(server_port),
                                   listing, kcd1](std::uint64_t nonce) {
      std::int64_t expires = 0;
      kcds::runtime::identity::LicenceRefusal refusal;
      auto pass = kcds::runtime::identity::licence_pass(master, endpoint, listing,
                                                        kcd2s::transport::licence_binding(nonce),
                                                        log, &expires, &refusal, kcd1);
      const auto refresh = expires > 0 ? double(expires - std::time(nullptr) - 10) : 180.0;
      return local::Client::LicenceProof{std::move(pass), refresh, std::move(refusal.text),
                                         refusal.banned};
    });
    if (!password.empty()) {
      created->set_password_proof([password](std::uint64_t nonce) {
        return kcds::runtime::identity::password_proof(kcd2s::transport::licence_binding(nonce),
                                                       password);
      });
    }
    // Resource files this machine downloaded before, by their SHA-256.
    created->set_download_cache(std::make_unique<kcd2s::local::resources::DownloadCache>(
        kcds::runtime::session::resource_cache_folder(),
        [](const std::string& bytes) { return kcds::runtime::identity::sha256_hex(bytes); },
        [](const std::filesystem::path& file) {
          return kcds::runtime::identity::sha256_file_hex(file);
        }));
    // Its files over HTTP on the game's port, when its catalog offers them.
    created->set_http_files(kcds::runtime::session::make_resource_files(
        server_ip, server_port, kcds::runtime::session::resource_cache_folder()));
    // The cached files this server's game may hand to the native UI, maps and pages: its own.
    created->set_catalog_listener([](const std::set<std::string>& hashes) {
      kcds::runtime::session::set_catalog_hashes(hashes);
    });
    created->set_resource_logger(
        [](const std::string& text) { log("RESOURCE client %s", text.c_str()); });
    // Resource web views (Web global): their calls stay on this machine.
    kcds::runtime::web::start(module_directory(), log);
    created->set_voice_datagram_handler(&kcds::runtime::voice::on_datagram);
    kcds::runtime::voice::session_started([](const std::uint8_t* data, std::size_t size) {
      // The engine's capture thread: `client` lives until voice is told the session ended.
      return client && client->send_voice_datagram(data, size);
    });
    created->set_local_runtime_handler([](const kcd2s::sdk::Value& message) {
      // The last command a client script sent, for a crash report (crash/crash_context.hpp).
      std::string resource;
      if (message.tag() == kcd2s::sdk::Tag::Map) {
        for (const auto& entry : message.as_map()) {
          if (entry.first == "resource" && entry.second.tag() == kcd2s::sdk::Tag::Str) {
            resource = entry.second.as_str();
          }
        }
      }
      kcds::runtime::crash::note_script(kcd2s::integration::script_operation(message), resource);
      return kcds::runtime::web::handle_runtime_message(message) ||
             kcds::runtime::voice::handle_runtime_message(message) ||
             kcds::runtime::input::handle_runtime_message(message) ||
             kcds::runtime::keybinds::handle_runtime_message(message) ||
             kcds::runtime::hud::handle_runtime_message(message) ||
             kcds::runtime::chat::handle_runtime_message(message) ||
             kcds::runtime::action_hints::handle_runtime_message(message) ||
             kcds::runtime::world_map::handle_runtime_message(message) ||
             kcds::runtime::book::handle_runtime_message(message) ||
             kcds::runtime::native_ui::handle_runtime_message(message) ||
             kcds::runtime::character_creator::handle_runtime_message(message) ||
             kcds::runtime::noclip::handle_runtime_message(message) ||
             kcds::runtime::camera::handle_runtime_message(message) ||
             kcds::runtime::vfx::local::handle_runtime_message(message) ||
             kcds::runtime::lights::handle_runtime_message(message) ||
             kcds::runtime::render::handle_runtime_message(message) ||
             kcds::runtime::npcs::local::handle_runtime_message(message) ||
             kcds::runtime::gizmos::handle_runtime_message(message) ||
             kcds::runtime::nameplates::handle_runtime_message(message) ||
             kcds::runtime::discord::handle_runtime_message(message) ||
             kcds::runtime::audio::handle_runtime_message(message) ||
             kcds::runtime::follow::handle_runtime_message(message) ||
             kcds::runtime::combat::targetable::handle_runtime_message(message) ||
             kcds::runtime::seats::collisions::handle_runtime_message(message) ||
             kcds::runtime::physics::handle_local_world_message(message) ||
             kcds::runtime::props::local_previews::handle_runtime_message(message) ||
             kcds::runtime::prefabs::local::handle_runtime_message(message) ||
             kcds::runtime::collision::local::handle_runtime_message(message) ||
             kcds::runtime::props::local::handle_runtime_message(message) ||
             kcds::runtime::maps::custom::handle_runtime_message(message) ||
             kcds::runtime::branding::editor_chrome::handle_runtime_message(message) ||
             kcds::runtime::architect::level_edits::handle_runtime_message(message) ||
             kcds::runtime::architect::level_doors::handle_runtime_message(message) ||
             kcds::runtime::architect::level_sounds::handle_runtime_message(message) ||
             kcds::runtime::architect::navmesh_bake::handle_runtime_message(message) ||
             kcds::runtime::architect::overlay::handle_runtime_message(message) ||
             kcds::runtime::architect::editor_pipe::handle_runtime_message(message) ||
             kcds::runtime::studio::rope::handle_runtime_message(message) ||
             kcds::runtime::studio::whip::handle_runtime_message(message) ||
             kcds::runtime::studio::exporter::handle_runtime_message(message) ||
             kcds::runtime::studio::skeleton::handle_runtime_message(message) ||
             kcds::runtime::studio::pose::handle_runtime_message(message) ||
             kcds::runtime::studio::outfit::handle_runtime_message(message) ||
             kcds::runtime::studio::sky::handle_runtime_message(message) ||
             kcds::runtime::studio::mesh::handle_runtime_message(message) ||
             kcds::runtime::studio::skin::handle_runtime_message(message) ||
             kcds::runtime::studio::game_clip::handle_runtime_message(message) ||
             kcds::runtime::pose_animation::players::handle_runtime_message(message) ||
             kcds::runtime::studio::game_files::handle_runtime_message(message) ||
             kcds::runtime::studio::hold::handle_runtime_message(message) ||
             kcds::runtime::studio::prop_gizmo::handle_runtime_message(message) ||
             kcds::runtime::studio::textures::handle_runtime_message(message) ||
             kcds::runtime::studio::saves::handle_runtime_message(message) ||
             kcds::runtime::studio::events::handle_runtime_message(message) ||
             kcds::runtime::studio::floor::handle_runtime_message(message) ||
             kcds::runtime::studio::grid::handle_runtime_message(message) ||
             kcds::runtime::architect::projects::handle_runtime_message(message) ||
             kcds::runtime::architect::world_look::handle_runtime_message(message) ||
             kcds::runtime::architect::local_mode::handle_runtime_message(message) ||
             kcds::runtime::architect::level_plants::handle_runtime_message(message) ||
             kcds::runtime::architect::level_layers::handle_runtime_message(message) ||
             kcds::runtime::architect::user_assets::handle_runtime_message(message) ||
             kcds::runtime::architect::asset_files::handle_runtime_message(message) ||
             kcds::runtime::architect::level_properties::handle_runtime_message(message) ||
             kcds::runtime::architect::saves::handle_runtime_message(message) ||
             kcds::runtime::architect::ai_bridge::handle_runtime_message(message) ||
             kcds::runtime::architect::cursor_pick::handle_runtime_message(message) ||
             kcds::runtime::architect::player_body::handle_runtime_message(message) ||
             kcds::runtime::terrain::ops::handle_runtime_message(message) ||
             kcds::runtime::world::navmesh_holes::handle_runtime_message(message) ||
             kcds::runtime::vegetation::handle_runtime_message(message) ||
             kcds::runtime::markers::handle_runtime_message(message) ||
             kcds::runtime::markers::local_decals::handle_runtime_message(message) ||
             kcds::runtime::inventory::handle_use_message(message) ||
             kcds::runtime::hud::monologue::handle_runtime_message(message) ||
             kcds::runtime::hud::handle_xp_notices_message(message) ||
             kcds::runtime::hud::story_cutscenes::handle_runtime_message(message) ||
             kcds::runtime::hud::character_stats::handle_runtime_message(message) ||
             kcds::runtime::riding::horse_inspect::handle_runtime_message(message) ||
             handle_game_message(message);
    });
    created->start_resource_runtime(module_directory() + L"kcd2s-resource-runtime.exe");
    std::lock_guard<std::mutex> lock(network_mutex);
    client = std::move(created);
    ++network_client_generation;
  } catch (const std::exception& e) {
    log("Network setup failed: %s", e.what());
    return false;
  }
  return true;
}

DWORD open_network() {
  try {
    winsock = std::make_unique<local::Winsock>();
    network_ready.store(true);
    if (kcds::runtime::architect::local_mode::enabled()) {
      // World Architect's local mode: the resource runtime runs, nothing connects.
      if (!open_client("127.0.0.1:9")) {
        return 1;
      }
      {
        std::lock_guard<std::mutex> lock(network_mutex);
        client->set_offline();
      }
      client_ready.store(true);
    } else if (!browser_mode) {
      // Without a launcher address and without the browser: the local development server.
      const std::string address = launcher_server_address();
      if (!open_client(address.empty() ? "127.0.0.1:7777" : address)) {
        return 1;
      }
      client_ready.store(true);
    }
  } catch (const std::exception& e) {
    log("Network setup failed: %s", e.what());
    return 1;
  }
  return 0;
}

DWORD start_network_worker() {
  auto worker = CreateThread(nullptr, 0, network_worker, nullptr, 0, nullptr);
  if (!worker) {
    MH_DisableHook(update_target);
    log("Network worker creation failed");
    // Not the game's fault: 2, not 1 (see initialize in game_client.cpp).
    return 2;
  }
  CloseHandle(worker);
  log("Framework update hook and network worker installed; %s",
      browser_mode ? "server browser shown" : "connecting to the launcher's server");
  return 0;
}

}  // namespace kcds::game_client
