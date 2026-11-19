#include "skeleton_info.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace kcds::runtime::animation::skeleton_info {
namespace {

namespace sdk = kcd2s::sdk;

// CEntity GetCharacter (slot 101, character slot 0); ICharacterInstance's default skeleton (slot
// 11); IDefaultSkeleton: a joint's name (slot 6) and its default absolute QuatT (slot 8), as
// female_retarget reads them; CDefaultSkeleton's joints (48 bytes each, their count before the
// first, the parent at + 22).
constexpr std::size_t kGetCharacterSlot = 101;
constexpr std::size_t kDefaultSkeletonSlot = 11;
constexpr std::size_t kJointNameSlot = 6;
constexpr std::size_t kDefaultAbsoluteSlot = 8;
constexpr std::size_t kSkeletonJoints = 8;
constexpr std::size_t kJointSize = 48;
constexpr std::size_t kJointParent = 22;
constexpr std::uint16_t kNoParent = 0xFFFF;
constexpr int kMaxJoints = 1024;

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index]);
}

}  // namespace

sdk::Value describe(void* entity) {
  if (!entity) {
    return sdk::Value();
  }
  using GetCharacter = void*(__fastcall*)(void* entity, int slot);
  using Getter = void*(__fastcall*)(void* object);
  void* character = slot<GetCharacter>(entity, kGetCharacterSlot)(entity, 0);
  void* skeleton = character ? slot<Getter>(character, kDefaultSkeletonSlot)(character) : nullptr;
  if (!skeleton) {
    return sdk::Value();
  }
  const auto* joints =
      *reinterpret_cast<const char* const*>(static_cast<const char*>(skeleton) + kSkeletonJoints);
  if (!joints) {
    return sdk::Value();
  }
  const int count = *reinterpret_cast<const int*>(joints - sizeof(int));
  if (count <= 0 || count > kMaxJoints) {
    return sdk::Value();
  }
  using JointName = const char*(__fastcall*)(void* skeleton, std::int16_t id);
  using DefaultJoint = const float*(__fastcall*)(void* skeleton, std::int16_t id);
  const auto name_of = [&](int joint) {
    const char* name = slot<JointName>(skeleton, kJointNameSlot)(skeleton, std::int16_t(joint));
    return std::string(name ? name : "");
  };
  // A message to the runtime holds at most 64 KB and a skeleton has hundreds of joints: the names
  // and parents (index, -1 for the root) as ',' lists, the rest pose of each joint in the body's
  // space (QuatT: x, y, z, w in ten-thousandths, then the position in hundredths of a millimetre)
  // as one ',' list of integers, 7 a joint. The worker works out each joint's pose relative to its
  // parent's.
  std::string names;
  std::string parents;
  std::string rest;
  int found = 0;
  for (int joint = 0; joint < count; ++joint) {
    const auto parent =
        *reinterpret_cast<const std::uint16_t*>(joints + joint * kJointSize + kJointParent);
    const float* pose =
        slot<DefaultJoint>(skeleton, kDefaultAbsoluteSlot)(skeleton, std::int16_t(joint));
    const float identity[7] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    const float* absolute = pose ? pose : identity;
    found += pose ? 1 : 0;
    if (joint > 0) {
      names += ',';
      parents += ',';
      rest += ',';
    }
    names += name_of(joint);
    parents += std::to_string(parent != kNoParent && parent < count ? int(parent) : -1);
    for (int part = 0; part < 7; ++part) {
      if (part > 0) {
        rest += ',';
      }
      const double scale = part < 4 ? 10000.0 : 100000.0;
      rest += std::to_string(std::llround(double(absolute[part]) * scale));
    }
  }
  if (found == 0) {
    return sdk::Value();
  }
  return sdk::Value(sdk::Value::Map{{"names", sdk::Value(std::move(names))},
                                    {"parents", sdk::Value(std::move(parents))},
                                    {"rest", sdk::Value(std::move(rest))}});
}

}  // namespace kcds::runtime::animation::skeleton_info
