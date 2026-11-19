'use strict';

// How long each client resource holds the client scripts with synchronous calls to the game
// (Client/resources/sync_waits.cjs, sync_channel.cjs, runtime.cjs).
const test = require('node:test');
const assert = require('node:assert/strict');
const { SyncWaits } = require('../../../../Client/resources/sync_waits.cjs');
const { SyncChannel } = require('../../../../Client/resources/sync_channel.cjs');

function watched() {
  const logged = [];
  let time = 0;
  const waits = new SyncWaits({
    log: (owner, text) => logged.push([owner, text]),
    now: () => time,
  });
  return {
    waits,
    logged,
    at: (ms) => {
      time = ms;
    },
  };
}

test('a resource that waited over 200 ms in a second is named with its calls', () => {
  const { waits, logged, at } = watched();
  for (let call = 0; call < 38; call++) {
    waits.record('my_hud', 'localWorldCamera', 16);
  }
  waits.record('my_hud', 'localWorldRay', 20);
  waits.record('quiet', 'localWorldRay', 50);
  at(1000);
  waits.record('', 'localWorldRay', 1);
  assert.equal(logged.length, 1);
  assert.equal(logged[0][0], 'my_hud');
  assert.match(logged[0][1], /^my_hud held the client scripts for 628 ms in the last second/);
  assert.match(logged[0][1], /Camera\.getPose ×38, World\.raycast ×1/);
});

test('a resource is named at most once every 30 seconds', () => {
  const { waits, logged, at } = watched();
  const second = (start) => {
    waits.record('busy', 'followCheck', 300);
    at(start + 1000);
    waits.record('', 'x', 0);
  };
  second(0);
  second(1000);
  second(2000);
  assert.equal(logged.length, 1);
  assert.match(logged[0][1], /followCheck ×1/);
  at(40000);
  waits.record('busy', 'followCheck', 300);
  at(41000);
  waits.record('', 'x', 0);
  assert.equal(logged.length, 2);
});

test('a long wait in a window that ran long is measured against the time it ran', () => {
  const { waits, logged, at } = watched();
  // One call that waited 2 s while the game loaded, then nothing for 37 s.
  waits.record('my_hud', 'localWorldCamera', 2013);
  at(37000);
  waits.record('my_hud', 'localWorldRay', 10);
  assert.deepEqual(logged, [], '2013 ms over 37 s is no hold');
  // Raycasts every frame for the next seconds: named.
  for (let call = 0; call < 45; call++) {
    waits.record('my_hud', 'localWorldRay', 8);
  }
  at(38000);
  waits.record('', 'x', 0);
  assert.equal(logged.length, 1);
  assert.match(logged[0][1], /World\.raycast ×46/);
  at(41000);
  waits.record('my_hud', 'localWorldRay', 900);
  at(44000);
  waits.record('', 'x', 0);
  assert.equal(logged.length, 1, 'still within the quiet time');
});

test('a hold over more seconds says so', () => {
  const { waits, logged, at } = watched();
  waits.record('slow', 'followCheck', 900);
  at(3000);
  waits.record('', 'x', 0);
  assert.equal(logged.length, 1);
  assert.match(logged[0][1], /^slow held the client scripts for 900 ms in the last 3 seconds/);
});

test('the runtime’s own calls and short waits are not named', () => {
  const { waits, logged, at } = watched();
  waits.record('', 'localWorldCamera', 900);
  waits.record('light', 'localWorldCamera', 150);
  at(1000);
  waits.record('', 'x', 0);
  assert.deepEqual(logged, []);
});

test('the sync channel tells the waits each call with the resource that made it', () => {
  const told = [];
  const channel = new SyncChannel(() => {}, {
    waits: { record: (owner, op, ms) => told.push([owner, op, ms]) },
    who: () => 'my_hud',
  });
  // Without the page's buffer there is nothing to wait for; the call is still told.
  assert.equal(channel.call({ op: 'localWorldCamera' }), undefined);
  assert.deepEqual(told, []);
  const buffer = new SharedArrayBuffer(8 + 262144);
  channel.attach(buffer);
  assert.equal(channel.call({ op: 'localWorldCamera' }, 5), undefined);
  assert.equal(told.length, 1);
  assert.equal(told[0][0], 'my_hud');
  assert.equal(told[0][1], 'localWorldCamera');
  assert.ok(told[0][2] >= 0);
});
