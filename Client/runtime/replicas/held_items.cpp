// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "held_items.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "remote.hpp"

namespace kcds::game_client {

namespace {

// IEntity GetCharacter(slot), the body's character in slot 0; its mesh slot's model and the
// slot's flags (bits 1 and 2 draw it, as appearance/disguise.cpp clears them).
constexpr std::size_t kGetCharacter = 101;
constexpr std::size_t kGetStatObj = 103;
constexpr std::size_t kSetSlotFlags = 98;
constexpr std::size_t kGetSlotFlags = 99;
constexpr int kMeshSlot = 0;
constexpr std::uint32_t kDrawBits = 3;
// ICharacterInstance GetIAttachmentManager; IAttachmentManager CreateAttachment(name, type,
// joint, force) with type 0 a joint's attachment, and GetInterfaceByName.
constexpr std::size_t kAttachmentManager = 9;
constexpr std::size_t kCreateAttachment = 2;
constexpr std::size_t kAttachmentByName = 8;
constexpr int kJointAttachment = 0;
// IAttachment (CAttachmentBONE): its place on the joint, absolute (slot 9, kept at + 272) or
// relative (slot 11, kept at + 244; the flag at + 242 says which), HideAttachment, AddBinding,
// the bound object, ClearBinding; its joint's name (a string at + 104).
constexpr std::size_t kSetAbsolute = 9;
constexpr std::size_t kSetRelative = 11;
constexpr std::size_t kAbsolutePlace = 272;
constexpr std::size_t kRelativePlace = 244;
constexpr std::size_t kPlaceIsRelative = 242;
constexpr std::size_t kHideAttachment = 17;
constexpr std::size_t kAddBinding = 26;
constexpr std::size_t kBoundObject = 28;
constexpr std::size_t kClearBinding = 30;
constexpr std::size_t kJointName = 104;
// IAttachmentObject GetAttachmentType, an entity binding's type, and its entity's id (+ 8).
constexpr std::size_t kAttachmentType = 1;
constexpr int kEntityBinding = 4;
constexpr std::size_t kBoundEntity = 8;
// A CCGFAttachment: 24 bytes, its mesh's reference at + 8 (IStatObj AddRef, slot 1).
constexpr std::size_t kMeshBindingBytes = 24;
constexpr std::size_t kMeshBindingMesh = 8;
constexpr std::size_t kAddRef = 1;

using Allocate = void*(__fastcall*)(std::size_t size);
Allocate allocate = nullptr;
void* mesh_binding_table = nullptr;

template <class Fn>
Fn slot_of(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index]);
}

template <class T>
T at(void* object, std::size_t offset) {
  return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset);
}

// An item entity one of a replica's attachments holds (a hand, a scabbard, a back slot), drawn on
// its skeleton by a mirror attachment: the entity's id (0: none) and its mesh slot's flags before.
struct Held {
  void* mirror = nullptr;
  std::string mirror_name;
  std::uint32_t entity = 0;
  std::uint32_t flags = 0;
};
struct ReplicaItems {
  // By the game's attachment that holds the entity.
  std::unordered_map<void*, Held> by_source;
  std::unordered_set<void*> mirrors;
  unsigned next_mirror = 0;
};
// Engine thread only: by the body's engine id.
std::unordered_map<std::uint32_t, ReplicaItems> held;
// Attachments looked at per body at most (a body has a few dozen).
constexpr unsigned kMostAttachments = 256;
constexpr std::size_t kAttachmentByIndex = 9;

void* entity_system_now() {
  void* framework = framework_slot ? *framework_slot : nullptr;
  void* actor_system =
      framework ? at<void*>(framework, 1304) : nullptr;  // as replicas/deferred_moves.cpp
  return actor_system ? at<void*>(actor_system, 48) : nullptr;
}

// The entity a hand's attachment holds (an entity binding), or 0.
std::uint32_t entity_in(void* hand) {
  using Get = void*(__fastcall*)(void* object);
  using Type = int(__fastcall*)(void* binding);
  void* bound = hand ? slot_of<Get>(hand, kBoundObject)(hand) : nullptr;
  if (!bound || slot_of<Type>(bound, kAttachmentType)(bound) != kEntityBinding) {
    return 0;
  }
  return at<std::uint32_t>(bound, kBoundEntity);
}

// The attachment `name` on the hand's joint, where the hand's attachment sits; null when it cannot
// be made.
void* mirror_of(void* manager, void* hand, const char* name) {
  using ByName = void*(__fastcall*)(void* manager, const char* name);
  using Create =
      void*(__fastcall*)(void* manager, const char* name, int type, const char* joint, char force);
  using Place = char(__fastcall*)(void* attachment, const void* place);
  void* mirror = slot_of<ByName>(manager, kAttachmentByName)(manager, name);
  if (!mirror) {
    const char* joint = at<const char*>(hand, kJointName);
    if (!joint || !*joint) {
      return nullptr;
    }
    mirror = slot_of<Create>(manager, kCreateAttachment)(manager, name, kJointAttachment, joint, 0);
    if (!mirror) {
      return nullptr;
    }
  }
  auto* bytes = static_cast<std::uint8_t*>(hand);
  if (bytes[kPlaceIsRelative]) {
    slot_of<Place>(mirror, kSetRelative)(mirror, bytes + kRelativePlace);
  } else {
    slot_of<Place>(mirror, kSetAbsolute)(mirror, bytes + kAbsolutePlace);
  }
  return mirror;
}

