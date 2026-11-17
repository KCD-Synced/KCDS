#include "passenger_pose.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>

#include "../animation/upper_body.hpp"
#include "passenger_hold.hpp"

namespace kcds::runtime::riding::passenger_pose {
namespace {

using Clock = std::chrono::steady_clock;

// CEntity GetCharacter (slot 101, character slot 0); the character's default skeleton (slot 11)
// and its GetJointIDByName (slot 7); ISkeletonAnim's PushPoseModifier for a stage of the finished
// pose (slot 37), the drawn pose's stage 2: as animation/pointing.cpp.
constexpr std::size_t kGetCharacterSlot = 101;
constexpr std::size_t kDefaultSkeletonSlot = 11;
constexpr std::size_t kJointIdByNameSlot = 7;
constexpr std::size_t kPushStageModifierSlot = 37;
constexpr int kDrawnPoseStage = 2;
constexpr char kModifierName[] = "KCDS passenger";
// CDefaultSkeleton's joints (48 bytes each, their count before the first, the parent at + 22) and
// the pose's joint arrays, as upper_body reads them.
constexpr std::size_t kSkeletonJoints = 8;
constexpr std::size_t kJointSize = 48;
constexpr std::size_t kJointParent = 22;
constexpr int kMaxJoints = 1024;
constexpr std::size_t kParamsCharacter = 0;
constexpr std::size_t kParamsPose = 8;
constexpr std::size_t kPoseBuffer = 32;
constexpr std::size_t kPoseAbsoluteOffset = 8;
// The joints, the game's men's and women's skeletons alike.
constexpr char kHips[] = "Hips";
constexpr char kSpine[] = "Spine";
constexpr char kNeck[] = "Neck";
constexpr const char* kUpperArm[2] = {"LeftArm", "RightArm"};
constexpr const char* kForeArm[2] = {"LeftForeArm", "RightForeArm"};
constexpr const char* kHand[2] = {"LeftHand", "RightHand"};
// A body comes into the hold over this time; one not named again for this long is let go.
constexpr std::chrono::milliseconds kBlendIn{400};
constexpr std::chrono::milliseconds kHeldFor{250};

using passenger_hold::Joint;

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index]);
}

template <class T>
T read(const void* base, std::size_t offset) {
  return *reinterpret_cast<const T*>(static_cast<const char*>(base) + offset);
}

Logger log_line = nullptr;

// The game's IAnimationPoseModifier, as its feet lock lays it out (animation/upper_body.cpp).
// One per character.
class HoldModifier {
 public:
  explicit HoldModifier(Clock::time_point since) : since_(since) {
  }
  virtual void* query_interface(const void* /*interface_id*/) const {
    return const_cast<HoldModifier*>(this);
  }
  virtual void* query_composite(const char* /*name*/) const {
    return nullptr;
  }
  virtual void* factory() const {
    return nullptr;
  }
  virtual ~HoldModifier() = default;
  virtual bool prepare(const void* /*params*/) {
    return true;
  }
  virtual bool execute(const void* params) {
    void* character = read<void*>(params, kParamsCharacter);
    void* pose = read<void*>(params, kParamsPose);
    using Getter = void*(__fastcall*)(void* object);
    void* skeleton = character ? slot<Getter>(character, kDefaultSkeletonSlot)(character) : nullptr;
    if (!skeleton || !pose || !learn(skeleton)) {
      return false;
    }
    const auto buffer = read<std::uintptr_t>(pose, kPoseBuffer);
    const auto offset = read<std::uintptr_t>(pose, kPoseAbsoluteOffset);
    Joint* relative = reinterpret_cast<Joint*>((buffer + 3) & ~std::uintptr_t(3));
    Joint* absolute = reinterpret_cast<Joint*>(offset + ((buffer + 3) & ~std::uintptr_t(3)));
    const auto held = std::chrono::duration<float>(Clock::now() - since_).count();
    const float weight =
        std::clamp(held / std::chrono::duration<float>(kBlendIn).count(), 0.0f, 1.0f);
    passenger_hold::Rider rider;
    {
      std::lock_guard<std::mutex> lock(rider_mutex_);
      rider = rider_;
    }
    return passenger_hold::apply(skeleton_joints_, relative, absolute, weight, rider);
  }

