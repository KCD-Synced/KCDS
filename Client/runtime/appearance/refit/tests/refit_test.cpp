#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "../body_fit.hpp"
#include "../skin_file.hpp"
#include "kcd2s/testkit.hpp"

using namespace kcds::runtime::appearance::refit;

namespace {

constexpr Matrix34 kIdentity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

void put_u16(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint16_t value) {
  std::memcpy(&bytes[at], &value, sizeof(value));
}

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
  std::memcpy(&bytes[at], &value, sizeof(value));
}

void put_f32(std::vector<std::uint8_t>& bytes, std::size_t at, float value) {
  std::memcpy(&bytes[at], &value, sizeof(value));
}

// A chunk file with two joints ("Root" and "Head" under it, Head 1.5 m up), and a mesh of two
// vertices: one on Root, one shared half and half.
std::vector<std::uint8_t> small_skin() {
  const std::size_t bones_at = 16 + 3 * 16;
  const std::size_t bones_size = 32 + 2 * 584;
  const std::size_t positions_at = bones_at + bones_size;
  const std::size_t positions_size = 24 + 2 * 12;
  const std::size_t map_at = positions_at + positions_size;
  const std::size_t map_size = 24 + 2 * 12;
  std::vector<std::uint8_t> bytes(map_at + map_size, 0);
  std::memcpy(bytes.data(), "CrCh", 4);
  put_u32(bytes, 4, 0x746);
  put_u32(bytes, 8, 3);
  put_u32(bytes, 12, 16);
  const std::size_t entries[3][3] = {{0x2000, bones_size, bones_at},
                                     {0x1016, positions_size, positions_at},
                                     {0x1016, map_size, map_at}};
  for (std::size_t index = 0; index < 3; ++index) {
    put_u16(bytes, 16 + index * 16, std::uint16_t(entries[index][0]));
    put_u32(bytes, 16 + index * 16 + 8, std::uint32_t(entries[index][1]));
    put_u32(bytes, 16 + index * 16 + 12, std::uint32_t(entries[index][2]));
  }
  const char* names[2] = {"Root", "Head"};
  for (std::size_t joint = 0; joint < 2; ++joint) {
    const std::size_t record = bones_at + 32 + joint * 584;
    for (std::size_t k = 0; k < 12; ++k) {
      put_f32(bytes, record + 264 + k * 4, kIdentity[k]);
    }
    if (joint == 1) {
      put_f32(bytes, record + 264 + 11 * 4, 1.5f);
      put_u32(bytes, record + 572, std::uint32_t(-1));
    }
    std::memcpy(&bytes[record + 312], names[joint], std::strlen(names[joint]));
  }
  put_u32(bytes, positions_at + 4, 0);
  put_u32(bytes, positions_at + 8, 2);
  put_u32(bytes, positions_at + 12, 12);
  const float positions[6] = {0.1f, 0.2f, 0.3f, 0.0f, 0.0f, 1.6f};
  std::memcpy(&bytes[positions_at + 24], positions, sizeof(positions));
  put_u32(bytes, map_at + 4, 9);
  put_u32(bytes, map_at + 8, 2);
  put_u32(bytes, map_at + 12, 12);
  bytes[map_at + 24 + 8] = 255;
  put_u16(bytes, map_at + 36, 0);
  put_u16(bytes, map_at + 38, 1);
  bytes[map_at + 36 + 8] = 128;
  bytes[map_at + 36 + 9] = 127;
  return bytes;
}

// A sphere of points (and its outward normals) around `centre`, all on one joint.
SkinFile sphere(float radius, int rings, int segments) {
  SkinFile skin;
  Joint root;
  root.name = "Root";
  root.bind = kIdentity;
  skin.joints.push_back(root);
  const float pi = 3.14159265f;
  for (int ring = 1; ring < rings; ++ring) {
    const float polar = pi * float(ring) / float(rings);
    for (int segment = 0; segment < segments; ++segment) {
      const float around = 2.0f * pi * float(segment) / float(segments);
      const float n[3] = {std::sin(polar) * std::cos(around), std::sin(polar) * std::sin(around),
                          std::cos(polar)};
      for (int k = 0; k < 3; ++k) {
        skin.positions.push_back(n[k] * radius);
        skin.normals.push_back(n[k]);
      }
      Influence influence;
      influence.weights = {1.0f, 0.0f, 0.0f, 0.0f};
      skin.influences.push_back(influence);
    }
  }
  return skin;
}

float mean_radius(const std::vector<float>& positions) {
  float sum = 0.0f;
  const std::size_t count = positions.size() / 3;
  for (std::size_t vertex = 0; vertex < count; ++vertex) {
    const float* p = &positions[vertex * 3];
    sum += std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
  }
  return sum / float(count);
}

}  // namespace

