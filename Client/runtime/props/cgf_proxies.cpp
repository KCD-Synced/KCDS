#include "cgf_proxies.hpp"

#include <cmath>
#include <cstring>
#include <map>

namespace kcds::runtime::props::cgf {
namespace {

constexpr std::uint32_t kChunkVersion = 0x746;
constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kTableEntryBytes = 16;
constexpr std::uint16_t kMeshChunk = 0x1000;
constexpr std::uint16_t kNodeChunk = 0x100B;
constexpr std::uint16_t kPhysicsDataChunk = 0x1018;
constexpr std::size_t kNodeObject = 64;
constexpr std::size_t kNodeParent = 68;
constexpr std::size_t kNodeTransform = 84;
constexpr std::size_t kNodeBytes = kNodeTransform + 64;
constexpr std::size_t kMeshPhysicsData = 92;
constexpr std::size_t kPhysicsDataHead = 24;
// Deeper node chains than this are taken as broken (a loop of parents).
constexpr int kMostDepth = 16;

struct Chunk {
  std::uint16_t type = 0;
  std::uint32_t size = 0;
  std::uint32_t offset = 0;
};

template <class T>
T read(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  T value{};
  if (at + sizeof(T) <= bytes.size()) {
    std::memcpy(&value, bytes.data() + at, sizeof(T));
  }
  return value;
}

Transform local_of(const std::vector<std::uint8_t>& file, const Chunk& node) {
  Transform local;
  for (std::size_t row = 0; row < 3; ++row) {
    for (std::size_t column = 0; column < 3; ++column) {
      const std::size_t at = node.offset + kNodeTransform + (row * 4 + column) * 4;
      local.rows[row * 3 + column] = read<float>(file, at);
    }
    local.place[row] = read<float>(file, node.offset + kNodeTransform + (12 + row) * 4);
  }
  return local;
}

std::map<std::uint32_t, Chunk> chunks_of(const std::vector<std::uint8_t>& file) {
  std::map<std::uint32_t, Chunk> chunks;
  const std::uint32_t count = read<std::uint32_t>(file, 8);
  const std::uint32_t table = read<std::uint32_t>(file, 12);
  for (std::uint32_t index = 0; index < count; ++index) {
    const std::size_t at = std::size_t(table) + std::size_t(index) * kTableEntryBytes;
    if (at + kTableEntryBytes > file.size()) {
      break;
    }
    Chunk chunk;
    chunk.type = read<std::uint16_t>(file, at);
    chunk.size = read<std::uint32_t>(file, at + 8);
    chunk.offset = read<std::uint32_t>(file, at + 12);
    if (std::size_t(chunk.offset) + chunk.size <= file.size()) {
      chunks[read<std::uint32_t>(file, at + 4)] = chunk;
    }
  }
  return chunks;
}

}  // namespace

Transform combine(const Transform& inner, const Transform& outer) {
  Transform result;
  for (std::size_t row = 0; row < 3; ++row) {
    for (std::size_t column = 0; column < 3; ++column) {
      float sum = 0.0f;
      for (std::size_t k = 0; k < 3; ++k) {
        sum += inner.rows[row * 3 + k] * outer.rows[k * 3 + column];
      }
      result.rows[row * 3 + column] = sum;
    }
  }
  for (std::size_t column = 0; column < 3; ++column) {
    float sum = outer.place[column];
    for (std::size_t k = 0; k < 3; ++k) {
      sum += inner.place[k] * outer.rows[k * 3 + column];
    }
    result.place[column] = sum;
  }
  return result;
}

std::array<float, 4> quaternion_of(const std::array<float, 9>& rows) {
  // Column convention: m(r, c) is the row matrix's (c, r).
  const auto m = [&rows](int r, int c) { return rows[std::size_t(c * 3 + r)]; };
  const float trace = m(0, 0) + m(1, 1) + m(2, 2);
  std::array<float, 4> q{0.0f, 0.0f, 0.0f, 1.0f};
  if (trace > 0.0f) {
    const float s = std::sqrt(trace + 1.0f) * 2.0f;
    q = {(m(2, 1) - m(1, 2)) / s, (m(0, 2) - m(2, 0)) / s, (m(1, 0) - m(0, 1)) / s, 0.25f * s};
  } else if (m(0, 0) > m(1, 1) && m(0, 0) > m(2, 2)) {
    const float s = std::sqrt(1.0f + m(0, 0) - m(1, 1) - m(2, 2)) * 2.0f;
    q = {0.25f * s, (m(0, 1) + m(1, 0)) / s, (m(0, 2) + m(2, 0)) / s, (m(2, 1) - m(1, 2)) / s};
  } else if (m(1, 1) > m(2, 2)) {
    const float s = std::sqrt(1.0f + m(1, 1) - m(0, 0) - m(2, 2)) * 2.0f;
    q = {(m(0, 1) + m(1, 0)) / s, 0.25f * s, (m(1, 2) + m(2, 1)) / s, (m(0, 2) - m(2, 0)) / s};
  } else {
    const float s = std::sqrt(1.0f + m(2, 2) - m(0, 0) - m(1, 1)) * 2.0f;
    q = {(m(0, 2) + m(2, 0)) / s, (m(1, 2) + m(2, 1)) / s, 0.25f * s, (m(1, 0) - m(0, 1)) / s};
  }
  const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (!(length > 0.0f)) {
    return {0.0f, 0.0f, 0.0f, 1.0f};
  }
  for (float& part : q) {
    part /= length;
  }
  return q;
}

std::vector<Proxy> proxies_of(const std::vector<std::uint8_t>& file) {
  std::vector<Proxy> proxies;
  if (file.size() < kHeaderBytes || std::memcmp(file.data(), "CrCh", 4) != 0 ||
      read<std::uint32_t>(file, 4) != kChunkVersion) {
    return proxies;
  }
  const std::map<std::uint32_t, Chunk> chunks = chunks_of(file);
  for (const auto& [id, node] : chunks) {
    if (node.type != kNodeChunk || node.size < kNodeBytes) {
      continue;
    }
    const auto mesh = chunks.find(read<std::uint32_t>(file, node.offset + kNodeObject));
    if (mesh == chunks.end() || mesh->second.type != kMeshChunk ||
        mesh->second.size < kMeshPhysicsData + 4) {
      continue;
    }
    const auto physics =
        chunks.find(read<std::uint32_t>(file, mesh->second.offset + kMeshPhysicsData));
    if (physics == chunks.end() || physics->second.type != kPhysicsDataChunk ||
        physics->second.size <= kPhysicsDataHead) {
      continue;
    }
    const std::uint32_t size = read<std::uint32_t>(file, physics->second.offset);
    if (size == 0 || kPhysicsDataHead + size > physics->second.size) {
      continue;
    }
    Proxy proxy;
    proxy.offset = physics->second.offset + kPhysicsDataHead;
    proxy.size = size;
    proxy.where = local_of(file, node);
    std::int32_t parent = read<std::int32_t>(file, node.offset + kNodeParent);
    for (int depth = 0; parent >= 0 && depth < kMostDepth; ++depth) {
      const auto up = chunks.find(std::uint32_t(parent));
      if (up == chunks.end() || up->second.type != kNodeChunk || up->second.size < kNodeBytes) {
        break;
      }
      proxy.where = combine(proxy.where, local_of(file, up->second));
      parent = read<std::int32_t>(file, up->second.offset + kNodeParent);
    }
    proxies.push_back(proxy);
  }
  return proxies;
}

}  // namespace kcds::runtime::props::cgf
