#include "player_animation.hpp"

#include "paired_start.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace kcds::runtime::player_animation {
namespace paired_start = kcds::runtime::animation::paired_start;
namespace {

using kcd2s::integration::AnimationEnd;
using kcd2s::integration::AnimationExit;
using kcd2s::integration::AnimationOrder;
using kcd2s::integration::AnimationReason;

using Allocate = void*(__fastcall*)(std::size_t);
using Construct = void*(__fastcall*)(void*, int, int, const void*, int, int, int);
using Assign = void*(__fastcall*)(void**, void*);
using Stop = void(__fastcall*)(void*);
using FindTag = int(__fastcall*)(const void*, const char*);
using SetTag = void(__fastcall*)(const void*, void*, int, bool);
using SetGlobalTag = void(__fastcall*)(void*, int, bool);
using Getter = void*(__fastcall*)(void*);
using Services = void*(__fastcall*)();
using FragmentTags = void*(__fastcall*)(void*, int);
using Queue = void(__fastcall*)(void*, void*, float);
using Fail = void(__fastcall*)(void*, int);
using Exit = void(__fastcall*)(void*);
using Interrupt = bool(__fastcall*)(void*);

constexpr std::size_t kActionBytes = 144;
// The action's status (+ 40) and flags (+ 44); FragmentIsOneShot is set while a one-shot plays.
constexpr std::size_t kActionStatus = 40;
constexpr std::size_t kActionFlags = 44;
constexpr unsigned kFlagFragmentIsOneShot = 0x1000;
constexpr int kStatusNone = 0;
constexpr int kStatusInstalled = 2;
constexpr int kStatusExiting = 3;
constexpr int kStatusFinished = 4;
// The action controller's vtable: a fragment's tag definition, Queue, the animation context.
constexpr std::size_t kFragmentTagsSlot = 18;
constexpr std::size_t kQueueSlot = 19;
constexpr std::size_t kContextSlot = 23;
// The context: its controller definition (the fragment ids at + 24) and its tag state (+ 8: the
// tag definition, then the state).
constexpr std::size_t kDefinitionFragments = 24;
constexpr std::size_t kContextTagState = 8;
constexpr std::size_t kTagStateBytes = 20;
// The flow node's own: no time limit.
constexpr float kNoTimeLimit = -1.0f;
// The action's priority among the body's actions. Live, 0 to 1000 made no difference
// to the game's gestures (Pointing, ADLG_Laugh played to their end, PrayStand looped).
constexpr int kPriority = 3;
// On a seated player the seat's own action holds the body and an action of
// kPriority waited behind it for good (status 1); one of this priority plays
// within the seat (Guest_DrawTankard installed, played its 4 s and ended, the player still seated).
constexpr int kSeatedPriority = 100;
// TAction<SAnimationContext>'s vtable: 29 slots after its locator; Fail (3), Exit (4)
// and Interrupt (8) tell how an action ended.
constexpr std::size_t kActionSlots = 29;
constexpr std::size_t kFailSlot = 3;
constexpr std::size_t kExitSlot = 4;
constexpr std::size_t kInterruptSlot = 8;

// The action map manager: the framework (services + 8) gives it (vtable + 224); it enables
// (+ 240: name, token) and disables (+ 248: name, token, all tokens) an action filter. The
// filter counts per token; the client's own token, so no other enable or disable of the game's
// takes ours away (except one that clears every token).
constexpr std::size_t kServicesFramework = 8;
constexpr std::size_t kActionMapManagerSlot = 28;
constexpr std::size_t kEnableFilterSlot = 30;
constexpr std::size_t kDisableFilterSlot = 31;
constexpr char kNoMoveFilter[] = "no_move";
constexpr std::uint8_t kFilterToken = 0xA5;
using EnableFilter = void(__fastcall*)(void*, const char*, std::uint8_t);
using DisableFilter = void(__fastcall*)(void*, const char*, std::uint8_t, bool);
// Whether a filter is on for any token (+ 264).
constexpr std::size_t kFilterEnabledSlot = 33;
using FilterEnabled = bool(__fastcall*)(void*, const char*);

// A clip: the body's character (entity slot 101, character slot 0), its ISkeletonAnim (character
// slot 5) and animation set (slot 16), as props/prop_entity.cpp plays clips on a prop.
constexpr std::size_t kGetCharacterSlot = 101;
constexpr int kCharacterSlot = 0;
constexpr std::size_t kSkeletonAnimSlot = 5;
constexpr std::size_t kAnimationSetSlot = 16;
// IAnimationSet: GetAnimIDByName and GetFilePathByID.
constexpr std::size_t kAnimIdByNameSlot = 6;
constexpr std::size_t kFilePathByIdSlot = 10;
// IAnimationSet GetDuration_sec(id) (its "illegal animation index used in function
// GetDuration_sec" string): a clip's length in seconds.
constexpr std::size_t kDurationByIdSlot = 13;
// ISkeletonAnim: StartAnimationById, StopAnimationInLayer, how many animations a layer holds and
// one of them (the newest last), whose clip id is at + 16.
constexpr std::size_t kStartAnimationSlot = 9;
constexpr std::size_t kStopLayerSlot = 10;
constexpr std::size_t kLayerCountSlot = 15;
constexpr std::size_t kLayerAnimationSlot = 18;
constexpr std::size_t kAnimationClip = 16;
// The layer of a script clip: above the layers the game was seen to give the body (0, 4, 6, the
// facial overlay 11, 14 and 15 in the kit's logs), so it covers the body's own
// animation for the joints it moves, and the body's own shows again once it ends.
constexpr std::uint32_t kClipLayer = 12;
constexpr float kClipBlendIn = 0.2f;
constexpr float kClipBlendOut = 0.25f;
// The game refuses a clip while the same clip fades out on the layer (a script playing it again at
// once): the order tries again for this long (s), which covers the fade.
constexpr float kClipRetrySeconds = 1.0f;
// CryCharAnimationParams flags: loop, or hold the last key.
constexpr std::uint32_t kClipLoop = 2;
constexpr std::uint32_t kClipHoldLastKey = 4;
// A one-shot is at its end from this far through its clip.
constexpr float kClipEnd = 0.999f;

// A request's speed. A fragment's is its action's speed bias (+ 92, as the game's own
// debug text "SpeedBias=%.2f" reads it): the controller plays the fragment that many
// times as fast, its clips and its timeline alike, and a bias of 0 holds it where it is. A clip's
// is its layer's playback scale (ISkeletonAnim::SetLayerPlaybackScale, slot 23; a
// layer's scale at + 80 + 64 * layer). A start time is reached by playing up to kSkipSpeed times
// as fast until the request has played that long.
constexpr std::size_t kActionSpeedBias = 92;
constexpr std::size_t kLayerScaleSlot = 23;
constexpr std::size_t kLayerScales = 80;
constexpr std::size_t kLayerStride = 64;
constexpr float kSkipSpeed = 20.0f;
// A replica follows its owner's speed mark: its layers play at the mark's speed, and
// its clip on the mark's layer is moved to the owner's place
// (ISkeletonAnim::SetLayerNormalizedTime, slot 29, as the entity binding's SetAnimationTime)
// when it is kReplicaDrift of its clip off, or kReplicaHeldDrift while the request is
// held still. A fragment's speed reaches every layer of the body's but the face (11), the script
// clip (12) and the talking (13); a clip's only its layer.
constexpr std::size_t kSetLayerTimeSlot = 29;
constexpr float kReplicaDrift = 0.08f;
constexpr float kReplicaHeldDrift = 0.01f;
constexpr std::uint32_t kFragmentLayers = 0xFFFFu & ~((1u << 11) | (1u << 12) | (1u << 13));
constexpr float kMarkSteps = 50.0f;
// The longest frame the start's skip counts (s): a stall does not jump the request ahead.
constexpr float kLongestFrame = 0.25f;

// Player.setSpeaking: the mouth moves by a loop of one of the game's own face clips,
// on a layer of its own above the script clip's (none of the game's own in the kit's logs),
// so a body's script clip and its talking go together. The face clips are the
// dialogue lines' lip-sync animations, named by the line's voice file; every body's animation set
// has them all (a male body finds female speakers' lines and the female face set's own
// clips), and the face rig is shared, so one clip per style serves every body.
constexpr std::uint32_t kTalkLayer = 13;
struct TalkClip {
  const char* style;
  const char* clip;
};
constexpr TalkClip kTalkClips[] = {
    {"neutral", "tmck_fris_henry_vzdyt_ja_eh_js_4yuL"},  // a long line of Henry's
    {"happy", "tmck_spec_henry_jenze_to_vesel_MgcP"},    // Henry, cheerful
    {"drunk", "drus_opil_hynek_suc_jen_jdi_RqsO"},       // the drunk Hynek
    {"chew", "fa_chew_01"},                              // the dialogue gesture ADLG_FA_Chew's clip
};

// A clip a resource ships (an order's clip "@<resource>/<path>"). The runtime sent its
// file (native_ui, kept in memory); the client puts it in the game's file system as
// Animations/kcds/<name>.caf, a pack of its own (native_ui::mount_shipped), and adds it to the
// body's animation set under <name> (shipped_clip_name) with the set's own CAF loader, the way a
// chrparams list adds the game's clips (CAnimationSet::LoadFileCAF: set, the path's
// hash, the name's hash, path, name; the hashes are's, lower case). That loader reads the
// cvar ca_UseIMG_CAF, which the game sets to 1: it then looks a file up only in the
// prebuilt table of the game's own clips by its path's hash, finds none for a new file and goes
// on with id -1. Held at 0 for that one call, the loader makes the clip's header from the file
// as a non-IMG build does; a file it knows already keeps its header either way. A set
// belongs to the body's model, so every body of that model on this client has the clip from then
// on; each client adds it to its own sets, at an id of its own (the field carries it by name).
constexpr const char* kShippedClipRoot = "Animations/kcds/";
using LoadFileCaf = int(__fastcall*)(void* set, std::uint64_t path_hash, std::uint64_t name_hash,
                                     const char* path, const char* name);
using NameHash = std::uint64_t(__fastcall*)(const char* text, unsigned length);

// The animation parameters as the entity binding's StartAnimation fills them.
struct ClipParams {
  float transition;
  float key_time;
  float speed;
  float weight;
  std::uint32_t layer;
  float layer_weight;
  std::uint32_t flags;
  std::uint32_t user_token;
  float user_data[8];
};
static_assert(sizeof(ClipParams) == 64);

using GetCharacter = void*(__fastcall*)(void*, int);
using AnimIdByName = int(__fastcall*)(void*, const char*);
using FilePathById = const char*(__fastcall*)(void*, int);
using DurationById = float(__fastcall*)(void*, int);
using StartAnimation = bool(__fastcall*)(void*, int, const ClipParams*);
using StopLayer = bool(__fastcall*)(void*, std::uint32_t, float);
using SetLayerScale = void(__fastcall*)(void*, std::uint32_t, float);
using SetLayerTime = void(__fastcall*)(void*, std::uint32_t, float);
using LayerCount = unsigned(__fastcall*)(void*, std::uint32_t);
using LayerAnimation = const void*(__fastcall*)(void*, std::uint32_t, unsigned);

Natives natives;
bool ready = false;
bool movement_locked = false;

// The client's own copy of the action's vtable, its locator first, with Fail, Exit and Interrupt
// wrapped.
std::array<void*, kActionSlots + 1> action_vtable{};
void* const* game_action_vtable = nullptr;

// How the controller let one of our actions go, while the client still watches it.
struct Watched {
  void* action = nullptr;
  bool failed = false;
  bool exited = false;
  bool interrupted = false;
  // The action's status, flags and time played when Interrupt came (+ 40, + 44, + 16).
  int interrupt_status = -1;
  unsigned interrupt_flags = 0;
  float interrupt_time = 0;
};
// How the last ended request's action went, for the client's log.
char end_detail_text[160] = {};
std::vector<Watched> watched;

Watched* watch_of(void* action) {
  for (auto& entry : watched) {
    if (entry.action == action) {
      return &entry;
    }
  }
  return nullptr;
}

void __fastcall on_fail(void* action, int failure) {
  if (Watched* entry = watch_of(action)) {
    entry->failed = true;
  }
  reinterpret_cast<Fail>(game_action_vtable[kFailSlot])(action, failure);
}

void __fastcall on_exit(void* action) {
  if (Watched* entry = watch_of(action)) {
    entry->exited = true;
  }
  reinterpret_cast<Exit>(game_action_vtable[kExitSlot])(action);
}

bool __fastcall on_interrupt(void* action) {
  if (Watched* entry = watch_of(action)) {
    entry->interrupted = true;
    const auto* bytes = static_cast<const std::uint8_t*>(action);
    std::memcpy(&entry->interrupt_status, bytes + 40, sizeof(int));
    std::memcpy(&entry->interrupt_flags, bytes + 44, sizeof(unsigned));
    std::memcpy(&entry->interrupt_time, bytes + 16, sizeof(float));
  }
  return reinterpret_cast<Interrupt>(game_action_vtable[kInterruptSlot])(action);
}

// A request on the body: its global tags stay until it is stopped or replaced.
struct Running {
  AnimationOrder order;  // for a stand-up: its fragment and the stance's tags
  AnimationExit exit = AnimationExit::None;
  void* controller = nullptr;
  void* action = nullptr;  // referenced while it runs
  bool installed = false;
  int logged_status = -1;  // the action's status last logged
  std::vector<int> global_tags;
  // Of them, the sitting stance's (a tag named sitting...): a seated player's seat holds the same
  // tag, so they stay when the request ends while the player sits.
  std::vector<int> stance_tags;
  int clip = -1;         // the clip's id in the body's animation set while it plays on kClipLayer
  bool shipped = false;  // the clip is one a resource ships
  float clip_retry = 0.0f;  // how much longer a clip the game refused is tried again (s)
  // A loop's next pass, queued behind the playing one (referenced): the controller goes from pass
  // to pass without the idle between them, whose blend swung a resting prop (the item slot).
  void* next = nullptr;
  // The speed set last frame and how long the request has played, in its own time.
  float applied_speed = 1.0f;
  float played = 0.0f;
  // A paired request's slave: the body's place when the order came (the master's,
  // where the server put it), when that was, and whether the body went to its clip's start yet.
  paired_start::Place start;
  std::chrono::steady_clock::time_point started;
  bool aligned = false;
  // When the action was installed, and how many clips the local body had started by then.
  std::chrono::steady_clock::time_point installed_at;
  std::uint64_t clips_at_install = 0;
};
std::optional<Running> running;
// The clips the local body started, on any layer but the look's (note_local_clip_start).
std::atomic<std::uint64_t> local_clip_starts{0};
// A fragment's action installed this long without starting a clip plays nothing: the body stood in
// its bind pose, arms out, until the request was stopped.
constexpr auto kNoClipGrace = std::chrono::milliseconds(600);
// The face's expression (Player.setExpression): an action of its own on the
// face's scope, beside the running request, held until the next expression or "".
std::optional<Running> face;
// The NPCs' expressions on the bodies simulated here (face_on_body), by their network ids.
std::unordered_map<std::uint32_t, Running> body_faces;
// While the local body starts a shipped clip: the client's animation sync leaves that start out.
bool starting_shipped = false;

// A shipped clip a replica plays because its owner's field names it.
struct ReplicaClip {
  void* body = nullptr;
  kcd2s::integration::ShippedClip clip;
  void* skeleton = nullptr;  // null when it did not start
  int id = -1;
};
std::unordered_map<std::uint32_t, ReplicaClip> replica_clips;

// A paired request's slave goes to its clip's start once its own clip and its
// master's (the newest the master's replica took on its base layer since the order came: the
// master's clip goes through its own client and the server first) both have a start location; it
// tries for kAlignWindow, then stays at the master's place.
constexpr std::uint32_t kBaseLayer = 0;
constexpr auto kAlignWindow = std::chrono::milliseconds(1500);
// How far apart (m, on the ground) two halves of one pair can start.
constexpr float kPairReach = 4.0f;
struct PartnerClip {
  void* body = nullptr;
  int clip = -1;
  std::chrono::steady_clock::time_point at;
};
// Each replica's newest clip on its base layer, by its owner's network id.
std::unordered_map<std::uint32_t, PartnerClip> base_clips;

// A replica that plays at its owner's request's speed: the layers it scaled.
struct ReplicaSpeed {
  void* skeleton = nullptr;
  std::uint32_t layers = 0;
};
std::unordered_map<std::uint32_t, ReplicaSpeed> replica_speeds;

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

template <class T>
T field(const void* object, std::size_t offset) {
  T value;
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(T));
  return value;
}

