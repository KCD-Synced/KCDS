// The client's shared state: the game's globals and natives found at startup, the connection to
// the server and the session's flags (split out of game_client.cpp). Declared in
// client_internal.hpp and client_state.hpp.
#include "client_internal.hpp"

#include "client_state.hpp"

namespace kcds::game_client {

void** framework_slot;
// C_LockBase::SetLocked and the lock of an entity id, resolved once with
// the pillory's natives: the pillory hooks SetLocked, after which its signature no longer matches.
void* lock_set_locked = nullptr;
void* lock_of_entity_id = nullptr;
// An entity's WUID as the game's GetMyWUID binding gets it: the WUID map lookup and the
// map's global, resolved once with the pillory's natives (a vendor's keeper).
void* wuid_lookup_native = nullptr;
void* wuid_map_native = nullptr;
ServicesGetter get_services;
ParamsInit params_init;
StringAssign string_assign;
void** console_slot;

void* update_target;
std::unique_ptr<local::Winsock> winsock;
std::unique_ptr<local::Client> client;
// Set once `client` exists; with the server browser that is after the player joined a server.
std::atomic<bool> client_ready{false};
// Set by frame() when the native world request was accepted: the browser's join is complete.
std::atomic<bool> world_requested{false};
// The browser page is shown over the running world (Disconnected popup).
std::atomic<bool> page_over_world{false};
// The engine's script system was checked against this build (hud/engine_script.hpp).
std::atomic<bool> engine_scripts_verified{false};
// The resource Hud's natives were found (hud/resource_hud.hpp).
std::atomic<bool> hud_natives_found{false};
// The 3D engine global (I3DEngine*), read from the script binding's SetScreenFx.
void* const* engine_3d_slot = nullptr;
std::atomic<bool> view_camera_verified{false};
// The dialogue manager's lookup of a soul's conversation, as the Lua human binding's IsInDialog
// calls it: (manager, soul id, true, true), null when there is none.
DialogueLookup dialogue_lookup = nullptr;
// Whether the local player's soul is in one of the game's conversations; engine thread writes.
std::atomic<bool> game_in_dialogue{false};
// Quit through the game's own path, on the engine thread. C_UISaveLoad::ExitGame
// ends in ISystem::Quit: system = (env getter)()+0x170, vtable +0xE8, with a reason
// string and its length; Lua's System.Quit makes the same call.
NativeEnvGetter native_env = nullptr;
std::atomic<bool> quit_requested{false};
// When the player chose to quit. The engine-thread path exists only once the adapter finished its
// setup (about 10 s after the main menu); if it never comes, the process ends after this delay.
std::atomic<ULONGLONG> quit_requested_at{0};
// Leaving a server's world back to the main menu, without saving and without a restart. The
// pause menu's "Save and exit to main menu" saves first; when the save is done its message handler
// calls GoToMainMenu on the loading screen. The game's failed-load
// path makes the same two calls without saving:
//   loading = GetLoadingScreen(*(services + 0xE8)); GoToMainMenu(loading)
// GoToMainMenu switches the loading screen to state 1 (180AEE5C), which loads the
// main-menu level ("main_menu_trosecko") and so unloads the world.
GetLoadingScreen native_loading_screen = nullptr;
GoToMainMenu native_go_to_main_menu = nullptr;
// Set by the browser thread; served on the engine thread by leave_world_if_requested().
std::atomic<bool> leave_requested{false};
// Set by the engine thread once the world is left; the browser thread shows the list again.
std::atomic<bool> world_left{false};
// Set once the network worker pumps the client. Until then the browser thread pumps it: the worker
// starts only after the game's hooks are installed, 10-20 s after the first join, and a client
// that nobody pumps neither hears the server nor answers it (the server drops it as silent).
std::atomic<bool> network_worker_running{false};
std::mutex network_mutex;
std::uint64_t network_client_generation = 0;  // network_mutex; changes only for a new client
std::uint32_t requested_revision;
RevealNativeFow native_reveal_fow = nullptr;
DiscoverNativeMap native_discover_map = nullptr;
std::uint32_t revealed_map_revision = 0;

std::uint32_t load_counter_before;
bool failed = false;
const char* engine_stage = "idle";
std::atomic<bool> multiplayer_world{false};
// True only after the server world finished loading (WORLD READY) until the next world request.
std::atomic<bool> multiplayer_world_ready{false};
// The game's own loading screen keeps its "loading" action map enabled while it is shown
// (C_UILoadingScreen state change) and disables it when it goes away: the moment the
// player can play (playerReady). The enable happens before the adapter's hooks exist, so only the
// disable after this world's request is observed (0.8-2.5 s after world-ready).
std::atomic<bool> loading_down_since_request{false};

}  // namespace kcds::game_client
