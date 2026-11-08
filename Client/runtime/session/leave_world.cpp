#include "leave_world.hpp"
#include "../voice/voice_client.hpp"

#include "../appearance/worn_only.hpp"
#include "../chat/chat_overlay.hpp"
#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../gizmos/gizmos.hpp"
#include "../lockpicking/lockpicking.hpp"
#include "../markers/blips.hpp"
#include "../npcs/native_npcs.hpp"
#include "../lobby/lobby.hpp"
#include "../vendors/vendor_screen.hpp"
#include "../web/resource_web.hpp"
#include "kcds/menu_native/main_menu.hpp"
#include "kcds/web/web.hpp"

namespace kcds::game_client {

namespace native_chat = kcd2s::native_chat;

// The game UI's place in the services (the loading screen's owner).
constexpr std::size_t kServicesUiOffset = 0xE8;
// ISystem::Quit's reason: a string and its length.
struct NativeQuitReason {
  const char* text;
  std::uint64_t length;
};
using NativeSystemQuit = void(__fastcall*)(void*, const NativeQuitReason*, std::uint64_t);
void quit_if_requested() {
  // The request stays pending until the native exit is resolved.
  if (!native_env || !quit_requested.exchange(false)) {
    return;
  }
  static const char kReason[] = "Quit requested by the KCDS server browser";
  const NativeQuitReason reason{kReason, sizeof(kReason) - 1};
  void* system = *reinterpret_cast<void**>(native_env() + 0x170);
  if (!system) {
    log("MENU quit: engine system unavailable");
    return;
  }
  log("MENU quit through ISystem::Quit");
  slot<NativeSystemQuit>(system, 29)(system, &reason, 0);
}
// Leaves the server's world for the main menu on the engine thread: our replicas and session go
// first, so nothing of ours is touched while the game unloads the level.
void leave_world_if_requested() {
  if (!leave_requested.load()) {
    return;
  }
  // A world the game is still starting is left once it stands: going to the main menu in the
  // middle of the debug start left the game refusing the next start (NewGameDebug accepted=0),
  // and the player waited for the realm forever.
  if (world_requested.load() && !multiplayer_world_ready.load() && !failed) {
    return;
  }
  // The lock's screen goes only with its minigame: it ends first, as when the player lets go.
  if (kcds::runtime::lockpicking::ending_for_leave()) {
    return;
  }
  leave_requested.store(false);
  if (!native_loading_screen || !native_go_to_main_menu || !get_services) {
    log("MENU leave: native main-menu path unavailable");
    return;
  }
  // Our replicas and horses are forgotten here but not removed: the level unload removes every
  // entity and lets the game drop its own references first. Removing them ourselves in the same
  // frame left a game UI item context with a dead actor, and the main-menu transition crashed
  // reading it.
  // A vendor's screen ends with the connection.
  kcds::runtime::vendors::reset(true);
  // Player.wearItem's pieces leave the inventory with the server that asked for them.
  kcds::runtime::appearance::worn_only::reset(local_combat_actor.load());
  // The bodies stay for the unload, but their walks end now: a path search left queued ran on in
  // the next level and crashed the game there (the Animation Studio after a server).
  {
    void* framework = framework_slot ? *framework_slot : nullptr;
    void* actors = framework
                       ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + 1304)
                       : nullptr;
    stop_native_npc_walks(
        actors ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actors) + 48) : nullptr);
  }
  reset_world_replicas(nullptr);
  kcds::runtime::gizmos::level("");
  kcds::runtime::markers::forget_own_blips();
  requested_revision = 0;
  multiplayer_world.store(false);
  world_requested.store(false);
  loading_down_since_request.store(false);
  client_ready.store(false);
  native_chat::set_ready(false);
  {
    // Disconnect only: the network worker and the game's hooks keep a client object, and the next
    // join replaces it (open_client), which also stops its resource runtime.
    std::lock_guard<std::mutex> lock(network_mutex);
    if (client) {
      kcds::runtime::voice::session_ended();
      client->disconnect();
    }
  }
  // A server's pages end here: its resource runtime lives on until the next join.
  kcds::web::destroy_resource_views();
  kcds::runtime::web::forget_loading_page();
  // A player held out of the world gets the game's sound back.
  kcds::runtime::lobby::set(false);
  kcds::menu_native::reset_main_menu_built();
  void* services = get_services();
  void* ui =
      services ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(services) + kServicesUiOffset)
               : nullptr;
  void* loading = ui ? native_loading_screen(ui) : nullptr;
  if (!loading) {
    log("MENU leave: loading screen unavailable");
    return;
  }
  log("MENU leave: world left, loading the main menu");
  native_go_to_main_menu(loading);
  world_left.store(true);
}

}  // namespace kcds::game_client
