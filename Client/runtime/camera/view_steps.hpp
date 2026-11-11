// The F5 key's steps through the views: the first person, then the third person close behind the
// shoulder, farther, and farthest, then the first person again. Where the server forces the third
// person, the key steps through the three distances only. Pure; the camera (third_person.cpp)
// keeps the step and places the camera at its distance.
#pragma once

namespace kcds::runtime::camera::view_steps {

// 0 is the first person; 1 to kThirdPersonSteps the third person, nearest first.
inline constexpr int kThirdPersonSteps = 3;

// The step a third person the key did not bring stands at (a game screen closing, the server
// forcing it, a script letting it go): the middle one.
inline constexpr int kUsualStep = 2;

// How far behind the point over the shoulders the camera stands at steps 1 to 3 (metres).
inline constexpr float kDistances[kThirdPersonSteps] = {1.5f, 2.4f, 4.0f};

// The step after a press of the key at `step`. `first_person_allowed`: false while the server
// forces the third person.
inline int next(int step, bool first_person_allowed) {
  if (step < 0 || step >= kThirdPersonSteps) {
    return first_person_allowed ? 0 : 1;
  }
  return step + 1;
}

// The camera's distance at `step`; the usual step's for the first person or a step out of range.
inline float distance(int step) {
  if (step < 1 || step > kThirdPersonSteps) {
    return kDistances[kUsualStep - 1];
  }
  return kDistances[step - 1];
}

}  // namespace kcds::runtime::camera::view_steps
