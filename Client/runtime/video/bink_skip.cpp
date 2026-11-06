#include "bink_skip.hpp"

#include <windows.h>

#include "MinHook.h"

namespace kcds::runtime::video::bink_skip {

namespace {

// bink2w64.dll: S32 BinkShouldSkip(HBINK bink), non-zero when the video is late and the caller
// should skip the frame.
using ShouldSkip = int(__stdcall*)(void* bink);

ShouldSkip original_should_skip = nullptr;

int __stdcall hooked_should_skip(void* /*bink*/) {
  return 0;
}

}  // namespace

bool install(Logger logger) {
  const auto status = MH_Initialize();
  if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
    return false;
  }
  const HMODULE bink = GetModuleHandleW(L"bink2w64.dll");
  void* target = bink ? reinterpret_cast<void*>(GetProcAddress(bink, "BinkShouldSkip")) : nullptr;
  if (!target) {
    if (logger) {
      logger("VIDEO frame skip hook unavailable: bink2w64.dll has no BinkShouldSkip");
    }
    return false;
  }
  if (MH_CreateHook(target, reinterpret_cast<void*>(&hooked_should_skip),
                    reinterpret_cast<void**>(&original_should_skip)) != MH_OK ||
      MH_EnableHook(target) != MH_OK) {
    if (logger) {
      logger("VIDEO frame skip hook could not be installed");
    }
    return false;
  }
  if (logger) {
    logger("VIDEO frame skip off: late videos show their next frame");
  }
  return true;
}

}  // namespace kcds::runtime::video::bink_skip
