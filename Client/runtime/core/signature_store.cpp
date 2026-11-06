// Winsock before Windows.h (see client_internal.hpp).
#include "client_internal.hpp"

#include "signature_store.hpp"

#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

#include "kcd2s/bridge/signature_cache.hpp"
#include "log.hpp"
#include "natives.hpp"

namespace kcds::game_client {

namespace {

std::mutex store_mutex;
std::unique_ptr<kcd2s::bridge::SignatureCache> cache;
ULONGLONG opened_at = 0;
unsigned from_cache = 0;
unsigned scanned = 0;

std::wstring store_path() {
  wchar_t local[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }
  CreateDirectoryW((std::wstring(local) + L"\\KCDS").c_str(), nullptr);
  return std::wstring(local) + L"\\KCDS\\signatures.txt";
}

// The linker's stamp and the loaded size of a module, which differ between its builds.
std::string identity_of(const void* base) {
  const auto* bytes = static_cast<const std::uint8_t*>(base);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(bytes + dos->e_lfanew);
  char text[32] = {};
  std::snprintf(text, sizeof(text), "%08lx-%08lx", nt->FileHeader.TimeDateStamp,
                nt->OptionalHeader.SizeOfImage);
  return text;
}

}  // namespace

void open_signature_store() {
  std::lock_guard<std::mutex> lock(store_mutex);
  if (cache) {
    return;
  }
  opened_at = GetTickCount64();
  cache = std::make_unique<kcd2s::bridge::SignatureCache>(identity_of(image) + " " +
                                                          identity_of(module));
  const std::wstring path = store_path();
  if (path.empty()) {
    return;
  }
  std::ifstream file(path, std::ios::binary);
  if (file) {
    std::stringstream saved;
    saved << file.rdbuf();
    cache->load(saved.str());
  }
}

std::optional<std::size_t> known_signature(const char* pattern) {
  std::lock_guard<std::mutex> lock(store_mutex);
  return cache ? cache->find(pattern) : std::nullopt;
}

void remember_signature(const char* pattern, std::size_t offset) {
  std::lock_guard<std::mutex> lock(store_mutex);
  if (cache) {
    cache->remember(pattern, offset);
  }
}

void count_signature(bool known) {
  std::lock_guard<std::mutex> lock(store_mutex);
  if (known) {
    ++from_cache;
  } else {
    ++scanned;
  }
}

void save_signature_store() {
  std::lock_guard<std::mutex> lock(store_mutex);
  if (!cache) {
    return;
  }
  log("STARTUP installed in %llu ms: %u signatures from the cache, %u scanned",
      static_cast<unsigned long long>(GetTickCount64() - opened_at), from_cache, scanned);
  const std::wstring path = store_path();
  if (path.empty() || !cache->changed()) {
    return;
  }
  // Written apart and moved over, since several game clients may start together.
  const std::wstring part = path + L"." + std::to_wstring(GetCurrentProcessId());
  {
    std::ofstream file(part, std::ios::binary | std::ios::trunc);
    file << cache->serialized();
    if (!file.good()) {
      file.close();
      DeleteFileW(part.c_str());
      return;
    }
  }
  if (!MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(part.c_str());
  }
}

}  // namespace kcds::game_client
