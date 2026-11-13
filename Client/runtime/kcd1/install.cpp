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
#include <utility>

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

// The packs the map is built from: Rataje's level and navigation, KCD1's game data, characters,
// models, buildings and textures.
const wchar_t* const kNeededFiles[] = {
    L"Data/Levels/rataje/level.pak",
    L"Data/Levels/rataje/recast.pak",
    L"Data/GameData.pak",
    L"Data/Characters.pak",
    L"Data/Objects.pak",
    L"Data/Buildings.pak",
    L"Data/Textures.pak",
};

#ifdef _WIN32
// A folder Steam keeps in the registry: the user's Steam, else the machine's (32- and 64-bit
// views).
std::filesystem::path registry_folder(HKEY root, const wchar_t* key, const wchar_t* value) {
  wchar_t path[MAX_PATH] = {};
  DWORD size = sizeof(path);
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS) {
    return {};
  }
  return std::filesystem::path(path);
}

std::filesystem::path steam_folder() {
  std::filesystem::path folder =
      registry_folder(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
  if (folder.empty()) {
    folder =
        registry_folder(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
  }
  if (folder.empty()) {
    folder = registry_folder(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
  }
  return folder;
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

std::vector<std::filesystem::path> missing_files(const std::filesystem::path& folder) {
  std::vector<std::filesystem::path> missing;
  for (const wchar_t* name : kNeededFiles) {
    const std::filesystem::path file = std::filesystem::path(name).make_preferred();
    std::error_code error;
    if (folder.empty() || !std::filesystem::is_regular_file(folder / file, error)) {
      missing.push_back(file);
    }
  }
  return missing;
}

std::vector<std::filesystem::path> library_paths(const std::string& vdf) {
  std::vector<std::filesystem::path> paths;
  for (const std::string& value : values_of(vdf, "path")) {
    paths.push_back(from_utf8(value));
  }
  return paths;
}

std::string utf8(const std::filesystem::path& path) {
  const auto text = path.u8string();
  return std::string(text.begin(), text.end());
}

std::filesystem::path from_utf8(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string install_folder(const std::string& manifest) {
  const std::vector<std::string> values = values_of(manifest, "installdir");
  return values.empty() ? std::string() : values.front();
}

std::string crypak_path(const std::filesystem::path& path) {
  const auto plain = [](const std::wstring& wide) {
    std::string out;
    for (const wchar_t c : wide) {
      if (c == 0 || c > 0x7F) {
        return std::string();
      }
      out += char(c);
    }
    return out;
  };
  const std::string as_is = plain(path.wstring());
  if (!as_is.empty() || path.empty()) {
    return as_is;
  }
#ifdef _WIN32
  // The short form exists only for files and folders that are there.
  const DWORD needed = GetShortPathNameW(path.c_str(), nullptr, 0);
  if (needed == 0) {
    return {};
  }
  std::wstring short_form(needed, L'\0');
  const DWORD length = GetShortPathNameW(path.c_str(), short_form.data(), needed);
  if (length == 0 || length >= needed) {
    return {};
  }
  short_form.resize(length);
  return plain(short_form);
#else
  return {};
#endif
}

InstallSearch search_install() {
  InstallSearch search;
#ifdef _WIN32
  // A folder with every pack is the install; the first one short of some is kept for the log.
  const auto consider = [&search](const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> missing = missing_files(folder);
    if (missing.empty()) {
      search.folder = folder;
      return true;
    }
    if (search.incomplete.empty()) {
      search.incomplete = folder;
      search.missing = std::move(missing);
    }
    return false;
  };
  const std::filesystem::path chosen = environment_folder();
  if (!chosen.empty()) {
    consider(chosen);
    return search;
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
    if (!folder.empty() && consider(apps / L"common" / from_utf8(folder))) {
      return search;
    }
  }
  const std::filesystem::path beside = own_library() / L"KingdomComeDeliverance";
  std::error_code error;
  if (std::filesystem::is_directory(beside, error)) {
    consider(beside);
  }
#endif
  return search;
}

std::filesystem::path find_install() {
  return search_install().folder;
}

}  // namespace kcds::runtime::kcd1
