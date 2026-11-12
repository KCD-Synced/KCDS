#include "install.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <fstream>
#include <iterator>
#include <system_error>

namespace kcds::runtime::kcd1 {
namespace {

std::string read_text(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The quoted values that follow a quoted key in a Valve key-value text ("key"  "value"), with
// the text's escaped backslashes undone.
std::vector<std::string> values_of(const std::string& text, const std::string& key) {
  std::vector<std::string> values;
  const std::string quoted = "\"" + key + "\"";
  std::size_t at = text.find(quoted);
  while (at != std::string::npos) {
    std::size_t open = text.find('"', at + quoted.size());
    const std::size_t line_end = text.find('\n', at);
    if (open == std::string::npos || (line_end != std::string::npos && open > line_end)) {
      at = text.find(quoted, at + quoted.size());
      continue;
    }
    std::string value;
    std::size_t index = open + 1;
    while (index < text.size() && text[index] != '"') {
      if (text[index] == '\\' && index + 1 < text.size()) {
        index += 1;
      }
      value += text[index];
      index += 1;
    }
    values.push_back(value);
    at = text.find(quoted, index);
  }
  return values;
}

// Whether a folder is the game with its Rataje level.
bool has_game(const std::filesystem::path& folder) {
  std::error_code error;
  return !folder.empty() && std::filesystem::is_regular_file(
                                folder / L"Data" / L"Levels" / L"rataje" / L"level.pak", error);
}

#ifdef _WIN32
std::filesystem::path steam_folder() {
  wchar_t path[MAX_PATH] = {};
  DWORD size = sizeof(path);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ,
                   nullptr, path, &size) != ERROR_SUCCESS) {
    return {};
  }
  return std::filesystem::path(path);
}

std::filesystem::path environment_folder() {
  wchar_t path[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"KCDS_KCD1_DIR", path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }
  return std::filesystem::path(path);
}

// ...\steamapps\common\KingdomComeDeliverance2\Bin\Win64...\KingdomCome.exe -> ...\common.
std::filesystem::path own_library() {
  wchar_t executable[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }
  return std::filesystem::path(executable).parent_path().parent_path().parent_path().parent_path();
}
#endif

}  // namespace

std::vector<std::filesystem::path> library_paths(const std::string& vdf) {
  std::vector<std::filesystem::path> paths;
  for (const std::string& value : values_of(vdf, "path")) {
    paths.emplace_back(value);
  }
  return paths;
}

std::string install_folder(const std::string& manifest) {
  const std::vector<std::string> values = values_of(manifest, "installdir");
  return values.empty() ? std::string() : values.front();
}

std::filesystem::path find_install() {
#ifdef _WIN32
  const std::filesystem::path chosen = environment_folder();
  if (!chosen.empty()) {
    return has_game(chosen) ? chosen : std::filesystem::path();
  }
  const std::string manifest_name = "appmanifest_" + std::to_string(kSteamApp) + ".acf";
  const std::filesystem::path steam = steam_folder();
  std::vector<std::filesystem::path> libraries;
  if (!steam.empty()) {
    libraries.push_back(steam);
    for (const auto& library :
         library_paths(read_text(steam / L"steamapps" / L"libraryfolders.vdf"))) {
      libraries.push_back(library);
    }
  }
  for (const auto& library : libraries) {
    const std::filesystem::path apps = library / L"steamapps";
    const std::string folder = install_folder(read_text(apps / manifest_name));
    if (!folder.empty() && has_game(apps / L"common" / folder)) {
      return apps / L"common" / folder;
    }
  }
  const std::filesystem::path beside = own_library() / L"KingdomComeDeliverance";
  if (has_game(beside)) {
    return beside;
  }
#endif
  return {};
}

}  // namespace kcds::runtime::kcd1