KCD2S_TEST(refit, reads_joints_positions_and_bone_map) {
  SkinFile skin;
  KCD2S_CHECK(read_skin(small_skin(), skin));
  KCD2S_CHECK(skin.joints.size() == 2);
  KCD2S_CHECK(skin.joints[0].name == "Root" && skin.joints[0].parent == -1);
  KCD2S_CHECK(skin.joints[1].name == "Head" && skin.joints[1].parent == 0);
  KCD2S_CHECK(std::fabs(skin.joints[1].bind[11] - 1.5f) < 1e-6f);
  KCD2S_CHECK(skin.positions.size() == 6 && std::fabs(skin.positions[5] - 1.6f) < 1e-6f);
  KCD2S_CHECK(skin.influences.size() == 2);
  KCD2S_CHECK(skin.influences[1].joints[1] == 1);
  KCD2S_CHECK(std::fabs(skin.influences[1].weights[0] - 128.0f / 255.0f) < 1e-6f);
}

KCD2S_TEST(refit, a_copy_changes_only_its_positions) {
  const std::vector<std::uint8_t> bytes = small_skin();
  SkinFile skin;
  KCD2S_CHECK(read_skin(bytes, skin));
  std::vector<float> moved = skin.positions;
  moved[0] = 9.0f;
  const std::vector<std::uint8_t> copy = with_positions(bytes, skin, moved);
  KCD2S_CHECK(copy.size() == bytes.size());
  SkinFile again;
  KCD2S_CHECK(read_skin(copy, again));
  KCD2S_CHECK(again.positions == moved);
  std::size_t differing = 0;
  for (std::size_t at = 0; at < bytes.size(); ++at) {
    differing += bytes[at] != copy[at] ? 1 : 0;
  }
  KCD2S_CHECK(differing <= 4);
  // A position list of another size gives nothing.
  KCD2S_CHECK(with_positions(bytes, skin, std::vector<float>(3, 0.0f)).empty());
}

KCD2S_TEST(refit, not_a_chunk_file_is_refused) {
  SkinFile skin;
  KCD2S_CHECK(!read_skin(std::vector<std::uint8_t>(64, 0), skin));
  std::vector<std::uint8_t> bytes = small_skin();
  put_u32(bytes, 4, 0x745);
  KCD2S_CHECK(!read_skin(bytes, skin));
}

KCD2S_TEST(refit, carry_follows_her_joints_and_the_nearest_ancestor) {
  std::vector<Joint> piece(3);
  piece[0].name = "Root";
  piece[0].bind = kIdentity;
  piece[1].name = "Head";
  piece[1].parent = 0;
  piece[1].bind = kIdentity;
  piece[1].bind[11] = 1.6f;
  piece[2].name = "strap";
  piece[2].parent = 1;
  piece[2].bind = kIdentity;
  std::vector<Joint> hers(2);
  hers[0].name = "Root";
  hers[0].bind = kIdentity;
  hers[1].name = "Head";
  hers[1].parent = 0;
  hers[1].bind = kIdentity;
  hers[1].bind[11] = 1.5f;
  const std::vector<Matrix34> carried = carry(piece, hers);
  KCD2S_CHECK(std::fabs(carried[0][11]) < 1e-6f);
  // Her head is 10 cm lower: what hangs on the head comes down with it, the strap too.
  KCD2S_CHECK(std::fabs(carried[1][11] + 0.1f) < 1e-5f);
  KCD2S_CHECK(std::fabs(carried[2][11] + 0.1f) < 1e-5f);
}

KCD2S_TEST(refit, a_piece_follows_the_smaller_body) {
  // His body a sphere of 20 cm, hers of 15 cm, the piece 3 cm off his.
  const SkinFile his = sphere(0.20f, 24, 48);
  const SkinFile hers_body = sphere(0.15f, 24, 48);
  const SkinFile piece = sphere(0.23f, 12, 24);
  std::vector<Joint> her_joints = his.joints;
  BodyField field;
  FieldSettings settings;
  settings.reach = 0.10f;
  KCD2S_CHECK(field.build(his, her_joints, hers_body.positions, settings));
  const std::vector<float> fitted = fit(piece, carry(piece.joints, her_joints), field);
  KCD2S_CHECK(fitted.size() == piece.positions.size());
  // It keeps its 3 cm off her body, give or take the mesh's own spacing.
  KCD2S_CHECK(std::fabs(mean_radius(fitted) - 0.18f) < 0.01f);
}

KCD2S_TEST(refit, far_from_the_body_nothing_moves) {
  const SkinFile his = sphere(0.20f, 16, 32);
  const SkinFile hers_body = sphere(0.15f, 16, 32);
  const SkinFile piece = sphere(0.60f, 6, 12);
  BodyField field;
  KCD2S_CHECK(field.build(his, his.joints, hers_body.positions));
  const std::vector<float> fitted = fit(piece, carry(piece.joints, his.joints), field);
  KCD2S_CHECK(fitted == piece.positions);
}
