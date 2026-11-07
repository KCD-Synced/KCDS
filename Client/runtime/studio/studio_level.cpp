#include "studio_level.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <string>
#include <utility>

#include "../files/stored_zip.hpp"

namespace kcds::runtime::studio::level {
namespace {

using Bytes = std::vector<std::uint8_t>;

// The studio's own ground: a flat square of kTerrainSize metres at kFloorHeight, in sectors of
// kSectorSize metres (one root node and four leaves).
constexpr int kTerrainSize = 128;
constexpr int kSectorSize = 64;
constexpr float kFloorHeight = 32.0f;
constexpr float kBoxMargin = 0.5f;
// The floor's look, footsteps and physics: one of the level's surface types (leveldata.xml
// SurfaceTypes), 37 Materials/terrain/cobblestone/cobble_stones_c_4x4.
constexpr std::uint8_t kFloorSurface = 37;
// The terrain's levels of detail: the sector halved down to one unit (1 m), 64 -> 1 is six.
constexpr int kLodLevels = 6;
// The ground texture's sectors: 256 x 256 pixels in two BC1 layers (colour, then normal), as
// klaster's cover.ctc; the floor's colour and its normal straight up, in RGB.
constexpr int kTextureSectorSize = 256;
constexpr std::uint8_t kTextureLayerFormat = 0x16;
constexpr int kFloorColour[3] = {72, 72, 72};
constexpr int kFloorNormal[3] = {128, 255, 128};

void put8(Bytes& out, std::uint8_t value) {
  out.push_back(value);
}

void put16(Bytes& out, std::uint16_t value) {
  out.push_back(std::uint8_t(value));
  out.push_back(std::uint8_t(value >> 8));
}

void put32(Bytes& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(std::uint8_t(value >> shift));
  }
}

void put_float(Bytes& out, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  put32(out, bits);
}

void put_zeros(Bytes& out, std::size_t count) {
  out.insert(out.end(), count, 0);
}

void put_text(Bytes& out, const std::string& text) {
  out.insert(out.end(), text.begin(), text.end());
}

// One terrain node (CTerrainNode load) and its children, flat at kFloorHeight. The 44-byte chunk:
// version 8, holes, the heightmap box, the base height and the height step, the heightmap side
// and the surface palette length. A leaf (one sector) carries its (side x side) heightmap words,
// all 0: flat ground of the palette's first surface. Then the levels of detail's errors and the
// palette, padded to 4 bytes.
void terrain_node(Bytes& out, int x, int y, int size) {
  const bool leaf = size == kSectorSize;
  const int side = leaf ? kSectorSize + 1 : 0;
  put16(out, 8);
  put8(out, 0);
  put8(out, 0);
  // A box with no height at all was culled now and then (the floor not drawn in some starts).
  put_float(out, float(x));
  put_float(out, float(y));
  put_float(out, kFloorHeight - kBoxMargin);
  put_float(out, float(x + size));
  put_float(out, float(y + size));
  put_float(out, kFloorHeight + kBoxMargin);
  put_float(out, leaf ? kFloorHeight : 0.0f);
  put_float(out, leaf ? 0.05f : 0.0f);
  put16(out, std::uint16_t(side));
  put16(out, 0);
  put32(out, 1);
  put_zeros(out, std::size_t(4) * side * side);
  put_zeros(out, std::size_t(4) * kLodLevels);
  put8(out, kFloorSurface);
  put_zeros(out, 3);
  if (!leaf) {
    const int half = size / 2;
    terrain_node(out, x, y, half);
    terrain_node(out, x + half, y, half);
    terrain_node(out, x, y + half, half);
    terrain_node(out, x + half, y + half, half);
  }
}

// A BC1 picture of one colour: every 4 x 4 block both end colours the same, indices 0.
void bc1_solid(Bytes& out, const int colour[3], int size) {
  const std::uint16_t packed = std::uint16_t(
      ((colour[0] * 31 / 255) << 11) | ((colour[1] * 63 / 255) << 5) | (colour[2] * 31 / 255));
  const int blocks = (size / 4) * (size / 4);
  for (int block = 0; block < blocks; ++block) {
    put16(out, packed);
    put16(out, packed);
    put32(out, 0);
  }
}

std::string lower_slashed(const std::string& name) {
  std::string out = name;
  for (char& c : out) {
    if (c == '\\') {
      c = '/';
    } else {
      c = char(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  return out;
}

std::string as_text(const Bytes& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

Bytes as_bytes(const std::string& text) {
  return Bytes(text.begin(), text.end());
}

void replace_all(std::string& text, const std::string& from, const std::string& to) {
  std::size_t at = 0;
  while ((at = text.find(from, at)) != std::string::npos) {
    text.replace(at, from.size(), to);
    at += to.size();
  }
}

// Every <tag>...</tag> (the first end after each start) made an empty <tag />.
void empty_elements(std::string& text, const std::string& tag) {
  const std::string open = "<" + tag + ">";
  const std::string close = "</" + tag + ">";
  const std::string empty = "<" + tag + " />";
  std::size_t at = 0;
  while ((at = text.find(open, at)) != std::string::npos) {
    const std::size_t end = text.find(close, at + open.size());
    if (end == std::string::npos) {
      return;
    }
    text.replace(at, end + close.size() - at, empty);
    at += empty.size();
  }
}

// Every CGFCount="<digits>" made CGFCount="0" (the level loads no models).
void no_models(std::string& text) {
  const std::string key = "CGFCount=\"";
  std::size_t at = 0;
  while ((at = text.find(key, at)) != std::string::npos) {
    const std::size_t digits = at + key.size();
    std::size_t end = digits;
    while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) {
      end += 1;
    }
    if (end > digits && end < text.size() && text[end] == '"') {
      text.replace(digits, end - digits, "0");
    }
    at = digits;
  }
}

}  // namespace

std::vector<std::uint8_t> terrain() {
  // CTerrain::Load_T: chunk version 29, no vegetation, brushes or brush materials, the flat node
  // tree and an empty object octree (one 32-byte node, version 8, no children, no instances).
  Bytes body;
  put32(body, 0);
  put32(body, 0);
  put32(body, 0);
  terrain_node(body, 0, 0, kTerrainSize);
  put16(body, 8);
  put16(body, 0);
  put_float(body, 0.0f);
  put_float(body, 0.0f);
  put_float(body, 0.0f);
  put_float(body, float(kTerrainSize));
  put_float(body, float(kTerrainSize));
  put_float(body, float(kTerrainSize));
  put32(body, 0);
  Bytes out;
  put8(out, 29);
  put8(out, 0);
  put8(out, 6);
  put8(out, 1);
  put32(out, std::uint32_t(32 + body.size()));
  put32(out, kTerrainSize);
  put_float(out, 1.0f);
  put32(out, kSectorSize);
  put32(out, kTerrainSize / kSectorSize);
  put_float(out, 0.003814697265625f);
  put_float(out, 0.0f);
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

std::vector<std::uint8_t> ground_texture() {
  // cover.ctc: "CRY", version 100, file version 9; two layers; the texture index of every terrain
  // node in the node order, a negative entry skipping a child, so each leaf is followed by four
  // -1; then each index's sector, the layers one after the other.
  Bytes out;
  put_text(out, std::string("CRY", 4));
  put8(out, 100);
  put8(out, 0);
  put16(out, 9);
  put16(out, 2);
  put16(out, 0);
  put_float(out, 1.0f);
  for (int layer = 0; layer < 2; ++layer) {
    put16(out, kTextureSectorSize);
    put16(out, 0);
    put8(out, kTextureLayerFormat);
    put_zeros(out, 3);
    put32(out, kTextureSectorSize * kTextureSectorSize / 2);
  }
  const int leaves = (kTerrainSize / kSectorSize) * (kTerrainSize / kSectorSize);
  std::vector<std::int16_t> indices = {0};
  for (int leaf = 0; leaf < leaves; ++leaf) {
    indices.push_back(std::int16_t(1 + leaf));
    for (int skipped = 0; skipped < 4; ++skipped) {
      indices.push_back(-1);
    }
  }
  put16(out, std::uint16_t(indices.size()));
  for (const std::int16_t index : indices) {
    put16(out, std::uint16_t(index));
  }
  Bytes sector;
  bc1_solid(sector, kFloorColour, kTextureSectorSize);
  bc1_solid(sector, kFloorNormal, kTextureSectorSize);
  for (int index = 0; index < 1 + leaves; ++index) {
    out.insert(out.end(), sector.begin(), sector.end());
  }
  return out;
}

std::optional<std::vector<std::uint8_t>> build(const files::ZipReadAt& read, std::uint64_t size) {
  const auto entries = files::zip_entries(read, size);
  if (!entries) {
    return std::nullopt;
  }
  std::map<std::string, const files::ZipEntry*> by_name;
  for (const files::ZipEntry& entry : *entries) {
    by_name[lower_slashed(entry.name)] = &entry;
  }
  const auto bytes_of = [&](const std::string& name) -> std::optional<Bytes> {
    const auto found = by_name.find(name);
    if (found == by_name.end()) {
      return std::nullopt;
    }
    return files::zip_entry_bytes(read, *found->second);
  };

  // The level's files, in the order the studio's tool writes them.
  std::vector<std::pair<std::string, Bytes>> pak;
  const auto info = bytes_of("levelinfo.xml");
  const auto data = bytes_of("leveldata.xml");
  const auto action = bytes_of("leveldataaction.xml");
  const auto mission = bytes_of("mission_mission0.xml");
  if (!info || !data || !action || !mission) {
    return std::nullopt;
  }
  std::string text = as_text(*info);
  replace_all(text, "Name=\"data/levels/klaster\"", "Name=\"data/levels/kcds_studio\"");
  pak.emplace_back("levelinfo.xml", as_bytes(text));

  text = as_text(*data);
  replace_all(text, "Name=\"klaster\"", "Name=\"kcds_studio\"");
  empty_elements(text, "Layers");
  empty_elements(text, "Vegetation");
  pak.emplace_back("leveldata.xml", as_bytes(text));

  text = as_text(*action);
  no_models(text);
  replace_all(text, "ProgressBarRange=\"3789\"", "ProgressBarRange=\"1\"");
  pak.emplace_back("leveldataaction.xml", as_bytes(text));

  text = as_text(*mission);
  no_models(text);
  pak.emplace_back("mission_mission0.xml", as_bytes(text));
  pak.emplace_back("objects_mission0.xml", as_bytes("<Objects>\n</Objects>\n"));

  for (const char* name : {"levelprototypes.xml", "serialize.xml", "lensflarelist.xml",
                           "dyntexsrclayeract.xml", "shaderslist.txt"}) {
    auto copied = bytes_of(name);
    if (!copied) {
      return std::nullopt;
    }
    pak.emplace_back(name, std::move(*copied));
  }
  // Step 7 needs a brush list; an empty one is fine.
  pak.emplace_back("brushlist.txt", Bytes());

  for (const files::ZipEntry& entry : *entries) {
    std::string name = entry.name;
    std::replace(name.begin(), name.end(), '\\', '/');
    if (lower_slashed(name).rfind("tables/", 0) != 0) {
      continue;
    }
    auto copied = files::zip_entry_bytes(read, entry);
    if (!copied) {
      return std::nullopt;
    }
    pak.emplace_back(name, std::move(*copied));
  }

  // The studio's own ground texture: without one the terrain is not drawn at all.
  pak.emplace_back("terrain/cover.ctc", ground_texture());
  pak.emplace_back("terrain/terrain.dat", terrain());
  // No vis areas (version 6, the 20-byte chunk, no areas or portals).
  Bytes indoor;
  put32(indoor, 6);
  put32(indoor, 20);
  put_zeros(indoor, 12);
  pak.emplace_back("terrain/indoor.dat", std::move(indoor));
  // Distant levels of detail (step 9): only the root cluster, no layers. hlods.dat is version 2
  // (checked on open) and the root's 4 bytes of data, read by offset when streamed.
  pak.emplace_back(
      "terrain/hlods.xml",
      as_bytes("<HLod DataOffset=\"4\" DataSize=\"4\" ProxyIndex=\"-1\" Type=\"Cluster\" "
               "Radius=\"8000\" NearestObserverDistance=\"0\" Name=\"root\" Hash=\"214082742\" "
               "/>\n"));
  Bytes hlods;
  put32(hlods, 2);
  put32(hlods, 0);
  pak.emplace_back("terrain/hlods.dat", std::move(hlods));
  return files::stored_zip_entries(pak);
}

}  // namespace kcds::runtime::studio::level
