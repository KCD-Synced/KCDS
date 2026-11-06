// HTTP transport of the master-server client on Linux and other POSIX systems: the
// same request as http_win.hpp, with libcurl. HTTPS, or plain HTTP to a loopback host for a
// development master. No redirects, cookies or credentials. Header only.
#pragma once

#ifndef _WIN32

#include <curl/curl.h>

#include <cstddef>
#include <mutex>
#include <string>

namespace kcd2s::platform::http_curl {

struct Result {
  int status = 0;     // HTTP status, 0 when no response arrived
  std::string body;   // response body, at most max_bytes
  std::string error;  // the failing step and curl's message, empty on a response
};

namespace detail {

inline void global_init() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct Sink {
  std::string* body = nullptr;
  std::size_t max_bytes = 0;
  bool too_large = false;
};

inline std::size_t collect(char* data, std::size_t size, std::size_t count, void* user) {
  auto* sink = static_cast<Sink*>(user);
  const std::size_t bytes = size * count;
  if (sink->body->size() + bytes > sink->max_bytes) {
    sink->too_large = true;
    return 0;  // makes curl stop with CURLE_WRITE_ERROR
  }
  sink->body->append(data, bytes);
  return bytes;
}

// https://host..., or http:// only to a loopback host.
inline bool allowed(const std::string& url) {
  if (url.rfind("https://", 0) == 0) {
    return url.find('@') == std::string::npos;
  }
  for (const char* prefix : {"http://localhost", "http://127.0.0.1", "http://[::1]"}) {
    const std::string start(prefix);
    if (url.rfind(start, 0) == 0) {
      const char next = url.size() > start.size() ? url[start.size()] : '/';
      return next == '/' || next == ':';
    }
  }
  return false;
}

}  // namespace detail

// Sends one request. method is "GET" or "POST"; body is sent as application/json when not empty.
inline Result request(const char* method, const std::string& url, const std::string& body,
                      long timeout_ms, std::size_t max_bytes) {
  Result result;
  if (!detail::allowed(url)) {
    result.error = "url refused (https required, http only to loopback)";
    return result;
  }
  detail::global_init();
  CURL* curl = curl_easy_init();
  if (!curl) {
    result.error = "open";
    return result;
  }
  detail::Sink sink{&result.body, max_bytes, false};
  curl_slist* headers = nullptr;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "kcds-master-client/1");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, detail::collect);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  if (!body.empty()) {
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, long(body.size()));
  }
  const CURLcode code = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  result.status = int(status);
  if (sink.too_large) {
    result.error = "response too large";
    result.body.clear();
  } else if (code != CURLE_OK) {
    result.error = std::string("request: ") + curl_easy_strerror(code);
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return result;
}

}  // namespace kcd2s::platform::http_curl

#endif  // !_WIN32
