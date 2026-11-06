// A studio mesh's vertices and triangles read from its .cgf (studio/cgf_mesh.hpp).
#include "cgf_mesh.hpp"

#include <cstdint>
#include <vector>

#include "kcd2s/testkit.hpp"

namespace {

namespace cgf = kcds::runtime::studio::cgf;

void put(std::vector<std::uint8_t>& out, std::uint32_t value, std::size_t size = 4) {
  for (std::size_t part = 0; part < size; ++part) {
    out.push_back(std::uint8_t(value >> (part * 8)));
  }
}

// Half-precision 0, 0.5, 1 and -1.
constexpr std::uint16_t kZero = 0x0000;
constexpr std::uint16_t kHalf = 0x3800;
constexpr std::uint16_t kOne = 0x3C00;
constexpr std::uint16_t kMinusOne = 0xBC00;

// A file with a positions stream (three vertices) and an indices stream (one triangle naming
// `last` as its third vertex), as kcds_cgf.py writes them, after another chunk.
std::vector<std::uint8_t> model_with(std::uint16_t last) {
  std::vector<std::uint8_t> positions;
  const std::uint16_t vertices[3][3] = {
      {kZero, kZero, kZero}, {kOne, kHalf, kZero}, {kMinusOne, kZero, kOne}};
  for (const auto& vertex : vertices) {
    for (const std::uint16_t axis : vertex) {
      put(positions, axis, 2);
    }
    put(positions, kOne, 2);
    put(positions, 0xFFFFFFFF);
    put(positions, 0);
  }
  std::vector<std::uint8_t> indices;
  put(indices, 0, 2);
  put(indices, 1, 2);
  put(indices, last, 2);
  indices.push_back(0);
  indices.push_back(0);

  const std::uint32_t first = 16 + 3 * 16;
  const std::uint32_t positions_size = 24 + std::uint32_t(positions.size());
  const std::uint32_t indices_size = 24 + std::uint32_t(indices.size());
  std::vector<std::uint8_t> out;
  out.insert(out.end(), {'C', 'r', 'C', 'h'});
  put(out, 0x746);
  put(out, 3);
  put(out, 16);
  // A Node chunk, then the two streams.
  put(out, 0x100B, 2);
  put(out, 0x824, 2);
  put(out, 1);
  put(out, 4);
  put(out, first);
  put(out, 0x1016, 2);
  put(out, 0x800, 2);
  put(out, 2);
  put(out, positions_size);
  put(out, first + 4);
  put(out, 0x1016, 2);
  put(out, 0x800, 2);
  put(out, 3);
  put(out, indices_size);
  put(out, first + 4 + positions_size);
  put(out, 0);
  for (const std::uint32_t word : {0u, 15u, 3u, 16u, 0u, 0u}) {
    put(out, word);
  }
  out.insert(out.end(), positions.begin(), positions.end());
  for (const std::uint32_t word : {0u, 5u, 3u, 2u, 0u, 0u}) {
    put(out, word);
  }
  out.insert(out.end(), indices.begin(), indices.end());
  return out;
}

}  // namespace

KCD2S_TEST(cgf_mesh, reads_the_vertices_and_triangles) {
  cgf::MeshShape shape;
  KCD2S_CHECK(cgf::read_shape(model_with(2), shape));
  KCD2S_CHECK_EQ(shape.positions.size(), std::size_t(9));
  KCD2S_CHECK_EQ(shape.positions[3], 1.0f);
  KCD2S_CHECK_EQ(shape.positions[4], 0.5f);
  KCD2S_CHECK_EQ(shape.positions[6], -1.0f);
  KCD2S_CHECK_EQ(shape.positions[8], 1.0f);
  KCD2S_CHECK_EQ(shape.indices.size(), std::size_t(3));
  KCD2S_CHECK_EQ(int(shape.indices[2]), 2);
}

KCD2S_TEST(cgf_mesh, refuses_a_triangle_past_the_vertices) {
  cgf::MeshShape shape;
  KCD2S_CHECK(!cgf::read_shape(model_with(3), shape));
}

KCD2S_TEST(cgf_mesh, refuses_what_is_not_such_a_model) {
  cgf::MeshShape shape;
  auto model = model_with(2);
  model[4] = 0x45;
  KCD2S_CHECK(!cgf::read_shape(model, shape));
  std::vector<std::uint8_t> other = {'n', 'o', 'p', 'e', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  KCD2S_CHECK(!cgf::read_shape(other, shape));
}
