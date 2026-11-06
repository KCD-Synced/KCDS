#include "nickname.hpp"

#include <Windows.h>

#include <vector>

namespace kcds::runtime::identity {
namespace {

using SteamFriends = void*(__cdecl*)();
using GetPersonaName = const char*(__cdecl*)(void* friends);

std::string from_environment() {
  const DWORD length = GetEnvironmentVariableW(L"KCDS_NICKNAME", nullptr, 0);
  if (length <= 1) {
    return {};
  }
  std::vector<wchar_t> wide(length);
  GetEnvironmentVariableW(L"KCDS_NICKNAME", wide.data(), length);
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), -1, nullptr, 0, nullptr, nullptr);
  if (bytes <= 1) {
    return {};
  }
  std::string name(static_cast<std::size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), -1, name.data(), bytes, nullptr, nullptr);
  name.resize(static_cast<std::size_t>(bytes) - 1);
  return name;
}

std::string from_steam() {
  HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
  if (!steam) {
    return {};
  }
  auto friends =
      reinterpret_cast<SteamFriends>(GetProcAddress(steam, "SteamAPI_SteamFriends_v017"));
  auto persona = reinterpret_cast<GetPersonaName>(
      GetProcAddress(steam, "SteamAPI_ISteamFriends_GetPersonaName"));
  void* interface_pointer = friends ? friends() : nullptr;
  const char* name = interface_pointer && persona ? persona(interface_pointer) : nullptr;
  return name ? std::string(name) : std::string();
}

}  // namespace

std::string local_nickname() {
  std::string name = from_environment();
  if (name.empty()) {
    name = from_steam();
  }
  return name;
}

}  // namespace kcds::runtime::identity
