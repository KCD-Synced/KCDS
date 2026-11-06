#include "studio_skin.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../core/log.hpp"
#include "../native_ui/native_ui.hpp"
#include "cgf_patch.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::studio::skin {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;
namespace fs = std::filesystem;

// IEntity GetCharacter(slot): the body's character in slot 0.
constexpr std::size_t kEntityCharacter = 101;
// ICharacterInstance GetIAttachmentManager.
constexpr std::size_t kAttachmentManager = 9;
// IAttachmentManager CreateAttachment(name, type) and GetInterfaceByName(name).
constexpr std::size_t kCreateAttachment = 2;
constexpr std::size_t kAttachmentByName = 8;
constexpr std::uint32_t kSkinAttachment = 2;
// IAttachment AddBinding(object, skin, flags), ClearBinding(flags), its skin interface.
constexpr std::size_t kAddBinding = 26;
constexpr std::size_t kClearBinding = 30;
constexpr std::size_t kSkinInterface = 29;
constexpr std::uint32_t kBindFlags = 2048;
// ICharacterManager LoadModelSKIN(out skin, path, flags).
constexpr std::size_t kLoadSkin = 6;
// A CSKINAttachment: 64 bytes, its attachment's skin interface at + 8.
constexpr std::size_t kSkinObjectBytes = 64;
constexpr std::size_t kSkinObjectOwner = 8;
constexpr const char* kAttachmentName = "kcds_studio_skin";
constexpr std::uintmax_t kLargestFile = 64u * 1024u * 1024u;

Natives natives;
std::mutex request_mutex;
// The skin the editor asked for ('' takes it off), until the next frame puts it on.
std::optional<std::string> wanted;
unsigned worn_count = 0;
// The skins put on, kept for the rest of the game: the attachment holds its own reference, and
// a skin taken off stays loaded (a few hundred kilobytes each).
std::vector<void*> loaded_skins;

template <typename Fn>
Fn slot_of(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

std::optional<fs::path> meshes_folder() {
  const char* local = std::getenv("LOCALAPPDATA");
  if (!local || !*local) {
    return std::nullopt;
  }
  return fs::path(local) / "KCDS" / "studio" / "meshes";
}

std::optional<std::vector<std::uint8_t>> read_file(const fs::path& path) {
  std::error_code error;
  const auto size = fs::file_size(path, error);
  if (error || size == 0 || size > kLargestFile) {
    return std::nullopt;
  }
  std::ifstream file(path, std::ios::binary);
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
  return file ? std::optional(bytes) : std::nullopt;
}

// A plain name: letters, digits, '_' and '-'.
bool plain_name(const std::string& name) {
  if (name.empty() || name.size() > 64) {
    return false;
  }
  for (const char character : name) {
    const bool letter =
        (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
    const bool digit = character >= '0' && character <= '9';
    if (!letter && !digit && character != '_' && character != '-') {
      return false;
    }
  }
  return true;
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

// The studio's skin attachment of the character, made the first time.
void* skin_attachment(void* manager) {
  using ByName = void*(__fastcall*)(void* manager, const char* name);
  void* attachment = slot_of<ByName>(manager, kAttachmentByName)(manager, kAttachmentName);
  if (attachment) {
    return attachment;
  }
  using Create = void*(__fastcall*)(void* manager, const char* name, std::uint32_t type);
  return slot_of<Create>(manager, kCreateAttachment)(manager, kAttachmentName, kSkinAttachment);
}

// The skin file of the game path `path` loaded by the character manager, or null.
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

// Puts the skin on as the game's clothing does; false when the game refused it.
bool bind(void* attachment, void* skin) {
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

void wear(void* entity, const std::string& name) {
  void* manager = attachment_manager(entity);
  void* attachment = manager ? skin_attachment(manager) : nullptr;
  if (!attachment) {
    game_client::log("STUDIO skin %s: the character has no skin attachment", name.c_str());
    return;
  }
  using ClearBinding = void(__fastcall*)(void* attachment, std::uint32_t flags);
  slot_of<ClearBinding>(attachment, kClearBinding)(attachment, 0);
  if (name.empty()) {
    game_client::log("STUDIO skin taken off");
    return;
  }
  const auto folder = meshes_folder();
  const auto model = folder ? read_file(*folder / (name + ".skin")) : std::nullopt;
  if (!model) {
    game_client::log("STUDIO skin %s: no such skin file", name.c_str());
    return;
  }
  ++worn_count;
  // A name of its own each time, the model and its material alike: the game keeps a model and a
  // material it has loaded by their names (studio_mesh.cpp).
  const std::string stem = "kcds_studio/" + name + "_skin_" + std::to_string(worn_count);
  std::vector<std::uint8_t> renamed = *model;
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> entries;
  const auto material = read_file(*folder / (name + ".mtl"));
  if (material && cgf::rename_material(renamed, "objects/" + stem)) {
    entries.emplace_back(stem + ".mtl", *material);
  }
  entries.emplace(entries.begin(), stem + ".skin", renamed);
  const std::string key = "_kcds/studio/skin-" + std::to_string(worn_count);
  if (!native_ui::mount_files(key, "objects/", entries)) {
    game_client::log("STUDIO skin %s: its pack could not be mounted", name.c_str());
    return;
  }
  const std::string path = "objects/" + stem + ".skin";
  void* skin = load_skin(path);
  if (!skin) {
    game_client::log("STUDIO skin %s: the game did not load %s", name.c_str(), path.c_str());
    return;
  }
  loaded_skins.push_back(skin);
  const bool bound = bind(attachment, skin);
  game_client::log("STUDIO skin %s: %s (%s)", name.c_str(), bound ? "worn" : "not bound",
                   path.c_str());
}

}  // namespace

void install(const Natives& given) {
  natives = given;
}

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "studioSkin") {
    return false;
  }
  const sdk::Value* name = integration::script_field(message, "name");
  if (!name || name->tag() != sdk::Tag::Str) {
    return true;
  }
  const std::string text = name->as_str();
  if (!text.empty() && !plain_name(text)) {
    game_client::log("STUDIO skin '%s' refused: not a plain name", text.c_str());
    return true;
  }
  std::lock_guard<std::mutex> lock(request_mutex);
  wanted = text;
  return true;
}

void tick(void* local_entity) {
  std::optional<std::string> name;
  {
    std::lock_guard<std::mutex> lock(request_mutex);
    name.swap(wanted);
  }
  if (!name) {
    return;
  }
  if (!natives.character_manager || !natives.allocate || !natives.skin_attachment) {
    game_client::log("STUDIO skin %s: the game's skin functions were not found", name->c_str());
    return;
  }
  wear(local_entity, *name);
}

}  // namespace kcds::runtime::studio::skin
