// Joint turns on characters: turns added to joints of a character every frame on
// top of its animation, with the game's own joint operator queue, the way
// CProceduralClipPostProcessAdjust::Update applies its adjustment.
// The studio's posing (studio/studio_pose.hpp) and the players' pose animations
// (pose_animation/player_animations.hpp) use it.
//
//          makes an "AnimationPoseModifier_OperatorQueue" (CryCreateClassInstance)
//                        into a std::shared_ptr<IAnimationOperatorQueue> (pointer, control block;
//                        the uses count at + 8); found by its call in the clip's OnEnter. Each
//                        character has its own queue.
//   character + 40       slot 5, GetISkeletonAnim
//   ISkeletonAnim + 304  slot 38, PushPoseModifier(layer, shared_ptr by value, name): into the
//                        body's place in this frame's animation processing, so it is pushed
//                        when that place is made: in a hook of
//                        CCharInstance::StartAnimationProcessing (slot 19, which
//                        writes the place at + 2776), once per processing (Execute runs what was
//                        pushed before Prepare flips the queue's buffers); pushed at another
//                        time it lands in a place that is not run
//   queue + 112          slot 14, Clear
//   queue + 72           slot 9, PushOrientation(joint, op, const Quat*); op 4 is
//                        AdditiveRelative: the turn is added in the joint's parent's space
//   layer 15             the top layer: the turns come after every animation of the body (the
//                        arms are animated on layers above the base one, which would cover them)
//
// install: once, with the signature scanner; the rest: engine thread.
#pragma once

#include <array>
#include <functional>
#include <vector>

namespace kcds::runtime::pose_animation::joint_turns {

using Logger = void (*)(const char* format, ...);
using Resolver =
    std::function<void*(const char* name, const char* pattern, int displacement, int length)>;

struct Turn {
  int joint = 0;
  // A quaternion x, y, z, w in the joint's parent's space.
  std::array<float, 4> rotation = {0, 0, 0, 1};
};

// False when the queue's maker or the processing hook is not found: no joint is turned then.
bool install(const Resolver& resolve, Logger logger);

// The turns the character's joints get from its next animation processing on; none forgets it.
void set(void* character, std::vector<Turn> turns);

// A hand's grip round a prop (pose_animation/hand_grip.hpp), kept apart from set()'s turns and
// pushed after them on the joints they leave alone; none takes it away.
void set_grip(void* character, std::vector<Turn> turns);

// Forgets a character's turns (its animation ended); with a grip it keeps the grip and its
// queue, else it lets go of the queue.
void forget(void* character);

// Forgets a character's grip (its prop went); without turns it lets go of the queue.
void forget_grip(void* character);

// One joint of the character moved by `shift` metres in its parent's space (COperatorQueue
// PushPosition, slot 8, AdditiveRelative), after its turns: a body kept in place under a
// cutscene's clip (pose_animation/clip_in_place.hpp), the hips lifted by a pose animation's keys
// (pose_animation/player_animations.hpp). A new shift replaces the one before.
void set_shift(void* character, int joint, const std::array<float, 3>& shift);

// Takes the character's shift away; without turns or a grip it lets go of the queue.
void forget_shift(void* character);

// The test builds' layer and operation (studioPose's layer and op): any layer below 16, any
// operation 0 to 4.
void set_layer(unsigned layer, short operation);

}  // namespace kcds::runtime::pose_animation::joint_turns
