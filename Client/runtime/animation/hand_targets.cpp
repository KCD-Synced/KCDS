#include "hand_targets.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "hand_ik.hpp"
#include "kcd2s/integration/hand_targets.hpp"
#include "upper_body.hpp"

namespace kcds::runtime::animation::hand_targets {
namespace {

namespace integration = kcd2s::integration;

// CEntity GetCharacter (slot 101, character slot 0); the character's default skeleton (slot 11)
// and its GetJointIDByName (slot 7); ISkeletonAnim's PushPoseModifier for a stage of the finished
// pose (slot 37), the drawn pose's stage 2 (as pointing and upper_body).
constexpr std::size_t kGetCharacterSlot = 101;
constexpr std::size_t kDefaultSkeletonSlot = 11;
constexpr std::size_t kJointIdByNameSlot = 7;
constexpr std::size_t kPushStageModifierSlot = 37;
constexpr int kDrawnPoseStage = 2;
constexpr char kModifierName[] = "KCDS hand targets";
// CDefaultSkeleton's joints (48 bytes each, their count before the first, the parent at + 22) and
// the pose arrays of SAnimationPoseModifierParams, as pointing reads them.
constexpr std::size_t kSkeletonJoints = 8;
constexpr std::size_t kJointSize = 48;
constexpr std::size_t kJointParent = 22;
constexpr int kMaxJoints = 1024;
constexpr std::size_t kParamsCharacter = 0;
constexpr std::size_t kParamsPose = 8;
constexpr std::size_t kPoseBuffer = 32;
constexpr std::size_t kPoseAbsoluteOffset = 8;
// The longest step a fade takes in one frame (seconds): a hitch does not jump it.
constexpr double kLongestStep = 0.25;

// Each arm's joints: the forearm (its parent, the upper arm, turns too), the hand and the hand's
// slot, the point held items attach at (the grip the target names).
struct ArmNames {
  const char* fore;
  const char* hand;
  const char* grip;
};
constexpr ArmNames kArms[2] = {{"RightForeArm", "RightHand", "RightHandSlot"},
                               {"LeftForeArm", "LeftHand", "LeftHandSlot"}};

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index]);
}

template <class T>
T read(const void* base, std::size_t offset) {
  return *reinterpret_cast<const T*>(static_cast<const char*>(base) + offset);
}

// What the modifier solves a hand toward this frame.
struct HandGoal {
  bool active = false;
  std::string joint;
  hand_ik::Goal goal;
};

