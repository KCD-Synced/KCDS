#include "no_gravity.hpp"

#include <intrin.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../physics/line_of_sight.hpp"

namespace kcds::runtime::animation::no_gravity {
namespace {

// The physics component getter returns a std::shared_ptr: the pointer and its control block.
struct SharedPointer {
  void* object;
  void* control;
};

using PhysicsComponent = SharedPointer*(__fastcall*)(void* entity, SharedPointer* out, int create);
using EnablePhysics = void(__fastcall*)(void* component, bool enable);

// The physics component's EnablePhysics (vtable + 192, slot 24).
constexpr std::size_t kEnablePhysics = 192;
// CEntity SetWorldTM(matrix, reason) and GetWorldTM (vtable + 240 and + 256, as NoClip).
constexpr std::size_t kSetWorldTM = 240;
constexpr std::size_t kGetWorldTM = 256;
// The body lands on the first surface within this far below it (metres), this far above it.
constexpr float kLandProbeAbove = 0.5f;
constexpr float kLandProbeBelow = 50.0f;
constexpr float kLandAbove = 0.05f;

void* physics_component = nullptr;
Logger log_line = nullptr;
// The body whose physics this module switched off, null for none.
void* switched_off = nullptr;

// Releases a shared_ptr the game handed out (its control block's use count at + 8).
void release(const SharedPointer& pointer) {
  if (!pointer.control) {
    return;
  }
  auto counts = reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(pointer.control) + 8);
  if (_InterlockedExchangeAdd(counts, -1) == 1) {
    void** vtable = *static_cast<void***>(pointer.control);
    reinterpret_cast<void(__fastcall*)(void*)>(vtable[0])(pointer.control);
    if (_InterlockedExchangeAdd(counts + 1, -1) == 1) {
      reinterpret_cast<void(__fastcall*)(void*)>(vtable[1])(pointer.control);
    }
  }
}

bool set_physics(void* entity, bool enable) {
  SharedPointer component{};
  reinterpret_cast<PhysicsComponent>(physics_component)(entity, &component, 1);
  const bool found = component.object != nullptr;
  if (found) {
    void** vtable = *static_cast<void***>(component.object);
    reinterpret_cast<EnablePhysics>(vtable[kEnablePhysics / sizeof(void*)])(component.object,
                                                                            enable);
  }
  release(component);
  return found;
}

// Puts the body on the first surface below it, before its physics comes back: a body left in the
// air stayed there, and a fall could hurt. False when nothing is below it.
bool land(void* entity) {
  using GetWorldTM = const float*(__fastcall*)(void* entity);
  using SetWorldTM = void(__fastcall*)(void* entity, const float* matrix, unsigned reason);
  void** vtable = *static_cast<void***>(entity);
  float matrix[12];
  std::memcpy(matrix, reinterpret_cast<GetWorldTM>(vtable[kGetWorldTM / sizeof(void*)])(entity),
              sizeof(matrix));
  const float from[3] = {matrix[3], matrix[7], matrix[11] + kLandProbeAbove};
  const float to[3] = {matrix[3], matrix[7], matrix[11] - kLandProbeBelow};
  float distance = 0.0f;
  if (!physics::first_hit(from, to, distance)) {
    return false;
  }
  matrix[11] = from[2] - distance + kLandAbove;
  reinterpret_cast<SetWorldTM>(vtable[kSetWorldTM / sizeof(void*)])(entity, matrix, 0);
  return true;
}

}  // namespace

void install(void* given, Logger logger) {
  physics_component = given;
  log_line = logger;
}

void keep(void* entity, bool wanted) {
  if (!physics_component || !entity) {
    return;
  }
  if (wanted && switched_off != entity) {
    if (set_physics(entity, false)) {
      switched_off = entity;
      if (log_line) {
        log_line("ANIMATION the body has no gravity while the request runs");
      }
    }
    return;
  }
  if (!wanted && switched_off == entity) {
    land(entity);
    set_physics(entity, true);
    switched_off = nullptr;
    if (log_line) {
      log_line("ANIMATION the body has its gravity again");
    }
  }
}

void reset() {
  switched_off = nullptr;
}

}  // namespace kcds::runtime::animation::no_gravity
