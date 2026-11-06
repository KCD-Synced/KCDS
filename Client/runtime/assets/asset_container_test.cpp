#include "asset_container.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/testkit.hpp"

namespace {

namespace container = kcds::runtime::assets::container;

std::vector<std::uint8_t> sample_pak() {
  std::vector<std::uint8_t> pak;
  for (int i = 0; i < 5000; ++i) {
    pak.push_back(static_cast<std::uint8_t>(i * 31 + 7));
  }
  return pak;
}

}  // namespace

KCD2S_TEST(asset_container, opens_with_its_resource_and_secret) {
  const auto pak = sample_pak();
  const auto sealed = container::seal(pak, "whips", "server-secret");
  KCD2S_CHECK(!sealed.empty());
  KCD2S_CHECK(container::resource_of(sealed) == std::optional<std::string>("whips"));
  const auto opened = container::open(sealed, "whips", "server-secret");
  KCD2S_CHECK(opened.has_value());
  KCD2S_CHECK(*opened == pak);
}

KCD2S_TEST(asset_container, plain_bytes_are_not_the_pak) {
  const auto pak = sample_pak();
  const auto sealed = container::seal(pak, "whips", "server-secret");
  // The payload is not the zip: no plain run of the pak is in the container.
  const std::vector<std::uint8_t> run(pak.begin() + 100, pak.begin() + 132);
  bool found = false;
  for (std::size_t at = 0; at + run.size() <= sealed.size(); ++at) {
    if (std::equal(run.begin(), run.end(), sealed.begin() + at)) {
      found = true;
    }
  }
  KCD2S_CHECK(!found);
  // Two seals of the same pak differ (a new nonce each time).
  KCD2S_CHECK(container::seal(pak, "whips", "server-secret") != sealed);
}

KCD2S_TEST(asset_container, refuses_another_secret_resource_or_change) {
  const auto pak = sample_pak();
  const auto sealed = container::seal(pak, "whips", "server-secret");
  KCD2S_CHECK(!container::open(sealed, "whips", "another-secret").has_value());
  KCD2S_CHECK(!container::open(sealed, "ropes", "server-secret").has_value());
  auto changed = sealed;
  changed[changed.size() - 10] ^= 0x01;
  KCD2S_CHECK(!container::open(changed, "whips", "server-secret").has_value());
  // A container renamed to another resource does not open under that name either.
  auto renamed = sealed;
  renamed[42] = 'W';
  KCD2S_CHECK(!container::open(renamed, "Whips", "server-secret").has_value());
}

// A file the server sealed (Backend/scripting/runtimes/js/resource_seal.cjs): the
// game client opens what Node's AES-GCM sealed, as the server opens what this side seals
// (resource_seal.test.cjs).
KCD2S_TEST(asset_container, opens_a_file_the_server_sealed) {
  const std::vector<std::uint8_t> sealed = {
      0x4b, 0x43, 0x44, 0x53, 0x41, 0x53, 0x54, 0x31, 0x01, 0x00, 0x00, 0x00, 0xf4, 0xdd,
      0xa6, 0x14, 0xfe, 0xd6, 0x74, 0xbc, 0x6a, 0x4d, 0xa5, 0x8a, 0x6c, 0x45, 0xa0, 0x95,
      0x3b, 0x57, 0x8c, 0x81, 0xab, 0x5f, 0xa0, 0x95, 0x34, 0x1f, 0xc5, 0x8c, 0x12, 0x00,
      0x6b, 0x63, 0x64, 0x73, 0x2d, 0x74, 0x65, 0x73, 0x74, 0x2d, 0x72, 0x65, 0x73, 0x6f,
      0x75, 0x72, 0x63, 0x65, 0x6a, 0x10, 0xc6, 0x1f, 0x8a, 0x79, 0xc2, 0x5d, 0x8b, 0x6f,
      0x3f, 0x1b, 0x64, 0xaf, 0xca, 0xbf, 0xfc, 0x3d, 0x8c, 0xa3,
  };
  const auto opened = container::open(sealed, "kcds-test-resource", "fixture-secret-0123456789");
  KCD2S_CHECK(opened.has_value());
  const std::string text = "sealed by the server";
  KCD2S_CHECK(*opened == std::vector<std::uint8_t>(text.begin(), text.end()));
}

KCD2S_TEST(asset_container, refuses_what_is_no_container) {
  KCD2S_CHECK(!container::resource_of(sample_pak()).has_value());
  KCD2S_CHECK(!container::open(sample_pak(), "whips", "server-secret").has_value());
  KCD2S_CHECK(!container::open({}, "whips", "server-secret").has_value());
}
