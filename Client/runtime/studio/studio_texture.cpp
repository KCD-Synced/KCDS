// Winsock before Windows.h (see client_internal.hpp); then Windows' own image decoder (WIC).
#include "../core/client_internal.hpp"

#include <wincodec.h>

#include "studio_texture.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
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
#include "../props/prop_entity.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "studio_mesh.hpp"
#include "studio_view.hpp"
#include "texture_dds.hpp"

namespace kcds::runtime::studio::textures {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;
namespace fs = std::filesystem;

constexpr std::size_t kLongestName = 48;
constexpr std::uintmax_t kLargestFile = 64u * 1024u * 1024u;
constexpr UINT kLargestSide = 2048;
// The mesh's subsets may use any of its first materials: each gets the texture.
constexpr int kSubMaterials = 8;
constexpr const char* kExtensions[] = {".png", ".jpg", ".jpeg", ".dds"};

struct Request {
  bool list = false;
  std::string name;
  double tiling = 1.0;
};

std::mutex requests_mutex;
std::vector<Request> requests;
// Each texture shown gets packs of its own (the game keeps a mounted pack and a loaded texture).
unsigned shown_count = 0;

// A name is the file's stem; letters, digits, _ and - only.
bool texture_name(const std::string& name) {
  return !name.empty() && name.size() <= kLongestName &&
         std::all_of(name.begin(), name.end(), [](char c) {
           return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
         });
}

std::optional<fs::path> textures_folder() {
  const char* local = std::getenv("LOCALAPPDATA");
  if (!local || !*local) {
    return std::nullopt;
  }
  return fs::path(local) / "KCDS" / "studio" / "textures";
}

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return text;
}

bool known_extension(const fs::path& path) {
  const std::string extension = lower(path.extension().string());
  return std::any_of(std::begin(kExtensions), std::end(kExtensions),
                     [&](const char* known) { return extension == known; });
}

std::vector<std::string> texture_names() {
  std::vector<std::string> names;
  const auto folder = textures_folder();
  std::error_code error;
  if (!folder || !fs::is_directory(*folder, error)) {
    return names;
  }
  for (const auto& entry : fs::directory_iterator(*folder, error)) {
    const std::string name = entry.path().stem().string();
    // <texture>_normal and <texture>_spec are that texture's maps, not textures of their own.
    const bool normal_map = name.size() > 7 && name.compare(name.size() - 7, 7, "_normal") == 0;
    const bool specular_map = name.size() > 5 && name.compare(name.size() - 5, 5, "_spec") == 0;
    if (!normal_map && !specular_map && known_extension(entry.path()) && texture_name(name) &&
        std::find(names.begin(), names.end(), name) == names.end()) {
      names.push_back(name);
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

// The file of a texture's name, whichever of the known extensions it has.
std::optional<fs::path> texture_file(const std::string& name) {
  const auto folder = textures_folder();
  if (!folder) {
    return std::nullopt;
  }
  for (const char* extension : kExtensions) {
    std::error_code error;
    const fs::path path = *folder / (name + extension);
    if (fs::is_regular_file(path, error)) {
      return path;
    }
  }
  return std::nullopt;
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
  return file || file.eof() ? std::optional(bytes) : std::nullopt;
}

template <class T>
void release(T*& object) {
  if (object) {
    object->Release();
    object = nullptr;
  }
}

// The picture as 32-bit BGRA rows, at most kLargestSide a side (scaled down keeping its shape).
std::optional<texture::Image> decode(const fs::path& path, std::string& error) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool uninitialize = com == S_OK || com == S_FALSE;
  IWICImagingFactory* factory = nullptr;
  IWICBitmapDecoder* decoder = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* converter = nullptr;
  IWICBitmapScaler* scaler = nullptr;
  std::optional<texture::Image> image;
  UINT width = 0;
  UINT height = 0;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory)))) {
    error = "Windows' image decoder is missing";
  } else if (FAILED(factory->CreateDecoderFromFilename(path.wstring().c_str(), nullptr,
                                                       GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                       &decoder)) ||
             FAILED(decoder->GetFrame(0, &frame))) {
    error = "the picture could not be read";
  } else if (FAILED(factory->CreateFormatConverter(&converter)) ||
             FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                          WICBitmapDitherTypeNone, nullptr, 0,
                                          WICBitmapPaletteTypeCustom)) ||
             FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0) {
    error = "the picture's pixels could not be read";
  } else {
    IWICBitmapSource* source = converter;
    const UINT longest = std::max(width, height);
    if (longest > kLargestSide) {
      const UINT scaled_width = std::max<UINT>(1, width * kLargestSide / longest);
      const UINT scaled_height = std::max<UINT>(1, height * kLargestSide / longest);
      if (SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
          SUCCEEDED(scaler->Initialize(converter, scaled_width, scaled_height,
                                       WICBitmapInterpolationModeFant))) {
        source = scaler;
        width = scaled_width;
        height = scaled_height;
      }
    }
    texture::Image read;
    read.width = int(width);
    read.height = int(height);
    read.bgra.resize(std::size_t(width) * std::size_t(height) * 4);
    if (SUCCEEDED(
            source->CopyPixels(nullptr, width * 4, UINT(read.bgra.size()), read.bgra.data()))) {
      image = std::move(read);
    } else {
      error = "the picture's pixels could not be copied";
    }
  }
  release(scaler);
  release(converter);
  release(frame);
  release(decoder);
  release(factory);
  if (uninitialize) {
    CoUninitialize();
  }
  return image;
}

