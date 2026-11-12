'use strict';

// kcds-bots compare: a run against the last main's baseline, for CI. It fails when one of the
// watched numbers got worse by more than its allowance (10 % by default), and on any refused check
// or failed connect. CI runners are noisy, so a scenario runs twice and the better run of each
// number counts (several current results may be given). A number that stays under its floor is
// never a regression: a tick of 0.3 ms becoming 0.4 ms is noise.

const WATCHED = Object.freeze([
  { name: 'tick_p99_ms', floor: 2, read: (result) => result.server?.tick_ms?.p99 },
  { name: 'kbps_per_player_p95', floor: 16, read: (result) => result.clients?.kbps_in?.p95 },
  {
    name: 'snapshot_age_p95_ms',
    floor: 20,
    read: (result) => result.clients?.snapshot_age_ms?.p95,
  },
  {
    name: 'corrections_per_min',
    floor: 0.05,
    read: (result) => result.clients?.corrections_per_min,
  },
]);

const DEFAULT_ALLOWANCE = 0.1;

function refused(result) {
  const checks = result.server?.refused_checks ?? {};
  return (checks.movement ?? 0) + (checks.melee ?? 0);
}

// { ok, lines } for `current` (one or more results of a scenario) against `baseline`.
function compareRuns(baseline, current, { allowance = DEFAULT_ALLOWANCE } = {}) {
  const runs = Array.isArray(current) ? current : [current];
  const lines = [];
  let ok = true;
  for (const watched of WATCHED) {
    const before = watched.read(baseline);
    const values = runs.map(watched.read).filter(Number.isFinite);
    if (!Number.isFinite(before) || values.length === 0) {
      lines.push(`- ${watched.name}: no number to compare`);
      continue;
    }
    const best = Math.min(...values);
    const limit = Math.max(before * (1 + allowance), watched.floor);
    const change = before > 0 ? ((best - before) / before) * 100 : 0;
    const worse = best > limit;
    ok = ok && !worse;
    lines.push(
      `- ${worse ? 'FAIL' : 'ok'} ${watched.name}: ${before.toFixed(2)} -> ${best.toFixed(2)} ` +
        `(${change >= 0 ? '+' : ''}${change.toFixed(1)} %, allowed up to ${limit.toFixed(2)})`,
    );
  }
  for (const run of runs) {
    const checks = refused(run);
    const failed = run.clients?.failed_connects ?? 0;
    if (checks > 0) {
      ok = false;
      lines.push(`- FAIL ${checks} refused checks in a run (bots are legitimate players)`);
    }
    if (failed > 0) {
      ok = false;
      lines.push(`- FAIL ${failed} failed connects in a run`);
    }
  }
  return { ok, lines };
}

module.exports = { compareRuns, WATCHED, DEFAULT_ALLOWANCE };
