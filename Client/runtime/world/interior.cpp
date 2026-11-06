#include "interior.hpp"

#include <cstddef>
#include <cstdint>

namespace kcds::runtime::world::interior {
namespace {

using GetObject = void* (*)();
using GetMember = void* (*)(void* self);
using MakeString = void** (*)(void** string, const char* text);
using HasContext = std::uint64_t (*)(void* contexts, void* key, void** name);
using FreeString = void (*)(void** string);

constexpr std::size_t kContextOwner = 0x160;
constexpr std::size_t kContextSource = 0xA8;
constexpr std::size_t kSlotContextHolder = 0x10 / 8;
constexpr std::size_t kSlotContextKey = 0;

Natives natives;

void** vtable_of(void* object) {
  return *static_cast<void***>(object);
}

void* member_at(void* object, std::size_t offset) {
  return *reinterpret_cast<void**>(static_cast<std::uint8_t*>(object) + offset);
}

// The key the game files the local player's contexts under, null while there is none.
void* player_key() {
  void* environment = reinterpret_cast<GetObject>(natives.get_environment)();
  if (!environment) {
    return nullptr;
  }
  void* owner = member_at(environment, kContextOwner);
  if (!owner) {
    return nullptr;
  }
  void* source = member_at(owner, kContextSource);
  if (!source) {
    return nullptr;
  }
  void* holder = reinterpret_cast<GetMember>(vtable_of(source)[kSlotContextHolder])(source);
  if (!holder) {
    return nullptr;
  }
  return reinterpret_cast<GetMember>(vtable_of(holder)[kSlotContextKey])(holder);
}

}  // namespace

bool install(const Natives& found, Logger logger) {
  natives = found;
  const bool complete = natives.get_environment && natives.contexts && natives.make_string &&
                        natives.has_context && natives.free_string;
  if (!complete) {
    natives = Natives{};
  }
  if (logger) {
    logger("WORLD interior %s", complete ? "installed" : "unavailable");
  }
  return complete;
}

bool player_inside() {
  if (!natives.has_context) {
    return false;
  }
  void* key = player_key();
  void* contexts = reinterpret_cast<GetObject>(natives.contexts)();
  if (!key || !contexts) {
    return false;
  }
  void* name = nullptr;
  reinterpret_cast<MakeString>(natives.make_string)(&name, "interior");
  const bool inside =
      (reinterpret_cast<HasContext>(natives.has_context)(contexts, key, &name) & 0xFF) != 0;
  reinterpret_cast<FreeString>(natives.free_string)(&name);
  return inside;
}

}  // namespace kcds::runtime::world::interior
