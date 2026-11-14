#include "xbox_licence.hpp"

#if defined(_WIN32) && defined(KCDS_WITH_GDK_PROOF)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <appmodel.h>
// The official SDK declarations. The functions come from the GDK's static xgameruntime.lib
// (CMakeLists.txt): it imports no DLL, so a Steam/GOG/Epic build still loads without the runtime.
#include <XGameRuntimeInit.h>
#include <XStore.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#endif

namespace kcds::runtime::identity {

#if defined(_WIN32) && defined(KCDS_WITH_GDK_PROOF)
namespace {

constexpr char kProductId[] = "9N4W31HSMVVD";
constexpr size_t kMaximumChallengeSize = 512;
constexpr size_t kMaximumTokenSize = 48 * 1024;
constexpr auto kPollBudget = std::chrono::seconds(9);
constexpr auto kPollInterval = std::chrono::milliseconds(20);

void log_failure(Logger log, const char* operation, HRESULT result) {
  if (log) {
    log("[licence] Xbox %s failed (HRESULT 0x%08lX)", operation,
        static_cast<unsigned long>(result));
  }
}

struct StoreApi {
  HMODULE module{};
  decltype(&XStoreCreateContext) create_context{};
  decltype(&XStoreCloseContextHandle) close_context{};
  decltype(&XStoreQueryLicenseTokenAsync) query_token{};
  decltype(&XStoreQueryLicenseTokenResultSize) token_size{};
  decltype(&XStoreQueryLicenseTokenResult) token_result{};
  decltype(&XAsyncGetStatus) async_status{};
  decltype(&XAsyncCancel) async_cancel{};
  decltype(&XTaskQueueCreate) create_queue{};
  decltype(&XTaskQueueCloseHandle) close_queue{};

  bool load(Logger log) {
    if (module) {
      return true;
    }
    HMODULE runtime{};
    // Only in a game that already runs the GDK runtime (a Microsoft Store copy): another copy
    // of the game never loads one, and launcher credentials are never touched. A successful
    // lookup retains the already loaded runtime for the lifetime of the provider below.
    if (!GetModuleHandleExW(0, L"xgameruntime.dll", &runtime)) {
      if (log) {
        log("[licence] Xbox proof unavailable: no running GDK runtime");
      }
      return false;
    }
    // The runtime DLL exports none of these by name (only its QueryApiImpl entry points): the
    // GDK's static library reaches them. Its state is this module's own, so it takes its own
    // reference on the runtime the game initialized (one per process, counted), kept for the
    // provider's lifetime like the module.
    const HRESULT initialized = XGameRuntimeInitialize();
    if (FAILED(initialized)) {
      FreeLibrary(runtime);
      log_failure(log, "runtime initialize", initialized);
      return false;
    }
    create_context = &XStoreCreateContext;
    close_context = &XStoreCloseContextHandle;
    query_token = &XStoreQueryLicenseTokenAsync;
    token_size = &XStoreQueryLicenseTokenResultSize;
    token_result = &XStoreQueryLicenseTokenResult;
    async_status = &XAsyncGetStatus;
    async_cancel = &XAsyncCancel;
    create_queue = &XTaskQueueCreate;
    close_queue = &XTaskQueueCloseHandle;
    module = runtime;
    return true;
  }
};

struct TokenRequest {
  StoreApi& api;
  std::string challenge;
  // Own every argument for the whole asynchronous operation, including after a timeout.
  std::string product{kProductId};
  const char* products[1]{product.c_str()};
  XStoreContextHandle context{};
  XTaskQueueHandle queue{};
  XAsyncBlock async{};

  TokenRequest(StoreApi& store_api, const std::string& nonce) : api(store_api), challenge(nonce) {
  }

