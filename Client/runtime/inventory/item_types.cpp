#include "item_types.hpp"

#include <malloc.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "MinHook.h"

namespace kcds::runtime::inventory {
namespace {

namespace integration = kcd2s::integration;

// A miscellaneous item's class and the fields a type changes.
constexpr std::size_t kClassBytes = 0xE0;
constexpr std::size_t kClassGuid = 0x08;
constexpr std::size_t kClassName = 0x18;
constexpr std::size_t kClassModel = 0x38;
// The model's material, "" for the mesh's own (animation_props reads the same pair).
constexpr std::size_t kClassMaterial = 0x40;
constexpr std::size_t kClassWeight = 0x58;
constexpr std::size_t kClassPrice = 0x5C;
constexpr std::size_t kClassIcon = 0x90;
constexpr std::size_t kClassDescriptionKey = 0x98;
constexpr std::size_t kClassNameKey = 0xA0;
constexpr std::size_t kClassStackable = 0xB0;
// The class's stacking getter.
constexpr std::size_t kStackableSlot = 7;
// A miscellaneous item every game has (animal_animalPulp), the base of a type whose visual is
// not one.
constexpr char kReferenceMisc[] = "3650cb5f-c380-4eba-89aa-06be675c4dff";
// A CryString's header before its text; a count this high is never released to zero.
constexpr std::int32_t kNeverFreed = 0x3FFFFFFF;

using Lookup = void*(__fastcall*)(void* registry, const void* guid);

struct GuidHash {
  std::size_t operator()(const integration::Guid& guid) const {
    std::uint64_t low = 0;
    std::uint64_t high = 0;
    std::memcpy(&low, guid.data(), 8);
    std::memcpy(&high, guid.data() + 8, 8);
    return std::size_t(low ^ (high * 0x9E3779B97F4A7C15ull));
  }
};
// A type's class, whether it is usable and its use label key.
struct Made {
  void* item_class = nullptr;
  bool usable = false;
  std::string use_label_key;
};
using Classes = std::unordered_map<integration::Guid, Made, GuidHash>;

Logger log_line = nullptr;
Lookup original_lookup = nullptr;
std::atomic<void*> registry{nullptr};
// The registered types' ids and GUIDs for the resource runtime, how often they changed, and what
// a session's runtime last heard.
std::mutex known_mutex;
// A registered type as the resource runtime hears it: its GUID, icon and icon file.
struct KnownType {
  integration::Guid id{};
  std::string icon;
  std::string icon_file;
};
std::map<std::string, KnownType> known_types;
std::uint64_t known_revision = 0;
const void* heard_session = nullptr;
std::uint64_t heard_revision = 0;
// The registered types' classes, replaced whole (the old maps are kept) so the hook reads them
// without a lock.
std::atomic<const Classes*> classes{nullptr};
std::mutex apply_mutex;
// Types that came before the game looked any class up.
std::vector<integration::ItemTypes> waiting;

void* __fastcall lookup(void* registry_object, const void* guid) {
  if (!registry.load(std::memory_order_relaxed)) {
    registry.store(registry_object, std::memory_order_relaxed);
  }
  if (const Classes* made = classes.load(std::memory_order_acquire)) {
    integration::Guid key;
    std::memcpy(key.data(), guid, key.size());
    const auto found = made->find(key);
    if (found != made->end()) {
      return found->second.item_class;
    }
  }
  return original_lookup(registry_object, guid);
}

template <class Value>
void put(std::uint8_t* object, std::size_t offset, const Value& value) {
  std::memcpy(object + offset, &value, sizeof(value));
}

void* slot_of(const void* object, std::size_t index) {
  return (*static_cast<void* const* const*>(object))[index];
}

// A CryString's text the game may copy and never frees.
const char* lasting_string(const std::string& text) {
  auto* block = static_cast<std::uint8_t*>(::operator new(12 + text.size() + 1));
  const std::int32_t header[3] = {kNeverFreed, std::int32_t(text.size()),
                                  std::int32_t(text.size())};
  std::memcpy(block, header, sizeof(header));
  std::memcpy(block + 12, text.data(), text.size());
  block[12 + text.size()] = 0;
  return reinterpret_cast<const char*>(block + 12);
}

void* make_class(void* registry_object, const integration::ItemType& type) {
  alignas(16) integration::Guid visual_guid = type.visual;
  alignas(16) integration::Guid reference_guid{};
  if (!integration::parse_guid(kReferenceMisc, reference_guid)) {
    return nullptr;
  }
  auto* visual = static_cast<std::uint8_t*>(original_lookup(registry_object, visual_guid.data()));
  auto* reference =
      static_cast<std::uint8_t*>(original_lookup(registry_object, reference_guid.data()));
  if (!visual || !reference) {
    return nullptr;
  }
  const bool miscellaneous = slot_of(visual, kStackableSlot) == slot_of(reference, kStackableSlot);
  auto* made = static_cast<std::uint8_t*>(_aligned_malloc(kClassBytes, 16));
  std::memcpy(made, miscellaneous ? visual : reference, kClassBytes);
  if (!miscellaneous) {
    std::memcpy(made + kClassModel, visual + kClassModel, sizeof(void*));
    std::memcpy(made + kClassIcon, visual + kClassIcon, sizeof(void*));
  }
  std::memcpy(made + kClassGuid, type.id.data(), type.id.size());
  put(made, kClassName, lasting_string(type.name));
  put(made, kClassWeight, type.weight);
  put(made, kClassPrice, type.price);
  put(made, kClassDescriptionKey, lasting_string(type.description_key));
  put(made, kClassNameKey, lasting_string(type.name_key));
  if (!type.icon.empty()) {
    put(made, kClassIcon, lasting_string(type.icon));
  }
  // A mesh of its own: the ground, the shops and the held item draw the class's
  // model, with its own material rather than the one the visual's model wears.
  if (!type.model.empty()) {
    put(made, kClassModel, lasting_string(type.model));
    put(made, kClassMaterial, lasting_string(std::string()));
  }
  made[kClassStackable] = type.stackable ? 1 : 0;
  return made;
}

void remember(const integration::ItemTypes& types) {
  std::lock_guard<std::mutex> lock(known_mutex);
  if (types.full) {
    known_types.clear();
  }
  for (const integration::ItemType& type : types.types) {
    known_types[type.name] = KnownType{type.id, type.icon, type.icon_file};
  }
  known_revision += 1;
}

void make(const integration::ItemTypes& types, void* registry_object) {
  const Classes* before = classes.load(std::memory_order_acquire);
  auto* next = (types.full || !before) ? new Classes() : new Classes(*before);
  if (types.full) {
    next->clear();
  }
  unsigned made = 0;
  for (const integration::ItemType& type : types.types) {
    if (void* item_class = make_class(registry_object, type)) {
      (*next)[type.id] = Made{item_class, type.usable, type.use_label_key};
      ++made;
    } else if (log_line) {
      log_line("INVENTORY item type %s: its visual is not in the game", type.name.c_str());
    }
  }
  classes.store(next, std::memory_order_release);
  if (log_line) {
    log_line("INVENTORY item types: %u made (%s), %zu in all", made, types.full ? "full" : "change",
             next->size());
  }
}

}  // namespace

bool install_item_types(void* game_lookup, Logger logger) {
  log_line = logger;
  if (!game_lookup) {
    return false;
  }
  return MH_CreateHook(game_lookup, reinterpret_cast<void*>(lookup),
                       reinterpret_cast<void**>(&original_lookup)) == MH_OK &&
         MH_EnableHook(game_lookup) == MH_OK;
}

void* game_item_class(const integration::Guid& id) {
  void* registry_object = registry.load(std::memory_order_relaxed);
  if (!registry_object || !original_lookup) {
    return nullptr;
  }
  alignas(16) integration::Guid guid = id;
  return original_lookup(registry_object, guid.data());
}

bool usable_type(const void* item_class, std::string& use_label_key) {
  const Classes* made = classes.load(std::memory_order_acquire);
  if (!made || !item_class) {
    return false;
  }
  integration::Guid key;
  std::memcpy(key.data(), static_cast<const std::uint8_t*>(item_class) + kClassGuid, key.size());
  const auto found = made->find(key);
  if (found == made->end() || found->second.item_class != item_class || !found->second.usable) {
    return false;
  }
  use_label_key = found->second.use_label_key;
  return true;
}

void apply_item_types(const integration::ItemTypes& types) {
  if (!original_lookup) {
    return;
  }
  std::lock_guard<std::mutex> lock(apply_mutex);
  const bool any = types.full || !types.types.empty();
  if (any) {
    remember(types);
  }
  void* registry_object = registry.load(std::memory_order_relaxed);
  if (!registry_object) {
    if (any) {
      waiting.push_back(types);
    }
    return;
  }
  for (const integration::ItemTypes& earlier : waiting) {
    make(earlier, registry_object);
  }
  waiting.clear();
  if (any) {
    make(types, registry_object);
  }
}

void pump_types(const void* session, const RuntimeSender& send) {
  kcd2s::sdk::Value::Array list;
  std::uint64_t revision = 0;
  {
    std::lock_guard<std::mutex> lock(known_mutex);
    if (session != heard_session) {
      heard_session = session;
      heard_revision = 0;
    }
    if (known_revision == heard_revision) {
      return;
    }
    revision = known_revision;
    for (const auto& [name, known] : known_types) {
      list.push_back(kcd2s::sdk::Value(
          kcd2s::sdk::Value::Map{{"id", kcd2s::sdk::Value(name)},
                                 {"item", kcd2s::sdk::Value(integration::guid_text(known.id))},
                                 {"icon", kcd2s::sdk::Value(known.icon)},
                                 {"iconFile", kcd2s::sdk::Value(known.icon_file)}}));
    }
  }
  const kcd2s::sdk::Value message(
      kcd2s::sdk::Value::Map{{"op", kcd2s::sdk::Value(std::string("itemTypes"))},
                             {"types", kcd2s::sdk::Value(std::move(list))}});
  if (send(message)) {
    std::lock_guard<std::mutex> lock(known_mutex);
    if (heard_session == session) {
      heard_revision = revision;
    }
  }
}

}  // namespace kcds::runtime::inventory