// The game's IAnimationPoseModifier, as its feet lock lays it out (see upper_body.cpp). One per
// character: it solves each held hand toward the goal the engine thread last gave it.
class HandModifier {
 public:
  virtual void* query_interface(const void* /*interface_id*/) const {
    return const_cast<HandModifier*>(this);
  }
  virtual void* query_composite(const char* /*name*/) const {
    return nullptr;
  }
  virtual void* factory() const {
    return nullptr;
  }
  virtual ~HandModifier() = default;
  virtual bool prepare(const void* /*params*/) {
    return true;
  }
  virtual bool execute(const void* params) {
    void* character = read<void*>(params, kParamsCharacter);
    void* pose_object = read<void*>(params, kParamsPose);
    std::array<HandGoal, 2> goals;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      goals = goals_;
    }
    using Getter = void*(__fastcall*)(void* object);
    void* skeleton = character ? slot<Getter>(character, kDefaultSkeletonSlot)(character) : nullptr;
    if (!skeleton || !pose_object || !learn(skeleton)) {
      return false;
    }
    const auto buffer = read<std::uintptr_t>(pose_object, kPoseBuffer);
    const auto offset = read<std::uintptr_t>(pose_object, kPoseAbsoluteOffset);
    hand_ik::Pose pose;
    pose.relative = reinterpret_cast<hand_ik::QuatT*>((buffer + 3) & ~std::uintptr_t(3));
    pose.absolute = reinterpret_cast<hand_ik::QuatT*>(offset + ((buffer + 3) & ~std::uintptr_t(3)));
    pose.parents = parents_.data();
    pose.count = int(parents_.size());
    bool solved = false;
    for (int arm = 0; arm < 2; ++arm) {
      if (!goals[arm].active || chains_[arm].upper < 0) {
        continue;
      }
      hand_ik::Goal goal = goals[arm].goal;
      // Measured from a joint: its place in the pose now, on the body's axes.
      if (!goals[arm].joint.empty()) {
        const int base = joint_id(skeleton, goals[arm].joint);
        if (base < 0) {
          continue;
        }
        const float* at = pose.absolute[base].t;
        for (int axis = 0; axis < 3; ++axis) {
          goal.position[axis] += at[axis];
          goal.pole[axis] += at[axis];
        }
      }
      solved = hand_ik::solve(pose, chains_[arm], goal) || solved;
    }
    return solved;
  }
  virtual void synchronize() {
  }
  virtual void memory_usage(void* /*sizer*/) const {
  }

  void set(const std::array<HandGoal, 2>& goals) {
    std::lock_guard<std::mutex> lock(mutex_);
    goals_ = goals;
  }

 private:
  // The parents and both arms' chains, once per skeleton.
  bool learn(void* skeleton) {
    if (skeleton == skeleton_) {
      return known_;
    }
    skeleton_ = skeleton;
    known_ = false;
    joint_ids_.clear();
    const auto joints = read<const char*>(skeleton, kSkeletonJoints);
    if (!joints) {
      return false;
    }
    const int count = *reinterpret_cast<const int*>(joints - sizeof(int));
    if (count <= 0 || count > kMaxJoints) {
      return false;
    }
    parents_.resize(count);
    for (int joint = 0; joint < count; ++joint) {
      parents_[joint] = read<std::uint16_t>(joints, joint * kJointSize + kJointParent);
      if (parents_[joint] != hand_ik::kNoParent && parents_[joint] >= count) {
        return false;
      }
    }
    for (int arm = 0; arm < 2; ++arm) {
      chains_[arm] = hand_ik::Chain{};
      if (!hand_ik::find_chain(parents_.data(), count, joint_id(skeleton, kArms[arm].fore),
                               joint_id(skeleton, kArms[arm].hand),
                               joint_id(skeleton, kArms[arm].grip), chains_[arm])) {
        chains_[arm].upper = -1;
      }
    }
    known_ = chains_[0].upper >= 0 || chains_[1].upper >= 0;
    return known_;
  }

  int joint_id(void* skeleton, const std::string& name) {
    const auto found = joint_ids_.find(name);
    if (found != joint_ids_.end()) {
      return found->second;
    }
    using JointId = std::int16_t(__fastcall*)(void* skeleton, const char* name);
    const int id = slot<JointId>(skeleton, kJointIdByNameSlot)(skeleton, name.c_str());
    const int known = id >= 0 && id < int(parents_.size()) ? id : -1;
    joint_ids_[name] = known;
    return known;
  }

  std::mutex mutex_;
  std::array<HandGoal, 2> goals_{};
  const void* skeleton_ = nullptr;
  bool known_ = false;
  std::vector<std::uint16_t> parents_;
  std::array<hand_ik::Chain, 2> chains_{};
  std::unordered_map<std::string, int> joint_ids_;
};

// One hand of a body on the engine thread: what the field asks of it and how far it is faded in.
struct Hand {
  std::optional<integration::HandTarget> target;
  float blend = 0.0f;
  float weight = 0.0f;
};

struct Body {
  void* character = nullptr;
  std::shared_ptr<HandModifier> modifier = std::make_shared<HandModifier>();
  std::string field;
  std::array<Hand, 2> hands;
  double last = 0.0;
};

Logger log_line = nullptr;
// By entity: the bodies with a hand held or fading out (engine thread).
std::unordered_map<void*, Body> bodies;
// By character: the modifiers the passes push (the animation jobs read it).
std::mutex pushed_mutex;
std::unordered_map<void*, std::shared_ptr<HandModifier>> pushed;

void* character_of(void* entity) {
  using GetCharacter = void*(__fastcall*)(void* entity, int slot);
  return entity ? slot<GetCharacter>(entity, kGetCharacterSlot)(entity, 0) : nullptr;
}

void on_pass(void* character, void* animation) {
  std::shared_ptr<HandModifier> modifier;
  {
    std::lock_guard<std::mutex> lock(pushed_mutex);
    if (pushed.empty()) {
      return;
    }
    const auto found = pushed.find(character);
    if (found == pushed.end()) {
      return;
    }
    modifier = found->second;
  }
  using Push = bool(__fastcall*)(void* animation, int stage, void* modifier, const char* name);
  alignas(
      std::shared_ptr<HandModifier>) unsigned char storage[sizeof(std::shared_ptr<HandModifier>)];
  // The push takes the std::shared_ptr by value: the copy is the callee's to let go.
  auto* copy = new (storage) std::shared_ptr<HandModifier>(modifier);
  slot<Push>(animation, kPushStageModifierSlot)(animation, kDrawnPoseStage, copy, kModifierName);
}

