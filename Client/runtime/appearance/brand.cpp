#include "brand.hpp"

#include <cstddef>
#include <cstring>

namespace kcds::runtime::appearance::brand {
namespace {

// The human's "show this variant of a part": vtable + 248 (part name, variant list).
constexpr std::size_t kShowVariant = 248 / sizeof(void*);
constexpr std::size_t kActorHuman = 1672;
// Where loads them: each lea's offset in the function; its rip-relative target ends
// the seven-byte instruction.
constexpr std::size_t kNoneLea = 0xA9;
constexpr std::size_t kFirstLea = 0x86;
constexpr std::size_t kSecondLea = 0x62;
constexpr std::size_t kPartLea = 0xB3;
constexpr std::size_t kLeaLength = 7;
constexpr std::size_t kLeaDisplacement = 3;

using ShowVariant = void(__fastcall*)(void* human, const void* part, const void* variants);

const void* part = nullptr;
const void* variants[3] = {};

const void* lea_target(const std::uint8_t* function, std::size_t offset) {
  std::int32_t displacement = 0;
  std::memcpy(&displacement, function + offset + kLeaDisplacement, sizeof(displacement));
  return function + offset + kLeaLength + displacement;
}

}  // namespace

bool install(void* branding_state, Logger logger) {
  if (!branding_state) {
    if (logger) {
      logger("APPEARANCE brand: the game's function was not found, bodies show no brand");
    }
    return false;
  }
  const auto* function = static_cast<const std::uint8_t*>(branding_state);
  part = lea_target(function, kPartLea);
  variants[0] = lea_target(function, kNoneLea);
  variants[1] = lea_target(function, kFirstLea);
  variants[2] = lea_target(function, kSecondLea);
  return true;
}

bool show(void* actor, std::uint8_t state) {
  if (!part || !actor || state > 2) {
    return false;
  }
  void* human = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + kActorHuman);
  if (!human) {
    return false;
  }
  void** vtable = *static_cast<void***>(human);
  reinterpret_cast<ShowVariant>(vtable[kShowVariant])(human, part, variants[state]);
  return true;
}

}  // namespace kcds::runtime::appearance::brand
