#include "scripted_input.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#include "../core/natives.hpp"

namespace kcds::runtime::input::scripted {
namespace {

// The action ids searched: the registry's first entries after its vtable.
constexpr std::size_t kFirstAction = 8;
constexpr std::size_t kLastAction = 504;

struct Step {
  double at = 0;
  std::string action;
  int mode = 0;
  float value = 0;
};

Logger log_line = nullptr;
HANDLE signal = nullptr;
std::deque<Step> steps;

std::string script_path() {
  wchar_t temp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, temp);
  char path[MAX_PATH * 2] = {};
  std::snprintf(path, sizeof(path), "%lskcds-input-%lu.txt", temp, GetCurrentProcessId());
  return path;
}

// The steps of the script file, timed from now.
void load(double now) {
  steps.clear();
  std::ifstream file(script_path());
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream fields(line);
    Step step;
    double seconds = 0;
    if (fields >> seconds >> step.action >> step.mode >> step.value) {
      step.at = now + seconds;
      steps.push_back(step);
    }
  }
  if (log_line) {
    log_line("INPUT script steps=%zu", steps.size());
  }
}

// A game action kept outside the registry, in a static id of its own (trigger_use: the seats',
// beds' and other triggers' use, in WHGame 1.5.5): its initializer passes the name to
// the string constructor, "lea rdx, [name]" then "lea rcx, [the id]". The name's
// first copy in the image, and the id that initializer fills; nullptr when there is none.
// Searched once per name.
const char** static_action_named(const std::string& name) {
  static std::unordered_map<std::string, const char**> found;
  const auto known = found.find(name);
  if (known != found.end()) {
    return known->second;
  }
  const char** result = nullptr;
  std::uint8_t* image = kcds::game_client::image;
  const std::size_t size = kcds::game_client::image_size;
  const std::size_t length = name.size() + 1;
  std::uint8_t* text = nullptr;
  for (std::size_t at = 1; image && at + length < size && !text; ++at) {
    if (image[at - 1] == 0 && std::memcmp(image + at, name.c_str(), length) == 0) {
      text = image + at;
    }
  }
  for (std::size_t at = 0; text && at + 14 < size && !result; ++at) {
    const std::uint8_t* code = image + at;
    if (code[0] != 0x48 || code[1] != 0x8D || code[2] != 0x15) {
      continue;
    }
    std::int32_t to_text = 0;
    std::memcpy(&to_text, code + 3, 4);
    if (code + 7 + to_text != text || code[7] != 0x48 || code[8] != 0x8D || code[9] != 0x0D) {
      continue;
    }
    std::int32_t to_id = 0;
    std::memcpy(&to_id, code + 10, 4);
    result = reinterpret_cast<const char**>(const_cast<std::uint8_t*>(code + 14 + to_id));
  }
  if (result && (!*result || name != *result)) {
    result = nullptr;
  }
  found[name] = result;
  return result;
}

}  // namespace

const char** action_named(const std::uint8_t* registry, const std::string& name) {
  for (std::size_t offset = kFirstAction; offset <= kLastAction; offset += 8) {
    auto id = reinterpret_cast<const char**>(const_cast<std::uint8_t*>(registry) + offset);
    if (*id && name == *id) {
      return id;
    }
  }
  return nullptr;
}

bool install(Logger logger) {
  log_line = logger;
  if (GetEnvironmentVariableW(L"KCDS_TEST_HOOKS", nullptr, 0) == 0) {
    return false;
  }
  wchar_t name[96];
  swprintf_s(name, L"Local\\KCD2S.InputScript.%lu", GetCurrentProcessId());
  signal = CreateEventW(nullptr, FALSE, FALSE, name);
  return signal != nullptr;
}

void tick(const std::uint8_t* registry, void* input, double now, Deliver deliver, Special special) {
  if (signal && WaitForSingleObject(signal, 0) == WAIT_OBJECT_0) {
    load(now);
  }
  if (!registry || !input) {
    return;
  }
  while (!steps.empty() && steps.front().at <= now) {
    const Step step = steps.front();
    steps.pop_front();
    const char** action = action_named(registry, step.action);
    if (!action) {
      action = static_action_named(step.action);
    }
    if (!action && special && special(step.action.c_str(), unsigned(step.mode))) {
      if (log_line) {
        log_line("INPUT script %s mode=%d (the client's own)", step.action.c_str(), step.mode);
      }
      continue;
    }
    if (!action) {
      if (log_line) {
        log_line("INPUT script has no action %s", step.action.c_str());
      }
      continue;
    }
    deliver(input, action, unsigned(step.mode), step.value);
    if (log_line) {
      log_line("INPUT script %s mode=%d value=%.2f", step.action.c_str(), step.mode, step.value);
    }
  }
}

}  // namespace kcds::runtime::input::scripted
