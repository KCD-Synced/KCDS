#include "story_layers.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../files/zip_read.hpp"
#include "layer_catalog.hpp"
#include "level_edits.hpp"

namespace kcds::runtime::level::layers {
namespace {

// IEntitySystem: FindLayer (+ 568), EnableLayer (+ 592: name, on, save), IsLayerEnabled (+ 600:
// name, must be loaded); see story_layers.hpp.
constexpr std::size_t kFindLayer = 568 / 8;
constexpr std::size_t kEnableLayer = 592 / 8;
constexpr std::size_t kIsLayerEnabled = 600 / 8;

using FindLayer = void*(__fastcall*)(void* system, const char* name);
using EnableLayer = void(__fastcall*)(void* system, const char* name, bool on, bool save);
using IsLayerEnabled = bool(__fastcall*)(void* system, const char* name, bool must_be_loaded);

// Half the side of a tile the brushes are asked for (metres; it also reaches this far up and down).
constexpr float kTileHalf = 25.0f;

void* const* entity_system_global = nullptr;
Logger log = nullptr;

// The level's own state of each layer changed here, by full name.
std::map<std::string, bool> level_states;
// The level's own switch for its layers (set_fallback), and the layers changed through it.
LayerFallback fallback = nullptr;
std::set<std::string> fallback_layers;

template <class F>
F slot(void* object, std::size_t index) {
  return reinterpret_cast<F>((*reinterpret_cast<void***>(object))[index]);
}

void* entity_system() {
  return entity_system_global ? *entity_system_global : nullptr;
}

// What the level's files say of a map's layers.
struct MapLayers {
  std::unordered_map<std::uint16_t, std::string> names;  // by id
  std::unordered_map<std::string, LayerPlace> places;    // by full name, the layers with entities
  bool read = false;
};
std::map<std::string, MapLayers> maps;

// The game's folder: KingdomCome.exe is in Bin/<build>/.
std::filesystem::path game_folder() {
  wchar_t path[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
    return {};
  }
  return std::filesystem::path(path).parent_path().parent_path().parent_path();
}

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
  return text;
}

// Reads the map's leveldata.xml and every layer file from its level.pak, once.
const MapLayers& read_map(const std::string& map) {
  MapLayers& layers = maps[map];
  if (layers.read) {
    return layers;
  }
  layers.read = true;
  const auto pak = game_folder() / "Data" / "Levels" / map / "level.pak";
  std::ifstream file(pak, std::ios::binary);
  if (!file) {
    if (log) {
      log("LAYERS no level pack for map %s", map.c_str());
    }
    return layers;
  }
  file.seekg(0, std::ios::end);
  const auto size = std::uint64_t(file.tellg());
  const files::ZipReadAt read = [&file](std::uint64_t offset, std::size_t length,
                                        std::vector<std::uint8_t>& out) {
    out.resize(length);
    file.clear();
    file.seekg(std::streamoff(offset));
    file.read(reinterpret_cast<char*>(out.data()), std::streamsize(length));
    return std::size_t(file.gcount()) == length;
  };
  const auto entries = files::zip_entries(read, size);
  if (!entries) {
    return layers;
  }
  std::unordered_map<std::string, const files::ZipEntry*> by_name;
  for (const auto& entry : *entries) {
    std::string name = lower(entry.name);
    std::replace(name.begin(), name.end(), '\\', '/');
    by_name[name] = &entry;
  }
  const auto text_of = [&](const std::string& name) -> std::optional<std::string> {
    const auto found = by_name.find(name);
    if (found == by_name.end()) {
      return std::nullopt;
    }
    const auto bytes = files::zip_entry_bytes(read, *found->second);
    if (!bytes) {
      return std::nullopt;
    }
    return std::string(bytes->begin(), bytes->end());
  };
  const auto leveldata = text_of("leveldata.xml");
  if (!leveldata) {
    return layers;
  }
  for (const auto& layer : parse_level_layers(*leveldata)) {
    layers.names[layer.id] = layer.name;
    if (const auto xml = text_of("layers/" + lower(layer.name) + ".xml")) {
      if (auto place = parse_layer_file(*xml)) {
        layers.places[layer.name] = std::move(*place);
      }
    }
  }
  if (log) {
    log("LAYERS map %s: %zu layers, %zu with entities", map.c_str(), layers.names.size(),
        layers.places.size());
  }
  return layers;
}

float distance(const float a[3], const float b[3]) {
  const float dx = a[0] - b[0];
  const float dy = a[1] - b[1];
  const float dz = a[2] - b[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

bool install(void* const* entity_system, Logger logger) {
  entity_system_global = entity_system;
  log = logger;
  return entity_system != nullptr;
}

std::optional<bool> enabled(const std::string& name) {
  void* system = entity_system();
  if (!system || name.empty() || !slot<FindLayer>(system, kFindLayer)(system, name.c_str())) {
    return std::nullopt;
  }
  return slot<IsLayerEnabled>(system, kIsLayerEnabled)(system, name.c_str(), false);
}

void set_fallback(LayerFallback layer_fallback) {
  fallback = layer_fallback;
}

bool set(const std::string& name, bool on) {
  void* system = entity_system();
  const auto now = enabled(name);
  // A level with its own switch shows or hides the layer's objects itself; the entity system,
  // when it knows the layer too, then switches the layer's entities.
  if (fallback && fallback(name, on)) {
    fallback_layers.insert(name);
    if (system && now) {
      level_states.emplace(name, *now);
      if (*now != on) {
        slot<EnableLayer>(system, kEnableLayer)(system, name.c_str(), on, false);
      }
    }
    if (log) {
      log("LAYERS %s %s (the level's own switch, entities: %s)", name.c_str(), on ? "on" : "off",
          now ? (*now ? "were on" : "were off") : "none");
    }
    return true;
  }
  if (!system || !now) {
    return false;
  }
  level_states.emplace(name, *now);
  if (*now != on) {
    slot<EnableLayer>(system, kEnableLayer)(system, name.c_str(), on, false);
  }
  if (log) {
    log("LAYERS %s %s (level: %s)", name.c_str(), on ? "on" : "off",
        level_states[name] ? "on" : "off");
  }
  return true;
}

void restore(const std::string& name) {
  if (fallback_layers.erase(name) > 0 && fallback) {
    fallback(name, std::nullopt);
  }
  const auto level = level_states.find(name);
  if (level == level_states.end()) {
    return;
  }
  void* system = entity_system();
  const auto now = enabled(name);
  if (system && now && *now != level->second) {
    slot<EnableLayer>(system, kEnableLayer)(system, name.c_str(), level->second, false);
  }
  level_states.erase(level);
}

void restore_all() {
  void* system = entity_system();
  if (system) {
    for (const auto& [name, on] : level_states) {
      const auto now = enabled(name);
      if (now && *now != on) {
        slot<EnableLayer>(system, kEnableLayer)(system, name.c_str(), on, false);
      }
    }
  }
  level_states.clear();
  if (fallback) {
    for (const std::string& name : fallback_layers) {
      fallback(name, std::nullopt);
    }
  }
  fallback_layers.clear();
}

void forget() {
  level_states.clear();
  fallback_layers.clear();
}

std::vector<NearLayer> around(const std::string& map, const float center[3], float radius,
                              std::size_t most) {
  const MapLayers& layers = read_map(map);
  std::map<std::uint16_t, NearLayer> found;
  std::map<std::uint16_t, std::array<double, 3>> sums;
  // The 3D engine answers at most a box's worth of objects at once (level_edits' limit, which a
  // fortress' surroundings exceed), so the area is asked tile by tile; a brush two tiles meet is
  // counted once.
  std::unordered_set<void*> counted;
  const int tiles = radius > kTileHalf ? int(std::ceil(radius / kTileHalf)) : 1;
  for (int ty = -tiles; ty < tiles; ++ty) {
    for (int tx = -tiles; tx < tiles; ++tx) {
      const float tile[3] = {center[0] + (float(tx) + 0.5f) * 2.0f * kTileHalf,
                             center[1] + (float(ty) + 0.5f) * 2.0f * kTileHalf, center[2]};
      for (const auto& brush : brushes_near(tile, kTileHalf)) {
        const float at[3] = {brush.matrix[3], brush.matrix[7], brush.matrix[11]};
        if (brush.layer == 0 || distance(at, center) > radius ||
            !counted.insert(brush.brush).second) {
          continue;
        }
        NearLayer& layer = found[brush.layer];
        layer.id = brush.layer;
        ++layer.brushes;
        layer.shown += brush.hidden ? 0 : 1;
        auto& sum = sums[brush.layer];
        for (int axis = 0; axis < 3; ++axis) {
          sum[std::size_t(axis)] += at[axis];
        }
      }
    }
  }
  for (auto& [id, layer] : found) {
    for (int axis = 0; axis < 3; ++axis) {
      layer.at[axis] = float(sums[id][std::size_t(axis)] / double(layer.brushes));
    }
    const auto name = layers.names.find(id);
    if (name != layers.names.end()) {
      layer.name = name->second;
    }
  }
  for (const auto& [id, name] : layers.names) {
    const auto place = layers.places.find(name);
    if (place == layers.places.end() || distance(place->second.centre, center) > radius) {
      continue;
    }
    NearLayer& layer = found[id];
    if (layer.brushes == 0) {
      std::copy(place->second.centre, place->second.centre + 3, layer.at);
    }
    layer.id = id;
    layer.name = name;
  }
  std::vector<NearLayer> result;
  for (auto& [id, layer] : found) {
    if (layer.name.empty()) {
      continue;
    }
    const auto place = layers.places.find(layer.name);
    if (place != layers.places.end()) {
      layer.path = place->second.path;
      layer.entities = place->second.entities;
    }
    layer.on = enabled(layer.name).value_or(false);
    result.push_back(std::move(layer));
  }
  std::sort(result.begin(), result.end(), [center](const NearLayer& a, const NearLayer& b) {
    return distance(a.at, center) < distance(b.at, center);
  });
  if (result.size() > most) {
    result.resize(most);
  }
  return result;
}

}  // namespace kcds::runtime::level::layers