void* action_map_manager() {
  if (!natives.services) {
    return nullptr;
  }
  void* services = reinterpret_cast<Services>(natives.services)();
  void* framework = services ? field<void*>(services, kServicesFramework) : nullptr;
  return framework ? slot<Getter>(framework, kActionMapManagerSlot)(framework) : nullptr;
}

// Holds the local player still (or lets them go) by the "no_move" filter, once.
void lock_movement(bool lock) {
  if (lock == movement_locked) {
    return;
  }
  void* manager = action_map_manager();
  if (!manager) {
    if (natives.log) {
      natives.log("ANIMATION movement lock unavailable: no action map manager");
    }
    return;
  }
  if (lock) {
    slot<EnableFilter>(manager, kEnableFilterSlot)(manager, kNoMoveFilter, kFilterToken);
  } else {
    slot<DisableFilter>(manager, kDisableFilterSlot)(manager, kNoMoveFilter, kFilterToken, false);
  }
  movement_locked = lock;
  if (natives.log) {
    const bool enabled = slot<FilterEnabled>(manager, kFilterEnabledSlot)(manager, kNoMoveFilter);
    natives.log("ANIMATION movement %s (no_move filter %s, manager %p)", lock ? "locked" : "free",
                enabled ? "on" : "off", manager);
  }
}

void* context_of(void* controller) {
  return controller ? slot<Getter>(controller, kContextSlot)(controller) : nullptr;
}

std::vector<std::string> tag_names(const std::string& tags) {
  std::vector<std::string> names;
  std::size_t begin = 0;
  while (begin <= tags.size()) {
    const std::size_t end = std::min(tags.find('+', begin), tags.size());
    if (end > begin) {
      names.push_back(tags.substr(begin, end - begin));
    }
    begin = end + 1;
  }
  return names;
}

// The fragment's own tag definition, or null when it has none.
const void* fragment_tags_of(void* controller, int fragment) {
  return fragment >= 0 ? slot<FragmentTags>(controller, kFragmentTagsSlot)(controller, fragment)
                       : nullptr;
}

// Sets the request's tags that are not the fragment's own on the context, once per request.
void set_global_tags(Running& request, int fragment) {
  auto* context = static_cast<std::uint8_t*>(context_of(request.controller));
  if (!context) {
    return;
  }
  const void* fragment_tags = fragment_tags_of(request.controller, fragment);
  const void* definition = field<const void*>(context, kContextTagState);
  for (const auto& name : tag_names(request.order.tags)) {
    if (fragment_tags &&
        reinterpret_cast<FindTag>(natives.find_tag)(fragment_tags, name.c_str()) >= 0) {
      continue;
    }
    const int id =
        definition ? reinterpret_cast<FindTag>(natives.find_tag)(definition, name.c_str()) : -1;
    if (id < 0 || std::find(request.global_tags.begin(), request.global_tags.end(), id) !=
                      request.global_tags.end()) {
      continue;
    }
    reinterpret_cast<SetGlobalTag>(natives.set_global_tag)(context + kContextTagState, id, true);
    request.global_tags.push_back(id);
    if (name.rfind("sitting", 0) == 0) {
      request.stance_tags.push_back(id);
    }
  }
}

}  // namespace

