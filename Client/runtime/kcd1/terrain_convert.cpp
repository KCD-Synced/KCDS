#include "terrain_convert.hpp"

#include <cctype>
#include <cstring>
#include <string>

namespace kcds::runtime::kcd1 {
namespace {

constexpr std::size_t kHeaderSize = 32;
constexpr std::uint8_t kTerrainVersion = 28;
constexpr std::size_t kVegetationGroupSize = 360;
constexpr std::size_t kNameSize = 256;
// A terrain node: its 44-byte header (version 7 in version 28 files), its height words, its
// levels of detail's errors and its surface palette.
constexpr std::size_t kNodeHeaderSize = 44;
constexpr std::int16_t kNodeVersion = 7;
constexpr int kSectorSize = 64;
constexpr int kDetailLevels = 6;
// An octree node's header: version, child mask, box, the size of its objects.
constexpr std::size_t kOctreeHeaderSize = 32;
constexpr std::int16_t kKcd1OctreeVersion = 5;
constexpr std::int16_t kKcd2OctreeVersion = 8;
constexpr int kMostDepth = 32;

template <class T>
bool read(const std::vector<std::uint8_t>& data, std::size_t at, T& value) {
  if (at + sizeof(T) > data.size()) {
    return false;
  }
  std::memcpy(&value, data.data() + at, sizeof(T));
  return true;
}

template <class T>
void write(std::vector<std::uint8_t>& out, std::size_t at, T value) {
  std::memcpy(out.data() + at, &value, sizeof(T));
}

std::size_t aligned(std::size_t at) {
  return (at + 3) & ~std::size_t(3);
}

// A terrain node and its children from `at`; where they end, 0 when they cannot be read.
std::size_t skip_node(const std::vector<std::uint8_t>& terrain, std::size_t at, int size,
                      int& count) {
  std::int16_t version = 0;
  std::int16_t side = 0;
  std::int32_t palette = 0;
  if (!read(terrain, at, version) || !read(terrain, at + 36, side) ||
      !read(terrain, at + 40, palette) || version != kNodeVersion || side < 0 || palette < 0) {
    return 0;
  }
  at += kNodeHeaderSize + std::size_t(side) * std::size_t(side) * 2;
  at = aligned(at);
  at += kDetailLevels * 4;
  at = aligned(at + std::size_t(palette));
  count += 1;
  if (size > kSectorSize) {
    for (int child = 0; child < 4 && at != 0; ++child) {
      at = skip_node(terrain, at, size / 2, count);
    }
  }
  return at > terrain.size() ? 0 : at;
}

// KCD1's distant stand-ins for whole blocks of buildings (its "uber" levels of detail: the map's
// grid in objects/uber/ and each town's in objects/uberlods/<town>/): KCD1 shows them only from
// afar, KCD2 would draw them over the buildings.
bool distant_stand_in(const std::string& model) {
  std::string path;
  for (const char c : model) {
    path += c == '\\' ? '/' : char(std::tolower(unsigned(c)));
  }
  return path.rfind("objects/uber/", 0) == 0 || path.rfind("objects/uberlods/", 0) == 0;
}

// The brush models (by their index) whose brushes are left out.
std::vector<bool> left_out_models(const std::vector<std::uint8_t>& terrain,
                                  const TerrainLayout& layout) {
  std::vector<bool> left_out;
  std::int32_t plants = 0;
  std::int32_t models = 0;
  if (!read(terrain, layout.tables, plants) || plants < 0) {
    return left_out;
  }
  const std::size_t names = layout.tables + 4 + std::size_t(plants) * kVegetationGroupSize;
  if (!read(terrain, names, models) || models < 0 ||
      names + 4 + std::size_t(models) * kNameSize > terrain.size()) {
    return left_out;
  }
  left_out.resize(std::size_t(models), false);
  for (std::int32_t index = 0; index < models; ++index) {
    const char* name =
        reinterpret_cast<const char*>(terrain.data() + names + 4 + std::size_t(index) * kNameSize);
    left_out[std::size_t(index)] = distant_stand_in(std::string(name, strnlen(name, kNameSize)));
  }
  return left_out;
}

// An octree node and its children from `at`, written as version 8 to `out`; where they end in
// `terrain`, 0 when they cannot be read. Brushes of `left_out` models are left out.
std::size_t convert_octree(const std::vector<std::uint8_t>& terrain, std::size_t at,
                           std::vector<std::uint8_t>& out, ObjectCounts& counts,
                           const std::vector<bool>& left_out, int depth) {
  std::int16_t version = 0;
  std::uint16_t mask = 0;
  std::int32_t size = 0;
  if (depth > kMostDepth || !read(terrain, at, version) || !read(terrain, at + 2, mask) ||
      !read(terrain, at + 28, size) || version != kKcd1OctreeVersion || size < 0 ||
      at + kOctreeHeaderSize + std::size_t(size) > terrain.size()) {
    return 0;
  }
  counts.octree_nodes += 1;
  const std::size_t header = out.size();
  out.insert(out.end(), terrain.begin() + std::ptrdiff_t(at),
             terrain.begin() + std::ptrdiff_t(at + kOctreeHeaderSize));
  write(out, header, kKcd2OctreeVersion);
  const std::size_t objects_start = out.size();
  std::size_t object = at + kOctreeHeaderSize;
  const std::size_t objects_end = object + std::size_t(size);
  std::vector<std::uint8_t> record;
  while (object < objects_end) {
    std::int32_t type = 0;
    read(terrain, object, type);
    record.clear();
    const std::size_t used = convert_object(terrain.data(), objects_end, object, record);
    if (used == 0) {
      // A kind whose length is not known: the node's other objects are left out.
      counts.dropped[type] += 1;
      break;
    }
    std::uint16_t model = 0;
    const bool stand_in = type == 1 && read(terrain, object + 4 + 32, model) &&
                          model < left_out.size() && left_out[model];
    if (stand_in) {
      counts.distant_stand_ins += 1;
    } else if (record.empty()) {
      counts.dropped[type] += 1;
    } else {
      counts.kept[type] += 1;
      out.insert(out.end(), record.begin(), record.end());
    }
    object += used;
  }
  write(out, header + 28, std::int32_t(out.size() - objects_start));
  at = objects_end;
  for (int child = 0; child < 8; ++child) {
    if ((mask & (1u << child)) != 0) {
      at = convert_octree(terrain, at, out, counts, left_out, depth + 1);
      if (at == 0) {
        return 0;
      }
    }
  }
  return at;
}

// KCD1's object kinds (EERType).
constexpr std::int32_t kBrush = 1;
constexpr std::int32_t kVegetation = 2;
constexpr std::int32_t kDecal = 7;
constexpr std::int32_t kWaterVolume = 9;
constexpr std::int32_t kAreaShape = 10;
constexpr std::int32_t kRoad = 11;
constexpr std::int32_t kKind12 = 12;
constexpr std::int32_t kKind18 = 18;
constexpr std::int32_t kMergedMesh = 23;
// Record sizes after the type (KCD1's COctreeNode::LoadSingleObject, version 5).
constexpr std::size_t kKcd1BrushSize = 96;
constexpr std::size_t kKcd2BrushSize = 100;
constexpr std::size_t kVegetationSize = 60;

std::uint32_t u32_at(const std::uint8_t* data, std::size_t at) {
  std::uint32_t value = 0;
  std::memcpy(&value, data + at, sizeof(value));
  return value;
}

// The render node header shared by every record: KCD1's is 36 bytes (32-bit render flags at
// + 28, the kind's own 16-bit index at + 32, view distance and level of detail ratios at + 34),
// KCD2's 40 (64-bit flags at + 28, the index at + 36, the ratios at + 38).
void convert_header(const std::uint8_t* record, std::uint8_t* out) {
  // Box (24 bytes), layer, shadow bias and its pad byte.
  std::memcpy(out, record, 28);
  const std::uint64_t flags = u32_at(record, 28);
  std::memcpy(out + 28, &flags, sizeof(flags));
  std::memcpy(out + 36, record + 32, 2);
  out[38] = record[34];
  out[39] = record[35];
}

// How many bytes a KCD1 record of `type` takes after its type (counts read from the record),
// 0 when it is not known or does not fit.
std::size_t kcd1_record_size(std::int32_t type, const std::uint8_t* record, std::size_t left) {
  const auto count = [record, left](std::size_t at) -> std::size_t {
    return at + 4 <= left ? u32_at(record, at) : 0;
  };
  std::size_t size = 0;
  switch (type) {
    case kBrush:
      size = kKcd1BrushSize;
      break;
    case kVegetation:
      size = kVegetationSize;
      break;
    case kDecal:
      size = 112;
      break;
    case kKind12:
      size = 64;
      break;
    case kKind18:
      size = 92;
      break;
    case kMergedMesh:
      size = 64 + 8 * count(36);
      break;
    case kAreaShape:
      size = 148 + 12 * count(88);
      break;
    case kWaterVolume: {
      const std::size_t extra = left > 39 ? std::size_t(std::int8_t(record[39])) : 0;
      size = 140 + 4 * extra + 12 * count(120) + 12 * count(132);
      break;
    }
    case kRoad:
      size = 104 + 12 * count(76) + 2 * count(80) + 8 * count(84) + 40 * count(88) + 12 * count(92);
      size = aligned(size);
      break;
    default:
      return 0;
  }
  return size <= left ? size : 0;
}

}  // namespace

std::size_t convert_object(const std::uint8_t* data, std::size_t size, std::size_t at,
                           std::vector<std::uint8_t>& out) {
  out.clear();
  if (at + 4 > size) {
    return 0;
  }
  const std::int32_t type = std::int32_t(u32_at(data, at));
  const std::uint8_t* record = data + at + 4;
  const std::size_t record_size = kcd1_record_size(type, record, size - at - 4);
  if (record_size == 0) {
    return 0;
  }
  if (type == kBrush) {
    // After the header: the 3x4 matrix, collision class, flags, the brush material's index and
    // one byte more, each 4 bytes later in KCD2; the brush model's index is the header's.
    out.assign(4 + kKcd2BrushSize, 0);
    std::memcpy(out.data(), &type, 4);
    std::uint8_t* to = out.data() + 4;
    convert_header(record, to);
    std::memcpy(to + 40, record + 36, 48);
    std::memcpy(to + 88, record + 84, 4);
    std::memcpy(to + 92, record + 88, 4);
    std::memcpy(to + 96, record + 92, 4);
  } else if (type == kVegetation) {
    // After the header: position, scale and the brightness and three angles; KCD1's last four
    // bytes are left.
    out.assign(4 + kVegetationSize, 0);
    std::memcpy(out.data(), &type, 4);
    std::uint8_t* to = out.data() + 4;
    convert_header(record, to);
    std::memcpy(to + 40, record + 36, 20);
  }
  // Other kinds are not translated yet: `out` stays empty and the record is left out.
  return 4 + record_size;
}

std::optional<TerrainLayout> terrain_layout(const std::vector<std::uint8_t>& terrain) {
  TerrainLayout layout;
  std::int32_t size = 0;
  if (terrain.size() < kHeaderSize || terrain[0] != kTerrainVersion || !read(terrain, 8, size) ||
      size <= 0) {
    return std::nullopt;
  }
  layout.terrain_size = size;
  std::size_t at = layout.tables;
  std::int32_t count = 0;
  if (!read(terrain, at, count) || count < 0) {
    return std::nullopt;
  }
  at += 4 + std::size_t(count) * kVegetationGroupSize;
  for (int list = 0; list < 2; ++list) {
    if (!read(terrain, at, count) || count < 0) {
      return std::nullopt;
    }
    at += 4 + std::size_t(count) * kNameSize;
  }
  layout.nodes = at;
  layout.octree = skip_node(terrain, at, size, layout.node_count);
  if (layout.octree == 0) {
    return std::nullopt;
  }
  return layout;
}

std::optional<std::vector<std::uint8_t>> convert_terrain(const std::vector<std::uint8_t>& terrain,
                                                         ObjectCounts& counts) {
  const std::optional<TerrainLayout> layout = terrain_layout(terrain);
  if (!layout) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> out(terrain.begin(), terrain.begin() + std::ptrdiff_t(layout->octree));
  out.reserve(terrain.size() + terrain.size() / 8);
  const std::vector<bool> left_out = left_out_models(terrain, *layout);
  const std::size_t end = convert_octree(terrain, layout->octree, out, counts, left_out, 0);
  if (end != terrain.size()) {
    return std::nullopt;
  }
  // The header's chunk size: the whole file.
  write(out, 4, std::int32_t(out.size()));
  return out;
}

std::vector<std::uint8_t> empty_indoor() {
  // Version 6, a 20-byte header and no vis areas (as World Architect's custom maps write it).
  const std::int32_t words[5] = {6, 20, 0, 0, 0};
  std::vector<std::uint8_t> out(sizeof(words));
  std::memcpy(out.data(), words, sizeof(words));
  return out;
}

}  // namespace kcds::runtime::kcd1
