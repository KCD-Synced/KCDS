#include "resource_files.hpp"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_set>
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
// Files asked for in one batch request (web/file_server.cjs), and the most bytes a batch answer may
// hold (the server's 4 MB, or one file of up to its in-memory size past it).
constexpr std::size_t kBatchFiles = 64;
constexpr std::uint64_t kMaxBatchBytes = 64ull * 1024 * 1024;

// What the workers share with the network thread; it outlives the downloader when a worker is
// still in a request as the session ends.
struct Shared {
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::string> queue;
  std::vector<HttpFile> done;
  std::string token;
  bool stopping = false;
  // Whether the server answers batches (an older one does not: 404), and the files a batch left
  // to their own requests.
  bool batches = true;
  std::unordered_set<std::string> single;
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

// A part file in the cache's folder (on its own volume, so the cache moves it into place).
std::filesystem::path part_file(const std::filesystem::path& folder, const std::string& hash) {
  static std::atomic<unsigned> counter{0};
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  return folder / (hash + "." + std::to_string(GetCurrentProcessId()) + "." +
                   std::to_string(counter++) + ".http.part");
}

// What a batch request brought.
enum class BatchAnswer { files, unsupported, failed };

// GET /kcds/batch?h=<hash>,...: each file the answer holds written to its part file, in
// `written` (the same order as `hashes`, an empty path for one the server left to its own
// request).
BatchAnswer download_batch(const std::wstring& host, INTERNET_PORT port,
                           const std::vector<std::string>& hashes, const std::string& token,
                           const std::filesystem::path& folder,
                           std::vector<std::filesystem::path>& written) {
  Handle session{WinHttpOpen(L"KCDS", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session.value) {
    return BatchAnswer::failed;
  }
  WinHttpSetTimeouts(session.value, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
  Handle connection{WinHttpConnect(session.value, host.c_str(), port, 0)};
  std::wstring path = L"/kcds/batch?h=";
  for (std::size_t i = 0; i < hashes.size(); ++i) {
    if (i > 0) {
      path += L",";
    }
    path += std::wstring(hashes[i].begin(), hashes[i].end());
  }
  Handle request{connection.value
                     ? WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
                     : nullptr};
  const std::wstring header = L"x-kcds-token: " + std::wstring(token.begin(), token.end());
  if (!request.value ||
      !WinHttpSendRequest(request.value, header.c_str(), DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0,
                          0) ||
      !WinHttpReceiveResponse(request.value, nullptr)) {
    return BatchAnswer::failed;
  }
  DWORD status = 0;
  DWORD size = sizeof(status);
  if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
    return BatchAnswer::failed;
  }
  if (status == 404) {
    // Its short text read, so WinHTTP keeps the connection for the next file.
    char rest[256];
    DWORD read = 0;
    while (WinHttpReadData(request.value, rest, sizeof(rest), &read) && read > 0) {
    }
    return BatchAnswer::unsupported;
  }
  // Up to 12 digits and a comma per file.
  wchar_t sizes_text[kBatchFiles * 14] = {};
  DWORD sizes_bytes = sizeof(sizes_text);
  if (status != 200 || !WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CUSTOM, L"x-kcds-sizes",
                                            sizes_text, &sizes_bytes, WINHTTP_NO_HEADER_INDEX)) {
    return BatchAnswer::failed;
  }
  // The sizes, "-" (none) as -1, one per hash asked.
  std::vector<std::int64_t> sizes;
  std::wstringstream entries(sizes_text);
  std::wstring entry;
  while (std::getline(entries, entry, L',')) {
    if (entry == L"-") {
      sizes.push_back(-1);
      continue;
    }
    if (entry.empty() || entry.size() > 12 ||
        entry.find_first_not_of(L"0123456789") != std::wstring::npos) {
      return BatchAnswer::failed;
    }
    sizes.push_back(std::stoll(entry));
  }
  if (sizes.size() != hashes.size()) {
    return BatchAnswer::failed;
  }
  std::string body;
  std::vector<char> piece(kPiece);
  for (;;) {
    DWORD read = 0;
    if (!WinHttpReadData(request.value, piece.data(), kPiece, &read)) {
      return BatchAnswer::failed;
    }
    if (read == 0) {
      break;
    }
    if (body.size() + read > kMaxBatchBytes) {
      return BatchAnswer::failed;
    }
    body.append(piece.data(), read);
  }
  std::uint64_t total = 0;
  for (const std::int64_t bytes : sizes) {
    total += bytes > 0 ? std::uint64_t(bytes) : 0;
  }
  if (total != body.size()) {
    return BatchAnswer::failed;
  }
  written.assign(hashes.size(), {});
  std::size_t at = 0;
  for (std::size_t i = 0; i < hashes.size(); ++i) {
    if (sizes[i] < 0) {
      continue;
    }
    const std::filesystem::path file = part_file(folder, hashes[i]);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(body.data() + at, std::streamsize(sizes[i])) || !out.flush()) {
      out.close();
      std::error_code error;
      std::filesystem::remove(file, error);
      for (auto& done : written) {
        if (!done.empty()) {
          std::filesystem::remove(done, error);
        }
      }
      return BatchAnswer::failed;
    }
    at += std::size_t(sizes[i]);
    written[i] = file;
  }
  return BatchAnswer::files;
}

// A worker's batch: the files it got, and the rest back at the front of the queue for requests
// of their own.
void finish_batch(Shared& shared, const std::vector<std::string>& hashes,
                  std::vector<std::filesystem::path>& written, BatchAnswer answer) {
  std::lock_guard<std::mutex> lock(shared.mutex);
  if (answer == BatchAnswer::unsupported) {
    shared.batches = false;
  }
  for (std::size_t i = hashes.size(); i-- > 0;) {
    const bool got = answer == BatchAnswer::files && !written[i].empty();
    if (got && !shared.stopping) {
      shared.done.push_back({hashes[i], true, written[i]});
    } else if (got) {
      std::error_code error;
      std::filesystem::remove(written[i], error);
    } else {
      shared.single.insert(hashes[i]);
      shared.queue.push_front(hashes[i]);
    }
  }
  shared.wake.notify_all();
}

void work(std::shared_ptr<Shared> shared, std::wstring host, INTERNET_PORT port,
          std::filesystem::path folder) {
  for (;;) {
    std::string hash;
    std::string token;
    // More queued files to ask for with it in one batch.
    std::vector<std::string> batch;
    {
      std::unique_lock<std::mutex> lock(shared->mutex);
      shared->wake.wait(lock, [&] { return shared->stopping || !shared->queue.empty(); });
      if (shared->stopping) {
        return;
      }
      hash = std::move(shared->queue.front());
      shared->queue.pop_front();
      token = shared->token;
      if (shared->batches && !shared->single.count(hash)) {
        batch.push_back(hash);
        for (auto next = shared->queue.begin();
             next != shared->queue.end() && batch.size() < kBatchFiles;) {
          if (shared->single.count(*next)) {
            ++next;
            continue;
          }
          batch.push_back(std::move(*next));
          next = shared->queue.erase(next);
        }
      }
    }
    if (batch.size() > 1) {
      std::vector<std::filesystem::path> written;
      const BatchAnswer answer = token.empty()
                                     ? BatchAnswer::failed
                                     : download_batch(host, port, batch, token, folder, written);
      finish_batch(*shared, batch, written, answer);
      continue;
    }
    std::error_code error;
    const std::filesystem::path file = part_file(folder, hash);
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
