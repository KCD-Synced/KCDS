#include "skin_binding.hpp"

#include <cstddef>

namespace kcds::runtime::appearance::skin_binding {
namespace {

// IEntity GetCharacter(slot): the body's character in slot 0.
constexpr std::size_t kEntityCharacter = 101;
// ICharacterInstance GetIAttachmentManager.
constexpr std::size_t kAttachmentManager = 9;
// IAttachmentManager CreateAttachment(name, type), GetInterfaceByName(name) and
// GetInterfaceByIndex(index).
constexpr std::size_t kCreateAttachment = 2;
constexpr std::size_t kAttachmentByName = 8;
constexpr std::size_t kAttachmentByIndex = 9;
constexpr std::uint32_t kSkinAttachment = 2;
// IAttachment AddBinding(object, skin, flags), its attachment object, its skin interface and
// ClearBinding(flags).
constexpr std::size_t kAddBinding = 26;
constexpr std::size_t kBoundObject = 28;
constexpr std::size_t kSkinInterface = 29;
constexpr std::size_t kClearBinding = 30;
constexpr std::uint32_t kBindFlags = 2048;
// ICharacterManager LoadModelSKIN(out skin, path, flags).
constexpr std::size_t kLoadSkin = 6;
// A CSKINAttachment: 64 bytes, its attachment's skin interface at + 8; the material drawn for a
// level of detail and the replacement setter.
constexpr std::size_t kSkinObjectBytes = 64;
constexpr std::size_t kSkinObjectOwner = 8;
constexpr std::size_t kReplacements = 16;
constexpr std::size_t kDrawnMaterial = 14;
constexpr std::size_t kReplaceMaterial = 15;

Natives natives;

template <typename Fn>
Fn slot_of(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

}  // namespace

void install(const Natives& given) {
  natives = given;
}

bool installed() {
  return natives.character_manager && natives.allocate && natives.skin_attachment;
}

void* attachment_manager(void* entity) {
  if (!entity) {
    return nullptr;
  }
  using GetCharacter = void*(__fastcall*)(void* entity, int slot);
  void* character = slot_of<GetCharacter>(entity, kEntityCharacter)(entity, 0);
  if (!character) {
    return nullptr;
  }
  using Get = void*(__fastcall*)(void* object);
  return slot_of<Get>(character, kAttachmentManager)(character);
}

void* named_attachment(void* manager, const char* name) {
  using ByName = void*(__fastcall*)(void* manager, const char* name);
  return slot_of<ByName>(manager, kAttachmentByName)(manager, name);
}

void* own_attachment(void* manager, const char* name) {
  void* attachment = named_attachment(manager, name);
  if (attachment) {
    return attachment;
  }
  using Create = void*(__fastcall*)(void* manager, const char* name, std::uint32_t type);
  return slot_of<Create>(manager, kCreateAttachment)(manager, name, kSkinAttachment);
}

void* attachment_at(void* manager, std::uint32_t index) {
  using ByIndex = void*(__fastcall*)(void* manager, std::uint32_t index);
  return slot_of<ByIndex>(manager, kAttachmentByIndex)(manager, index);
}

void* bound_object(void* attachment) {
  using Get = void*(__fastcall*)(void* attachment);
  return slot_of<Get>(attachment, kBoundObject)(attachment);
}

bool skin_object(void* object) {
  return object && natives.skin_object_table &&
         *static_cast<void**>(object) == natives.skin_object_table;
}

void* load_skin(const std::string& path) {
  void* manager = natives.character_manager ? *natives.character_manager : nullptr;
  if (!manager) {
    return nullptr;
  }
  void* skin = nullptr;
  using LoadSkin =
      void**(__fastcall*)(void* manager, void** out, const char* path, std::uint32_t flags);
  slot_of<LoadSkin>(manager, kLoadSkin)(manager, &skin, path.c_str(), 0);
  return skin;
}

bool bind(void* attachment, void* skin) {
  if (!installed()) {
    return false;
  }
  using Allocate = void*(__fastcall*)(std::size_t size);
  void* memory = reinterpret_cast<Allocate>(natives.allocate)(kSkinObjectBytes);
  if (!memory) {
    return false;
  }
  using Construct = void*(__fastcall*)(void* memory);
  void* object = reinterpret_cast<Construct>(natives.skin_attachment)(memory);
  using Get = void*(__fastcall*)(void* attachment);
  void* owner = slot_of<Get>(attachment, kSkinInterface)(attachment);
  if (owner) {
    using AddRef = void(__fastcall*)(void* owner);
    slot_of<AddRef>(owner, 0)(owner);
  }
  *reinterpret_cast<void**>(static_cast<std::uint8_t*>(object) + kSkinObjectOwner) = owner;
  using AddBinding =
      std::uint32_t(__fastcall*)(void* attachment, void* object, void* skin, std::uint32_t flags);
  return slot_of<AddBinding>(attachment, kAddBinding)(attachment, object, skin, kBindFlags) != 0;
}

void clear(void* attachment) {
  using ClearBinding = void(__fastcall*)(void* attachment, std::uint32_t flags);
  slot_of<ClearBinding>(attachment, kClearBinding)(attachment, 0);
}

void* drawn_material(void* object, int lod) {
  if (!skin_object(object) || lod < 0 || lod >= kLods) {
    return nullptr;
  }
  using Get = void*(__fastcall*)(void* object, std::uint32_t lod);
  return slot_of<Get>(object, kDrawnMaterial)(object, std::uint32_t(lod));
}

void* replacement(void* object, int lod) {
  if (!skin_object(object) || lod < 0 || lod >= kLods) {
    return nullptr;
  }
  return *reinterpret_cast<void**>(static_cast<std::uint8_t*>(object) + kReplacements +
                                   std::size_t(lod) * sizeof(void*));
}

void replace_material(void* object, int lod, void* material) {
  if (!skin_object(object) || lod < 0 || lod >= kLods) {
    return;
  }
  using Set = void(__fastcall*)(void* object, void* material, std::uint32_t lod);
  slot_of<Set>(object, kReplaceMaterial)(object, material, std::uint32_t(lod));
}

}  // namespace kcds::runtime::appearance::skin_binding
