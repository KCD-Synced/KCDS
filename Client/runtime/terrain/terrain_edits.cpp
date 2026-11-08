#include "terrain_edits.hpp"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../core/natives.hpp"
#include "merged_grass.hpp"

namespace kcds::runtime::terrain {
namespace {

using kcds::game_client::resolve_unique;

// The end of the game's deformation apply (+ 0x249): the nodes in the box (+ 17), each
// rebuilt (+ 63), the two height caches cleared (+ 105, + 125).
constexpr char kDeformTail[] =
    "49 8B 48 28 48 85 C9 74 0D 4C 8D 45 ?? 48 8D 55 ?? E8 ?? ?? ?? ?? 48 8B 55 ?? 33 F6 48 8B "
    "5D ?? 48 8B C2 48 2B C3 48 C1 F8 03 48 85 C0 74 ?? 33 FF 48 8B 0C 3B 48 85 C9 74 ?? 45 33 C0 "
    "33 D2 E8 ?? ?? ?? ?? 48 8B 55 ?? 48 8B 5D ?? FF C6 48 8B CA 48 2B CB 48 63 C6 48 C1 F9 03 48 "
    "83 C7 08 48 3B C1 72 ?? BF 00 80 00 00 48 8D 0D ?? ?? ?? ?? 44 8B C7 33 D2 E8 ?? ?? ?? ?? 44 "
    "8B C7 48 8D 0D";
constexpr int kNodesInBoxCall = 18;
constexpr int kNodesInBoxEnd = 22;
constexpr int kRebuildCall = 64;
constexpr int kRebuildEnd = 68;
// A sector node's load from the level: its palette of surface types (+ 168) emptied, then sized
// to the count it read (the vector's resize, the third E8).
constexpr char kPaletteLoad[] =
    "48 8D BB A8 00 00 00 41 8B F4 48 8B 17 48 8B 47 08 48 2B C2 48 C1 F8 05 48 85 C0 74 27 49 8B "
    "CC 48 C1 E1 05 48 03 CA E8 ?? ?? ?? ?? 48 8B 17 FF C6 48 8B 47 08 48 2B C2 48 63 CE 48 C1 F8 "
    "05 48 3B C8 72 DC 48 8B CF E8 ?? ?? ?? ?? 48 63 55 DF 48 8B CF E8";
constexpr int kPaletteResizeCall = 84;
constexpr int kPaletteResizeEnd = 88;
constexpr int kCacheALoad = 108;
constexpr int kCacheAEnd = 112;
constexpr int kCacheBLoad = 128;
constexpr int kCacheBEnd = 132;
constexpr std::size_t kHeightCacheBytes = 0x8000;
// The deformation apply's start: the size of a heightmap unit in metres (+ 18).
constexpr char kUnitLoad[] =
    "83 B8 70 03 00 00 00 0F 84 ?? ?? ?? ?? F3 44 0F 10 15 ?? ?? ?? ?? 0F 57 DB F3 0F 10 52 10";
constexpr int kUnitLoadAt = 18;
constexpr int kUnitLoadEnd = 22;
// C3DEngine::OnExplosion: the terrain (CTerrain*) global (+ 24).
constexpr char kTerrainLoad[] =
    "48 8B 05 ?? ?? ?? ?? 40 B6 01 39 98 70 03 00 00 75 03 40 8A F3 4C 8B 35 ?? ?? ?? ?? 0F 28 D7 "
    "49 8B CE";
constexpr int kTerrainLoadAt = 24;
constexpr int kTerrainLoadEnd = 28;

// C3DEngine::OnExplosion's roads: the render nodes of type 11 (roads) in the crater's
// box gathered into a vector the game allocates (+ 23), each laid on the ground as it is
// now (+ 60), and the vector freed (+ 117).
constexpr char kRoadsTail[] =
    "41 8D 51 0B F2 0F 11 44 24 ?? 89 45 ?? 48 C7 44 24 20 FF FF FF FF E8 ?? ?? ?? ?? 48 8B 44 24 "
    "?? 44 8B F3 4C 8B 44 24 ?? 49 2B C0 48 C1 F8 03 48 85 C0 74 ?? 48 8B F3 49 8B 0C 30 E8 ?? ?? "
    "?? ?? 48 8B 4C 24 ?? 48 8D 76 08 4C 8B 44 24 ?? 41 FF C6 49 2B C8 49 63 C6 48 C1 F9 03 48 3B "
    "C1 72 ?? 4D 85 C0 74 ?? 48 8B 54 24 ?? 49 8B C8 49 2B D0 48 83 E2 F8 E8 ?? ?? ?? ??";
constexpr int kRoadsInBoxCall = 23;
constexpr int kRoadsInBoxEnd = 27;
constexpr int kRelayRoadCall = 60;
constexpr int kRelayRoadEnd = 64;
constexpr int kFreeCall = 117;
constexpr int kFreeEnd = 121;
constexpr int kRoadType = 11;

constexpr std::size_t kTerrainHeightmap = 80;
constexpr std::size_t kGrid = 16;
constexpr std::size_t kGridWidth = 8;
constexpr std::size_t kRootNode = 40;
constexpr std::size_t kSectorShift = 96;
constexpr std::size_t kUnits = 224;
constexpr std::size_t kNodeHeights = 192;
constexpr std::uint32_t kHeightShift = 20;
constexpr std::uint32_t kLowMask = (1u << kHeightShift) - 1;
constexpr long kTopLevel = (1L << (32 - kHeightShift)) - 1;
// The box the nodes are looked for in reaches this far above and below.
constexpr float kBoxHeight = 2000.0f;
// A widened sector reaches this much further than the height that needed it, so the next dabs of
// a stroke fit without widening it again.
constexpr float kWidenMargin = 10.0f;
// One edit changes at most this many samples (a circle 180 m across).
constexpr int kMaxSamples = 25000;
// The ground's look (terrain_edits.hpp, Paint): a sector node's local surfaces table and palette
// list, the level's surface types, a sample's surface bits.
constexpr std::size_t kNodeSurfaceList = 168;
constexpr std::size_t kNodeLocalSurfaces = 216;
constexpr std::size_t kTerrainSurfaceTypes = 200;
constexpr std::size_t kSurfaceTypeBytes = 440;
constexpr int kLocalSurfaces = 15;
constexpr std::uint8_t kNoSurface = 127;
constexpr int kFullWeight = 15;
constexpr std::size_t kSurfaceListEntry = 32;
// A surface type's material (its name is the first 128 bytes), and the most entries a sector's
// palette holds (the level's load stops at 128).
constexpr std::size_t kSurfaceTypeMaterial = 128;
constexpr std::size_t kPaletteMost = 128;

struct NodeHeights {
  float base;
  float step;
  std::uint32_t* samples;
  std::uint16_t size;
  std::uint8_t lower_detail;
};

struct NodeList {
  std::uint8_t** begin = nullptr;
  std::uint8_t** end = nullptr;
  std::uint8_t** capacity = nullptr;
};

using NodesInBox = void(__fastcall*)(void* root, const float* box, NodeList* out);
using RoadsInBox = void(__fastcall*)(NodeList* out, int type, const float* box, std::int64_t none,
                                     std::int64_t all);
using RelayRoad = void(__fastcall*)(void* road);
using FreeBlock = void(__fastcall*)(void* block, std::size_t bytes);
using RebuildNode = void(__fastcall*)(void* node, std::int64_t a, std::int64_t b);
// The palette vector's resize: the entries kept, the new ones empty (no type, no meshes).
using ResizePalette = void(__fastcall*)(void* palette, std::uint64_t count);

// A sample as it was before an edit.
struct Saved {
  std::uint32_t* sample = nullptr;
  std::uint32_t value = 0;
};

// A sector's height block as it was before an edit widened its range.
struct SavedNode {
  NodeHeights* node = nullptr;
  float base = 0.0f;
  float step = 0.0f;
  std::vector<std::uint32_t> samples;
};

// A sector's local surfaces table as it was before a paint gave a free slot a surface.
struct SavedTable {
  std::uint8_t* table = nullptr;
  std::uint8_t bytes[16] = {};
};

struct Kept {
  std::vector<Saved> samples;
  std::vector<SavedNode> nodes;
  std::vector<SavedTable> tables;
  float box[6] = {};
};

Logger log_line = nullptr;
std::uint8_t** terrain_global = nullptr;
const float* unit_size = nullptr;
NodesInBox nodes_in_box = nullptr;
RebuildNode rebuild_node = nullptr;
// Optional: without it a sector takes only the surface types its palette has.
ResizePalette resize_palette = nullptr;
// Optional: without them the ground changes and the roads stay where they were.
RoadsInBox roads_in_box = nullptr;
RelayRoad relay_road = nullptr;
FreeBlock free_block = nullptr;
void* cache_a = nullptr;
void* cache_b = nullptr;
std::map<std::uint32_t, Kept> kept;

std::uint8_t* heightmap() {
  std::uint8_t* terrain = terrain_global ? *terrain_global : nullptr;
  return terrain ? *reinterpret_cast<std::uint8_t**>(terrain + kTerrainHeightmap) : nullptr;
}

NodeHeights* heights_of(std::uint8_t* map, int x, int y) {
  const int shift = *reinterpret_cast<int*>(map + kSectorShift);
  auto* grid = *reinterpret_cast<std::uint8_t**>(map + kGrid);
  if (!grid) {
    return nullptr;
  }
  auto** nodes = *reinterpret_cast<std::uint8_t***>(grid);
  const int width = *reinterpret_cast<int*>(grid + kGridWidth);
  std::uint8_t* node = nodes[(y >> shift) + width * (x >> shift)];
  if (!node) {
    return nullptr;
  }
  auto* heights = reinterpret_cast<NodeHeights*>(node + kNodeHeights);
  if (!heights->samples || heights->lower_detail != 0 || heights->size < 2) {
    return nullptr;
  }
  return heights;
}

std::uint32_t* sample_in(NodeHeights* heights, int local_x, int local_y) {
  return &heights->samples[local_y + heights->size * local_x];
}

float height_of(const NodeHeights* heights, std::uint32_t sample) {
  return float(sample >> kHeightShift) * heights->step + heights->base;
}

// The sample of unit (x, y) in its own sector and, on a sector's first row or column, the same
// sample kept by the sectors before it.
int samples_of(std::uint8_t* map, int x, int y, std::uint32_t* out[4], NodeHeights* owners[4]) {
  const int shift = *reinterpret_cast<int*>(map + kSectorShift);
  const int sector = 1 << shift;
  const int local_x = x & (sector - 1);
  const int local_y = y & (sector - 1);
  int count = 0;
  for (int dx = 0; dx <= 1; ++dx) {
    for (int dy = 0; dy <= 1; ++dy) {
      if ((dx == 1 && (local_x != 0 || x == 0)) || (dy == 1 && (local_y != 0 || y == 0))) {
        continue;
      }
      NodeHeights* heights = heights_of(map, x - dx, y - dy);
      if (!heights) {
        continue;
      }
      const int edge = heights->size - 1;
      out[count] = sample_in(heights, dx == 1 ? edge : local_x, dy == 1 ? edge : local_y);
      owners[count] = heights;
      ++count;
    }
  }
  return count;
}

// Widens a sector's range of heights to reach `height` (a pit dug deep, a hill raised high: owner,
// "a hole under the ground, a quarry"). Its 4096 levels then span the wider range:
// every sample of it is encoded again (the surface bits kept) and its base and step changed, as
// the game reads them for each sample (height = level * step + base). The block as it was is kept
// once per edit, for revert.
void widen(NodeHeights* node, float height, Kept& saved) {
  const float top = node->base + node->step * float(kTopLevel);
  const float low = (std::min)(node->base, height - kWidenMargin);
  const float high = (std::max)(top, height + kWidenMargin);
  const float step = (high - low) / float(kTopLevel);
  const std::size_t count = std::size_t(node->size) * node->size;
  const bool known = std::any_of(saved.nodes.begin(), saved.nodes.end(),
                                 [node](const SavedNode& kept) { return kept.node == node; });
  if (!known) {
    SavedNode kept;
    kept.node = node;
    kept.base = node->base;
    kept.step = node->step;
    kept.samples.assign(node->samples, node->samples + count);
    saved.nodes.push_back(std::move(kept));
  }
  for (std::size_t index = 0; index < count; ++index) {
    std::uint32_t& sample = node->samples[index];
    const float was = height_of(node, sample);
    const long level = std::clamp(std::lround((was - low) / step), 0L, kTopLevel);
    sample = (sample & kLowMask) | (std::uint32_t(level) << kHeightShift);
  }
  node->base = low;
  node->step = step;
}

// 1 inside the radius, smoothly down to 0 at radius + falloff.
float weight(float distance, float radius, float falloff) {
  if (distance <= radius) {
    return 1.0f;
  }
  if (falloff <= 0.0f || distance >= radius + falloff) {
    return 0.0f;
  }
  const float t = 1.0f - (distance - radius) / falloff;
  return t * t * (3.0f - 2.0f * t);
}

// A sample's surfaces: up to three local surfaces and their weights out of kFullWeight.
struct Blend {
  int surface[3] = {0, 0, 0};
  int weight[3] = {kFullWeight, 0, 0};
};

Blend decode(std::uint32_t sample) {
  Blend blend;
  blend.surface[0] = int(sample & 0xF);
  blend.surface[1] = int((sample >> 4) & 0xF);
  blend.surface[2] = int((sample >> 8) & 0xF);
  blend.weight[1] = int((sample >> 12) & 0xF);
  blend.weight[2] = int((sample >> 16) & 0xF);
  blend.weight[0] = (std::max)(0, kFullWeight - blend.weight[1] - blend.weight[2]);
  return blend;
}

std::uint32_t encode(const Blend& blend) {
  return std::uint32_t(blend.surface[0]) | (std::uint32_t(blend.surface[1]) << 4) |
         (std::uint32_t(blend.surface[2]) << 8) | (std::uint32_t(blend.weight[1]) << 12) |
         (std::uint32_t(blend.weight[2]) << 16);
}

// `local` given `strength` (0 to 1) of the sample: the others share the rest by their weights, the
// strongest three kept, the first taking what rounding leaves.
std::uint32_t paint_sample(std::uint32_t sample, int local, float strength) {
  const Blend before = decode(sample);
  float shares[16] = {};
  for (int i = 0; i < 3; ++i) {
    if (before.weight[i] > 0) {
      shares[before.surface[i]] += float(before.weight[i]) / float(kFullWeight);
    }
  }
  for (float& share : shares) {
    share *= 1.0f - strength;
  }
  shares[local] += strength;
  int order[16];
  for (int i = 0; i < 16; ++i) {
    order[i] = i;
  }
  std::stable_sort(order, order + 16, [&](int a, int b) { return shares[a] > shares[b]; });
  Blend after;
  after.surface[0] = order[0];
  int given = 0;
  for (int i = 1; i < 3; ++i) {
    const int weight = int(std::lround(shares[order[i]] * float(kFullWeight)));
    after.surface[i] = weight > 0 ? order[i] : order[0];
    after.weight[i] = weight;
    given += weight;
  }
  if (given > kFullWeight) {
    after.weight[2] = (std::max)(0, kFullWeight - after.weight[1]);
  }
  return (sample & ~kLowMask) | encode(after);
}

// The level's surface type for `material`: "name|part|part ...", the type of that name (any
// case), else the first whose name (a material file's path) holds the first part, then the next.
// -1 when none does. The level's names go to the log once.
int surface_named(const std::string& material) {
  std::uint8_t* terrain = terrain_global ? *terrain_global : nullptr;
  auto* types =
      terrain ? *reinterpret_cast<std::uint8_t**>(terrain + kTerrainSurfaceTypes) : nullptr;
  if (!types) {
    return -1;
  }
  const auto lower = [](std::string text) {
    for (char& c : text) {
      c = char(std::tolower(std::uint8_t(c)));
    }
    return text;
  };
  std::vector<std::string> parts;
  for (std::size_t start = 0; start <= material.size();) {
    const std::size_t bar = (std::min)(material.find('|', start), material.size());
    if (bar > start) {
      parts.push_back(lower(material.substr(start, bar - start)));
    }
    start = bar + 1;
  }
  std::vector<std::string> names(static_cast<std::size_t>(kNoSurface));
  static bool listed = false;
  std::string listing;
  for (int surface = 0; surface < int(kNoSurface); ++surface) {
    const auto* name = reinterpret_cast<const char*>(types + kSurfaceTypeBytes * surface);
    names[std::size_t(surface)] = lower(std::string(name, strnlen(name, kSurfaceTypeMaterial)));
    if (!listed && !names[std::size_t(surface)].empty()) {
      listing += std::to_string(surface) + "=" + names[std::size_t(surface)] + " ";
    }
  }
  if (!listed && log_line) {
    listed = true;
    log_line("TERRAIN the level's surface types: %s", listing.c_str());
  }
  for (std::size_t part = 0; part < parts.size(); ++part) {
    for (int surface = 0; surface < int(kNoSurface); ++surface) {
      const std::string& own = names[std::size_t(surface)];
      const bool same = own == parts[part];
      // The first part is the material's own name: only a type of that very name.
      if (!own.empty() && (same || (part > 0 && own.find(parts[part]) != std::string::npos))) {
        return surface;
      }
    }
  }
  return -1;
}

// The local surface (0 to 14) of the level's surface type `surface` in the sector `owner`, a free
// slot given it when the sector's palette has it; -1 when it cannot be painted there.
int local_surface(NodeHeights* owner, int surface, Kept& saved) {
  std::uint8_t* terrain = terrain_global ? *terrain_global : nullptr;
  auto* node = reinterpret_cast<std::uint8_t*>(owner) - kNodeHeights;
  auto* table = *reinterpret_cast<std::uint8_t**>(node + kNodeLocalSurfaces);
  if (!terrain || !table || surface < 0 || surface >= int(kNoSurface)) {
    return -1;
  }
  for (int local = 0; local < kLocalSurfaces; ++local) {
    if (table[local] == surface) {
      return local;
    }
  }
  // The sector's palette must have a detail layer for it.
  auto* types = *reinterpret_cast<std::uint8_t**>(terrain + kTerrainSurfaceTypes);
  auto* begin = *reinterpret_cast<std::uint8_t**>(node + kNodeSurfaceList);
  auto* end = *reinterpret_cast<std::uint8_t**>(node + kNodeSurfaceList + 8);
  bool listed = false;
  for (std::uint8_t* entry = begin; types && entry && entry < end; entry += kSurfaceListEntry) {
    auto* type = *reinterpret_cast<std::uint8_t**>(entry);
    if (type == types + kSurfaceTypeBytes * std::size_t(surface)) {
      listed = true;
      break;
    }
  }
  if (!listed) {
    // A surface type the level defines (its name and material) joins the sector's palette: the
    // sector draws its layer once the node is rebuilt (rebuild).
    std::uint8_t* type = types ? types + kSurfaceTypeBytes * std::size_t(surface) : nullptr;
    const auto count = std::size_t(end - begin) / kSurfaceListEntry;
    if (!resize_palette || !type || type[0] == 0 ||
        *reinterpret_cast<void**>(type + kSurfaceTypeMaterial) == nullptr ||
        count >= kPaletteMost) {
      return -1;
    }
    resize_palette(node + kNodeSurfaceList, count + 1);
    auto* grown = *reinterpret_cast<std::uint8_t**>(node + kNodeSurfaceList);
    if (!grown) {
      return -1;
    }
    *reinterpret_cast<std::uint8_t**>(grown + kSurfaceListEntry * count) = type;
    if (log_line) {
      log_line("TERRAIN surface %d (%s) added to a sector's palette (%zu there)", surface,
               reinterpret_cast<const char*>(type), count + 1);
    }
  }
  for (int local = 0; local < kLocalSurfaces; ++local) {
    if (table[local] != kNoSurface) {
      continue;
    }
    const bool kept_before =
        std::any_of(saved.tables.begin(), saved.tables.end(),
                    [&](const SavedTable& kept_table) { return kept_table.table == table; });
    if (!kept_before) {
      SavedTable kept_table;
      kept_table.table = table;
      std::memcpy(kept_table.bytes, table, sizeof(kept_table.bytes));
      saved.tables.push_back(kept_table);
    }
    table[local] = std::uint8_t(surface);
    return local;
  }
  return -1;
}

// The roads in the box laid on the ground as it is now, as the game's crater does.
void relay_roads(const float box[6]) {
  if (!roads_in_box || !relay_road || !free_block) {
    return;
  }
  NodeList roads;
  roads_in_box(&roads, kRoadType, box, 0, -1);
  int count = 0;
  for (std::uint8_t** road = roads.begin; road && road < roads.end; ++road) {
    if (*road) {
      relay_road(*road);
      ++count;
    }
  }
  if (roads.begin) {
    const auto bytes = std::size_t(reinterpret_cast<std::uint8_t*>(roads.capacity) -
                                   reinterpret_cast<std::uint8_t*>(roads.begin));
    free_block(roads.begin, bytes & ~std::size_t(7));
  }
  if (count > 0 && log_line) {
    log_line("TERRAIN %d road(s) laid on the new ground", count);
  }
}

// What the game does after its own deformation: the nodes in the box rebuilt, the height caches
// cleared. The terrain's collision reads the heights as they are now: nothing more to tell.
void rebuild(const float box[6]) {
  std::uint8_t* map = heightmap();
  void* root = map ? *reinterpret_cast<void**>(map + kRootNode) : nullptr;
  NodeList nodes;
  if (root) {
    nodes_in_box(root, box, &nodes);
  }
  for (std::uint8_t** node = nodes.begin; node && node < nodes.end; ++node) {
    if (*node) {
      rebuild_node(*node, 0, 0);
    }
  }
  std::memset(cache_a, 0, kHeightCacheBytes);
  std::memset(cache_b, 0, kHeightCacheBytes);
  relay_roads(box);
}

}  // namespace

bool install(Logger logger) {
  log_line = logger;
  terrain_global = static_cast<std::uint8_t**>(
      resolve_unique("TerrainGlobal", kTerrainLoad, kTerrainLoadAt, kTerrainLoadEnd));
  unit_size = static_cast<const float*>(
      resolve_unique("TerrainUnit", kUnitLoad, kUnitLoadAt, kUnitLoadEnd));
  nodes_in_box = reinterpret_cast<NodesInBox>(
      resolve_unique("TerrainNodesInBox", kDeformTail, kNodesInBoxCall, kNodesInBoxEnd));
  rebuild_node = reinterpret_cast<RebuildNode>(
      resolve_unique("TerrainRebuildNode", kDeformTail, kRebuildCall, kRebuildEnd));
  resize_palette = reinterpret_cast<ResizePalette>(
      resolve_unique("TerrainPaletteResize", kPaletteLoad, kPaletteResizeCall, kPaletteResizeEnd));
  cache_a = resolve_unique("TerrainCacheA", kDeformTail, kCacheALoad, kCacheAEnd);
  cache_b = resolve_unique("TerrainCacheB", kDeformTail, kCacheBLoad, kCacheBEnd);
  roads_in_box = reinterpret_cast<RoadsInBox>(
      resolve_unique("TerrainRoadsInBox", kRoadsTail, kRoadsInBoxCall, kRoadsInBoxEnd));
  relay_road = reinterpret_cast<RelayRoad>(
      resolve_unique("TerrainRelayRoad", kRoadsTail, kRelayRoadCall, kRelayRoadEnd));
  free_block = reinterpret_cast<FreeBlock>(
      resolve_unique("TerrainFreeBlock", kRoadsTail, kFreeCall, kFreeEnd));
  const bool found =
      terrain_global && unit_size && nodes_in_box && rebuild_node && cache_a && cache_b;
  if (log_line) {
    log_line("TERRAIN edits %s (roads follow the ground: %s; new surfaces painted: %s)",
             found ? "installed" : "unavailable",
             roads_in_box && relay_road && free_block ? "yes" : "no",
             resize_palette ? "yes" : "no");
  }
  return found;
}

Result apply(std::uint32_t id, const Edit& edit) {
  Result result;
  std::uint8_t* map = heightmap();
  if (!map || !nodes_in_box || edit.radius < 0.0f || edit.falloff < 0.0f) {
    return result;
  }
  const float unit = *unit_size;
  const int units = *reinterpret_cast<int*>(map + kUnits);
  const float reach = edit.radius + edit.falloff;
  const int first_x = (std::max)(0, int(std::floor((edit.x - reach) / unit)));
  const int last_x = (std::min)(units - 1, int(std::ceil((edit.x + reach) / unit)));
  const int first_y = (std::max)(0, int(std::floor((edit.y - reach) / unit)));
  const int last_y = (std::min)(units - 1, int(std::ceil((edit.y + reach) / unit)));
  if ((last_x - first_x + 1) * (last_y - first_y + 1) > kMaxSamples) {
    if (log_line) {
      log_line("TERRAIN edit %u too large (%.0f m across)", id, 2.0f * reach);
    }
    return result;
  }
  // The grass standing on this ground moves with it (merged_grass.hpp).
  const float column[6] = {edit.x - reach - unit, edit.y - reach - unit, -kBoxHeight,
                           edit.x + reach + unit, edit.y + reach + unit, kBoxHeight};
  grass::before(column);
  Kept& saved = kept[id];
  saved.samples.clear();
  // A paint's surface type: the level's own of the material's name, else the number given.
  const int named = edit.material.empty() ? -1 : surface_named(edit.material);
  const int surface = named >= 0 ? named : int(edit.value);
  float low = 1e9f;
  float high = -1e9f;
  for (int ux = first_x; ux <= last_x; ++ux) {
    for (int uy = first_y; uy <= last_y; ++uy) {
      const float dx = float(ux) * unit - edit.x;
      const float dy = float(uy) * unit - edit.y;
      const float w = weight(std::sqrt(dx * dx + dy * dy), edit.radius, edit.falloff);
      if (w <= 0.0f) {
        continue;
      }
      std::uint32_t* samples[4] = {};
      NodeHeights* owners[4] = {};
      const int count = samples_of(map, ux, uy, samples, owners);
      if (count == 0) {
        continue;
      }
      const float before = height_of(owners[0], *samples[0]);
      if (edit.shape == Shape::Paint) {
        bool painted = false;
        for (int k = 0; k < count; ++k) {
          const int local = local_surface(owners[k], surface, saved);
          if (local < 0) {
            continue;
          }
          saved.samples.push_back({samples[k], *samples[k]});
          *samples[k] = paint_sample(*samples[k], local, w);
          painted = true;
        }
        if (painted) {
          ++result.samples;
        } else {
          ++result.refused;
        }
        low = (std::min)(low, before);
        high = (std::max)(high, before);
        continue;
      }
      const float target = edit.shape == Shape::Flatten ? edit.value : before + edit.value;
      const float after = before + (target - before) * w;
      for (int k = 0; k < count; ++k) {
        long level = std::lround((after - owners[k]->base) / owners[k]->step);
        if (level < 0 || level > kTopLevel) {
          widen(owners[k], after, saved);
          ++result.widened;
          level = std::lround((after - owners[k]->base) / owners[k]->step);
        }
        if (level < 0 || level > kTopLevel) {
          ++result.clamped;
          level = std::clamp(level, 0L, kTopLevel);
        }
        saved.samples.push_back({samples[k], *samples[k]});
        *samples[k] = (*samples[k] & kLowMask) | (std::uint32_t(level) << kHeightShift);
      }
      low = (std::min)(low, (std::min)(before, after));
      high = (std::max)(high, (std::max)(before, after));
      ++result.samples;
    }
  }
  const float box[6] = {edit.x - reach - unit, edit.y - reach - unit, low - kBoxHeight,
                        edit.x + reach + unit, edit.y + reach + unit, high + kBoxHeight};
  std::copy(box, box + 6, saved.box);
  rebuild(box);
  grass::after();
  result.applied = true;
  if (log_line) {
    const char* shape = edit.shape == Shape::Flatten ? "flatten"
                        : edit.shape == Shape::Raise ? "raise"
                                                     : "paint";
    log_line(
        "TERRAIN edit %u %s at %.1f %.1f r %.1f+%.1f by %.2f: %d samples, %d clamped, %d widened, "
        "%d refused",
        id, shape, edit.x, edit.y, edit.radius, edit.falloff, edit.value, result.samples,
        result.clamped, result.widened, result.refused);
  }
  return result;
}

bool revert(std::uint32_t id) {
  const auto found = kept.find(id);
  if (found == kept.end()) {
    return false;
  }
  grass::before(found->second.box);
  // Back to front: a sample saved twice gets its oldest value last.
  for (auto it = found->second.samples.rbegin(); it != found->second.samples.rend(); ++it) {
    *it->sample = it->value;
  }
  // A sector whose free local surface a paint used: its table as it was.
  for (const SavedTable& table : found->second.tables) {
    std::memcpy(table.table, table.bytes, sizeof(table.bytes));
  }
  // A sector the edit widened: its block as it was, range and samples.
  for (const SavedNode& node : found->second.nodes) {
    node.node->base = node.base;
    node.node->step = node.step;
    std::copy(node.samples.begin(), node.samples.end(), node.node->samples);
  }
  rebuild(found->second.box);
  grass::after();
  kept.erase(found);
  if (log_line) {
    log_line("TERRAIN edit %u reverted", id);
  }
  return true;
}

bool height_at(float x, float y, float& height) {
  std::uint8_t* map = heightmap();
  if (!map || !unit_size) {
    return false;
  }
  const int ux = int(std::lround(x / *unit_size));
  const int uy = int(std::lround(y / *unit_size));
  const int units = *reinterpret_cast<int*>(map + kUnits);
  if (ux < 0 || uy < 0 || ux >= units || uy >= units) {
    return false;
  }
  std::uint32_t* samples[4] = {};
  NodeHeights* owners[4] = {};
  if (samples_of(map, ux, uy, samples, owners) == 0) {
    return false;
  }
  height = height_of(owners[0], *samples[0]);
  return true;
}

void reset() {
  kept.clear();
}

}  // namespace kcds::runtime::terrain
