#pragma once
// Portable settings shared by the opt-in server metrics and their unit tests.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>

namespace kcd2s::local::metrics {

// Parses the KCDS_SERVER_METRICS value: a positive reporting interval in seconds (at most one
// hour). Anything else, including an unset or empty variable, leaves metrics disabled.
inline std::optional<double> parse_metrics_interval(const char* text) {
  if (!text || !*text) {
    return std::nullopt;
  }
  char* end = nullptr;
  const double seconds = std::strtod(text, &end);
  if (end == text || *end != '\0' || !(seconds > 0.0) || seconds > 3600.0) {
    return std::nullopt;
  }
  return seconds;
}

// Wall-clock timestamp shared by the server and probe lines so a report can align their windows.
inline std::int64_t unix_milliseconds() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

}  // namespace kcd2s::local::metrics
