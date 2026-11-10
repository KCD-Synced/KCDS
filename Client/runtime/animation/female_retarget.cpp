#include "female_retarget.hpp"

#include <algorithm>
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

#include "../look/native_look.hpp"
#include "../ranged/aim.hpp"
#include "arm_ik.hpp"
#include "upper_body.hpp"

namespace kcds::runtime::animation::female_retarget {
namespace {

// ICharacterInstance's default skeleton (as upper_body).
constexpr std::size_t kDefaultSkeletonSlot = 11;
// IDefaultSkeleton: a joint's name (slot 6), its default absolute and relative QuatT (slots 8 and
// 9, the pose data at +24) and the skeleton's file (slot 10, the string at +552).
constexpr std::size_t kJointNameSlot = 6;
constexpr std::size_t kDefaultAbsoluteSlot = 8;
constexpr std::size_t kDefaultRelativeSlot = 9;
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

const float* bind_relative(void* skeleton, int joint) {
  using DefaultJoint = const float*(__fastcall*)(void* skeleton, std::int16_t id);
  return slot<DefaultJoint>(skeleton, kDefaultRelativeSlot)(skeleton, std::int16_t(joint));
}

// The male bind pose by joint name (in the character's space) and each joint's parent, from the
// first male skeleton the passes show.
struct MaleJoint {
  QuatT absolute;
  std::string parent;
};
using MaleBind = std::unordered_map<std::string, MaleJoint>;
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
  const auto joints = read<const char*>(skeleton, kSkeletonJoints);
  for (int joint = 0; joints && joint < count; ++joint) {
    const char* name = joint_name(skeleton, joint);
    const auto* absolute = reinterpret_cast<const QuatT*>(bind_absolute(skeleton, joint));
    if (!name || !absolute) {
      continue;
    }
    const auto parent = read<std::uint16_t>(joints, joint * kJointSize + kJointParent);
    const char* parent_name =
        parent != kNoParent && parent < count ? joint_name(skeleton, parent) : nullptr;
    (*bind)[name] = {*absolute, parent_name ? parent_name : ""};
  }
  if (bind->empty()) {
    return;
  }
  male_bind = std::move(bind);
  male_known.store(true);
}

// The transform that undoes `t`.
QuatT inverse(const QuatT& t) {
  QuatT rotation{};
  conjugate(t.q, rotation.q);
  QuatT moved{{0.0f, 0.0f, 0.0f, 1.0f}, {-t.t[0], -t.t[1], -t.t[2]}};
  return compose(rotation, moved);
}

// One female skeleton's retarget: a parent-first order of its joints and, for each joint the male
// skeleton has too, the rotations around the clip's (left * clip * right).
struct Retarget {
  std::vector<std::uint16_t> parents;
  std::vector<int> order;
  // A joint the clips leave unkeyed keeps the skeleton's own rest rotation, hers: taken for a
  // male clip's rotation, it turned her ring and little fingers' palm bones off the grip. Her rest
  // rotation (`rest`) is read as the male one (`male_rest`) before anything uses the clip.
  struct Moved {
    int joint;
    float left[4];
    float right[4];
    float rest[4];
    float male_rest[4];
  };
  std::vector<Moved> moved;
  // A slot an item hangs on, placed after the rest as the male one sits on its anchor: for a worn
  // weapon's slot (kWeaponSlots) the nearest joint both skeletons have, from the male bind pose;
  // for a hand's slot (kHandSlots) the hand, from the clip (`driven`). `to_male` turns the
  // anchor's retargeted rotation (her joint's own axes) back into the male joint's axes.
  struct Slot {
    int joint;
    int anchor;
    QuatT from_anchor;
    float to_male[4];
    bool driven;
    // A joint a worn mesh is skinned to (the scabbard): the mesh is drawn from her rest pose of
    // the joint, which is not the male one, so the joint stands where the male one does times
    // the male rest pose's turn into hers (`skin`). At the male joint itself her scabbard hung a
    // hand higher than the sword in it.
    bool skinned;
    QuatT skin;
    // A hand slot is held in the middle of the curled middle finger, between its knuckle and its
    // tip, as far from there as the male slot is from his: her knuckles sit otherwise in her palm
    // and her fingers are shorter, and at the male offset from her wrist or her knuckle the bow lay
    // in her fingertips. Her knuckle (-1 for none), the male knuckle's place from the male hand,
    // and her fingertip (Retarget::hands[tip_hand].fingers[tip_finger]; -1 for none).
    int finger;
    float finger_bone[3];
    int tip_hand;
    int tip_finger;
  };
  std::vector<Slot> slots;
  // An arm that reaches, while she aims, where the male hand is from the head (arm_ik.hpp): her
  // joints, the male chain from the chest's child to the hand (her joint for the clip's rotation,
  // the male bone for the length) and the chest's and the hand's turn into the male axes. The head
  // is learned the same way, its chain to the head, with no shoulder or elbow.
  struct Arm {
    int chest;
    int shoulder;
    int elbow;
    int hand;
    std::vector<int> chain;
    std::vector<std::array<float, 3>> male_bones;
    float chest_to_male[4];
    float hand_to_male[4];
  };
  std::vector<Arm> arms;
  Arm head{};
  bool has_head = false;
  // A hand's fingers, each joint's bone turned to point where the male one points from the male
  // hand (her rest pose holds the fingers otherwise than his, so his turns left her grip half
  // open). The joints parent first, each with its parent (-1 for the hand), its one finger child
  // (-1 for none or several) and the male bone from the male parent.
  struct FingerJoint {
    int joint;
    int parent;
    int child;
    float male_bone[3];
  };
  struct Hand {
    int joint;
    float to_male[4];
    std::vector<FingerJoint> fingers;
  };
  std::vector<Hand> hands;
};

// A chain as it reaches while she aims, from the male skeleton's joints between the chest and its
// end (a hand, with the arm's shoulder and elbow; the head, with none); false when either skeleton
// lacks a joint of it.
bool learn_arm(void* skeleton, const MaleBind& male,
               const std::unordered_map<std::string, int>& index, const std::string& hand,
               const char* shoulder, const char* elbow, Retarget::Arm& arm) {
  const std::string chest = "Spine4";
  const auto joint = [&](const std::string& name) {
    const auto found = index.find(name);
    return found == index.end() ? -1 : found->second;
  };
  arm.chest = joint(chest);
  arm.shoulder = shoulder ? joint(shoulder) : -1;
  arm.elbow = elbow ? joint(elbow) : -1;
  arm.hand = joint(hand);
  if (arm.chest < 0 || arm.hand < 0 || (shoulder && arm.shoulder < 0) || (elbow && arm.elbow < 0)) {
    return false;
  }
  // The male chain from the hand up to the chest, then turned the right way.
  std::vector<std::string> names;
  for (std::string at = hand; at != chest;) {
    const auto found = male.find(at);
    if (found == male.end() || int(names.size()) >= kMaxJoints) {
      return false;
    }
    names.push_back(at);
    at = found->second.parent;
  }
  for (auto it = names.rbegin(); it != names.rend(); ++it) {
    const int her = joint(*it);
    if (her < 0) {
      return false;
    }
    const MaleJoint& male_joint = male.at(*it);
    const QuatT bone = compose(inverse(male.at(male_joint.parent).absolute), male_joint.absolute);
    arm.chain.push_back(her);
    arm.male_bones.push_back({bone.t[0], bone.t[1], bone.t[2]});
  }
  float flipped[4];
  conjugate(bind_absolute(skeleton, arm.chest), flipped);
  multiply(flipped, male.at(chest).absolute.q, arm.chest_to_male);
  conjugate(bind_absolute(skeleton, arm.hand), flipped);
  multiply(flipped, male.at(hand).absolute.q, arm.hand_to_male);
  return true;
}

// The joints the worn weapons hang on: the scabbard and the sword in it, the back's bow, crossbow,
// quiver and shield. The male clips do not drive them, and the female skeleton sets them for
// nothing a woman of the game wears: her own slots, or the male directions on them, left the bow
// across her shoulders, the quiver off her back and the sword beside its scabbard. Each is placed
// as on a man, from the nearest joint above it that both skeletons have (her hips, her spine).
const char* const kWeaponSlots[] = {
    "LeftWeaponSecondaryBelt", "LeftWeaponSheath", "LeftWeaponEnd",
    "LeftWeaponSlot",          "BackSlot_bow",     "BackSlot_crossbow",
    "BackSlot_quiver",         "BackSlot_shield"};

bool is_weapon_slot(const std::string& name) {
  for (const char* joint : kWeaponSlots) {
    if (name == joint) {
      return true;
    }
  }
  return false;
}

// The weapon slots a worn mesh is skinned to (Retarget::Slot::skinned): the scabbard's. The sword
// in it hangs from LeftWeaponSlot, a joint it is attached to, not skinned to.
bool is_skinned_slot(const std::string& name) {
  return name == "LeftWeaponSheath" || name == "LeftWeaponEnd";
}

// The slots a hand holds an item in (a torch, a drawn weapon). The clips drive them in the hand's
// frame, the male hand's: turned into her hand's own axes, her torch stood beside her open hand.
bool is_hand_slot(const char* name) {
  return std::strcmp(name, "LeftHandSlot") == 0 || std::strcmp(name, "RightHandSlot") == 0;
}

// The middle finger a hand slot is held from (Retarget::Slot::finger); finger -1 when either
// skeleton lacks it.
void learn_slot_finger(void* skeleton, const MaleBind& male,
                       const std::unordered_map<std::string, int>& index, const std::string& hand,
                       Retarget::Slot& slot) {
  slot.finger = -1;
  const std::string finger = hand + "Middle1";
  const auto her_finger = index.find(finger);
  const auto male_hand = male.find(hand);
  const auto male_finger = male.find(finger);
  if (her_finger == index.end() || male_hand == male.end() || male_finger == male.end() ||
      male_finger->second.parent != hand) {
    return;
  }
  const QuatT bone = compose(inverse(male_hand->second.absolute), male_finger->second.absolute);
  std::memcpy(slot.finger_bone, bone.t, sizeof(slot.finger_bone));
  slot.finger = her_finger->second;
}

// A hand's finger joints (Retarget::Hand): the joints under it, parent first, that the male hand
// has under it too; false when there are none.
bool learn_hand(void* skeleton, const MaleBind& male, const Retarget& learned,
                const std::vector<const char*>& names, int hand, Retarget::Hand& out) {
  const auto male_hand = male.find(names[hand]);
  if (male_hand == male.end()) {
    return false;
  }
  out.joint = hand;
  float flipped[4];
  conjugate(bind_absolute(skeleton, hand), flipped);
  multiply(flipped, male_hand->second.absolute.q, out.to_male);
  // Her joint's place in out.fingers.
  std::unordered_map<int, int> finger_of;
  for (const int joint : learned.order) {
    const std::uint16_t parent = learned.parents[joint];
    if (parent == kNoParent || !names[joint] || !names[parent] || is_hand_slot(names[joint])) {
      continue;
    }
    const auto parent_finger = finger_of.find(parent);
    if (parent != hand && parent_finger == finger_of.end()) {
      continue;
    }
    const auto male_joint = male.find(names[joint]);
    if (male_joint == male.end() || male_joint->second.parent != names[parent]) {
      continue;
    }
    const QuatT bone =
        compose(inverse(male.at(male_joint->second.parent).absolute), male_joint->second.absolute);
    Retarget::FingerJoint finger{};
    finger.joint = joint;
    finger.parent = parent == hand ? -1 : parent_finger->second;
    finger.child = -1;
    std::memcpy(finger.male_bone, bone.t, sizeof(finger.male_bone));
    finger_of[joint] = int(out.fingers.size());
    out.fingers.push_back(finger);
  }
  // A joint's bone points to its child: only a joint with one child has a direction to match.
  std::vector<int> children(out.fingers.size(), 0);
  for (std::size_t i = 0; i < out.fingers.size(); ++i) {
    const int parent = out.fingers[i].parent;
    if (parent >= 0) {
      ++children[parent];
      out.fingers[parent].child = int(i);
    }
  }
  for (std::size_t i = 0; i < out.fingers.size(); ++i) {
    if (children[i] != 1) {
      out.fingers[i].child = -1;
    }
  }
  return !out.fingers.empty();
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
  // Her joints by name, for the weapon slots' anchors.
  std::unordered_map<std::string, int> index;
  for (int joint = 0; joint < count; ++joint) {
    if (names[joint]) {
      index[names[joint]] = joint;
    }
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
      if (male_joint == male.end()) {
        continue;
      }
      if (is_weapon_slot(names[node])) {
        // Its anchor: the nearest of the male slot's ancestors her skeleton has, not a slot itself.
        std::string above = male_joint->second.parent;
        int anchor = -1;
        for (int steps = 0; !above.empty() && anchor < 0 && steps < kMaxJoints; ++steps) {
          const auto found = index.find(above);
          if (found != index.end() && !is_weapon_slot(above)) {
            anchor = found->second;
            break;
          }
          const auto next = male.find(above);
          above = next == male.end() ? std::string() : next->second.parent;
        }
        const auto male_anchor = anchor >= 0 ? male.find(names[anchor]) : male.end();
        if (male_anchor != male.end()) {
          Retarget::Slot slot{};
          slot.joint = node;
          slot.anchor = anchor;
          slot.from_anchor =
              compose(inverse(male_anchor->second.absolute), male_joint->second.absolute);
          float flipped[4];
          conjugate(bind_absolute(skeleton, anchor), flipped);
          multiply(flipped, male_anchor->second.absolute.q, slot.to_male);
          slot.skinned = is_skinned_slot(names[node]);
          if (slot.skinned) {
            const auto& hers = *reinterpret_cast<const QuatT*>(bind_absolute(skeleton, node));
            slot.skin = compose(inverse(male_joint->second.absolute), hers);
          }
          learned->slots.push_back(slot);
        }
        continue;
      }
      const auto male_hand = is_hand_slot(names[node]) && male_joint->second.parent == names[parent]
                                 ? male.find(names[parent])
                                 : male.end();
      if (male_hand != male.end()) {
        Retarget::Slot slot{};
        slot.joint = node;
        slot.anchor = parent;
        slot.driven = true;
        learn_slot_finger(skeleton, male, index, names[parent], slot);
        float flipped[4];
        conjugate(bind_absolute(skeleton, parent), flipped);
        multiply(flipped, male_hand->second.absolute.q, slot.to_male);
        learned->slots.push_back(slot);
        continue;
      }
      const auto male_parent = male.find(names[parent]);
      if (male_parent == male.end()) {
        continue;
      }
      Retarget::Moved entry{};
      entry.joint = node;
      float flipped[4];
      conjugate(bind_absolute(skeleton, parent), flipped);
      multiply(flipped, male_parent->second.absolute.q, entry.left);
      conjugate(male_joint->second.absolute.q, flipped);
      multiply(flipped, bind_absolute(skeleton, node), entry.right);
      std::memcpy(entry.rest, bind_relative(skeleton, node), sizeof(entry.rest));
      const QuatT male_rest =
          compose(inverse(male_parent->second.absolute), male_joint->second.absolute);
      std::memcpy(entry.male_rest, male_rest.q, sizeof(entry.male_rest));
      learned->moved.push_back(entry);
    }
  }
  learned->has_head = learn_arm(skeleton, male, index, "Head", nullptr, nullptr, learned->head);
  for (const std::string side : {"Right", "Left"}) {
    Retarget::Arm arm{};
    if (learned->has_head && learn_arm(skeleton, male, index, side + "Hand", (side + "Arm").c_str(),
                                       (side + "ForeArm").c_str(), arm)) {
      learned->arms.push_back(std::move(arm));
    }
    const auto hand = index.find(side + "Hand");
    Retarget::Hand fingers{};
    if (hand != index.end() && learn_hand(skeleton, male, *learned, names, hand->second, fingers)) {
      learned->hands.push_back(std::move(fingers));
    }
  }
  // Each hand slot's middle fingertip, among its hand's finger joints.
  for (auto& held : learned->slots) {
    held.tip_hand = -1;
    held.tip_finger = -1;
    if (!held.driven || held.finger < 0 || !names[held.anchor]) {
      continue;
    }
    const auto tip = index.find(std::string(names[held.anchor]) + "Middle4");
    for (std::size_t h = 0; tip != index.end() && h < learned->hands.size(); ++h) {
      const Retarget::Hand& hand = learned->hands[h];
      for (std::size_t f = 0; hand.joint == held.anchor && f < hand.fingers.size(); ++f) {
        if (hand.fingers[f].joint == tip->second) {
          held.tip_hand = int(h);
          held.tip_finger = int(f);
        }
      }
    }
  }
  return learned->moved.empty() && learned->slots.empty() ? nullptr : learned;
}