  // The rider as the engine thread saw them last.
  void see(const passenger_hold::Rider& rider) {
    std::lock_guard<std::mutex> lock(rider_mutex_);
    rider_ = rider;
  }
  virtual void synchronize() {
  }
  virtual void memory_usage(void* /*sizer*/) const {
  }

 private:
  // The joints by name and the parents, once per skeleton.
  bool learn(void* skeleton) {
    if (skeleton == skeleton_) {
      return ready_;
    }
    skeleton_ = skeleton;
    ready_ = false;
    passenger_hold::Skeleton joints;
    const auto list = read<const char*>(skeleton, kSkeletonJoints);
    if (!list) {
      return false;
    }
    const int count = *reinterpret_cast<const int*>(list - sizeof(int));
    if (count <= 0 || count > kMaxJoints) {
      return false;
    }
    using JointId = std::int16_t(__fastcall*)(void* skeleton, const char* name);
    const auto id = [skeleton](const char* name) {
      return int(slot<JointId>(skeleton, kJointIdByNameSlot)(skeleton, name));
    };
    joints.hips = id(kHips);
    joints.spine = id(kSpine);
    joints.neck = id(kNeck);
    for (int side = 0; side < 2; ++side) {
      joints.upper_arm[side] = id(kUpperArm[side]);
      joints.forearm[side] = id(kForeArm[side]);
      joints.hand[side] = id(kHand[side]);
    }
    joints.parents.resize(std::size_t(count));
    for (int joint = 0; joint < count; ++joint) {
      const auto parent = read<std::uint16_t>(list, std::size_t(joint) * kJointSize + kJointParent);
      if (parent != passenger_hold::kNoParent && parent >= count) {
        return false;
      }
      joints.parents[std::size_t(joint)] = parent;
    }
    if (!passenger_hold::prepare(joints)) {
      return false;
    }
    skeleton_joints_ = std::move(joints);
    ready_ = true;
    return true;
  }

  Clock::time_point since_;
  std::mutex rider_mutex_;
  passenger_hold::Rider rider_;
  const void* skeleton_ = nullptr;
  bool ready_ = false;
  passenger_hold::Skeleton skeleton_joints_;
};

struct Held {
  std::shared_ptr<HoldModifier> modifier;
  Clock::time_point given;
};

std::mutex held_mutex;
// By character: the modifiers the passes push while their bodies are named.
std::unordered_map<void*, Held> held;

void* character_of(void* entity) {
  using GetCharacter = void*(__fastcall*)(void* entity, int slot);
  return entity ? slot<GetCharacter>(entity, kGetCharacterSlot)(entity, 0) : nullptr;
}

void on_pass(void* character, void* animation) {
  std::shared_ptr<HoldModifier> modifier;
  {
    std::lock_guard<std::mutex> lock(held_mutex);
    if (held.empty()) {
      return;
    }
    const auto found = held.find(character);
    if (found == held.end()) {
      return;
    }
    if (Clock::now() - found->second.given > kHeldFor) {
      held.erase(found);
      return;
    }
    modifier = found->second.modifier;
  }
  using Push = bool(__fastcall*)(void* animation, int stage, void* modifier, const char* name);
  alignas(
      std::shared_ptr<HoldModifier>) unsigned char storage[sizeof(std::shared_ptr<HoldModifier>)];
  // The push takes the std::shared_ptr by value: the copy is the callee's to let go.
  auto* copy = new (storage) std::shared_ptr<HoldModifier>(modifier);
  slot<Push>(animation, kPushStageModifierSlot)(animation, kDrawnPoseStage, copy, kModifierName);
}

}  // namespace

void install(Logger logger) {
  log_line = logger;
  animation::upper_body::add_pass_listener(&on_pass);
}

void hold(void* entity, const passenger_hold::Rider& rider) {
  void* character = character_of(entity);
  if (!character) {
    return;
  }
  const auto now = Clock::now();
  std::lock_guard<std::mutex> lock(held_mutex);
  Held& entry = held[character];
  if (!entry.modifier || now - entry.given > kHeldFor) {
    entry.modifier = std::make_shared<HoldModifier>(now);
    if (log_line) {
      log_line("PASSENGER pose holds on (character %p)", character);
    }
  }
  entry.given = now;
  entry.modifier->see(rider);
}

void reset() {
  std::lock_guard<std::mutex> lock(held_mutex);
  held.clear();
}

}  // namespace kcds::runtime::riding::passenger_pose
