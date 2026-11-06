#include "shared_ladders.hpp"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <utility>

#include "MinHook.h"

namespace kcds::runtime::movement::ladders {

namespace {

// The ladder helper's actor (+ 56).
constexpr std::size_t kHelperActor = 56;
// GrabOnLadder's owner (+ 8) and its actor getter (slot 2), as GrabOnLadder itself asks.
constexpr std::size_t kGrabOwner = 8;
constexpr std::size_t kOwnerActor = 2;
// A body on a ladder: closer than this along it, two bodies overlap.
constexpr float kBody = 2.0f;
// How close across of the local player a remote player is on the same ladder, when the ladder is
// not known (the helper's grab, and the climb).
constexpr float kAcross = 1.6f;
// How close to a known ladder's axis a remote player is on it, and how far along it.
constexpr float kFromAxis = 1.0f;
constexpr float kBelowFoot = 1.0f;
constexpr float kAxisLength = 15.0f;
// The climb's actions: up and down a ladder.
constexpr const char* kClimbUp = "moveforward";
constexpr const char* kClimbDown = "moveback";

using HelperGrab = std::int64_t(__fastcall*)(void* helper, void* ladder, float direction);
using GrabById = char(__fastcall*)(void* self, std::uint32_t ladder);
using CanUse = char(__fastcall*)(void* actor, void* ladder, float height, char top, char flag);
using ActorOf = void*(__fastcall*)(void* owner);
using EntityId = std::uint32_t(__fastcall*)(void* entity);
// An entity's id (IEntity vtable + 8, as CanUseLadder itself asks).
constexpr std::size_t kEntityId = 1;

HelperGrab original_helper_grab = nullptr;
GrabById original_grab_by_id = nullptr;
CanUse original_can_use = nullptr;
Logger log_line = nullptr;
EntityMatrix matrix_of = nullptr;
DWORD engine_thread = 0;
void* local = nullptr;
std::array<float, 3> local_position{};
bool local_on_ladder = false;
std::vector<Climber> on_ladders;
// The climb action the local player holds (up or down), to let go of when it would overlap.
struct HeldClimb {
  void* input = nullptr;
  const char** action = nullptr;
};
HeldClimb held_climb;

float dot(const float* a, const float* b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Whether a remote player on the ladder whose entity is `ladder` is within a body of where the
// local player takes it (its foot or its top); a grab refused for it is logged.
bool overlaps_on(std::uint32_t ladder, bool grab) {
  float m[12] = {};
  if (!matrix_of || !matrix_of(ladder, m)) {
    return false;
  }
  const float foot[3] = {m[3], m[7], m[11]};
  float up[3] = {m[2], m[6], m[10]};
  const float length = std::sqrt(dot(up, up));
  if (length < 0.001f) {
    return false;
  }
  for (float& axis : up) {
    axis /= length;
  }
  const float from_local[3] = {local_position[0] - foot[0], local_position[1] - foot[1],
                               local_position[2] - foot[2]};
  const float local_along = dot(from_local, up);
  for (const Climber& climber : on_ladders) {
    const float to[3] = {climber.position[0] - foot[0], climber.position[1] - foot[1],
                         climber.position[2] - foot[2]};
    const float along = dot(to, up);
    if (along < -kBelowFoot || along > kAxisLength) {
      continue;
    }
    const float off[3] = {to[0] - along * up[0], to[1] - along * up[1], to[2] - along * up[2]};
    const float across = std::sqrt(dot(off, off));
    if (across <= kFromAxis && std::fabs(along - local_along) < kBody) {
      if (log_line && grab) {
        log_line(
            "LADDER local player's grab refused: player=%u is on it within a body (%.2f m "
            "from its axis, %.2f m up it, the local player at %.2f m)",
            climber.player, across, along, local_along);
      }
      return true;
    }
  }
  return false;
}

// Whether a remote player on a ladder is within a body of the local player's place (the
// helper's grab, which names no entity), and how far up (> 0) or down it is.
bool overlaps_here(float* rise) {
  for (const Climber& climber : on_ladders) {
    const float dx = climber.position[0] - local_position[0];
    const float dy = climber.position[1] - local_position[1];
    const float dz = climber.position[2] - local_position[2];
    if (dx * dx + dy * dy <= kAcross * kAcross && std::fabs(dz) < kBody) {
      if (rise) {
        *rise = dz;
      }
      return true;
    }
  }
  return false;
}

bool local_grab(void* actor) {
  return actor && actor == local && GetCurrentThreadId() == engine_thread;
}

std::int64_t __fastcall hooked_helper_grab(void* helper, void* ladder, float direction) {
  void* actor = helper
                    ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(helper) + kHelperActor)
                    : nullptr;
  if (local_grab(actor) && overlaps_here(nullptr)) {
    if (log_line) {
      log_line("LADDER local player's grab refused: a remote player on it is within a body");
    }
    return 0;
  }
  return original_helper_grab(helper, ladder, direction);
}

char __fastcall hooked_grab_by_id(void* self, std::uint32_t ladder) {
  void* owner =
      self ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(self) + kGrabOwner) : nullptr;
  void* actor = owner ? (*static_cast<ActorOf**>(owner))[kOwnerActor](owner) : nullptr;
  if (local_grab(actor) && overlaps_on(ladder, true)) {
    return 0;
  }
  return original_grab_by_id(self, ladder);
}

char __fastcall hooked_can_use(void* actor, void* ladder, float height, char top, char flag) {
  if (local_grab(actor) && ladder) {
    const std::uint32_t id = (*static_cast<EntityId**>(ladder))[kEntityId](ladder);
    if (overlaps_on(id, false)) {
      static ULONGLONG next_log = 0;
      if (log_line && GetTickCount64() >= next_log) {
        next_log = GetTickCount64() + 1000;
        log_line("LADDER ladder %u not offered: a remote player is on it within a body", id);
      }
      return 0;
    }
  }
  return original_can_use(actor, ladder, height, top, flag);
}

bool hook(void* target, void* detour, void** original) {
  return target && MH_CreateHook(target, detour, original) == MH_OK &&
         MH_EnableHook(target) == MH_OK;
}

// The climb direction an action is (1 up, -1 down, 0 neither).
int climb_of(const char* action) {
  if (std::strcmp(action, kClimbUp) == 0) {
    return 1;
  }
  if (std::strcmp(action, kClimbDown) == 0) {
    return -1;
  }
  return 0;
}

// Whether climbing that way would take the local player into a remote player on its ladder.
bool climb_blocked(int direction) {
  float rise = 0;
  if (!local_on_ladder || direction == 0 || !overlaps_here(&rise)) {
    return false;
  }
  return direction > 0 ? rise > 0 : rise < 0;
}

}  // namespace

