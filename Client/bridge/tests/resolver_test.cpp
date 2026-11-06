#include "kcd2s/bridge/resolver.hpp"

#include <cstdint>
#include <vector>

#include "kcd2s/testkit.hpp"

using namespace kcd2s::bridge;

KCD2S_TEST(resolver, resolves_function_entry) {
  // A fake module with a recognizable prologue at offset 0x10.
  std::vector<std::uint8_t> img(0x40, 0x00);
  img[0x10] = 0x40;
  img[0x11] = 0x53;  // push rbx
  img[0x12] = 0x48;
  img[0x13] = 0x83;
  img[0x14] = 0xEC;  // sub rsp, imm8

  Resolver r;
  KCD2S_CHECK(r.add("SomeFunc", "40 53 48 83 EC"));
  auto off = r.resolve("SomeFunc", img.data(), img.size());
  KCD2S_CHECK(off.has_value());
  KCD2S_CHECK_EQ(*off, std::size_t{0x10});
}

KCD2S_TEST(resolver, resolves_rip_relative_global) {
  // "mov rax, [rip+disp]" = 48 8B 05 <disp32>, len 7, at offset 0x08; global at 0x30.
  // disp = 0x30 - (0x08+7) = 0x30 - 0x0F = 0x21.
  std::vector<std::uint8_t> img(0x40, 0x00);
  img[0x08] = 0x48;
  img[0x09] = 0x8B;
  img[0x0A] = 0x05;
  img[0x0B] = 0x21;

  Resolver r;
  // disp_at = 3 (within the match), insn_len = 7.
  KCD2S_CHECK(r.add("gEnv", "48 8B 05 ?? ?? ?? ??", /*disp_at=*/3, /*insn_len=*/7));
  auto off = r.resolve("gEnv", img.data(), img.size());
  KCD2S_CHECK(off.has_value());
  KCD2S_CHECK_EQ(*off, std::size_t{0x30});
}

KCD2S_TEST(resolver, unknown_key_and_missing_pattern) {
  std::vector<std::uint8_t> img(0x10, 0x00);
  Resolver r;
  r.add("present", "DE AD BE EF");
  KCD2S_CHECK(!r.resolve("nope", img.data(), img.size()).has_value());     // unknown key
  KCD2S_CHECK(!r.resolve("present", img.data(), img.size()).has_value());  // pattern absent
}

KCD2S_TEST(resolver, rejects_bad_pattern) {
  Resolver r;
  KCD2S_CHECK(!r.add("bad", "zz zz"));
  KCD2S_CHECK_EQ(r.size(), std::size_t{0});
}
