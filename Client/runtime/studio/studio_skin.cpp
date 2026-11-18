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

#include "../appearance/skin_binding.hpp"
#include "../core/log.hpp"
#include "../native_ui/native_ui.hpp"
#include "cgf_patch.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::studio::skin {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;
namespace fs = std::filesystem;
namespace binding = appearance::skin_binding;

constexpr const char* kAttachmentName = "kcds_studio_skin";
constexpr std::uintmax_t kLargestFile = 64u * 1024u * 1024u;

std::mutex request_mutex;
// The skin the editor asked for ('' takes it off), until the next frame puts it on.
std::optional<std::string> wanted;
unsigned worn_count = 0;
// The skins put on, kept for the rest of the game: the attachment holds its own reference, and
// a skin taken off stays loaded (a few hundred kilobytes each).
std::vector<void*> loaded_skins;

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

void wear(void* entity, const std::string& name) {
  void* manager = binding::attachment_manager(entity);
  void* attachment = manager ? binding::own_attachment(manager, kAttachmentName) : nullptr;
  if (!attachment) {
    game_client::log("STUDIO skin %s: the character has no skin attachment", name.c_str());
    return;
  }
  binding::clear(attachment);
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
  void* skin = binding::load_skin(path);
  if (!skin) {
    game_client::log("STUDIO skin %s: the game did not load %s", name.c_str(), path.c_str());
    return;
  }
  loaded_skins.push_back(skin);
  const bool bound = binding::bind(attachment, skin);
  game_client::log("STUDIO skin %s: %s (%s)", name.c_str(), bound ? "worn" : "not bound",
                   path.c_str());
}

}  // namespace

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
  if (!binding::installed()) {
    game_client::log("STUDIO skin %s: the game's skin functions were not found", name->c_str());
    return;
  }
  wear(local_entity, *name);
}

}  // namespace kcds::runtime::studio::skin
