'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');

// Freecam.setEnabled (a KCDS addition): whether players may open the game's photo mode (F1).

test('the setting reaches every player, and a newcomer on arrival', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-freecam-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  await host.startAll();
  const player = (id) =>
    host.updatePlayer({
      id,
      ready: true,
      position: { x: 0, y: 0, z: 0 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
    });
  const freecam = () => output.filter((e) => e.op === 'freecam').map((e) => [e.id, e.allowed]);
  player(7);
  assert.deepEqual(freecam(), []);
  const api = host.api.Freecam;
  assert.equal(api.enabled, true);
  api.setEnabled(false);
  player(8);
  assert.deepEqual(freecam(), [
    [7, false],
    [8, false],
  ]);
  api.setEnabled(false);
  assert.equal(freecam().length, 2);
  assert.throws(() => api.setEnabled('no'), TypeError);
});
