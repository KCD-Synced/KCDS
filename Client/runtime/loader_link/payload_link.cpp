#include "payload_link.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

#include <psapi.h>

#include <cstring>

#include "kcd2s/bridge/sigscan.hpp"
#include "kcds/loader/kcds_abi.h"

namespace kcds::runtime::loader_link {
namespace {

Logger log_line = nullptr;
std::atomic<bool> stop_requested{false};
std::mutex threads_mutex;
std::condition_variable threads_ended;
// A running thread of the client: its name, and its handle for cancelling a blocking read.
struct RunningThread {
  const char* name = nullptr;
  HANDLE handle = nullptr;
};
std::vector<RunningThread> running_threads;
// How long the client's threads get to end before the unload goes on without them.
constexpr auto kThreadsWait = std::chrono::seconds(10);
std::mutex stops_mutex;
std::vector<std::pair<std::string, std::function<void()>>> stops;
std::mutex steps_mutex;
std::vector<std::pair<std::string, std::function<bool()>>> steps;

const KcdsLoaderApi* loader() {
  static const KcdsLoaderApi* api = [] {
    HMODULE module = GetModuleHandleW(KCDS_LOADER_MODULE);
    auto get = module ? reinterpret_cast<KcdsGetLoaderApi>(
                            reinterpret_cast<void*>(GetProcAddress(module, KCDS_LOADER_EXPORT)))
                      : nullptr;
    // The swap needs the loader's version 2; an older loader only hooks.
    return get ? get(2) : nullptr;
  }();
  return api;
}

int prepare_unload() {
  // The client's threads first: each loop ends at its next turn.
  stop_requested.store(true);
  {
    std::vector<std::pair<std::string, std::function<void()>>> wakes;
    {
      std::lock_guard<std::mutex> lock(stops_mutex);
      wakes = stops;
    }
    for (const auto& [name, wake] : wakes) {
      if (log_line) {
        log_line("PAYLOAD waking: %s", name.c_str());
      }
      wake();
    }
  }
  {
    // A thread blocked in a synchronous read (a pipe that does not answer) never sees the stop:
    // its read is cancelled, again and again while it runs, and it then ends at its loop.
    std::unique_lock<std::mutex> lock(threads_mutex);
    const auto deadline = std::chrono::steady_clock::now() + kThreadsWait;
    while (!running_threads.empty() && std::chrono::steady_clock::now() < deadline) {
      for (const RunningThread& thread : running_threads) {
        CancelSynchronousIo(thread.handle);
      }
      threads_ended.wait_for(lock, std::chrono::milliseconds(100),
                             [] { return running_threads.empty(); });
    }
    if (log_line) {
      if (running_threads.empty()) {
        log_line("PAYLOAD every thread of the client ended");
      }
      for (const RunningThread& thread : running_threads) {
        log_line("PAYLOAD thread still running: %s", thread.name);
      }
    }
  }
  std::vector<std::pair<std::string, std::function<bool()>>> taken;
  {
    std::lock_guard<std::mutex> lock(steps_mutex);
    taken = steps;
  }
  if (log_line) {
    log_line("PAYLOAD unloading: %zu steps", taken.size());
  }
  for (auto it = taken.rbegin(); it != taken.rend(); ++it) {
    if (log_line) {
      log_line("PAYLOAD unload step: %s", it->first.c_str());
    }
    if (!it->second()) {
      if (log_line) {
        log_line("PAYLOAD unload step failed: %s; the client keeps running", it->first.c_str());
      }
      return 1;
    }
  }
  if (log_line) {
    log_line("PAYLOAD ready to be unloaded");
  }
  return 0;
}

const KcdsPayloadApi payload_api = {KCDS_LOADER_API_VERSION, &prepare_unload};

}  // namespace

bool stopping() {
  return stop_requested.load();
}

ThreadScope::ThreadScope(const char* name) : name_(name) {
  enter_thread(name_);
}

ThreadScope::~ThreadScope() {
  leave_thread(name_);
}

void enter_thread(const char* name) {
  RunningThread thread;
  thread.name = name;
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread.handle,
                  THREAD_TERMINATE, FALSE, 0);
  std::lock_guard<std::mutex> lock(threads_mutex);
  running_threads.push_back(thread);
}

void leave_thread(const char* name) {
  {
    std::lock_guard<std::mutex> lock(threads_mutex);
    for (auto it = running_threads.begin(); it != running_threads.end(); ++it) {
      if (it->name == name) {
        if (it->handle) {
          CloseHandle(it->handle);
        }
        running_threads.erase(it);
        break;
      }
    }
  }
  threads_ended.notify_all();
}

bool register_payload(void* module, Logger logger) {
  log_line = logger;
  if (!loader()) {
    if (log_line) {
      log_line("PAYLOAD no loader with swaps: this client cannot be swapped");
    }
    return false;
  }
  loader()->register_payload(module, &payload_api);
  if (log_line) {
    log_line("PAYLOAD registered with the loader");
  }
  return true;
}

