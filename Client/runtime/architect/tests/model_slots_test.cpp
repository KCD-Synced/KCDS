#include "../model_slots.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "kcd2s/testkit.hpp"

namespace model_slots = kcds::runtime::architect::model_slots;

namespace {

// A CGF holding one material chunk: its name, then each slot's physicalization type and name.
std::vector<std::uint8_t> cgf_with(const std::string& material,
                                   const std::vector<std::string>& slots) {
  std::vector<std::uint8_t> body(model_slots::kMaterialName, 0);
  std::memcpy(body.data(), material.data(), material.size());
  const auto add32 = [&body](std::int32_t value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    body.insert(body.end(), bytes, bytes + 4);
  };
  add32(std::int32_t(slots.size()));
  for (std::size_t index = 0; index < slots.size(); ++index) {
    add32(-1);
  }
  for (const std::string& slot : slots) {
    body.insert(body.end(), slot.begin(), slot.end());
    body.push_back(0);
  }
  std::vector<std::uint8_t> file(16 + 16, 0);
  std::memcpy(file.data(), "CrCh", 4);
  const auto put32 = [&file](std::size_t at, std::uint32_t value) {
    std::memcpy(file.data() + at, &value, 4);
  };
  put32(4, 0x746);
  put32(8, 1);
  put32(12, 16);
  const std::uint16_t kind = model_slots::kChunkMaterial;
  std::memcpy(file.data() + 16, &kind, 2);
  put32(16 + 8, std::uint32_t(body.size()));
  put32(16 + 12, 32);
  file.insert(file.end(), body.begin(), body.end());
  return file;
}

}  // namespace

KCD2S_TEST(model_slots, a_models_material_and_its_slots_are_read_in_order) {
  std::string material;
  std::vector<std::string> slots;
  const auto file = cgf_with("objects/kcds/opened/stjames5", {"glass", "plaster_b", "void"});
  KCD2S_CHECK(model_slots::read(file, material, slots));
  KCD2S_CHECK_EQ(material, std::string("objects/kcds/opened/stjames5"));
  KCD2S_CHECK_EQ(slots.size(), std::size_t(3));
  KCD2S_CHECK_EQ(slots[1], std::string("plaster_b"));
}

KCD2S_TEST(model_slots, a_file_without_a_material_chunk_is_refused) {
  std::string material;
  std::vector<std::string> slots;
  std::vector<std::uint8_t> nothing = {'n', 'o', 'p', 'e'};
  KCD2S_CHECK(!model_slots::read(nothing, material, slots));
  auto file = cgf_with("m", {"a"});
  file[16] = 0;  // another kind of chunk
  KCD2S_CHECK(!model_slots::read(file, material, slots));
}

KCD2S_TEST(model_slots, the_editor_writes_its_own_materials_and_textures_only) {
  KCD2S_CHECK(model_slots::writable("objects/kcds/materials/stjames5_walls.mtl"));
  KCD2S_CHECK(model_slots::writable("objects/kcds/textures/textures/stone_a_diff.dds"));
  KCD2S_CHECK(!model_slots::writable("objects/kcds/materials/../../x.mtl"));
  KCD2S_CHECK(!model_slots::writable("objects/manmade/x.mtl"));
  KCD2S_CHECK(!model_slots::writable("objects/kcds/materials/x.cgf"));
  KCD2S_CHECK(model_slots::writable("objects/kcds/opened/house1_walls.cgf"));
  KCD2S_CHECK(!model_slots::writable("objects/kcds/opened/sub/house1.cgf"));
  KCD2S_CHECK(!model_slots::writable("objects/kcds//x.mtl"));
  KCD2S_CHECK(!model_slots::writable("Objects/kcds/materials/X.mtl"));
}
