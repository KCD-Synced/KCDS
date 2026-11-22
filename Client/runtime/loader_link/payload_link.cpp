#include "payload_link.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

#include "kcds/loader/kcds_abi.h"

namespace kcds::runtime::loader_link {
namespace {

Logger log_line = nullptr;
std::mutex steps_mutex;
std::vector<std::pair<std::string, std::function<void()>>> steps;

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
  std::vector<std::pair<std::string, std::function<void()>>> taken;
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
    it->second();
  }
  if (log_line) {
    log_line("PAYLOAD ready to be unloaded");
  }
  return 0;
}

const KcdsPayloadApi payload_api = {KCDS_LOADER_API_VERSION, &prepare_unload};

}  // namespace

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

void on_unload(const char* name, std::function<void()> step) {
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
