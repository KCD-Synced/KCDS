// Single-instance check of KingdomCome.exe, patched in memory; see multiple_instances.hpp.
//
// KingdomCome.exe 1.5.5 (Win64MasterMasterSteamPGO), file offset 0x1FB9:
//
//   FF 15 xx xx xx xx    call [CreateMutexA]
//   FF 15 xx xx xx xx    call [GetLastError]
//   3D B7 00 00 00       cmp  eax, ERROR_ALREADY_EXISTS
//   0F 85 rel32          jne  continue_startup      <- becomes  E9 rel32+1 / 90  (jmp, nop)
//   ...                  "already running" path, which quits
//
// The jump target stays the same: the 5-byte jmp ends one byte earlier than the 6-byte jne, so its
// displacement is one larger.
#include "multiple_instances.hpp"

#include <Windows.h>

#include <cstdint>
#include <cstring>

#include "kcd2s/bridge/sigscan.hpp"

namespace kcds::runtime::instances {
namespace {

namespace bridge = kcd2s::bridge;

constexpr const char* kCheckSignature =
    "FF 15 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 3D B7 00 00 00 0F 85 ?? ?? ?? ??";
// Offset of the jne (0F 85 rel32) inside the signature.
constexpr std::size_t kJumpOffset = 17;
constexpr std::size_t kJumpSize = 6;

}  // namespace

bool allow_multiple_instances(Logger logger) {
  auto* image = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);

  const auto signature = bridge::Signature::parse(kCheckSignature);
  if (!signature) {
    logger("INSTANCES signature does not parse");
    return false;
  }

  // Search the code sections only, and require exactly one match across all of them.
  std::uint8_t* match = nullptr;
  int matches = 0;
  const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
  for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
    if ((section->Characteristics & IMAGE_SCN_CNT_CODE) == 0) {
      continue;
    }
    std::uint8_t* start = image + section->VirtualAddress;
    std::size_t length = section->Misc.VirtualSize;
    while (length >= signature->size()) {
      const std::size_t found = bridge::scan(start, length, *signature);
      if (found == bridge::knpos) {
        break;
      }
      match = start + found;
      ++matches;
      start += found + 1;
      length -= found + 1;
    }
  }
  if (matches == 0) {
    logger("INSTANCES single-instance check not found; the game allows one instance");
    return false;
  }
  if (matches > 1) {
    logger("INSTANCES single-instance check found %d times; left unchanged", matches);
    return false;
  }

  std::uint8_t* jump = match + kJumpOffset;
  std::int32_t displacement = 0;
  std::memcpy(&displacement, jump + 2, sizeof(displacement));
  std::uint8_t patched[kJumpSize];
  patched[0] = 0xE9;
  const std::int32_t jmp_displacement = displacement + 1;
  std::memcpy(patched + 1, &jmp_displacement, sizeof(jmp_displacement));
  patched[5] = 0x90;

  DWORD old_protection = 0;
  if (!VirtualProtect(jump, kJumpSize, PAGE_EXECUTE_READWRITE, &old_protection)) {
    logger("INSTANCES VirtualProtect failed (%lu); the game allows one instance", GetLastError());
    return false;
  }
  std::memcpy(jump, patched, kJumpSize);
  DWORD ignored = 0;
  VirtualProtect(jump, kJumpSize, old_protection, &ignored);
  FlushInstructionCache(GetCurrentProcess(), jump, kJumpSize);
  logger("INSTANCES single-instance check bypassed in memory at exe+0x%zx",
         static_cast<std::size_t>(jump - image));
  return true;
}

}  // namespace kcds::runtime::instances
