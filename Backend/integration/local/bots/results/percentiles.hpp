#pragma once
// Exact percentiles of a run's per-bot numbers (kbps, corrections a minute): nearest rank over the
// sorted values, so p50 of 1..100 is 50 and p95 is 95. The durations of many samples (snapshot
// age, tick time) use metrics::DurationHistogram instead.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace kcd2s::local::bots {

struct Summary {
  std::size_t count = 0;
  double mean = 0.0;
  double p50 = 0.0;
  double p95 = 0.0;
  double p99 = 0.0;
  double max = 0.0;
};

// Nearest-rank percentile `p` (0..100) of sorted values; 0 for none.
inline double percentile_of_sorted(const std::vector<double>& sorted, double p) {
  if (sorted.empty()) {
    return 0.0;
  }
  const double rank = std::ceil(p / 100.0 * double(sorted.size()));
  const std::size_t index = rank < 1.0 ? 0 : std::min(sorted.size() - 1, std::size_t(rank) - 1);
  return sorted[index];
}

inline Summary summarize(std::vector<double> values) {
  Summary summary;
  summary.count = values.size();
  if (values.empty()) {
    return summary;
  }
  std::sort(values.begin(), values.end());
  double total = 0.0;
  for (const double value : values) {
    total += value;
  }
  summary.mean = total / double(values.size());
  summary.p50 = percentile_of_sorted(values, 50.0);
  summary.p95 = percentile_of_sorted(values, 95.0);
  summary.p99 = percentile_of_sorted(values, 99.0);
  summary.max = values.back();
  return summary;
}

}  // namespace kcd2s::local::bots