// A .dds's width and height from its header; false when it is not one.
bool dds_size(const std::vector<std::uint8_t>& bytes, int& width, int& height) {
  if (bytes.size() < 128 || bytes[0] != 'D' || bytes[1] != 'D' || bytes[2] != 'S' ||
      bytes[3] != ' ') {
    return false;
  }
  auto word = [&](std::size_t offset) {
    return int(bytes[offset] | (bytes[offset + 1] << 8) | (bytes[offset + 2] << 16) |
               (bytes[offset + 3] << 24));
  };
  height = word(12);
  width = word(16);
  return width > 0 && height > 0;
}

}  // namespace

std::string material_text(const std::string& texture_path, double tiling) {
  return material_text(texture_path, tiling, "");
}

std::string material_text(const std::string& texture_path, double tiling,
                          const std::string& normal_path) {
  return material_text(texture_path, tiling, normal_path, "");
}

std::string material_text(const std::string& texture_path, double tiling,
                          const std::string& normal_path, const std::string& specular_path) {
  char tile[96] = {};
  std::snprintf(tile, sizeof(tile), "<TexMod TileU=\"%.3g\" TileV=\"%.3g\"/>", tiling, tiling);
  // A normal map (Map "Bumpmap", the slot the game's own materials put their _ddna in) and a
  // specular map (Map "Specular", as the game's chain_a_spec), tiled as the colour; the shader's
  // NORMAL_MAP and SPECULAR_MAP switches on with them.
  std::string flags;
  std::string extra;
  if (!normal_path.empty()) {
    flags += "%NORMAL_MAP";
    extra +=
        "        <Texture Map=\"Bumpmap\" File=\"" + normal_path + "\">" + tile + "</Texture>\n";
  }
  if (!specular_path.empty()) {
    flags += "%SPECULAR_MAP";
    extra +=
        "        <Texture Map=\"Specular\" File=\"" + specular_path + "\">" + tile + "</Texture>\n";
  }
  const std::string& normal = extra;
  std::string text = "<Material MtlFlags=\"524544\" vertModifType=\"0\">\n  <SubMaterials>\n";
  for (int index = 0; index < kSubMaterials; ++index) {
    text += "    <Material Name=\"kcds_texture_" + std::to_string(index) +
            "\" MtlFlags=\"524416\" Shader=\"Illum\" GenMask=\"20000000000000\" "
            "StringGenMask=\"" +
            flags +
            "\" SurfaceType=\"mat_wood\" Diffuse=\"1,1,1\" "
            "Specular=\"0.04,0.04,0.04\" Opacity=\"1\" Shininess=\"10\" vertModifType=\"0\" "
            "LayerAct=\"1\">\n";
    text += "      <Textures>\n        <Texture Map=\"Diffuse\" File=\"" + texture_path + "\">" +
            tile + "</Texture>\n" + normal + "      </Textures>\n";
    text += "    </Material>\n";
  }
  text += "  </SubMaterials>\n</Material>\n";
  return text;
}

namespace {

sdk::Value answer(const Request& request, bool ok, int width, int height,
                  const std::string& error) {
  return sdk::Value(sdk::Value::Map{{"op", sdk::Value("studioTextured")},
                                    {"name", sdk::Value(request.name)},
                                    {"ok", sdk::Value(ok)},
                                    {"width", sdk::Value(std::int64_t(width))},
                                    {"height", sdk::Value(std::int64_t(height))},
                                    {"error", sdk::Value(error)}});
}

}  // namespace