bool hold_global_tag(void* controller, const char* name, bool on) {
  auto* context = static_cast<std::uint8_t*>(controller ? context_of(controller) : nullptr);
  if (!context || !natives.find_tag || !natives.set_global_tag) {
    return false;
  }
  const void* definition = field<const void*>(context, kContextTagState);
  const int id = definition ? reinterpret_cast<FindTag>(natives.find_tag)(definition, name) : -1;
  if (id < 0) {
    return false;
  }
  reinterpret_cast<SetGlobalTag>(natives.set_global_tag)(context + kContextTagState, id, on);
  return true;
}

namespace {

// Clears the tags the request set. A player who sits on a seat keeps the sitting stance's: the
// seat holds the same tag, and clearing it stood the player up from the bench; the
// seat's own stand-up clears it.
void clear_global_tags(Running& request) {
  auto* context = static_cast<std::uint8_t*>(context_of(request.controller));
  const bool seated = natives.seated && natives.seated();
  if (context) {
    for (const int id : request.global_tags) {
      const bool stance = std::find(request.stance_tags.begin(), request.stance_tags.end(), id) !=
                          request.stance_tags.end();
      if (seated && stance) {
        if (natives.log) {
          natives.log("ANIMATION request=%llu ended on a seat: its sitting stance stays",
                      static_cast<unsigned long long>(request.order.request));
        }
        continue;
      }
      reinterpret_cast<SetGlobalTag>(natives.set_global_tag)(context + kContextTagState, id, false);
    }
  }
  request.global_tags.clear();
  request.stance_tags.clear();
}

int fragment_id(void* controller, const std::string& fragment) {
  const void* context = context_of(controller);
  const void* definition = context ? field<const void*>(context, 0) : nullptr;
  const void* fragments =
      definition ? field<const void*>(definition, kDefinitionFragments) : nullptr;
  return fragments ? reinterpret_cast<FindTag>(natives.find_tag)(fragments, fragment.c_str()) : -1;
}

// Gives the action the client's vtable copy (made from the first action's own).
bool watch_action(void* action) {
  void* const* vtable = *static_cast<void* const**>(action);
  if (!game_action_vtable) {
    game_action_vtable = vtable;
    std::memcpy(action_vtable.data(), vtable - 1, action_vtable.size() * sizeof(void*));
    action_vtable[1 + kFailSlot] = reinterpret_cast<void*>(on_fail);
    action_vtable[1 + kExitSlot] = reinterpret_cast<void*>(on_exit);
    action_vtable[1 + kInterruptSlot] = reinterpret_cast<void*>(on_interrupt);
  }
  if (vtable != game_action_vtable) {
    return false;
  }
  *static_cast<void**>(action) = action_vtable.data() + 1;
  watched.push_back(Watched{action});
  return true;
}

// Takes an action out of the watch list.
void unwatch_action(void* action) {
  watched.erase(std::remove_if(watched.begin(), watched.end(),
                               [&](const Watched& entry) { return entry.action == action; }),
                watched.end());
}

void release_action(Running& request) {
  if (!request.action) {
    return;
  }
  watched.erase(
      std::remove_if(watched.begin(), watched.end(),
                     [&](const Watched& entry) { return entry.action == request.action; }),
      watched.end());
  reinterpret_cast<Assign>(natives.assign)(&request.action, nullptr);
  request.action = nullptr;
}

// Queues the request's fragment with its fragment tags, as its action or, for a loop, as the pass
// after it; false when the game would not take it.
bool queue_action(Running& request, bool as_next = false) {
  const int fragment = fragment_id(request.controller, request.order.fragment);
  if (fragment < 0) {
    return false;
  }
  const void* fragment_tags = fragment_tags_of(request.controller, fragment);
  alignas(8) std::array<std::uint8_t, kTagStateBytes> tag_state{};
  if (fragment_tags) {
    for (const auto& name : tag_names(request.order.tags)) {
      const int id = reinterpret_cast<FindTag>(natives.find_tag)(fragment_tags, name.c_str());
      if (id >= 0) {
        reinterpret_cast<SetTag>(natives.set_tag)(fragment_tags, tag_state.data(), id, true);
      }
    }
  }
  void* memory = reinterpret_cast<Allocate>(natives.allocate)(kActionBytes);
  if (!memory) {
    return false;
  }
  const bool seated = natives.seated && natives.seated();
  void* action = reinterpret_cast<Construct>(natives.construct)(
      memory, seated ? kSeatedPriority : kPriority, fragment, tag_state.data(), 0, 0, 0);
  void** holder = as_next ? &request.next : &request.action;
  reinterpret_cast<Assign>(natives.assign)(holder, action);
  if (!watch_action(action)) {
    unwatch_action(action);
    reinterpret_cast<Assign>(natives.assign)(holder, nullptr);
    *holder = nullptr;
    return false;
  }
  if (!as_next) {
    request.installed = false;
  }
  slot<Queue>(request.controller, kQueueSlot)(request.controller, action, kNoTimeLimit);
  return true;
}

// A loop's pass: the next one is queued once the playing one is installed.
void queue_next_pass(Running& request) {
  if (request.order.loop && request.installed && !request.next) {
    queue_action(request, true);
  }
}

// The next pass takes over from the one that ended; false without one (or one the game dropped).
bool take_next_pass(Running& request) {
  if (!request.next) {
    return false;
  }
  const int status = field<int>(request.next, kActionStatus);
  const Watched* entry = watch_of(request.next);
  if (status == kStatusFinished || (entry && (entry->failed || entry->exited))) {
    unwatch_action(request.next);
    reinterpret_cast<Assign>(natives.assign)(&request.next, nullptr);
    request.next = nullptr;
    return false;
  }
  request.action = request.next;
  request.next = nullptr;
  request.installed = status == kStatusInstalled;
  request.logged_status = -1;
  return true;
}

// Takes the queued next pass off the controller (stopped unless `stop` is false) and drops it.
void drop_next_pass(Running& request, bool stop) {
  if (!request.next) {
    return;
  }
  const int status = field<int>(request.next, kActionStatus);
  if (stop && status != kStatusNone && status != kStatusFinished) {
    reinterpret_cast<Stop>(natives.stop)(request.next);
  }
  unwatch_action(request.next);
  if (stop) {
    reinterpret_cast<Assign>(natives.assign)(&request.next, nullptr);
  }
  request.next = nullptr;
}

// A body's skeleton (its engine entity's character), and its animation set when asked; null
// without a character.
void* skeleton_of(void* entity, void** animations) {
  void* character =
      entity ? slot<GetCharacter>(entity, kGetCharacterSlot)(entity, kCharacterSlot) : nullptr;
  if (!character) {
    return nullptr;
  }
  if (animations) {
    *animations = slot<Getter>(character, kAnimationSetSlot)(character);
  }
  return slot<Getter>(character, kSkeletonAnimSlot)(character);
}

// The local player's skeleton, and its animation set when asked; null without a body.
void* local_skeleton(void** animations) {
  return skeleton_of(natives.local_entity ? natives.local_entity() : nullptr, animations);
}

// The name of a shipped clip ("<resource>/<path>") in a body's animation set and of its file in
// the game's file system: kcds_<resource>_<path> without ".caf", lower case, every other character
// '_' (as the book pictures a resource ships are named).
std::string shipped_clip_name(const std::string& clip) {
  std::string name = "kcds_" + clip;
  if (name.size() >= 4 && _stricmp(name.c_str() + name.size() - 4, ".caf") == 0) {
    name.resize(name.size() - 4);
  }
  for (char& c : name) {
    if (c >= 'A' && c <= 'Z') {
      c = char(c - 'A' + 'a');
    } else if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9')) {
      c = '_';
    }
  }
  return name;
}

// The set's CAF loader, the cvar ca_UseIMG_CAF held at 0 for this one call (see
// kShippedClipRoot); it is put back however the call ends.
int load_caf_from_file(void* animations, std::uint64_t path_hash, std::uint64_t name_hash,
                       const char* path, const char* name) {
  const int img = *natives.use_img_caf;
  int result = -1;
  __try {
    *natives.use_img_caf = 0;
    result = reinterpret_cast<LoadFileCaf>(natives.load_file_caf)(animations, path_hash, name_hash,
                                                                  path, name);
  } __finally {
    *natives.use_img_caf = img;
  }
  return result;
}

// The id of a shipped clip ("<resource>/<path>") in a body's animation set, which it is added to
// the first time; -1 when its file has not come from the runtime or the set did not take it.
int shipped_clip_id(void* animations, const std::string& clip) {
  const std::string name = shipped_clip_name(clip);
  int id = slot<AnimIdByName>(animations, kAnimIdByNameSlot)(animations, name.c_str());
  if (id >= 0) {
    return id;
  }
  if (!natives.load_file_caf || !natives.name_hash || !natives.use_img_caf ||
      !natives.mount_shipped) {
    return -1;
  }
  const std::size_t slash = clip.find('/');
  const std::string file = name + ".caf";
  if (!natives.mount_shipped(clip.substr(0, slash), clip.substr(slash + 1), kShippedClipRoot,
                             file)) {
    if (natives.log) {
      natives.log("ANIMATION shipped clip %s is not in the game's files", clip.c_str());
    }
    return -1;
  }
  const std::string path = std::string(kShippedClipRoot) + file;
  const auto hash = reinterpret_cast<NameHash>(natives.name_hash);
  load_caf_from_file(animations, hash(path.c_str(), unsigned(path.size())),
                     hash(name.c_str(), unsigned(name.size())), path.c_str(), name.c_str());
  id = slot<AnimIdByName>(animations, kAnimIdByNameSlot)(animations, name.c_str());
  if (natives.log) {
    natives.log("ANIMATION shipped clip %s added to the animation set %p as %s (id %d)",
                clip.c_str(), animations, name.c_str(), id);
  }
  return id;
}

