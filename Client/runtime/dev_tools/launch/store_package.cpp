#include "store_package.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <appmodel.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace kcds::runtime::launch {
namespace {

namespace fs = std::filesystem;

// The two files a packaged copy has in its install root: the Windows package manifest, and the
// game's own configuration that the package manifest is generated from.
constexpr const wchar_t* kManifestNames[] = {L"appxmanifest.xml", L"MicrosoftGame.config"};

// Manifests are a few KiB; anything far larger is not one.
constexpr std::uintmax_t kMaximumManifestBytes = 1024 * 1024;

std::wstring widen(const std::string& utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int size =
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(size > 0 ? size : 0), L'\0');
  if (size > 0) {
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
  }
  return out;
}

// A manifest's text as UTF-8; UTF-16 files (with their byte order mark) are converted.
std::string read_manifest(const fs::path& path) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec || size == 0 || size > kMaximumManifestBytes) {
    return {};
  }
  std::ifstream input(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
      static_cast<unsigned char>(bytes[1]) == 0xFE) {
    // Copied into wide characters rather than read in place: the bytes have no wchar_t alignment.
    std::wstring wide((bytes.size() - 2) / sizeof(wchar_t), L'\0');
    std::memcpy(wide.data(), bytes.data() + 2, wide.size() * sizeof(wchar_t));
    const int length = static_cast<int>(wide.size());
    const int size8 =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(size8 > 0 ? size8 : 0), '\0');
    if (size8 > 0) {
      WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, utf8.data(), size8, nullptr, nullptr);
    }
    return utf8;
  }
  if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) {
    bytes.erase(0, 3);
  }
  return bytes;
}

std::wstring family_of(const PackageIdentity& identity) {
  std::wstring name = widen(identity.name);
  std::wstring publisher = widen(identity.publisher);
  PACKAGE_ID id{};
  id.name = name.data();
  id.publisher = publisher.data();
  wchar_t family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1]{};
  UINT32 length = PACKAGE_FAMILY_NAME_MAX_LENGTH + 1;
  if (PackageFamilyNameFromId(&id, &length, family) != ERROR_SUCCESS) {
    return {};
  }
  return family;
}

// The full name of the family's package registered for the current user, or empty.
std::wstring installed_full_name(const std::wstring& family) {
  UINT32 count = 0;
  UINT32 buffer_length = 0;
  LONG result =
      GetPackagesByPackageFamily(family.c_str(), &count, nullptr, &buffer_length, nullptr);
  if (result != ERROR_INSUFFICIENT_BUFFER || count == 0 || buffer_length == 0) {
    return {};
  }
  std::vector<PWSTR> names(count);
  std::vector<wchar_t> buffer(buffer_length);
  result = GetPackagesByPackageFamily(family.c_str(), &count, names.data(), &buffer_length,
                                      buffer.data());
  if (result != ERROR_SUCCESS || count == 0 || names[0] == nullptr) {
    return {};
  }
  return names[0];
}

}  // namespace

std::optional<StorePackage> find_store_package(const fs::path& game_exe) {
  // <root>\Bin\<build>\KingdomCome.exe: the manifests live in <root>; a layout with the exe in
  // the root is covered by looking beside the exe too.
  const fs::path exe_folder = game_exe.parent_path();
  const fs::path root = exe_folder.parent_path().parent_path();
  for (const fs::path& folder : {root, exe_folder}) {
    if (folder.empty()) {
      continue;
    }
    for (const wchar_t* name : kManifestNames) {
      const fs::path manifest = folder / name;
      std::error_code ec;
      if (!fs::is_regular_file(manifest, ec)) {
        continue;
      }
      StorePackage package;
      package.manifest = manifest;
      const auto identity = parse_package_identity(read_manifest(manifest));
      if (identity) {
        package.identity = *identity;
        package.family = family_of(*identity);
        if (!package.family.empty()) {
          package.full_name = installed_full_name(package.family);
        }
      }
      // A manifest that can't be read still marks a packaged copy: it can't be started as a
      // plain process either.
      return package;
    }
  }
  return std::nullopt;
}

std::wstring app_user_model_id(const StorePackage& package) {
  if (package.family.empty() || package.identity.app_id.empty()) {
    return {};
  }
  return package.family + L"!" + widen(package.identity.app_id);
}

}  // namespace kcds::runtime::launch
