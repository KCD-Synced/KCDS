// Clips that should move only the upper body (the cuffed pose; upper-body animations
// while walking). The game plays a clip on the whole skeleton: the cuffed pose
// (hands_tied_back_idle on the script clip layer 12) also holds the hips, legs and feet of its own
// standing pose, so a cuffed player who walked slid on stiff legs a few centimetres too high,
// with a foot stretched flat along the ground.
//
// keep_legs uses two pose modifiers of our own. The store, pushed for the walk's layer 0
// (ISkeletonAnim::PushPoseModifier for a layer, slot 38; a layer's modifiers run only
// while it plays something), keeps the relative joints of the walk. The character's animation
// pass reads the layers' modifiers when it starts, before the framework's update
// where keep_legs runs, so the store is pushed from that pass (install hooks it): pushed from
// keep_legs it ran in two passes a second, and the legs held one walk pose and then jumped. The
// pass pushes the restore too, once (keep_legs runs several times a frame for the local body). The
// restore, pushed for the finished pose (its stage 2, slot 37: a layer's modifiers work
// on that layer and never reach the pose the body is drawn with) gives every joint that is not the
// spine or under it (the root, the hips, the legs, the feet and the toes) the walk's: a body that
// stands has the walk layer's idle stance. The clip's own standing legs, and a blend from them to
// the walk's by the body's speed, looked stretched (long, stiff legs) and splayed while the blend
// ran. The clip's spine keeps its orientation in the character's
// space and only follows where the hips are: the walk turns the hips to the way the body moves
// (sideways, backwards), and the clip's upper body set on those hips twisted and leant over, and
// the player's own camera with its head. The absolute joints are worked out again. The finished
// pose's stages run before the layers, so the legs are those of the update before.
//
// Any clip that should move only the upper body uses the same: keep_legs for its
// layer every frame it plays.
//
// The modifiers follow the game's IAnimationPoseModifier (as its feet lock, CFeetPoseStore
// QueryInterface, QueryComposite, GetFactory, destructor, Prepare, Execute,
// Synchronize, GetMemoryUsage) and read the pose as it does.
//
// Engine thread only (the modifiers themselves run in the game's animation jobs).
#pragma once

namespace kcds::runtime::animation::upper_body {

using Logger = void (*)(const char* format, ...);

// Hooks the character's animation pass so the walk's store reaches every pass. Without
// it the store is pushed from keep_legs and the legs move in steps. False when not installed.
bool install(void* animation_pass, Logger logger);

// Called by every character's animation pass before it reads its modifiers, with the character and
// its ISkeletonAnim, so another feature can push its own (female_retarget, pointing),
// in the order they were added (up to four). The passes run on the engine and in animation jobs:
// a listener must be quick and thread-safe. Add them at startup, before the passes run.
using PassListener = void (*)(void* character, void* animation);
void add_pass_listener(PassListener listener);

// For a body that plays an upper-body clip on `layer` this frame: everything but the spine and
// what hangs from it keeps the pose of the layers under it. Call it every frame the clip plays.
void keep_legs(void* entity, unsigned layer);

}  // namespace kcds::runtime::animation::upper_body
