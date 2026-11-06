#include "female_retarget.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "upper_body.hpp"

namespace kcds::runtime::animation::female_retarget {
namespace {

// ICharacterInstance's default skeleton (as upper_body).
constexpr std::size_t kDefaultSkeletonSlot = 11;
// IDefaultSkeleton: a joint's name (slot 6), its default absolute and relative QuatT (slots 8 and
// 9, the pose data at +24) and the skeleton's file (slot 10, the string at +552).
constexpr std::size_t kJointNameSlot = 6;
constexpr std::size_t kDefaultAbsoluteSlot = 8;
constexpr std::size_t kModelPathSlot = 10;
// ISkeletonAnim PushPoseModifier for a stage of the finished pose and the stage whose
// modifiers change the pose the body is drawn with.
constexpr std::size_t kPushStageModifierSlot = 37;
constexpr int kDrawnPoseStage = 2;
constexpr char kModifierName[] = "KCDS female retarget";
// ICharacterInstance's ISkeletonAnim (slot 5) and animation set (slot 16); ISkeletonAnim: how many
// animations a layer holds; IAnimationSet's count of clips (slot 5: a clip's id is its
// place in the set); the script clip layer (as player_animation).
constexpr std::size_t kSkeletonAnimSlot = 5;
constexpr std::size_t kAnimationSetSlot = 16;
constexpr std::size_t kLayerCountSlot = 15;
constexpr std::size_t kClipCountSlot = 5;
constexpr std::uint32_t kClipLayer = 12;
// CDefaultSkeleton's joints (48 bytes each, their count in the int before the first, the parent
// at +22) and the pose arrays of SAnimationPoseModifierParams (as upper_body).
constexpr std::size_t kSkeletonJoints = 8;
constexpr std::size_t kJointSize = 48;
constexpr std::size_t kJointParent = 22;
constexpr std::uint16_t kNoParent = 0xFFFF;
constexpr int kMaxJoints = 1024;
constexpr std::size_t kParamsCharacter = 0;
constexpr std::size_t kParamsPose = 8;
constexpr std::size_t kPoseBuffer = 32;
constexpr std::size_t kPoseAbsoluteOffset = 8;
// The skeletons, by the end of their file's path.
constexpr char kPlayerFemale[] = "/kcds_player_female.chr";
constexpr char kMale[] = "/humans/male/skeleton/male.chr";
// Characters not seen for this long are forgotten.
constexpr std::chrono::seconds kForgetAfter{10};

struct QuatT {
  float q[4];
  float t[3];
};
static_assert(sizeof(QuatT) == 28, "the engine's QuatT is 28 bytes");

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index]);
}

template <class T>
T read(const void* base, std::size_t offset) {
  return *reinterpret_cast<const T*>(static_cast<const char*>(base) + offset);
}

