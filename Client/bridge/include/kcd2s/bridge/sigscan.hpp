// Signature scanning — the mechanism that locates WHGame.dll functions and globals by byte pattern
// per game version, instead of hard-coding addresses that shift every patch.
// This is pure byte matching + RIP-relative math, so it is engine-agnostic and fully
// headless-testable; the Windows glue (engine_win.*) feeds it the real module image at runtime.
#ifndef KCD2S_BRIDGE_SIGSCAN_HPP
#define KCD2S_BRIDGE_SIGSCAN_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace kcd2s::bridge {

constexpr std::size_t knpos = static_cast<std::size_t>(-1);

struct SigByte {
  std::uint8_t value = 0;
  bool wildcard = false;
};

// A byte pattern, e.g. "48 8B 05 ?? ?? ?? ?? 48 85 C0" ("??" or "?" = wildcard).
class Signature {
 public:
  // Strict: any invalid token fails the whole parse. Lenient: stop at the first invalid token and
  // keep the valid prefix. Either way, nullopt if zero valid tokens were parsed.
  static std::optional<Signature> parse(std::string_view pattern, bool lenient = false);

  std::size_t size() const {
    return bytes_.size();
  }
  bool empty() const {
    return bytes_.empty();
  }
  const std::vector<SigByte>& bytes() const {
    return bytes_;
  }

  // Does this pattern match at p (with `avail` bytes readable from p)?
  bool match_at(const std::uint8_t* p, std::size_t avail) const;

 private:
  std::vector<SigByte> bytes_;
};

// First offset in [data, data+len) where sig matches, or knpos.
std::size_t scan(const std::uint8_t* data, std::size_t len, const Signature& sig);

// Resolve a 32-bit RIP-relative displacement into an image-relative target offset.
// `disp_off` is where the 4-byte little-endian displacement sits; `next_insn_off` is the offset of
// the instruction's end (RIP at execution). Returns knpos if the read would run past the image.
std::size_t rip_target(const std::uint8_t* image, std::size_t image_len, std::size_t disp_off,
                       std::size_t next_insn_off);

// The offset of the vtable of the class whose RTTI type name is `type_name` (".?AVC_Foo@ns@@"),
// found as MSVC lays RTTI out on x64: the type descriptor holds the name at + 16; a complete
// object locator for the class's own vtable (signature 1, offset 0) names the descriptor's and its
// own offsets from the image; the vtable follows a pointer to that locator. `image_base` is the
// address the image's pointers assume (the loaded module's own address at runtime). knpos when
// there is none.
std::size_t rtti_vtable(const std::uint8_t* image, std::size_t image_len, std::uint64_t image_base,
                        std::string_view type_name);

}  // namespace kcd2s::bridge

#endif  // KCD2S_BRIDGE_SIGSCAN_HPP
