#include "prop_physics.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace kcds::runtime::props::physics {
namespace {

constexpr std::size_t kGetPhysics = 79;
constexpr std::size_t kGetStatus = 6;
constexpr std::size_t kAction = 7;

constexpr std::int32_t kActionImpulse = 2;
constexpr std::int32_t kActionAwake = 8;
constexpr std::int32_t kActionVelocity = 10;
constexpr std::int32_t kStatusAwake = 7;
constexpr std::int32_t kStatusDynamics = 8;

constexpr std::uint32_t kUnusedFloat = 0xFFBFFFFFu;
constexpr std::int32_t kUnusedInt = std::int32_t(0x80000000u);

// pe_action_impulse
constexpr std::size_t kImpulse = 4;
constexpr std::size_t kImpulseAngular = 16;
constexpr std::size_t kImpulsePoint = 28;
constexpr std::size_t kImpulsePartId = 40;
constexpr std::size_t kImpulsePart = 44;
constexpr std::size_t kImpulseApplyTime = 48;
constexpr std::size_t kImpulseSource = 52;
// pe_action_awake
constexpr std::size_t kAwake = 4;
constexpr std::size_t kAwakeMinTime = 8;
// pe_action_set_velocity
constexpr std::size_t kVelocityPart = 4;
constexpr std::size_t kVelocityPartId = 8;
constexpr std::size_t kVelocity = 12;
constexpr std::size_t kVelocitySpin = 24;
constexpr std::size_t kVelocityAroundPivot = 36;
// pe_status_awake, pe_status_dynamics
constexpr std::size_t kStatusLag = 4;
constexpr std::size_t kDynamicsPartId = 4;
constexpr std::size_t kDynamicsPart = 8;
constexpr std::size_t kDynamicsVelocity = 12;
constexpr std::size_t kDynamicsMass = 76;

constexpr std::size_t kBufferBytes = 128;

using GetPhysics = void*(__fastcall*)(void* entity);
using Action = int(__fastcall*)(void* body, void* action, int thread_safe);
using GetStatus = int(__fastcall*)(void* body, void* status);

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index]);
}

template <class T>
void put(std::uint8_t* buffer, std::size_t offset, const T& value) {
  std::memcpy(buffer + offset, &value, sizeof(T));
}

void put_floats(std::uint8_t* buffer, std::size_t offset, const float* values) {
  std::memcpy(buffer + offset, values, 3 * sizeof(float));
}

void put_unused_floats(std::uint8_t* buffer, std::size_t offset) {
  for (std::size_t i = 0; i < 3; ++i) {
    put(buffer, offset + i * sizeof(float), kUnusedFloat);
  }
}

bool act(void* body, std::uint8_t* action) {
  return body && slot<Action>(body, kAction)(body, action, 0) != 0;
}

}  // namespace

void* body_of(void* entity) {
  return entity ? slot<GetPhysics>(entity, kGetPhysics)(entity) : nullptr;
}

bool push(void* body, const std::array<float, 3>& impulse, const std::array<float, 3>& angular,
          const float* point) {
  alignas(16) std::uint8_t action[kBufferBytes] = {};
  put(action, 0, kActionImpulse);
  put_floats(action, kImpulse, impulse.data());
  put_floats(action, kImpulseAngular, angular.data());
  if (point) {
    put_floats(action, kImpulsePoint, point);
  } else {
    put_unused_floats(action, kImpulsePoint);
  }
  put(action, kImpulsePartId, kUnusedInt);
  put(action, kImpulsePart, kUnusedInt);
  put(action, kImpulseApplyTime, std::int32_t(0));
  put(action, kImpulseSource, std::int32_t(0));
  return set_awake(body, true) && act(body, action);
}

bool set_velocity(void* body, const std::array<float, 3>& velocity,
                  const std::array<float, 3>& spin) {
  alignas(16) std::uint8_t action[kBufferBytes] = {};
  put(action, 0, kActionVelocity);
  put(action, kVelocityPart, kUnusedInt);
  put(action, kVelocityPartId, kUnusedInt);
  put_floats(action, kVelocity, velocity.data());
  put_floats(action, kVelocitySpin, spin.data());
  put(action, kVelocityAroundPivot, std::int32_t(0));
  return set_awake(body, true) && act(body, action);
}

bool set_awake(void* body, bool awake) {
  alignas(16) std::uint8_t action[kBufferBytes] = {};
  put(action, 0, kActionAwake);
  put(action, kAwake, std::int32_t(awake ? 1 : 0));
  put(action, kAwakeMinTime, kUnusedFloat);
  return act(body, action);
}

bool awake(void* body) {
  if (!body) {
    return false;
  }
  alignas(16) std::uint8_t status[kBufferBytes] = {};
  put(status, 0, kStatusAwake);
  put(status, kStatusLag, std::int32_t(0));
  return slot<GetStatus>(body, kGetStatus)(body, status) != 0;
}

// The whole body's pe_status_dynamics; false when the body has none.
bool dynamics(void* body, std::uint8_t* status) {
  if (!body) {
    return false;
  }
  put(status, 0, kStatusDynamics);
  // The whole body's, not one part's.
  put(status, kDynamicsPartId, kUnusedInt);
  put(status, kDynamicsPart, kUnusedInt);
  return slot<GetStatus>(body, kGetStatus)(body, status) != 0;
}

float mass(void* body) {
  alignas(16) std::uint8_t status[kBufferBytes] = {};
  if (!dynamics(body, status)) {
    return 0.0f;
  }
  float value = 0.0f;
  std::memcpy(&value, status + kDynamicsMass, sizeof(value));
  return value;
}

bool velocity(void* body, std::array<float, 3>& out) {
  out = {};
  alignas(16) std::uint8_t status[kBufferBytes] = {};
  if (!dynamics(body, status)) {
    return false;
  }
  std::memcpy(out.data(), status + kDynamicsVelocity, sizeof(float) * 3);
  for (const float value : out) {
    if (!std::isfinite(value)) {
      out = {};
      return false;
    }
  }
  return true;
}

}  // namespace kcds::runtime::props::physics
