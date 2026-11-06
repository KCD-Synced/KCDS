#include "../core/client_internal.hpp"

#include "terrain_probe.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "../core/log.hpp"

namespace kcds::runtime::testing::terrain_probe {
namespace {

using kcds::game_client::log;

// WHGame.dll 1.5.5 at its. A test-only probe names what it uses by address;
// the mod's own code finds it by signature once the path is proven.
constexpr std::uintptr_t kIdaBase = 0x180000000ULL;
// The terrain (CTerrain*) and its heightmap: the deformation list at terrain + 56 keeps it at
// + 24 (C3DEngine::OnExplosion -> ->).
constexpr std::uintptr_t kTerrain = 0x185496F68ULL;
constexpr std::size_t kTerrainHeightmap = 56 + 24;
// The heightmap: its sector grid at + 16 ({ nodes**, width at + 8 }), its root node at + 40, the
// sector shift (in units) at + 96, its size in units at + 224.
constexpr std::size_t kGrid = 16;
constexpr std::size_t kGridWidth = 8;
constexpr std::size_t kRootNode = 40;
constexpr std::size_t kSectorShift = 96;
constexpr std::size_t kUnits = 224;
// A sector node's heights (+ 192): base (+ 0), step (+ 4), samples (+ 8), samples a side (+ 16),
// a lower detail shift (+ 18). A sample's height is (sample >> 20) * step + base.
constexpr std::size_t kNodeHeights = 192;
constexpr int kHeightBits = 12;
constexpr std::uint32_t kHeightShift = 20;
constexpr std::uint32_t kLowMask = (1u << kHeightShift) - 1;
// One over a unit's size in metres, and the unit's size.
constexpr std::uintptr_t kInverseUnit = 0x184920104ULL;
constexpr std::uintptr_t kUnitSize = 0x1849200E8ULL;
// The nodes in a box (on the root node), a node rebuilt, and
// the two height caches the deformation clears (0x8000 bytes each).
constexpr std::uintptr_t kNodesInBox = 0x183555E40ULL;
constexpr std::uintptr_t kRebuildNode = 0x18073A370ULL;
constexpr std::uintptr_t kHeightCacheA = 0x18533CF00ULL;
constexpr std::uintptr_t kHeightCacheB = 0x185344F00ULL;
constexpr std::size_t kHeightCacheBytes = 0x8000;

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
using RebuildNode = void(__fastcall*)(void* node, std::int64_t a, std::int64_t b);

HANDLE event = nullptr;

template <typename Pointer>
Pointer at(std::uintptr_t ida_address) {
  const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"WHGame.dll"));
  return reinterpret_cast<Pointer>(base + (ida_address - kIdaBase));
}

std::uint8_t* sector_node(std::uint8_t* heightmap, int x, int y) {
  const int shift = *reinterpret_cast<int*>(heightmap + kSectorShift);
  auto* grid = *reinterpret_cast<std::uint8_t**>(heightmap + kGrid);
  if (!grid) {
    return nullptr;
  }
  auto** nodes = *reinterpret_cast<std::uint8_t***>(grid);
  const int width = *reinterpret_cast<int*>(grid + kGridWidth);
  return nodes[(y >> shift) + width * (x >> shift)];
}

// Writes the height into the sample at unit (x, y) of the node; false when it is out of the
// node's range (the node's base and step allow 4096 levels) or the node keeps lower detail.
bool write(std::uint8_t* node, int x, int y, float height, int& clamped) {
  if (!node) {
    return false;
  }
  auto* heights = reinterpret_cast<NodeHeights*>(node + kNodeHeights);
  if (!heights->samples || heights->lower_detail != 0 || heights->size < 2) {
    return false;
  }
  const int mask = heights->size - 2;
  const int index = (y & mask) + heights->size * (x & mask);
  long level = std::lround((height - heights->base) / heights->step);
  const long top = (1L << kHeightBits) - 1;
  if (level < 0 || level > top) {
    ++clamped;
    level = std::clamp(level, 0L, top);
  }
  std::uint32_t& sample = heights->samples[index];
  sample = (sample & kLowMask) | (static_cast<std::uint32_t>(level) << kHeightShift);
  return true;
}

