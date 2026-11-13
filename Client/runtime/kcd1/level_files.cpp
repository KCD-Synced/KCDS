#include "level_files.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <string_view>
#include <vector>

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

namespace {

// A layer's or profile's name in lower case, without the GUID KCD1 adds ("rain{4978D8E4-...}").
std::string base_name(const std::string& name) {
  std::string base;
  for (const char c : name) {
    if (c == '{') {
      break;
    }
    base += char(std::tolower(unsigned(c)));
  }
  return base;
}

bool ends_with(const std::string& text, const std::string& tail) {
  return text.size() >= tail.size() &&
         text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

// The text between <tag> and </tag> of each such element from `from` to `to`, in order.
std::vector<std::string> values_of(const std::string& text, const std::string& tag,
                                   std::size_t from = 0, std::size_t to = std::string::npos) {
  const std::string open = "<" + tag + ">";
  const std::string close = "</" + tag + ">";
  std::vector<std::string> values;
  std::size_t at = from;
  while ((at = text.find(open, at)) != std::string::npos && at < to) {
    const std::size_t start = at + open.size();
    const std::size_t end = text.find(close, start);
    if (end == std::string::npos) {
      break;
    }
    values.push_back(text.substr(start, end - start));
    at = end + close.size();
  }
  return values;
}

// Whether the game profile `name` (KCD1's whdata_1) is part of the state a new KCD1 game starts
// in: farms not burnt yet, Talmberk's fort as it was, the Vranik camp and Rataje's hall in their
// first state, Pribyslavitz's ruins and building sites, the ambient life without rain.
bool start_profile(const std::string& name) {
  static const char* const kEnds[] = {"_ok", "_original", "_normal", "_site", "state0"};
  for (const char* tail : kEnds) {
    if (ends_with(name, tail)) {
      return true;
    }
  }
  static const char* const kNames[] = {"normal_mode",
                                       "weather_bees",
                                       "weather_butterflies",
                                       "weather_fireflies",
                                       "weather_flies",
                                       "tal_others",
                                       "tal_others_castle",
                                       "tal_ecosystem_boars",
                                       "tal_bandit_camps",
                                       "tal_castle_gn_wn_open",
                                       "tal_assets_unloaded_during_conquest",
                                       "newhomes_church_ruin",
                                       "newhomes_bridge_wooden",
                                       "newhomes_townhall_tent",
                                       "mon_monastery_guards",
                                       "neu_stables",
                                       "nobles_horses"};
  for (const char* start : kNames) {
    if (name == start) {
      return true;
    }
  }
  return false;
}

// Whether a layer no profile holds belongs to KCD1's story (cutscenes, quests, events, battles,
// a later state of a place) or its weather (rain), which a new game does not show.
bool story_layer(const std::string& base) {
  static const char* const kStarts[] = {"cin_",     "q_",         "event_",
                                        "battle",   "tournament", "murder",
                                        "rain",     "leaves",     "particles_heavy_rain",
                                        "charlatan"};
  for (const char* start : kStarts) {
    if (base.rfind(start, 0) == 0) {
      return true;
    }
  }
  const std::size_t state = base.find("state");
  return state != std::string::npos && state + 5 < base.size() &&
         std::isdigit(unsigned(base[state + 5])) && base[state + 5] != '0';
}

}  // namespace

std::string layer_starts(const std::string& kcd1_whdata, const std::string& kcd1_leveldata) {
  // The layers the start profiles hold, and every layer some profile holds.
  std::set<std::string> on;
  std::set<std::string> in_profile;
  std::string out;
  const std::string open = "<GameProfile>";
  const std::string close = "</GameProfile>";
  std::size_t at = 0;
  while ((at = kcd1_whdata.find(open, at)) != std::string::npos) {
    const std::size_t end = kcd1_whdata.find(close, at);
    if (end == std::string::npos) {
      break;
    }
    const std::vector<std::string> names = values_of(kcd1_whdata, "Name", at, end);
    const bool start = !names.empty() && start_profile(base_name(names.front()));
    for (const std::string& layer : values_of(kcd1_whdata, "GameLayer", at, end)) {
      in_profile.insert(layer);
      if (start) {
        on.insert(layer);
      }
    }
    at = end + close.size();
  }
  const std::regex layer("<Layer Name=\"([^\"]+)\" Id=\"(\\d+)\"");
  for (auto it = std::sregex_iterator(kcd1_leveldata.begin(), kcd1_leveldata.end(), layer);
       it != std::sregex_iterator(); ++it) {
    const std::string name = (*it)[1].str();
    const bool starts_on =
        on.count(name) > 0 || (in_profile.count(name) == 0 && !story_layer(base_name(name)));
    out += (starts_on ? "1 " : "0 ") + (*it)[2].str() + " " + name + "\n";
  }
  return out;
}

namespace {

// An attribute's value in a tag, empty when it has none.
std::string attribute(const std::string& tag, const std::string& name) {
  const std::string key = " " + name + "=\"";
  const std::size_t at = tag.find(key);
  if (at == std::string::npos) {
    return {};
  }
  const std::size_t start = at + key.size();
  const std::size_t end = tag.find('"', start);
  return end == std::string::npos ? std::string() : tag.substr(start, end - start);
}

// The tag without the attribute.
std::string without_attribute(const std::string& tag, const std::string& name) {
  const std::string key = " " + name + "=\"";
  const std::size_t at = tag.find(key);
  if (at == std::string::npos) {
    return tag;
  }
  const std::size_t end = tag.find('"', at + key.size());
  return end == std::string::npos ? tag : tag.substr(0, at) + tag.substr(end + 1);
}

std::string replaced_all(std::string text, const std::string& from, const std::string& to) {
  std::size_t at = text.find(from);
  while (at != std::string::npos) {
    text.replace(at, from.size(), to);
    at = text.find(from, at + to.size());
  }
  return text;
}

// The texture list without the textures of `map` (each a self-closing <Texture .../> or one with
// its own </Texture>).
std::string without_textures(const std::string& body, const std::string& map) {
  std::string out = body;
  const std::string key = "Map=\"" + map + "\"";
  std::size_t key_at = out.find(key);
  while (key_at != std::string::npos) {
    const std::size_t start = out.rfind("<Texture", key_at);
    const std::size_t head_end = out.find('>', key_at);
    if (start == std::string::npos || head_end == std::string::npos) {
      break;
    }
    std::size_t end = head_end + 1;
    if (out[head_end - 1] != '/') {
      const std::size_t close = out.find("</Texture>", head_end);
      end = close == std::string::npos ? end : close + 10;
    }
    out.erase(start, end - start);
    key_at = out.find(key, start);
  }
  return out;
}

// One material's own part: its tag and its texture list up to its sub-materials or end.
std::string translate_one(const std::string& tag, const std::string& body) {
  const std::string flags = attribute(tag, "StringGenMask");
  std::string new_tag = without_attribute(tag, "GenMask");
  // Both games multiply a second UV overlay (dirt and wear, %SNDUVS) into the base, but KCD1 takes
  // it from the "[1] Diffuse" slot and KCD2 from the Opacity slot ("Blending Map"), where KCD1
  // keeps a mask KCD2 would then smear over the walls. The overlay moves to KCD2's slot; without
  // one the flag goes.
  const bool overlay = body.find("Map=\"[1] Diffuse\"") != std::string::npos;
  if (flags.find("%SNDUVS") != std::string::npos && overlay) {
    return new_tag + replaced_all(without_textures(body, "Opacity"), "Map=\"[1] Diffuse\"",
                                  "Map=\"Opacity\"");
  }
  if (flags.find("%SNDUVS") != std::string::npos) {
    const std::string kept_flags = replaced_all(flags, "%SNDUVS", "");
    new_tag = replaced_all(new_tag, "StringGenMask=\"" + flags + "\"",
                           "StringGenMask=\"" + kept_flags + "\"");
  }
  return new_tag + without_textures(body, "[1] Diffuse");
}

}  // namespace

std::string translate_material(const std::string& text) {
  std::string out;
  std::size_t at = 0;
  while (true) {
    const std::size_t start = text.find("<Material", at);
    if (start == std::string::npos) {
      out += text.substr(at);
      break;
    }
    out += text.substr(at, start - at);
    const std::size_t tag_end = text.find('>', start);
    if (tag_end == std::string::npos) {
      out += text.substr(start);
      break;
    }
    // Its own part ends where a sub-material starts or a material ends.
    std::size_t body_end =
        std::min(text.find("<Material", tag_end), text.find("</Material", tag_end));
    if (body_end == std::string::npos) {
      body_end = text.size();
    }
    out += translate_one(text.substr(start, tag_end + 1 - start),
                         text.substr(tag_end + 1, body_end - tag_end - 1));
    at = body_end;
  }
  return out;
}

bool prop_file(const std::string& name) {
  return starts_with(normalised(name), "objects/characters/assets/");
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
  // KCD1's layers: the 3D engine shows the objects of a layer it knows by its id, and the client
  // switches them (layer_starts).
  const std::string layers = element(kcd1_data, "Layers");
  data = with_element(data, "Layers", layers.empty() ? std::string("<Layers />") : layers);
  // The plant kinds are terrain.dat's own.
  data = with_element(data, "Vegetation", "<Vegetation />");
  return with_element(data, "SurfaceTypes", element(kcd1_data, "SurfaceTypes"));
}

std::string layer_entities(const std::string& layer) {
  const std::size_t open = layer.find("<Objects>");
  const std::size_t close = layer.rfind("</Objects>");
  if (open == std::string::npos || close == std::string::npos || close < open) {
    return {};
  }
  return layer.substr(open + 9, close - open - 9);
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