void forget(void* entity) {
  const auto found = bodies.find(entity);
  if (found == bodies.end()) {
    return;
  }
  std::lock_guard<std::mutex> lock(pushed_mutex);
  pushed.erase(found->second.character);
  bodies.erase(found);
}

}  // namespace

void install(Logger logger) {
  log_line = logger;
  upper_body::add_pass_listener(&on_pass);
}

void keep(void* entity, const std::string& field, bool first_person, double now) {
  if (!entity) {
    return;
  }
  auto found = bodies.find(entity);
  if (found == bodies.end()) {
    if (field.empty()) {
      return;
    }
    found = bodies.emplace(entity, Body{}).first;
    found->second.last = now;
  }
  Body& body = found->second;
  if (field != body.field) {
    std::vector<integration::HandTarget> targets;
    if (!integration::parse_hand_targets_field(field, targets)) {
      targets.clear();
    }
    body.field = field;
    for (int arm = 0; arm < 2; ++arm) {
      Hand& hand = body.hands[arm];
      const auto target = std::find_if(targets.begin(), targets.end(),
                                       [arm](const auto& one) { return one.right == (arm == 0); });
      if (target != targets.end()) {
        hand.target = *target;
        hand.blend = target->blend;
      } else if (hand.target) {
        // Let go: it fades out over the blend it had, holding the target it had.
        hand.target->weight = 0.0f;
      }
    }
    if (log_line) {
      log_line("HAND TARGETS body=%p field='%s'", entity, field.c_str());
    }
  }
  const double step = std::clamp(now - body.last, 0.0, kLongestStep);
  body.last = now;
  std::array<HandGoal, 2> goals;
  bool any = false;
  for (int arm = 0; arm < 2; ++arm) {
    Hand& hand = body.hands[arm];
    if (!hand.target) {
      continue;
    }
    const float wanted = hand.target->weight;
    const float change = hand.blend > 0.0f ? float(step / hand.blend) : 1.0f;
    if (hand.weight < wanted) {
      hand.weight = std::min(wanted, hand.weight + change);
    } else {
      hand.weight = std::max(wanted, hand.weight - change);
    }
    if (!(hand.weight > 0.0f) && !(wanted > 0.0f)) {
      hand.target.reset();
      continue;
    }
    const integration::HandTarget& target = *hand.target;
    HandGoal& goal = goals[arm];
    goal.active = true;
    goal.joint = target.joint;
    const auto& position = first_person && target.first_person_position
                               ? *target.first_person_position
                               : target.position;
    std::copy(position.begin(), position.end(), goal.goal.position);
    const auto& rotation = first_person && target.first_person_rotation
                               ? target.first_person_rotation
                               : target.rotation;
    goal.goal.has_rotation = rotation.has_value();
    if (rotation) {
      std::copy(rotation->begin(), rotation->end(), goal.goal.rotation);
    }
    goal.goal.has_pole = target.pole.has_value();
    if (target.pole) {
      std::copy(target.pole->begin(), target.pole->end(), goal.goal.pole);
    }
    goal.goal.weight = hand.weight;
    any = true;
  }
  if (!any) {
    forget(entity);
    return;
  }
  body.modifier->set(goals);
  void* character = character_of(entity);
  std::lock_guard<std::mutex> lock(pushed_mutex);
  if (character != body.character) {
    pushed.erase(body.character);
    body.character = character;
  }
  if (character) {
    pushed[character] = body.modifier;
  }
}

void keep_only(const std::vector<void*>& entities) {
  const std::unordered_set<void*> kept(entities.begin(), entities.end());
  std::vector<void*> gone;
  for (const auto& [entity, body] : bodies) {
    if (!kept.count(entity)) {
      gone.push_back(entity);
    }
  }
  for (void* entity : gone) {
    forget(entity);
  }
}

void reset() {
  bodies.clear();
  std::lock_guard<std::mutex> lock(pushed_mutex);
  pushed.clear();
}

}  // namespace kcds::runtime::animation::hand_targets
