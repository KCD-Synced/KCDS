'use strict';

// The results of kcds-bots: histograms as the engine keeps them, merging several results exactly.

const test = require('node:test');
const assert = require('node:assert/strict');

const {
  bucketUpperMs,
  histogramPercentile,
  mergeHistograms,
  percentile,
  summarize,
} = require('../src/results/histogram.cjs');
const { mergeResults } = require('../src/results/merge.cjs');

// A histogram in the engine's export form, made from samples (the C++ bucket_index).
function histogramOf(samples) {
  const histogram = { count: 0, sum_ms: 0, max_ms: 0, buckets: {} };
  for (const ms of samples) {
    const us = ms * 1000;
    let index;
    if (us < 1000) {
      index = Math.floor(us / 10);
    } else if (us < 100000) {
      index = 100 + Math.floor((us - 1000) / 100);
    } else if (us < 10000000) {
      index = 1090 + Math.floor((us - 100000) / 10000);
    } else {
      index = 2080;
    }
    histogram.buckets[index] = (histogram.buckets[index] ?? 0) + 1;
    histogram.count += 1;
    histogram.sum_ms += ms;
    histogram.max_ms = Math.max(histogram.max_ms, ms);
  }
  return histogram;
}

test('the bucket bounds are the engine’s', () => {
  assert.equal(bucketUpperMs(0), 0.01);
  assert.equal(bucketUpperMs(99), 1);
  assert.equal(bucketUpperMs(100), 1.1);
  assert.equal(bucketUpperMs(1089), 100);
  assert.equal(bucketUpperMs(1090), 110);
  assert.equal(bucketUpperMs(2079), 10000);
  assert.equal(bucketUpperMs(2080), Infinity);
});

test('a merged histogram has the percentiles of all its samples', () => {
  const one = histogramOf([10, 20, 30]);
  const two = histogramOf([40, 50, 60, 70, 80, 90, 100]);
  const merged = mergeHistograms([one, two, undefined]);
  const all = histogramOf([10, 20, 30, 40, 50, 60, 70, 80, 90, 100]);
  assert.deepEqual(merged, all);
  // The upper bound of the 0.1 ms bucket that holds the sample, as the engine reports it.
  assert.equal(histogramPercentile(merged, 50), 50.1);
  assert.equal(histogramPercentile(merged, 95), 100);
  assert.equal(histogramPercentile(histogramOf([33.04]), 50), 33.04); // clamped to the maximum
  assert.equal(histogramPercentile(histogramOf([]), 50), 0);
});

test('plain percentiles are nearest rank', () => {
  const values = Array.from({ length: 100 }, (_, index) => 100 - index);
  assert.equal(percentile(values, 50), 50);
  assert.equal(percentile(values, 95), 95);
  assert.deepEqual(summarize([1, 3]), { avg: 2, p50: 1, p95: 3, max: 3 });
  assert.deepEqual(summarize([]), { avg: 0, p50: 0, p95: 0, max: 0 });
});

function engineResult({ bots, ages, server, machine, command }) {
  return {
    schema: 1,
    command,
    kcds_version: '1.0.6',
    git_sha: 'abc',
    scenario: 'town',
    seed: 1,
    started_at: '2026-11-12T10:00:05Z',
    duration_s: 60,
    interrupted: false,
    environment: { bot_machine: machine, bot_machines: 2, network: { latency_ms: [30, 90] } },
    server: server ?? { scraped: false },
    clients: {},
    generator: { ticks: 3600, overruns: 0 },
    timeseries: 'town.timeseries.csv',
    merge: {
      bots,
      histograms: {
        snapshot_age_ms: histogramOf(ages),
        connect_to_spawn_ms: histogramOf(bots.map(() => 300)),
      },
      counters: { spawned: bots.length, join_attempts: bots.length, swings: 10, corrections: 1 },
      refusal_reasons: {},
      generator: { ticks: 3600, overruns: 3, cpu_pct_of_one_core: 50, threads: 2 },
    },
  };
}

test('results of two machines merge into one', () => {
  const mac = { system: 'macos', cpu: 'Apple M5 Pro', threads: 18, ram_gb: 64 };
  const pc = { system: 'windows', cpu: 'Intel Core i7-14700K', threads: 28, ram_gb: 64 };
  const server = { scraped: true, tick_ms: { p99: 4 }, refused_checks: { movement: 0, melee: 0 } };
  const merged = mergeResults([
    engineResult({
      bots: [{ group: 'a', kbps_in: 100, kbps_out: 50, corrections_per_min: 0 }],
      ages: [30, 31],
      machine: mac,
      command: 'kcds-bots --share 1/2',
    }),
    engineResult({
      bots: [
        { group: 'a', kbps_in: 200, kbps_out: 60, corrections_per_min: 1 },
        { group: 'b', kbps_in: 300, kbps_out: 70, corrections_per_min: 0 },
      ],
      ages: [40, 50],
      machine: pc,
      server,
      command: 'kcds-bots --share 2/2',
    }),
  ]);
  assert.equal(merged.clients.bots, 3);
  assert.equal(merged.clients.spawned, 3);
  assert.equal(merged.clients.kbps_in.avg, 200);
  assert.equal(merged.clients.kbps_in.max, 300);
  assert.equal(merged.clients.snapshot_age_ms.count, 4);
  assert.equal(merged.clients.snapshot_age_ms.p50, 31.1);
  assert.equal(merged.clients.actions.swings, 20);
  assert.equal(merged.clients.corrections, 2);
  assert.deepEqual(merged.server, server); // the machine that read /metrics
  assert.equal(merged.environment.bot_machines, 2);
  assert.equal(merged.environment.machines.length, 2);
  assert.equal(merged.generator.overruns, 6);
  assert.equal(merged.generator.threads, 4);
  // Each machine's command stays, for the report's Reproduce.
  assert.deepEqual(merged.commands, ['kcds-bots --share 1/2', 'kcds-bots --share 2/2']);
  // A merged result merges again.
  const again = mergeResults([merged]);
  assert.equal(again.clients.snapshot_age_ms.count, 4);
  assert.deepEqual(again.commands, merged.commands);
  assert.throws(() => mergeResults([{ schema: 2 }]), /schema 1/);
  assert.throws(() => mergeResults([]), /nothing/);
});
