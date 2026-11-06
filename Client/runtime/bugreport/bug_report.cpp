#include "bug_report.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <thread>

#include "kcd2s/core/crypto/sha256.hpp"
#include "kcd2s/platform/http_win.hpp"
#include "kcds/launcher/json.hpp"

#ifndef KCDS_BUG_REPORT_KEY
// Without CMake's key (a build of this file alone): a key the bot does not take.
#define KCDS_BUG_REPORT_KEY 0
#endif

namespace kcds::runtime::bug_report {
namespace {

constexpr std::array<std::uint8_t, 32> kKey = {KCDS_BUG_REPORT_KEY};
constexpr char kEndpoint[] = "https://kcdsynced.com/api/bug-report";
constexpr unsigned kTimeoutMs = 60000;
constexpr std::size_t kMaxAnswerBytes = 4096;

std::atomic<bool> sending{false};

// A JSON string with its quotes: quotes, backslashes and control characters escaped.
std::string json_string(const std::string& text) {
  std::string out = "\"";
  out.reserve(text.size() + 2);
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (byte < 0x20) {
      char escaped[8];
      std::snprintf(escaped, sizeof(escaped), "\\u%04x", unsigned(byte));
      out += escaped;
    } else {
      out += c;
    }
  }
  return out + "\"";
}

// The end of a file (at most kMaxLogBytes); "" when it cannot be read.
std::string tail_of(const std::wstring& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    return {};
  }
  const std::streamoff size = file.tellg();
  const std::streamoff start = size > std::streamoff(kMaxLogBytes) ? size - kMaxLogBytes : 0;
  file.seekg(start);
  std::string text(static_cast<std::size_t>(size - start), '\0');
  file.read(text.data(), std::streamsize(text.size()));
  text.resize(static_cast<std::size_t>(file.gcount()));
  return text;
}

std::string hex(const std::uint8_t* bytes, std::size_t count) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(count * 2);
  for (std::size_t i = 0; i < count; ++i) {
    out += digits[bytes[i] >> 4];
    out += digits[bytes[i] & 0x0F];
  }
  return out;
}

}  // namespace

std::string body_of(const Report& report) {
  std::string body = "{\"accept\":true";
  body += ",\"text\":" + json_string(report.text.substr(0, kMaxTextBytes));
  body += ",\"nickname\":" + json_string(report.nickname);
  body += ",\"version\":" + json_string(report.version);
  body += ",\"server\":" + json_string(report.server);
  body += ",\"map\":" + json_string(report.map);
  body += ",\"ids\":{\"steam\":" + json_string(report.steam_id);
  body += ",\"epic\":" + json_string(report.epic_id);
  body += ",\"discord\":" + json_string(report.discord_id);
  body += ",\"hwid\":" + json_string(report.hardware_id) + "}";
  body += ",\"files\":[";
  bool first = true;
  for (const auto& [name, path] : report.logs) {
    const std::string text = tail_of(path);
    if (text.empty()) {
      continue;
    }
    if (!first) {
      body += ',';
    }
    first = false;
    body += "{\"name\":" + json_string(name) + ",\"text\":" + json_string(text) + "}";
  }
  return body + "]}";
}

std::string signed_query(const std::string& body, long long t) {
  const std::string message = std::to_string(t) + "." + body;
  const auto digest = kcd2s::core::crypto::hmac_sha256(
      kKey.data(), kKey.size(), reinterpret_cast<const std::uint8_t*>(message.data()),
      message.size());
  return "?t=" + std::to_string(t) + "&s=" + hex(digest.data(), digest.size());
}

std::string outcome_message(const Outcome& outcome) {
  const bool ok = outcome.status == 200;
  return "{\"type\":\"bugReportResult\",\"ok\":" + std::string(ok ? "true" : "false") +
         ",\"status\":" + std::to_string(outcome.status) +
         ",\"error\":" + json_string(outcome.error) + "}";
}

namespace {

// The outcome of the bot's answer: its "error" when it refused the report.
Outcome outcome_of(const kcd2s::platform::http_win::Result& answer) {
  Outcome outcome;
  outcome.status = answer.status;
  outcome.error = answer.error;
  if (outcome.error.empty() && answer.status != 200) {
    namespace json = kcds::launcher::json;
    json::Limits limits;
    limits.max_bytes = kMaxAnswerBytes;
    limits.max_depth = 2;
    json::Value value;
    std::string ignored;
    if (json::parse(answer.body, limits, value, ignored) && value.member("error").is_string()) {
      outcome.error = value.member("error").string();
    } else {
      outcome.error = "HTTP " + std::to_string(answer.status);
    }
  }
  return outcome;
}

}  // namespace

bool send(Report report, Logger log, std::function<void(const Outcome&)> done) {
  if (sending.exchange(true)) {
    return false;
  }
  std::thread([report = std::move(report), log, done = std::move(done)] {
    const std::string body = body_of(report);
    const long long now = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    const auto answer =
        kcd2s::platform::http_win::request("POST", std::string(kEndpoint) + signed_query(body, now),
                                           body, kTimeoutMs, kMaxAnswerBytes);
    const Outcome outcome = outcome_of(answer);
    if (log) {
      log("BUGREPORT sent: HTTP %d, %zu bytes%s%s", outcome.status, body.size(),
          outcome.error.empty() ? "" : ", ", outcome.error.c_str());
    }
    sending.store(false);
    if (done) {
      done(outcome);
    }
  }).detach();
  return true;
}

}  // namespace kcds::runtime::bug_report
