// A rigid prop's body through the game's physics, WHGame 1.5.5.
//
//   CEntity          vtable slot 79 GetPhysics: its IPhysicalEntity
//   CRigidEntity: slot 6 GetStatus, slot 7 Action
//   pe_action        the type at + 0; an unused float holds 0xFFBFFFFF (the game tests the bits
//                    against 0xFFA00000), an unused int 0x80000000
//     impulse (2)    impulse + 4, angular impulse + 16, point + 28, part id + 40, part + 44,
//                    apply time + 48 (0: now), source + 52
//     awake (8)      awake + 4, minimum awake time + 8 (CPhysicalEntity's, which calls Awake)
//     velocity (10)  part + 4, part id + 8, v + 12, w + 24, around the pivot + 36
//   pe_status        awake (7): lag + 4, the result is whether the body moves; dynamics (8): its
//                    velocity at + 12, its mass at + 76
//
// Engine thread.
#pragma once

#include <array>

namespace kcds::runtime::props::physics {

// The entity's physical body, or null.
void* body_of(void* entity);

// Pushes the body: an impulse (kg m/s) with an angular impulse (kg m^2/s) on top, at the point
// in world space when there is one (else the centre of mass). Wakes it.
bool push(void* body, const std::array<float, 3>& impulse, const std::array<float, 3>& angular,
          const float* point);

// Sets the body's velocity (m/s) and spin (rad/s) outright. Wakes it.
bool set_velocity(void* body, const std::array<float, 3>& velocity,
                  const std::array<float, 3>& spin);

// Wakes the body where it lies, or puts it to sleep where it is.
bool set_awake(void* body, bool awake);

// Whether the body moves (it sleeps once it came to rest).
bool awake(void* body);

// The body's mass in kilograms; 0 for none (a mesh whose collision carries no mass).
float mass(void* body);

// The body's velocity in m/s; false (and none) when it has none.
bool velocity(void* body, std::array<float, 3>& out);

}  // namespace kcds::runtime::props::physics
