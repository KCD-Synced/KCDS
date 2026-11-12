'use strict';

// kcds-bots' command line: the options of each command, read into a plain object, every problem
// an Error that says what is wrong.

const fs = require('node:fs');
const path = require('node:path');

const SCENARIOS = path.join(__dirname, '..', '..', 'scenarios');

const USAGE = `kcds-bots: load-test a KCDS server with bots (a development server with KCDS_ALLOW_BOTS=1)

  kcds-bots --server <host:port> --scenario <name or file.yaml> --out <result.json>
            [--bots N] [--threads N] [--workers N] [--seed N] [--duration 10m]
            [--metrics-url http://<host>:<port>/metrics] [--share I/N] [--machines N]
            [--server-hardware "text"] [--engine <path>] [--verbose]
      Runs a scenario. --threads: threads of each engine (about 50 bots a thread by default);
      --workers: engines on this machine (1); --share I/N: this machine's part when N machines
      run the scenario together; --metrics-url: the server's /metrics (KCDS_HTTP_PORT), read by
      one machine only.

  kcds-bots merge <result.json>... --out <merged.json>
      One result from the results of several machines.

  kcds-bots report <result.json>... [--out report.md] [--thresholds file.json] [--title "text"]
      The Markdown report.

  kcds-bots compare --baseline <result.json> <result.json>... [--allowance 10%]
      Fails (exit 1) when a run got worse than the baseline.

  kcds-bots scenarios
      The scenarios that come with kcds-bots.
`;

// A scenario name of the package (town) or a file.
function resolveScenario(value) {
  const bare = !value.includes('/') && !value.includes('\\') && !value.endsWith('.yaml');
  if (bare) {
    const file = path.join(SCENARIOS, `${value}.yaml`);
    if (!fs.existsSync(file)) {
      throw new Error(`no scenario ${value} (kcds-bots scenarios lists them)`);
    }
    return file;
  }
  if (!fs.existsSync(value)) {
    throw new Error(`no scenario file ${value}`);
  }
  return path.resolve(value);
}

function listScenarios() {
  return fs
    .readdirSync(SCENARIOS)
    .filter((name) => name.endsWith('.yaml') && name !== 'areas.yaml')
    .map((name) => name.slice(0, -'.yaml'.length))
    .sort();
}

function wholeNumber(name, text, low, high) {
  const value = Number(text);
  if (!/^\d+$/.test(String(text)) || value < low || value > high) {
    throw new Error(`${name} must be a whole number from ${low} to ${high}`);
  }
  return value;
}

// Splits arguments into options (--name value, --flag) and the rest.
function split(argv, flags) {
  const options = new Map();
  const rest = [];
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (!argument.startsWith('--')) {
      rest.push(argument);
      continue;
    }
    if (flags.includes(argument)) {
      options.set(argument, true);
      continue;
    }
    if (index + 1 >= argv.length) {
      throw new Error(`${argument} needs a value`);
    }
    options.set(argument, argv[index + 1]);
    index += 1;
  }
  return { options, rest };
}

function refuseUnknown(options, known) {
  for (const name of options.keys()) {
    if (!known.includes(name)) {
      throw new Error(`unknown option ${name}`);
    }
  }
}

function parseRun(argv) {
  const known = [
    '--server',
    '--scenario',
    '--out',
    '--bots',
    '--threads',
    '--workers',
    '--seed',
    '--duration',
    '--metrics-url',
    '--share',
    '--machines',
    '--server-hardware',
    '--engine',
    '--tick-hz',
    '--verbose',
  ];
  const { options, rest } = split(argv, ['--verbose']);
  refuseUnknown(options, known);
  if (rest.length > 0) {
    throw new Error(`unexpected ${rest[0]}`);
  }
  for (const required of ['--server', '--scenario', '--out']) {
    if (!options.has(required)) {
      throw new Error(`${required} is required`);
    }
  }
  if (!/^[^:]+:\d+$/.test(options.get('--server'))) {
    throw new Error('--server must be host:port');
  }
  let shareIndex = 1;
  let shareCount = 1;
  if (options.has('--share')) {
    const match = /^(\d+)\/(\d+)$/.exec(options.get('--share'));
    if (!match || Number(match[1]) < 1 || Number(match[1]) > Number(match[2])) {
      throw new Error('--share must be I/N with I from 1 to N, such as 1/2');
    }
    shareIndex = Number(match[1]);
    shareCount = Number(match[2]);
  }
  const optional = (name, low, high) =>
    options.has(name) ? wholeNumber(name, options.get(name), low, high) : undefined;
  return {
    server: options.get('--server'),
    scenario: resolveScenario(options.get('--scenario')),
    out: options.get('--out'),
    bots: optional('--bots', 1, 20000),
    threads: optional('--threads', 1, 256),
    workers: optional('--workers', 1, 64) ?? 1,
    seed: optional('--seed', 0, 0xffffffff),
    duration: options.get('--duration'),
    metricsUrl: options.get('--metrics-url'),
    shareIndex,
    shareCount,
    machines: Math.max(shareCount, optional('--machines', 1, 64) ?? 1),
    serverHardware: options.get('--server-hardware'),
    engine: options.get('--engine'),
    tickHz: optional('--tick-hz', 10, 240),
    verbose: options.get('--verbose') === true,
    statusEverySeconds: 5,
    // As it can be pasted again: a value with spaces in quotes.
    command: `kcds-bots ${argv
      .map((argument) => (/[\s"]/.test(argument) ? JSON.stringify(argument) : argument))
      .join(' ')}`,
  };
}

function parseReport(argv) {
  const { options, rest } = split(argv, []);
  refuseUnknown(options, ['--out', '--thresholds', '--title']);
  if (rest.length === 0) {
    throw new Error('report needs one or more result.json files');
  }
  return {
    results: rest,
    out: options.get('--out'),
    thresholds: options.get('--thresholds'),
    title: options.get('--title'),
  };
}

function parseMerge(argv) {
  const { options, rest } = split(argv, []);
  refuseUnknown(options, ['--out']);
  if (rest.length < 1 || !options.has('--out')) {
    throw new Error('merge needs result.json files and --out <merged.json>');
  }
  return { results: rest, out: options.get('--out') };
}

function parseCompare(argv) {
  const { options, rest } = split(argv, []);
  refuseUnknown(options, ['--baseline', '--allowance']);
  if (!options.has('--baseline') || rest.length === 0) {
    throw new Error('compare needs --baseline <result.json> and one or more result.json files');
  }
  let allowance = 0.1;
  if (options.has('--allowance')) {
    const match = /^(\d+(?:\.\d+)?)%$/.exec(options.get('--allowance'));
    if (!match) {
      throw new Error('--allowance must be a percentage such as 10%');
    }
    allowance = Number(match[1]) / 100;
  }
  return { baseline: options.get('--baseline'), results: rest, allowance };
}

module.exports = {
  USAGE,
  parseRun,
  parseReport,
  parseMerge,
  parseCompare,
  resolveScenario,
  listScenarios,
};
