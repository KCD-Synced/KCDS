#include "seat_collisions.hpp"

#include <intrin.h>

#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::seats::collisions {
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

Natives natives;
Logger log_line = nullptr;
bool installed = false;
// The replicas whose physics is switched off, by player.
std::unordered_map<std::uint32_t, std::uint32_t> switched_off;
// The replicas whose physics could not be switched, told once each.
std::unordered_map<std::uint32_t, std::uint32_t> refused;

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

// Switches an entity's physics on or off; false when it has no physics component.
bool set_physics(std::uint32_t entity_id, bool enable) {
  void* entity = natives.entity_by_id ? natives.entity_by_id(entity_id) : nullptr;
  if (!entity) {
    return false;
  }
  SharedPointer component{};
  // As the entity's own lookup asks (NoClip): the component, made when the entity has none.
  reinterpret_cast<PhysicsComponent>(natives.physics_component)(entity, &component, 1);
  const bool found = component.object != nullptr;
  if (found) {
    void** vtable = *static_cast<void***>(component.object);
    reinterpret_cast<EnablePhysics>(vtable[kEnablePhysics / sizeof(void*)])(component.object,
                                                                            enable);
  }
  release(component);
  return found;
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  installed = natives.physics_component && natives.entity_by_id;
  return installed;
}

void keep(std::uint32_t player, std::uint32_t entity_id, bool replica_seated, bool local_seated) {
  if (!installed || !entity_id) {
    return;
  }
  const bool off = replica_seated || local_seated;
  const auto found = switched_off.find(player);
  const bool was_off = found != switched_off.end() && found->second == entity_id;
  if (off == was_off) {
    return;
  }
  if (!set_physics(entity_id, !off)) {
    if (log_line && refused[player] != entity_id) {
      refused[player] = entity_id;
      log_line("SEATS player=%u body %u has no physics to switch", player, entity_id);
    }
    return;
  }
  if (off) {
    switched_off[player] = entity_id;
  } else {
    switched_off.erase(player);
  }
  if (log_line) {
    log_line("SEATS player=%u body %s (%s)", player, off ? "passes through" : "collides again",
             replica_seated ? "it sits" : (local_seated ? "the local player sits" : "both stand"));
  }
}

namespace {

// The players a client script took out of the collisions.
std::mutex script_mutex;
std::unordered_set<std::uint32_t> script_off;

}  // namespace

bool handle_runtime_message(const kcd2s::sdk::Value& message) {
  if (kcd2s::integration::script_operation(message) != "playerCollision") {
    return false;
  }
  const kcd2s::sdk::Value* id = kcd2s::integration::script_field(message, "id");
  const kcd2s::sdk::Value* collides = kcd2s::integration::script_field(message, "collides");
  if (!id || !collides || collides->tag() != kcd2s::sdk::Tag::Bool) {
    return true;
  }
  double number = -1;
  if (id->tag() == kcd2s::sdk::Tag::Int) {
    number = double(id->as_int());
  } else if (id->tag() == kcd2s::sdk::Tag::Double) {
    number = id->as_double();
  }
  if (!(number > 0 && number <= 4294967295.0)) {
    return true;
  }
  const auto player = std::uint32_t(number);
  std::lock_guard<std::mutex> lock(script_mutex);
  if (collides->as_bool()) {
    script_off.erase(player);
  } else {
    script_off.insert(player);
  }
  if (log_line) {
    log_line("SEATS player=%u body collisions %s by a client script", player,
             collides->as_bool() ? "back" : "off");
  }
  return true;
}

bool script_passes_through(std::uint32_t player) {
  std::lock_guard<std::mutex> lock(script_mutex);
  return script_off.count(player) != 0;
}

void forget(std::uint32_t player) {
  switched_off.erase(player);
  refused.erase(player);
}

void reset() {
  switched_off.clear();
  refused.clear();
}

}  // namespace kcds::runtime::seats::collisions
