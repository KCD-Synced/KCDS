// Windows HTTP transport of the master-server client: one bounded request with
// WinHTTP, shared by the game client (GET /servers) and the server (POST /heartbeat). HTTPS, or
// plain HTTP to a loopback host for a development master. No redirects, cookies or credentials.
// Header only and Windows only; the portable masterclient code never includes it.
#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winhttp.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <string>

namespace kcd2s::platform::http_win {

struct Result {
  int status = 0;     // HTTP status, 0 when no response arrived
  std::string body;   // response body, at most max_bytes
  std::string error;  // the failing step and its Windows error, empty on a response
};

namespace detail {

class Handle {
 public:
  explicit Handle(HINTERNET handle) : handle_(handle) {
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() {
    if (handle_) {
      WinHttpCloseHandle(handle_);
    }
  }
  HINTERNET get() const {
    return handle_;
  }
  HINTERNET release() {
    const HINTERNET handle = handle_;
    handle_ = nullptr;
    return handle;
  }

 private:
  HINTERNET handle_;
};

inline Result failure(const char* step) {
  Result result;
  result.error = std::string(step) + " failed (error " + std::to_string(GetLastError()) + ")";
  return result;
}

inline bool loopback(const std::wstring& host) {
  return host == L"localhost" || host == L"127.0.0.1" || host == L"[::1]" || host == L"::1";
}

// WinHTTP can still use the context and I/O buffers after cancellation. The caller and the
// callback registration each own a reference; HANDLE_CLOSING releases the latter, final lease.
struct AsyncState {
  explicit AsyncState(const std::string& body) : post_body(body) {
  }
  ~AsyncState() {
    if (connection) {
      WinHttpCloseHandle(connection);
    }
    if (session) {
      WinHttpCloseHandle(session);
    }
  }
  void retain() {
    references.fetch_add(1, std::memory_order_relaxed);
  }
  void release() {
    if (references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      delete this;
    }
  }

  std::atomic<unsigned> references{1};
  std::mutex mutex;
  std::condition_variable changed;
  DWORD expected = 0;
  DWORD error = 0;
  DWORD read_size = 0;
  bool completed = false;
  bool closed = false;
  HINTERNET session = nullptr;
  HINTERNET connection = nullptr;
  HINTERNET request = nullptr;
  std::string post_body;
  std::array<char, 8192> read_buffer{};
};

inline void CALLBACK request_callback(HINTERNET, DWORD_PTR context, DWORD status, void* information,
                                      DWORD information_length) noexcept {
  auto* state = reinterpret_cast<AsyncState*>(context);
  if (!state) {
    return;
  }
  state->retain();
  {
    std::lock_guard lock(state->mutex);
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
      state->closed = true;
      state->completed = true;
    } else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
      state->error = ERROR_INVALID_DATA;
      if (information && information_length == sizeof(WINHTTP_ASYNC_RESULT)) {
        const auto* result = static_cast<const WINHTTP_ASYNC_RESULT*>(information);
        if (result->dwError != ERROR_SUCCESS) {
          state->error = result->dwError;
        }
      }
      state->completed = true;
    } else if (status == state->expected) {
      if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
        if (information_length > state->read_buffer.size()) {
          state->error = ERROR_INVALID_DATA;
        } else {
          state->read_size = information_length;
        }
      }
      state->completed = true;
    }
  }
  state->changed.notify_all();
  if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
    state->release();
  }
  state->release();
}

class AsyncRequest {
 public:
  explicit AsyncRequest(const std::string& body) : state_(new AsyncState(body)) {
  }
  AsyncRequest(const AsyncRequest&) = delete;
  AsyncRequest& operator=(const AsyncRequest&) = delete;
  ~AsyncRequest() {
    // Only this caller issues WinHTTP operations, and each starting API has returned before
    // destruction. Closing an async request now safely cancels a pending completion.
    if (state_->request) {
      WinHttpCloseHandle(state_->request);
    }
    state_->release();
  }
  AsyncState& state() {
    return *state_;
  }
  bool install_callback() {
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(state_);
    if (!WinHttpSetOption(state_->request, WINHTTP_OPTION_CONTEXT_VALUE, &context,
                          sizeof(context))) {
      return false;
    }
    state_->retain();
    const DWORD notifications = WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE |
                                WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE |
                                WINHTTP_CALLBACK_FLAG_READ_COMPLETE |
                                WINHTTP_CALLBACK_FLAG_REQUEST_ERROR | WINHTTP_CALLBACK_FLAG_HANDLES;
    if (WinHttpSetStatusCallback(state_->request, request_callback, notifications, 0) ==
        WINHTTP_INVALID_STATUS_CALLBACK) {
      state_->release();
      return false;
    }
    return true;
  }
  template <typename Start>
  bool perform(DWORD expected, std::chrono::steady_clock::time_point deadline, Start start) {
    if (std::chrono::steady_clock::now() >= deadline) {
      SetLastError(ERROR_TIMEOUT);
      return false;
    }
    {
      std::lock_guard lock(state_->mutex);
      state_->expected = expected;
      state_->completed = false;
      state_->read_size = 0;
    }
    // A completion can arrive inline. Do not hold the mutex or start another operation from
    // the callback; the initiating call and its completion must both finish first.
    if (!start()) {
      return false;
    }
    std::unique_lock lock(state_->mutex);
    if (!state_->changed.wait_until(lock, deadline, [&] { return state_->completed; })) {
      SetLastError(ERROR_TIMEOUT);
      return false;
    }
    if (state_->closed || state_->error != ERROR_SUCCESS) {
      SetLastError(state_->error ? state_->error : ERROR_OPERATION_ABORTED);
      return false;
    }
    return true;
  }

