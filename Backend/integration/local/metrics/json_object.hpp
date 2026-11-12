#pragma once
// Minimal single-line JSON object writer for machine-readable metric lines. Keys are written in
// insertion order; non-finite numbers become null so every emitted line stays valid JSON.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "duration_histogram.hpp"

namespace kcd2s::local::metrics {

class JsonObject {
 public:
  JsonObject& add_count(std::string_view key, std::uint64_t value) {
    begin_member(key);
    body_ += std::to_string(value);
    return *this;
  }

  JsonObject& add_integer(std::string_view key, std::int64_t value) {
    begin_member(key);
    body_ += std::to_string(value);
    return *this;
  }

  JsonObject& add_number(std::string_view key, double value) {
    begin_member(key);
    if (!std::isfinite(value)) {
      body_ += "null";
      return *this;
    }
    char text[64];
    std::snprintf(text, sizeof(text), "%.6g", value);
    body_ += text;
    return *this;
  }

  JsonObject& add_flag(std::string_view key, bool value) {
    begin_member(key);
    body_ += value ? "true" : "false";
    return *this;
  }

  JsonObject& add_text(std::string_view key, std::string_view value) {
    begin_member(key);
    append_string(value);
    return *this;
  }

  JsonObject& add_object(std::string_view key, const JsonObject& value) {
    begin_member(key);
    body_ += value.str();
    return *this;
  }

  // A value that is JSON already (an array the caller wrote), put in as it is.
  JsonObject& add_raw(std::string_view key, std::string_view json) {
    begin_member(key);
    body_ += json;
    return *this;
  }

  std::string str() const {
    return "{" + body_ + "}";
  }

 private:
  void begin_member(std::string_view key) {
    if (!body_.empty()) {
      body_ += ',';
    }
    append_string(key);
    body_ += ':';
  }

  void append_string(std::string_view value) {
    body_ += '"';
    for (const char raw : value) {
      const auto c = static_cast<unsigned char>(raw);
      if (c == '"') {
        body_ += "\\\"";
      } else if (c == '\\') {
        body_ += "\\\\";
      } else if (c == '\n') {
        body_ += "\\n";
      } else if (c == '\r') {
        body_ += "\\r";
      } else if (c == '\t') {
        body_ += "\\t";
      } else if (c < 0x20) {
        char escaped[8];
        std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
        body_ += escaped;
      } else {
        body_ += raw;
      }
    }
    body_ += '"';
  }

  std::string body_;
};

// Lossless form of a histogram for merging across processes: the non-empty buckets as
// {"<bucket index>": count}, plus count, sum and max. Bucket bounds are DurationHistogram's.
inline JsonObject histogram_buckets(const DurationHistogram& histogram) {
  JsonObject buckets;
  for (std::size_t i = 0; i < DurationHistogram::bucket_count(); ++i) {
    if (histogram.bucket(i) != 0) {
      buckets.add_count(std::to_string(i), histogram.bucket(i));
    }
  }
  JsonObject object;
  object.add_count("count", histogram.count());
  object.add_number("sum_ms", histogram.sum());
  object.add_number("max_ms", histogram.max());
  object.add_object("buckets", buckets);
  return object;
}

// Standard summary of a duration histogram: count, mean, p50, p95, p99 and max in milliseconds.
inline JsonObject describe(const DurationHistogram& histogram) {
  JsonObject summary;
  summary.add_count("count", histogram.count());
  summary.add_number("mean_ms", histogram.mean());
  summary.add_number("p50_ms", histogram.percentile(50));
  summary.add_number("p95_ms", histogram.percentile(95));
  summary.add_number("p99_ms", histogram.percentile(99));
  summary.add_number("max_ms", histogram.max());
  return summary;
}

}  // namespace kcd2s::local::metrics
