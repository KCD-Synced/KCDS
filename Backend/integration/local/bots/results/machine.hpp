#pragma once
// The machine the bots ran on, for the result: its CPU's name, hardware threads and memory.
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
#include "../../udp_win.hpp"
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#else
#include <unistd.h>
#endif

namespace kcd2s::local::bots {

struct MachineInfo {
  std::string cpu;
  unsigned threads = 0;
  double ram_gb = 0.0;
  std::string system;
};

inline MachineInfo this_machine() {
  MachineInfo info;
  info.threads = std::thread::hardware_concurrency();
#ifdef _WIN32
  info.system = "windows";
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  if (GlobalMemoryStatusEx(&memory)) {
    info.ram_gb = double(memory.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
  }
  char name[256] = {};
  DWORD size = sizeof(name);
  if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                   "ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS) {
    info.cpu = name;
  }
#elif defined(__APPLE__)
  info.system = "macos";
  std::uint64_t memory = 0;
  std::size_t size = sizeof(memory);
  if (sysctlbyname("hw.memsize", &memory, &size, nullptr, 0) == 0) {
    info.ram_gb = double(memory) / (1024.0 * 1024.0 * 1024.0);
  }
  char name[256] = {};
  size = sizeof(name);
  if (sysctlbyname("machdep.cpu.brand_string", name, &size, nullptr, 0) == 0) {
    info.cpu = name;
  }
#else
  info.system = "linux";
  const long pages = sysconf(_SC_PHYS_PAGES);
  const long page = sysconf(_SC_PAGE_SIZE);
  if (pages > 0 && page > 0) {
    info.ram_gb = double(pages) * double(page) / (1024.0 * 1024.0 * 1024.0);
  }
  std::ifstream cpuinfo("/proc/cpuinfo");
  std::string line;
  while (std::getline(cpuinfo, line)) {
    if (line.rfind("model name", 0) == 0) {
      const std::size_t colon = line.find(':');
      if (colon != std::string::npos) {
        info.cpu = line.substr(colon + 2);
      }
      break;
    }
  }
#endif
  // Trailing spaces some CPUs report.
  while (!info.cpu.empty() && info.cpu.back() == ' ') {
    info.cpu.pop_back();
  }
  return info;
}

}  // namespace kcd2s::local::bots
