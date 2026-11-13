// The running game's GOG install, read for its licence proof (gog_report.hpp): hashes of its key
// files, Galaxy64.dll's Authenticode signer and emulator or crack files beside the game. Only
// file names inside the game's folder and hashes leave the machine, never file contents.
#include "gog_report.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <SoftPub.h>
#include <WinTrust.h>
#include <wincrypt.h>

#include <mutex>
#include <system_error>

#include "sha256.hpp"

namespace kcds::runtime::identity {
namespace {

namespace fs = std::filesystem;

fs::path module_path(HMODULE module) {
  wchar_t path[MAX_PATH * 4]{};
  const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
  if (length == 0 || length >= std::size(path)) {
    return {};
  }
  return fs::path(std::wstring(path, length));
}

std::string utf8(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                       nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
  if (size > 0) {
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
  }
  return out;
}

// The subject of file's Authenticode signer when the signature is valid, else "". Revocation is
// not checked online: the Master compares the signer's name, and the game may be offline.
std::string authenticode_signer(const fs::path& file) {
  WINTRUST_FILE_INFO file_info{};
  file_info.cbStruct = sizeof(file_info);
  file_info.pcwszFilePath = file.c_str();
  WINTRUST_DATA data{};
  data.cbStruct = sizeof(data);
  data.dwUIChoice = WTD_UI_NONE;
  data.fdwRevocationChecks = WTD_REVOKE_NONE;
  data.dwUnionChoice = WTD_CHOICE_FILE;
  data.pFile = &file_info;
  data.dwStateAction = WTD_STATEACTION_VERIFY;
  data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
  std::string signer;
  if (status == ERROR_SUCCESS) {
    CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
    CRYPT_PROVIDER_SGNR* signer_data =
        provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
    CRYPT_PROVIDER_CERT* certificate =
        signer_data ? WTHelperGetProvCertFromChain(signer_data, 0) : nullptr;
    if (certificate && certificate->pCert) {
      wchar_t name[256]{};
      const DWORD length = CertGetNameStringW(certificate->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0,
                                              nullptr, name, static_cast<DWORD>(std::size(name)));
      if (length > 1) {
        signer = utf8(std::wstring(name, length - 1));
      }
    }
  }
  data.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
  return signer;
}

std::vector<std::string> crack_markers(const fs::path& exe_folder, const fs::path& root) {
  std::vector<std::string> found;
  for (const std::string& name : gog_crack_marker_names()) {
    const fs::path relative(std::u8string(name.begin(), name.end()));
    std::error_code ec;
    if (fs::exists(exe_folder / relative, ec) || fs::exists(root / relative, ec)) {
      found.push_back(name);
    }
  }
  return found;
}

GogInstall read_install() {
  GogInstall install;
  const fs::path exe = module_path(nullptr);
  if (exe.empty()) {
    return install;
  }
  const fs::path folder = exe.parent_path();
  fs::path whgame = module_path(GetModuleHandleW(L"WHGame.dll"));
  if (whgame.empty()) {
    whgame = folder / L"WHGame.dll";
  }
  install.exe_sha256 = sha256_file_hex(exe);
  install.whgame_sha256 = sha256_file_hex(whgame);
  const fs::path galaxy = folder / L"Galaxy64.dll";
  std::error_code ec;
  if (fs::is_regular_file(galaxy, ec)) {
    install.galaxy_sha256 = sha256_file_hex(galaxy);
    install.galaxy_signer = authenticode_signer(galaxy);
  }
  install.markers = crack_markers(folder, folder.parent_path().parent_path());
  return install;
}

}  // namespace

GogInstall read_gog_install() {
  // The files don't change while the game runs: read once (WHGame.dll is large).
  static std::once_flag once;
  static GogInstall install;
  std::call_once(once, [] { install = read_install(); });
  return install;
}

}  // namespace kcds::runtime::identity
