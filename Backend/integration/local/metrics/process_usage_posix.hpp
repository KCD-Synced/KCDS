#pragma once
// Process CPU time and memory for the opt-in server metrics on Linux and macOS: the same
// interface as process_usage_win.hpp, from getrusage and /proc/self/status (on macOS the task's
// resident size, which has no /proc).
#ifdef _WIN32
#error "process_usage_posix.hpp is for POSIX systems; Windows uses process_usage_win.hpp"
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>

namespace kcd2s::local::metrics {

struct ProcessUsage {
  double wall_seconds = 0.0;
  double cpu_seconds = 0.0;  // user + kernel time of all threads of this process
  std::uint64_t working_set_bytes = 0;
  std::uint64_t peak_working_set_bytes = 0;
};

// A "VmRSS:   1234 kB" style line of /proc/self/status, in bytes (0 when missing).
inline std::uint64_t status_kilobytes(const std::string& name) {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind(name + ":", 0) == 0) {
      return std::stoull(line.substr(name.size() + 1)) * 1024u;
    }
  }
  return 0;
}

inline ProcessUsage sample_process_usage() {
  ProcessUsage usage;
  usage.wall_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  rusage self{};
  if (getrusage(RUSAGE_SELF, &self) == 0) {
    usage.cpu_seconds = double(self.ru_utime.tv_sec) + double(self.ru_utime.tv_usec) / 1.0e6 +
                        double(self.ru_stime.tv_sec) + double(self.ru_stime.tv_usec) / 1.0e6;
  }
#ifdef __APPLE__
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                &count) == KERN_SUCCESS) {
    usage.working_set_bytes = info.resident_size;
    usage.peak_working_set_bytes = info.resident_size_max;
  }
#else
  usage.working_set_bytes = status_kilobytes("VmRSS");
  usage.peak_working_set_bytes = status_kilobytes("VmHWM");
#endif
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
  const long count = sysconf(_SC_NPROCESSORS_ONLN);
  return count > 0 ? static_cast<unsigned>(count) : 1u;
}

}  // namespace kcd2s::local::metrics