// The id of a clip of the game's own files by its whole path ("Animations/....caf": a cutscene's,
// which no body's set lists), added to the body's set under its file's name the first time with
// the set's CAF loader (CAnimationSet::LoadFileCAF); -1 for anything else or when the set did not
// take it (no such file).
bool is_game_clip_path(const std::string& path) {
  return path.size() > 15 && _strnicmp(path.c_str(), "Animations/", 11) == 0 &&
         _stricmp(path.c_str() + path.size() - 4, ".caf") == 0 &&
         path.find("..") == std::string::npos;
}

// The ids of clips taken by a game path (a cutscene's motion: clip_in_place keeps a body playing
// one where it stands). The same ids on every client: each loads the same clip the same way.
std::mutex game_clip_ids_mutex;
std::unordered_set<int> game_clip_ids;

void note_game_clip(int id) {
  if (id >= 0) {
    std::lock_guard<std::mutex> lock(game_clip_ids_mutex);
    game_clip_ids.insert(id);
  }
}

int game_clip_id_unnoted(void* animations, const std::string& path);

int game_clip_id(void* animations, const std::string& path) {
  const int id = game_clip_id_unnoted(animations, path);
  note_game_clip(id);
  return id;
}

int game_clip_id_unnoted(void* animations, const std::string& path) {
  if (!is_game_clip_path(path) || !natives.load_file_caf || !natives.name_hash ||
      !natives.use_img_caf) {
    return -1;
  }
  const std::size_t slash = path.rfind('/');
  std::string name = path.substr(slash + 1, path.size() - slash - 5);
  for (char& c : name) {
    if (c >= 'A' && c <= 'Z') {
      c = char(c - 'A' + 'a');
    }
  }
  int id = slot<AnimIdByName>(animations, kAnimIdByNameSlot)(animations, name.c_str());
  if (id >= 0) {
    return id;
  }
  if (natives.log) {
    natives.log("ANIMATION game clip %s: loading it as %s", path.c_str(), name.c_str());
  }
  const auto hash = reinterpret_cast<NameHash>(natives.name_hash);
  load_caf_from_file(animations, hash(path.c_str(), unsigned(path.size())),
                     hash(name.c_str(), unsigned(name.size())), path.c_str(), name.c_str());
  id = slot<AnimIdByName>(animations, kAnimIdByNameSlot)(animations, name.c_str());
  if (natives.log) {
    natives.log("ANIMATION game clip %s added to the animation set %p as %s (id %d)", path.c_str(),
                animations, name.c_str(), id);
  }
  return id;
}

// A path in lower case with forward slashes and without its extension.
std::string plain_path(const char* path) {
  std::string text = path;
  for (char& c : text) {
    if (c == '\\') {
      c = '/';
    } else if (c >= 'A' && c <= 'Z') {
      c = char(c - 'A' + 'a');
    }
  }
  const std::size_t slash = text.rfind('/');
  const std::size_t dot = text.rfind('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
    text.resize(dot);
  }
  return text;
}

// The id of a clip in the body's animation set, -1 for none: the set names a clip by its file's
// name without the extension. A path (one with a folder) must be that clip's file, any case, the
// set's path possibly starting further up.
int clip_id(void* animations, const std::string& clip) {
  const std::string wanted = plain_path(clip.c_str());
  const std::size_t slash = wanted.rfind('/');
  const std::string name = slash == std::string::npos ? wanted : wanted.substr(slash + 1);
  const int id = slot<AnimIdByName>(animations, kAnimIdByNameSlot)(animations, name.c_str());
  if (id < 0 || slash == std::string::npos) {
    return id;
  }
  const char* file = slot<FilePathById>(animations, kFilePathByIdSlot)(animations, id);
  const std::string have = plain_path(file ? file : "");
  const bool same = have == wanted ||
                    (have.size() > wanted.size() && have[have.size() - wanted.size() - 1] == '/' &&
                     have.compare(have.size() - wanted.size(), wanted.size(), wanted) == 0);
  if (!same && natives.log) {
    natives.log("ANIMATION clip %s: the body's %s is %s", clip.c_str(), name.c_str(),
                file ? file : "(no file)");
  }
  return same ? id : -1;
}

// The newest animation on a layer (the clip layer unless named), or null.
const void* newest_clip(void* skeleton, std::uint32_t layer = kClipLayer) {
  const unsigned count = slot<LayerCount>(skeleton, kLayerCountSlot)(skeleton, layer);
  return count ? slot<LayerAnimation>(skeleton, kLayerAnimationSlot)(skeleton, layer, count - 1)
               : nullptr;
}

// Starts a clip on a skeleton's layer (the clip layer unless named), looped or holding its last
// key; false when the skeleton did not take it.
bool start_on(void* skeleton, int id, bool loop, std::uint32_t layer = kClipLayer,
              float key_time = -1.0f) {
  ClipParams params{};
  params.transition = kClipBlendIn;
  params.key_time = key_time;
  params.speed = 1.0f;
  params.weight = 1.0f;
  params.layer = layer;
  params.layer_weight = 1.0f;
  params.flags = loop ? kClipLoop : kClipHoldLastKey;
  return slot<StartAnimation>(skeleton, kStartAnimationSlot)(skeleton, id, &params);
}

// The clip's id, with a fault in the game's code kept from taking it down (below): -2.
int guarded_clip_id(void* animations, const std::string& clip);

// Starts the request's clip on the body; false when the body has no such clip.
bool start_clip(Running& request) {
  void* animations = nullptr;
  void* skeleton = local_skeleton(&animations);
  if (!skeleton || !animations || !natives.progress) {
    return false;
  }
  kcd2s::integration::ShippedClip shipped;
  const bool from_resource = kcd2s::integration::shipped_clip_of(request.order.clip, shipped);
  // A clip of the game's own files the body's set does not list (a cutscene's, say), named by
  // its whole path, is added to the set as a .chrparams list adds the game's clips; looked up
  // guarded, as the studio does: the lookup by its path faulted in the game for one (the
  // executioner's whipping).
  const int id = from_resource ? shipped_clip_id(animations, shipped.clip)
                               : guarded_clip_id(animations, request.order.clip);
  if (id < 0) {
    if (id == -2 && natives.log) {
      natives.log("ANIMATION request=%llu clip %s: the game faulted looking it up",
                  static_cast<unsigned long long>(request.order.request),
                  request.order.clip.c_str());
    }
    return false;
  }
  // `time` seconds into a clip: started there (its key time of its length, IAnimationSet
  // GetDuration_sec, slot 13) instead of played there fast (a second of a
  // cutscene's walking at 20 times its speed before each strike).
  float key_time = -1.0f;
  if (request.order.start > 0.0f) {
    const float seconds = slot<DurationById>(animations, kDurationByIdSlot)(animations, id);
    if (seconds > request.order.start) {
      key_time = request.order.start / seconds;
      request.played = request.order.start;
    }
  }
  starting_shipped = from_resource;
  const bool started = start_on(skeleton, id, request.order.loop, kClipLayer, key_time);
  starting_shipped = false;
  if (!started) {
    return false;
  }
  request.clip = id;
  request.shipped = from_resource;
  if (natives.log) {
    natives.log("ANIMATION request=%llu clip %s plays (id %d, layer %u, loop %u)",
                static_cast<unsigned long long>(request.order.request), request.order.clip.c_str(),
                id, kClipLayer, unsigned(request.order.loop));
  }
  return true;
}

// Sets a layer's playback scale when it is not that already.
void set_layer_scale(void* skeleton, std::uint32_t layer, float scale) {
  if (field<float>(skeleton, kLayerScales + kLayerStride * layer) != scale) {
    slot<SetLayerScale>(skeleton, kLayerScaleSlot)(skeleton, layer, scale);
  }
}

// The clip layer plays as authored again once the request's clip is gone.
void release_clip_speed(Running& request) {
  void* skeleton = local_skeleton(nullptr);
  if (skeleton && request.applied_speed != 1.0f) {
    set_layer_scale(skeleton, kClipLayer, 1.0f);
  }
  request.applied_speed = 1.0f;
}

// Blends the request's clip out while it is still the one on its layer.
void stop_clip(Running& request) {
  if (request.clip < 0) {
    return;
  }
  void* skeleton = local_skeleton(nullptr);
  const void* animation = skeleton ? newest_clip(skeleton) : nullptr;
  if (animation && field<int>(animation, kAnimationClip) == request.clip) {
    slot<StopLayer>(skeleton, kStopLayerSlot)(skeleton, kClipLayer, kClipBlendOut);
  }
  release_clip_speed(request);
  request.clip = -1;
}