void on_stop(const char* name, std::function<void()> wake) {
  std::lock_guard<std::mutex> lock(stops_mutex);
  stops.emplace_back(name, std::move(wake));
}

void on_unload(const char* name, std::function<bool()> step) {
  std::lock_guard<std::mutex> lock(steps_mutex);
  steps.emplace_back(name, std::move(step));
}

bool request_swap(const std::wstring& path, const std::string& join) {
  if (!loader()) {
    return false;
  }
  const bool accepted = loader()->request_swap(path.c_str(), join.c_str()) == 0;
  if (log_line) {
    log_line("PAYLOAD swap %s", accepted ? "requested" : "refused by the loader");
  }
  return accepted;
}

void prepare_scan_view(Logger logger) {
  if (!loader() || loader()->version < 3 || !loader()->restore_original_bytes) {
    return;
  }
  HMODULE game = GetModuleHandleW(L"WHGame.dll");
  MODULEINFO info{};
  if (!game || !GetModuleInformation(GetCurrentProcess(), game, &info, sizeof(info))) {
    return;
  }
  const auto* image = static_cast<const std::uint8_t*>(info.lpBaseOfDll);
  const std::size_t size = info.SizeOfImage;
  auto* copy = new std::uint8_t[size]();
  // Only what is mapped readable is copied; the rest stays zero (no signature lies there).
  constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                              PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
  std::size_t at = 0;
  while (at < size) {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(image + at, &region, sizeof(region)) == 0) {
      break;
    }
    const auto* region_end =
        static_cast<const std::uint8_t*>(region.BaseAddress) + region.RegionSize;
    const std::size_t end = region_end - image < static_cast<std::ptrdiff_t>(size)
                                ? std::size_t(region_end - image)
                                : size;
    if (region.State == MEM_COMMIT && (region.Protect & kReadable) &&
        !(region.Protect & PAGE_GUARD)) {
      std::memcpy(copy + at, image + at, end - at);
    }
    at = end > at ? end : size;
  }
  const std::uint32_t restored = loader()->restore_original_bytes(image, size, copy);
  if (restored == 0) {
    delete[] copy;
    return;
  }
  // Kept while the client runs (later scans read it); freed before the client is unloaded.
  kcd2s::bridge::set_scan_view(image, size, copy);
  on_unload("signature view", [copy]() {
    kcd2s::bridge::set_scan_view(nullptr, 0, nullptr);
    delete[] copy;
    return true;
  });
  if (logger) {
    logger("PAYLOAD signatures read the game's own bytes (%u hooked places written back)",
           unsigned(restored));
  }
}

bool hook_pointer(void** where, void* detour, void** original) {
  if (loader() && loader()->version >= 6 && loader()->hook_pointer) {
    return loader()->hook_pointer(where, detour, original) == 0;
  }
  DWORD old = 0;
  if (!VirtualProtect(where, sizeof(void*), PAGE_READWRITE, &old)) {
    return false;
  }
  if (original) {
    *original = *where;
  }
  *where = detour;
  DWORD unused = 0;
  VirtualProtect(where, sizeof(void*), old, &unused);
  return true;
}

const KcdsLoaderApi* loader_api(std::uint32_t min_version) {
  const KcdsLoaderApi* api = loader();
  return api && api->version >= min_version ? api : nullptr;
}

bool can_swap() {
  return loader() && loader()->version >= 5 && loader()->request_swap;
}

bool loaded_by_swap() {
  // Read once, at the client's start: later swaps unload this client.
  static const bool swapped =
      loader() && loader()->version >= 5 && loader()->swaps && loader()->swaps() > 0;
  return swapped;
}

std::string take_join() {
  const char* join = loader() ? loader()->take_join() : nullptr;
  return join ? join : "";
}

void frame() {
  static const bool hooks = GetEnvironmentVariableW(L"KCDS_TEST_HOOKS", nullptr, 0) > 0;
  if (!hooks) {
    return;
  }
  wchar_t folder[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, folder);
  wchar_t file[MAX_PATH] = {};
  swprintf_s(file, L"%skcds-swap-%lu.txt", folder, GetCurrentProcessId());
  FILE* in = nullptr;
  if (_wfopen_s(&in, file, L"rb") != 0 || !in) {
    return;
  }
  char text[600] = {};
  const std::size_t length = std::fread(text, 1, sizeof(text) - 1, in);
  std::fclose(in);
  DeleteFileW(file);
  std::string line(text, length);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
    line.pop_back();
  }
  const std::size_t bar = line.find('|');
  const std::string path = line.substr(0, bar);
  const std::string join = bar == std::string::npos ? "" : line.substr(bar + 1);
  std::wstring wide(path.begin(), path.end());
  request_swap(wide, join);
}

}  // namespace kcds::runtime::loader_link
