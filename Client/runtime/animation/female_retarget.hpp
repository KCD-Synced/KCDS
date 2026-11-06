// A player woman's body on the male clips. A player woman is drawn on our copy of the
// female skeleton, kcds_player_female.chr, but plays the player's male clips
// (appearance/female_skeleton.hpp). A clip holds each joint's rotation relative to its parent as
// the male skeleton has it, and the two skeletons point their joints' axes differently in the bind
// pose (a hand's own rotation 105 degrees apart, its fingers 5 to 41, its bones 76 to 88 degrees
// apart in the character's space): the male rotations bent her fingers into a claw and turned her
// hands.
//
// A pose modifier on the pose the body is drawn with (stage 2, as upper_body's restore) moves each
// joint's rotation from the male bind pose to the female one in the character's space:
//
//   female = female_bind_abs(parent)^-1 * male_bind_abs(parent) * clip
//            * male_bind_abs(joint)^-1 * female_bind_abs(joint)
//
// so every joint turns from its bind pose as the male joint turns from his. It is pushed by every
// character's animation pass (upper_body::add_pass_listener) whose skeleton is
// kcds_player_female.chr: the local player, other players and the bodies left behind. The game's
// own women (female.chr, playing the female clips) are left alone. The male bind pose comes from
// the first body seen on the game's male.chr (Henry, any man).
//
// A clip of the female set on the script clip layer (the fragments only the game's women have)
// is made for her skeleton: while one plays, the modifier stays out.
//
// The modifier runs in the game's animation jobs.
#pragma once

namespace kcds::runtime::animation::female_retarget {

using Logger = void (*)(const char* format, ...);

// Starts listening to the characters' animation pass.
void install(Logger logger);

// Engine thread: a clip started on a body's ISkeletonAnim. Tells the modifier whether its script
// clip layer plays a female clip now.
void note_clip_start(void* skeleton, int clip, unsigned layer);

}  // namespace kcds::runtime::animation::female_retarget
