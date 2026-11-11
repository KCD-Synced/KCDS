#include "pointing.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kcd2s/integration/animation_props.hpp"
#include "kcd2s/integration/player_animation.hpp"
#include "player_animation.hpp"
#include "quick_access_key.hpp"
#include "upper_body.hpp"

namespace kcds::runtime::animation::pointing {
namespace {

// The game's quick-access menu on the pointing key, and whether this module turned it off.
constexpr char kQuickAccessMap[] = "qam_init";
quick_access_key::State quick_access;

std::uint64_t now_ms() {
  return std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count());
}

using kcd2s::integration::AnimationOrder;

// The game's pointing pose: the crowd's clip, held where the arm is out and the finger points
// (0.5 s in; at 0.8 s the arm is down again).
constexpr char kPointClip[] = "crowd_peasant_male_audience_point_03";
constexpr float kPointTime = 0.5f;
// The arm points at the camera's line of sight this far ahead (metres); the mark carries it in
// steps of kMarkStep.
constexpr float kTargetDistance = 20.0f;
constexpr float kMarkStep = 0.1f;
// The joints: the forearm (its parent, the upper arm, is what turns) and the tip of the index
// finger.
constexpr char kForeArm[] = "RightForeArm";
constexpr char kFingerTip[] = "RightHandIndex3";
// CEntity GetCharacter (slot 101, character slot 0) and GetWorldTM (vtable + 256); the character's
// default skeleton (slot 11) and its GetJointIDByName (slot 7); ISkeletonAnim's PushPoseModifier
// for a stage of the finished pose (slot 37), the drawn pose's stage 2 (as upper_body).
constexpr std::size_t kGetCharacterSlot = 101;
constexpr std::size_t kGetWorldTM = 256;
constexpr std::size_t kDefaultSkeletonSlot = 11;
constexpr std::size_t kJointIdByNameSlot = 7;
constexpr std::size_t kPushStageModifierSlot = 37;
constexpr int kDrawnPoseStage = 2;
constexpr char kModifierName[] = "KCDS pointing";
// CDefaultSkeleton's joints (48 bytes each, their count before the first, the parent at + 22) and
// the pose's joint arrays, as upper_body reads them.
constexpr std::size_t kSkeletonJoints = 8;
constexpr std::size_t kJointSize = 48;
constexpr std::size_t kJointParent = 22;
constexpr std::uint16_t kNoParent = 0xFFFF;
constexpr int kMaxJoints = 1024;
constexpr std::size_t kParamsCharacter = 0;
constexpr std::size_t kParamsPose = 8;
constexpr std::size_t kPoseBuffer = 32;
constexpr std::size_t kPoseAbsoluteOffset = 8;
// A target not given again for this long is let go (the pass stops pushing the modifier).
constexpr std::chrono::milliseconds kTargetFor{250};

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

// a * b.
void multiply(const float a[4], const float b[4], float out[4]) {
  out[0] = a[3] * b[0] + b[3] * a[0] + (a[1] * b[2] - a[2] * b[1]);
  out[1] = a[3] * b[1] + b[3] * a[1] + (a[2] * b[0] - a[0] * b[2]);
  out[2] = a[3] * b[2] + b[3] * a[2] + (a[0] * b[1] - a[1] * b[0]);
  out[3] = a[3] * b[3] - (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
}

// parent * child: the child's joint in the parent's space.
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

bool normalize(float v[3]) {
  const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (!(length > 1e-5f)) {
    return false;
  }
  v[0] /= length;
  v[1] /= length;
  v[2] /= length;
  return true;
}

// The shortest rotation taking unit vector `from` onto unit vector `to`; false when they point
// opposite ways.
bool rotation_between(const float from[3], const float to[3], float out[4]) {
  const float dot = from[0] * to[0] + from[1] * to[1] + from[2] * to[2];
  if (dot < -0.999f) {
    return false;
  }
  out[0] = from[1] * to[2] - from[2] * to[1];
  out[1] = from[2] * to[0] - from[0] * to[2];
  out[2] = from[0] * to[1] - from[1] * to[0];
  out[3] = 1.0f + dot;
  const float length =
      std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
  for (int i = 0; i < 4; ++i) {
    out[i] /= length;
  }
  return true;
}

// The game's IAnimationPoseModifier, as its feet lock lays it out (see upper_body.cpp). One per
// character: it turns the upper arm toward the target the engine thread last gave it.
class PointModifier {
 public:
  virtual void* query_interface(const void* /*interface_id*/) const {
    return const_cast<PointModifier*>(this);
  }
  virtual void* query_composite(const char* /*name*/) const {
    return nullptr;
  }
  virtual void* factory() const {
    return nullptr;
  }
  virtual ~PointModifier() = default;
  virtual bool prepare(const void* /*params*/) {
    return true;
  }
  virtual bool execute(const void* params) {
    void* character = read<void*>(params, kParamsCharacter);
    void* pose = read<void*>(params, kParamsPose);
    float target[3];
    {
      std::lock_guard<std::mutex> lock(mutex_);
      target[0] = target_[0];
      target[1] = target_[1];
      target[2] = target_[2];
    }
    using Getter = void*(__fastcall*)(void* object);
    void* skeleton = character ? slot<Getter>(character, kDefaultSkeletonSlot)(character) : nullptr;
    if (!skeleton || !pose || !learn(skeleton)) {
      return false;
    }
    const auto buffer = read<std::uintptr_t>(pose, kPoseBuffer);
    const auto offset = read<std::uintptr_t>(pose, kPoseAbsoluteOffset);
    QuatT* relative = reinterpret_cast<QuatT*>((buffer + 3) & ~std::uintptr_t(3));
    QuatT* absolute = reinterpret_cast<QuatT*>(offset + ((buffer + 3) & ~std::uintptr_t(3)));
    const QuatT& shoulder = absolute[upper_];
    float along[3] = {absolute[tip_].t[0] - shoulder.t[0], absolute[tip_].t[1] - shoulder.t[1],
                      absolute[tip_].t[2] - shoulder.t[2]};
    float wanted[3] = {target[0] - shoulder.t[0], target[1] - shoulder.t[1],
                       target[2] - shoulder.t[2]};
    float turn[4];
    if (!normalize(along) || !normalize(wanted) || !rotation_between(along, wanted, turn)) {
      return false;
    }
    float turned[4];
    multiply(turn, absolute[upper_].q, turned);
    const std::uint16_t parent = parents_[upper_];
    if (parent != kNoParent) {
      const float* p = absolute[parent].q;
      const float inverse[4] = {-p[0], -p[1], -p[2], p[3]};
      multiply(inverse, turned, relative[upper_].q);
    }
    for (int i = 0; i < 4; ++i) {
      absolute[upper_].q[i] = turned[i];
    }
    for (const int joint : below_) {
      absolute[joint] = compose(absolute[parents_[joint]], relative[joint]);
    }
    return true;
  }
  virtual void synchronize() {
  }
  virtual void memory_usage(void* /*sizer*/) const {
  }

  void aim(const float target[3]) {
    std::lock_guard<std::mutex> lock(mutex_);
    target_[0] = target[0];
    target_[1] = target[1];
    target_[2] = target[2];
  }

 private:
  // The upper arm, the finger tip, the parents and the joints under the upper arm (parents first),
  // once per skeleton.
  bool learn(void* skeleton) {
    if (skeleton == skeleton_) {
      return upper_ >= 0;
    }
    skeleton_ = skeleton;
    upper_ = -1;
    below_.clear();
    const auto joints = read<const char*>(skeleton, kSkeletonJoints);
    if (!joints) {
      return false;
    }
    const int count = *reinterpret_cast<const int*>(joints - sizeof(int));
    using JointId = std::int16_t(__fastcall*)(void* skeleton, const char* name);
    const int fore_arm = slot<JointId>(skeleton, kJointIdByNameSlot)(skeleton, kForeArm);
    tip_ = slot<JointId>(skeleton, kJointIdByNameSlot)(skeleton, kFingerTip);
    if (count <= 0 || count > kMaxJoints || fore_arm < 0 || fore_arm >= count || tip_ < 0 ||
        tip_ >= count) {
      return false;
    }
    parents_.resize(count);
    for (int joint = 0; joint < count; ++joint) {
      parents_[joint] = read<std::uint16_t>(joints, joint * kJointSize + kJointParent);
      if (parents_[joint] != kNoParent && parents_[joint] >= count) {
        return false;
      }
    }
    const int upper = parents_[fore_arm];
    if (upper == kNoParent) {
      return false;
    }
    // The joints under the upper arm, nearest first, so a parent is placed before its children.
    std::vector<std::pair<int, int>> under;
    for (int joint = 0; joint < count; ++joint) {
      int depth = 0;
      for (int at = parents_[joint]; at != kNoParent && depth <= count; at = parents_[at]) {
        ++depth;
        if (at == upper) {
          under.emplace_back(depth, joint);
          break;
        }
      }
    }
    std::sort(under.begin(), under.end());
    for (const auto& [depth, joint] : under) {
      below_.push_back(joint);
    }
    upper_ = upper;
    return true;
  }

  std::mutex mutex_;
  float target_[3] = {};
  const void* skeleton_ = nullptr;
  int upper_ = -1;
  int tip_ = -1;
  std::vector<std::uint16_t> parents_;
  std::vector<int> below_;
};

struct Aimed {
  std::shared_ptr<PointModifier> modifier;
  std::chrono::steady_clock::time_point given;
};

Natives natives;
std::mutex aimed_mutex;
// By character: the modifiers the passes push while their targets are fresh.
std::unordered_map<void*, Aimed> aimed;
// The local pointing: whether it runs, and its target in the world.
bool pointing = false;
float local_target[3] = {};

void* character_of(void* entity) {
  using GetCharacter = void*(__fastcall*)(void* entity, int slot);
  return entity ? slot<GetCharacter>(entity, kGetCharacterSlot)(entity, 0) : nullptr;
}

// The body's character aims at a point in the world from now on (its character's space: the
// entity's, as the character sits at the entity's origin).
void aim(void* entity, const float world[3]) {
  void* character = character_of(entity);
  if (!character) {
    return;
  }
  using GetWorldTM = const float*(__fastcall*)(void* entity);
  const float* m = slot<GetWorldTM>(entity, kGetWorldTM / sizeof(void*))(entity);
  const float d[3] = {world[0] - m[3], world[1] - m[7], world[2] - m[11]};
  const float local[3] = {m[0] * d[0] + m[4] * d[1] + m[8] * d[2],
                          m[1] * d[0] + m[5] * d[1] + m[9] * d[2],
                          m[2] * d[0] + m[6] * d[1] + m[10] * d[2]};
  std::lock_guard<std::mutex> lock(aimed_mutex);
  Aimed& entry = aimed[character];
  if (!entry.modifier) {
    entry.modifier = std::make_shared<PointModifier>();
  }
  entry.modifier->aim(local);
  entry.given = std::chrono::steady_clock::now();
}

void on_pass(void* character, void* animation) {
  std::shared_ptr<PointModifier> modifier;
  {
    std::lock_guard<std::mutex> lock(aimed_mutex);
    if (aimed.empty()) {
      return;
    }
    const auto found = aimed.find(character);
    if (found == aimed.end()) {
      return;
    }
    if (std::chrono::steady_clock::now() - found->second.given > kTargetFor) {
      aimed.erase(found);
      return;
    }
    modifier = found->second.modifier;
  }
  using Push = bool(__fastcall*)(void* animation, int stage, void* modifier, const char* name);
  alignas(
      std::shared_ptr<PointModifier>) unsigned char storage[sizeof(std::shared_ptr<PointModifier>)];
  // The push takes the std::shared_ptr by value: the copy is the callee's to let go.
  auto* copy = new (storage) std::shared_ptr<PointModifier>(modifier);
  slot<Push>(animation, kPushStageModifierSlot)(animation, kDrawnPoseStage, copy, kModifierName);
}

void stop_local(void* controller) {
  pointing = false;
  if (player_animation::local_request_running()) {
    player_animation::stop_local(controller);
  }
  if (natives.log) {
    natives.log("POINTING ends");
  }
}

}  // namespace

void install(const Natives& given) {
  natives = given;
  upper_body::add_pass_listener(&on_pass);
}

void keep_local(void* controller, void* local_entity) {
  const bool may_point = !natives.allowed || natives.allowed();
  // The game's own quick-access menu on the same key stays off while the player may point.
  const bool was_off = quick_access.off;
  const auto change = quick_access_key::step(quick_access, may_point, now_ms());
  if (change != quick_access_key::Switch::None && natives.enable_action_map) {
    const bool on = change == quick_access_key::Switch::On;
    natives.enable_action_map(kQuickAccessMap, on);
    if (natives.log && was_off == on) {
      natives.log("POINTING the game's quick-access key %s", on ? "given back" : "off");
    }
  }
  const bool wanted =
      local_entity && controller && natives.key_down && natives.key_down() && may_point;
  if (pointing && (!wanted || !player_animation::local_request_running())) {
    stop_local(controller);
    return;
  }
  if (!pointing) {
    // A request of the server's plays: the key waits.
    if (!wanted || player_animation::running_request() != 0) {
      return;
    }
    AnimationOrder order;
    order.clip = kPointClip;
    order.loop = true;
    order.upper_body = true;
    order.speed = 0.0f;
    order.start = kPointTime;
    if (!player_animation::play_local(controller, order)) {
      return;
    }
    pointing = true;
    if (natives.log) {
      natives.log("POINTING begins");
    }
  }
  float camera[12];
  if (!natives.camera || !natives.camera(camera)) {
    return;
  }
  for (int axis = 0; axis < 3; ++axis) {
    local_target[axis] = camera[axis * 4 + 3] + camera[axis * 4 + 1] * kTargetDistance;
  }
  aim(local_entity, local_target);
}

std::optional<std::string> local_mark() {
  if (!pointing) {
    return std::nullopt;
  }
  char text[64];
  std::snprintf(text, sizeof(text), "%.1f,%.1f,%.1f",
                std::round(local_target[0] / kMarkStep) * kMarkStep,
                std::round(local_target[1] / kMarkStep) * kMarkStep,
                std::round(local_target[2] / kMarkStep) * kMarkStep);
  return std::string(text);
}

void keep_replica(void* entity, const std::string& mark) {
  float target[3];
  if (!mark.empty() && entity && kcd2s::integration::parse_pointing_mark(mark, target)) {
    aim(entity, target);
  }
}

void reset() {
  pointing = false;
  std::lock_guard<std::mutex> lock(aimed_mutex);
  aimed.clear();
}

}  // namespace kcds::runtime::animation::pointing
