// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "load_progress.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

#include "MinHook.h"

namespace kcds::runtime::session::load_progress {
namespace {

using SetProgress = void(__fastcall*)(void* section, float value);

constexpr float kSections = 14.0f;
constexpr int kMostBeforeReady = 95;

SetProgress original_set = nullptr;
std::mutex state_mutex;
std::vector<void*> seen;
int newest = -1;
int reported = -1;

void __fastcall hooked_set(void* section, float value) {
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (std::find(seen.begin(), seen.end(), section) == seen.end()) {
      seen.push_back(section);
    }
    // A section gone back to after the newest one (the load's last steps) adds nothing.
    if (seen.back() == section) {
      const float whole = (float(seen.size() - 1) + (std::clamp)(value, 0.0f, 1.0f)) / kSections;
      newest = (std::max)(newest, (std::min)(int(whole * 100.0f), kMostBeforeReady));
    }
  }
  original_set(section, value);
}

}  // namespace

bool install(void* section_set_progress, Logger logger) {
  const bool hooked_it = section_set_progress &&
                         MH_CreateHook(section_set_progress, reinterpret_cast<void*>(&hooked_set),
                                       reinterpret_cast<void**>(&original_set)) == MH_OK &&
                         MH_EnableHook(section_set_progress) == MH_OK;
  if (logger) {
    logger("LOADING progress hook %s", hooked_it ? "installed" : "unavailable");
  }
  return hooked_it;
}

void reset() {
  std::lock_guard<std::mutex> lock(state_mutex);
  seen.clear();
  newest = -1;
  reported = -1;
}

int take_changed() {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (newest < 0 || newest == reported) {
    return -1;
  }
  reported = newest;
  return newest;
}

}  // namespace kcds::runtime::session::load_progress
