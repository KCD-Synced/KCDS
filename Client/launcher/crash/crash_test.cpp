// Parsing of the crash test switch; see kcds/launcher/crash_test.hpp.
#include "kcds/launcher/crash_test.hpp"

namespace kcds::launcher::crash {

CrashTest parse_crash_test(const std::string& value) {
  if (value == "launcher") {
    return CrashTest::Launcher;
  }
  if (value == "client") {
    return CrashTest::Client;
  }
  return CrashTest::None;
}

std::uint32_t parse_crash_test_delay(const std::string& value) {
  if (value.empty() || value.size() > 4) {
    return kDefaultCrashTestDelaySeconds;
  }
  std::uint32_t seconds = 0;
  for (char c : value) {
    if (c < '0' || c > '9') {
      return kDefaultCrashTestDelaySeconds;
    }
    seconds = seconds * 10 + static_cast<std::uint32_t>(c - '0');
  }
  if (seconds > kMaxCrashTestDelaySeconds) {
    return kDefaultCrashTestDelaySeconds;
  }
  return seconds;
}

const char* describe(CrashTest test) {
  switch (test) {
    case CrashTest::None:
      return "none";
    case CrashTest::Launcher:
      return "launcher";
    case CrashTest::Client:
      return "client";
  }
  return "none";
}

}  // namespace kcds::launcher::crash
