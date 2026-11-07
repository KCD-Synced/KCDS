// A game building's streamed meshes (its .cgfm) split into models of their own, for World
// Architect's opened buildings (asset_files.hpp). The game streams a building's meshes from the
// .cgfm beside its model, which it does not do for an author's assets (a copy stalled the
// render): each of the .cgfm's drawn nodes becomes a model of its own here, its mesh with its
// subsets and streams, drawn with another material, without physics and children. The same split
// as Dev/tools/architect-mcp/models/open_core.cjs drawnPieces, which the MCP server runs on its
// own PC; this one runs in the game, where the largest .cgfm (36 MB) is read without a copy
// through the resource runtime. Header only, so the test reads it without the game.
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace kcds::runtime::architect::model_split {

constexpr std::uint16_t kChunkMesh = 0x1000;
constexpr std::uint16_t kChunkNode = 0x100b;
constexpr std::uint16_t kChunkMaterial = 0x1014;
constexpr std::uint16_t kChunkExport = 0x1015;
constexpr std::size_t kMaterialName = 128;
// A node's mesh, parent, number of children and material chunk.
constexpr std::size_t kNodeObject = 64;
constexpr std::size_t kNodeParent = 68;
constexpr std::size_t kNodeChildren = 72;
constexpr std::size_t kNodeMaterial = 76;
// A mesh's subsets chunk, its 16 stream chunks and its 4 physics chunks.
constexpr std::size_t kMeshSubsets = 20;
constexpr std::size_t kMeshStreams = 28;
constexpr std::size_t kMeshPhysics = 92;
constexpr std::size_t kMostPieces = 256;

struct Chunk {
  std::uint16_t kind = 0;
  std::uint16_t version = 0;
  std::uint32_t id = 0;
  std::vector<std::uint8_t> body;
};

namespace detail {

inline std::uint32_t u32(const std::uint8_t* at) {
  std::uint32_t value = 0;
  std::memcpy(&value, at, 4);
  return value;
}

inline std::int32_t i32(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  std::int32_t value = 0;
  if (at + 4 <= bytes.size()) {
    std::memcpy(&value, bytes.data() + at, 4);
  }
  return value;
}

inline void put32(std::vector<std::uint8_t>& bytes, std::size_t at, std::int32_t value) {
  if (at + 4 <= bytes.size()) {
    std::memcpy(bytes.data() + at, &value, 4);
  }
}

}  // namespace detail

// The chunks of a CrCh chunk file, in the table's order; false when it is none or a chunk lies
// outside it.
inline bool chunks_of(const std::vector<std::uint8_t>& file, std::vector<Chunk>& chunks) {
  if (file.size() < 16 || std::memcmp(file.data(), "CrCh", 4) != 0) {
    return false;
  }
  const std::uint32_t count = detail::u32(file.data() + 8);
  const std::uint32_t table = detail::u32(file.data() + 12);
  chunks.clear();
  for (std::uint32_t index = 0; index < count; ++index) {
    const std::size_t at = std::size_t(table) + std::size_t(index) * 16;
    if (at + 16 > file.size()) {
      return false;
    }
    Chunk chunk;
    std::memcpy(&chunk.kind, file.data() + at, 2);
    std::memcpy(&chunk.version, file.data() + at + 2, 2);
    chunk.id = detail::u32(file.data() + at + 4);
    const std::size_t size = detail::u32(file.data() + at + 8);
    const std::size_t offset = detail::u32(file.data() + at + 12);
    if (offset > file.size() || size > file.size() - offset) {
      return false;
    }
    chunk.body.assign(file.begin() + std::ptrdiff_t(offset),
                      file.begin() + std::ptrdiff_t(offset + size));
    chunks.push_back(std::move(chunk));
  }
  return true;
}

// A chunk file of chunks: the header, the table after it, then the bodies in order.
inline std::vector<std::uint8_t> chunk_file(const std::vector<Chunk>& chunks) {
  const std::size_t header = 16;
  std::size_t offset = header + chunks.size() * 16;
  std::size_t total = offset;
  for (const Chunk& chunk : chunks) {
    total += chunk.body.size();
  }
  std::vector<std::uint8_t> out(total, 0);
  std::memcpy(out.data(), "CrCh", 4);
  detail::put32(out, 4, 0x746);
  detail::put32(out, 8, std::int32_t(chunks.size()));
  detail::put32(out, 12, std::int32_t(header));
  for (std::size_t index = 0; index < chunks.size(); ++index) {
    const Chunk& chunk = chunks[index];
    const std::size_t at = header + index * 16;
    std::memcpy(out.data() + at, &chunk.kind, 2);
    std::memcpy(out.data() + at + 2, &chunk.version, 2);
    detail::put32(out, at + 4, std::int32_t(chunk.id));
    detail::put32(out, at + 8, std::int32_t(chunk.body.size()));
    detail::put32(out, at + 12, std::int32_t(offset));
    if (!chunk.body.empty()) {
      std::memcpy(out.data() + offset, chunk.body.data(), chunk.body.size());
    }
    offset += chunk.body.size();
  }
  return out;
}

// Each drawn node of a .cgfm as a model of its own, drawn with `material`; false when the bytes
// are no chunk file or the material's name does not fit.
inline bool split(const std::vector<std::uint8_t>& cgfm, const std::string& material,
                  std::vector<std::vector<std::uint8_t>>& pieces) {
  std::vector<Chunk> chunks;
  if (material.size() >= kMaterialName || !chunks_of(cgfm, chunks)) {
    return false;
  }
  std::map<std::uint32_t, const Chunk*> by_id;
  const Chunk* exported = nullptr;
  for (const Chunk& chunk : chunks) {
    by_id[chunk.id] = &chunk;
    if (chunk.kind == kChunkExport && !exported) {
      exported = &chunk;
    }
  }
  const auto find = [&by_id](std::int32_t id) -> const Chunk* {
    const auto found = by_id.find(std::uint32_t(id));
    return found == by_id.end() ? nullptr : found->second;
  };
  pieces.clear();
  for (const Chunk& node : chunks) {
    if (node.kind != kChunkNode || node.body.size() < kNodeMaterial + 4) {
      continue;
    }
    const Chunk* mesh = find(detail::i32(node.body, kNodeObject));
    if (!mesh || mesh->kind != kChunkMesh || mesh->body.size() < kMeshPhysics + 16) {
      continue;
    }
    if (pieces.size() >= kMostPieces) {
      return false;
    }
    Chunk node_copy = node;
    detail::put32(node_copy.body, kNodeParent, -1);
    detail::put32(node_copy.body, kNodeChildren, 0);
    Chunk mesh_copy = *mesh;
    std::memset(mesh_copy.body.data() + kMeshPhysics, 0, 16);
    std::vector<Chunk> own;
    if (exported) {
      own.push_back(*exported);
    }
    const Chunk* named = find(detail::i32(node_copy.body, kNodeMaterial));
    if (named && named->body.size() >= kMaterialName) {
      Chunk material_copy = *named;
      std::memset(material_copy.body.data(), 0, kMaterialName);
      std::memcpy(material_copy.body.data(), material.data(), material.size());
      own.push_back(std::move(material_copy));
    }
    if (const Chunk* subsets = find(detail::i32(mesh_copy.body, kMeshSubsets))) {
      own.push_back(*subsets);
    }
    for (std::size_t k = 0; k < 16; ++k) {
      if (const Chunk* stream = find(detail::i32(mesh_copy.body, kMeshStreams + k * 4))) {
        own.push_back(*stream);
      }
    }
    own.push_back(std::move(mesh_copy));
    own.push_back(std::move(node_copy));
    pieces.push_back(chunk_file(own));
  }
  return true;
}

}  // namespace kcds::runtime::architect::model_split
