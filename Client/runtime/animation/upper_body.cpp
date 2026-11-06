#include "upper_body.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

#include "MinHook.h"

namespace kcds::runtime::animation::upper_body {
namespace {

// CEntity GetCharacter, ICharacterInstance its ISkeletonAnim and default skeleton,
// IDefaultSkeleton GetJointIDByName (as props/prop_entity.cpp).
constexpr std::size_t kGetCharacterSlot = 101;
constexpr int kCharacterSlot = 0;
constexpr std::size_t kSkeletonAnimSlot = 5;
constexpr std::size_t kDefaultSkeletonSlot = 11;
constexpr std::size_t kJointIdByNameSlot = 7;
// ISkeletonAnim PushPoseModifier for a layer (layer, std::shared_ptr by value) and for a
// stage of the finished pose (stage, std::shared_ptr by value, name).
constexpr std::size_t kPushLayerModifierSlot = 38;
constexpr std::size_t kPushStageModifierSlot = 37;
constexpr unsigned kLayers = 16;
// The layer the body's walk plays on (its locomotion).
constexpr unsigned kWalkLayer = 0;
// The stage whose modifiers change the pose the body is drawn with (stages 0 and 1 do not).
constexpr int kDrawnPoseStage = 2;
constexpr char kModifierName[] = "KCDS upper body";
// CDefaultSkeleton: its joints, 48 bytes each, their count in the int before the first; each
// joint's name at +0 and its parent at +22, 0xFFFF for the root.
constexpr std::size_t kSkeletonJoints = 8;
constexpr std::size_t kJointSize = 48;
constexpr std::size_t kJointParent = 22;
constexpr std::uint16_t kNoParent = 0xFFFF;
constexpr int kMaxJoints = 1024;
// SAnimationPoseModifierParams: the character instance, then the pose data. The pose's joint
// arrays: the relative QuatTs at its buffer (+32) rounded up to 4 bytes, the absolute
// ones that far plus the offset at +8.
constexpr std::size_t kParamsCharacter = 0;
constexpr std::size_t kParamsPose = 8;
constexpr std::size_t kPoseBuffer = 32;
constexpr std::size_t kPoseAbsoluteOffset = 8;
// The joint whose subtree is the upper body.
constexpr char kSpine[] = "spine";
// Bodies not kept for this long are forgotten.
constexpr std::chrono::seconds kForgetAfter{10};

// The engine's QuatT: the rotation (x, y, z, w), then the translation.
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

// The rotation b brought into a's space: conj(a) * b, for unit quaternions.
void relative_rotation(const float a[4], const float b[4], float out[4]) {
  const float c[4] = {-a[0], -a[1], -a[2], a[3]};
  out[0] = c[3] * b[0] + b[3] * c[0] + (c[1] * b[2] - c[2] * b[1]);
  out[1] = c[3] * b[1] + b[3] * c[1] + (c[2] * b[0] - c[0] * b[2]);
  out[2] = c[3] * b[2] + b[3] * c[2] + (c[0] * b[1] - c[1] * b[0]);
  out[3] = c[3] * b[3] - (c[0] * b[0] + c[1] * b[1] + c[2] * b[2]);
}

// parent * child: the child's joint in the parent's space.
QuatT compose(const QuatT& parent, const QuatT& child) {
  const float* a = parent.q;
  const float* b = child.q;
  QuatT out{};
  out.q[0] = a[3] * b[0] + b[3] * a[0] + (a[1] * b[2] - a[2] * b[1]);
  out.q[1] = a[3] * b[1] + b[3] * a[1] + (a[2] * b[0] - a[0] * b[2]);
  out.q[2] = a[3] * b[2] + b[3] * a[2] + (a[0] * b[1] - a[1] * b[0]);
  out.q[3] = a[3] * b[3] - (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
  // The child's translation turned by the parent's rotation: v + w * u + q x u, u = 2 (q x v).
  const float* v = child.t;
  const float u[3] = {2.0f * (a[1] * v[2] - a[2] * v[1]), 2.0f * (a[2] * v[0] - a[0] * v[2]),
                      2.0f * (a[0] * v[1] - a[1] * v[0])};
  out.t[0] = v[0] + a[3] * u[0] + (a[1] * u[2] - a[2] * u[1]) + parent.t[0];
  out.t[1] = v[1] + a[3] * u[1] + (a[2] * u[0] - a[0] * u[2]) + parent.t[1];
  out.t[2] = v[2] + a[3] * u[2] + (a[0] * u[1] - a[1] * u[0]) + parent.t[2];
  return out;
}

// What the two modifiers of one body share: the relative joints under the clip and, per joint,
// whether it belongs to the upper body. Used by the animation job of that body only.
struct Shared {
  std::vector<QuatT> below;
  const void* skeleton = nullptr;
  int spine = -1;
  // The clip's joints in the character's space, worked out by the restore.
  std::vector<QuatT> clip;
  std::vector<std::uint16_t> parents;
  std::vector<std::uint8_t> upper;
  // Joints in an order where every parent comes before its children.
  std::vector<int> order;
};

QuatT* relative_joints(void* pose) {
  const auto buffer = read<std::uintptr_t>(pose, kPoseBuffer);
  return reinterpret_cast<QuatT*>((buffer + 3) & ~std::uintptr_t(3));
}

QuatT* absolute_joints(void* pose) {
  const auto buffer = read<std::uintptr_t>(pose, kPoseBuffer);
  const auto offset = read<std::uintptr_t>(pose, kPoseAbsoluteOffset);
  return reinterpret_cast<QuatT*>(offset + ((buffer + 3) & ~std::uintptr_t(3)));
}

// The skeleton's parents, upper-body joints and parent-first order, once per skeleton. False for
// a skeleton without a spine or with a broken parent chain.
bool learn_skeleton(Shared& shared, void* skeleton) {
  if (shared.skeleton == skeleton) {
    return !shared.order.empty();
  }
  shared.skeleton = skeleton;
  shared.below.clear();
  shared.parents.clear();
  shared.upper.clear();
  shared.order.clear();
  const auto joints = read<const char*>(skeleton, kSkeletonJoints);
  if (!joints) {
    return false;
  }
  const int count = *reinterpret_cast<const int*>(joints - sizeof(int));
  using JointId = std::int16_t(__fastcall*)(void* skeleton, const char* name);
  const int spine = slot<JointId>(skeleton, kJointIdByNameSlot)(skeleton, kSpine);
  if (count <= 0 || count > kMaxJoints || spine < 0 || spine >= count) {
    return false;
  }
  shared.spine = spine;
  shared.parents.resize(count);
  for (int joint = 0; joint < count; ++joint) {
    const auto parent = read<std::uint16_t>(joints, joint * kJointSize + kJointParent);
    if (parent != kNoParent && parent >= count) {
      shared.parents.clear();
      return false;
    }
    shared.parents[joint] = parent;
  }
  // From each joint up to its first placed ancestor, then back down: parents before children.
  std::vector<std::uint8_t> placed(count, 0);
  shared.upper.assign(count, 0);
  std::vector<int> chain;
  for (int joint = 0; joint < count; ++joint) {
    chain.clear();
    int at = joint;
    while (at != kNoParent && !placed[at]) {
      if (int(chain.size()) >= count) {
        shared.order.clear();
        return false;
      }
      chain.push_back(at);
      at = shared.parents[at];
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      const int node = *it;
      const std::uint16_t parent = shared.parents[node];
      shared.upper[node] = node == spine || (parent != kNoParent && shared.upper[parent]);
      placed[node] = 1;
      shared.order.push_back(node);
    }
  }
  return true;
}

// The game's IAnimationPoseModifier, as its feet lock lays it out. The store keeps the relative
// joints of the layers under the clip; the restore puts the lower body back into the finished pose
// and works the absolute joints out again. The finished pose's stages run before the layers of
// the same update, so the restore uses the joints the store kept on the update before.
class LegModifier {
 public:
  LegModifier(std::shared_ptr<Shared> shared, bool restores)
      : shared_(std::move(shared)), restores_(restores) {
  }

  virtual void* query_interface(const void* /*interface_id*/) const {
    return const_cast<LegModifier*>(this);
  }
  virtual void* query_composite(const char* /*name*/) const {
    return nullptr;
  }
  virtual void* factory() const {
    return nullptr;
  }
  virtual ~LegModifier() = default;
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
    Shared& shared = *shared_;
    if (!skeleton || !learn_skeleton(shared, skeleton)) {
      return false;
    }
    const std::size_t count = shared.parents.size();
    QuatT* relative = relative_joints(pose);
    if (!restores_) {
      shared.below.assign(relative, relative + count);
      return true;
    }
    if (shared.below.size() != count) {
      return false;
    }
    // The clip's spine in the character's space: the upper body keeps it, only following where
    // the hips are (the walk turns the hips to the way the body moves, which the clip's spine on
    // top of them would follow).
    shared.clip.resize(count);
    for (const int joint : shared.order) {
      const std::uint16_t parent = shared.parents[joint];
      if (parent == kNoParent) {
        shared.clip[joint] = relative[joint];
      } else {
        shared.clip[joint] = compose(shared.clip[parent], relative[joint]);
      }
    }
    // The root stays the clip's: the walk's root carries the locomotion's own movement (a stop
    // moved it, and the body was drawn a metre or two from where it stood for a frame).
    for (std::size_t joint = 0; joint < count; ++joint) {
      if (!shared.upper[joint] && shared.parents[joint] != kNoParent) {
        relative[joint] = shared.below[joint];
      }
    }
    QuatT* absolute = absolute_joints(pose);
    for (const int joint : shared.order) {
      const std::uint16_t parent = shared.parents[joint];
      if (parent == kNoParent) {
        absolute[joint] = relative[joint];
        continue;
      }
      if (joint == shared.spine) {
        relative_rotation(absolute[parent].q, shared.clip[joint].q, relative[joint].q);
      }
      absolute[joint] = compose(absolute[parent], relative[joint]);
    }
    return true;
  }
  virtual void synchronize() {
  }
  virtual void memory_usage(void* /*sizer*/) const {
  }

 private:
  std::shared_ptr<Shared> shared_;
  bool restores_;
};

struct Body {
  std::shared_ptr<Shared> shared;
  std::shared_ptr<LegModifier> store;
  std::shared_ptr<LegModifier> restore;
  std::chrono::steady_clock::time_point used;
};

std::unordered_map<void*, Body> bodies;

void push_layer(void* animation, unsigned layer, const std::shared_ptr<LegModifier>& modifier);
void push_stage(void* animation, int stage, const std::shared_ptr<LegModifier>& modifier);

// A character's animation pass (its second argument's first field is the character):
// it reads the modifiers pushed for its layers and stages and builds the pass's
// commands from them. A layer modifier pushed after that is never run. keep_legs runs in the
// framework's update, after most passes had read theirs, so the store ran in two passes a second
// (on another client's body almost never) and the legs held one walk pose, then jumped to the next
// The store and the restore are pushed from the pass itself, before the original
// reads them, once each (keep_legs runs several times in one drawn frame for the local body).
using AnimationPass = std::int64_t(__fastcall*)(std::int64_t job, std::uint64_t* pass,
                                                std::int64_t view);
AnimationPass original_pass = nullptr;

// The modifiers the passes push, by character, and when keep_legs last asked for them: a body
// that no longer plays its clip stops getting them after kStoreFor.
struct PassStore {
  std::shared_ptr<LegModifier> store;
  std::shared_ptr<LegModifier> restore;
  std::chrono::steady_clock::time_point kept;
};
constexpr std::chrono::milliseconds kStoreFor{250};
std::mutex pass_stores_mutex;
std::unordered_map<void*, PassStore> pass_stores;
std::atomic<bool> any_pass_store{false};
constexpr std::size_t kMaxPassListeners = 4;
std::atomic<PassListener> pass_listeners[kMaxPassListeners]{};

std::int64_t __fastcall hooked_pass(std::int64_t job, std::uint64_t* pass, std::int64_t view) {
  void* character = pass ? reinterpret_cast<void*>(pass[0]) : nullptr;
  if (character && pass_listeners[0].load(std::memory_order_relaxed)) {
    using Getter = void*(__fastcall*)(void* object);
    if (void* animation = slot<Getter>(character, kSkeletonAnimSlot)(character)) {
      for (const auto& slot_listener : pass_listeners) {
        const PassListener listener = slot_listener.load(std::memory_order_relaxed);
        if (!listener) {
          break;
        }
        listener(character, animation);
      }
    }
  }
  if (character && any_pass_store.load(std::memory_order_relaxed)) {
    std::shared_ptr<LegModifier> store;
    std::shared_ptr<LegModifier> restore;
    {
      std::lock_guard<std::mutex> lock(pass_stores_mutex);
      const auto found = pass_stores.find(character);
      if (found != pass_stores.end() &&
          std::chrono::steady_clock::now() - found->second.kept < kStoreFor) {
        store = found->second.store;
        restore = found->second.restore;
      }
    }
    if (store && restore) {
      using Getter = void*(__fastcall*)(void* object);
      void* animation = slot<Getter>(character, kSkeletonAnimSlot)(character);
      if (animation) {
        push_layer(animation, kWalkLayer, store);
        push_stage(animation, kDrawnPoseStage, restore);
      }
    }
  }
  return original_pass(job, pass, view);
}

// The pass pushes the body's modifiers from now on; old characters are forgotten.
void keep_store(void* character, const Body& body, std::chrono::steady_clock::time_point now) {
  std::lock_guard<std::mutex> lock(pass_stores_mutex);
  for (auto it = pass_stores.begin(); it != pass_stores.end();) {
    if (now - it->second.kept > kForgetAfter) {
      it = pass_stores.erase(it);
    } else {
      ++it;
    }
  }
  pass_stores[character] = PassStore{body.store, body.restore, now};
  any_pass_store.store(true, std::memory_order_relaxed);
}

// Both pushes take the std::shared_ptr by value: the caller makes the copy, the callee lets it go.
std::shared_ptr<LegModifier>* copy_into(void* storage, const std::shared_ptr<LegModifier>& from) {
  return new (storage) std::shared_ptr<LegModifier>(from);
}

void push_layer(void* animation, unsigned layer, const std::shared_ptr<LegModifier>& modifier) {
  using Push = bool(__fastcall*)(void* animation, unsigned layer, void* modifier);
  alignas(std::shared_ptr<LegModifier>) unsigned char storage[sizeof(std::shared_ptr<LegModifier>)];
  slot<Push>(animation, kPushLayerModifierSlot)(animation, layer, copy_into(storage, modifier));
}

void push_stage(void* animation, int stage, const std::shared_ptr<LegModifier>& modifier) {
  using Push = bool(__fastcall*)(void* animation, int stage, void* modifier, const char* name);
  alignas(std::shared_ptr<LegModifier>) unsigned char storage[sizeof(std::shared_ptr<LegModifier>)];
  slot<Push>(animation, kPushStageModifierSlot)(animation, stage, copy_into(storage, modifier),
                                                kModifierName);
}

void forget_old(std::chrono::steady_clock::time_point now) {
  for (auto it = bodies.begin(); it != bodies.end();) {
    if (now - it->second.used > kForgetAfter) {
      it = bodies.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace

bool install(void* animation_pass, Logger logger) {
  if (!animation_pass) {
    if (logger) {
      logger("ANIMATION upper body unavailable: the animation pass was not found");
    }
    return false;
  }
  const auto status = MH_Initialize();
  if ((status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) ||
      MH_CreateHook(animation_pass, reinterpret_cast<void*>(&hooked_pass),
                    reinterpret_cast<void**>(&original_pass)) != MH_OK ||
      MH_EnableHook(animation_pass) != MH_OK) {
    if (logger) {
      logger("ANIMATION upper body unavailable: the animation pass hook failed");
    }
    return false;
  }
  return true;
}

void add_pass_listener(PassListener listener) {
  for (auto& slot_listener : pass_listeners) {
    PassListener empty = nullptr;
    if (slot_listener.compare_exchange_strong(empty, listener)) {
      return;
    }
  }
}

void keep_legs(void* entity, unsigned layer) {
  if (!entity || layer == 0 || layer >= kLayers) {
    return;
  }
  using GetCharacter = void*(__fastcall*)(void* entity, int slot);
  void* character = slot<GetCharacter>(entity, kGetCharacterSlot)(entity, kCharacterSlot);
  if (!character) {
    return;
  }
  using Getter = void*(__fastcall*)(void* object);
  void* animation = slot<Getter>(character, kSkeletonAnimSlot)(character);
  if (!animation) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (bodies.size() > 64) {
    forget_old(now);
  }
  Body& body = bodies[entity];
  if (!body.store) {
    body.shared = std::make_shared<Shared>();
    body.store = std::make_shared<LegModifier>(body.shared, false);
    body.restore = std::make_shared<LegModifier>(body.shared, true);
  }
  body.used = now;
  // The store runs on the walk's own layer: a layer's modifiers run only while it plays
  // something, and the layers between the walk and the clip are mostly empty (a store pushed for
  // one of them kept one stale pose, and the legs stood still in mid-stride while the body moved).
  // The character's pass pushes both (hooked_pass).
  if (original_pass) {
    keep_store(character, body, now);
    return;
  }
  push_layer(animation, kWalkLayer, body.store);
  push_stage(animation, kDrawnPoseStage, body.restore);
}

}  // namespace kcds::runtime::animation::upper_body
