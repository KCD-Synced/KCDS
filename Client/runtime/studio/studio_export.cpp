#include "studio_export.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "../architect/local_mode.hpp"
#include "../core/log.hpp"
#include "cgf_patch.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "studio_mesh.hpp"
#include "studio_texture.hpp"
#include "studio_view.hpp"

namespace kcds::runtime::studio::exporter {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

using Bytes = std::vector<std::uint8_t>;

constexpr std::size_t kMaxName = 64;
constexpr std::size_t kMaxFiles = 16;
constexpr std::size_t kMaxFile = 64 * 1024;
constexpr std::size_t kMaxPath = 128;

bool name_fits(const std::string& name) {
  if (name.empty() || name.size() > kMaxName) {
    return false;
  }
  for (const char c : name) {
    const bool fits = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    if (!fits) {
      return false;
    }
  }
  return true;
}

bool path_fits(const std::string& path) {
  if (path.empty() || path.size() > kMaxPath || path.front() == '/' ||
      path.find("..") != std::string::npos) {
    return false;
  }
  for (const char c : path) {
    const bool fits = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '-' || c == '.' || c == '/';
    if (!fits) {
      return false;
    }
  }
  return true;
}

std::string text_of(const sdk::Value* value) {
  return value && value->tag() == sdk::Tag::Str ? value->as_str() : std::string();
}

Bytes bytes_of(const std::string& text) {
  return Bytes(text.begin(), text.end());
}

// Your mesh in the resource's stream folder: stream/objects/kcds/<resource>/<mesh>
// .cgf (its material path the resource's), its .mtl, and with a texture <mesh>_tex.dds and a
// material that shows it. False (and why in the log) when a file is missing or does not convert.
bool add_mesh(const std::string& resource, const sdk::Value& mesh,
              std::vector<std::pair<std::string, Bytes>>& files) {
  const std::string name = text_of(integration::script_field(mesh, "name"));
  const std::string texture = text_of(integration::script_field(mesh, "texture"));
  double tiling = 1.0;
  const sdk::Value* given_tiling = integration::script_field(mesh, "tiling");
  if (given_tiling) {
    view::number_of(*given_tiling, tiling);
  }
  Bytes model;
  Bytes material;
  if (!mesh::mesh_files(name, model, material)) {
    game_client::log("STUDIO export: no mesh %s", name.c_str());
    return false;
  }
  const std::string game_folder = "objects/kcds/" + resource + "/";
  if (!cgf::rename_material(model, game_folder + name)) {
    game_client::log("STUDIO export: %s.cgf is not a model the studio can export", name.c_str());
    return false;
  }
  const std::string stream = "stream/" + game_folder;
  files.emplace_back(stream + name + ".cgf", std::move(model));
  if (!texture.empty()) {
    Bytes dds;
    int width = 0;
    int height = 0;
    std::string error;
    if (!textures::dds_of_texture(texture, dds, width, height, error)) {
      game_client::log("STUDIO export: texture %s: %s", texture.c_str(), error.c_str());
      return false;
    }
    files.emplace_back(stream + name + "_tex.dds", std::move(dds));
    // Its normal map too, when the texture has one (<texture>_normal).
    Bytes normal;
    std::string normal_path;
    if (textures::dds_of_texture(texture + "_normal", normal, width, height, error)) {
      files.emplace_back(stream + name + "_tex_normal.dds", std::move(normal));
      normal_path = game_folder + name + "_tex_normal.dds";
    }
    // And its specular map (<texture>_spec).
    Bytes specular;
    std::string specular_path;
    if (textures::dds_of_texture(texture + "_spec", specular, width, height, error)) {
      files.emplace_back(stream + name + "_tex_spec.dds", std::move(specular));
      specular_path = game_folder + name + "_tex_spec.dds";
    }
    material = bytes_of(textures::material_text(game_folder + name + "_tex.dds", tiling,
                                                normal_path, specular_path));
  }
  if (texture.empty()) {
    // Its own textures from Blender (<mesh>_<material>.dds), the material naming the resource's.
    for (auto& [file, bytes] : mesh::mesh_textures(name)) {
      files.emplace_back(stream + file, std::move(bytes));
    }
    std::string text(material.begin(), material.end());
    const std::string studio_folder = "objects/kcds_studio/";
    for (std::size_t at = text.find(studio_folder); at != std::string::npos;
         at = text.find(studio_folder, at + game_folder.size())) {
      text.replace(at, studio_folder.size(), game_folder);
    }
    material = bytes_of(text);
  }
  if (!material.empty()) {
    files.emplace_back(stream + name + ".mtl", std::move(material));
  }
  return true;
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "studioExport") {
    return false;
  }
  const std::string name = text_of(integration::script_field(message, "name"));
  const sdk::Value* files = integration::script_field(message, "files");
  const char* local = std::getenv("LOCALAPPDATA");
  if (!architect::local_mode::enabled() || !local || !name_fits(name) || !files ||
      files->tag() != sdk::Tag::Array || files->as_array().size() > kMaxFiles) {
    game_client::log("STUDIO export refused");
    return true;
  }
  std::vector<std::pair<std::string, Bytes>> checked;
  for (const sdk::Value& entry : files->as_array()) {
    if (entry.tag() != sdk::Tag::Array || entry.as_array().size() != 2) {
      game_client::log("STUDIO export refused: a file is not [path, text]");
      return true;
    }
    const std::string path = text_of(&entry.as_array()[0]);
    const std::string text = text_of(&entry.as_array()[1]);
    if (!path_fits(path) || text.size() > kMaxFile) {
      game_client::log("STUDIO export refused: %s", path.c_str());
      return true;
    }
    checked.emplace_back(path, bytes_of(text));
  }
  const sdk::Value* mesh = integration::script_field(message, "mesh");
  if (mesh && mesh->tag() == sdk::Tag::Map && !add_mesh(name, *mesh, checked)) {
    return true;
  }
  const std::filesystem::path folder =
      std::filesystem::path(local) / "KCDS" / "studio" / "exports" / name;
  std::error_code error;
  for (const auto& [path, bytes] : checked) {
    const std::filesystem::path file = folder / std::filesystem::path(path);
    std::filesystem::create_directories(file.parent_path(), error);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!out) {
      game_client::log("STUDIO export: %s could not be written", path.c_str());
      return true;
    }
  }
  game_client::log("STUDIO exported %zu files to %s", checked.size(), folder.string().c_str());
  return true;
}

}  // namespace kcds::runtime::studio::exporter