// The request's speed this frame: its own, or faster while it has not yet played as
// long as its start time; `seconds` is the frame's length. The fragment's action (and a loop's next
// pass) get it as their speed bias, a clip's layer as its playback scale.
void keep_speed(Running& request, float seconds) {
  const bool playing =
      request.clip >= 0 ||
      (request.action && field<int>(request.action, kActionStatus) == kStatusInstalled);
  if (playing) {
    request.played += std::min(seconds, kLongestFrame) * request.applied_speed;
  }
  float speed = request.order.speed;
  if (request.played < request.order.start) {
    const float left = request.order.start - request.played;
    speed = std::min(kSkipSpeed, left / std::max(seconds, 0.001f));
    speed = std::max(speed, std::min(request.order.speed, kSkipSpeed));
  }
  if (request.action || request.next || request.clip >= 0) {
    for (void* action : {request.action, request.next}) {
      if (action && field<float>(action, kActionSpeedBias) != speed) {
        std::memcpy(static_cast<std::uint8_t*>(action) + kActionSpeedBias, &speed, sizeof(speed));
      }
    }
    void* skeleton = request.clip >= 0 ? local_skeleton(nullptr) : nullptr;
    if (skeleton) {
      set_layer_scale(skeleton, kClipLayer, speed);
    }
  }
  if (speed != request.applied_speed && natives.log && request.played >= request.order.start) {
    natives.log("ANIMATION request=%llu plays at %.2f (played %.2f s)",
                static_cast<unsigned long long>(request.order.request), speed, request.played);
  }
  request.applied_speed = speed;
}

// Watches the request's clip: a one-shot that played to its end is blended out (finished); a
// clip the game replaced on its layer, or took away, is let go (interrupted). Nothing while it
// plays.
std::optional<AnimationReason> watch_clip(Running& request) {
  if (request.clip < 0) {
    return std::nullopt;
  }
  void* skeleton = local_skeleton(nullptr);
  const void* animation = skeleton ? newest_clip(skeleton) : nullptr;
  const unsigned long long id = request.order.request;
  if (!animation || field<int>(animation, kAnimationClip) != request.clip) {
    if (natives.log) {
      natives.log("ANIMATION request=%llu clip left its layer", id);
    }
    release_clip_speed(request);
    request.clip = -1;
    return AnimationReason::Interrupted;
  }
  if (request.order.loop || natives.progress(skeleton, animation) < kClipEnd) {
    return std::nullopt;
  }
  if (natives.log) {
    natives.log("ANIMATION request=%llu clip played to its end", id);
  }
  stop_clip(request);
  return AnimationReason::Finished;
}

// A clip the game refused: tried again each frame while its time lasts. True once its time ran
// out without the clip starting.
bool retry_clip(Running& request, float seconds) {
  if (request.clip_retry <= 0.0f) {
    return false;
  }
  request.clip_retry -= seconds;
  if (start_clip(request)) {
    request.clip_retry = 0.0f;
    return false;
  }
  if (request.clip_retry > 0.0f) {
    return false;
  }
  if (natives.log) {
    natives.log("ANIMATION request=%llu clip %s did not start",
                static_cast<unsigned long long>(request.order.request), request.order.clip.c_str());
  }
  return true;
}

// Ends the running request at once, without a report (the server raises stopped or replaced).
void end_running() {
  if (!running) {
    return;
  }
  stop_clip(*running);
  drop_next_pass(*running, true);
  if (running->action) {
    const int status = field<int>(running->action, kActionStatus);
    if (status != kStatusNone && status != kStatusFinished) {
      reinterpret_cast<Stop>(natives.stop)(running->action);
    }
    release_action(*running);
  }
  clear_global_tags(*running);
  running.reset();
}

// The talking the server asked for: its style ("" while not talking), the clip's id and the
// skeleton it loops on while it does.
struct Talking {
  std::string style;
  int clip = -1;
  void* skeleton = nullptr;
  // A replica's loop that did not start on a body with the clip (its layer still blending out a
  // moment ago) is tried again from then on.
  std::chrono::steady_clock::time_point retry_at{};
  bool retry = false;
};
// How soon a replica's talking loop that did not start is tried again.
constexpr auto kTalkRetry = std::chrono::milliseconds(250);
Talking talking;
// The player is in one of the game's dialogues: its own lip-sync speaks for the body, so the loop
// steps aside and comes back after.
bool talking_paused = false;
// The replicas' talking, by their owners' network ids.
std::unordered_map<std::uint32_t, Talking> replica_talking;

const char* talk_clip_of(const std::string& style) {
  for (const TalkClip& entry : kTalkClips) {
    if (style == entry.style) {
      return entry.clip;
    }
  }
  return nullptr;
}

// Blends the talking loop out while it is still the one on its layer.
void stop_talking() {
  void* skeleton = talking.clip >= 0 ? local_skeleton(nullptr) : nullptr;
  const void* animation =
      skeleton && skeleton == talking.skeleton ? newest_clip(skeleton, kTalkLayer) : nullptr;
  if (animation && field<int>(animation, kAnimationClip) == talking.clip) {
    slot<StopLayer>(skeleton, kStopLayerSlot)(skeleton, kTalkLayer, kClipBlendOut);
  }
  talking.clip = -1;
  talking.skeleton = nullptr;
}

// Keeps the talking loop on the body: started when it is not on its layer (a new body, or the
// game took the layer), once a frame.
void keep_talking() {
  if (talking.style.empty()) {
    return;
  }
  if (talking_paused) {
    if (talking.clip >= 0) {
      stop_talking();
      if (natives.log) {
        natives.log("ANIMATION talking %s paused for the game's dialogue", talking.style.c_str());
      }
    }
    return;
  }
  void* animations = nullptr;
  void* skeleton = local_skeleton(&animations);
  if (!skeleton || !animations) {
    return;
  }
  const void* animation = newest_clip(skeleton, kTalkLayer);
  if (skeleton == talking.skeleton && animation &&
      field<int>(animation, kAnimationClip) == talking.clip) {
    return;
  }
  const char* clip = talk_clip_of(talking.style);
  const int id = clip ? clip_id(animations, clip) : -1;
  // Sent by name in the field, as a shipped clip is: the clip sync leaves the start out.
  starting_shipped = true;
  const bool started = id >= 0 && start_on(skeleton, id, true, kTalkLayer);
  starting_shipped = false;
  if (natives.log && (skeleton != talking.skeleton || !started)) {
    natives.log("ANIMATION talking %s: clip %s %s (id %d, layer %u)", talking.style.c_str(),
                clip ? clip : "(none)", started ? "loops" : "not started", id, kTalkLayer);
  }
  talking.clip = started ? id : -1;
  talking.skeleton = skeleton;
  if (!started) {
    // Not tried again every frame: a body without the clip keeps its own face.
    talking.style.clear();
  }
}

// The style the mouth moves in now, or none ("").
void set_talking(const std::string& style) {
  if (style == talking.style) {
    return;
  }
  stop_talking();
  talking.style = style;
  keep_talking();
  if (style.empty() && natives.log) {
    natives.log("ANIMATION talking stopped");
  }
}

// Ends the face's expression at once, its tags too.
void end_face() {
  if (!face) {
    return;
  }
  if (face->action) {
    const int status = field<int>(face->action, kActionStatus);
    if (status != kStatusNone && status != kStatusFinished) {
      reinterpret_cast<Stop>(natives.stop)(face->action);
    }
    release_action(*face);
  }
  clear_global_tags(*face);
  face.reset();
}

// A new expression for the face, or none: the game's own face comes back.
void set_face(void* controller, const AnimationOrder& order) {
  end_face();
  if (order.fragment.empty()) {
    return;
  }
  Running expression;
  expression.order = order;
  expression.controller = controller;
  set_global_tags(expression, fragment_id(controller, order.fragment));
  if (!queue_action(expression)) {
    clear_global_tags(expression);
    if (natives.log) {
      natives.log("ANIMATION face %s is not the body's fragment", order.fragment.c_str());
    }
    return;
  }
  face = std::move(expression);
}

// The game's way out of the stance a request held, from its tags: StandUp from a sitting
// stance, GetUp from a lying one, none for any other (to verify live against vanilla).
AnimationExit exit_of(const std::string& tags) {
  for (const auto& name : tag_names(tags)) {
    if (name.rfind("sitting", 0) == 0) {
      return AnimationExit::StandUp;
    }
    if (name.rfind("lying", 0) == 0) {
      return AnimationExit::GetUp;
    }
  }
  return AnimationExit::None;
}

void play(void* controller, const AnimationOrder& order, std::vector<AnimationEnd>& ends) {
  Running request;
  request.order = order;
  request.controller = controller;
  set_global_tags(request, fragment_id(controller, order.fragment));
  lock_movement(order.lock_movement);
  // A clip stands in for a fragment; the server raises no event for a request without one.
  if (!order.clip.empty() && !order.restore && !start_clip(request)) {
    request.clip_retry = kClipRetrySeconds;
  }
  // Tags only, none of them the body's ('monk' exists on a man's body, not on a
  // woman's): refused, so the script hears that nothing changed.
  const bool tags_only =
      order.fragment.empty() && order.clip.empty() && !order.restore && !order.tags.empty();
  if (tags_only && request.global_tags.empty()) {
    if (natives.log) {
      natives.log("ANIMATION request=%llu: none of the tags %s is the body's",
                  static_cast<unsigned long long>(order.request), order.tags.c_str());
    }
    ends.push_back({order.request, AnimationReason::Refused, AnimationExit::None});
  }
  // Only tags (and props) to put on, or a request that already ended: nothing to play or report.
  if (order.fragment.empty() || order.restore) {
    running = std::move(request);
    return;
  }
  if (!queue_action(request)) {
    ends.push_back({order.request, AnimationReason::Refused, AnimationExit::None});
  }
  // A pair's slave starts where the server put it, at the master's place.
  request.started = std::chrono::steady_clock::now();
  void* body = natives.local_entity ? natives.local_entity() : nullptr;
  const bool placed = order.paired_slave && body && natives.body_place && natives.move_body &&
                      natives.body_place(body, request.start.q, request.start.t);
  request.aligned = !placed;
  running = std::move(request);
}