bool install(void* helper_grab, void* grab_by_id, void* can_use, EntityMatrix entity_matrix,
             Logger logger) {
  log_line = logger;
  matrix_of = entity_matrix;
  return hook(helper_grab, reinterpret_cast<void*>(hooked_helper_grab),
              reinterpret_cast<void**>(&original_helper_grab)) &&
         hook(grab_by_id, reinterpret_cast<void*>(hooked_grab_by_id),
              reinterpret_cast<void**>(&original_grab_by_id)) &&
         hook(can_use, reinterpret_cast<void*>(hooked_can_use),
              reinterpret_cast<void**>(&original_can_use));
}

bool refuse_action(void* input, const char** action, unsigned mode) {
  if (!action || !*action) {
    return false;
  }
  const int direction = climb_of(*action);
  if (direction == 0) {
    return false;
  }
  if (mode == 2) {
    if (held_climb.action == action) {
      held_climb = {};
    }
    return false;
  }
  // 1 the press, 4 the key held on.
  if (mode != 1 && mode != 4) {
    return false;
  }
  if (climb_blocked(direction)) {
    if (log_line && mode == 1) {
      log_line("LADDER local player's climb %s held back: a remote player is within a body",
               direction > 0 ? "up" : "down");
    }
    return true;
  }
  if (mode == 1) {
    held_climb = {input, action};
  }
  return false;
}

void tick(void* local_actor, const std::array<float, 3>& position, bool on_ladder,
          std::vector<Climber> climbers, Deliver deliver) {
  engine_thread = GetCurrentThreadId();
  local = local_actor;
  local_position = position;
  local_on_ladder = on_ladder;
  on_ladders = std::move(climbers);
  if (!on_ladder) {
    held_climb = {};
    return;
  }
  if (held_climb.action && deliver && climb_blocked(climb_of(*held_climb.action))) {
    const HeldClimb releasing = held_climb;
    held_climb = {};
    deliver(releasing.input, releasing.action, 2, 0.0f);
    if (log_line) {
      log_line("LADDER local player's climb stopped: a remote player is within a body");
    }
  }
}

}  // namespace kcds::runtime::movement::ladders
