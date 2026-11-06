// What the features split out of game_client.cpp still use from it: its shared state
// and helpers, declared here until each moves into core/ or into the feature it belongs to. This
// header shrinks with every step of the split and goes when the split is done.
#pragma once

// Winsock before Windows.h (the network headers need its newer socket types), and no min/max
// macros (the shared headers use std::min and std::max).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "../../../Backend/integration/local/runtime.hpp"
#include "../knockout/knockout.hpp"
#include "kcd2s/integration/combat.hpp"

namespace kcds::game_client {

namespace core = kcd2s::core;
namespace local = kcd2s::local;
namespace wire = kcd2s::wire;

// The replicated entities as each frame copies them from the client's mirror.
using MirrorEntities =
    std::remove_cvref_t<decltype(std::declval<const kcd2s::replication::Mirror&>().entities())>;

// The connection to the server and its lock (the network worker pumps it).
extern std::unique_ptr<local::Client> client;
extern std::mutex network_mutex;
// Set once `client` exists (after the player joined a server), and creates it for an address.
extern std::atomic<bool> client_ready;
// The KCDS Master's address (KCDS_MASTER_URL, or the public Master); "" when it is off.
std::string master_url();
// `listing`: the server's id on the Master when the player chose it in the browser (the
// licence pass names it); looked up by address otherwise.
// `password`: the server's password the player typed, or empty; only a proof of it
// is sent.
bool open_client(const std::string& address, const std::string& listing = {},
                 const std::string& password = {});
// Set by frame() when the native world request was accepted: the browser's join is complete.
extern std::atomic<bool> world_requested;
// The browser page is shown over the running world (Disconnected popup).
extern std::atomic<bool> page_over_world;
// Leaving the world back to the main menu: asked by the browser thread, done by the engine thread.
extern std::atomic<bool> leave_requested;
extern std::atomic<bool> world_left;
// The network worker runs; until then the browser thread pumps the web views itself.
extern std::atomic<bool> network_worker_running;
void pump_web_views();
// The local player is in a server's world (the game's own single-player systems step aside).
extern std::atomic<bool> multiplayer_world;
// The adapter stopped after an exception.
extern bool failed;

// Quitting: the game's native environment getter (its exit path), the player's quit and when they
// chose it; the process ends after kQuitFallbackMs if the engine thread never serves it.
using NativeEnvGetter = std::uint8_t*(__fastcall*)();
extern NativeEnvGetter native_env;
extern std::atomic<bool> quit_requested;
extern std::atomic<ULONGLONG> quit_requested_at;
inline constexpr ULONGLONG kQuitFallbackMs = 20000;
// The learn-perk test hook's event (KCDS_TEST_HOOKS only).
extern HANDLE learn_perk_event;
// The test hooks' page message event, and whether an event is set (without waiting).
extern HANDLE page_message_event;
bool signaled(HANDLE event);
// The local player's actor as combat last saw it.
extern std::atomic<void*> local_combat_actor;
// Until when mounting is traced in the log.
extern std::atomic<ULONGLONG> mount_trace_until;
// What the engine adapter does now, for the crash log.
extern const char* engine_stage;
// The server's world revision this client asked for.
extern std::uint32_t requested_revision;
// The local player's network id, and the engine id of the horse they ride (0 none).
extern std::atomic<core::EntityId> local_network_id;
// player.keyBypass of the local player.
extern std::atomic<bool> local_key_bypass;
extern std::atomic<unsigned> local_ridden_horse_engine;
// Set while the client itself plays an animation on a replica (its hooks let it through).
extern thread_local bool replaying_animation;

// The game's services (gEnv-like) getter and the console's global.
using ServicesGetter = void*(__fastcall*)();
extern ServicesGetter get_services;
extern void** console_slot;
// The local player's engine id.
extern std::atomic<std::uint32_t> local_engine_id;
// When the local player last asked to lock on to an NPC (NPC_ENGAGE).
extern std::atomic<ULONGLONG> npc_lock_asked_ms;
// Melee: the game's queued native hit (hooked: the original), a hit's damage queued for the
// server's ruling, and set while the client replays a melee hit itself.
using QueueNativeHit = std::intptr_t(__fastcall*)(void*, const void*);
extern QueueNativeHit original_queue_native_hit;
void queue_hit_damage(const std::uint8_t* hit);
// Combat: ends the local player's fight with an opponent, drops the hits held for the server's
// ruling (all, or one opponent's), and sends the local player's damage report.
void cancel_combat_with(void* local_actor, std::uint32_t opponent);
void discard_held_hits(const char* reason, core::EntityId opponent = core::kInvalidEntity);
void send_player_damage();
extern thread_local bool replaying_melee;

// The game framework's global.
extern void** framework_slot;
// C_LockBase::SetLocked and the lock of an entity id, resolved with the pillory's natives.
extern void* lock_set_locked;
extern void* lock_of_entity_id;
// The WUID map lookup and the map's global, resolved with the pillory's natives.
extern void* wuid_lookup_native;
extern void* wuid_map_native;

// Knock-out: whether a soul is unconscious, the native wake of a replica, whether a soul is a
// replica's, and the hooked push of an actor's ragdoll action (with its original).
extern kcds::runtime::knockout::IsUnconscious native_soul_is_unconscious;
using RequestReplicaWake = void(__fastcall*)(void*);
extern RequestReplicaWake native_request_replica_wake;
bool is_replica_soul(void* soul);
using PushRagdollAction = char(__fastcall*)(void* expansion, void* unused, void* kind);
extern PushRagdollAction original_push_ragdoll_action;
char __fastcall hooked_push_ragdoll_action(void* expansion, void* unused, void* kind);

// A game or overlay UI owns the keyboard and mouse now, and the game's own menu is open.
bool ui_owns_input();
bool game_menu_open();

// HUD: the engine's script system checked against this build, the Hud natives found, and the
// game's dialogue state as read this frame.
extern std::atomic<bool> engine_scripts_verified;
extern std::atomic<bool> hud_natives_found;
extern std::atomic<bool> game_in_dialogue;
bool read_game_dialogue();
void read_view_camera(bool in_world);
void run_server_rays_and_shots(bool in_world);
void tick_dialogue(bool in_world);
// Test hooks (KCDS_TEST_HOOKS only): the probe's Lua and its chat line.
std::string take_probe_lua();
std::string take_test_chat_line();
// A scripted health reduction of the local player, queued for the server's ruling.
void queue_scripted_health_reduction(float amount);
// The local player sits on a horse.
extern std::atomic<bool> local_rider_seated;
// IsOnLadder: actor vtable + 1288, as the script binding asks.
inline constexpr std::size_t kActorIsOnLadder = 1288 / 8;
// The local player's base-layer clip while its game holds it by hand (the ladder's climb clip).
bool local_held_climb(int& clip, float& progress);
// Runs the server's player commands (setStat and the like) on the local player's soul.
void run_player_commands();
// The network id of the replica whose combat movement this is (kInvalidEntity for none), and the
// local player's combat control state logged at a stage of input handling.
core::EntityId remote_combat_id(void* movement);
void log_native_control_state(const char* stage, void* movement);
// The singleton CActionMapManager, captured from its own EnableActionMap calls.
extern std::atomic<void*> action_map_manager;
// The replicas' C_Player vtable copy (its RTTI locator at [0], the entries from + 1): C_Player's
// own, with the remote lifecycle entries taken from C_Actor (no singleton UI/input registration).
extern void* remote_player_table[290];

// The server's world finished loading (WORLD READY), and the game's loading screen went away
// after this world's request.
extern std::atomic<bool> multiplayer_world_ready;
extern std::atomic<bool> loading_down_since_request;
// Drops this world's replicas and per-world state.
void reset_world_replicas(void* entity_system);
// The game's loading screen of its UI and its GoToMainMenu (the way back to the main menu).
using GetLoadingScreen = void*(__fastcall*)(void* ui);
using GoToMainMenu = void(__fastcall*)(void* loading_screen);
extern GetLoadingScreen native_loading_screen;
extern GoToMainMenu native_go_to_main_menu;
// The dialogue manager's lookup of a soul's conversation (as IsInDialog calls it).
using DialogueLookup = void*(__fastcall*)(void*, std::uint64_t, bool, bool);
extern DialogueLookup dialogue_lookup;
// GetViewCamera checked against this build.
extern std::atomic<bool> view_camera_verified;
// Where each remote player who is on a ladder is, by network id (movement/shared_ladders.hpp).
extern std::unordered_map<core::EntityId, std::array<float, 3>> ladder_climbers;

// The client object's generation (network_mutex; changes only for a new client).
extern std::uint64_t network_client_generation;
// The local player's soul the held hits and damage reports belong to.
extern void* pending_combat_soul;
// Drops the local player's damage reports not yet sent; queues the health of the killing blow.
void discard_player_damage(const char* reason);
void queue_killing_tick();
// Reports the local player's missiles the server already committed, before a knock-out.
void report_committed_missiles_before_knockout();

// Combat: sends a combat action of the local fighter to the others, the native action a
// movement runs now, and opens the window in which a replica's reaction is its game's own.
void relay_combat_action(const kcd2s::integration::CombatActionData& data);
unsigned native_action_type(void* movement);
void open_reaction_window(core::EntityId player);
// Set while a replica's relayed attack or defence runs: the replica and its opponent.
extern thread_local void* replicated_attack_actor;
extern thread_local void* replicated_opponent_actor;

// The replicas' combat souls and movements by their owners (under combat_identity_mutex).
extern std::mutex combat_identity_mutex;
extern std::unordered_map<void*, core::EntityId> remote_combat_souls;
extern std::unordered_map<void*, void*> remote_combat_movements;
// Every clip start is logged until then (diagnostics).
extern std::atomic<ULONGLONG> clip_trace_until;
// A trampled replica's fall plays on this layer; the skeletons falling now (trample_fall_mutex).
inline constexpr unsigned kTrampleFallLayer = 8;
extern std::mutex trample_fall_mutex;
extern std::unordered_set<void*> trample_fall_skeletons;
// The engine update thread, the local player's view wrapper (its NPC lookup at slot 4), and a
// replica's native wake request.
extern std::atomic<DWORD> engine_update_thread;
extern void* local_view_wrapper;
void request_replica_native_wake(void* actor, void* state);
// A replica's human overlay released (replicas: the overlay code still in game_client.cpp).
struct Remote;
void release_remote_human_overlay(Remote& remote);
// The last attack a replica started here (its owner's replayed press): who and when; when the
// local fighter's attack started; a skeleton layer's playback scale.
extern core::EntityId remote_attack_source;
extern ULONGLONG remote_attack_started;
extern ULONGLONG local_attack_started;
float& layer_playback_scale(void* skeleton, unsigned layer);
// How long after a replica's attack started a defender's block still answers it, in ms.
inline constexpr ULONGLONG kAttackAnswerMs = 1200;
// The native action of a fighter who attacks.
inline constexpr unsigned kNativeAttackAction = 3;
// Combat model int properties of the combat core (their values at + 8): the guard zone, and the
// one the pair-row query reads for the defender; the game sets it to 24 when a
// paired attack begins on a fighter.
inline constexpr std::size_t kCoreGuard = 240;
inline constexpr std::size_t kCoreMode = 96;
// A movement's clinch component (C_CombatActorClinch).
inline constexpr std::size_t kMovementClinch = 1008;
// The native combat input (IActionListener::OnAction: self, ActionId&, mode, value; hooked: the
// original), whether a replica's input is replayed now, the replica soul whose manual intent the
// replayed input is, and the movement whose hit reaction runs now (its queued queries are logged).
using CombatInput = void(__fastcall*)(void*, const void*, int, float);
extern CombatInput original_combat_input;
// The game's scripted combat damage (hooked: the original).
using CombatDamage = void(__fastcall*)(void*, const void*);
extern CombatDamage original_combat_damage;
extern thread_local bool replaying_combat_input;
extern thread_local void* remote_manual_combat_soul;
extern thread_local void* observed_reaction_movement;
// Whom each replica's owner has its combat lock on (key: the replica's Soul): that player's network
// id (the local player's or another remote's), 0 for nobody. Protected by combat_identity_mutex.
extern std::unordered_map<void*, core::EntityId> remote_native_targets;
// Whether a movement's native attack runs now.
bool native_attack_active(void* movement);
// Whether a native action kind is a paired one, and whether a movement is in a pair's alignment.
bool paired_action(unsigned kind);
bool native_pair_alignment(void* movement);
// The player whose replica dealt a hit (the hit's source entity id at + 0), 0 for anyone else.
std::uint32_t hit_attacker(const std::uint8_t* hit);
// The local player's skeleton, and a clip's normalized time as the game reads it.
extern std::atomic<void*> local_skeleton;
using AnimationProgress = float(__fastcall*)(void*, const void*);
extern AnimationProgress native_animation_progress;

// The local player's input and view, kept through a replica's creation and removal.
void retain_local_input(void* local_actor);
void retain_local_view();

// An entity of the entity system by id, or null.
void* get_entity(void* system, std::uint32_t id);
// The turn of a 3x4 world matrix.
core::Quat rotation(const float* m);
// Moves an entity (position and turn), as the replication does.
// A body's Animations field decoded, kept until it changes (replicas/replica_state.cpp).
const std::optional<local::AnimationState>& decoded_animation(core::EntityId id,
                                                              const std::string& field,
                                                              bool weapon = false);
void forget_decoded_animation(core::EntityId id);
void set_transform(void* entity, const core::Vec3& p, const core::Quat& q, unsigned reason = 0);
// WHGame 1.5.5 script SetWorldPos passes this SetWorldTM reason; see apply_forced_transforms.
inline constexpr unsigned kScriptSetWorldPosReason = 0x400000u;
// The view camera's world matrix (3x4, rows; the position in the last column, the view along
// its second column) as read this frame.
extern std::array<float, 12> view_matrix;
extern bool view_matrix_valid;
// Its vertical field of view in radians (the CCamera's, after the matrix).
extern float view_fov;

}  // namespace kcds::game_client
