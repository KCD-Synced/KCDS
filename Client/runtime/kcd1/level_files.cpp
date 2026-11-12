#include "level_files.hpp"

#include <algorithm>
#include <cctype>
#include <regex>

namespace kcds::runtime::kcd1 {
namespace {

std::string normalised(const std::string& name) {
  std::string out = name;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](char c) { return c == '\\' ? '/' : char(std::tolower(unsigned(c))); });
  return out;
}

bool starts_with(const std::string& text, const char* prefix) {
  return text.rfind(prefix, 0) == 0;
}

// Where an element named `tag` starts and ends (past its "/>" or its closing tag) in an XML text;
// npos when it is not there.
std::pair<std::size_t, std::size_t> element_range(const std::string& text, const std::string& tag) {
  const std::string open = "<" + tag;
  std::size_t start = text.find(open);
  while (start != std::string::npos) {
    const char after = start + open.size() < text.size() ? text[start + open.size()] : '\0';
    if (after == ' ' || after == '>' || after == '/' || after == '\t' || after == '\r' ||
        after == '\n') {
      break;
    }
    start = text.find(open, start + 1);
  }
  if (start == std::string::npos) {
    return {std::string::npos, std::string::npos};
  }
  const std::size_t head_end = text.find('>', start);
  if (head_end == std::string::npos) {
    return {std::string::npos, std::string::npos};
  }
  if (text[head_end - 1] == '/') {
    return {start, head_end + 1};
  }
  const std::string close = "</" + tag + ">";
  const std::size_t close_at = text.find(close, head_end);
  if (close_at == std::string::npos) {
    return {std::string::npos, std::string::npos};
  }
  return {start, close_at + close.size()};
}

std::string element(const std::string& text, const std::string& tag) {
  const auto [start, end] = element_range(text, tag);
  return start == std::string::npos ? std::string() : text.substr(start, end - start);
}

// The text with its element `tag` replaced (unchanged when either is missing).
std::string with_element(const std::string& text, const std::string& tag,
                         const std::string& replacement) {
  const auto [start, end] = element_range(text, tag);
  if (start == std::string::npos || replacement.empty()) {
    return text;
  }
  return text.substr(0, start) + replacement + text.substr(end);
}

std::string renamed(const std::string& text, const std::string& from, const std::string& to) {
  std::string out = text;
  const std::size_t at = out.find(from);
  if (at != std::string::npos) {
    out.replace(at, from.size(), to);
  }
  return out;
}

}  // namespace

bool level_file(const std::string& name) {
  const std::string path = normalised(name);
  return path == "terrain/terrain.dat" || path == "terrain/cover.ctc" ||
         path == "terrain/indoor.dat" || starts_with(path, "terrain/merged_meshes_sectors/") ||
         starts_with(path, "brush/");
}

bool material_file(const std::string& name) {
  return starts_with(normalised(name), "materials/");
}

bool texture_file(const std::string& name) {
  static constexpr const char* kWorldFolders[] = {
      "textures/structures/", "textures/terrain/",    "textures/cubemaps/",
      "textures/particles/",  "textures/rock/",       "textures/detailmaps/",
      "textures/decals/",     "textures/projectors/", "textures/rivers/"};
  const std::string path = normalised(name);
  for (const char* folder : kWorldFolders) {
    if (starts_with(path, folder)) {
      return true;
    }
  }
  return false;
}

bool texture_header(const std::string& name) {
  const std::string path = normalised(name);
  const auto ends_with = [&path](const char* tail) {
    const std::size_t length = std::char_traits<char>::length(tail);
    return path.size() > length && path.compare(path.size() - length, length, tail) == 0;
  };
  return ends_with(".dds") || ends_with(".dds.a");
}

std::string split_file(const std::string& header_name, int k) {
  const std::string path = normalised(header_name);
  if (path.size() > 2 && path.compare(path.size() - 2, 2, ".a") == 0) {
    return header_name.substr(0, header_name.size() - 2) + "." + std::to_string(k) + "a";
  }
  return header_name + "." + std::to_string(k);
}

bool fix_persistent_mips(std::vector<std::uint8_t>& header, bool attached_alpha, int split_files) {
  // A .dds starts with "DDS ", a .dds.a with the header itself (its size, 124).
  constexpr std::size_t kHeaderSize = 124;
  constexpr std::size_t kMipCountAt = 24;
  constexpr std::size_t kPersistentMipsAt = 112;
  const std::size_t start = attached_alpha ? 0 : 4;
  if (header.size() < start + kHeaderSize) {
    return false;
  }
  if (!attached_alpha &&
      (header[0] != 'D' || header[1] != 'D' || header[2] != 'S' || header[3] != ' ')) {
    return false;
  }
  const std::uint8_t* at = header.data() + start;
  const std::uint32_t size = std::uint32_t(at[0]) | (std::uint32_t(at[1]) << 8) |
                             (std::uint32_t(at[2]) << 16) | (std::uint32_t(at[3]) << 24);
  const std::uint32_t mips =
      std::uint32_t(at[kMipCountAt]) | (std::uint32_t(at[kMipCountAt + 1]) << 8) |
      (std::uint32_t(at[kMipCountAt + 2]) << 16) | (std::uint32_t(at[kMipCountAt + 3]) << 24);
  const int held = int(mips) - split_files;
  if (size != kHeaderSize || held <= 0 || held > 0xFF ||
      header[start + kPersistentMipsAt] == std::uint8_t(held)) {
    return false;
  }
  header[start + kPersistentMipsAt] = std::uint8_t(held);
  return true;
}

std::string level_info(const std::string& klaster_info, const std::string& kcd1_info) {
  const std::string info = std::regex_replace(
      klaster_info, std::regex("Name=\"data/levels/[a-z0-9_]+\""),
      std::string("Name=\"data/levels/") + kLevel + "\"", std::regex_constants::format_first_only);
  return with_element(info, "TerrainInfo", element(kcd1_info, "TerrainInfo"));
}

std::string level_data(const std::string& klaster_data, const std::string& kcd1_data) {
  std::string data = klaster_data;
  std::string level = element(kcd1_data, "LevelInfo");
  level = renamed(level, "Name=\"rataje\"", std::string("Name=\"") + kLevel + "\"");
  data = with_element(data, "LevelInfo", level);
  data = with_element(data, "Layers", "<Layers />");
  // The plant kinds are terrain.dat's own.
  data = with_element(data, "Vegetation", "<Vegetation />");
  return with_element(data, "SurfaceTypes", element(kcd1_data, "SurfaceTypes"));
}

std::string level_action(const std::string& klaster_action) {
  const std::string action =
      std::regex_replace(klaster_action, std::regex("CGFCount=\"\\d+\""), "CGFCount=\"0\"");
  return std::regex_replace(action, std::regex("ProgressBarRange=\"\\d+\""),
                            "ProgressBarRange=\"1\"");
}

std::string mission(const std::string& klaster_mission) {
  return std::regex_replace(klaster_mission, std::regex("CGFCount=\"\\d+\""), "CGFCount=\"0\"");
}

}  // namespace kcds::runtime::kcd1