Logger log = nullptr;

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
  if (learned && log) {
    std::size_t fingers = 0;
    for (const auto& hand : learned->hands) {
      fingers += hand.fingers.size();
    }
    log("ANIMATION female retarget: %zu joints, %zu slots, %zu arms, %zu finger joints",
        learned->moved.size(), learned->slots.size(), learned->arms.size(), fingers);
  }
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
    // Her rest rotation where the clips left a joint unkeyed is the male rest rotation.
    for (const auto& entry : retarget->moved) {
      float* rotation = relative[entry.joint].q;
      const float same = rotation[0] * entry.rest[0] + rotation[1] * entry.rest[1] +
                         rotation[2] * entry.rest[2] + rotation[3] * entry.rest[3];
      if (std::abs(same) > 0.999999f) {
        std::memcpy(rotation, entry.male_rest, sizeof(entry.male_rest));
      }
    }
    // While she aims, each hand as the clip puts the male one, from the head: the clip's own
    // rotations on the male bones, before the retarget turns them into hers.
    const bool aiming = aiming_.load(std::memory_order_relaxed);
    reach_ = std::clamp(reach_ + (aiming ? kReachStep : -kReachStep), 0.0f, 1.0f);
    std::array<QuatT, 2> male_hands{};
    const std::size_t reaching =
        reach_ > 0.0f ? std::min<std::size_t>(retarget->arms.size(), 2) : 0;
    const auto male_end = [&](const Retarget::Arm& arm) {
      QuatT end{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
      for (std::size_t link = 0; link < arm.chain.size(); ++link) {
        QuatT bone{};
        std::memcpy(bone.q, relative[arm.chain[link]].q, sizeof(bone.q));
        std::memcpy(bone.t, arm.male_bones[link].data(), sizeof(bone.t));
        end = compose(end, bone);
      }
      return end;
    };
    // The draw is held at the face: each hand from the male head, so the drawing hand comes to her
    // cheek and the bow keeps the male distance from it (the arrow is made for it).
    const QuatT from_head = reaching > 0 ? inverse(male_end(retarget->head)) : QuatT{};
    for (std::size_t side = 0; side < reaching; ++side) {
      male_hands[side] = compose(from_head, male_end(retarget->arms[side]));
    }
    // Each finger joint as the clip puts the male one, from the male hand, before the retarget.
    male_fingers_.clear();
    for (const auto& hand : retarget->hands) {
      const std::size_t first = male_fingers_.size();
      for (const auto& finger : hand.fingers) {
        QuatT bone{};
        std::memcpy(bone.q, relative[finger.joint].q, sizeof(bone.q));
        std::memcpy(bone.t, finger.male_bone, sizeof(bone.t));
        const QuatT placed =
            finger.parent < 0 ? bone : compose(male_fingers_[first + finger.parent], bone);
        male_fingers_.push_back(placed);
      }
    }
    for (const auto& entry : retarget->moved) {
      float* rotation = relative[entry.joint].q;
      float step[4];
      multiply(entry.left, rotation, step);
      multiply(step, entry.right, rotation);
    }
    QuatT* absolute = absolute_joints(pose);
    const auto place_all = [&]() {
      for (const int joint : retarget->order) {
        const std::uint16_t parent = retarget->parents[joint];
        absolute[joint] =
            parent == kNoParent ? relative[joint] : compose(absolute[parent], relative[joint]);
      }
    };
    place_all();
    match_fingers(*retarget, relative, absolute);
    if (!retarget->slots.empty()) {
      // The weapon slots where the male ones sit on the same joints (parents before children),
      // then everything under them follows.
      for (std::size_t i = 0; i < retarget->slots.size(); ++i) {
        const Retarget::Slot& slot = retarget->slots[i];
        // The anchor as the male joint would stand: her position, the male joint's axes.
        QuatT anchor = absolute[slot.anchor];
        float turned[4];
        multiply(anchor.q, slot.to_male, turned);
        std::memcpy(anchor.q, turned, sizeof(turned));
        QuatT held = slot.driven ? relative[slot.joint] : slot.from_anchor;
        if (slot.driven && slot.finger >= 0) {
          // From her middle finger's knuckle (and its tip when known), turned with the hand: the
          // male slot's place from his.
          float male_from[3];
          float her_from[3];
          std::memcpy(male_from, slot.finger_bone, sizeof(male_from));
          std::memcpy(her_from, absolute[slot.finger].t, sizeof(her_from));
          if (slot.tip_hand >= 0) {
            std::size_t first = 0;
            for (int h = 0; h < slot.tip_hand; ++h) {
              first += retarget->hands[h].fingers.size();
            }
            const auto& tip = retarget->hands[slot.tip_hand].fingers[slot.tip_finger];
            const float* male_tip = male_fingers_[first + slot.tip_finger].t;
            const float* her_tip = absolute[tip.joint].t;
            for (int k = 0; k < 3; ++k) {
              male_from[k] = 0.5f * (male_from[k] + male_tip[k]);
              her_from[k] = 0.5f * (her_from[k] + her_tip[k]);
            }
          }
          for (int k = 0; k < 3; ++k) {
            held.t[k] -= male_from[k];
          }
          std::memcpy(anchor.t, her_from, sizeof(anchor.t));
        }
        QuatT wanted = compose(anchor, held);
        if (slot.skinned) {
          wanted = compose(wanted, slot.skin);
        }
        const std::uint16_t parent = retarget->parents[slot.joint];
        relative[slot.joint] =
            parent == kNoParent ? wanted : compose(inverse(absolute[parent]), wanted);
        absolute[slot.joint] = wanted;
      }
      place_all();
    }
    for (std::size_t side = 0; side < reaching; ++side) {
      reach(*retarget, relative, absolute, retarget->arms[side], male_hands[side], place_all);
    }
    return true;
  }
  virtual void synchronize() {
  }
  virtual void memory_usage(void* /*sizer*/) const {
  }

  // Whether she aims now (her aim blender's state), from her animation pass.
  void set_aiming(bool aiming) {
    aiming_.store(aiming, std::memory_order_relaxed);
  }

 private:
  // How far the arms reach the male hands, eased in and out over a few frames as she starts and
  // stops aiming.
  static constexpr float kReachStep = 0.15f;
  std::atomic<bool> aiming_{false};
  float reach_ = 0.0f;
  // The male finger joints of this pass, from the male hand (Retarget::hands, in order).
  std::vector<QuatT> male_fingers_;

  // Each finger joint turned so its bone points from her hand where the male one points from the
  // male hand; the joints after it (its own fingers) follow.
  void match_fingers(const Retarget& retarget, QuatT* relative, QuatT* absolute) const {
    namespace ik = arm_ik;
    std::size_t first = 0;
    for (const auto& hand : retarget.hands) {
      float hand_axes[4];
      multiply(absolute[hand.joint].q, hand.to_male, hand_axes);
      for (std::size_t i = 0; i < hand.fingers.size(); ++i) {
        const Retarget::FingerJoint& finger = hand.fingers[i];
        if (finger.child < 0) {
          continue;
        }
        const int child = hand.fingers[finger.child].joint;
        float male_bone[3];
        ik::subtract(male_fingers_[first + finger.child].t, male_fingers_[first + i].t, male_bone);
        float wanted[3];
        ik::rotate(hand_axes, male_bone, wanted);
        float now[3];
        ik::subtract(absolute[child].t, absolute[finger.joint].t, now);
        float swing[4];
        ik::rotation_between(now, wanted, swing);
        float turned[4];
        multiply(swing, absolute[finger.joint].q, turned);
        turn(retarget, relative, absolute, finger.joint, turned);
        for (std::size_t after = i + 1; after < hand.fingers.size(); ++after) {
          const int joint = hand.fingers[after].joint;
          absolute[joint] = compose(absolute[retarget.parents[joint]], relative[joint]);
        }
      }
      first += hand.fingers.size();
    }
  }

  // A joint's rotation set in the character's space, its relative one worked out from its parent.
  static void turn(const Retarget& retarget, QuatT* relative, QuatT* absolute, int joint,
                   const float rotation[4]) {
    std::memcpy(absolute[joint].q, rotation, sizeof(float) * 4);
    const std::uint16_t parent = retarget.parents[joint];
    if (parent == kNoParent) {
      std::memcpy(relative[joint].q, rotation, sizeof(float) * 4);
      return;
    }
    float flipped[4];
    conjugate(absolute[parent].q, flipped);
    multiply(flipped, rotation, relative[joint].q);
  }

  // Her hand where the male hand is from the head: the elbow bends to its distance, the shoulder
  // turns the hand onto it, the hand keeps its own turn (arm_ik.hpp).
  template <class PlaceAll>
  void reach(const Retarget& retarget, QuatT* relative, QuatT* absolute, const Retarget::Arm& arm,
             const QuatT& male_hand, PlaceAll& place_all) {
    namespace ik = arm_ik;
    QuatT head = absolute[retarget.head.hand];
    float turned[4];
    multiply(head.q, retarget.head.hand_to_male, turned);
    std::memcpy(head.q, turned, sizeof(turned));
    const QuatT wanted = compose(head, male_hand);
    // The hand's turn as the retarget left it: the male hand's own, from her chest down. Taken from
    // the head, it rolled her bow hand on the grip whenever her head stood otherwise than his.
    float hand_turn[4];
    std::memcpy(hand_turn, absolute[arm.hand].q, sizeof(hand_turn));
    float target[3];
    for (int i = 0; i < 3; ++i) {
      target[i] = absolute[arm.hand].t[i] + (wanted.t[i] - absolute[arm.hand].t[i]) * reach_;
    }
    float axis[3];
    const float bend = ik::bend_change(absolute[arm.shoulder].t, absolute[arm.elbow].t,
                                       absolute[arm.hand].t, target, axis);
    if (bend != 0.0f) {
      float step[4];
      ik::axis_angle(axis, bend, step);
      float bent[4];
      multiply(step, absolute[arm.elbow].q, bent);
      turn(retarget, relative, absolute, arm.elbow, bent);
      place_all();
    }
    float now[3];
    float to[3];
    ik::subtract(absolute[arm.hand].t, absolute[arm.shoulder].t, now);
    ik::subtract(target, absolute[arm.shoulder].t, to);
    float swing[4];
    ik::rotation_between(now, to, swing);
    float swung[4];
    multiply(swing, absolute[arm.shoulder].q, swung);
    turn(retarget, relative, absolute, arm.shoulder, swung);
    place_all();
    turn(retarget, relative, absolute, arm.hand, hand_turn);
    place_all();
  }
};

