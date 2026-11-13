#include "game_folder.hpp"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <cwctype>
#include <string>

namespace kcds::runtime::game_folder {
namespace {

bool is_bin(const std::filesystem::path& folder) {
  const std::wstring name = folder.filename().wstring();
  return name.size() == 3 && std::towlower(name[0]) == L'b' && std::towlower(name[1]) == L'i' &&
         std::towlower(name[2]) == L'n';
}

}  // namespace

std::filesystem::path root_of(const std::filesystem::path& exe) {
  const std::filesystem::path folder = exe.parent_path();
  if (is_bin(folder.parent_path())) {
    return folder.parent_path().parent_path();
  }
  return folder;
}

#ifdef _WIN32
std::filesystem::path root() {
  wchar_t exe[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }
  return root_of(std::filesystem::path(std::wstring(exe, length)));
}
#endif

}  // namespace kcds::runtime::game_folder
