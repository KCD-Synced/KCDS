#include "natives.hpp"

#include <cstring>

#include "kcd2s/bridge/sigscan.hpp"
#include "log.hpp"
#include "signature_store.hpp"

namespace kcds::game_client {

namespace bridge = kcd2s::bridge;
std::uint8_t* image;
std::size_t image_size;
void* resolve_unique(const char* name, const char* pattern, int displacement, int length) {
  auto sig = bridge::Signature::parse(pattern);
  if (!sig)
    return nullptr;
  // Found at an earlier start of this same game and client, and those bytes still match it there:
  // a function hooked since then is no longer found, as a scan would not find it.
  const auto known = known_signature(pattern);
  const bool still =
      known && *known < image_size && sig->match_at(image + *known, image_size - *known);
  count_signature(still);
  std::size_t first = bridge::knpos;
  if (still) {
    first = *known;
  } else {
    first = bridge::scan(image, image_size, *sig);
    if (first == bridge::knpos)
      return nullptr;
    auto second = bridge::scan(image + first + 1, image_size - first - 1, *sig);
    if (second != bridge::knpos) {
      log("Ambiguous signature: %s", name);
      return nullptr;
    }
    remember_signature(pattern, first);
  }
  // The match is the function entry, or the instruction whose RIP-relative operand names the
  // target (as bridge::Resolver resolves it, without scanning the module a third time).
  if (displacement < 0)
    return image + first;
  const auto target =
      bridge::rip_target(image, image_size, first + std::size_t(displacement), first + length);
  return target == bridge::knpos ? nullptr : image + target;
}
// The match of a signature whose RIP-relative operand (rel32 at + displacement, the instruction
// ending at + length) is the vtable of the RTTI class `type_name` (".?AVC_...@@"); functions the
// compiler made identical but for their vtable share every other byte. Its own address, or (with
// `vtable`) that vtable; null when no match or more than one has that class.
void* resolve_by_vtable_class(const char* name, const char* pattern, int displacement, int length,
                              const char* type_name, bool vtable) {
  auto sig = bridge::Signature::parse(pattern);
  if (!sig) {
    return nullptr;
  }
  void* found = nullptr;
  std::size_t cursor = 0;
  while (cursor < image_size) {
    const auto offset = bridge::scan(image + cursor, image_size - cursor, *sig);
    if (offset == bridge::knpos) {
      break;
    }
    const std::size_t at = cursor + offset;
    cursor = at + 1;
    const auto target = bridge::rip_target(image, image_size, at + std::size_t(displacement),
                                           at + std::size_t(length));
    if (target == bridge::knpos || target < 8) {
      continue;
    }
    const auto* table = image + target;
    const auto* locator = *reinterpret_cast<const std::uint8_t* const*>(table - 8);
    const auto base = reinterpret_cast<std::uintptr_t>(image);
    const auto address = reinterpret_cast<std::uintptr_t>(locator);
    if (address < base || address + 16 > base + image_size ||
        *reinterpret_cast<const std::uint32_t*>(locator) != 1) {
      continue;
    }
    const auto descriptor = *reinterpret_cast<const std::uint32_t*>(locator + 12);
    if (descriptor + 16 >= image_size) {
      continue;
    }
    const auto* type = reinterpret_cast<const char*>(image + descriptor + 16);
    if (strncmp(type, type_name, 128) != 0) {
      continue;
    }
    if (found) {
      log("Ambiguous class signature: %s", name);
      return nullptr;
    }
    found = vtable ? static_cast<void*>(image + target) : static_cast<void*>(image + at);
  }
  return found;
}
// The match of a signature whose call (rel32 at + displacement, the instruction ending at +
// length) goes to `callee`: factories the compiler made identical but for the constructor they
// call. Null when no match or more than one calls it.
void* resolve_by_callee(const char* name, const char* pattern, int displacement, int length,
                        const void* callee) {
  auto sig = bridge::Signature::parse(pattern);
  if (!callee || !sig) {
    return nullptr;
  }
  void* found = nullptr;
  std::size_t cursor = 0;
  while (cursor < image_size) {
    const auto offset = bridge::scan(image + cursor, image_size - cursor, *sig);
    if (offset == bridge::knpos) {
      break;
    }
    const std::size_t at = cursor + offset;
    cursor = at + 1;
    const auto target = bridge::rip_target(image, image_size, at + std::size_t(displacement),
                                           at + std::size_t(length));
    if (target == bridge::knpos || image + target != callee) {
      continue;
    }
    if (found) {
      log("Ambiguous callee signature: %s", name);
      return nullptr;
    }
    found = image + at;
  }
  return found;
}

void* get_entity(void* system, std::uint32_t id) {
  return slot<void*(__fastcall*)(void*, std::uint32_t)>(system, 14)(system, id);
}

}  // namespace kcds::game_client