void stop(void* controller, const AnimationOrder& order, std::vector<AnimationEnd>& ends) {
  const std::string stance = running ? running->order.tags : std::string();
  end_running();
  lock_movement(false);
  if (!order.stand_up || !order.request) {
    return;
  }
  const AnimationExit exit = exit_of(stance);
  if (exit == AnimationExit::None) {
    ends.push_back({order.request, AnimationReason::Finished, AnimationExit::None});
    return;
  }
  Running request;
  request.order.request = order.request;
  request.order.fragment = exit == AnimationExit::StandUp ? "StandUp" : "GetUp";
  request.order.tags = stance;
  request.exit = exit;
  request.controller = controller;
  set_global_tags(request, fragment_id(controller, request.order.fragment));
  if (!queue_action(request)) {
    clear_global_tags(request);
    ends.push_back({order.request, AnimationReason::Refused, exit});
    return;
  }
  running = std::move(request);
}

}  // namespace

bool install(const Natives& given) {
  natives = given;
  ready = natives.allocate && natives.construct && natives.assign && natives.stop &&
          natives.find_tag && natives.set_tag && natives.set_global_tag;
  return ready;
}

void take(void* controller, const AnimationOrder& order, std::vector<AnimationEnd>& ends) {
  const bool play_order = order.kind == AnimationOrder::Kind::Play;
  if (!ready || !controller) {
    if (play_order && !order.fragment.empty() && !order.restore) {
      ends.push_back({order.request, AnimationReason::Refused, AnimationExit::None});
    }
    return;
  }
  if ((running && running->controller != controller) || (face && face->controller != controller)) {
    reset();
  }
  if (order.kind == AnimationOrder::Kind::Face) {
    set_face(controller, order);
    return;
  }
  if (order.kind == AnimationOrder::Kind::Talk) {
    set_talking(order.fragment);
    return;
  }
  // The running request's speed or props; an order for one that is gone is late.
  if (order.kind == AnimationOrder::Kind::Speed || order.kind == AnimationOrder::Kind::Props) {
    if (!running || running->order.request != order.request) {
      return;
    }
    if (order.kind == AnimationOrder::Kind::Speed) {
      running->order.speed = order.speed;
    } else {
      running->order.props = order.props;
    }
    return;
  }
  if (play_order) {
    end_running();
    play(controller, order, ends);
  } else {
    stop(controller, order, ends);
  }
}

namespace {

// How the request's queued action went: none while it still plays, else how it ended (also kept
// in end_detail()).
std::optional<AnimationReason> judge(Running& request) {
  const Watched* entry = watch_of(request.action);
  const int status = field<int>(request.action, kActionStatus);
  if (status == kStatusInstalled) {
    if (!request.installed) {
      request.installed_at = std::chrono::steady_clock::now();
      request.clips_at_install = local_clip_starts.load();
    }
    request.installed = true;
  }
  if (status != request.logged_status && natives.log) {
    natives.log("ANIMATION request=%llu action status %d -> %d (flags 0x%x)",
                static_cast<unsigned long long>(request.order.request), request.logged_status,
                status, field<unsigned>(request.action, kActionFlags));
    request.logged_status = status;
  }
  std::optional<AnimationReason> reason;
  if (entry && entry->failed) {
    reason = AnimationReason::Refused;
  } else if (entry && entry->interrupted) {
    // A one-shot that played to its end is already exiting when the controller lets it go
    // (status 3, flags 0x1a20 with FragmentIsOneShot, after 2.26 s and 3.10 s of the game's own
    // one-shots); one cut short by another action is still installed. A looping fragment never
    // ends by itself: exiting without FragmentIsOneShot is another action taking over
    // (PrayStand, flags 0xa20 after 9.11 s, when combat took the body).
    const bool played_out = entry->interrupt_status == kStatusExiting &&
                            (entry->interrupt_flags & kFlagFragmentIsOneShot) != 0;
    reason = played_out ? AnimationReason::Finished : AnimationReason::Interrupted;
  } else if (status == kStatusFinished || (entry && entry->exited)) {
    reason = request.installed || status == kStatusFinished ? AnimationReason::Finished
                                                            : AnimationReason::Refused;
  } else if (status == kStatusNone) {
    reason = request.installed ? AnimationReason::Interrupted : AnimationReason::Refused;
  }
  if (!reason) {
    return std::nullopt;
  }
  std::snprintf(end_detail_text, sizeof(end_detail_text),
                "status=%d installed=%u failed=%u exited=%u interrupted=%u at_interrupt=(status "
                "%d flags 0x%x time %.2f)",
                status, unsigned(request.installed), unsigned(entry && entry->failed),
                unsigned(entry && entry->exited), unsigned(entry && entry->interrupted),
                entry ? entry->interrupt_status : -1, entry ? entry->interrupt_flags : 0u,
                entry ? entry->interrupt_time : 0.0f);
  return reason;
}

// Takes the action out of the watch without releasing it (its controller may be gone).
void unwatch(void* action) {
  watched.erase(std::remove_if(watched.begin(), watched.end(),
                               [&](const Watched& entry) { return entry.action == action; }),
                watched.end());
}

// A paired request's slave: once its fragment's clip and its master's both have a
// start location, the body goes from the master's place to its clip's start relative to the
// master's, keeping what it moved since the order came.
void align_slave(Running& request) {
  if (request.aligned || !request.installed) {
    return;
  }
  const unsigned long long id = request.order.request;
  const auto seen = base_clips.find(request.order.partner);
  const bool fresh = seen != base_clips.end() && seen->second.at >= request.started;
  if (std::chrono::steady_clock::now() - request.started > kAlignWindow) {
    request.aligned = true;
    if (natives.log) {
      natives.log(
          "ANIMATION request=%llu paired: no start locations (master clip %d), left at the "
          "master's place",
          id, fresh ? seen->second.clip : -1);
    }
    return;
  }
  if (!fresh) {
    return;
  }
  const PartnerClip& partner = seen->second;
  void* body = natives.local_entity();
  void* own_set = nullptr;
  void* skeleton = body ? skeleton_of(body, &own_set) : nullptr;
  const void* own = skeleton ? newest_clip(skeleton, kBaseLayer) : nullptr;
  void* master_set = nullptr;
  skeleton_of(partner.body, &master_set);
  if (!own || !master_set) {
    return;
  }
  const int own_clip = field<int>(own, kAnimationClip);
  paired_start::Place master_start;
  paired_start::Place slave_start;
  paired_start::Place now;
  if (!paired_start::start_location(master_set, partner.clip, master_start) ||
      !paired_start::start_location(own_set, own_clip, slave_start) ||
      !natives.body_place(body, now.q, now.t)) {
    return;
  }
  // Two halves of one pair start close together; a master clip that is not the pair's (its
  // fragment did not play: Quest_StandHlbrdKill without a halberd ends at once) leaves the slave
  // where it is.
  const float dx = master_start.t[0] - slave_start.t[0];
  const float dy = master_start.t[1] - slave_start.t[1];
  if (dx * dx + dy * dy > kPairReach * kPairReach) {
    request.aligned = true;
    if (natives.log) {
      natives.log(
          "ANIMATION request=%llu paired: master clip %d starts %.1f m from own clip %d, "
          "not its pair: left at the master's place",
          id, partner.clip, std::sqrt(dx * dx + dy * dy), own_clip);
    }
    return;
  }
  const paired_start::Place target =
      paired_start::slave_place(request.start, master_start, slave_start);
  const paired_start::Place moved = paired_start::compose(
      target, paired_start::compose(paired_start::inverse(request.start), now));
  natives.move_body(body, moved.q, moved.t);
  request.aligned = true;
  if (natives.log) {
    natives.log(
        "ANIMATION request=%llu paired: master clip %d, own clip %d: from %.2f,%.2f,%.2f "
        "to %.2f,%.2f,%.2f",
        id, partner.clip, own_clip, now.t[0], now.t[1], now.t[2], moved.t[0], moved.t[1],
        moved.t[2]);
  }
}

// Fragments a resource plays on other bodies: NPCs this client simulates, by the
// NPC's id. A request's global tags stay until it is stopped or replaced, as the player's do.
std::unordered_map<std::uint32_t, Running> bodies;

}  // namespace

// A one-shot that ended by itself frees the player: its lockMovement and lockRotation go. A
// request that leaves the body in a stance (sitting, lying) keeps them until it is stopped, as
// its stance does.
static void free_after_end(Running& request) {
  if (request.order.loop || exit_of(request.order.tags) != AnimationExit::None) {
    return;
  }
  if (request.order.lock_movement) {
    request.order.lock_movement = false;
    lock_movement(false);
  }
  request.order.lock_rotation = false;
}

