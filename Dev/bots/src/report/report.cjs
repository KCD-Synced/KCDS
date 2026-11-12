'use strict';

// The load-test report: Markdown from one or more result.json files, for the release notes and
// the website. A header (version, commit, date, server and bot machines, network), a row per
// scenario, "Known limits" with every number over its threshold flagged (a person then words
// them), and "Reproduce" with the command lines.

// What a result may reach before the report flags it. Change them with --thresholds <file.json>.
const DEFAULT_THRESHOLDS = Object.freeze({
  tick_p99_ms: 10, // of the 16.7 ms a 60 Hz tick has
  missed_ticks: 0,
  kbps_down_p95: 512,
  snapshot_age_p95_ms: 100,
  corrections_per_min: 0.1,
  refused_checks: 0,
  failed_connects: 0,
  bot_overrun_pct: 1,
  server_cpu_pct: 400,
});

function number(value, digits = 1) {
  return Number.isFinite(value) ? value.toFixed(digits) : '-';
}

function refusedChecks(result) {
  const checks = result.server?.refused_checks ?? {};
  return (checks.movement ?? 0) + (checks.melee ?? 0);
}

// The numbers of a result the report shows and checks.
function rowOf(result) {
  return {
    scenario: result.scenario,
    // Bots, not joins: a join_leave bot joins again and again.
    players: result.clients?.bots ?? result.clients?.spawned ?? 0,
    tickP50: result.server?.tick_ms?.p50,
    tickP99: result.server?.tick_ms?.p99,
    missedTicks: result.server?.missed_ticks ?? 0,
    kbpsDownP95: result.clients?.kbps_in?.p95,
    kbpsDownAvg: result.clients?.kbps_in?.avg,
    snapshotAgeP95: result.clients?.snapshot_age_ms?.p95,
    correctionsPerMin: result.clients?.corrections_per_min ?? 0,
    serverCpu: result.server?.cpu_pct?.avg,
    refusedChecks: refusedChecks(result),
    failedConnects: result.clients?.failed_connects ?? 0,
    botOverrunPct: result.generator?.overrun_pct ?? 0,
    scraped: result.server?.scraped === true,
  };
}

// The flags of one result: [text, ...].
function limitsOf(result, thresholds) {
  const row = rowOf(result);
  const flags = [];
  const over = (value, limit, text) => {
    if (Number.isFinite(value) && value > limit) {
      flags.push(text);
    }
  };
  over(row.tickP99, thresholds.tick_p99_ms, `server tick p99 ${number(row.tickP99, 2)} ms`);
  over(row.missedTicks, thresholds.missed_ticks, `${row.missedTicks} missed server ticks`);
  over(row.kbpsDownP95, thresholds.kbps_down_p95, `${number(row.kbpsDownP95, 0)} kbps per player`);
  over(
    row.snapshotAgeP95,
    thresholds.snapshot_age_p95_ms,
    `snapshot age p95 ${number(row.snapshotAgeP95, 0)} ms`,
  );
  over(
    row.correctionsPerMin,
    thresholds.corrections_per_min,
    `${number(row.correctionsPerMin, 2)} corrections a minute per bot`,
  );
  over(row.refusedChecks, thresholds.refused_checks, `${row.refusedChecks} refused checks`);
  over(row.failedConnects, thresholds.failed_connects, `${row.failedConnects} failed connects`);
  over(
    row.serverCpu,
    thresholds.server_cpu_pct,
    `server CPU ${number(row.serverCpu, 0)}% of a core`,
  );
  over(
    row.botOverrunPct,
    thresholds.bot_overrun_pct,
    `the bot machines fell behind ${number(row.botOverrunPct, 2)}% of their ticks ` +
      '(the numbers then understate the server)',
  );
  if (!row.scraped) {
    flags.push('no server numbers: /metrics was not read (--metrics-url)');
  }
  return flags;
}

function machineText(machine) {
  if (!machine) {
    return 'unknown';
  }
  const parts = [machine.cpu || 'unknown CPU'];
  if (machine.threads) {
    parts.push(`${machine.threads} threads`);
  }
  if (machine.ram_gb) {
    parts.push(`${Math.round(machine.ram_gb)} GB`);
  }
  if (machine.system) {
    parts.push(machine.system);
  }
  return parts.join(', ');
}

function renderReport(results, { thresholds = DEFAULT_THRESHOLDS, title } = {}) {
  if (results.length === 0) {
    throw new Error('no results for the report');
  }
  const first = results[0];
  const environment = first.environment ?? {};
  const network = environment.network ?? {};
  const machines = environment.machines ?? [environment.bot_machine];
  const lines = [];
  lines.push(`# ${title ?? `KCDS ${first.kcds_version} load test`}`);
  lines.push('');
  lines.push(`- KCDS ${first.kcds_version}, commit ${first.git_sha || 'unknown'}`);
  lines.push(`- Date: ${first.started_at}`);
  lines.push(`- Server: ${environment.server_hardware || 'not given (--server-hardware)'}`);
  lines.push(
    `- Bot machines (${environment.bot_machines ?? machines.length}): ` +
      machines.map(machineText).join('; '),
  );
  const latency = network.latency_ms ?? [0, 0];
  lines.push(
    `- Network per bot: ${latency[0]}-${latency[1]} ms delay, ${network.jitter_ms ?? 0} ms ` +
      `jitter, ${network.loss_pct ?? 0}% loss`,
  );
  lines.push('');
  lines.push(
    '| Scenario | Players | Tick p50 / p99 (ms) | Missed ticks | kbps per player (down, avg / p95) | ' +
      'Snapshot age p95 (ms) | Corrections / min | Server CPU (% of a core) |',
  );
  lines.push('|---|---:|---:|---:|---:|---:|---:|---:|');
  for (const result of results) {
    const row = rowOf(result);
    lines.push(
      `| ${row.scenario} | ${row.players} | ${number(row.tickP50, 2)} / ${number(
        row.tickP99,
        2,
      )} | ` +
        `${row.missedTicks} | ${number(row.kbpsDownAvg, 0)} / ${number(row.kbpsDownP95, 0)} | ` +
        `${number(row.snapshotAgeP95, 0)} | ${number(row.correctionsPerMin, 2)} | ` +
        `${number(row.serverCpu, 0)} |`,
    );
  }
  lines.push('');
  lines.push('## Known limits');
  lines.push('');
  let flagged = false;
  for (const result of results) {
    for (const flag of limitsOf(result, thresholds)) {
      flagged = true;
      lines.push(`- **${result.scenario}**: ${flag}.`);
    }
  }
  if (!flagged) {
    lines.push('- None over the thresholds.');
  }
  lines.push('');
  lines.push('## Reproduce');
  lines.push('');
  lines.push('```sh');
  for (const result of results) {
    lines.push(result.command || `kcds-bots --scenario ${result.scenario} --seed ${result.seed}`);
  }
  lines.push('```');
  lines.push('');
  return lines.join('\n');
}

module.exports = { renderReport, limitsOf, rowOf, DEFAULT_THRESHOLDS };
