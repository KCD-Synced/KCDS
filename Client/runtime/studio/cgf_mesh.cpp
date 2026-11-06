#include "cgf_mesh.hpp"

#include <cmath>
#include <cstring>

namespace kcds::runtime::studio::cgf {
namespace {

constexpr std::uint32_t kVersion = 0x746;
constexpr std::uint16_t kDataStream = 0x1016;
constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kEntrySize = 16;
constexpr std::size_t kStreamHeadSize = 24;
constexpr std::int32_t kPositionsStream = 15;
constexpr std::int32_t kPositionSize = 16;
constexpr std::int32_t kIndicesStream = 5;
constexpr std::int32_t kIndexSize = 2;
constexpr std::uint32_t kMostChunks = 4096;

std::uint32_t word_at(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  std::memcpy(&value, &bytes[offset], sizeof(value));
  return value;
}

std::uint16_t half_at(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  std::uint16_t value = 0;
  std::memcpy(&value, &bytes[offset], sizeof(value));
  return value;
}

// An IEEE half-precision number as a float.
float float_of_half(std::uint16_t half) {
  const bool negative = (half & 0x8000) != 0;
  const int exponent = (half >> 10) & 0x1F;
  const int fraction = half & 0x3FF;
  float value = 0.0f;
  if (exponent == 0) {
    value = std::ldexp(float(fraction), -24);
  } else if (exponent == 31) {
    value = fraction ? NAN : INFINITY;
  } else {
    value = std::ldexp(float(fraction | 0x400), exponent - 25);
  }
  return negative ? -value : value;
}

// One DataStream chunk's elements: where they start and how many there are.
struct Stream {
  std::size_t data = 0;
  std::uint32_t count = 0;
  bool found = false;
};

}  // namespace

bool read_shape(const std::vector<std::uint8_t>& model, MeshShape& shape) {
  if (model.size() < kHeaderSize || std::memcmp(model.data(), "CrCh", 4) != 0 ||
      word_at(model, 4) != kVersion) {
    return false;
  }
  const std::uint32_t count = word_at(model, 8);
  const std::uint32_t table = word_at(model, 12);
  if (count > kMostChunks || std::size_t(table) + std::size_t(count) * kEntrySize > model.size()) {
    return false;
  }
  Stream positions;
  Stream indices;
  for (std::uint32_t index = 0; index < count; ++index) {
    const std::size_t entry = table + std::size_t(index) * kEntrySize;
    if (half_at(model, entry) != kDataStream) {
      continue;
    }
    const std::uint32_t size = word_at(model, entry + 8);
    const std::uint32_t offset = word_at(model, entry + 12);
    if (size < kStreamHeadSize || std::size_t(offset) + size > model.size()) {
      return false;
    }
    const auto type = std::int32_t(word_at(model, offset + 4));
    const std::uint32_t elements = word_at(model, offset + 8);
    const auto element_size = std::int32_t(word_at(model, offset + 12));
    const std::size_t needed = kStreamHeadSize + std::size_t(elements) * std::size_t(element_size);
    Stream* stream = nullptr;
    if (type == kPositionsStream && element_size == kPositionSize) {
      stream = &positions;
    } else if (type == kIndicesStream && element_size == kIndexSize) {
      stream = &indices;
    }
    if (!stream) {
      continue;
    }
    if (needed > size) {
      return false;
    }
    stream->data = offset + kStreamHeadSize;
    stream->count = elements;
    stream->found = true;
  }
  if (!positions.found || !indices.found || positions.count == 0 || indices.count < 3 ||
      indices.count % 3 != 0) {
    return false;
  }
  shape.positions.clear();
  shape.indices.clear();
  shape.positions.reserve(std::size_t(positions.count) * 3);
  for (std::uint32_t vertex = 0; vertex < positions.count; ++vertex) {
    const std::size_t at = positions.data + std::size_t(vertex) * kPositionSize;
    for (std::size_t axis = 0; axis < 3; ++axis) {
      shape.positions.push_back(float_of_half(half_at(model, at + axis * 2)));
    }
  }
  shape.indices.reserve(indices.count);
  for (std::uint32_t corner = 0; corner < indices.count; ++corner) {
    const std::uint16_t vertex = half_at(model, indices.data + std::size_t(corner) * kIndexSize);
    if (vertex >= positions.count) {
      return false;
    }
    shape.indices.push_back(vertex);
  }
  return true;
}

}  // namespace kcds::runtime::studio::cgf
