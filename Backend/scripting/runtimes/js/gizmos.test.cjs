'use strict';

// Gizmo: debug shapes the players' games draw.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');

function hostWith(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-gizmo-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'debug');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'debug', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  return { host, output };
}

const ORIGIN = { x: 1, y: 2, z: 3 };

test('each shape goes out as one gizmoDraw with its own fields', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  const { Gizmo } = host.makeAPI();
  const line = Gizmo.line(ORIGIN, { x: 4, y: 5, z: 6 }, { color: { r: 1, g: 0, b: 0 } });
  const sphere = Gizmo.sphere(ORIGIN, 2, { durationMs: 5000, players: [7, { id: 8 }] });
  const box = Gizmo.box(ORIGIN, { x: 1, y: 2, z: 3 });
  const label = Gizmo.text(ORIGIN, 'camp\nhere', { textSize: 2 });
  const draws = output.filter((e) => e.op === 'gizmoDraw');
  assert.equal(draws.length, 4);
  assert.deepEqual(
    draws.map((d) => d.gizmo),
    [line, sphere, box, label],
  );
  assert.deepEqual(draws[0].end, { x: 4, y: 5, z: 6 });
  assert.deepEqual(draws[0].color, { r: 1, g: 0, b: 0, a: 1 });
  assert.equal(draws[0].durationMs, 0);
  assert.equal(draws[0].players, undefined, 'every ready player');
  assert.equal(draws[1].radius, 2);
  assert.deepEqual(draws[1].players, [7, 8]);
  assert.deepEqual(draws[2].size, { x: 1, y: 2, z: 3 });
  assert.deepEqual(draws[2].rotation, { x: 0, y: 0, z: 0, w: 1 });
  assert.equal(draws[3].text, 'camp here');
  assert.equal(draws[3].textSize, 2);
  assert.deepEqual(draws[3].color, { r: 1, g: 1, b: 1, a: 1 });
});

test('an id redraws in place, and clear takes one or all', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  const { Gizmo } = host.makeAPI();
  assert.equal(Gizmo.line(ORIGIN, ORIGIN, { id: 42 }), 42);
  Gizmo.clear(42);
  Gizmo.clear();
  const clears = output.filter((e) => e.op === 'gizmoClear');
  assert.deepEqual(
    clears.map((c) => c.gizmo),
    [42, 0],
  );
});

test('bad values are refused before anything is sent', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  const { Gizmo } = host.makeAPI();
  assert.throws(() => Gizmo.line({ x: NaN, y: 0, z: 0 }, ORIGIN), TypeError);
  assert.throws(() => Gizmo.sphere(ORIGIN, -1), TypeError);
  assert.throws(() => Gizmo.text(ORIGIN, ''), TypeError);
  assert.throws(() => Gizmo.line(ORIGIN, ORIGIN, { color: { r: 2 } }), TypeError);
  assert.throws(() => Gizmo.line(ORIGIN, ORIGIN, { durationMs: 1.5 }), TypeError);
  assert.throws(() => Gizmo.line(ORIGIN, ORIGIN, { players: 'all' }), TypeError);
  assert.equal(output.filter((e) => e.op === 'gizmoDraw').length, 0);
});

test('a stopped resource clears the gizmos it drew', async () => {
  const { host, output } = hostWith(
    `globalThis.drawn = kcds.world.Gizmo.sphere({ x: 0, y: 0, z: 0 }, 1);`,
  );
  await host.startAll();
  const drawn = output.find((e) => e.op === 'gizmoDraw').gizmo;
  await host.stop('debug');
  const clear = output.find((e) => e.op === 'gizmoClear');
  assert.equal(clear.gizmo, drawn);
});
