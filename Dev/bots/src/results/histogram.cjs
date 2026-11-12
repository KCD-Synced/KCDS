'use strict';

// The engine's duration histograms (metrics::DurationHistogram) in JavaScript: the same bucket
// bounds, merged by adding buckets, and the same nearest-rank percentile (the upper bound of the
// bucket that holds the ranked sample, never above the exact maximum). A merged histogram's
// percentiles are what one engine would have reported for all the samples.

const FINE_WIDTH_US = 10;
const FINE_LIMIT_US = 1000;
const MEDIUM_WIDTH_US = 100;
const MEDIUM_LIMIT_US = 100000;
const COARSE_WIDTH_US = 10000;
const COARSE_LIMIT_US = 10000000;
const FINE_BUCKETS = FINE_LIMIT_US / FINE_WIDTH_US;
const MEDIUM_BUCKETS = (MEDIUM_LIMIT_US - FINE_LIMIT_US) / MEDIUM_WIDTH_US;
const COARSE_BUCKETS = (COARSE_LIMIT_US - MEDIUM_LIMIT_US) / COARSE_WIDTH_US;

// The exclusive upper bound of bucket `index`, in milliseconds.
function bucketUpperMs(index) {
  if (index < FINE_BUCKETS) {
    return ((index + 1) * FINE_WIDTH_US) / 1000;
  }
  if (index < FINE_BUCKETS + MEDIUM_BUCKETS) {
    const offset = index - FINE_BUCKETS;
    return (FINE_LIMIT_US + (offset + 1) * MEDIUM_WIDTH_US) / 1000;
  }
  if (index < FINE_BUCKETS + MEDIUM_BUCKETS + COARSE_BUCKETS) {
    const offset = index - FINE_BUCKETS - MEDIUM_BUCKETS;
    return (MEDIUM_LIMIT_US + (offset + 1) * COARSE_WIDTH_US) / 1000;
  }
  return Infinity;
}

// An empty histogram in the engine's export form ({ count, sum_ms, max_ms, buckets }).
function emptyHistogram() {
  return { count: 0, sum_ms: 0, max_ms: 0, buckets: {} };
}

// The sum of histograms in the export form.
function mergeHistograms(histograms) {
  const merged = emptyHistogram();
  for (const histogram of histograms) {
    if (!histogram) {
      continue;
    }
    merged.count += histogram.count ?? 0;
    merged.sum_ms += histogram.sum_ms ?? 0;
    merged.max_ms = Math.max(merged.max_ms, histogram.max_ms ?? 0);
    for (const [index, count] of Object.entries(histogram.buckets ?? {})) {
      merged.buckets[index] = (merged.buckets[index] ?? 0) + count;
    }
  }
  return merged;
}

// Nearest-rank percentile `p` (0..100) of a histogram in the export form; 0 for an empty one.
function histogramPercentile(histogram, p) {
  if (!histogram || histogram.count === 0) {
    return 0;
  }
  const clamped = Math.min(100, Math.max(0, p));
  const rank = Math.max(1, Math.ceil((clamped / 100) * histogram.count));
  const indices = Object.keys(histogram.buckets)
    .map(Number)
    .sort((a, b) => a - b);
  let seen = 0;
  for (const index of indices) {
    seen += histogram.buckets[index];
    if (seen >= rank) {
      return Math.min(bucketUpperMs(index), histogram.max_ms);
    }
  }
  return histogram.max_ms;
}

// The summary the engine writes for a histogram ({ count, p50, p95, p99, max }).
function describeHistogram(histogram) {
  return {
    count: histogram.count,
    p50: histogramPercentile(histogram, 50),
    p95: histogramPercentile(histogram, 95),
    p99: histogramPercentile(histogram, 99),
    max: histogram.max_ms,
  };
}

// Nearest-rank percentile of plain numbers (the engine's percentiles.hpp).
function percentile(values, p) {
  if (values.length === 0) {
    return 0;
  }
  const sorted = [...values].sort((a, b) => a - b);
  const rank = Math.ceil((p / 100) * sorted.length);
  return sorted[Math.min(sorted.length - 1, Math.max(0, rank - 1))];
}

// { avg, p50, p95, max } of plain numbers.
function summarize(values) {
  if (values.length === 0) {
    return { avg: 0, p50: 0, p95: 0, max: 0 };
  }
  const total = values.reduce((sum, value) => sum + value, 0);
  return {
    avg: total / values.length,
    p50: percentile(values, 50),
    p95: percentile(values, 95),
    max: Math.max(...values),
  };
}

module.exports = {
  bucketUpperMs,
  emptyHistogram,
  mergeHistograms,
  histogramPercentile,
  describeHistogram,
  percentile,
  summarize,
};
