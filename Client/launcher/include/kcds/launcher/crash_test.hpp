// A controlled crash for verifying crash reporting end to end.
//
// Set the environment variable KCDS_CRASH_TEST before starting the launcher
// (players never set it; it is not in any shipped configuration):
//
//   KCDS_CRASH_TEST=launcher  the launcher crashes right after crash
//                             reporting started (before any update or game
//                             start), with its log attached;
//   KCDS_CRASH_TEST=client    the game-client DLL crashes on a thread of its
//                             own KCDS_CRASH_TEST_DELAY seconds (default 60,
//                             at most 600) after crash reporting started in
//                             the game, with client-<pid>.log attached.
//
// The client value reaches the game through the environment the launcher
// hands down (kcd2s-launch-client.exe -> the game). Nothing crashes unless
// crash reporting actually started, so the switch cannot crash a game that
// would not report it, and any other value is ignored.
#ifndef KCDS_LAUNCHER_CRASH_TEST_HPP
#define KCDS_LAUNCHER_CRASH_TEST_HPP

#include <cstdint>
#include <string>

namespace kcds::launcher::crash {

inline constexpr const char* kCrashTestVariable = "KCDS_CRASH_TEST";
inline constexpr const char* kCrashTestDelayVariable = "KCDS_CRASH_TEST_DELAY";
inline constexpr std::uint32_t kDefaultCrashTestDelaySeconds = 60;
inline constexpr std::uint32_t kMaxCrashTestDelaySeconds = 600;

enum class CrashTest {
  None,
  Launcher,
  Client,
};

// "launcher" or "client" (exactly, lowercase); anything else is None.
CrashTest parse_crash_test(const std::string& value);

// Seconds from KCDS_CRASH_TEST_DELAY: decimal digits up to the maximum;
// anything else gives the default.
std::uint32_t parse_crash_test_delay(const std::string& value);

const char* describe(CrashTest test);

#ifdef _WIN32
CrashTest crash_test_from_environment();
std::uint32_t crash_test_delay_from_environment();

// Crashes this process now with an access violation (a null write in a
// function of its own, so the crashing frame is ours).
[[noreturn]] void crash_now_for_test();

// Starts a detached thread that calls crash_now_for_test() after seconds.
// Returns false when the thread could not be started.
bool schedule_crash_for_test(std::uint32_t seconds);
#endif

}  // namespace kcds::launcher::crash

#endif  // KCDS_LAUNCHER_CRASH_TEST_HPP