bool dds_of_texture(const std::string& name, std::vector<std::uint8_t>& dds, int& width,
                    int& height, std::string& error) {
  const auto file = texture_name(name) ? texture_file(name) : std::nullopt;
  if (!file) {
    error = "no such texture file";
    return false;
  }
  if (lower(file->extension().string()) == ".dds") {
    const auto bytes = read_file(*file);
    if (!bytes || !dds_size(*bytes, width, height)) {
      error = "the .dds could not be read";
      return false;
    }
    dds = *bytes;
    return true;
  }
  const auto image = decode(*file, error);
  if (!image) {
    return false;
  }
  width = image->width;
  height = image->height;
  // A picture's colours are sRGB; a normal map's are not (a specular map's are, as the game's).
  const bool normal_map = name.size() > 7 && name.compare(name.size() - 7, 7, "_normal") == 0;
  dds = texture::dds_of(*image, !normal_map);
  return true;
}

namespace {

sdk::Value show(const Request& request) {
  void* entity = mesh::shown_entity();
  if (!entity) {
    return answer(request, false, 0, 0, "show a mesh first");
  }
  if (request.name.empty()) {
    props::entity::draw_with_material(entity, mesh::own_material());
    props::entity::redraw(entity);
    return answer(request, true, 0, 0, "");
  }
  std::vector<std::uint8_t> dds;
  int width = 0;
  int height = 0;
  std::string error;
  if (!dds_of_texture(request.name, dds, width, height, error)) {
    return answer(request, false, 0, 0, error);
  }
  ++shown_count;
  const std::string stem = "kcds_studio/tex_" + request.name + "_" + std::to_string(shown_count);
  // Its normal map, when <name>_normal is beside it.
  std::vector<std::uint8_t> normal;
  int normal_width = 0;
  int normal_height = 0;
  std::string normal_error;
  const bool with_normal =
      dds_of_texture(request.name + "_normal", normal, normal_width, normal_height, normal_error);
  // Its specular map, when <name>_spec is beside it.
  std::vector<std::uint8_t> specular;
  const bool with_specular =
      dds_of_texture(request.name + "_spec", specular, normal_width, normal_height, normal_error);
  const std::string material =
      material_text("objects/" + stem + ".dds", request.tiling,
                    with_normal ? "objects/" + stem + "_normal.dds" : std::string(),
                    with_specular ? "objects/" + stem + "_spec.dds" : std::string());
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> entries;
  entries.emplace_back(stem + ".dds", std::move(dds));
  if (with_normal) {
    entries.emplace_back(stem + "_normal.dds", std::move(normal));
  }
  if (with_specular) {
    entries.emplace_back(stem + "_spec.dds", std::move(specular));
  }
  entries.emplace_back(stem + ".mtl", std::vector<std::uint8_t>(material.begin(), material.end()));
  if (!native_ui::mount_files("_kcds/studio/tex-" + std::to_string(shown_count), "objects/",
                              entries)) {
    return answer(request, false, width, height, "its pack could not be mounted");
  }
  const std::string before = props::entity::material_name(entity, 128);
  props::entity::draw_with_material(entity, "objects/" + stem);
  props::entity::redraw(entity);
  const std::string after = props::entity::material_name(entity, 128);
  game_client::log("STUDIO texture %s%s%s: %dx%d on the mesh (material '%s' -> '%s')",
                   request.name.c_str(), with_normal ? " with its normal map" : "",
                   with_specular ? " and specular map" : "", width, height, before.c_str(),
                   after.c_str());
  if (after.find("tex_" + request.name) == std::string::npos) {
    return answer(request, false, width, height, "the game did not take its material");
  }
  return answer(request, true, width, height, "");
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = integration::script_operation(message);
  Request request;
  if (operation == "studioTextureList") {
    request.list = true;
  } else if (operation == "studioTexture") {
    const sdk::Value* name = integration::script_field(message, "name");
    request.name = name && name->tag() == sdk::Tag::Str ? name->as_str() : std::string();
    if (!request.name.empty() && !texture_name(request.name)) {
      return true;
    }
    const sdk::Value* tiling = integration::script_field(message, "tiling");
    double times = 1.0;
    if (tiling && view::number_of(*tiling, times)) {
      request.tiling = std::clamp(times, 0.05, 64.0);
    }
  } else {
    return false;
  }
  std::lock_guard<std::mutex> lock(requests_mutex);
  requests.push_back(request);
  return true;
}

void tick(const std::function<bool(const sdk::Value&)>& send) {
  std::vector<Request> now;
  {
    std::lock_guard<std::mutex> lock(requests_mutex);
    now.swap(requests);
  }
  for (const Request& request : now) {
    if (request.list) {
      sdk::Value::Array names;
      for (const std::string& name : texture_names()) {
        names.push_back(sdk::Value(name));
      }
      send(sdk::Value(
          sdk::Value::Map{{"op", sdk::Value("studioTextures")}, {"names", sdk::Value(names)}}));
    } else {
      send(show(request));
    }
  }
}

}  // namespace kcds::runtime::studio::textures
