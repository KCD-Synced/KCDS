'use strict';

// kcds-bots' command line, the engines' share of a scenario, the live line and finding the engine.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const args = require('../src/cli/args.cjs');
const { engineArguments, liveLine } = require('../src/run/run.cjs');
const { locateEngine, platformPackage } = require('../src/engine/locate.cjs');
const { main } = require('../src/cli/commands.cjs');

test('the run options', () => {
  const options = args.parseRun([
    '--server',
    '192.168.1.20:7790',
    '--scenario',
    'battle',
    '--out',
    'battle.json',
    '--workers',
    '2',
    '--share',
    '2/2',
    '--bots',
    '120',
    '--metrics-url',
    'http://192.168.1.20:7791/metrics',
  ]);
  assert.equal(options.server, '192.168.1.20:7790');
  assert.ok(options.scenario.endsWith(path.join('scenarios', 'battle.yaml')));
  assert.equal(options.workers, 2);
  assert.equal(options.shareIndex, 2);
  assert.equal(options.shareCount, 2);
  assert.equal(options.machines, 2);
  assert.equal(options.bots, 120);
  assert.match(options.command, /^kcds-bots --server 192\.168\.1\.20:7790/);
  const fail = (argv, message) => assert.throws(() => args.parseRun(argv), message);
  fail(['--scenario', 'town', '--out', 'x'], /--server is required/);
  fail(['--server', 'host', '--scenario', 'town', '--out', 'x'], /host:port/);
  fail(['--server', 'h:1', '--scenario', 'nowhere', '--out', 'x'], /no scenario nowhere/);
  fail(['--server', 'h:1', '--scenario', 'town', '--out', 'x', '--share', '3/2'], /--share/);
  fail(['--server', 'h:1', '--scenario', 'town', '--out', 'x', '--colour', 'red'], /unknown/);
  fail(['--server', 'h:1', '--scenario', 'town', '--out', 'x', '--bots', 'many'], /whole number/);
});

test('every engine of every machine runs its own bots, and one reads /metrics', () => {
  const options = args.parseRun([
    '--server',
    'h:7790',
    '--scenario',
    'town',
    '--out',
    'x.json',
    '--share',
    '2/2',
    '--metrics-url',
    'http://h:7791/metrics',
  ]);
  const shares = [0, 1, 2].map((worker) => {
    const argv = engineArguments(options, worker, 3, `e${worker}.json`);
    return {
      share: argv[argv.indexOf('--share') + 1],
      metrics: argv.includes('--metrics-url'),
      status: argv.includes('--json-status'),
    };
  });
  // Machine 2 of 2 with 3 engines: shares 4, 5 and 6 of 6.
  assert.deepEqual(
    shares.map((one) => one.share),
    ['4/6', '5/6', '6/6'],
  );
  assert.deepEqual(
    shares.map((one) => one.metrics),
    [true, false, false],
  );
  assert.ok(shares.every((one) => one.status));
});

test('the live line adds up the engines', () => {
  const statuses = [
    { elapsed_s: 10, bots: 50, playing: 50, bytes_in: 1000, bytes_out: 500, corrections: 0 },
    { elapsed_s: 11, bots: 50, playing: 49, failed: 1, bytes_in: 3000, bytes_out: 500 },
  ];
  const first = liveLine(statuses, null, 5);
  assert.match(first.text, /bots 99\/100 playing, 1 failed/);
  const later = liveLine(
    [
      { ...statuses[0], bytes_in: 6000 },
      { ...statuses[1], bytes_in: 8000 },
    ],
    first.total,
    5,
  );
  assert.match(later.text, /in 16 kbps/); // 10000 bytes in 5 s
});

test('the engine is found where it is', () => {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-bots-'));
  const engine = path.join(folder, 'kcds-bots-engine');
  fs.writeFileSync(engine, '');
  assert.equal(locateEngine({ engine }), engine);
  assert.equal(locateEngine({ env: { KCDS_BOTS_ENGINE: engine } }), engine);
  assert.throws(() => locateEngine({ engine: path.join(folder, 'missing') }), /not found/);
  fs.rmSync(folder, { recursive: true, force: true });
  assert.equal(platformPackage('darwin', 'arm64'), '@kcds/bots-darwin-arm64');
  assert.equal(platformPackage('win32', 'x64'), '@kcds/bots-win32-x64');
});

test('the scenarios that come with it', async () => {
  assert.deepEqual(args.listScenarios(), [
    'battle',
    'cavalry',
    'join_storm',
    'smoke',
    'soak',
    'town',
  ]);
  const lines = [];
  assert.equal(await main(['scenarios'], { log: (line) => lines.push(line) }), 0);
  assert.match(lines.join(), /battle/);
  assert.equal(await main(['--help'], { log: (line) => lines.push(line) }), 0);
  const errors = [];
  assert.equal(await main(['compare'], { logError: (line) => errors.push(line) }), 2);
  assert.match(errors.join(), /--baseline/);
});