void frame(void* controller, std::vector<AnimationEnd>& ends) {
  keep_talking();
  static auto last_frame = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  const float seconds = std::chrono::duration<float>(now - last_frame).count();
  last_frame = now;
  if (running) {
    const bool clip_refused = retry_clip(*running, seconds);
    const std::optional<AnimationReason> clip_end = watch_clip(*running);
    keep_speed(*running, seconds);
    // A clip played without a fragment ends with its clip (playerAnimationEnd with the clip's
    // name): finished, interrupted when the game took its layer, refused when it never started.
    if (!running->action && running->order.fragment.empty() && !running->order.clip.empty()) {
      if (clip_refused) {
        ends.push_back({running->order.request, AnimationReason::Refused, AnimationExit::None});
        free_after_end(*running);
      } else if (clip_end) {
        ends.push_back({running->order.request, *clip_end, AnimationExit::None});
        free_after_end(*running);
      }
    }
  }
  if (!running || !running->action) {
    return;
  }
  if (running->controller != controller) {
    reset();
    return;
  }
  const std::optional<AnimationReason> reason = judge(*running);
  // A fragment with no clip for these tags on this body (a man's Picking with only "player"):
  // refused and ended as a stop ends it, so the body goes back to its idle instead of standing in
  // its bind pose until the script stops it.
  const bool fragment_order = running->order.clip.empty() && !running->order.fragment.empty() &&
                              running->exit == AnimationExit::None;
  if (!reason && fragment_order && running->installed &&
      local_clip_starts.load() == running->clips_at_install &&
      std::chrono::steady_clock::now() - running->installed_at > kNoClipGrace) {
    if (natives.log) {
      natives.log("ANIMATION request=%llu fragment %s [%s] started no clip: refused",
                  static_cast<unsigned long long>(running->order.request),
                  running->order.fragment.c_str(), running->order.tags.c_str());
    }
    std::snprintf(end_detail_text, sizeof(end_detail_text), "the fragment started no clip");
    ends.push_back({running->order.request, AnimationReason::Refused, AnimationExit::None});
    end_running();
    lock_movement(false);
    return;
  }
  if (!reason) {
    align_slave(*running);
    queue_next_pass(*running);
    return;
  }
  release_action(*running);
  // A loop goes on pass after pass, and after the game cut it short; it reports only a refusal.
  if (running->order.loop && *reason != AnimationReason::Refused) {
    if (take_next_pass(*running)) {
      return;
    }
    if (!queue_action(*running)) {
      ends.push_back({running->order.request, AnimationReason::Refused, AnimationExit::None});
    }
    return;
  }
  ends.push_back({running->order.request, *reason, running->exit});
  free_after_end(*running);
  // A stand-up's stance tags go once the player is up; a played request's stay until it is
  // stopped or replaced.
  if (running->exit != AnimationExit::None) {
    clear_global_tags(*running);
    running.reset();
  }
}

const char* end_detail() {
  return end_detail_text;
}

void note_local_clip_start() {
  local_clip_starts.fetch_add(1);
}

bool play_on_body(std::uint32_t body, void* controller, const std::string& fragment,
                  const std::string& tags, bool loop, std::uint64_t request) {
  stop_on_body(body);
  if (!ready || !controller) {
    return false;
  }
  Running started;
  started.order.request = request;
  started.order.fragment = fragment;
  started.order.tags = tags;
  started.order.loop = loop;
  started.controller = controller;
  set_global_tags(started, fragment_id(controller, fragment));
  if (!queue_action(started)) {
    clear_global_tags(started);
    return false;
  }
  bodies[body] = std::move(started);
  return true;
}

std::optional<AnimationReason> watch_body(std::uint32_t body, void* controller) {
  const auto found = bodies.find(body);
  if (found == bodies.end() || !found->second.action) {
    return std::nullopt;
  }
  Running& request = found->second;
  if (request.controller != controller) {
    // A new body for the NPC: the old one's action went with it.
    forget_body(body);
    return AnimationReason::Interrupted;
  }
  const std::optional<AnimationReason> reason = judge(request);
  if (!reason) {
    queue_next_pass(request);
    return std::nullopt;
  }
  release_action(request);
  // A loop goes on pass after pass, and after the game cut it short; it reports only a refusal.
  if (request.order.loop && *reason != AnimationReason::Refused) {
    if (take_next_pass(request) || queue_action(request)) {
      return std::nullopt;
    }
    clear_global_tags(request);
    bodies.erase(found);
    return AnimationReason::Refused;
  }
  return reason;
}

void stop_on_body(std::uint32_t body) {
  const auto found = bodies.find(body);
  if (found == bodies.end()) {
    return;
  }
  Running& request = found->second;
  drop_next_pass(request, true);
  if (request.action) {
    const int status = field<int>(request.action, kActionStatus);
    if (status != kStatusNone && status != kStatusFinished) {
      reinterpret_cast<Stop>(natives.stop)(request.action);
    }
    release_action(request);
  }
  clear_global_tags(request);
  bodies.erase(found);
}

bool face_on_body(std::uint32_t body, void* controller, const std::string& fragment,
                  const std::string& tags) {
  const auto held = body_faces.find(body);
  if (held != body_faces.end()) {
    Running& expression = held->second;
    if (expression.controller == controller && expression.action) {
      const int status = field<int>(expression.action, kActionStatus);
      if (status != kStatusNone && status != kStatusFinished) {
        reinterpret_cast<Stop>(natives.stop)(expression.action);
      }
      release_action(expression);
      clear_global_tags(expression);
    } else {
      unwatch(expression.action);
    }
    body_faces.erase(held);
  }
  if (fragment.empty() || !ready || !controller) {
    return fragment.empty();
  }
  Running expression;
  expression.order.fragment = fragment;
  expression.order.tags = tags;
  expression.controller = controller;
  set_global_tags(expression, fragment_id(controller, fragment));
  if (!queue_action(expression)) {
    clear_global_tags(expression);
    return false;
  }
  body_faces[body] = std::move(expression);
  return true;
}

void forget_body(std::uint32_t body) {
  const auto face_held = body_faces.find(body);
  if (face_held != body_faces.end()) {
    unwatch(face_held->second.action);
    body_faces.erase(face_held);
  }
  const auto found = bodies.find(body);
  if (found == bodies.end()) {
    return;
  }
  // Its controller is gone: the action is neither stopped nor released, its tags went with it.
  unwatch(found->second.action);
  drop_next_pass(found->second, false);
  bodies.erase(found);
}

void reset() {
  // The body's controller may be gone: its actions are neither stopped nor released (their
  // reference is left to the game), and its tags went with its context.
  if (running && running->action) {
    watched.erase(
        std::remove_if(watched.begin(), watched.end(),
                       [&](const Watched& entry) { return entry.action == running->action; }),
        watched.end());
  }
  if (running) {
    drop_next_pass(*running, false);
  }
  running.reset();
  if (face && face->action) {
    watched.erase(
        std::remove_if(watched.begin(), watched.end(),
                       [&](const Watched& entry) { return entry.action == face->action; }),
        watched.end());
  }
  face.reset();
  // The NPCs' expressions went with the world too.
  for (const auto& [body, expression] : body_faces) {
    unwatch(expression.action);
  }
  body_faces.clear();
  // The NPCs' bodies went with the world.
  for (const auto& [body, request] : bodies) {
    unwatch(request.action);
  }
  bodies.clear();
  // The body's skeleton went too; the server sends the talking again with the world.
  talking = Talking{};
  // The action map manager outlives the body and the world: the player goes free.
  lock_movement(false);
  // The replicas' bodies went with the world.
  replica_clips.clear();
  replica_talking.clear();
}

const std::vector<kcd2s::integration::AnimationProp>& props() {
  static const std::vector<kcd2s::integration::AnimationProp> none;
  return running ? running->order.props : none;
}

std::string speed_mark() {
  if (!running || (running->order.speed == 1.0f && running->order.start == 0.0f)) {
    return {};
  }
  const std::uint32_t layer = running->clip >= 0 ? kClipLayer : 0;
  float place = 0.0f;
  void* skeleton = local_skeleton(nullptr);
  const void* animation = skeleton && natives.progress ? newest_clip(skeleton, layer) : nullptr;
  if (animation) {
    place = std::round(natives.progress(skeleton, animation) * kMarkSteps) / kMarkSteps;
  }
  char text[48];
  std::snprintf(text, sizeof(text), "%.2f:%u:%.2f", running->applied_speed, layer,
                std::clamp(place, 0.0f, 1.0f));
  return text;
}

void keep_replica_speed(std::uint32_t holder, void* body, const std::string& mark, int owner_clip) {
  float speed = 1.0f;
  unsigned layer = 0;
  float place = 0.0f;
  const bool marked = kcd2s::integration::parse_speed_mark(mark, speed, layer, place);
  void* skeleton = body ? skeleton_of(body, nullptr) : nullptr;
  auto found = replica_speeds.find(holder);
  // A new body: the old one's layers went with it.
  if (found != replica_speeds.end() && found->second.skeleton != skeleton) {
    replica_speeds.erase(found);
    found = replica_speeds.end();
  }
  if (!marked || !skeleton) {
    // The owner's request plays as authored again, or ended: so does the replica.
    if (found != replica_speeds.end()) {
      for (std::uint32_t at = 0; at < 16; ++at) {
        if ((found->second.layers & (1u << at)) != 0) {
          set_layer_scale(skeleton, at, 1.0f);
        }
      }
      replica_speeds.erase(found);
    }
    return;
  }
  ReplicaSpeed& entry = replica_speeds[holder];
  entry.skeleton = skeleton;
  const std::uint32_t layers = layer == kClipLayer ? (1u << kClipLayer) : kFragmentLayers;
  for (std::uint32_t at = 0; at < 16; ++at) {
    if ((layers & (1u << at)) != 0) {
      set_layer_scale(skeleton, at, speed);
    } else if ((entry.layers & (1u << at)) != 0) {
      set_layer_scale(skeleton, at, 1.0f);
    }
  }
  entry.layers = layers;
  // The replica's clip on the owner's layer goes to the owner's place when it is off.
  const void* animation = natives.progress ? newest_clip(skeleton, layer) : nullptr;
  if (!animation || field<int>(animation, kAnimationClip) != owner_clip) {
    return;
  }
  const float own = natives.progress(skeleton, animation);
  float off = std::fabs(own - place);
  off = std::min(off, 1.0f - off);
  if (off > (speed == 0.0f ? kReplicaHeldDrift : kReplicaDrift)) {
    slot<SetLayerTime>(skeleton, kSetLayerTimeSlot)(skeleton, layer, place);
  }
}

void forget_replica_speed(std::uint32_t holder) {
  replica_speeds.erase(holder);
  base_clips.erase(holder);
}

bool upper_body_running() {
  return running && running->order.upper_body;
}

bool no_gravity_running() {
  return running && running->order.no_gravity;
}

bool lock_rotation_running() {
  return running && running->order.lock_rotation;
}