void multiply(const float a[4], const float b[4], float out[4]) {
  const float x = a[3] * b[0] + b[3] * a[0] + (a[1] * b[2] - a[2] * b[1]);
  const float y = a[3] * b[1] + b[3] * a[1] + (a[2] * b[0] - a[0] * b[2]);
  const float z = a[3] * b[2] + b[3] * a[2] + (a[0] * b[1] - a[1] * b[0]);
  const float w = a[3] * b[3] - (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
  out[0] = x;
  out[1] = y;
  out[2] = z;
  out[3] = w;
}

void conjugate(const float q[4], float out[4]) {
  out[0] = -q[0];
  out[1] = -q[1];
  out[2] = -q[2];
  out[3] = q[3];
}

QuatT compose(const QuatT& parent, const QuatT& child) {
  QuatT out{};
  multiply(parent.q, child.q, out.q);
  const float* a = parent.q;
  const float* v = child.t;
  const float u[3] = {2.0f * (a[1] * v[2] - a[2] * v[1]), 2.0f * (a[2] * v[0] - a[0] * v[2]),
                      2.0f * (a[0] * v[1] - a[1] * v[0])};
  out.t[0] = v[0] + a[3] * u[0] + (a[1] * u[2] - a[2] * u[1]) + parent.t[0];
  out.t[1] = v[1] + a[3] * u[1] + (a[2] * u[0] - a[0] * u[2]) + parent.t[1];
  out.t[2] = v[2] + a[3] * u[2] + (a[0] * u[1] - a[1] * u[0]) + parent.t[2];
  return out;
}

QuatT* relative_joints(void* pose) {
  const auto buffer = read<std::uintptr_t>(pose, kPoseBuffer);
  return reinterpret_cast<QuatT*>((buffer + 3) & ~std::uintptr_t(3));
}

QuatT* absolute_joints(void* pose) {
  const auto buffer = read<std::uintptr_t>(pose, kPoseBuffer);
  const auto offset = read<std::uintptr_t>(pose, kPoseAbsoluteOffset);
  return reinterpret_cast<QuatT*>(offset + ((buffer + 3) & ~std::uintptr_t(3)));
}

bool path_ends_with(void* skeleton, const char* ending) {
  using Path = const char*(__fastcall*)(void* skeleton);
  const char* path = slot<Path>(skeleton, kModelPathSlot)(skeleton);
  if (!path) {
    return false;
  }
  const std::size_t length = std::strlen(path);
  const std::size_t wanted = std::strlen(ending);
  return length >= wanted && _stricmp(path + length - wanted, ending) == 0;
}

int joint_count(void* skeleton) {
  const auto joints = read<const char*>(skeleton, kSkeletonJoints);
  const int count = joints ? *reinterpret_cast<const int*>(joints - sizeof(int)) : 0;
  return count > 0 && count <= kMaxJoints ? count : 0;
}

const char* joint_name(void* skeleton, int joint) {
  using JointName = const char*(__fastcall*)(void* skeleton, std::int16_t id);
  return slot<JointName>(skeleton, kJointNameSlot)(skeleton, std::int16_t(joint));
}

const float* bind_absolute(void* skeleton, int joint) {
  using DefaultJoint = const float*(__fastcall*)(void* skeleton, std::int16_t id);
  return slot<DefaultJoint>(skeleton, kDefaultAbsoluteSlot)(skeleton, std::int16_t(joint));
}

// The male bind pose by joint name (rotations in the character's space), from the first male
// skeleton the passes show.
using MaleBind = std::unordered_map<std::string, std::array<float, 4>>;
std::atomic<bool> male_known{false};
std::mutex male_mutex;
std::shared_ptr<const MaleBind> male_bind;

void learn_male(void* skeleton) {
  std::lock_guard<std::mutex> lock(male_mutex);
  if (male_bind) {
    return;
  }
  auto bind = std::make_shared<MaleBind>();
  const int count = joint_count(skeleton);
  for (int joint = 0; joint < count; ++joint) {
    const char* name = joint_name(skeleton, joint);
    const float* absolute = bind_absolute(skeleton, joint);
    if (name && absolute) {
      (*bind)[name] = {absolute[0], absolute[1], absolute[2], absolute[3]};
    }
  }
  if (bind->empty()) {
    return;
  }
  male_bind = std::move(bind);
  male_known.store(true);
}

// One female skeleton's retarget: a parent-first order of its joints and, for each joint the male
// skeleton has too, the rotations around the clip's (left * clip * right).
struct Retarget {
  std::vector<std::uint16_t> parents;
  std::vector<int> order;
  struct Moved {
    int joint;
    float left[4];
    float right[4];
    // A joint of the scabbard's chain (kScabbardChain): its rotation at the skeleton's default, and
    // the one that default becomes.
    bool scabbard = false;
    float at_default[4];
    float as_on_a_man[4];
  };
  std::vector<Moved> moved;
};

// The joints a scabbard and the sword in it hang on. The male clips do not drive them, and the two
// skeletons set them differently: on the female one LeftWeaponSheath and LeftWeaponSlot lie 12.5
// degrees off LeftWeaponEnd and LeftWeaponSecondaryBelt, which on the male one are all parallel, so
// a holstered blade stood out beside its scabbard (two swords at her hip). They take the male
// directions, on her own positions.
const char* const kScabbardChain[] = {"LeftWeaponSecondaryBelt", "LeftWeaponSheath",
                                      "LeftWeaponEnd", "LeftWeaponSlot"};

bool in_scabbard_chain(const char* name) {
  for (const char* joint : kScabbardChain) {
    if (std::strcmp(name, joint) == 0) {
      return true;
    }
  }
  return false;
}

std::shared_ptr<const Retarget> learn_female(void* skeleton, const MaleBind& male) {
  auto learned = std::make_shared<Retarget>();
  const int count = joint_count(skeleton);
  const auto joints = read<const char*>(skeleton, kSkeletonJoints);
  if (count == 0 || !joints) {
    return nullptr;
  }
  learned->parents.resize(count);
  std::vector<const char*> names(count);
  for (int joint = 0; joint < count; ++joint) {
    const auto parent = read<std::uint16_t>(joints, joint * kJointSize + kJointParent);
    if (parent != kNoParent && parent >= count) {
      return nullptr;
    }
    learned->parents[joint] = parent;
    names[joint] = joint_name(skeleton, joint);
  }
  // From each joint up to its first placed ancestor, then back down: parents before children.
  std::vector<std::uint8_t> placed(count, 0);
  std::vector<int> chain;
  for (int joint = 0; joint < count; ++joint) {
    chain.clear();
    int at = joint;
    while (at != kNoParent && !placed[at]) {
      if (int(chain.size()) >= count) {
        return nullptr;
      }
      chain.push_back(at);
      at = learned->parents[at];
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      const int node = *it;
      placed[node] = 1;
      learned->order.push_back(node);
      const std::uint16_t parent = learned->parents[node];
      if (parent == kNoParent || !names[node] || !names[parent]) {
        continue;
      }
      const auto male_joint = male.find(names[node]);
      const auto male_parent = male.find(names[parent]);
      if (male_joint == male.end() || male_parent == male.end()) {
        continue;
      }
      Retarget::Moved entry{};
      entry.joint = node;
      float inverse[4];
      conjugate(bind_absolute(skeleton, parent), inverse);
      multiply(inverse, male_parent->second.data(), entry.left);
      conjugate(male_joint->second.data(), inverse);
      multiply(inverse, bind_absolute(skeleton, node), entry.right);
      if (in_scabbard_chain(names[node])) {
        entry.scabbard = true;
        conjugate(bind_absolute(skeleton, parent), inverse);
        multiply(inverse, bind_absolute(skeleton, node), entry.at_default);
        // Its parent's direction as this chain leaves it: the male one when the parent is in the
        // chain too, the skeleton's own otherwise.
        const float* parent_now = in_scabbard_chain(names[parent])
                                      ? male_parent->second.data()
                                      : bind_absolute(skeleton, parent);
        conjugate(parent_now, inverse);
        multiply(inverse, male_joint->second.data(), entry.as_on_a_man);
      }
      learned->moved.push_back(entry);
    }
  }
  return learned->moved.empty() ? nullptr : learned;
}

// The learned retargets, by female skeleton.
std::mutex retargets_mutex;
std::unordered_map<const void*, std::shared_ptr<const Retarget>> retargets;

std::shared_ptr<const Retarget> retarget_of(void* skeleton) {
  std::lock_guard<std::mutex> lock(retargets_mutex);
  const auto found = retargets.find(skeleton);
  if (found != retargets.end()) {
    return found->second;
  }
  std::shared_ptr<const MaleBind> male;
  {
    std::lock_guard<std::mutex> male_lock(male_mutex);
    male = male_bind;
  }
  auto learned = male ? learn_female(skeleton, *male) : nullptr;
  retargets[skeleton] = learned;
  return learned;
}

// The game's IAnimationPoseModifier (as upper_body's LegModifier lays it out).
class RetargetModifier {
 public:
  virtual void* query_interface(const void* /*interface_id*/) const {
    return const_cast<RetargetModifier*>(this);
  }
  virtual void* query_composite(const char* /*name*/) const {
    return nullptr;
  }
  virtual void* factory() const {
    return nullptr;
  }
  virtual ~RetargetModifier() = default;
  virtual bool prepare(const void* /*params*/) {
    return true;
  }
  virtual bool execute(const void* params) {
    void* character = read<void*>(params, kParamsCharacter);
    void* pose = read<void*>(params, kParamsPose);
    if (!character || !pose) {
      return false;
    }
    using Getter = void*(__fastcall*)(void* object);
    void* skeleton = slot<Getter>(character, kDefaultSkeletonSlot)(character);
    const auto retarget = skeleton ? retarget_of(skeleton) : nullptr;
    if (!retarget || int(retarget->parents.size()) != joint_count(skeleton)) {
      return false;
    }
    QuatT* relative = relative_joints(pose);
    for (const auto& entry : retarget->moved) {
      float* rotation = relative[entry.joint].q;
      if (entry.scabbard) {
        // What moves it (its sway as she walks) stays, from the male direction.
        float inverse[4];
        float moved[4];
        conjugate(entry.at_default, inverse);
        multiply(inverse, rotation, moved);
        multiply(entry.as_on_a_man, moved, rotation);
        continue;
      }
      float step[4];
      multiply(entry.left, rotation, step);
      multiply(step, entry.right, rotation);
    }
    QuatT* absolute = absolute_joints(pose);
    for (const int joint : retarget->order) {
      const std::uint16_t parent = retarget->parents[joint];
      absolute[joint] =
          parent == kNoParent ? relative[joint] : compose(absolute[parent], relative[joint]);
    }
    return true;
  }
  virtual void synchronize() {
  }
  virtual void memory_usage(void* /*sizer*/) const {
  }
};

// One modifier per female character, while its passes run.
struct Body {
  std::shared_ptr<RetargetModifier> modifier;
  std::chrono::steady_clock::time_point seen;
};
std::mutex bodies_mutex;
std::unordered_map<void*, Body> bodies;
Logger log = nullptr;

std::shared_ptr<RetargetModifier> modifier_for(void* character) {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(bodies_mutex);
  if (bodies.size() > 256) {
    for (auto it = bodies.begin(); it != bodies.end();) {
      it = now - it->second.seen > kForgetAfter ? bodies.erase(it) : std::next(it);
    }
  }
  Body& body = bodies[character];
  if (!body.modifier) {
    body.modifier = std::make_shared<RetargetModifier>();
    if (log) {
      log("ANIMATION female retarget on character %p", character);
    }
  }
  body.seen = now;
  return body.modifier;
}

void push_stage(void* animation, const std::shared_ptr<RetargetModifier>& modifier) {
  using Push = bool(__fastcall*)(void* animation, int stage, void* modifier, const char* name);
  alignas(std::shared_ptr<RetargetModifier>) unsigned char
      storage[sizeof(std::shared_ptr<RetargetModifier>)];
  // The push takes the std::shared_ptr by value: the copy is the callee's to let go.
  auto* copy = new (storage) std::shared_ptr<RetargetModifier>(modifier);
  slot<Push>(animation, kPushStageModifierSlot)(animation, kDrawnPoseStage, copy, kModifierName);
}

// How many clips the male set has: a player woman's set is that list, then the female clips, so an
// id from here on is a female clip. -1 until a man's body was seen.
std::atomic<int> male_clip_count{-1};
// The bodies whose script clip layer last started a female clip.
std::mutex female_clips_mutex;
std::unordered_set<void*> female_clips;

// Whether this character plays a female clip now. Runs on the game's animation threads, so it does
// not look at the clip itself, which the engine thread may be replacing: note_clip_start knows
// which one started, and an empty layer means it is over.
bool plays_female_clip(void* character) {
  using Getter = void*(__fastcall*)(void* object);
  using LayerCount = unsigned(__fastcall*)(void*, std::uint32_t);
  void* skeleton = slot<Getter>(character, kSkeletonAnimSlot)(character);
  if (!skeleton) {
    return false;
  }
  std::lock_guard<std::mutex> lock(female_clips_mutex);
  const auto found = female_clips.find(skeleton);
  if (found == female_clips.end()) {
    return false;
  }
  if (slot<LayerCount>(skeleton, kLayerCountSlot)(skeleton, kClipLayer) == 0) {
    female_clips.erase(found);
    return false;
  }
  return true;
}

void on_pass(void* character, void* animation) {
  using Getter = void*(__fastcall*)(void* object);
  void* skeleton = slot<Getter>(character, kDefaultSkeletonSlot)(character);
  if (!skeleton) {
    return;
  }
  if (!male_known.load(std::memory_order_relaxed)) {
    if (path_ends_with(skeleton, kMale)) {
      learn_male(skeleton);
      using ClipCount = unsigned(__fastcall*)(void* set);
      void* set = slot<Getter>(character, kAnimationSetSlot)(character);
      if (set) {
        male_clip_count.store(int(slot<ClipCount>(set, kClipCountSlot)(set)));
      }
    }
    return;
  }
  if (path_ends_with(skeleton, kPlayerFemale) && !plays_female_clip(character)) {
    push_stage(animation, modifier_for(character));
  }
}

}  // namespace

void note_clip_start(void* skeleton, int clip, unsigned layer) {
  if (layer != kClipLayer) {
    return;
  }
  const int male_clips = male_clip_count.load();
  std::lock_guard<std::mutex> lock(female_clips_mutex);
  if (male_clips >= 0 && clip >= male_clips) {
    female_clips.insert(skeleton);
  } else {
    female_clips.erase(skeleton);
  }
}

void install(Logger logger) {
  log = logger;
  upper_body::add_pass_listener(&on_pass);
}

}  // namespace kcds::runtime::animation::female_retarget