void run(float x, float y, float half, float height) {
  auto* terrain = *at<std::uint8_t**>(kTerrain);
  std::uint8_t* heightmap =
      terrain ? *reinterpret_cast<std::uint8_t**>(terrain + kTerrainHeightmap) : nullptr;
  if (!heightmap) {
    log("TERRAINPROBE no heightmap");
    return;
  }
  const float inverse_unit = *at<float*>(kInverseUnit);
  const float unit = *at<float*>(kUnitSize);
  const int units = *reinterpret_cast<int*>(heightmap + kUnits);
  const int shift = *reinterpret_cast<int*>(heightmap + kSectorShift);
  const int first_x = std::max(0, static_cast<int>((x - half) * inverse_unit));
  const int last_x = std::min(units - 1, static_cast<int>((x + half) * inverse_unit));
  const int first_y = std::max(0, static_cast<int>((y - half) * inverse_unit));
  const int last_y = std::min(units - 1, static_cast<int>((y + half) * inverse_unit));
  int written = 0;
  int refused = 0;
  int clamped = 0;
  const int sector = 1 << shift;
  for (int ux = first_x; ux <= last_x; ++ux) {
    for (int uy = first_y; uy <= last_y; ++uy) {
      if (write(sector_node(heightmap, ux, uy), ux, uy, height, clamped)) {
        ++written;
      } else {
        ++refused;
      }
      // A sample on a sector's first row or column is also the last of the sector before it.
      const bool edge_x = (ux % sector) == 0 && ux > 0;
      const bool edge_y = (uy % sector) == 0 && uy > 0;
      if (edge_x) {
        auto* node = sector_node(heightmap, ux - 1, uy);
        auto* heights = node ? reinterpret_cast<NodeHeights*>(node + kNodeHeights) : nullptr;
        if (heights && heights->samples && heights->lower_detail == 0) {
          const int index = (uy & (heights->size - 2)) + heights->size * (heights->size - 1);
          long level = std::lround((height - heights->base) / heights->step);
          level = std::clamp(level, 0L, (1L << kHeightBits) - 1);
          std::uint32_t& sample = heights->samples[index];
          sample = (sample & kLowMask) | (static_cast<std::uint32_t>(level) << kHeightShift);
        }
      }
      if (edge_y) {
        auto* node = sector_node(heightmap, ux, uy - 1);
        auto* heights = node ? reinterpret_cast<NodeHeights*>(node + kNodeHeights) : nullptr;
        if (heights && heights->samples && heights->lower_detail == 0) {
          const int index = (heights->size - 1) + heights->size * (ux & (heights->size - 2));
          long level = std::lround((height - heights->base) / heights->step);
          level = std::clamp(level, 0L, (1L << kHeightBits) - 1);
          std::uint32_t& sample = heights->samples[index];
          sample = (sample & kLowMask) | (static_cast<std::uint32_t>(level) << kHeightShift);
        }
      }
    }
  }
  // As the game's own deformation: the nodes in the box are rebuilt, the height caches cleared.
  const float box[6] = {x - half - unit, y - half - unit, height - 1000.0f,
                        x + half + unit, y + half + unit, height + 1000.0f};
  NodeList nodes;
  void* root = *reinterpret_cast<void**>(heightmap + kRootNode);
  if (root) {
    at<NodesInBox>(kNodesInBox)(root, box, &nodes);
  }
  int rebuilt = 0;
  for (std::uint8_t** node = nodes.begin; node && node < nodes.end; ++node) {
    if (*node) {
      at<RebuildNode>(kRebuildNode)(*node, 0, 0);
      ++rebuilt;
    }
  }
  std::memset(at<void*>(kHeightCacheA), 0, kHeightCacheBytes);
  std::memset(at<void*>(kHeightCacheB), 0, kHeightCacheBytes);
  log("TERRAINPROBE %.1f %.1f half %.1f to %.2f: %d samples set, %d refused, %d clamped, %d "
      "nodes rebuilt (unit %.2f, sector %d units, %d units)",
      x, y, half, height, written, refused, clamped, rebuilt, unit, sector, units);
}

}  // namespace

void install() {
  wchar_t name[96];
  swprintf_s(name, L"Local\\KCD2S.ProbeTerrain.%lu", GetCurrentProcessId());
  event = CreateEventW(nullptr, FALSE, FALSE, name);
}

void tick(bool in_world) {
  if (!event || !in_world || WaitForSingleObject(event, 0) != WAIT_OBJECT_0) {
    return;
  }
  wchar_t folder[MAX_PATH];
  GetTempPathW(MAX_PATH, folder);
  wchar_t file[MAX_PATH];
  swprintf_s(file, L"%skcds-terrain-%lu.txt", folder, GetCurrentProcessId());
  std::ifstream input(file);
  float x = 0.0f;
  float y = 0.0f;
  float half = 0.0f;
  float height = 0.0f;
  if (!(input >> x >> y >> half >> height)) {
    log("TERRAINPROBE request unreadable");
    return;
  }
  run(x, y, half, height);
}

}  // namespace kcds::runtime::testing::terrain_probe
