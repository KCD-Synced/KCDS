#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kcd2s/integration/animation_props.hpp"
#include "kcd2s/integration/player_animation.hpp"

// The server's Player.playAnimation and stopAnimation on the local player's body,
// played by the game's Mannequin the way its own flow node "Actor:PlayMannequinFragment" does
// the fragment's id from the controller's fragment definitions,
// its tags (separated by '+') as the fragment's own tags or else as global tags on the
// controller's context, a TAction<SAnimationContext> (144 bytes) queued on the
// player's action controller with no time limit. The client watches the action's status (+ 40:
// 1 pending, 2 installed, 4 finished, 0 none once the controller let it go) to tell how it ended:
// played to its end (finished), let go after it was installed (interrupted) or before (refused).
// A loop is queued again when it ends and reports only a refusal. A stop ends the action at once
// and clears the global tags the request set; a stand-up plays the game's StandUp or
// GetUp in the stance's variant. `lockMovement` enables the game's action filter "no_move" on the
// action map manager with a token of the client's own, as the game's dice game does:
// walking, jumping and sprinting stop, looking around does not; it stays until the
// request is stopped or replaced.
//
// A `clip` of the game's own animations is played as the game's scripts play a
// clip by name (the entity binding's StartAnimation): its id in the body's animation set
// (IAnimationSet slot 6 GetAnimIDByName: the set names a clip by its file's name without
// the extension; slot 10 GetFilePathByID, tells that it is the path's file), started on
// the skeleton (ISkeletonAnim slot 9) on a layer of its own above the body's (kClipLayer), looped
// or holding its last key. The owner's animations replicate as before, so every other client plays
// it on the replica too. A one-shot that played to its end (the game's whole-clip progress) blends
// out, and the body's own animation shows again; a stop or a new request blends it out too. The
// server raises no event for a request without a fragment, so a clip reports nothing.
//
// A clip a resource ships (the order's clip "@<resource>/<path>") is added to the body's animation
// set first (see kShippedClipRoot in the .cpp): its id there is this client's own, so the client's
// animation sync leaves its start out (starting_shipped_clip()), the client publishes it by name in
// the field "animationProps" while it plays (shipped_clip()), and every other client plays it on
// the replica from there (keep_replica_clip()).
//
// The request's props stay with it (props()); the client publishes them in the NetworkPlayer
// field "animationProps" and every client shows them (animation_props.hpp).
//
// Engine thread only.
namespace kcds::runtime::player_animation {

struct Natives {
  void* allocate = nullptr;        // void*(size_t), the game's allocator
  void* construct = nullptr;       // TAction ctor: memory, priority, fragment id,
                                   // tag state, flags, scope mask, user token
  void* assign = nullptr;          // _smart_ptr<IAction> assignment: slot, action
  void* stop = nullptr;            // ends an action and its children at once
  void* find_tag = nullptr;        // id of a name in a tag definition, -1 for none
  void* set_tag = nullptr;         // sets a tag in a tag state
  void* set_global_tag = nullptr;  // sets a tag in the context's tag state
  // The game's services: the framework at + 8, its action map manager by vtable
  // + 224. Without it `lockMovement` holds nobody still.
  void* services = nullptr;
  // For clips: the local player's engine entity (its character: entity slot 101), and how far an
  // animation of a skeleton is through its clip, 0 to 1 (the game's whole-clip progress). Without
  // them a clip is not played.
  void* (*local_entity)() = nullptr;
  float (*progress)(void* skeleton, const void* animation) = nullptr;
  void (*log)(const char* format, ...) = nullptr;
  // Whether the local player sits on a seat (holds a smart object, seats/seat_reservations.hpp):
  // a fragment then plays within the seat.
  bool (*seated)() = nullptr;
  // For clips a resource ships ("@<resource>/<path>"): the animation set's CAF loader
  // (CAnimationSet::LoadFileCAF), the name hash it keys clips by, the int
  // of the cvar ca_UseIMG_CAF it reads, and native_ui::mount_shipped. Without them such a clip is
  // refused.
  void* load_file_caf = nullptr;
  void* name_hash = nullptr;
  int* use_img_caf = nullptr;
  bool (*mount_shipped)(const std::string& resource, const std::string& path,
                        const std::string& root, const std::string& entry) = nullptr;
  // For a paired request's slave: an entity's place (its world matrix's turn,
  // quaternion x y z w, and position), and a move of it as the replication moves a body. Without
  // them the slave stays at the master's place.
  bool (*body_place)(void* entity, float q[4], float t[3]) = nullptr;
  void (*move_body)(void* entity, const float q[4], const float t[3]) = nullptr;
};

// False when a function is missing: the orders are then refused.
bool install(const Natives& natives);

// Sets or clears a global Mannequin tag by name (such as "sitting") on a body's animation context;
// false when the body has no context or its tag definition has no such tag (issue 016: an NPC
// replica sits as its simulator's body does).
bool hold_global_tag(void* controller, const char* name, bool on);

// Takes a server order for the player whose action controller this is. Ends the client reports
// (finished, interrupted or refused) go into `ends`.
void take(void* controller, const kcd2s::integration::AnimationOrder& order,
          std::vector<kcd2s::integration::AnimationEnd>& ends);

// Watches the running request once a frame: its end, a loop's next pass.
void frame(void* controller, std::vector<kcd2s::integration::AnimationEnd>& ends);

// The player's body or world went away: forgets the request without reporting it.
void reset();

// Npc.playAnimation with a fragment: an NPC this client simulates (body: the NPC's
// id) plays the fragment through its own action controller, queued as the player's request is,
// with its tags (the fragment's own on the action, the others on the controller's context until
// the request is stopped or replaced). `request` names it in the log. False when the game would
// not take it (no such fragment). A new request on the body replaces the one it plays.
bool play_on_body(std::uint32_t body, void* controller, const std::string& fragment,
                  const std::string& tags, bool loop, std::uint64_t request);

// Once a frame while the body plays it: how its request ended (finished, interrupted, refused),
// or none while it plays. A loop is queued again and reports only a refusal.
std::optional<kcd2s::integration::AnimationReason> watch_body(std::uint32_t body, void* controller);

// Stops the body's request at once and clears its tags (the body is still there).
void stop_on_body(std::uint32_t body);

// The body went away: forgets its request without touching it.
void forget_body(std::uint32_t body);

// An NPC's facial expression on the simulator's body, beside what the body plays: "" for the game's
// own face. False when the body has no such fragment.
bool face_on_body(std::uint32_t body, void* controller, const std::string& fragment,
                  const std::string& tags);

// The props of the request the body holds, from its start until it is stopped or replaced (also
// after its fragment ended); none without one.
const std::vector<kcd2s::integration::AnimationProp>& props();
// The running request's id, 0 for none.
std::uint64_t running_request();

// How the last request that ended went in the game's action (its status, flags and time), for
// the client's log.
const char* end_detail();

// The shipped clip the body plays now, for the field; none while it plays none (a one-shot that
// played to its end, a stop, a game clip).
std::optional<kcd2s::integration::ShippedClip> shipped_clip();

// True while the body starts a shipped clip: the animation sync must not send its id.
bool starting_shipped_clip();

// The skeleton layer script clips play on (the cuffed pose among them).
inline constexpr unsigned kScriptClipLayer = 12;

// Whether the running request plays only on the upper body (playAnimation upperBody): its
// clip on kScriptClipLayer, the legs kept from the walk.
bool upper_body_running();

// Whether the running request keeps the body without gravity (playAnimation gravity: false).
bool no_gravity_running();

// Whether the running request keeps the body's heading, with only the head and camera turning
// (playAnimation lockRotation: true).
bool lock_rotation_running();

// The running lockRotation request's look limits in degrees (playAnimation lockRotation
// { yaw, up, down }): yaw to each side of the body's heading, up and down from level. A yaw
// of 0 means none were given: the free look's own limits apply.
struct LookLimits {
  double yaw = 0;
  double up = 0;
  double down = 0;
};
LookLimits look_limits();

// The player the running request is paired with (playPairedAnimation) while its fragment
// plays, 0 for none: the two bodies do not push each other meanwhile.
std::uint32_t running_partner();

// A replica (holder: its owner's network id, body: its engine entity) took a new clip from its
// owner on its base layer. A paired request's slave places its body by its master's clip there:
// the game's paired clips start at places of their own around one origin
// (animation/paired_start.hpp); the slave's client moves its body from the master's place, where
// the server put it, to where its clip starts relative to the master's.
void replica_base_clip(std::uint32_t holder, void* body, int clip);

// A request of the client's own (pointing): played like the server's, with no id, so it
// reports no end. play_local replaces what runs and is true when its clip started; stop_local
// ends it. local_request_running: whether the running request is such a one.
bool play_local(void* controller, const kcd2s::integration::AnimationOrder& order);
void stop_local(void* controller);
bool local_request_running();

// A clip of the game's own on an entity's body, straight on its skeleton (ISkeletonAnim slot 9,
// as the scripts' StartAnimation), with no request (no fragment, tags, movement lock or report):
// by its name in the body's set or by its whole path ("Animations/....caf", added to the set from
// the game's files the first time), on `layer`, looped or holding its last key. False when the
// body or the clip is not there. The studio plays cutscene clips with it (studio_game_clip.hpp).
// `from`: where in the clip it starts (0 to 1, its key time; -1 its start). It blends in over
// 0.2 s from what the layer played, so a clip started again from a place crossfades there.
bool start_body_clip(void* entity, const std::string& clip, bool loop, unsigned layer,
                     float from = -1.0f);

// Blends out what plays on the body's `layer`.
void stop_body_clip(void* entity, unsigned layer);

// Where the newest clip on the body's `layer` is, 0 at its start to 1 at its end; -1 for none.
float body_clip_time(void* entity, unsigned layer);

// The id of the newest clip on the body's `layer`; -1 for none.
int body_layer_clip(void* entity, unsigned layer);

// Whether a clip id was taken by a game path (Animations/....caf: a cutscene's motion).
bool is_game_clip(int clip);

// A clip's whole length in seconds in the body's animation set (by name or game path, as
// start_body_clip takes it; IAnimationSet GetDuration_sec); 0 when it has none.
float body_clip_seconds(void* entity, const std::string& clip);

// Puts the newest clip on the body's `layer` at `time` (0 to 1: SetLayerNormalizedTime, slot 29).
void set_body_clip_time(void* entity, unsigned layer, float time);

// Whether the clip id (in the animation set of this entity's body) is the cuffed pose
// (Player.setCuffed's hands_tied_back_idle).
bool is_cuff_pose_clip(void* entity, int clip);

// A replica's body (holder: its owner's network id, body: its engine entity) plays the shipped
// clip its owner's field names, on the clip layer; none blends out the one it played. The caller
// lets the start through on a replica's synced layer.
void keep_replica_clip(std::uint32_t holder, void* body,
                       const std::optional<kcd2s::integration::ShippedClip>& clip);

// The replica is gone or plays no shipped clip any more.
void forget_replica_clip(std::uint32_t holder);

// Player.setSpeaking: the style the local body's mouth moves in, for the field
// "animationProps" ("" while it does not). The face clips have ids past what the clip sync carries,
// so every other client loops the style's clip by name on the replica (keep_replica_talking).
std::string talking_style();

// While the player is in one of the game's dialogues the talking loop steps aside, so the game's
// own lip-sync speaks for the body on every screen, and comes back after (engine thread).
void pause_talking(bool paused);

// A replica's body loops the face clip of its owner's talking style on the talking layer, kept
// there once a call; "" blends it out. The caller lets the start through on a replica's synced
// layer.
void keep_replica_talking(std::uint32_t holder, void* body, const std::string& style);

// The replica is gone.
void forget_replica_talking(std::uint32_t holder);

// The local request's speed mark for the field "animationProps": the speed it plays at
// now, its layer and how far through its clip that layer is ("0.50:0:0.42"); "" while it plays as
// authored from its start, or without a request.
std::string speed_mark();

// A replica's body follows its owner's speed mark ("" for none: its layers play as authored again);
// owner_clip is the clip the owner's field says the mark's layer plays, for moving the replica's
// clip to the owner's place.
void keep_replica_speed(std::uint32_t holder, void* body, const std::string& mark, int owner_clip);

// The replica is gone.
void forget_replica_speed(std::uint32_t holder);

}  // namespace kcds::runtime::player_animation
