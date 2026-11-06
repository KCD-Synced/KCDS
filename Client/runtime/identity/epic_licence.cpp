#include "epic_licence.hpp"

#include <Windows.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

#include "MinHook.h"

namespace kcds::runtime::identity {
namespace {

// Minimal x64 ABI declarations, not an EOS implementation. Verified against
// Epic's eos_ecom_types.h (QueryOwnershipToken API v2), eos_auth.h,
// eos_common.h, eos_base.h and eos_sdk.h public declarations. Their published
// header copy is at github.com/SpecialKO/SpecialK/tree/main/depends/include/EOS.
// The official Ecom guide describes the same ownership-token operation:
// dev.epicgames.com/docs/epic-games-store/services/ecom/ecom-overview
// EOS_CALL is the default convention on Win64; SDK structs use pack(8).
using Platform = void*;
using Auth = void*;
using Ecom = void*;
using Account = void*;

#pragma pack(push, 8)
struct OwnershipOptions {
  std::int32_t api_version;
  Account local_user;
  const char** catalog_items;
  std::uint32_t catalog_item_count;
  const char* catalog_namespace;
};

struct OwnershipResult {
  std::int32_t result;
  void* client_data;
  Account local_user;
  const char* token;
};
#pragma pack(pop)

static_assert(sizeof(void*) == 8, "The EOS licence provider supports Win64 only");
static_assert(sizeof(OwnershipOptions) == 40);
static_assert(offsetof(OwnershipOptions, local_user) == 8);
static_assert(offsetof(OwnershipOptions, catalog_items) == 16);
static_assert(offsetof(OwnershipOptions, catalog_item_count) == 24);
static_assert(offsetof(OwnershipOptions, catalog_namespace) == 32);
static_assert(sizeof(OwnershipResult) == 32);
static_assert(offsetof(OwnershipResult, client_data) == 8);
static_assert(offsetof(OwnershipResult, local_user) == 16);
static_assert(offsetof(OwnershipResult, token) == 24);

using PlatformTick = void (*)(Platform);
using PlatformRelease = void (*)(Platform);
using GetAuth = Auth (*)(Platform);
using GetEcom = Ecom (*)(Platform);
using AccountCount = std::int32_t (*)(Auth);
using AccountByIndex = Account (*)(Auth, std::int32_t);
using LoginStatus = std::int32_t (*)(Auth, Account);
using OperationComplete = std::int32_t (*)(std::int32_t);
using OwnershipCallback = void (*)(const OwnershipResult*);
using QueryOwnership = void (*)(Ecom, const OwnershipOptions*, void*, OwnershipCallback);
using AccountToString = std::int32_t (*)(Account, char*, std::int32_t*);

constexpr std::int32_t kOwnershipAPIVersion = 2;
constexpr std::int32_t kLoggedIn = 2;
constexpr std::int32_t kSuccess = 0;
constexpr std::size_t kMaxTokenBytes = 48 * 1024;
constexpr char kNamespace[] = "e5429798296d4f78b8090f243b00cf11";
constexpr char kCatalogItem[] = "d7a97fd7d48c4963bdae64681e4cd874";
constexpr auto kDispatchWait = std::chrono::seconds(2);
constexpr auto kTokenWait = std::chrono::seconds(10);

struct SDK {
  PlatformTick tick = nullptr;
  PlatformRelease release = nullptr;
  GetAuth auth = nullptr;
  GetEcom ecom = nullptr;
  AccountCount account_count = nullptr;
  AccountByIndex account_at = nullptr;
  LoginStatus login_status = nullptr;
  OperationComplete operation_complete = nullptr;
  QueryOwnership query = nullptr;
  // Optional: the account id as text, for bug reports (epic_account_id).
  AccountToString account_text = nullptr;
};

struct LivePlatform {
  Platform handle = nullptr;
  std::uint64_t epoch = 0;
  DWORD thread = 0;
};

struct Request {
  std::uintptr_t id = 0;
  Platform platform = nullptr;
  std::uint64_t epoch = 0;
  Account account = nullptr;
  OwnershipOptions options{};
  const char* items[1] = {kCatalogItem};
  bool started = false;
  bool done = false;
  bool abandoned = false;
  std::int32_t result = -1;
  std::string token;
};

struct State {
  std::mutex install_mutex;
  std::mutex mutex;
  // Serialize observed Tick/Release calls, including release from a callback.
  // A join worker never acquires this mutex or calls SDK functions itself.
  std::recursive_mutex sdk_mutex;
  std::condition_variable changed;
  SDK sdk;
  bool installed = false;
  std::uintptr_t next_request = 0;
  std::uint64_t next_epoch = 0;
  std::array<LivePlatform, 8> platforms{};
  std::shared_ptr<Request> pending;
  // The logged-in account's id, once read in a tick ("" until then).
  std::string account_id;
};

State& state() {
  // Hooks and late SDK callbacks may run during process shutdown. The bounded
  // state and hooked modules deliberately live until process exit.
  static State* value = new State;
  return *value;
}

bool platform_live_locked(State& current, Platform handle, std::uint64_t epoch) {
  for (const auto& platform : current.platforms) {
    if (platform.handle == handle && platform.epoch == epoch) {
      return true;
    }
  }
  return false;
}

void fail_request(std::uintptr_t id) noexcept {
  try {
    auto& current = state();
    std::lock_guard<std::mutex> lock(current.mutex);
    if (current.pending && current.pending->id == id) {
      current.pending->done = true;
      current.pending->token.clear();
      current.pending.reset();
      current.changed.notify_all();
    }
  } catch (...) {
    // Exceptions must not escape through the C SDK callback boundary.
  }
}

void ownership_complete(const OwnershipResult* result) noexcept {
  if (!result) {
    return;
  }
  // ClientData is a monotonically increasing opaque integer cookie, never a
  // pointer to caller-owned memory. A callback after timeout/release cannot
  // dereference a freed request, or match a newly allocated request by address.
  const auto id = reinterpret_cast<std::uintptr_t>(result->client_data);
  try {
    auto& current = state();
    if (!current.sdk.operation_complete(result->result)) {
      return;
    }
    std::lock_guard<std::mutex> lock(current.mutex);
    auto request = current.pending;
    if (!request || request->id != id || !request->started) {
      return;
    }
    if (!request->abandoned && result->result == kSuccess &&
        result->local_user == request->account &&
        platform_live_locked(current, request->platform, request->epoch) && result->token) {
      const auto length = strnlen_s(result->token, kMaxTokenBytes + 1);
      if (length > 0 && length <= kMaxTokenBytes) {
        // The SDK owns this string only for the duration of the callback.
        request->token.assign(result->token, length);
      }
    }
    request->result = result->result;
    request->done = true;
    current.pending.reset();
    current.changed.notify_all();
  } catch (...) {
    fail_request(id);
  }
}

// The logged-in account's id as text, read in the game's own tick (the SDK's thread).
void remember_account_id(State& current, Platform handle) {
  const auto auth = current.sdk.auth(handle);
  if (!auth || current.sdk.account_count(auth) != 1) {
    return;
  }
  const auto account = current.sdk.account_at(auth, 0);
  if (!account || current.sdk.login_status(auth, account) != kLoggedIn) {
    return;
  }
  char text[64] = {};
  std::int32_t length = sizeof(text);
  if (current.sdk.account_text(account, text, &length) != kSuccess) {
    return;
  }
  std::lock_guard<std::mutex> lock(current.mutex);
  current.account_id = text;
}

void dispatch_on_tick(Platform handle) {
  auto& current = state();
  std::shared_ptr<Request> request;
  std::uint64_t epoch = 0;
  bool need_account_id = false;
  {
    std::lock_guard<std::mutex> lock(current.mutex);
    if (!current.installed || !handle) {
      return;
    }
    LivePlatform* observed = nullptr;
    for (auto& platform : current.platforms) {
      if (platform.handle == handle) {
        observed = &platform;
        break;
      }
    }
    if (!observed) {
      for (auto& platform : current.platforms) {
        if (!platform.handle) {
          observed = &platform;
          platform.handle = handle;
          platform.epoch = ++current.next_epoch;
          break;
        }
      }
    }
    if (!observed) {
      return;
    }
    observed->thread = GetCurrentThreadId();
    epoch = observed->epoch;
    request = current.pending;
    need_account_id = current.account_id.empty() && current.sdk.account_text;
  }
  if (need_account_id) {
    remember_account_id(current, handle);
  }
  {
    std::lock_guard<std::mutex> lock(current.mutex);
    if (!request || request->started || request->abandoned || request->done) {
      return;
    }
  }

  // These operations execute only inside the game's existing platform tick,
  // on its thread and with its authenticated account and client policy.
  const auto auth = current.sdk.auth(handle);
  const auto ecom = current.sdk.ecom(handle);
  if (!auth || !ecom || current.sdk.account_count(auth) != 1) {
    return;
  }
  const auto account = current.sdk.account_at(auth, 0);
  if (!account || current.sdk.login_status(auth, account) != kLoggedIn) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(current.mutex);
    if (current.pending != request || request->abandoned || request->done ||
        !platform_live_locked(current, handle, epoch)) {
      return;
    }
    request->platform = handle;
    request->epoch = epoch;
    request->account = account;
    request->options = {kOwnershipAPIVersion, account, request->items, 1, kNamespace};
    request->started = true;
    current.changed.notify_all();
  }
  current.sdk.query(ecom, &request->options, reinterpret_cast<void*>(request->id),
                    ownership_complete);
}

void hooked_tick(Platform handle) {
  auto& current = state();
  std::lock_guard<std::recursive_mutex> sdk_lock(current.sdk_mutex);
  try {
    dispatch_on_tick(handle);
  } catch (...) {
    std::uintptr_t id = 0;
    {
      std::lock_guard<std::mutex> lock(current.mutex);
      if (current.pending) {
        id = current.pending->id;
      }
    }
    fail_request(id);
  }
  current.sdk.tick(handle);
}

void hooked_release(Platform handle) {
  auto& current = state();
  std::lock_guard<std::recursive_mutex> sdk_lock(current.sdk_mutex);
  {
    std::lock_guard<std::mutex> lock(current.mutex);
    for (auto& platform : current.platforms) {
      if (platform.handle == handle) {
        platform = {};
      }
    }
    if (current.pending && (!current.pending->started || current.pending->platform == handle)) {
      current.pending->abandoned = true;
      current.pending->done = true;
      current.pending->token.clear();
      current.pending.reset();
      current.changed.notify_all();
    }
  }
  current.sdk.release(handle);
}

template <typename Function>
Function sdk_export(HMODULE module, const char* name) {
  return reinterpret_cast<Function>(GetProcAddress(module, name));
}

}  // namespace

