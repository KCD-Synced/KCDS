#include "skin_file.hpp"

#include <cstring>

namespace kcds::runtime::appearance::refit {
namespace {

constexpr std::uint32_t kVersion = 0x746;
constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kEntrySize = 16;
constexpr std::uint32_t kMostChunks = 4096;

constexpr std::uint16_t kCompiledBones = 0x2000;
constexpr std::size_t kBonesHead = 32;
constexpr std::size_t kBoneSize = 584;
constexpr std::size_t kBoneBind = 264;
constexpr std::size_t kBoneName = 312;
constexpr std::size_t kBoneNameSize = 256;
constexpr std::size_t kBoneParent = 572;

constexpr std::uint16_t kDataStream = 0x1016;
constexpr std::size_t kStreamHead = 24;
constexpr std::uint32_t kPositionsStream = 0;
constexpr std::uint32_t kPositionSize = 12;
constexpr std::uint32_t kNormalsStream = 1;
constexpr std::uint32_t kNormalSize = 12;
constexpr std::uint32_t kBoneMapStream = 9;
constexpr std::uint32_t kBoneMapSize = 12;

template <class T>
T value_at(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  T value{};
  std::memcpy(&value, &bytes[offset], sizeof(value));
  return value;
}

struct Chunk {
  std::uint16_t type = 0;
  std::uint32_t size = 0;
  std::uint32_t offset = 0;
};

bool read_chunks(const std::vector<std::uint8_t>& bytes, std::vector<Chunk>& chunks) {
  if (bytes.size() < kHeaderSize || std::memcmp(bytes.data(), "CrCh", 4) != 0 ||
      value_at<std::uint32_t>(bytes, 4) != kVersion) {
    return false;
  }
  const auto count = value_at<std::uint32_t>(bytes, 8);
  const auto table = value_at<std::uint32_t>(bytes, 12);
  if (count > kMostChunks || std::size_t(table) + std::size_t(count) * kEntrySize > bytes.size()) {
    return false;
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    const std::size_t entry = table + std::size_t(index) * kEntrySize;
    Chunk chunk;
    chunk.type = value_at<std::uint16_t>(bytes, entry);
    chunk.size = value_at<std::uint32_t>(bytes, entry + 8);
    chunk.offset = value_at<std::uint32_t>(bytes, entry + 12);
    if (std::size_t(chunk.offset) + chunk.size > bytes.size()) {
      return false;
    }
    chunks.push_back(chunk);
  }
  return true;
}

bool read_joints(const std::vector<std::uint8_t>& bytes, const Chunk& chunk,
                 std::vector<Joint>& joints) {
  if (chunk.size < kBonesHead || (chunk.size - kBonesHead) % kBoneSize != 0) {
    return false;
  }
  const std::size_t count = (chunk.size - kBonesHead) / kBoneSize;
  for (std::size_t index = 0; index < count; ++index) {
    const std::size_t record = chunk.offset + kBonesHead + index * kBoneSize;
    Joint joint;
    const char* name = reinterpret_cast<const char*>(&bytes[record + kBoneName]);
    joint.name.assign(name, strnlen(name, kBoneNameSize));
    const auto parent = value_at<std::int32_t>(bytes, record + kBoneParent);
    joint.parent = parent == 0 ? -1 : int(index) + parent;
    if (joint.parent >= int(count) || joint.parent < -1) {
      return false;
    }
    std::memcpy(joint.bind.data(), &bytes[record + kBoneBind], sizeof(joint.bind));
    joints.push_back(joint);
  }
  return !joints.empty();
}

}  // namespace

bool read_skin(const std::vector<std::uint8_t>& bytes, SkinFile& skin) {
  std::vector<Chunk> chunks;
  if (!read_chunks(bytes, chunks)) {
    return false;
  }
  bool has_joints = false;
  bool has_positions = false;
  bool has_bone_map = false;
  std::size_t vertex_count = 0;
  std::size_t bone_map_count = 0;
  for (const Chunk& chunk : chunks) {
    if (chunk.type == kCompiledBones && !has_joints) {
      if (!read_joints(bytes, chunk, skin.joints)) {
        return false;
      }
      has_joints = true;
      continue;
    }
    if (chunk.type != kDataStream || chunk.size < kStreamHead) {
      continue;
    }
    const auto type = value_at<std::uint32_t>(bytes, chunk.offset + 4);
    const auto count = value_at<std::uint32_t>(bytes, chunk.offset + 8);
    const auto size = value_at<std::uint32_t>(bytes, chunk.offset + 12);
    const std::size_t data = chunk.offset + kStreamHead;
    if (std::size_t(count) * size > chunk.size - kStreamHead) {
      return false;
    }
    if (type == kPositionsStream && !has_positions) {
      if (size != kPositionSize) {
        return false;
      }
      skin.positions.resize(std::size_t(count) * 3);
      std::memcpy(skin.positions.data(), &bytes[data], std::size_t(count) * kPositionSize);
      skin.positions_offset = data;
      vertex_count = count;
      has_positions = true;
    } else if (type == kNormalsStream && size == kNormalSize && skin.normals.empty()) {
      skin.normals.resize(std::size_t(count) * 3);
      std::memcpy(skin.normals.data(), &bytes[data], std::size_t(count) * kNormalSize);
    } else if (type == kBoneMapStream && !has_bone_map) {
      if (size != kBoneMapSize) {
        return false;
      }
      skin.influences.resize(count);
      for (std::uint32_t vertex = 0; vertex < count; ++vertex) {
        const std::size_t at = data + std::size_t(vertex) * kBoneMapSize;
        Influence& influence = skin.influences[vertex];
        for (std::size_t k = 0; k < 4; ++k) {
          influence.joints[k] = value_at<std::uint16_t>(bytes, at + k * 2);
          influence.weights[k] = float(bytes[at + 8 + k]) / 255.0f;
        }
      }
      bone_map_count = count;
      has_bone_map = true;
    }
  }
  if (!has_joints || has_positions != has_bone_map || vertex_count != bone_map_count) {
    return false;
  }
  if (!skin.normals.empty() && skin.normals.size() != skin.positions.size()) {
    skin.normals.clear();
  }
  for (const Influence& influence : skin.influences) {
    for (std::size_t k = 0; k < 4; ++k) {
      if (influence.weights[k] > 0.0f && influence.joints[k] >= skin.joints.size()) {
        return false;
      }
    }
  }
  return true;
}

std::vector<std::uint8_t> with_positions(const std::vector<std::uint8_t>& bytes,
                                         const SkinFile& skin,
                                         const std::vector<float>& positions) {
  if (positions.size() != skin.positions.size() || skin.positions.empty() ||
      skin.positions_offset + positions.size() * sizeof(float) > bytes.size()) {
    return {};
  }
  std::vector<std::uint8_t> copy = bytes;
  std::memcpy(&copy[skin.positions_offset], positions.data(), positions.size() * sizeof(float));
  return copy;
}

}  // namespace kcds::runtime::appearance::refit
