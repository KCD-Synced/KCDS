'use strict';

// kcds-bots run: this machine's part of a scenario on one or more engines (each a native,
// multi-threaded bot client), one live line for all of them, and one result.json. With
// --workers N the machine's bots are split over N engines (every N-th bot to each); with
// --share I/M the machine runs its part of a scenario that M machines run together. Only the
// first engine reads the server's /metrics.

const { spawn } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const { locateEngine } = require('../engine/locate.cjs');
const { mergeResults } = require('../results/merge.cjs');

const STATUS_PREFIX = 'KCDS_BOTS_STATUS ';

// The engine's command line for engine `worker` (0-based) of `workers`.
function engineArguments(options, worker, workers, out) {
  const share = (options.shareIndex - 1) * workers + worker + 1;
  const shares = options.shareCount * workers;
  const args = [
    '--server',
    options.server,
    '--scenario',
    options.scenario,
    '--out',
    out,
    '--share',
    `${share}/${shares}`,
    '--machines',
    String(options.machines),
    '--json-status',
  ];
  const optional = [
    ['--bots', options.bots],
    ['--threads', options.threads],
    ['--seed', options.seed],
    ['--duration', options.duration],
    ['--server-hardware', options.serverHardware],
    ['--tick-hz', options.tickHz],
  ];
  for (const [name, value] of optional) {
    if (value !== undefined && value !== null && value !== '') {
      args.push(name, String(value));
    }
  }
  if (options.metricsUrl && worker === 0) {
    args.push('--metrics-url', options.metricsUrl);
  }
  return args;
}

// The live line of all engines' last status; `previous` is the line before (for rates).
function liveLine(statuses, previous, seconds) {
  const total = {
    elapsed_s: 0,
    bots: 0,
    playing: 0,
    failed: 0,
    bytes_in: 0,
    bytes_out: 0,
    corrections: 0,
    refused_joins: 0,
    ticks: 0,
    overruns: 0,
    cpu_pct: 0,
  };
  for (const status of statuses) {
    if (!status) {
      continue;
    }
    for (const name of Object.keys(total)) {
      if (name === 'elapsed_s') {
        total.elapsed_s = Math.max(total.elapsed_s, status.elapsed_s ?? 0);
      } else {
        total[name] += status[name] ?? 0;
      }
    }
  }
  const rate = (name, factor) =>
    previous && seconds > 0 ? ((total[name] - previous[name]) * factor) / seconds : 0;
  const text =
    `[${total.elapsed_s.toFixed(0).padStart(6)}s] bots ${total.playing}/${total.bots} playing, ` +
    `${total.failed} failed | in ${rate('bytes_in', 0.008).toFixed(0)} kbps, out ` +
    `${rate('bytes_out', 0.008).toFixed(0)} kbps | corrections ` +
    `${rate('corrections', 60).toFixed(1)}/min | refused joins ${total.refused_joins} | ` +
    `bot cpu ${total.cpu_pct.toFixed(0)}% of a core, overruns ${total.overruns}`;
  return { total, text };
}

// Runs the engines; resolves with the merged result (also written to options.out).
async function runScenario(options, { log = console.log, logError = console.error } = {}) {
  const engine = locateEngine({ engine: options.engine });
  const workers = Math.max(1, options.workers ?? 1);
  const out = path.resolve(options.out);
  fs.mkdirSync(path.dirname(out), { recursive: true });
  const engineOuts = [];
  for (let worker = 0; worker < workers; worker += 1) {
    engineOuts.push(workers === 1 ? out : out.replace(/(\.json)?$/, `.engine${worker + 1}.json`));
  }
  log(
    `kcds-bots: ${options.scenario} on ${workers} engine${workers === 1 ? '' : 's'} ` +
      `(${engine}), share ${options.shareIndex}/${options.shareCount}`,
  );

  const statuses = new Array(workers).fill(null);
  const children = [];
  const exits = [];
  for (let worker = 0; worker < workers; worker += 1) {
    const child = spawn(engine, engineArguments(options, worker, workers, engineOuts[worker]), {
      stdio: ['ignore', 'pipe', 'pipe'],
      windowsHide: true,
    });
    children.push(child);
    let pending = '';
    child.stdout.setEncoding('utf8');
    child.stdout.on('data', (chunk) => {
      pending += chunk;
      let newline = pending.indexOf('\n');
      while (newline >= 0) {
        const line = pending.slice(0, newline).trimEnd();
        pending = pending.slice(newline + 1);
        if (line.startsWith(STATUS_PREFIX)) {
          try {
            statuses[worker] = JSON.parse(line.slice(STATUS_PREFIX.length));
          } catch {
            // A torn line: the next one comes within a second.
          }
        } else if (line && options.verbose) {
          log(`engine ${worker + 1}: ${line}`);
        }
        newline = pending.indexOf('\n');
      }
    });
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', (chunk) => logError(`engine ${worker + 1}: ${chunk.trimEnd()}`));
    exits.push(
      new Promise((resolve) => {
        child.on('exit', (code, signal) => resolve({ code, signal }));
        child.on('error', (error) => resolve({ code: -1, error }));
      }),
    );
  }

  // Ctrl+C: the engines stop and write what they have. On Windows the console's Ctrl+C reaches
  // them by itself; elsewhere it is passed on.
  let stopping = false;
  const onInterrupt = () => {
    if (stopping) {
      return;
    }
    stopping = true;
    log('kcds-bots: stopping; the engines write their partial results');
    if (process.platform !== 'win32') {
      for (const child of children) {
        child.kill('SIGINT');
      }
    }
  };
  process.on('SIGINT', onInterrupt);
  process.on('SIGTERM', onInterrupt);

  let previous = null;
  let previousAt = Date.now();
  const timer = setInterval(() => {
    const now = Date.now();
    const line = liveLine(statuses, previous, (now - previousAt) / 1000);
    log(line.text);
    previous = line.total;
    previousAt = now;
  }, options.statusEverySeconds * 1000);

  const ended = await Promise.all(exits);
  clearInterval(timer);
  process.off('SIGINT', onInterrupt);
  process.off('SIGTERM', onInterrupt);

  const results = [];
  for (let worker = 0; worker < workers; worker += 1) {
    const exit = ended[worker];
    if (exit.code !== 0) {
      logError(
        `engine ${worker + 1} ended with ${exit.error ? exit.error.message : `code ${exit.code}`}`,
      );
    }
    if (fs.existsSync(engineOuts[worker])) {
      results.push(JSON.parse(fs.readFileSync(engineOuts[worker], 'utf8')));
    }
  }
  if (results.length === 0) {
    throw new Error('no engine wrote a result');
  }
  const result = workers === 1 ? results[0] : mergeResults(results);
  result.command = options.command;
  fs.writeFileSync(out, `${JSON.stringify(result, null, 1)}\n`);
  const clients = result.clients;
  log(
    `kcds-bots: ${clients.spawned} spawned, ${clients.failed_connects} failed connects, ` +
      `${clients.corrections} corrections, refused checks ` +
      `${
        (result.server?.refused_checks?.movement ?? 0) + (result.server?.refused_checks?.melee ?? 0)
      }; ` +
      `${out}${result.interrupted ? ' (interrupted: partial)' : ''}`,
  );
  return result;
}

module.exports = { runScenario, engineArguments, liveLine };
