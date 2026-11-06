// Windows part of the crash test switch: the environment and the crash
// itself. See kcds/launcher/crash_test.hpp.
#include <windows.h>

#include <string>

#include "kcds/launcher/crash_test.hpp"

namespace kcds::launcher::crash {

namespace {

// The switch values are short ASCII words or numbers; anything longer is
// treated as unset.
std::string read_short_variable(const char* name) {
  char buffer[32];
  DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
  if (length == 0 || length >= sizeof(buffer)) {
    return std::string();
  }
  return std::string(buffer, length);
}

DWORD WINAPI delayed_crash(LPVOID parameter) {
  DWORD seconds = static_cast<DWORD>(reinterpret_cast<UINT_PTR>(parameter));
  Sleep(seconds * 1000);
  crash_now_for_test();
}

}  // namespace

CrashTest crash_test_from_environment() {
  return parse_crash_test(read_short_variable(kCrashTestVariable));
}

std::uint32_t crash_test_delay_from_environment() {
  return parse_crash_test_delay(read_short_variable(kCrashTestDelayVariable));
}

__declspec(noinline) void crash_now_for_test() {
  volatile int* target = nullptr;
  *target = 0x4B434453;  // "KCDS"
  // Not reached; keeps [[noreturn]] honest if the write were ever removed.
  TerminateProcess(GetCurrentProcess(), 0xC0000005);
  for (;;) {
  }
}

bool schedule_crash_for_test(std::uint32_t seconds) {
  HANDLE thread =
      CreateThread(nullptr, 0, delayed_crash,
                   reinterpret_cast<LPVOID>(static_cast<UINT_PTR>(seconds)), 0, nullptr);
  if (thread == nullptr) {
    return false;
  }
  CloseHandle(thread);
  return true;
}

}  // namespace kcds::launcher::crash
