'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');

// Dirt, blood and wear: Player.grime, clean and soil.

function hostWith() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-grime-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  return { host, output };
}

const report = {
  id: 7,
  ready: true,
  position: { x: 0, y: 0, z: 0 },
  rotation: { w: 1, x: 0, y: 0, z: 0 },
};

test('grime reads the reported state, null before a report', async () => {
  const { host } = hostWith();
  await host.startAll();
  const p = host.updatePlayer(report);
  assert.equal(p.grime, null);
  host.updatePlayer({
    ...report,
    grime: {
      body: { dirt: 0.5, blood: [0, 0.2, 0, 0, 0, 0] },
      items: [{ item: 'abc', dirt: 0.9, blood: [1, 0, 0, 0, 0, 0], wear: 0.75 }],
    },
  });
  assert.equal(p.grime.body.dirt, 0.5);
  assert.deepEqual(
    { ...p.grime.body.blood },
    { head: 0, torso: 0.2, leftArm: 0, rightArm: 0, leftLeg: 0, rightLeg: 0 },
  );
  assert.equal(p.grime.items[0].blood.head, 1);
  assert.equal(p.grime.items[0].item, 'abc');
  assert.equal(p.grime.items[0].wear, 0.75);
  assert.ok(Object.isFrozen(p.grime) && Object.isFrozen(p.grime.items[0]));
});

test('clean and soil send the order to the player and check their options', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const p = host.updatePlayer(report);
  assert.equal(p.clean(), true);
  assert.equal(p.clean({ clothes: false }), true);
  assert.equal(p.soil({ dirt: 1, blood: 0.5, body: false }), true);
  assert.deepEqual(
    output.filter((e) => e.op === 'grimeOrder'),
    [
      { op: 'grimeOrder', id: 7, kind: 'clean', body: true, clothes: true },
      { op: 'grimeOrder', id: 7, kind: 'clean', body: true, clothes: false },
      { op: 'grimeOrder', id: 7, kind: 'soil', body: false, clothes: true, dirt: 255, blood: 128 },
    ],
  );
  assert.throws(() => p.soil({ dirt: 2 }), RangeError);
  assert.throws(() => p.clean({ body: false, clothes: false }), TypeError);
  assert.throws(() => p.clean('all'), TypeError);
  host.unseatPlayer?.(7);
});