bool install_epic_licence_hooks(Logger log) {
  auto& current = state();
  std::lock_guard<std::mutex> installation(current.install_mutex);
  {
    std::lock_guard<std::mutex> lock(current.mutex);
    if (current.installed) {
      return true;
    }
  }
  const auto module = GetModuleHandleW(L"EOSSDK-Win64-Shipping.dll");
  if (!module) {
    if (log) {
      log("LICENCE Epic unavailable: the game has no loaded EOS SDK");
    }
    return false;
  }
  SDK sdk;
  auto tick_target = GetProcAddress(module, "EOS_Platform_Tick");
  auto release_target = GetProcAddress(module, "EOS_Platform_Release");
  sdk.auth = sdk_export<GetAuth>(module, "EOS_Platform_GetAuthInterface");
  sdk.ecom = sdk_export<GetEcom>(module, "EOS_Platform_GetEcomInterface");
  sdk.account_count = sdk_export<AccountCount>(module, "EOS_Auth_GetLoggedInAccountsCount");
  sdk.account_at = sdk_export<AccountByIndex>(module, "EOS_Auth_GetLoggedInAccountByIndex");
  sdk.login_status = sdk_export<LoginStatus>(module, "EOS_Auth_GetLoginStatus");
  sdk.operation_complete = sdk_export<OperationComplete>(module, "EOS_EResult_IsOperationComplete");
  sdk.query = sdk_export<QueryOwnership>(module, "EOS_Ecom_QueryOwnershipToken");
  sdk.account_text = sdk_export<AccountToString>(module, "EOS_EpicAccountId_ToString");
  if (!tick_target || !release_target || !sdk.auth || !sdk.ecom || !sdk.account_count ||
      !sdk.account_at || !sdk.login_status || !sdk.operation_complete || !sdk.query) {
    if (log) {
      log("LICENCE Epic unavailable: required public EOS exports are missing");
    }
    return false;
  }
  const auto init = MH_Initialize();
  if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
    return false;
  }
  auto tick_address = reinterpret_cast<void*>(tick_target);
  auto release_address = reinterpret_cast<void*>(release_target);
  if (MH_CreateHook(release_address, reinterpret_cast<void*>(hooked_release),
                    reinterpret_cast<void**>(&sdk.release)) != MH_OK) {
    return false;
  }
  if (MH_CreateHook(tick_address, reinterpret_cast<void*>(hooked_tick),
                    reinterpret_cast<void**>(&sdk.tick)) != MH_OK) {
    MH_RemoveHook(release_address);
    return false;
  }
  HMODULE pinned_sdk = nullptr;
  HMODULE pinned_client = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"EOSSDK-Win64-Shipping.dll",
                          &pinned_sdk) ||
      !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(&ownership_complete), &pinned_client)) {
    MH_RemoveHook(tick_address);
    MH_RemoveHook(release_address);
    return false;
  }
  current.sdk = sdk;
  // Release is observed before any tick can dispatch work against a handle.
  if (MH_EnableHook(release_address) != MH_OK) {
    MH_RemoveHook(tick_address);
    MH_RemoveHook(release_address);
    return false;
  }
  if (MH_EnableHook(tick_address) != MH_OK) {
    MH_RemoveHook(tick_address);
    // Release may already be executing on a game thread. Keep its forwarding
    // hook/trampoline alive rather than reclaim code under that thread. The
    // provider remains unavailable; no ownership request can have started.
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(current.mutex);
    current.installed = true;
  }
  return true;
}

