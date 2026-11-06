// Horse.speedScale: a server horse moves at a share of its pace in every gait.
//
// A body's animated character hands the physics, each frame, how far its locomotion moves it
// (CAnimatedCharacter::UpdatePhysicalEntityMovement: the frame's QuatT, whose
// translation becomes the living entity's pe_action_move velocity). The game's horse speed stats
// are not read on that way (derived stats 31 to 34 and 116 have no reader by id), nor does the
// surface multiplier (actor + 380) reach a ridden horse. The hook shortens that translation for a
// scaled horse, so it covers that share of the ground walking, trotting or galloping, ridden or
// led, while its legs keep the gait's animation.
#pragma once

namespace kcds::runtime::riding::horse_speed_scale {

using Logger = void (*)(const char* format, ...);

// The movement hand-over: resolved and hooked once; false without it.
bool install(Logger log);

// The scale of one horse's body (its IEntity); 1 (or below 0) is the game's own pace.
void set(void* entity, float scale);

// The horse's body is gone.
void forget(void* entity);

}  // namespace kcds::runtime::riding::horse_speed_scale
