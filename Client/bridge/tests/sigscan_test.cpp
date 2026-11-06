#include "kcd2s/bridge/sigscan.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

#include "kcd2s/testkit.hpp"

using namespace kcd2s::bridge;

KCD2S_TEST(sigscan, parse_bytes_and_wildcards) {
  auto s = Signature::parse("48 8B ?? 89 ?");
  KCD2S_CHECK(s.has_value());
  KCD2S_CHECK_EQ(s->size(), std::size_t{5});
  KCD2S_CHECK_EQ(s->bytes()[0].value, std::uint8_t{0x48});
  KCD2S_CHECK(!s->bytes()[0].wildcard);
  KCD2S_CHECK(s->bytes()[2].wildcard);
  KCD2S_CHECK(s->bytes()[4].wildcard);
}

KCD2S_TEST(sigscan, parse_rejects_garbage) {
  KCD2S_CHECK(!Signature::parse("zz").has_value());
  KCD2S_CHECK(!Signature::parse("4").has_value());  // odd nibble
  KCD2S_CHECK(!Signature::parse("").has_value());
}

KCD2S_TEST(sigscan, finds_exact_match) {
  std::vector<std::uint8_t> img = {0x00, 0x11, 0x48, 0x8B, 0x05, 0xCC, 0x90, 0x90};
  auto s = Signature::parse("48 8B 05");
  KCD2S_CHECK_EQ(scan(img.data(), img.size(), *s), std::size_t{2});
}

KCD2S_TEST(sigscan, wildcards_match_any_byte) {
  std::vector<std::uint8_t> img = {0x48, 0x8B, 0xAB, 0x89, 0x12, 0x48, 0x8B, 0xFF, 0x89, 0x34};
  auto s = Signature::parse("48 8B ?? 89");
  KCD2S_CHECK_EQ(scan(img.data(), img.size(), *s), std::size_t{0});  // first occurrence
}

// The anchored scan finds what a comparison at every position finds: the first match, also
// with leading wildcards, common anchor bytes and a pattern of wildcards only.
KCD2S_TEST(sigscan, anchored_scan_matches_the_plain_scan) {
  std::vector<std::uint8_t> img(4096);
  std::uint32_t state = 12345;
  for (auto& b : img) {
    state = state * 1103515245u + 12345u;
    b = static_cast<std::uint8_t>((state >> 16) % 8 == 0 ? 0x48 : (state >> 16));
  }
  const char* patterns[] = {"48 8B ?? 89", "?? ?? 48", "48 48", "?? ??", "C3 ?? CC", "7F 3A ?? 48"};
  for (const char* text : patterns) {
    auto s = Signature::parse(text);
    KCD2S_CHECK(s.has_value());
    std::size_t expected = knpos;
    for (std::size_t i = 0; i + s->size() <= img.size(); ++i) {
      if (s->match_at(img.data() + i, img.size() - i)) {
        expected = i;
        break;
      }
    }
    KCD2S_CHECK_EQ(scan(img.data(), img.size(), *s), expected);
    if (expected != knpos) {
      const std::size_t rest = scan(img.data() + expected + 1, img.size() - expected - 1, *s);
      std::size_t expected_rest = knpos;
      for (std::size_t i = expected + 1; i + s->size() <= img.size(); ++i) {
        if (s->match_at(img.data() + i, img.size() - i)) {
          expected_rest = i - expected - 1;
          break;
        }
      }
      KCD2S_CHECK_EQ(rest, expected_rest);
    }
  }
}

KCD2S_TEST(sigscan, no_match_returns_npos) {
  std::vector<std::uint8_t> img = {0x01, 0x02, 0x03};
  auto s = Signature::parse("DE AD");
  KCD2S_CHECK(scan(img.data(), img.size(), *s) == knpos);
}

KCD2S_TEST(sigscan, pattern_longer_than_image) {
  std::vector<std::uint8_t> img = {0x48};
  auto s = Signature::parse("48 8B 05");
  KCD2S_CHECK(scan(img.data(), img.size(), *s) == knpos);
}

KCD2S_TEST(sigscan, rip_relative_target) {
  // mov rax, [rip+disp] -> "48 8B 05 <disp32>", instruction length 7. Place a global at offset
  // 0x20. At match offset 3 the disp sits; next instruction at match+7. We want target == 0x20.
  // disp = target - next_insn = 0x20 - 0x07 = 0x19.
  std::vector<std::uint8_t> img(0x40, 0x00);
  img[0] = 0x48;
  img[1] = 0x8B;
  img[2] = 0x05;
  img[3] = 0x19;  // disp low byte
  img[4] = 0x00;
  img[5] = 0x00;
  img[6] = 0x00;
  const std::size_t tgt = rip_target(img.data(), img.size(), /*disp_off=*/3, /*next_insn_off=*/7);
  KCD2S_CHECK_EQ(tgt, std::size_t{0x20});
}

KCD2S_TEST(sigscan, rip_relative_negative_disp) {
  std::vector<std::uint8_t> img(0x40, 0x00);
  // next instruction at 0x30, target 0x10 -> disp = 0x10 - 0x30 = -0x20 = 0xFFFFFFE0.
  img[0x2C] = 0xE0;
  img[0x2D] = 0xFF;
  img[0x2E] = 0xFF;
  img[0x2F] = 0xFF;
  const std::size_t tgt = rip_target(img.data(), img.size(), 0x2C, 0x30);
  KCD2S_CHECK_EQ(tgt, std::size_t{0x10});
}

KCD2S_TEST(sigscan, rip_out_of_bounds_is_npos) {
  std::vector<std::uint8_t> img(0x10, 0x00);
  // disp points far outside the image.
  img[0] = 0xFF;
  img[1] = 0xFF;
  img[2] = 0x00;
  img[3] = 0x00;
  KCD2S_CHECK(rip_target(img.data(), img.size(), 0, 4) == knpos);
}

namespace {

void put32(std::vector<std::uint8_t>& image, std::size_t at, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    image[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
  }
}

void put64(std::vector<std::uint8_t>& image, std::size_t at, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    image[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
  }
}

}  // namespace

KCD2S_TEST(sigscan, rtti_vtable_follows_the_complete_object_locator) {
  constexpr std::uint64_t kBase = 0x180000000;
  std::vector<std::uint8_t> image(0x200, 0);
  const char name[] = ".?AVC_Smithy@test@@";
  // The type descriptor at 0x40: its vtable pointer, a spare pointer, then the name.
  std::memcpy(image.data() + 0x50, name, sizeof(name));
  // The complete object locator at 0x100: signature 1, offset 0, cd offset, the descriptor's,
  // the hierarchy's and its own offsets.
  put32(image, 0x100, 1);
  put32(image, 0x104, 0);
  put32(image, 0x10C, 0x40);
  put32(image, 0x114, 0x100);
  // The vtable at 0x180 follows a pointer to the locator.
  put64(image, 0x178, kBase + 0x100);
  KCD2S_CHECK_EQ(rtti_vtable(image.data(), image.size(), kBase, name), std::size_t{0x180});
  KCD2S_CHECK_EQ(rtti_vtable(image.data(), image.size(), kBase, ".?AVC_Other@test@@"), knpos);
  // A locator of a base class's vtable (offset 8) is not the class's own.
  put32(image, 0x104, 8);
  KCD2S_CHECK_EQ(rtti_vtable(image.data(), image.size(), kBase, name), knpos);
}