std::string epic_account_id() {
  auto& current = state();
  std::lock_guard<std::mutex> lock(current.mutex);
  return current.account_id;
}

std::string epic_licence_token(Logger log) {
  if (!install_epic_licence_hooks(log)) {
    return {};
  }
  auto& current = state();
  std::unique_lock<std::mutex> lock(current.mutex);
  for (const auto& platform : current.platforms) {
    if (platform.handle && platform.thread == GetCurrentThreadId()) {
      lock.unlock();
      if (log) {
        log("LICENCE Epic refused a blocking request on the game's EOS tick thread");
      }
      return {};
    }
  }
  if (current.pending || current.next_request == (std::numeric_limits<std::uintptr_t>::max)()) {
    lock.unlock();
    if (log) {
      log("LICENCE Epic unavailable: a previous ownership query is still pending");
    }
    return {};
  }
  auto request = std::make_shared<Request>();
  request->id = ++current.next_request;
  current.pending = request;
  const auto deadline = std::chrono::steady_clock::now() + kTokenWait;
  if (!current.changed.wait_for(lock, kDispatchWait,
                                [&] { return request->started || request->done; })) {
    request->abandoned = true;
    if (current.pending == request) {
      current.pending.reset();
    }
    lock.unlock();
    if (log) {
      log("LICENCE Epic unavailable: no ticking platform with one authenticated game account");
    }
    return {};
  }
  if (!current.changed.wait_until(lock, deadline, [&] { return request->done; })) {
    request->abandoned = true;
    request->token.clear();
    // Keep at most this one outstanding SDK request until completion or release.
    // A timeout neither cancels the game's SDK work nor frees callback memory.
    lock.unlock();
    if (log) {
      log("LICENCE Epic ownership query timed out; late results will be discarded");
    }
    return {};
  }
  auto token = std::move(request->token);
  const auto result = request->result;
  lock.unlock();
  if (token.empty() && log) {
    log("LICENCE Epic ownership proof unavailable (EOS result=%d)", result);
  }
  return token;
}

}  // namespace kcds::runtime::identity