LookLimits look_limits() {
  LookLimits limits;
  if (running && running->order.lock_rotation) {
    limits.yaw = running->order.look_yaw;
    limits.up = running->order.look_up;
    limits.down = running->order.look_down;
  }
  return limits;
}

void replica_base_clip(std::uint32_t holder, void* body, int clip) {
  if (clip < 0 || !body) {
    return;
  }
  base_clips[holder] = {body, clip, std::chrono::steady_clock::now()};
}

std::uint32_t running_partner() {
  // Only while its fragment plays: a one-shot that ended lets the bodies collide again.
  return running && running->action ? running->order.partner : 0;
}

bool play_local(void* controller, const AnimationOrder& order) {
  AnimationOrder own = order;
  own.kind = AnimationOrder::Kind::Play;
  own.request = 0;
  std::vector<AnimationEnd> ends;
  take(controller, own, ends);
  return local_request_running() && running->clip >= 0;
}

void stop_local(void* controller) {
  AnimationOrder stop;
  stop.kind = AnimationOrder::Kind::Stop;
  std::vector<AnimationEnd> ends;
  take(controller, stop, ends);
}

bool local_request_running() {
  return running && running->order.request == 0 && !running->order.clip.empty();
}

std::uint64_t running_request() {
  return running ? running->order.request : 0;
}

std::optional<kcd2s::integration::ShippedClip> shipped_clip() {
  kcd2s::integration::ShippedClip shipped;
  if (!running || running->clip < 0 || !running->shipped ||
      !kcd2s::integration::shipped_clip_of(running->order.clip, shipped)) {
    return std::nullopt;
  }
  shipped.loop = running->order.loop;
  return shipped;
}

bool starting_shipped_clip() {
  return starting_shipped;
}

namespace {

// The clip's id (clip_id, else game_clip_id), with the game's code it calls kept from taking the
// engine adapter down: a fault there is -2 (the studio's whole game froze on one, live).
// A path of the game's own clips (Animations/....caf) goes by its name alone: clip_id's check of
// the found clip's file faulted in the game for one (the executioner's whipping).
int guarded_clip_id(void* animations, const std::string& clip) {
  const bool game_path = is_game_clip_path(clip);
  __try {
    if (game_path) {
      return game_clip_id(animations, clip);
    }
    const int id = clip_id(animations, clip);
    return id >= 0 ? id : game_clip_id(animations, clip);
  } __except (1) {
    return -2;
  }
}

}  // namespace

bool start_body_clip(void* entity, const std::string& clip, bool loop, unsigned layer, float from) {
  void* animations = nullptr;
  void* skeleton = skeleton_of(entity, &animations);
  if (!skeleton || !animations || clip.empty()) {
    return false;
  }
  const int id = guarded_clip_id(animations, clip);
  if (id == -2) {
    if (natives.log) {
      natives.log("ANIMATION body clip %s: the game faulted looking it up", clip.c_str());
    }
    return false;
  }
  if (id < 0) {
    if (natives.log) {
      natives.log("ANIMATION body clip %s: the body has no such clip", clip.c_str());
    }
    return false;
  }
  return start_on(skeleton, id, loop, layer, from >= 0.0f ? std::min(from, 1.0f) : -1.0f);
}

void stop_body_clip(void* entity, unsigned layer) {
  void* skeleton = skeleton_of(entity, nullptr);
  if (skeleton) {
    slot<StopLayer>(skeleton, kStopLayerSlot)(skeleton, layer, kClipBlendOut);
  }
}

int body_layer_clip(void* entity, unsigned layer) {
  void* skeleton = skeleton_of(entity, nullptr);
  const void* animation = skeleton ? newest_clip(skeleton, layer) : nullptr;
  return animation ? field<int>(animation, kAnimationClip) : -1;
}

bool is_game_clip(int clip) {
  std::lock_guard<std::mutex> lock(game_clip_ids_mutex);
  return game_clip_ids.count(clip) != 0;
}

float body_clip_seconds(void* entity, const std::string& clip) {
  void* animations = nullptr;
  if (!skeleton_of(entity, &animations) || !animations) {
    return 0.0f;
  }
  const int id = guarded_clip_id(animations, clip);
  return id >= 0 ? slot<DurationById>(animations, kDurationByIdSlot)(animations, id) : 0.0f;
}

float body_clip_time(void* entity, unsigned layer) {
  void* skeleton = natives.progress ? skeleton_of(entity, nullptr) : nullptr;
  const void* animation = skeleton ? newest_clip(skeleton, layer) : nullptr;
  return animation ? natives.progress(skeleton, animation) : -1.0f;
}

void set_body_clip_time(void* entity, unsigned layer, float time) {
  void* skeleton = skeleton_of(entity, nullptr);
  if (skeleton && newest_clip(skeleton, layer)) {
    slot<SetLayerTime>(skeleton, kSetLayerTimeSlot)(skeleton, layer, std::clamp(time, 0.0f, 1.0f));
  }
}

bool is_cuff_pose_clip(void* entity, int clip) {
  if (clip < 0) {
    return false;
  }
  void* animations = nullptr;
  if (!skeleton_of(entity, &animations) || !animations) {
    return false;
  }
  return clip_id(animations, "hands_tied_back_idle") == clip;
}

void keep_replica_clip(std::uint32_t holder, void* body,
                       const std::optional<kcd2s::integration::ShippedClip>& clip) {
  const auto found = replica_clips.find(holder);
  if (found != replica_clips.end() && clip && found->second.body == body &&
      found->second.clip == *clip) {
    return;
  }
  if (found != replica_clips.end()) {
    forget_replica_clip(holder);
  }
  if (!clip || !body) {
    return;
  }
  ReplicaClip entry;
  entry.body = body;
  entry.clip = *clip;
  void* animations = nullptr;
  void* skeleton = skeleton_of(body, &animations);
  const int id = skeleton && animations ? shipped_clip_id(animations, clip->clip) : -1;
  if (id >= 0 && start_on(skeleton, id, clip->loop)) {
    entry.skeleton = skeleton;
    entry.id = id;
  }
  if (natives.log) {
    natives.log("ANIMATION replica %u shipped clip %s %s (id %d, layer %u, loop %u)", holder,
                clip->clip.c_str(), entry.skeleton ? "plays" : "did not start", id, kClipLayer,
                unsigned(clip->loop));
  }
  replica_clips[holder] = entry;
}

std::string talking_style() {
  return talking.clip >= 0 ? talking.style : std::string();
}

void pause_talking(bool paused) {
  talking_paused = paused;
}

void keep_replica_talking(std::uint32_t holder, void* body, const std::string& style) {
  const auto found = replica_talking.find(holder);
  if (style.empty() || !body) {
    if (found != replica_talking.end()) {
      forget_replica_talking(holder);
    }
    return;
  }
  void* animations = nullptr;
  void* skeleton = skeleton_of(body, &animations);
  if (!skeleton || !animations) {
    return;
  }
  if (found != replica_talking.end() && found->second.style == style &&
      found->second.skeleton == skeleton) {
    const void* animation = newest_clip(skeleton, kTalkLayer);
    if (found->second.clip >= 0 && animation &&
        field<int>(animation, kAnimationClip) == found->second.clip) {
      return;
    }
    // A body without the clip keeps its own face; a start that failed waits a moment.
    if (found->second.clip < 0 &&
        (!found->second.retry || std::chrono::steady_clock::now() < found->second.retry_at)) {
      return;
    }
  }
  const char* clip = talk_clip_of(style);
  const int id = clip ? clip_id(animations, clip) : -1;
  const bool started = id >= 0 && start_on(skeleton, id, true, kTalkLayer);
  const bool first = found == replica_talking.end() || found->second.style != style;
  const bool was_retry = !first && found->second.clip < 0;
  if (natives.log && (first || (was_retry && started))) {
    natives.log("ANIMATION replica %u talking %s: clip %s %s (id %d, layer %u)%s", holder,
                style.c_str(), clip ? clip : "(none)", started ? "loops" : "not started", id,
                kTalkLayer, was_retry ? " on a retry" : "");
  }
  Talking entry{style, started ? id : -1, skeleton};
  // A body without the clip (id -1) is not tried again; a failed start is, a moment later.
  entry.retry = !started && id >= 0;
  entry.retry_at = std::chrono::steady_clock::now() + kTalkRetry;
  replica_talking[holder] = entry;
}

void forget_replica_talking(std::uint32_t holder) {
  const auto found = replica_talking.find(holder);
  if (found == replica_talking.end()) {
    return;
  }
  const Talking& entry = found->second;
  void* skeleton = entry.clip >= 0 ? entry.skeleton : nullptr;
  const void* animation = skeleton ? newest_clip(skeleton, kTalkLayer) : nullptr;
  if (animation && field<int>(animation, kAnimationClip) == entry.clip) {
    slot<StopLayer>(skeleton, kStopLayerSlot)(skeleton, kTalkLayer, kClipBlendOut);
  }
  replica_talking.erase(found);
}

void forget_replica_clip(std::uint32_t holder) {
  const auto found = replica_clips.find(holder);
  if (found == replica_clips.end()) {
    return;
  }
  const ReplicaClip& entry = found->second;
  // Blended out while it is still the one on the clip layer of that body.
  void* animations = nullptr;
  void* skeleton = entry.skeleton ? skeleton_of(entry.body, &animations) : nullptr;
  const void* animation = skeleton == entry.skeleton && skeleton ? newest_clip(skeleton) : nullptr;
  if (animation && field<int>(animation, kAnimationClip) == entry.id) {
    slot<StopLayer>(skeleton, kStopLayerSlot)(skeleton, kClipLayer, kClipBlendOut);
  }
  replica_clips.erase(found);
}

}  // namespace kcds::runtime::player_animation
