#pragma once
// Fixed-memory duration histogram for opt-in load metrics. Bucket widths grow with the value so a
// reported percentile is at most 10 us above the true sample below 1 ms, 100 us below 100 ms and
// 10 ms below 10 s. Larger samples share one overflow bucket; the exact maximum is always kept.
// Portable: used by the Windows server/probe and by the Linux unit tests.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace kcd2s::local::metrics {

class DurationHistogram {
 public:
  void add(double milliseconds) {
    if (!std::isfinite(milliseconds) || milliseconds < 0.0) {
      milliseconds = 0.0;
    }
    buckets_[bucket_index(milliseconds)] += 1;
    count_ += 1;
    sum_ += milliseconds;
    if (milliseconds > max_) {
      max_ = milliseconds;
    }
  }

  void merge(const DurationHistogram& other) {
    for (std::size_t i = 0; i < kBucketCount; ++i) {
      buckets_[i] += other.buckets_[i];
    }
    count_ += other.count_;
    sum_ += other.sum_;
    if (other.max_ > max_) {
      max_ = other.max_;
    }
  }

  void reset() {
    buckets_.fill(0);
    count_ = 0;
    sum_ = 0.0;
    max_ = 0.0;
  }

  std::uint64_t count() const {
    return count_;
  }

  static constexpr std::size_t bucket_count() {
    return kBucketCount;
  }
  // Samples in bucket `index` (exposed so separate processes can merge their histograms).
  std::uint64_t bucket(std::size_t index) const {
    return index < kBucketCount ? buckets_[index] : 0;
  }
  double sum() const {
    return sum_;
  }

  double max() const {
    return max_;
  }

  double mean() const {
    if (count_ == 0) {
      return 0.0;
    }
    return sum_ / static_cast<double>(count_);
  }

  // Nearest-rank percentile, `p` in [0, 100]. Reported as the upper bound of the bucket holding
  // the ranked sample, clamped to the exact maximum. Returns 0 for an empty histogram.
  double percentile(double p) const {
    if (count_ == 0) {
      return 0.0;
    }
    if (p < 0.0) {
      p = 0.0;
    }
    if (p > 100.0) {
      p = 100.0;
    }
    auto rank = static_cast<std::uint64_t>(std::ceil(p / 100.0 * static_cast<double>(count_)));
    if (rank == 0) {
      rank = 1;
    }
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < kBucketCount; ++i) {
      seen += buckets_[i];
      if (seen >= rank) {
        const double upper = bucket_upper_ms(i);
        return upper < max_ ? upper : max_;
      }
    }
    return max_;
  }

  // Exposed for tests: the exclusive upper bound of bucket `index` in milliseconds.
  static double bucket_upper_ms(std::size_t index) {
    if (index < kFineBuckets) {
      return static_cast<double>((index + 1) * kFineWidthUs) / 1000.0;
    }
    if (index < kFineBuckets + kMediumBuckets) {
      const std::size_t offset = index - kFineBuckets;
      return static_cast<double>(kFineLimitUs + (offset + 1) * kMediumWidthUs) / 1000.0;
    }
    if (index < kFineBuckets + kMediumBuckets + kCoarseBuckets) {
      const std::size_t offset = index - kFineBuckets - kMediumBuckets;
      return static_cast<double>(kMediumLimitUs + (offset + 1) * kCoarseWidthUs) / 1000.0;
    }
    return std::numeric_limits<double>::infinity();
  }

  static std::size_t bucket_index(double milliseconds) {
    const double microseconds = milliseconds * 1000.0;
    if (microseconds < static_cast<double>(kFineLimitUs)) {
      return static_cast<std::size_t>(microseconds) / kFineWidthUs;
    }
    if (microseconds < static_cast<double>(kMediumLimitUs)) {
      const auto offset = static_cast<std::size_t>(microseconds) - kFineLimitUs;
      return kFineBuckets + offset / kMediumWidthUs;
    }
    if (microseconds < static_cast<double>(kCoarseLimitUs)) {
      const auto offset = static_cast<std::size_t>(microseconds) - kMediumLimitUs;
      return kFineBuckets + kMediumBuckets + offset / kCoarseWidthUs;
    }
    return kBucketCount - 1;
  }

 private:
  static constexpr std::size_t kFineWidthUs = 10;
  static constexpr std::size_t kFineLimitUs = 1000;
  static constexpr std::size_t kMediumWidthUs = 100;
  static constexpr std::size_t kMediumLimitUs = 100000;
  static constexpr std::size_t kCoarseWidthUs = 10000;
  static constexpr std::size_t kCoarseLimitUs = 10000000;
  static constexpr std::size_t kFineBuckets = kFineLimitUs / kFineWidthUs;
  static constexpr std::size_t kMediumBuckets = (kMediumLimitUs - kFineLimitUs) / kMediumWidthUs;
  static constexpr std::size_t kCoarseBuckets = (kCoarseLimitUs - kMediumLimitUs) / kCoarseWidthUs;
  static constexpr std::size_t kBucketCount = kFineBuckets + kMediumBuckets + kCoarseBuckets + 1;

  std::array<std::uint64_t, kBucketCount> buckets_{};
  std::uint64_t count_ = 0;
  double sum_ = 0.0;
  double max_ = 0.0;
};

}  // namespace kcd2s::local::metrics