// Hangs `mesh` on the attachment as the game hangs a mesh by name.
bool bind_mesh(void* mirror, void* mesh) {
  if (!allocate || !mesh_binding_table) {
    return false;
  }
  void* memory = allocate(kMeshBindingBytes);
  if (!memory) {
    return false;
  }
  auto* words = static_cast<void**>(memory);
  words[0] = mesh_binding_table;
  words[1] = nullptr;
  words[2] = nullptr;
  using AddRef = void(__fastcall*)(void* mesh);
  slot_of<AddRef>(mesh, kAddRef)(mesh);
  *reinterpret_cast<void**>(static_cast<std::uint8_t*>(memory) + kMeshBindingMesh) = mesh;
  using AddBinding =
      std::uint32_t(__fastcall*)(void* attachment, void* object, void* skin, std::uint32_t flags);
  using Hide = void(__fastcall*)(void* attachment, std::uint32_t hidden);
  slot_of<AddBinding>(mirror, kAddBinding)(mirror, memory, nullptr, 0);
  slot_of<Hide>(mirror, kHideAttachment)(mirror, 0);
  return true;
}

std::uint32_t slot_flags(void* entity) {
  using Get = std::uint32_t(__fastcall*)(void* entity, int slot);
  return slot_of<Get>(entity, kGetSlotFlags)(entity, kMeshSlot);
}

void set_slot_flags(void* entity, std::uint32_t flags) {
  using Set = void(__fastcall*)(void* entity, int slot, std::uint32_t flags);
  slot_of<Set>(entity, kSetSlotFlags)(entity, kMeshSlot, flags);
}

// One attachment of one replica: the item it holds now against what its skeleton draws.
void tick_attachment(void* entity_system, void* manager, void* source, Held& state,
                     ReplicaItems& items) {
  using GetStatObj = void*(__fastcall*)(void* entity, int slot);
  using Clear = void(__fastcall*)(void* attachment, std::uint32_t flags);
  const std::uint32_t id = entity_in(source);
  void* item = id ? get_entity(entity_system, id) : nullptr;
  if (id != state.entity) {
    // What the attachment held before draws itself again, and its mesh leaves the skeleton.
    if (state.mirror) {
      slot_of<Clear>(state.mirror, kClearBinding)(state.mirror, 0);
    }
    if (void* before = state.entity ? get_entity(entity_system, state.entity) : nullptr) {
      set_slot_flags(before, slot_flags(before) | (state.flags & kDrawBits));
    }
    state.entity = 0;
    state.flags = 0;
    void* mesh = item ? slot_of<GetStatObj>(item, kGetStatObj)(item, kMeshSlot) : nullptr;
    if (mesh && state.mirror_name.empty()) {
      state.mirror_name = "kcds_item_" + std::to_string(items.next_mirror++);
    }
    // The mirror sits where the source does (placed again for a new item).
    void* mirror = mesh ? mirror_of(manager, source, state.mirror_name.c_str()) : nullptr;
    if (mirror) {
      state.mirror = mirror;
      items.mirrors.insert(mirror);
    }
    if (mirror && bind_mesh(mirror, mesh)) {
      state.entity = id;
      state.flags = slot_flags(item);
    }
  }
  // The entity's own mesh stays hidden while the skeleton draws it (the game may draw it again).
  if (state.entity && item) {
    const std::uint32_t flags = slot_flags(item);
    if (flags & kDrawBits) {
      set_slot_flags(item, flags & ~kDrawBits);
    }
  }
}

}  // namespace

bool install_held_items() {
  allocate = reinterpret_cast<Allocate>(
      resolve_unique("GameAllocate",
                     "40 53 48 83 EC 20 83 3D ?? ?? ?? ?? 00 48 8B D9 75 05 E8 ?? ?? ?? ?? 48 8B "
                     "05 ?? ?? ?? ?? 48 8D 54 24 38 45 33 C0 FF D0"));
  // CCGFAttachment's table, from the game's attachment of a mesh by name (its lea).
  mesh_binding_table =
      resolve_unique("MeshAttachmentTable",
                     "48 85 FF 74 1A 48 8D 05 ?? ?? ?? ?? 48 C7 47 08 00 00 00 00 48 89 07", 8, 12);
  return allocate && mesh_binding_table;
}

void tick_replica_held_items() {
  void* entity_system = entity_system_now();
  if (!entity_system || !allocate || !mesh_binding_table) {
    return;
  }
  using GetCharacter = void*(__fastcall*)(void* entity, int slot);
  using Get = void*(__fastcall*)(void* object);
  using ByIndex = void*(__fastcall*)(void* manager, unsigned index);
  std::unordered_set<std::uint32_t> bodies;
  for (const auto& [id, remote] : remotes) {
    bodies.insert(remote.engine_id);
  }
  for (auto it = held.begin(); it != held.end();) {
    it = bodies.count(it->first) ? std::next(it) : held.erase(it);
  }
  std::vector<void*> sources;
  for (const auto& [id, remote] : remotes) {
    void* body = remote.engine_id ? get_entity(entity_system, remote.engine_id) : nullptr;
    void* character = body ? slot_of<GetCharacter>(body, kGetCharacter)(body, 0) : nullptr;
    void* manager = character ? slot_of<Get>(character, kAttachmentManager)(character) : nullptr;
    if (!manager) {
      continue;
    }
    auto& items = held[remote.engine_id];
    // Listed first: a new mirror joins the manager's list.
    sources.clear();
    for (unsigned index = 0; index < kMostAttachments; ++index) {
      void* attachment = slot_of<ByIndex>(manager, kAttachmentByIndex)(manager, index);
      if (!attachment) {
        break;
      }
      if (items.mirrors.count(attachment)) {
        continue;
      }
      if (entity_in(attachment) || items.by_source.count(attachment)) {
        sources.push_back(attachment);
      }
    }
    for (void* source : sources) {
      tick_attachment(entity_system, manager, source, items.by_source[source], items);
    }
  }
}

}  // namespace kcds::game_client
