// A model's material path rewritten (studio/cgf_patch.hpp).
#include "cgf_patch.hpp"

#include <cstring>
#include <string>

#include "kcd2s/testkit.hpp"

namespace {

namespace cgf = kcds::runtime::studio::cgf;

void put(std::vector<std::uint8_t>& out, std::uint32_t value, std::size_t size = 4) {
  for (std::size_t part = 0; part < size; ++part) {
    out.push_back(std::uint8_t(value >> (part * 8)));
  }
}

// A file with two chunks: a MtlName (128-byte name, then one sub-material count) and another.
std::vector<std::uint8_t> model_with(const std::string& material) {
  std::vector<std::uint8_t> out;
  out.insert(out.end(), {'C', 'r', 'C', 'h'});
  put(out, 0x746);
  put(out, 2);
  put(out, 16);
  const std::uint32_t data = 16 + 2 * 16;
  put(out, 0x1014, 2);
  put(out, 0x802, 2);
  put(out, 1);
  put(out, 132);
  put(out, data);
  put(out, 0x100B, 2);
  put(out, 0x824, 2);
  put(out, 2);
  put(out, 4);
  put(out, data + 132);
  std::vector<std::uint8_t> name(128, 0);
  std::memcpy(name.data(), material.data(), material.size());
  out.insert(out.end(), name.begin(), name.end());
  put(out, 1);
  put(out, 0xDEADBEEF);
  return out;
}

std::string name_in(const std::vector<std::uint8_t>& model) {
  return std::string(reinterpret_cast<const char*>(&model[48]));
}

}  // namespace

KCD2S_TEST(cgf_patch, names_the_resources_material) {
  auto model = model_with("objects/kcds_studio/suzanne");
  KCD2S_CHECK(cgf::rename_material(model, "objects/kcds/kcds_studio_mesh/suzanne"));
  KCD2S_CHECK_EQ(name_in(model), std::string("objects/kcds/kcds_studio_mesh/suzanne"));
  // The rest is untouched: the sub-material count and the next chunk.
  KCD2S_CHECK_EQ(int(model[48 + 128]), 1);
  KCD2S_CHECK_EQ(int(model.back()), 0xDE);
  // A shorter name leaves no tail of the old one.
  KCD2S_CHECK(cgf::rename_material(model, "objects/kcds/x/y"));
  KCD2S_CHECK_EQ(name_in(model), std::string("objects/kcds/x/y"));
}

KCD2S_TEST(cgf_patch, refuses_what_is_not_such_a_model) {
  auto model = model_with("objects/kcds_studio/a");
  const auto before = model;
  KCD2S_CHECK(!cgf::rename_material(model, std::string(128, 'x')));
  KCD2S_CHECK(model == before);
  std::vector<std::uint8_t> other = {'n', 'o', 'p', 'e', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  KCD2S_CHECK(!cgf::rename_material(other, "objects/kcds/x/y"));
  model[4] = 0x45;
  KCD2S_CHECK(!cgf::rename_material(model, "objects/kcds/x/y"));
}
