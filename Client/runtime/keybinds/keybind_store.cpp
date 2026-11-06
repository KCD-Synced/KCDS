#include "keybind_store.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace kcds::runtime::keybinds {
namespace {

// FNV-1a: a file name for any server text.
std::uint64_t hash_of(const std::string& text) {
  std::uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::vector<std::string> split(const std::string& text, char separator) {
  std::vector<std::string> parts;
  std::string part;
  std::istringstream stream(text);
  while (std::getline(stream, part, separator)) {
    parts.push_back(part);
  }
  return parts;
}

}  // namespace

std::wstring store_path(const std::string& server) {
  wchar_t local[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  if (length == 0 || length >= MAX_PATH || server.empty()) {
    return {};
  }
  wchar_t name[32] = {};
  swprintf(name, 32, L"%016llx.txt", static_cast<unsigned long long>(hash_of(server)));
  const std::wstring folder = std::wstring(local) + L"\\KCDS\\keybinds";
  CreateDirectoryW((std::wstring(local) + L"\\KCDS").c_str(), nullptr);
  CreateDirectoryW(folder.c_str(), nullptr);
  return folder + L"\\" + name;
}

Choices load_choices(const std::wstring& path) {
  Choices choices;
  if (path.empty()) {
    return choices;
  }
  std::ifstream file(path);
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const std::vector<std::string> fields = split(line, '\t');
    if (fields.size() < 2 || fields[0].empty()) {
      continue;
    }
    const std::string value = fields.size() > 2 ? fields[2] : std::string();
    if (fields[1] == "keyboard") {
      std::vector<std::string> inputs;
      for (const std::string& input : split(value, ',')) {
        if (!input.empty()) {
          inputs.push_back(input);
        }
      }
      choices[fields[0]].keyboard = std::move(inputs);
    } else if (fields[1] == "controller") {
      choices[fields[0]].controller = value;
    }
  }
  return choices;
}

bool save_choices(const std::wstring& path, const Choices& choices) {
  if (path.empty()) {
    return false;
  }
  const std::wstring temporary = path + L".new";
  {
    std::ofstream file(temporary, std::ios::trunc);
    if (!file) {
      return false;
    }
    file << "# KCDS key bindings of one server's resource actions\n";
    for (const auto& [name, choice] : choices) {
      if (choice.keyboard) {
        std::string inputs;
        for (const std::string& input : *choice.keyboard) {
          inputs += (inputs.empty() ? "" : ",") + input;
        }
        file << name << "\tkeyboard\t" << inputs << "\n";
      }
      if (choice.controller) {
        file << name << "\tcontroller\t" << *choice.controller << "\n";
      }
    }
  }
  return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

}  // namespace kcds::runtime::keybinds