 private:
  AsyncState* state_;
};

}  // namespace detail

// Sends one request. method is "GET" or "POST"; body is sent as application/json when not empty.
// plain_http_allowed: http:// to any host, for content whose bytes the caller checks against a
// hash it already trusts (a game server's resource files, named by their SHA-256); every other
// request needs https unless it goes to this machine.
inline Result request(const char* method, const std::string& url, const std::string& body,
                      DWORD timeout_ms, std::size_t max_bytes, bool plain_http_allowed = false) {
  using detail::failure;
  using detail::Handle;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  if (timeout_ms == 0) {
    SetLastError(ERROR_TIMEOUT);
    return failure("request");
  }
  if (url.size() > (std::numeric_limits<DWORD>::max)() ||
      body.size() > (std::numeric_limits<DWORD>::max)()) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return failure("request");
  }
  const std::wstring wide_url(url.begin(), url.end());
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.dwHostNameLength = DWORD(-1);
  parts.dwUrlPathLength = DWORD(-1);
  parts.dwExtraInfoLength = DWORD(-1);
  parts.dwUserNameLength = DWORD(-1);
  if (!WinHttpCrackUrl(wide_url.c_str(), DWORD(wide_url.size()), 0, &parts)) {
    return failure("url");
  }
  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
  path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
  if (parts.dwUserNameLength != 0 || host.empty() ||
      (!secure && !plain_http_allowed && !detail::loopback(host))) {
    Result refused;
    refused.error = "url refused (https required, http only to loopback)";
    return refused;
  }
  if (path.empty()) {
    path = L"/";
  }

  Handle session(WinHttpOpen(L"kcds-master-client/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
  if (!session.get()) {
    return failure("open");
  }
  const int timeout = timeout_ms > DWORD((std::numeric_limits<int>::max)())
                          ? (std::numeric_limits<int>::max)()
                          : int(timeout_ms);
  if (!WinHttpSetTimeouts(session.get(), timeout, timeout, timeout, timeout)) {
    return failure("timeouts");
  }
  Handle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
  if (!connection.get()) {
    return failure("connect");
  }
  const std::wstring verb(method, method + std::char_traits<char>::length(method));
  Handle request(WinHttpOpenRequest(connection.get(), verb.c_str(), path.c_str(), nullptr,
                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    secure ? WINHTTP_FLAG_SECURE : 0));
  if (!request.get()) {
    return failure("request");
  }
  DWORD features =
      WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
  if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &features,
                        sizeof(features))) {
    return failure("options");
  }
  detail::AsyncRequest operation(body);
  auto& state = operation.state();
  state.session = session.release();
  state.connection = connection.release();
  state.request = request.release();
  if (!operation.install_callback()) {
    return failure("callback");
  }
  const wchar_t* headers =
      body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : L"Content-Type: application/json\r\n";
  const DWORD header_length = body.empty() ? 0 : DWORD(-1);
  if (!operation.perform(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, deadline, [&] {
        return WinHttpSendRequest(
            state.request, headers, header_length,
            state.post_body.empty() ? WINHTTP_NO_REQUEST_DATA : state.post_body.data(),
            DWORD(state.post_body.size()), DWORD(state.post_body.size()),
            reinterpret_cast<DWORD_PTR>(&state));
      })) {
    return failure("send");
  }
  if (!operation.perform(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, deadline,
                         [&] { return WinHttpReceiveResponse(state.request, nullptr); })) {
    return failure("receive");
  }
  Result result;
  DWORD code = 0;
  DWORD size = sizeof(code);
  if (!WinHttpQueryHeaders(state.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX)) {
    return failure("status");
  }
  result.status = int(code);
  for (;;) {
    if (!operation.perform(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, deadline, [&] {
          // Async WinHTTP requires a null byte-count pointer; the callback supplies it.
          return WinHttpReadData(state.request, state.read_buffer.data(),
                                 DWORD(state.read_buffer.size()), nullptr);
        })) {
      Result broken = failure("read");
      broken.status = result.status;
      return broken;
    }
    const DWORD count = state.read_size;
    if (count == 0) {
      break;
    }
    if (count > max_bytes - result.body.size()) {
      result.error = "response too large";
      result.body.clear();
      return result;
    }
    result.body.append(state.read_buffer.data(), count);
  }
  return result;
}

}  // namespace kcd2s::platform::http_win

#endif  // _WIN32
