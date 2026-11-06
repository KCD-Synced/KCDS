#include "resource_cache.hpp"

#include <windows.h>

#include <mutex>
#include <system_error>

namespace kcds::runtime::session {
namespace {

std::mutex catalog_mutex;
std::set<std::string> catalog_hashes;

}  // namespace

void set_catalog_hashes(const std::set<std::string>& hashes) {
  std::lock_guard<std::mutex> lock(catalog_mutex);
  catalog_hashes = hashes;
}

std::filesystem::path resource_cache_folder() {
  wchar_t local[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return std::filesystem::temp_directory_path() / L"KCDS" / L"cache" / L"resources";
  }
  return std::filesystem::path(local) / L"KCDS" / L"cache" / L"resources";
}

std::filesystem::path cached_resource_file(const std::string& hash) {
  if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos) {
    return {};
  }
  {
    std::lock_guard<std::mutex> lock(catalog_mutex);
    if (!catalog_hashes.count(hash)) {
      return {};
    }
  }
  const std::filesystem::path file = resource_cache_folder() / hash;
  std::error_code error;
  if (!std::filesystem::is_regular_file(file, error) || error) {
    return {};
  }
  return file;
}

}  // namespace kcds::runtime::session
