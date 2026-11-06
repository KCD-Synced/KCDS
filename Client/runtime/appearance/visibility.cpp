#include "visibility.hpp"

#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace kcds::runtime::appearance::visibility {
namespace {

using SetInvisible = void(__fastcall*)(void* entity, bool invisible, bool recursive);
using IsInvisible = bool(__fastcall*)(void* entity);

constexpr std::size_t kSetInvisibleSlot = 65;
constexpr std::size_t kIsInvisibleSlot = 66;
constexpr std::size_t kChildren = 152;
// Deeper than anything attached to a body (a prop in a hand, an item on a belt).
constexpr int kMaxDepth = 4;
// More children than any body has; a longer list is not one.
constexpr std::size_t kMaxChildren = 256;

// A body and the entities of it this module made invisible.
struct Body {
  void* entity = nullptr;
  std::unordered_set<void*> hidden;
};

Body local_body;
std::unordered_map<std::uint32_t, Body> replicas;

template <class T>
T read(const void* object, std::size_t offset) {
  T value;
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index]);
}

bool invisible(void* entity) {
  return slot<IsInvisible>(entity, kIsInvisibleSlot)(entity);
}

void set_invisible(void* entity, bool value) {
  slot<SetInvisible>(entity, kSetInvisibleSlot)(entity, value, false);
}

template <class Visit>
void each_child(void* entity, Visit visit) {
  void* const* list = read<void* const*>(entity, kChildren);
  if (!list) {
    return;
  }
  void* const* begin = static_cast<void* const*>(list[0]);
  void* const* end = static_cast<void* const*>(list[1]);
  if (!begin || end < begin || std::size_t(end - begin) > kMaxChildren) {
    return;
  }
  for (void* const* child = begin; child != end; ++child) {
    if (*child) {
      visit(*child);
    }
  }
}

void hide_tree(Body& body, void* entity, int depth) {
  if (!invisible(entity)) {
    set_invisible(entity, true);
    body.hidden.insert(entity);
  }
  if (depth < kMaxDepth) {
    each_child(entity, [&](void* child) { hide_tree(body, child, depth + 1); });
  }
}

void reveal_tree(Body& body, void* entity, int depth) {
  if (body.hidden.count(entity) && invisible(entity)) {
    set_invisible(entity, false);
  }
  if (depth < kMaxDepth) {
    each_child(entity, [&](void* child) { reveal_tree(body, child, depth + 1); });
  }
}

void keep(Body& body, void* entity, bool visible) {
  if (body.entity != entity) {
    // A new body: nothing of it is hidden yet, and the old one's entities may be gone.
    body = Body{};
    body.entity = entity;
  }
  if (!entity) {
    return;
  }
  if (!visible) {
    hide_tree(body, entity, 0);
  } else if (!body.hidden.empty()) {
    reveal_tree(body, entity, 0);
    body.hidden.clear();
  }
}

}  // namespace

void keep_local(void* entity, bool visible) {
  keep(local_body, entity, visible);
}

void keep_replica(std::uint32_t player, void* entity, bool visible) {
  keep(replicas[player], entity, visible);
}

void forget(std::uint32_t player) {
  replicas.erase(player);
}

}  // namespace kcds::runtime::appearance::visibility
