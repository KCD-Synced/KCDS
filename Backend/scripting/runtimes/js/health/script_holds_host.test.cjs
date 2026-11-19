'use strict';

// The server host names the resource and the code that held the script runtime.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { ResourceHost } = require('../host.cjs');

function fixture(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-holds-'));
  const logs = [];
  const host = new ResourceHost(root, { send: () => {}, log: (...entry) => logs.push(entry) });
  t.after(async () => {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  });
  return { root, logs, host };
}

function resource(root, name, code) {
  const dir = path.join(root, name);
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name, kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
}

const BUSY = `
  function busy(ms) {
    const until = performance.now() + ms;
    while (performance.now() < until) {}
  }
`;

test('a hold by a timer, a start handler or a loading script names its resource', async (t) => {
  const { root, logs, host } = fixture(t);
  resource(
    root,
    'catalogue',
    `${BUSY}
    busy(230);
    kcds.core.Events.on('resourceStart', (name) => {
      if (name === 'catalogue') busy(260);
    });`,
  );
  resource(
    root,
    'reconcile',
    `${BUSY}
    const timer = setInterval(() => {
      busy(240);
      clearInterval(timer);
    }, 20);`,
  );
  resource(root, 'quiet', `setInterval(() => {}, 20);`);
  await host.startAll();
  await new Promise((resolve) => setTimeout(resolve, 400));
  const held = logs.filter(([, text]) => text.startsWith('The script runtime was held'));
  const lines = held.map(([source, text]) => `${source}: ${text.replace(/\d+ ms/, 'N ms')}`);
  assert.deepEqual(lines.sort(), [
    'catalogue: The script runtime was held for N ms by catalogue (loading index.js); ' +
      'every resource and server event waited',
    'reconcile: The script runtime was held for N ms by reconcile ' +
      '(setInterval callback, every 20 ms); every resource and server event waited',
  ]);
  // The start handler's hold is counted, in the line the ten seconds after its first one.
  const longest = host.holds.takeLongestNamed();
  assert.ok(longest >= 260, `${longest} ms`);
  // KCDS-Admin's numbers.
  const usage = host.holds.usageOf('reconcile');
  assert.ok(usage.cpuMs >= 240, JSON.stringify(usage));
  assert.equal(usage.slowest[0].kind, 'setInterval callback, every 20 ms');
  assert.ok(host.holds.usageOf('quiet').cpuMs < 50);
  await host.stop('reconcile');
  assert.deepEqual(host.holds.usageOf('reconcile'), { cpuMs: 0, slowest: [] });
});
