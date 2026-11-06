// Windows part of the crash settings hand-off: the process environment.
#include <windows.h>

#include <string>
#include <vector>

#include "kcds/launcher/crash_handoff.hpp"

namespace kcds::launcher::crash {

namespace {

// Variable names and values are validated ASCII (DSN, tag, "1").
std::wstring ascii_to_wide(const std::string& text) {
  std::wstring wide;
  wide.reserve(text.size());
  for (char c : text) {
    wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
  }
  return wide;
}

std::string read_variable(const char* name) {
  std::wstring wide_name = ascii_to_wide(name);
  std::vector<wchar_t> buffer(512);
  while (true) {
    DWORD length = GetEnvironmentVariableW(wide_name.c_str(), buffer.data(),
                                           static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return std::string();
    }
    if (length < buffer.size()) {
      buffer.resize(length);
      break;
    }
    if (buffer.size() >= 32768) {
      return std::string();
    }
    buffer.resize(length);
  }

  // Anything outside printable ASCII cannot be one of our values.
  std::string value;
  for (wchar_t c : buffer) {
    if (c < 0x20 || c > 0x7e) {
      return std::string();
    }
    value.push_back(static_cast<char>(c));
  }
  return value;
}

}  // namespace

bool export_handoff(const std::vector<HandoffVariable>& variables) {
  bool ok = true;
  for (const HandoffVariable& variable : variables) {
    std::wstring name = ascii_to_wide(variable.name);
    BOOL changed = FALSE;
    if (variable.value.empty()) {
      changed = SetEnvironmentVariableW(name.c_str(), nullptr);
      // Removing a variable that does not exist is not a failure.
      if (!changed && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
        changed = TRUE;
      }
    } else {
      std::wstring value = ascii_to_wide(variable.value);
      changed = SetEnvironmentVariableW(name.c_str(), value.c_str());
    }
    if (!changed) {
      ok = false;
    }
  }
  return ok;
}

Handoff read_handoff_from_environment() {
  return read_handoff(read_variable(kDsnVariable), read_variable(kEnvironmentVariable),
                      read_variable(kAllowHttpLoopbackVariable));
}

}  // namespace kcds::launcher::crash
