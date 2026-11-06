#include "joint_turns.hpp"

#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

// After the headers that use std::min and std::max: MinHook brings windows.h and its macros.
#include "MinHook.h"

namespace kcds::runtime::pose_animation::joint_turns {
namespace {

constexpr std::size_t kSkeletonAnimSlot = 5;
constexpr std::size_t kPushPoseModifierSlot = 38;
constexpr std::size_t kClearSlot = 14;
constexpr std::size_t kPushOrientationSlot = 9;
// COperatorQueue slot 8: PushPosition(joint, op, const Vec3*),
// the same entry as PushOrientation's with its kind 0 and a position.
constexpr std::size_t kPushPositionSlot = 8;
constexpr short kAdditiveRelative = 4;
constexpr unsigned kTopLayer = 15;
// CCharInstance's place in this frame's animation processing (-1 when it is not queued).
constexpr std::size_t kProcessingSlot = 2776;
constexpr std::size_t kMostCharacters = 256;

struct SharedPointer {
  void* object = nullptr;
  void* control = nullptr;
};

struct Character {
  SharedPointer queue;
  std::vector<Turn> turns;
  // A hand's grip round a prop (set_grip), under the turns: a joint both turn takes the turns'.
  std::vector<Turn> grip;
  // One joint moved by a vector in its parent's space (set_shift), joint -1 for none.
  int shift_joint = -1;
  std::array<float, 3> shift = {0, 0, 0};
};

bool has_turns(const Character& state) {
  return !state.turns.empty() || !state.grip.empty() || state.shift_joint >= 0;
}

using MakeQueue = bool(__fastcall*)(void* unused, SharedPointer* out);
using StartProcessing = void(__fastcall*)(void* character, void* params);
using Get = void*(__fastcall*)(void* object);
using PushPoseModifier = bool(__fastcall*)(void* animation, unsigned layer, SharedPointer* modifier,
                                           const char* name);
using Clear = void(__fastcall*)(void* queue);
using PushOrientation = void(__fastcall*)(void* queue, unsigned joint, short op,
                                          const float* rotation);
using PushPosition = void(__fastcall*)(void* queue, unsigned joint, short op,
                                       const float* position);

template <typename Function>
Function slot(void* object, std::size_t index) {
  return reinterpret_cast<Function>((*static_cast<void***>(object))[index]);
}

Logger write_log = nullptr;
MakeQueue make_queue = nullptr;
StartProcessing original_start = nullptr;
bool told_failure = false;
std::atomic<unsigned> layer{kTopLayer};
std::atomic<int> operation_kind{kAdditiveRelative};

std::mutex characters_mutex;
std::unordered_map<void*, Character> characters;
// Whether any character has turns (the hook's quick way out).
std::atomic<bool> any{false};

// With characters_mutex held.
void update_any() {
  any.store(std::any_of(characters.begin(), characters.end(),
                        [](const auto& entry) { return has_turns(entry.second); }));
}

int processing_slot(void* character) {
  return *reinterpret_cast<const int*>(static_cast<char*>(character) + kProcessingSlot);
}

// Lets go of our reference to a queue (the control block's uses count at + 8; the game frees
// the queue with the last one).
void release(SharedPointer& pointer) {
  if (!pointer.control) {
    return;
  }
  auto counts = reinterpret_cast<volatile long*>(static_cast<char*>(pointer.control) + 8);
  if (_InterlockedExchangeAdd(counts, -1) == 1) {
    void** vtable = *static_cast<void***>(pointer.control);
    reinterpret_cast<void(__fastcall*)(void*)>(vtable[0])(pointer.control);
    if (_InterlockedExchangeAdd(counts + 1, -1) == 1) {
      reinterpret_cast<void(__fastcall*)(void*)>(vtable[1])(pointer.control);
    }
  }
  pointer = {};
}

// The modifier and the turns into the body's processing that just started.
void push(void* character, Character& state) {
  if (!state.queue.object) {
    if (!make_queue(nullptr, &state.queue) || !state.queue.object) {
      if (!told_failure && write_log) {
        write_log("POSE no joint operator queue was made");
      }
      told_failure = true;
      return;
    }
  }
  void* animation = slot<Get>(character, kSkeletonAnimSlot)(character);
  if (!animation) {
    return;
  }
  // PushPoseModifier takes its shared_ptr by value and lets go of it: it gets a reference of its
  // own.
  SharedPointer handed = state.queue;
  if (handed.control) {
    _InterlockedIncrement(reinterpret_cast<volatile long*>(static_cast<char*>(handed.control) + 8));
  }
  if (!slot<PushPoseModifier>(animation, kPushPoseModifierSlot)(animation, layer.load(), &handed,
                                                                "KcdsJointTurns")) {
    return;
  }
  void* queue = state.queue.object;
  slot<Clear>(queue, kClearSlot)(queue);
  const short how = short(operation_kind.load());
  for (const Turn& turn : state.turns) {
    slot<PushOrientation>(queue, kPushOrientationSlot)(queue, unsigned(turn.joint), how,
                                                       turn.rotation.data());
  }
  // The grip on the joints the turns leave alone.
  for (const Turn& turn : state.grip) {
    const bool turned = std::any_of(state.turns.begin(), state.turns.end(),
                                    [&](const Turn& other) { return other.joint == turn.joint; });
    if (!turned) {
      slot<PushOrientation>(queue, kPushOrientationSlot)(queue, unsigned(turn.joint), how,
                                                         turn.rotation.data());
    }
  }
  // The joint moved (a body kept in place under a cutscene's clip).
  if (state.shift_joint >= 0) {
    slot<PushPosition>(queue, kPushPositionSlot)(queue, unsigned(state.shift_joint),
                                                 kAdditiveRelative, state.shift.data());
  }
}

// CCharInstance::StartAnimationProcessing (slot 19): queues the body for this
// frame's animation (its processing place at + 2776) and starts the job. A body with turns newly
// queued gets them right then, where the game's own pose modifiers go.
void __fastcall hooked_start(void* character, void* params) {
  if (!any.load()) {
    original_start(character, params);
    return;
  }
  const bool queued_before = processing_slot(character) >= 0;
  original_start(character, params);
  if (queued_before || processing_slot(character) < 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(characters_mutex);
  const auto found = characters.find(character);
  if (found != characters.end() && has_turns(found->second)) {
    push(character, found->second);
  }
}

}  // namespace

bool install(const Resolver& resolve, Logger logger) {
  write_log = logger;
  // CProceduralClipPostProcessAdjust's OnEnter: the joint's id kept at + 76, then
  // the queue made into + 96.
  void* maker = resolve("CryCreateClassInstance<IAnimationOperatorQueue>",
                        "66 85 C0 78 0F 48 8D 53 60 66 89 43 4C E8 ?? ?? ?? ?? EB", 14, 18);
  void* start = resolve("CCharInstance::StartAnimationProcessing",
                        "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D AC 24 50 FB "
                        "FF FF 48 81 EC B0 05 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 A0 04 "
                        "00 00 4C 8B FA 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0",
                        -1, 0);
  make_queue = reinterpret_cast<MakeQueue>(maker);
  const bool hooked = start && make_queue &&
                      MH_CreateHook(start, reinterpret_cast<void*>(&hooked_start),
                                    reinterpret_cast<void**>(&original_start)) == MH_OK &&
                      MH_EnableHook(start) == MH_OK;
  if (!hooked) {
    make_queue = nullptr;
    if (write_log) {
      write_log("POSE joints cannot be turned (queue maker %d, processing %d)",
                int(maker != nullptr), int(start != nullptr));
    }
  }
  return hooked;
}

void set(void* character, std::vector<Turn> turns) {
  if (!character || !make_queue) {
    return;
  }
  std::lock_guard<std::mutex> lock(characters_mutex);
  if (turns.empty()) {
    const auto found = characters.find(character);
    if (found != characters.end()) {
      found->second.turns.clear();
    }
  } else if (characters.size() < kMostCharacters || characters.count(character) != 0) {
    characters[character].turns = std::move(turns);
  }
  update_any();
}

void set_grip(void* character, std::vector<Turn> turns) {
  if (!character || !make_queue) {
    return;
  }
  std::lock_guard<std::mutex> lock(characters_mutex);
  if (turns.empty()) {
    const auto found = characters.find(character);
    if (found != characters.end()) {
      found->second.grip.clear();
    }
  } else if (characters.size() < kMostCharacters || characters.count(character) != 0) {
    characters[character].grip = std::move(turns);
  }
  update_any();
}

void forget(void* character) {
  std::lock_guard<std::mutex> lock(characters_mutex);
  const auto found = characters.find(character);
  if (found == characters.end()) {
    return;
  }
  // A grip or a shift stays with the body (its prop is still in the hand, its clip plays): only
  // the turns go.
  if (!found->second.grip.empty() || found->second.shift_joint >= 0) {
    found->second.turns.clear();
    update_any();
    return;
  }
  release(found->second.queue);
  characters.erase(found);
  update_any();
}

void forget_grip(void* character) {
  std::lock_guard<std::mutex> lock(characters_mutex);
  const auto found = characters.find(character);
  if (found == characters.end()) {
    return;
  }
  found->second.grip.clear();
  if (found->second.turns.empty() && found->second.shift_joint < 0) {
    release(found->second.queue);
    characters.erase(found);
  }
  update_any();
}

void set_shift(void* character, int joint, const std::array<float, 3>& shift) {
  if (!character || !make_queue || joint < 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(characters_mutex);
  if (characters.size() < kMostCharacters || characters.count(character) != 0) {
    Character& state = characters[character];
    state.shift_joint = joint;
    state.shift = shift;
  }
  update_any();
}

void forget_shift(void* character) {
  std::lock_guard<std::mutex> lock(characters_mutex);
  const auto found = characters.find(character);
  if (found == characters.end()) {
    return;
  }
  found->second.shift_joint = -1;
  if (found->second.turns.empty() && found->second.grip.empty()) {
    release(found->second.queue);
    characters.erase(found);
  }
  update_any();
}

void set_layer(unsigned which, short operation) {
  if (which < 16) {
    layer.store(which);
  }
  if (operation >= 0 && operation <= 4) {
    operation_kind.store(operation);
  }
}

}  // namespace kcds::runtime::pose_animation::joint_turns
