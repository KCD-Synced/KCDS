#include "player_attach.hpp"

#include <intrin.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace kcds::runtime::attach {
namespace {

// CEntity: SetWorldTM(matrix, reason), GetWorldTM; its physics component's EnablePhysics(bool).
constexpr std::size_t kSetWorldTM = 240;
constexpr std::size_t kGetWorldTM = 256;
constexpr std::size_t kEnablePhysics = 192;
constexpr double kPi = 3.14159265358979323846;
// The actions an attached player keeps: looking around and the menus.
constexpr const char* kKeptPrefixes[] = {"rotate", "menu", "pause"};
constexpr unsigned kReleased = 2;

using SetWorldTM = void(__fastcall*)(void* entity, const float* matrix, unsigned reason);
using GetWorldTM = const float*(__fastcall*)(void* entity);
using EnablePhysics = void(__fastcall*)(void* component, bool enable);
struct SharedPointer {
  void* object;
  void* control;
};
using PhysicsComponent = SharedPointer*(__fastcall*)(void* entity, SharedPointer* out, int create);

Natives natives;
Logger log_line = nullptr;
// The order about the local player, and those about the others.
kcd2s::integration::PlayerAttach order;
std::unordered_map<std::uint32_t, kcd2s::integration::PlayerAttach> others;
// The body whose physics this took away, while attached.
void* held_body = nullptr;
// The replicas whose physics this took away: the player's id and the entity.
std::unordered_map<std::uint32_t, void*> held_replicas;

template <typename Function>
Function slot(void* object, std::size_t offset) {
  return *reinterpret_cast<Function*>(*static_cast<std::uint8_t**>(object) + offset);
}

void set_physics(void* entity, bool enable) {
  if (!natives.physics_component) {
    return;
  }
  SharedPointer component{};
  reinterpret_cast<PhysicsComponent>(natives.physics_component)(entity, &component, 1);
  if (!component.object) {
    return;
  }
  slot<EnablePhysics>(component.object, kEnablePhysics)(component.object, enable);
  // The getter hands out a reference: the control block's release, as std::shared_ptr's.
  if (component.control) {
    auto* counts =
        reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(component.control) + 8);
    if (_InterlockedDecrement(counts) == 0) {
      slot<void(__fastcall*)(void*)>(component.control, 0)(component.control);
      auto* weak =
          reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(component.control) + 12);
      if (_InterlockedDecrement(weak) == 0) {
        slot<void(__fastcall*)(void*)>(component.control, 8)(component.control);
      }
    }
  }
}

void release_body() {
  if (!held_body) {
    return;
  }
  set_physics(held_body, true);
  held_body = nullptr;
  if (log_line) {
    log_line("ATTACH body released");
  }
}

}  // namespace

void install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
}

void apply_order(const kcd2s::integration::PlayerAttach& given) {
  if (given.flags & kcd2s::integration::kAttachIsLocalPlayer) {
    order = given;
  } else if (given.target == 0) {
    others.erase(given.player);
  } else {
    others[given.player] = given;
  }
}

void clear() {
  order = {};
  others.clear();
  // The world is going: its entities go with it.
  held_body = nullptr;
  held_replicas.clear();
}

namespace {

// The attached replicas without physics; one no longer attached gets it back.
void keep_replicas() {
  for (auto it = held_replicas.begin(); it != held_replicas.end();) {
    void* entity = natives.replica_entity(it->first);
    if (others.count(it->first) && entity == it->second) {
      ++it;
      continue;
    }
    if (entity == it->second) {
      set_physics(entity, true);
    }
    it = held_replicas.erase(it);
  }
  for (const auto& [player, attached] : others) {
    (void)attached;
    if (held_replicas.count(player)) {
      continue;
    }
    if (void* entity = natives.replica_entity(player)) {
      set_physics(entity, false);
      held_replicas[player] = entity;
      if (log_line) {
        log_line("ATTACH replica of player %u: physics off", unsigned(player));
      }
    }
  }
}

}  // namespace

const kcd2s::integration::PlayerAttach* order_of(std::uint32_t player) {
  const auto found = others.find(player);
  return found == others.end() ? nullptr : &found->second;
}

bool local_pose(BodyPose& pose) {
  void* body = natives.local_entity ? natives.local_entity() : nullptr;
  if (!body) {
    return false;
  }
  const float* matrix = slot<GetWorldTM>(body, kGetWorldTM)(body);
  pose.x = matrix[3];
  pose.y = matrix[7];
  pose.z = matrix[11];
  pose.heading = static_cast<float>(std::atan2(double(matrix[4]), double(matrix[0])));
  return true;
}

BodyPose beside(const kcd2s::integration::PlayerAttach& given, const BodyPose& target) {
  const double c = std::cos(double(target.heading));
  const double s = std::sin(double(target.heading));
  BodyPose pose;
  pose.x = target.x + static_cast<float>(given.right * c - given.forward * s);
  pose.y = target.y + static_cast<float>(given.right * s + given.forward * c);
  pose.z = target.z + given.up;
  pose.heading = target.heading + static_cast<float>(double(given.yaw) * kPi / 180.0);
  return pose;
}

void frame(bool in_world) {
  if (!in_world || !natives.local_entity || !natives.replica_entity) {
    held_body = nullptr;
    return;
  }
  keep_replicas();
  if (order.target == 0) {
    release_body();
    return;
  }
  void* body = natives.local_entity();
  void* target = natives.replica_entity(order.target);
  if (!body || !target) {
    return;
  }
  if (held_body != body) {
    set_physics(body, false);
    held_body = body;
    if (log_line) {
      log_line("ATTACH body attached to player %u", unsigned(order.target));
    }
  }
  const float* from = slot<GetWorldTM>(target, kGetWorldTM)(target);
  // The target's heading: the rows of its matrix, flat on the ground.
  BodyPose seen;
  seen.x = from[3];
  seen.y = from[7];
  seen.z = from[11];
  seen.heading = static_cast<float>(std::atan2(double(from[4]), double(from[0])));
  const BodyPose place = beside(order, seen);
  // The body turns with the target whatever the look: a rider stays on the shoulders the right way.
  const float c = std::cos(place.heading);
  const float s = std::sin(place.heading);
  const float matrix[12] = {c, -s, 0, place.x, s, c, 0, place.y, 0, 0, 1, place.z};
  slot<SetWorldTM>(body, kSetWorldTM)(body, matrix, 0);
}

bool free_look_wanted() {
  return order.target != 0 && (order.flags & kcd2s::integration::kAttachFollowLook) == 0;
}

bool refuse_action(const char* action, unsigned mode) {
  if (order.target == 0 || !action || mode == kReleased) {
    return false;
  }
  for (const char* prefix : kKeptPrefixes) {
    if (std::strncmp(action, prefix, std::strlen(prefix)) == 0) {
      // look 'follow': no looking around either.
      const bool turning = std::strncmp(action, "rotate", 6) == 0;
      return turning && (order.flags & kcd2s::integration::kAttachFollowLook) != 0;
    }
  }
  return true;
}

}  // namespace kcds::runtime::attach
