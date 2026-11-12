'use strict';

// The report and the CI comparison of kcds-bots.

const test = require('node:test');
const assert = require('node:assert/strict');

const { renderReport, limitsOf, DEFAULT_THRESHOLDS } = require('../src/report/report.cjs');
const { compareRuns } = require('../src/compare/compare.cjs');

function result(overrides = {}) {
  return {
    schema: 1,
    kcds_version: '1.0.6',
    git_sha: '0123abcd',
    scenario: 'battle',
    seed: 42,
    started_at: '2026-11-12T10:00:00Z',
    environment: {
      server_hardware: 'i7-14700K, 64 GB',
      bot_machines: 2,
      machines: [{ cpu: 'Apple M5 Pro', threads: 18, ram_gb: 64, system: 'macos' }],
      network: { latency_ms: [40, 120], jitter_ms: 15, loss_pct: 1 },
    },
    server: {
      scraped: true,
      tick_ms: { p50: 3.1, p99: 6.2 },
      missed_ticks: 0,
      cpu_pct: { avg: 180 },
      refused_checks: { movement: 0, melee: 0 },
    },
    clients: {
      bots: 120,
      spawned: 120,
      kbps_in: { avg: 300, p95: 420 },
      snapshot_age_ms: { p95: 48 },
      corrections_per_min: 0,
      failed_connects: 0,
    },
    generator: { overrun_pct: 0 },
    command: 'kcds-bots --server 10.0.0.2:7790 --scenario battle --out battle.json',
    ...overrides,
  };
}

test('the report has the header, a row per scenario and how to reproduce it', () => {
  const text = renderReport([result(), result({ scenario: 'town' })]);
  assert.match(text, /^# KCDS 1\.0\.6 load test/);
  assert.match(text, /commit 0123abcd/);
  assert.match(text, /Server: i7-14700K, 64 GB/);
  assert.match(text, /Bot machines \(2\): Apple M5 Pro, 18 threads, 64 GB, macos/);
  assert.match(text, /40-120 ms delay, 15 ms jitter, 1% loss/);
  assert.match(
    text,
    /\| battle \| 120 \| 3\.10 \/ 6\.20 \| 0 \| 300 \/ 420 \| 48 \| 0\.00 \| 180 \|/,
  );
  assert.match(text, /\| town \|/);
  assert.match(text, /- None over the thresholds\./);
  assert.match(text, /kcds-bots --server 10\.0\.0\.2:7790 --scenario battle/);
});

test('numbers over their thresholds are flagged', () => {
  const bad = result({
    server: {
      scraped: true,
      tick_ms: { p50: 9, p99: 14.5 },
      missed_ticks: 12,
      cpu_pct: { avg: 100 },
      refused_checks: { movement: 2, melee: 1 },
    },
    clients: {
      bots: 300,
      spawned: 300,
      kbps_in: { avg: 500, p95: 700 },
      snapshot_age_ms: { p95: 130 },
      corrections_per_min: 0.5,
      failed_connects: 4,
    },
    generator: { overrun_pct: 3 },
  });
  const flags = limitsOf(bad, DEFAULT_THRESHOLDS).join('\n');
  for (const part of [
    'tick p99 14.50 ms',
    '12 missed server ticks',
    '700 kbps per player',
    'snapshot age p95 130 ms',
    '0.50 corrections',
    '3 refused checks',
    '4 failed connects',
    'fell behind 3.00%',
  ]) {
    assert.ok(flags.includes(part), part);
  }
  assert.match(
    limitsOf(result({ server: { scraped: false } }), DEFAULT_THRESHOLDS).join(),
    /\/metrics was not read/,
  );
});

test('a run 10 % worse than the baseline fails, noise under the floors does not', () => {
  const baseline = result();
  assert.equal(compareRuns(baseline, [result()]).ok, true);
  const slower = result({ server: { ...baseline.server, tick_ms: { p99: 7.5 } } });
  const comparison = compareRuns(baseline, [slower]);
  assert.equal(comparison.ok, false);
  assert.match(comparison.lines.join('\n'), /FAIL tick_p99_ms: 6\.20 -> 7\.50/);
  // The better of two runs counts.
  assert.equal(compareRuns(baseline, [slower, result()]).ok, true);
  // Under its floor a number never fails: 0.3 ms -> 0.4 ms is noise.
  const tiny = result({ server: { ...baseline.server, tick_ms: { p99: 0.3 } } });
  const tinyLater = result({ server: { ...baseline.server, tick_ms: { p99: 0.4 } } });
  assert.equal(compareRuns(tiny, [tinyLater]).ok, true);
  // Refused checks and failed connects fail whatever the numbers.
  const refused = result({
    server: { ...baseline.server, refused_checks: { movement: 1, melee: 0 } },
  });
  assert.equal(compareRuns(baseline, [refused]).ok, false);
  const failed = result({ clients: { ...baseline.clients, failed_connects: 1 } });
  assert.equal(compareRuns(baseline, [failed]).ok, false);
  // A looser allowance.
  assert.equal(compareRuns(baseline, [slower], { allowance: 0.25 }).ok, true);
});
