'use strict';

// kcds-bots against a real server: 10 bots join a development server that allows bots, spawn,
// get snapshots and trip no check; the same bots are turned away by a server without
// KCDS_ALLOW_BOTS. Needs a server and an engine built from this repository (build/, or
// KCDS_BOTS_SERVER and KCDS_BOTS_ENGINE); skipped without them.

const test = require('node:test');
const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');

const { locateEngine } = require('../src/engine/locate.cjs');
const { runScenario } = require('../src/run/run.cjs');

const REPOSITORY = path.resolve(__dirname, '..', '..', '..');
const SERVER_NAME = process.platform === 'win32' ? 'kcd2s-server.exe' : 'kcd2s-server';

function findServer() {
  if (process.env.KCDS_BOTS_SERVER) {
    return process.env.KCDS_BOTS_SERVER;
  }
  for (const folder of ['build', 'build-release', 'build-linux']) {
    const candidate = path.join(REPOSITORY, folder, 'Backend', 'integration', SERVER_NAME);
    if (fs.existsSync(candidate)) {
      return candidate;
    }
  }
  return null;
}

function findEngine() {
  try {
    return locateEngine();
  } catch {
    return null;
  }
}

const server = findServer();
const engine = findEngine();
const skip = !server || !engine ? 'no server or engine build of this repository' : false;

// A free port on 127.0.0.1 (TCP; the server takes UDP and TCP on it).
function freePort() {
  return new Promise((resolve, reject) => {
    const probe = net.createServer();
    probe.listen(0, '127.0.0.1', () => {
      const { port } = probe.address();
      probe.close(() => resolve(port));
    });
    probe.on('error', reject);
  });
}

// Starts a server; resolves once its resources run.
async function startServer(t, { allowBots }) {
  const port = await freePort();
  const httpPort = await freePort();
  const work = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-bots-live-'));
  const resources = path.join(work, 'resources');
  fs.cpSync(
    path.join(__dirname, '..', 'resource', 'kcds-bots'),
    path.join(resources, 'kcds-bots'),
    {
      recursive: true,
    },
  );
  const env = {
    ...process.env,
    KCDS_SERVER_METRICS: '1',
    KCDS_HTTP_PORT: String(httpPort),
    KCDS_MASTER_URL: 'off',
    KCDS_SERVER_MAX_PEERS: '64',
  };
  delete env.KCDS_ALLOW_BOTS;
  if (allowBots) {
    env.KCDS_ALLOW_BOTS = '1';
  }
  const child = spawn(
    server,
    [
      '--port',
      String(port),
      '--bind',
      '127.0.0.1',
      '--node',
      process.execPath,
      '--resource-host',
      path.join(REPOSITORY, 'Backend', 'scripting', 'runtimes', 'js', 'main.cjs'),
      '--resources',
      resources,
    ],
    { cwd: work, env, stdio: ['pipe', 'pipe', 'pipe'] },
  );
  let output = '';
  child.stdout.on('data', (chunk) => {
    output += chunk;
  });
  child.stderr.on('data', (chunk) => {
    output += chunk;
  });
  t.after(() => {
    child.kill();
    fs.rmSync(work, { recursive: true, force: true });
  });
  const started = Date.now();
  while (!output.includes('RESOURCE HOST ready')) {
    if (Date.now() - started > 30000 || child.exitCode !== null) {
      throw new Error(`the server did not start:\n${output}`);
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  return { port, httpPort, log: () => output };
}

function scenarioFile(work) {
  const file = path.join(work, 'live.yaml');
  fs.writeFileSync(
    file,
    [
      'name: live',
      'duration: 8s',
      'seed: 1',
      'areas: { square: { offset: [10, 0], radius: 8 } }',
      'groups:',
      '  - { name: walkers, count: 6, behavior: wander, spawn: { area: square } }',
      '  - { name: red, count: 2, behavior: melee_fighter, params: { target_group: blue } }',
      '  - { name: blue, count: 2, behavior: melee_fighter, params: { target_group: red } }',
      '',
    ].join('\n'),
  );
  return file;
}

async function runBots(t, target) {
  const work = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-bots-run-'));
  t.after(() => fs.rmSync(work, { recursive: true, force: true }));
  return runScenario(
    {
      server: `127.0.0.1:${target.port}`,
      scenario: scenarioFile(work),
      out: path.join(work, 'result.json'),
      workers: 1,
      shareIndex: 1,
      shareCount: 1,
      machines: 1,
      engine,
      metricsUrl: `http://127.0.0.1:${target.httpPort}/metrics`,
      statusEverySeconds: 60,
      command: 'live test',
    },
    { log: () => {}, logError: () => {} },
  );
}

test(
  '10 bots join a server that allows them and trip no check',
  { skip, timeout: 90000 },
  async (t) => {
    const target = await startServer(t, { allowBots: true });
    const result = await runBots(t, target);
    assert.equal(result.clients.bots, 10);
    assert.equal(result.clients.spawned, 10, target.log());
    assert.equal(result.clients.failed_connects, 0);
    assert.ok(result.merge.histograms.update_interval_ms_near.count > 0, 'snapshots arrived');
    assert.ok(result.clients.snapshot_age_ms.count > 0);
    assert.equal(result.clients.corrections, 0);
    assert.ok(result.server.scraped, '/metrics was read');
    assert.equal(result.server.refused_checks.movement, 0);
    assert.equal(result.server.refused_checks.melee, 0);
    assert.match(target.log(), /BOTS allowed/);
    assert.match(target.log(), /a bot \(KCDS_ALLOW_BOTS\)/);
  },
);

test(
  'a server without KCDS_ALLOW_BOTS turns every bot away',
  { skip, timeout: 90000 },
  async (t) => {
    const target = await startServer(t, { allowBots: false });
    const result = await runBots(t, target);
    assert.equal(result.clients.spawned, 0);
    assert.equal(result.clients.refused_connects, 10);
    assert.equal(result.clients.refusal_reasons['This server does not accept bots.'], 10);
    assert.match(target.log(), /JOIN refused: a bot \(kcds-bots\); bots join only when/);
  },
);
