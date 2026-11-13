'use strict';

// The server's metrics (the native server's KCDS_METRICS interval, metrics/server_metrics.hpp) in
// the Prometheus text format, for GET /metrics. Every number of the interval becomes a gauge named
// after its path: tick.work.p95_ms is kcds_tick_work_ms{stat="p95"}, udp.interval.bytes_out is
// kcds_udp_interval_bytes_out. Counters that only grow keep their _total name. Players and
// uptime come from the resource host itself, so /metrics answers before the first interval too.

// A histogram summary's fields (json_object.hpp describe()): written as one gauge with a label.
const SUMMARY_STATS = new Map([
  ['mean_ms', 'mean'],
  ['p50_ms', 'p50'],
  ['p95_ms', 'p95'],
  ['p99_ms', 'p99'],
  ['max_ms', 'max'],
]);

function metricName(parts) {
  return `kcds_${parts.join('_')}`.replace(/[^a-zA-Z0-9_]/g, '_');
}

// The lines of one object, depth first, in the order of its keys.
function flatten(object, parts, lines) {
  const keys = Object.keys(object);
  const summary = keys.some((key) => SUMMARY_STATS.has(key));
  for (const key of keys) {
    const value = object[key];
    if (value !== null && typeof value === 'object' && !Array.isArray(value)) {
      flatten(value, [...parts, key], lines);
      continue;
    }
    if (typeof value === 'boolean') {
      lines.push([metricName([...parts, key]), '', value ? 1 : 0]);
      continue;
    }
    if (typeof value !== 'number' || !Number.isFinite(value)) {
      continue;
    }
    if (summary && SUMMARY_STATS.has(key)) {
      lines.push([metricName([...parts, 'ms']), `{stat="${SUMMARY_STATS.get(key)}"}`, value]);
      continue;
    }
    lines.push([metricName([...parts, key]), '', value]);
  }
}

// The text of GET /metrics: `metrics` the last interval (an object, or null before the first),
// `extra` the host's own gauges { name: value }, `labelled` its gauges with labels
// [[name, { label: value }, number], ...].
function renderPrometheus(metrics, extra = {}, labelled = []) {
  const lines = [];
  for (const [name, value] of Object.entries(extra)) {
    lines.push([metricName([name]), '', value]);
  }
  for (const [name, labels, value] of labelled) {
    const text = Object.entries(labels)
      .map(([label, labelValue]) => `${label}="${String(labelValue).replace(/["\\\n]/g, '_')}"`)
      .join(',');
    lines.push([metricName([name]), `{${text}}`, value]);
  }
  if (metrics && typeof metrics === 'object') {
    const { schema, unix_ms: unixMs, ...rest } = metrics;
    void schema;
    void unixMs;
    flatten(rest, [], lines);
  }
  const typed = new Set();
  const out = [];
  for (const [name, labels, value] of lines) {
    if (!typed.has(name)) {
      typed.add(name);
      out.push(`# TYPE ${name} ${name.endsWith('_total') ? 'counter' : 'gauge'}`);
    }
    out.push(`${name}${labels} ${value}`);
  }
  return `${out.join('\n')}\n`;
}

// The server list's health from the last interval: tick time and missed ticks, or null before the
// first interval.
function healthOf(metrics) {
  const tick = metrics?.tick;
  if (!tick || typeof tick !== 'object') {
    return null;
  }
  const interval = Number(metrics.interval_s) > 0 ? Number(metrics.interval_s) : 0;
  const missed = Number(tick.missed_deadlines) || 0;
  return {
    tickMs: Number(tick.work?.p95_ms) || 0,
    missedPerMinute: interval > 0 ? Math.round((missed * 60) / interval) : 0,
    cpuPercent: Math.round(Number(metrics.process?.cpu_percent_of_one_core) || 0),
  };
}

module.exports = { renderPrometheus, healthOf };
