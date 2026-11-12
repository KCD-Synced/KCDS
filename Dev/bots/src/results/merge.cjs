'use strict';

// One result from several: the engines of one machine (kcds-bots --workers) or the machines of a
// run (kcds-bots merge). The bots' numbers and the histograms come from each result's `merge`
// section, so the merged percentiles are exact; the server's numbers come from the result that
// read /metrics (one machine reads it). The merged result has a `merge` section too, so it can be
// merged again.

const { describeHistogram, mergeHistograms, summarize } = require('./histogram.cjs');

const BANDS = ['near', 'mid', 'far'];

function sumCounters(parts) {
  const total = {};
  for (const part of parts) {
    for (const [name, value] of Object.entries(part ?? {})) {
      total[name] = (total[name] ?? 0) + value;
    }
  }
  return total;
}

// The machines of the results, once each (an engine reports its machine).
function machinesOf(results) {
  const seen = new Map();
  for (const result of results) {
    const machine = result.environment?.bot_machine ?? {};
    const key = `${machine.system}|${machine.cpu}|${machine.threads}|${machine.ram_gb}`;
    if (!seen.has(key)) {
      seen.set(key, machine);
    }
  }
  return [...seen.values()];
}

function mergeResults(results) {
  if (results.length === 0) {
    throw new Error('nothing to merge');
  }
  if (results.some((result) => result.schema !== 1 || !result.merge)) {
    throw new Error('only kcds-bots results of schema 1 with a merge section can be merged');
  }
  const first = results[0];
  const merges = results.map((result) => result.merge);
  const bots = merges.flatMap((merge) => merge.bots ?? []);
  const histogramNames = [
    'snapshot_age_ms',
    'connect_to_spawn_ms',
    ...BANDS.map((band) => `update_interval_ms_${band}`),
  ];
  const histograms = {};
  for (const name of histogramNames) {
    histograms[name] = mergeHistograms(merges.map((merge) => merge.histograms?.[name]));
  }
  const counters = sumCounters(merges.map((merge) => merge.counters));
  const refusalReasons = sumCounters(merges.map((merge) => merge.refusal_reasons));
  const generators = merges.map((merge) => merge.generator ?? {});
  const ticks = generators.reduce((sum, one) => sum + (one.ticks ?? 0), 0);
  const overruns = generators.reduce((sum, one) => sum + (one.overruns ?? 0), 0);
  const cpu = generators.reduce((sum, one) => sum + (one.cpu_pct_of_one_core ?? 0), 0);
  const threads = generators.reduce((sum, one) => sum + (one.threads ?? 0), 0);
  const machines = machinesOf(results);
  const scraped = results.find((result) => result.server?.scraped) ?? first;

  const connect = describeHistogram(histograms.connect_to_spawn_ms);
  const updateIntervals = {};
  for (const band of BANDS) {
    updateIntervals[band] = describeHistogram(histograms[`update_interval_ms_${band}`]);
  }
  const corrections = bots.map((bot) => bot.corrections_per_min);
  return {
    schema: 1,
    kcds_version: first.kcds_version,
    git_sha: first.git_sha,
    scenario: first.scenario,
    seed: first.seed,
    started_at: results.map((result) => result.started_at).sort()[0],
    duration_s: Math.max(...results.map((result) => result.duration_s ?? 0)),
    interrupted: results.some((result) => result.interrupted),
    environment: {
      ...first.environment,
      bot_machine: machines[0],
      bot_machines: Math.max(
        machines.length,
        ...results.map((result) => result.environment?.bot_machines ?? 1),
      ),
      machines,
      share: 'merged',
      engines: results.length,
    },
    server: scraped.server,
    clients: {
      bots: bots.length,
      joins: counters.join_attempts ?? 0,
      spawned: counters.spawned ?? 0,
      kbps_in: summarize(bots.map((bot) => bot.kbps_in)),
      kbps_out: summarize(bots.map((bot) => bot.kbps_out)),
      snapshot_age_ms: describeHistogram(histograms.snapshot_age_ms),
      update_interval_ms: updateIntervals,
      corrections_per_min: summarize(corrections).avg,
      corrections: counters.corrections ?? 0,
      connect_to_spawn_ms: { p50: connect.p50, p95: connect.p95, max: connect.max },
      failed_connects: (counters.refused_joins ?? 0) + (counters.timed_out_joins ?? 0),
      refused_connects: counters.refused_joins ?? 0,
      timed_out_connects: counters.timed_out_joins ?? 0,
      refusal_reasons: refusalReasons,
      actions: Object.fromEntries(
        Object.entries(counters).filter(
          ([name]) =>
            ![
              'join_attempts',
              'spawned',
              'refused_joins',
              'timed_out_joins',
              'corrections',
            ].includes(name),
        ),
      ),
    },
    generator: {
      engines: results.length,
      threads,
      ticks,
      overruns,
      overrun_pct: ticks ? (100 * overruns) / ticks : 0,
      cpu_pct_of_one_core: cpu,
      per_engine: results.map((result) => result.generator),
    },
    timeseries: results.map((result) => result.timeseries).filter(Boolean),
    // How each part was run (one command a machine), for the report's Reproduce.
    commands: results.flatMap((result) => result.commands ?? [result.command]).filter(Boolean),
    merge: {
      bots,
      histograms,
      counters,
      refusal_reasons: refusalReasons,
      generator: { ticks, overruns, cpu_pct_of_one_core: cpu, threads },
    },
  };
}

module.exports = { mergeResults };
