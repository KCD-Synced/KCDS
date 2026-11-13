// KCD1's compiled terrain (terrain\terrain.dat, version 28) as KCD2 reads it. Plain code, no game
// calls.
//
// The file is a 32-byte header, then the model tables (vegetation groups of 360 bytes, brush
// models and brush materials of 256 bytes, each list after its count), the terrain node tree and
// last the object octree (COctreeNode: a 32-byte header, version, child mask, box and the size of
// its objects, the objects, then its children by the mask).
//
// KCD2 still reads version 28's header, tables and node tree as they are (CTerrain::Load_T's
// version 28 path), but it refuses octree nodes older than version 8 ("COctreeNode: too old
// version") and the whole terrain load then fails. KCD1 writes version 5, whose object records
// differ: the render node header has 32-bit render flags (KCD2: 64-bit), so every record grows,
// and some records changed further. The octree is therefore written again as version 8, record
// by record (convert_object); objects of a kind not translated yet are left out.
//
// The vis areas (terrain\indoor.dat) are refused the same way; KCD1's Rataje has one, and the
// translated level has none (empty_indoor).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace kcds::runtime::kcd1 {

// Where terrain.dat's parts start, found by walking it.
struct TerrainLayout {
  std::size_t tables = 32;  // the vegetation groups' count
  std::size_t nodes = 0;    // the root terrain node
  std::size_t octree = 0;   // the root octree node
  int terrain_size = 0;     // metres a side
  int node_count = 0;
};

// The layout of a version 28 terrain.dat, nullopt when the bytes are no such file.
std::optional<TerrainLayout> terrain_layout(const std::vector<std::uint8_t>& terrain);

// What happened to the objects while converting.
struct ObjectCounts {
  std::map<int, std::size_t> kept;     // by object type
  std::map<int, std::size_t> dropped;  // by object type
  std::size_t octree_nodes = 0;
  // KCD1's distant stand-ins for blocks of buildings (objects/uber/...), left out: KCD1 shows
  // them only from afar, KCD2 would draw them over the buildings themselves.
  std::size_t distant_stand_ins = 0;
  // Objects of the layers the map starts without (level_files.hpp layer_starts), left out.
  std::size_t layers_off = 0;
};

// The terrain.dat KCD2 loads: header, tables and node tree as they are, the octree as version 8,
// without the objects of the layers `layers_off` names (their ids, the 16 bits at + 24 of every
// object's record). nullopt when the file cannot be walked (its counts set then to what was read).
std::optional<std::vector<std::uint8_t>> convert_terrain(
    const std::vector<std::uint8_t>& terrain, ObjectCounts& counts,
    const std::vector<std::uint16_t>& layers_off = {});

// One KCD1 object record (its type first) at `at`: how many bytes it takes in the file (0 when it
// cannot be read) and, in `out`, the version 8 record (empty when that kind is left out).
std::size_t convert_object(const std::uint8_t* data, std::size_t size, std::size_t at,
                           std::vector<std::uint8_t>& out);

// terrain\indoor.dat without vis areas.
std::vector<std::uint8_t> empty_indoor();

}  // namespace kcds::runtime::kcd1