  ~TokenRequest() {
    if (context) {
      api.close_context(context);
    }
    if (queue) {
      api.close_queue(queue);
    }
  }
};

struct StoreProvider {
  std::mutex mutex;
  StoreApi api;
  std::unique_ptr<TokenRequest> pending;
};

StoreProvider& store_provider() {
  // XAsyncCancel may be ignored or complete later. Never destroy an unfinished block during
  // DLL teardown, and never leave a callback pointing into this DLL. A single retained slot
  // bounds abandoned requests; subsequent calls reap it only after terminal SDK status.
  // The retained xgameruntime reference also keeps its internal queued work executable.
  static auto* provider = new StoreProvider;
  return *provider;
}

std::string read_token(TokenRequest& request, Logger log) {
  size_t required{};
  HRESULT result = request.api.token_size(&request.async, &required);
  if (FAILED(result)) {
    log_failure(log, "token size", result);
    return {};
  }
  if (required <= 1 || required > kMaximumTokenSize + 1) {
    if (log) {
      log("[licence] Xbox token has an unsupported size");
    }
    return {};
  }
  std::vector<char> buffer(required);
  result = request.api.token_result(&request.async, buffer.size(), buffer.data());
  if (FAILED(result)) {
    log_failure(log, "token result", result);
    return {};
  }
  const auto end = std::find(buffer.begin(), buffer.end(), '\0');
  if (end == buffer.begin() || end == buffer.end()) {
    if (log) {
      log("[licence] Xbox token result is empty or unterminated");
    }
    return {};
  }
  return {buffer.begin(), end};
}

std::string query_store_token(const std::string& challenge, Logger log) {
  if (challenge.empty() || challenge.size() > kMaximumChallengeSize ||
      challenge.find('\0') != std::string::npos) {
    if (log) {
      log("[licence] Xbox proof requires a valid Master challenge");
    }
    return {};
  }
  auto& provider = store_provider();
  std::unique_lock lock(provider.mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    if (log) {
      log("[licence] Xbox proof request is already in progress");
    }
    return {};
  }
  if (!provider.api.load(log)) {
    return {};
  }
  if (provider.pending) {
    const HRESULT previous = provider.api.async_status(&provider.pending->async, false);
    if (previous == E_PENDING) {
      if (log) {
        log("[licence] Xbox previous request is still cancelling");
      }
      return {};
    }
    if (SUCCEEDED(previous)) {
      // Consume a late result but never return a proof for an earlier challenge.
      read_token(*provider.pending, nullptr);
    }
    provider.pending.reset();
  }

  const auto deadline = std::chrono::steady_clock::now() + kPollBudget;
  auto request = std::make_unique<TokenRequest>(provider.api, challenge);
  HRESULT result = provider.api.create_queue(XTaskQueueDispatchMode::ThreadPool,
                                             XTaskQueueDispatchMode::ThreadPool, &request->queue);
  if (FAILED(result)) {
    log_failure(log, "task queue", result);
    return {};
  }
  // On PC, nullptr uses the current Microsoft Store account without signing in an XUser.
  result = provider.api.create_context(nullptr, &request->context);
  if (FAILED(result)) {
    log_failure(log, "Store context", result);
    return {};
  }
  request->async.queue = request->queue;
  // No callback: poll the SDK status, independent of the game's completion queue/thread.
  provider.pending = std::move(request);
  auto& active = *provider.pending;
  result = provider.api.query_token(active.context, active.products, 1, active.challenge.c_str(),
                                    &active.async);
  if (FAILED(result)) {
    log_failure(log, "token request", result);
    provider.pending.reset();
    return {};
  }
  while (true) {
    result = provider.api.async_status(&active.async, false);
    if (result != E_PENDING) {
      break;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      provider.api.async_cancel(&active.async);
      if (log) {
        log("[licence] Xbox token timed out; unfinished request retained safely");
      }
      return {};
    }
    std::this_thread::sleep_until(std::min(deadline, now + kPollInterval));
  }
  if (FAILED(result)) {
    log_failure(log, "token completion", result);
    provider.pending.reset();
    return {};
  }
  std::string token = read_token(active, log);
  provider.pending.reset();
  if (log && !token.empty()) {
    log("[licence] Xbox signed token received (%zu bytes)", token.size());
  }
  return token;
}

}  // namespace
#endif

std::string xbox_licence_token(const std::string& challenge, Logger log) {
#if defined(_WIN32) && defined(KCDS_WITH_GDK_PROOF)
  try {
    return query_store_token(challenge, log);
  } catch (const std::exception&) {
    if (log) {
      log("[licence] Xbox proof could not allocate local request resources");
    }
    return {};
  }
#else
  (void)challenge;
  if (log) {
    log("[licence] Xbox proof unavailable: this client was built without the GDK headers");
  }
  return {};
#endif
}

}  // namespace kcds::runtime::identity
