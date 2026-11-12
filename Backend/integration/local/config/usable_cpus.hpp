#pragma once
// The CPUs this process may use: the machine's hardware threads, fewer when the process is pinned
// to some of them (taskset, a cpuset) or its control group has a CPU quota (docker --cpus, a
// systemd CPUQuota, a hosting panel's limit). A worker pool sized for the whole machine overruns
// a quota, and the kernel then stops the whole process until the next quota period: ticks of
// tens of milliseconds while most of the machine is idle.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#if defined(__linux__)
#include <sched.h>
#endif

namespace kcd2s::local::config {

// The CPUs a cgroup v2 cpu.max line allows ("300000 100000" is 3); none for "max" (no quota) or a
// line that is not a quota.
inline std::optional<double> cpu_max_quota(std::string_view line) {
  std::istringstream in{std::string(line)};
  std::string quota;
  double period = 0;
  if (!(in >> quota >> period) || quota == "max" || period <= 0) {
    return std::nullopt;
  }
  char* end = nullptr;
  const double microseconds = std::strtod(quota.c_str(), &end);
  if (end == quota.c_str() || *end != '\0' || microseconds <= 0) {
    return std::nullopt;
  }
  return microseconds / period;
}

// The CPUs a cgroup v1 quota allows (cpu.cfs_quota_us over cpu.cfs_period_us; a quota of -1 is
// none).
inline std::optional<double> cfs_quota(double quota_us, double period_us) {
  if (quota_us <= 0 || period_us <= 0) {
    return std::nullopt;
  }
  return quota_us / period_us;
}

namespace detail {

inline std::optional<std::string> first_line(const std::string& file) {
  std::ifstream in(file);
  std::string line;
  if (!in || !std::getline(in, line)) {
    return std::nullopt;
  }
  return line;
}

inline void keep_tighter(std::optional<double>& tightest, std::optional<double> quota) {
  if (quota && (!tightest || *quota < *tightest)) {
    tightest = quota;
  }
}

}  // namespace detail

// The tightest cgroup v2 quota on the way from the process's group up to the hierarchy's root.
// `root`: where the hierarchy is mounted (/sys/fs/cgroup); `group`: the process's group as
// /proc/self/cgroup names it ("/system.slice/kcds.service"; "/" inside a container).
inline std::optional<double> cgroup_v2_quota(const std::string& root, std::string group) {
  std::optional<double> tightest;
  while (true) {
    const std::string folder = group == "/" ? root : root + group;
    if (const auto line = detail::first_line(folder + "/cpu.max")) {
      detail::keep_tighter(tightest, cpu_max_quota(*line));
    }
    if (group.empty() || group == "/") {
      break;
    }
    const auto slash = group.find_last_of('/');
    group = slash == 0 || slash == std::string::npos ? "/" : group.substr(0, slash);
  }
  return tightest;
}

// The process's CPU quota on Linux, cgroup v2 or v1; none elsewhere or without a quota.
inline std::optional<double> process_cpu_quota() {
#if defined(__linux__)
  std::ifstream groups("/proc/self/cgroup");
  std::string line;
  while (std::getline(groups, line)) {
    // cgroup v2: "0::/path".
    if (line.rfind("0::", 0) == 0) {
      return cgroup_v2_quota("/sys/fs/cgroup", line.substr(3));
    }
  }
  for (const char* folder : {"/sys/fs/cgroup/cpu", "/sys/fs/cgroup/cpu,cpuacct"}) {
    const auto quota = detail::first_line(std::string(folder) + "/cpu.cfs_quota_us");
    const auto period = detail::first_line(std::string(folder) + "/cpu.cfs_period_us");
    if (quota && period) {
      return cfs_quota(std::strtod(quota->c_str(), nullptr), std::strtod(period->c_str(), nullptr));
    }
  }
#endif
  return std::nullopt;
}

// The CPUs the process is pinned to; none where that cannot be read.
inline std::optional<unsigned> pinned_cpus() {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) == 0) {
    const int count = CPU_COUNT(&set);
    if (count > 0) {
      return static_cast<unsigned>(count);
    }
  }
#endif
  return std::nullopt;
}

// `hardware` threads, fewer when the process is pinned to `pinned` of them or has a quota of
// `quota` CPUs (2.5 CPUs count as 3: a partial CPU still runs a thread).
inline unsigned usable_cpus(unsigned hardware, std::optional<unsigned> pinned,
                            std::optional<double> quota) {
  unsigned usable = std::max(1u, hardware);
  if (pinned) {
    usable = std::min(usable, std::max(1u, *pinned));
  }
  if (quota) {
    usable = std::min(usable, std::max(1u, static_cast<unsigned>(std::ceil(*quota))));
  }
  return usable;
}

// The CPUs this process may use now.
inline unsigned usable_cpus() {
  return usable_cpus(std::thread::hardware_concurrency(), pinned_cpus(), process_cpu_quota());
}

}  // namespace kcd2s::local::config
