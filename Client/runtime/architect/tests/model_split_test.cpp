#include "../model_split.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "kcd2s/testkit.hpp"

namespace model_split = kcds::runtime::architect::model_split;

namespace {

std::vector<std::uint8_t> filled(std::size_t size, std::uint8_t value) {
  return std::vector<std::uint8_t>(size, value);
}

void put32(std::vector<std::uint8_t>& body, std::size_t at, std::int32_t value) {
  std::memcpy(body.data() + at, &value, 4);
}

std::int32_t get32(const std::vector<std::uint8_t>& body, std::size_t at) {
  std::int32_t value = 0;
  std::memcpy(&value, body.data() + at, 4);
  return value;
}

// A .cgfm as the game's: an export chunk, a material, and two nodes, each with a mesh whose
// subsets and one stream are chunks of their own and whose physics names a chunk.
std::vector<std::uint8_t> streamed() {
  std::vector<model_split::Chunk> chunks;
  chunks.push_back({model_split::kChunkExport, 1, 1, filled(20, 7)});
  std::vector<std::uint8_t> material(model_split::kMaterialName + 8, 0);
  std::memcpy(material.data(), "objects/manmade/house", 21);
  chunks.push_back({model_split::kChunkMaterial, 0x802, 2, material});
  for (std::uint32_t n = 0; n < 2; ++n) {
    const std::uint32_t base = 10 + n * 10;
    chunks.push_back({0x1017, 0x800, base, filled(16, std::uint8_t(n + 1))});
    chunks.push_back({0x1016, 0x800, base + 1, filled(24, std::uint8_t(n + 3))});
    std::vector<std::uint8_t> mesh(264, 0);
    put32(mesh, model_split::kMeshSubsets, std::int32_t(base));
    put32(mesh, model_split::kMeshStreams + 15 * 4, std::int32_t(base + 1));
    put32(mesh, model_split::kMeshPhysics, 99);
    chunks.push_back({model_split::kChunkMesh, 0x801, base + 2, mesh});
    std::vector<std::uint8_t> node(200, 0);
    put32(node, model_split::kNodeObject, std::int32_t(base + 2));
    put32(node, model_split::kNodeParent, 3);
    put32(node, model_split::kNodeChildren, 4);
    put32(node, model_split::kNodeMaterial, 2);
    chunks.push_back({model_split::kChunkNode, 0x824, base + 3, node});
  }
  return model_split::chunk_file(chunks);
}

}  // namespace

KCD2S_TEST(model_split, each_drawn_node_becomes_a_model_of_its_own) {
  std::vector<std::vector<std::uint8_t>> pieces;
  KCD2S_CHECK(model_split::split(streamed(), "objects/kcds/opened/house1", pieces));
  KCD2S_CHECK_EQ(pieces.size(), std::size_t(2));
  std::vector<model_split::Chunk> chunks;
  KCD2S_CHECK(model_split::chunks_of(pieces[1], chunks));
  // The export flags, the material, the subsets, the stream, the mesh and the node.
  KCD2S_CHECK_EQ(chunks.size(), std::size_t(6));
  KCD2S_CHECK_EQ(chunks[0].kind, model_split::kChunkExport);
  const std::string material(reinterpret_cast<const char*>(chunks[1].body.data()));
  KCD2S_CHECK_EQ(material, std::string("objects/kcds/opened/house1"));
  KCD2S_CHECK_EQ(chunks[2].body[0], std::uint8_t(2));
  KCD2S_CHECK_EQ(chunks[3].body[0], std::uint8_t(4));
  KCD2S_CHECK_EQ(get32(chunks[4].body, model_split::kMeshPhysics), 0);
  KCD2S_CHECK_EQ(get32(chunks[5].body, model_split::kNodeParent), -1);
  KCD2S_CHECK_EQ(get32(chunks[5].body, model_split::kNodeChildren), 0);
  // The chunks keep their ids, so the node still names its mesh.
  KCD2S_CHECK_EQ(get32(chunks[5].body, model_split::kNodeObject), std::int32_t(chunks[4].id));
}

KCD2S_TEST(model_split, other_bytes_and_long_material_names_are_refused) {
  std::vector<std::vector<std::uint8_t>> pieces;
  KCD2S_CHECK(!model_split::split({'n', 'o'}, "m", pieces));
  KCD2S_CHECK(!model_split::split(streamed(), std::string(200, 'a'), pieces));
  auto broken = streamed();
  put32(broken, 16 + 12, 1 << 30);  // the first chunk's body far outside the file
  KCD2S_CHECK(!model_split::split(broken, "m", pieces));
}
