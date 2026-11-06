#pragma once
// Windows process CPU time and memory sampling for the opt-in server metrics and the load probe.
// Each process reports its own usage so a saturated load generator is not mistaken for the server.
#ifndef NOMINMAX
#define NOMINMAX
#endif
// winsock2.h must precede Windows.h so the legacy winsock.h declarations are never pulled in.
#include <winsock2.h>
#include <Windows.h>
#include <psapi.h>
#include <chrono>
#include <cstdint>

namespace kcd2s::local::metrics {

struct ProcessUsage {
  double wall_seconds = 0.0;
  double cpu_seconds = 0.0;  // user + kernel time of all threads of this process
  std::uint64_t working_set_bytes = 0;
  std::uint64_t peak_working_set_bytes = 0;
};

inline double filetime_seconds(const FILETIME& value) {
  ULARGE_INTEGER ticks;
  ticks.LowPart = value.dwLowDateTime;
  ticks.HighPart = value.dwHighDateTime;
  return static_cast<double>(ticks.QuadPart) / 1.0e7;
}

inline ProcessUsage sample_process_usage() {
  ProcessUsage usage;
  usage.wall_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
    usage.cpu_seconds = filetime_seconds(kernel) + filetime_seconds(user);
  }
  PROCESS_MEMORY_COUNTERS memory{};
  memory.cb = sizeof(memory);
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) {
    usage.working_set_bytes = memory.WorkingSetSize;
    usage.peak_working_set_bytes = memory.PeakWorkingSetSize;
  }
  return usage;
}

// CPU use between two samples as a percentage of ONE logical core (100 = one core saturated).
inline double cpu_percent_of_one_core(const ProcessUsage& earlier, const ProcessUsage& later) {
  const double wall = later.wall_seconds - earlier.wall_seconds;
  if (wall <= 0.0) {
    return 0.0;
  }
  return 100.0 * (later.cpu_seconds - earlier.cpu_seconds) / wall;
}

inline unsigned logical_core_count() {
  return static_cast<unsigned>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
}

}  // namespace kcd2s::local::metrics
