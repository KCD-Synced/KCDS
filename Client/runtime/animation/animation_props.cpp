#include "animation_props.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "MinHook.h"
#include "kcd2s/bridge/sigscan.hpp"
#include "kcd2s/integration/animation_props.hpp"

#include "../props/pose_math.hpp"
#include "../props/prop_entity.hpp"

namespace kcds::runtime::animation_props {
namespace {

using kcd2s::integration::AnimationProp;

// The item class's model and material (CryStrings: a pointer to the text).
constexpr std::size_t kClassModel = 0x38;
constexpr std::size_t kClassMaterial = 0x40;
constexpr char kObjectsFolder[] = "Objects/";
constexpr char kRightHand[] = "RightHand";
constexpr char kLeftHand[] = "LeftHand";
// The hand slot joints the game hangs held items on (attach: 'game').
constexpr char kRightHandSlot[] = "RightHandSlot";
constexpr char kLeftHandSlot[] = "LeftHandSlot";
// How many frames a spawned prop may take to be there before its attach is given up.
constexpr int kAttachFrames = 300;

// SChildAttachParams: flag 4 attaches to a joint of the parent's character by name.
struct AttachParams {
  std::int32_t flags;
  const char* bone;
};
constexpr std::int32_t kAttachToJoint = 4;
using AttachChild = bool(__fastcall*)(void* parent, void* child, const AttachParams* params);

// The game's animation events: CSkeletonAnim's own slot 46 hands every
// event of a clip that plays on a character to the character's event callback. The event's data
// (CAnimEventData) holds its name at + 16 and its parameter at + 24 ("<action>,<slot>,<hand>" for
// a pickup, as Animations/humans/*/*.animevents write them).
constexpr char kSkeletonAnimType[] = ".?AVCSkeletonAnim@@";
constexpr std::size_t kFireEventSlot = 46;
constexpr std::size_t kEventName = 16;
constexpr std::size_t kEventParameter = 24;
using FireEvent = std::int64_t(__fastcall*)(void* skeleton, void* path, void* attachment, int clip,
                                            void* event, int queue, int priority);
FireEvent original_fire = nullptr;
// The cues kept between two frames at most (a body fires a handful).
constexpr std::size_t kCuesMax = 256;

// What an animation event does with the item in a hand.
enum class Cue : std::uint8_t {
  None,
  Take,     // pickup, spawnitem, attach: the hand takes the item
  PutDown,  // place, despawnitem, detach: the hand lets it go
};

// Which hand an event names: both when it names none.
enum class Hand : std::uint8_t {
  Both,
  Right,
  Left,
};

struct HandCue {
  void* skeleton = nullptr;
  Cue cue = Cue::None;
  Hand hand = Hand::Both;
  std::string event;
};

// The skeletons of the bodies holding a prop that follows its animation, and the events their
// animations fired since the last frame. The game may fire them off the engine thread.
std::mutex cues_lock;
std::unordered_map<void*, int> watched;
std::vector<HandCue> cues;

// The last take or put-down of each hand of every body, for a while: another player's request
// (its field) arrives a moment after their game started the animation, and the animation's pickup
// may already have fired by then on this client's copy of the body.
struct LastCue {
  Cue cue = Cue::None;
  ULONGLONG at = 0;
};
struct HandsCue {
  LastCue right;
  LastCue left;
};
std::unordered_map<void*, HandsCue> recent;
constexpr ULONGLONG kRecentMs = 3000;
// Recent entries are swept once this many bodies are kept.
constexpr std::size_t kRecentSweep = 512;

void remember(void* skeleton, Cue cue, Hand hand) {
  const ULONGLONG now = GetTickCount64();
  if (recent.size() >= kRecentSweep) {
    std::erase_if(recent, [&](const auto& entry) {
      return now - entry.second.right.at > kRecentMs && now - entry.second.left.at > kRecentMs;
    });
  }
  HandsCue& hands = recent[skeleton];
  if (hand != Hand::Left) {
    hands.right = {cue, now};
  }
  if (hand != Hand::Right) {
    hands.left = {cue, now};
  }
}

struct Spawned {
  std::string name;
  std::size_t prop = 0;  // its index in the field's props
  bool attached = false;
  int frames_waited = 0;
};

struct Shown {
  std::string field;
  void* body = nullptr;
  void* skeleton = nullptr;  // watched while a prop follows the animation
  std::vector<AnimationProp> props;
  std::vector<bool> held;  // whether each prop is in its hand
  std::vector<Spawned> spawned;
};

Natives natives;
std::unordered_map<std::uint32_t, Shown> shown;
std::vector<OwnCue> own_cues;
// Every spawn has a new name: the entity system removes an entity at the end of the frame, so a
// prop spawned under the name of one removed in the same frame would be found and lost with it.
unsigned generation = 0;

void say(const char* format, std::uint32_t holder, const char* text) {
  if (natives.log) {
    natives.log(format, holder, text);
  }
}

template <class T>
T read_at(const void* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
  return value;
}

bool same_text(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

Cue cue_of(std::string_view event) {
  constexpr const char* takes[] = {"pickup",      "spawnitem",  "attach",
                                   "attachright", "attachleft", "attachobject"};
  constexpr const char* put_downs[] = {"place", "despawnitem", "detach", "detachobject"};
  for (const char* name : takes) {
    if (same_text(event, name)) {
      return Cue::Take;
    }
  }
  for (const char* name : put_downs) {
    if (same_text(event, name)) {
      return Cue::PutDown;
    }
  }
  return Cue::None;
}

// The hand an event names: AttachRight and AttachLeft by their name, the others by a "Right" or
// "Left" among their parameter's comma-separated words.
Hand hand_of(std::string_view event, const char* parameter) {
  if (same_text(event, "attachright")) {
    return Hand::Right;
  }
  if (same_text(event, "attachleft")) {
    return Hand::Left;
  }
  std::string_view rest = parameter ? std::string_view(parameter) : std::string_view();
  while (!rest.empty()) {
    const std::size_t comma = rest.find(',');
    const std::string_view word = rest.substr(0, comma);
    if (same_text(word, "right")) {
      return Hand::Right;
    }
    if (same_text(word, "left")) {
      return Hand::Left;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    rest.remove_prefix(comma + 1);
  }
  return Hand::Both;
}

std::int64_t __fastcall hooked_fire(void* skeleton, void* path, void* attachment, int clip,
                                    void* event, int queue, int priority) {
  if (event) {
    const char* name = read_at<const char*>(event, kEventName);
    const Cue cue = name ? cue_of(name) : Cue::None;
    if (cue != Cue::None) {
      std::lock_guard<std::mutex> hold(cues_lock);
      const Hand hand = hand_of(name, read_at<const char*>(event, kEventParameter));
      remember(skeleton, cue, hand);
      if (watched.count(skeleton) != 0 && cues.size() < kCuesMax) {
        cues.push_back({skeleton, cue, hand, name});
      }
    }
  }
  return original_fire(skeleton, path, attachment, clip, event, queue, priority);
}

void watch(Shown& entry, void* skeleton) {
  entry.skeleton = skeleton;
  if (!skeleton) {
    return;
  }
  std::lock_guard<std::mutex> hold(cues_lock);
  ++watched[skeleton];
}

void unwatch(Shown& entry) {
  if (!entry.skeleton) {
    return;
  }
  std::lock_guard<std::mutex> hold(cues_lock);
  const auto found = watched.find(entry.skeleton);
  if (found != watched.end() && --found->second <= 0) {
    watched.erase(found);
    std::erase_if(cues, [&](const HandCue& cue) { return cue.skeleton == entry.skeleton; });
  }
  entry.skeleton = nullptr;
}

// Whether a hand of the skeleton took its item in the last kRecentMs and has not let it go.
bool took_recently(void* skeleton, bool left_hand) {
  std::lock_guard<std::mutex> hold(cues_lock);
  const auto found = recent.find(skeleton);
  if (found == recent.end()) {
    return false;
  }
  const LastCue& last = left_hand ? found->second.left : found->second.right;
  return last.cue == Cue::Take && GetTickCount64() - last.at <= kRecentMs;
}

// The events a skeleton's animations fired since the last frame, in their order.
std::vector<HandCue> take_cues(void* skeleton) {
  std::vector<HandCue> taken;
  std::lock_guard<std::mutex> hold(cues_lock);
  for (const HandCue& cue : cues) {
    if (cue.skeleton == skeleton) {
      taken.push_back(cue);
    }
  }
  std::erase_if(cues, [&](const HandCue& cue) { return cue.skeleton == skeleton; });
  return taken;
}

const char* class_text(const void* item_class, std::size_t offset) {
  const char* text = nullptr;
  std::memcpy(&text, static_cast<const std::uint8_t*>(item_class) + offset, sizeof(text));
  return text ? text : "";
}

// A path under Objects/ as the game keeps it in its paks.
std::string objects_path(const char* path) {
  const std::string text = path;
  if (_strnicmp(text.c_str(), kObjectsFolder, sizeof(kObjectsFolder) - 1) == 0) {
    return text;
  }
  return kObjectsFolder + text;
}

// The mesh file and material that show a prop; false for an item class this game does not have
// or one without a model.
bool mesh_of(const AnimationProp& prop, std::string& file, std::string& material) {
  material.clear();
  if (!prop.item) {
    file = prop.model;
    return true;
  }
  void* item_class = natives.item_class ? natives.item_class(*prop.item) : nullptr;
  if (!item_class) {
    return false;
  }
  const char* model = class_text(item_class, kClassModel);
  if (model[0] == '\0') {
    return false;
  }
  file = objects_path(model);
  const std::size_t slash = file.rfind('/');
  if (file.find('.', slash == std::string::npos ? 0 : slash) == std::string::npos) {
    file += ".cgf";
  }
  const char* class_material = class_text(item_class, kClassMaterial);
  if (class_material[0] != '\0') {
    material = objects_path(class_material);
  }
  return true;
}

// A prop that rests in the world before and after the animation: the server Prop
// (placed_props' kcds_prop_<id>) hidden while the animation holds the item, shown again after.
void show_rest(const AnimationProp& prop, bool shown_in_world) {
  if (prop.rest == 0) {
    return;
  }
  void* resting = props::entity::find("kcds_prop_" + std::to_string(prop.rest));
  if (resting) {
    props::entity::hide(resting, !shown_in_world);
  }
}

void remove_props(Shown& entry) {
  for (const auto& prop : entry.spawned) {
    props::entity::remove(prop.name);
  }
  entry.spawned.clear();
  for (const AnimationProp& prop : entry.props) {
    show_rest(prop, true);
  }
}

// The body's joint a script named. The game's skeleton names its joints without
// CryEngine's biped prefix ("Head", "Neck", "RightHand"), while scripts often use the biped names
// ("Bip01 Head", "Bip01 R Hand"): a name the body lacks is tried that way, and else the body's
// joints are logged once, so the script's author sees the right ones (an unknown joint hangs the
// prop at the body's feet).
std::string body_joint(void* body, const std::string& wanted) {
  const std::string name = props::entity::body_joint_name(body, wanted);
  if (!name.empty()) {
    if (name != wanted && natives.log) {
      natives.log("ANIMATION PROPS joint %s is the body's %s", wanted.c_str(), name.c_str());
    }
    return name;
  }
  if (natives.log) {
    natives.log("ANIMATION PROPS no joint %s on the body", wanted.c_str());
  }
  static bool listed = false;
  if (listed || !natives.log) {
    return wanted;
  }
  listed = true;
  // The body's own joints, a line of them at a time; the face's poses and rig, the blend and IK
  // helpers and the alignment joints are left out.
  std::string names;
  for (const std::string& joint : props::entity::joint_names(body)) {
    if (joint.rfind("FA_", 0) == 0 || joint.rfind("blend_", 0) == 0 ||
        joint.find("_IK") != std::string::npos || joint.rfind("Align", 0) == 0 ||
        joint.find("_SSDR_") != std::string::npos || joint.find("tongue") != std::string::npos) {
      continue;
    }
    names += names.empty() ? joint : ", " + joint;
    if (names.size() > 400) {
      natives.log("ANIMATION PROPS the body's joints: %s", names.c_str());
      names.clear();
    }
  }
  if (!names.empty()) {
    natives.log("ANIMATION PROPS the body's joints: %s", names.c_str());
  }
  return wanted;
}

// Puts prop `index` in its hand: spawned now, hung from its joint once it is there.
void take(std::uint32_t holder, Shown& entry, std::size_t index) {
  entry.held[index] = true;
  std::string file;
  std::string material;
  const AnimationProp& prop = entry.props[index];
  show_rest(prop, false);
  if (!mesh_of(prop, file, material)) {
    say("ANIMATION PROPS holder=%u has no model for %s", holder,
        prop.item ? kcd2s::integration::guid_text(*prop.item).c_str() : "");
    return;
  }
  float position[3];
  props::entity::world_position(entry.body, position);
  char name[64];
  std::snprintf(name, sizeof(name), "kcds_animation_prop_%u_%u", holder, ++generation);
  if (!props::entity::spawn_mesh(name, position, file, material)) {
    say("ANIMATION PROPS holder=%u could not show %s", holder, file.c_str());
    return;
  }
  say("ANIMATION PROPS holder=%u shows %s", holder, file.c_str());
  entry.spawned.push_back({name, index});
}

// Takes prop `index` out of its hand.
void put_down(Shown& entry, std::size_t index) {
  entry.held[index] = false;
  show_rest(entry.props[index], true);
  for (const auto& prop : entry.spawned) {
    if (prop.prop == index) {
      props::entity::remove(prop.name);
    }
  }
  std::erase_if(entry.spawned, [&](const Spawned& prop) { return prop.prop == index; });
}

// Hangs the spawned props that are there from their joints; one that never comes is given up.
void attach_props(std::uint32_t holder, Shown& entry) {
  if (!natives.attach_child) {
    return;
  }
  for (auto& spawned : entry.spawned) {
    if (spawned.attached) {
      continue;
    }
    void* prop = props::entity::find(spawned.name);
    if (!prop) {
      if (++spawned.frames_waited > kAttachFrames) {
        say("ANIMATION PROPS holder=%u prop never came: %s", holder, spawned.name.c_str());
        props::entity::remove(spawned.name);
        spawned.attached = true;
      }
      continue;
    }
    const AnimationProp& wanted = entry.props[spawned.prop];
    const std::string joint_name = !wanted.joint.empty() ? body_joint(entry.body, wanted.joint)
                                   : wanted.left_hand    ? std::string(kLeftHand)
                                                         : std::string(kRightHand);
    const char* joint = joint_name.c_str();
    const AttachParams params{kAttachToJoint, joint};
    reinterpret_cast<AttachChild>(natives.attach_child)(entry.body, prop, &params);
    std::array<float, 3> offset = wanted.position.value_or(std::array<float, 3>{});
    std::array<float, 4> rotation =
        wanted.rotation.value_or(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});
    // The game's grip: on a hand's slot joint the game hangs an item by its model's
    // grip helper, slt_0 for the right hand and slt_1 for the left (the attach request of its
    // hand content), so the helper, not the model's pivot, sits on the slot. The
    // script's position and rotation turn and move it from there.
    if (wanted.joint == kRightHandSlot || wanted.joint == kLeftHandSlot) {
      float helper_position[3];
      float helper_rotation[4];
      const char* helper = wanted.joint == kRightHandSlot ? "slt_0" : "slt_1";
      if (props::entity::helper_pose(prop, helper, helper_position, helper_rotation)) {
        float inverse_rotation[4];
        pose_math::conjugate(helper_rotation, inverse_rotation);
        float inverse_position[3];
        pose_math::rotate(inverse_rotation, helper_position, inverse_position);
        for (float& axis : inverse_position) {
          axis = -axis;
        }
        float position[3];
        float turn[4];
        pose_math::compose(offset.data(), rotation.data(), inverse_position, inverse_rotation,
                           position, turn);
        offset = {position[0], position[1], position[2]};
        rotation = {turn[0], turn[1], turn[2], turn[3]};
      }
    }
    props::entity::place_rotated(prop, offset.data(), rotation.data());
    say("ANIMATION PROPS holder=%u attached at %s", holder, joint);
    spawned.attached = true;
  }
}

bool follows_animation(const Shown& entry) {
  for (const auto& prop : entry.props) {
    if (prop.from_animation || prop.until_put_down) {
      return true;
    }
  }
  return false;
}

// The props that follow the animation go into their hands and out of them as its events say.
void follow_animation(std::uint32_t holder, Shown& entry) {
  if (!follows_animation(entry)) {
    return;
  }
  void* skeleton = props::entity::skeleton_anim_of(entry.body);
  if (skeleton != entry.skeleton) {
    unwatch(entry);
    watch(entry, skeleton);
    return;
  }
  for (const HandCue& cue : take_cues(skeleton)) {
    for (std::size_t i = 0; i < entry.props.size(); ++i) {
      const AnimationProp& prop = entry.props[i];
      const bool hand = cue.hand == Hand::Both || (cue.hand == Hand::Left) == prop.left_hand;
      if ((!prop.from_animation && !prop.until_put_down) || !hand) {
        continue;
      }
      // A prop held until the put-down is not taken again once the animation has let it go.
      if (cue.cue == Cue::Take && !entry.held[i] && prop.from_animation) {
        say("ANIMATION PROPS holder=%u the animation takes it (%s)", holder, cue.event.c_str());
        take(holder, entry, i);
        if (holder == kLocalHolder && own_cues.size() < 16) {
          own_cues.push_back({std::uint8_t(i), true});
        }
      } else if (cue.cue == Cue::PutDown && entry.held[i]) {
        say("ANIMATION PROPS holder=%u the animation puts it down (%s)", holder, cue.event.c_str());
        put_down(entry, i);
        if (holder == kLocalHolder && own_cues.size() < 16) {
          own_cues.push_back({std::uint8_t(i), false});
        }
      }
    }
  }
}

void hook_events() {
  if (!natives.image || !natives.log) {
    return;
  }
  const auto base = reinterpret_cast<std::uintptr_t>(natives.image);
  const std::size_t at =
      kcd2s::bridge::rtti_vtable(natives.image, natives.image_size, base, kSkeletonAnimType);
  if (at == kcd2s::bridge::knpos) {
    natives.log("ANIMATION PROPS the skeleton's vtable is missing");
    return;
  }
  void* target = read_at<void*>(natives.image, at + kFireEventSlot * sizeof(void*));
  if (!target ||
      MH_CreateHook(target, reinterpret_cast<void*>(&hooked_fire),
                    reinterpret_cast<void**>(&original_fire)) != MH_OK ||
      MH_EnableHook(target) != MH_OK) {
    natives.log("ANIMATION PROPS could not hook the animation events %p", target);
    return;
  }
  natives.log("ANIMATION PROPS animation events %p hooked", target);
}

}  // namespace

void install(const Natives& given) {
  natives = given;
  hook_events();
}

bool item_mesh(const kcd2s::integration::Guid& item, std::string& file, std::string& material) {
  AnimationProp prop;
  prop.item = item;
  return mesh_of(prop, file, material);
}

void keep(std::uint32_t holder, void* body, const std::string& field) {
  if (!body) {
    forget(holder);
    return;
  }
  auto found = shown.find(holder);
  if (found == shown.end()) {
    if (field.empty()) {
      return;
    }
    found = shown.emplace(holder, Shown{}).first;
  }
  Shown& entry = found->second;
  if (entry.field != field || entry.body != body) {
    remove_props(entry);
    unwatch(entry);
    if (field.empty()) {
      shown.erase(found);
      return;
    }
    entry.field = field;
    entry.body = body;
    if (!kcd2s::integration::parse_animation_props_field(field, entry.props)) {
      say("ANIMATION PROPS holder=%u field refused: %s", holder, field.c_str());
      entry.props.clear();
    }
    entry.held.assign(entry.props.size(), false);
    // A prop that follows the animation waits for the animation to take it, unless it just did.
    void* skeleton = follows_animation(entry) ? props::entity::skeleton_anim_of(body) : nullptr;
    watch(entry, skeleton);
    if (skeleton && natives.log) {
      natives.log("ANIMATION PROPS holder=%u follows the animation of skeleton %p", holder,
                  skeleton);
    }
    for (std::size_t i = 0; i < entry.props.size(); ++i) {
      const AnimationProp& prop = entry.props[i];
      if (!prop.from_animation || took_recently(skeleton, prop.left_hand)) {
        take(holder, entry, i);
      }
    }
    return;
  }
  follow_animation(holder, entry);
  attach_props(holder, entry);
}

void forget(std::uint32_t holder) {
  const auto found = shown.find(holder);
  if (found == shown.end()) {
    return;
  }
  remove_props(found->second);
  unwatch(found->second);
  shown.erase(found);
}

std::vector<OwnCue> take_own_cues() {
  return std::exchange(own_cues, {});
}

void reset() {
  own_cues.clear();
  for (auto& [holder, entry] : shown) {
    unwatch(entry);
  }
  shown.clear();
}

}  // namespace kcds::runtime::animation_props