// The modifier of one female character, while its passes run.
struct Body {
  std::shared_ptr<RetargetModifier> modifier;
  std::chrono::steady_clock::time_point seen;
};
std::mutex bodies_mutex;
std::unordered_map<void*, Body> bodies;

Body modifiers_for(void* character) {
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
  return body;
}

void push_stage(void* animation, int stage, const std::shared_ptr<RetargetModifier>& modifier) {
  using Push = bool(__fastcall*)(void* animation, int stage, void* modifier, const char* name);
  alignas(std::shared_ptr<RetargetModifier>) unsigned char
      storage[sizeof(std::shared_ptr<RetargetModifier>)];
  // The push takes the std::shared_ptr by value: the copy is the callee's to let go.
  auto* copy = new (storage) std::shared_ptr<RetargetModifier>(modifier);
  slot<Push>(animation, kPushStageModifierSlot)(animation, stage, copy, kModifierName);
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
    const Body body = modifiers_for(character);
    // Her aim blender is on while she aims a bow (her replica's holds its owner's): then her arms
    // reach the male hands (RetargetModifier::reach).
    void* aim = kcds::runtime::ranged::known_aim_of(kcds::game_client::native_pose(character));
    body.modifier->set_aiming(aim && kcds::runtime::ranged::read_aim(aim).enabled);
    push_stage(animation, kDrawnPoseStage, body.modifier);
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
