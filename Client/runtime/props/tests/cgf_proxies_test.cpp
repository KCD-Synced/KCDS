#include "../cgf_proxies.hpp"

#include <cmath>
#include <cstring>
#include <string>

#include "kcd2s/testkit.hpp"

using kcds::runtime::props::cgf::combine;
using kcds::runtime::props::cgf::proxies_of;
using kcds::runtime::props::cgf::quaternion_of;
using kcds::runtime::props::cgf::Transform;

namespace {

// A .cgf written chunk by chunk: the header, the chunks' bytes, then the table.
struct CgfWriter {
  struct Entry {
    std::uint16_t type;
    std::uint32_t id;
    std::uint32_t size;
    std::uint32_t offset;
  };
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(16, 0);
  std::vector<Entry> entries;

  template <class T>
  void put_at(std::size_t at, const T& value) {
    std::memcpy(bytes.data() + at, &value, sizeof(T));
  }

  std::size_t chunk(std::uint16_t type, std::uint32_t id, std::size_t size) {
    const std::size_t offset = bytes.size();
    bytes.resize(offset + size, 0);
    entries.push_back({type, id, std::uint32_t(size), std::uint32_t(offset)});
    return offset;
  }

  // A node with its mesh, its parent and a place (no turn unless given).
  void node(std::uint32_t id, std::uint32_t mesh, std::int32_t parent,
            const std::array<float, 3>& place,
            const std::array<float, 9>& rows = {1, 0, 0, 0, 1, 0, 0, 0, 1}) {
    const std::size_t at = chunk(0x100B, id, 84 + 64);
    put_at(at + 64, mesh);
    put_at(at + 68, parent);
    for (std::size_t row = 0; row < 3; ++row) {
      for (std::size_t column = 0; column < 3; ++column) {
        put_at(at + 84 + (row * 4 + column) * 4, rows[row * 3 + column]);
      }
      put_at(at + 84 + (12 + row) * 4, place[row]);
    }
    put_at(at + 84 + 15 * 4, 1.0f);
  }

  void mesh(std::uint32_t id, std::uint32_t physics) {
    const std::size_t at = chunk(0x1000, id, 264);
    put_at(at + 92, physics);
  }

  void physics(std::uint32_t id, std::uint32_t data_size) {
    const std::size_t at = chunk(0x1018, id, 24 + data_size);
    put_at(at, data_size);
    put_at(at + 24, std::int32_t(1));
  }

  std::vector<std::uint8_t> finish() {
    std::memcpy(bytes.data(), "CrCh", 4);
    put_at(4, std::uint32_t(0x746));
    put_at(8, std::uint32_t(entries.size()));
    put_at(12, std::uint32_t(bytes.size()));
    for (const Entry& entry : entries) {
      const std::size_t at = bytes.size();
      bytes.resize(at + 16, 0);
      put_at(at, entry.type);
      put_at(at + 4, entry.id);
      put_at(at + 8, entry.size);
      put_at(at + 12, entry.offset);
    }
    return bytes;
  }
};

bool close_to(float a, float b) {
  return std::fabs(a - b) < 1e-4f;
}

}  // namespace

KCD2S_TEST(cgf_proxies, finds_the_proxies_under_a_merged_mesh) {
  // As a shed's file: a shadow mesh and a merged mesh without physics at the root, and the proxies
  // as children of the merged mesh.
  CgfWriter cgf;
  cgf.node(10, 11, -1, {0, 0, 0});
  cgf.mesh(11, 0);
  cgf.node(20, 21, -1, {1, 2, 0});
  cgf.mesh(21, 0);
  cgf.node(30, 31, 20, {0.5f, 0, 1});
  cgf.mesh(31, 32);
  cgf.physics(32, 140);
  cgf.node(40, 41, 20, {-0.5f, 0, 1});
  cgf.mesh(41, 42);
  cgf.physics(42, 176);
  const auto file = cgf.finish();
  const auto proxies = proxies_of(file);
  KCD2S_CHECK(proxies.size() == 2);
  KCD2S_CHECK(proxies[0].size == 140);
  KCD2S_CHECK(proxies[1].size == 176);
  // The data starts after the 24-byte head with its version 1.
  std::int32_t version = 0;
  std::memcpy(&version, file.data() + proxies[0].offset, sizeof(version));
  KCD2S_CHECK(version == 1);
  // Their places add up through the parent.
  KCD2S_CHECK(close_to(proxies[0].where.place[0], 1.5f));
  KCD2S_CHECK(close_to(proxies[0].where.place[1], 2.0f));
  KCD2S_CHECK(close_to(proxies[0].where.place[2], 1.0f));
  KCD2S_CHECK(close_to(proxies[1].where.place[0], 0.5f));
}

KCD2S_TEST(cgf_proxies, a_turned_parent_turns_the_child_place) {
  // The parent turned 90 degrees about z (rows: x -> y, y -> -x).
  CgfWriter cgf;
  cgf.node(1, 2, -1, {10, 0, 0}, {0, 1, 0, -1, 0, 0, 0, 0, 1});
  cgf.mesh(2, 0);
  cgf.node(3, 4, 1, {1, 0, 0});
  cgf.mesh(4, 5);
  cgf.physics(5, 64);
  const auto proxies = proxies_of(cgf.finish());
  KCD2S_CHECK(proxies.size() == 1);
  KCD2S_CHECK(close_to(proxies[0].where.place[0], 10.0f));
  KCD2S_CHECK(close_to(proxies[0].where.place[1], 1.0f));
  const auto q = quaternion_of(proxies[0].where.rows);
  // 90 degrees about z.
  KCD2S_CHECK(close_to(q[2], std::sqrt(0.5f)));
  KCD2S_CHECK(close_to(q[3], std::sqrt(0.5f)));
}

KCD2S_TEST(cgf_proxies, other_files_and_broken_chunks_give_none) {
  KCD2S_CHECK(proxies_of({}).empty());
  std::vector<std::uint8_t> other(64, 0);
  std::memcpy(other.data(), "CrCh", 4);
  KCD2S_CHECK(proxies_of(other).empty());
  // A physics chunk whose stated size runs past its chunk is left out.
  CgfWriter cgf;
  cgf.node(1, 2, -1, {0, 0, 0});
  cgf.mesh(2, 3);
  cgf.physics(3, 32);
  auto file = cgf.finish();
  const std::uint32_t too_big = 4096;
  std::memcpy(file.data() + cgf.entries.back().offset, &too_big, sizeof(too_big));
  KCD2S_CHECK(proxies_of(file).empty());
}

KCD2S_TEST(cgf_proxies, identity_has_no_turn) {
  const Transform none;
  const auto q = quaternion_of(none.rows);
  KCD2S_CHECK(close_to(q[0], 0) && close_to(q[1], 0) && close_to(q[2], 0) && close_to(q[3], 1));
  const Transform moved = combine(none, none);
  KCD2S_CHECK(close_to(moved.rows[0], 1) && close_to(moved.place[0], 0));
}
