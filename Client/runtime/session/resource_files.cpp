#include "resource_files.hpp"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace kcds::runtime::session {
namespace {

using kcd2s::local::resources::HttpFile;

// Files downloaded at once, how long the server may be silent, and the largest file the catalog
// may hold (bundles.cjs LIMITS.fileBytes).
constexpr int kWorkers = 4;
constexpr int kTimeoutMs = 60000;
constexpr std::uint64_t kMaxFileBytes = 512ull * 1024 * 1024;
constexpr DWORD kPiece = 1 << 20;

// What the workers share with the network thread; it outlives the downloader when a worker is
// still in a request as the session ends.
struct Shared {
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::string> queue;
  std::vector<HttpFile> done;
  std::string token;
  bool stopping = false;
};

struct Handle {
  HINTERNET value = nullptr;
  ~Handle() {
    if (value) {
      WinHttpCloseHandle(value);
    }
  }
};

// GET http://<host>:<port>/kcds/files/<hash>, written to `file` as it comes. The bytes are not
// checked here: the cache keeps them only when their SHA-256 is the hash (download_cache.hpp
// put_file), so plain HTTP is enough.
bool download(const std::wstring& host, INTERNET_PORT port, const std::string& hash,
              const std::string& token, const std::filesystem::path& file) {
  Handle session{WinHttpOpen(L"KCDS", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session.value) {
    return false;
  }
  WinHttpSetTimeouts(session.value, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
  Handle connection{WinHttpConnect(session.value, host.c_str(), port, 0)};
  const std::wstring path = L"/kcds/files/" + std::wstring(hash.begin(), hash.end());
  Handle request{connection.value
                     ? WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
                     : nullptr};
  // The server answers only a connected player's game (web/file_server.cjs).
  const std::wstring header = L"x-kcds-token: " + std::wstring(token.begin(), token.end());
  if (!request.value ||
      !WinHttpSendRequest(request.value, header.c_str(), DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0,
                          0) ||
      !WinHttpReceiveResponse(request.value, nullptr)) {
    return false;
  }
  DWORD status = 0;
  DWORD size = sizeof(status);
  if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) ||
      status != 200) {
    return false;
  }
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  std::vector<char> piece(kPiece);
  std::uint64_t total = 0;
  for (;;) {
    DWORD read = 0;
    if (!WinHttpReadData(request.value, piece.data(), kPiece, &read)) {
      return false;
    }
    if (read == 0) {
      break;
    }
    total += read;
    if (total > kMaxFileBytes || !out.write(piece.data(), read)) {
      return false;
    }
  }
  return static_cast<bool>(out.flush());
}

void work(std::shared_ptr<Shared> shared, std::wstring host, INTERNET_PORT port,
          std::filesystem::path folder) {
  static std::atomic<unsigned> counter{0};
  for (;;) {
    std::string hash;
    std::string token;
    {
      std::unique_lock<std::mutex> lock(shared->mutex);
      shared->wake.wait(lock, [&] { return shared->stopping || !shared->queue.empty(); });
      if (shared->stopping) {
        return;
      }
      hash = std::move(shared->queue.front());
      shared->queue.pop_front();
      token = shared->token;
    }
    // On the cache's own volume, so the cache moves it into place.
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    const std::filesystem::path file =
        folder / (hash + "." + std::to_string(GetCurrentProcessId()) + "." +
                  std::to_string(counter++) + ".http.part");
    HttpFile result;
    result.hash = std::move(hash);
    result.ok = !token.empty() && download(host, port, result.hash, token, file);
    if (result.ok) {
      result.path = file;
    } else {
      std::filesystem::remove(file, error);
    }
    std::lock_guard<std::mutex> lock(shared->mutex);
    if (shared->stopping) {
      std::filesystem::remove(file, error);
      return;
    }
    shared->done.push_back(std::move(result));
  }
}

class ResourceFiles final : public kcd2s::local::resources::HttpFiles {
 public:
  ResourceFiles(const std::string& ip, std::uint16_t port, const std::filesystem::path& folder)
      : shared_(std::make_shared<Shared>()) {
    const std::wstring host(ip.begin(), ip.end());
    for (int i = 0; i < kWorkers; ++i) {
      std::thread(work, shared_, host, INTERNET_PORT(port), folder).detach();
    }
  }

  ~ResourceFiles() override {
    {
      std::lock_guard<std::mutex> lock(shared_->mutex);
      shared_->stopping = true;
      shared_->queue.clear();
    }
    shared_->wake.notify_all();
  }

  void set_token(const std::string& token) override {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->token = token;
  }

  void start(const std::string& hash) override {
    {
      std::lock_guard<std::mutex> lock(shared_->mutex);
      shared_->queue.push_back(hash);
    }
    shared_->wake.notify_one();
  }

  std::vector<HttpFile> take_done() override {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    std::vector<HttpFile> taken;
    taken.swap(shared_->done);
    return taken;
  }

 private:
  std::shared_ptr<Shared> shared_;
};

}  // namespace

std::unique_ptr<kcd2s::local::resources::HttpFiles> make_resource_files(
    const std::string& ip, std::uint16_t port, const std::filesystem::path& folder) {
  return std::make_unique<ResourceFiles>(ip, port, folder);
}

}  // namespace kcds::runtime::session
