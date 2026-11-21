'use strict';

// Player.setLantern: the game's NPC lamp in a player's hand. Every game lights the lamp with the
// game's own lantern light (Client/runtime/lanterns/lantern_light.hpp): the server sends no light.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');

async function started() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-lanterns-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  await host.startAll();
  const player = host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 0, y: 0, z: 10 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const sent = (op) => output.filter((e) => e.op === op);
  return { host, player, sent };
}

test('setLantern hangs the game lamp in the hand slot, lit by every game, no light of the server', async () => {
  const { player, sent } = await started();
  assert.equal(player.lantern, null);
  player.setLantern();
  assert.deepEqual(player.lantern, { model: 'leather', hand: 'left' });
  const prop = sent('propSpawn').at(-1);
  assert.match(prop.model, /lamp_leather_wearable/);
  assert.equal(prop.body, 7);
  assert.equal(prop.joint, 'LeftHandSlot');
  assert.equal(sent('lightSpawn').length, 0);
});

test('a new lantern replaces the old one, and null puts it out', async () => {
  const { player, sent } = await started();
  player.setLantern({ model: 'leather' });
  const first = sent('propSpawn').at(-1).id;
  player.setLantern({ model: 'fancy', hand: 'right' });
  assert.deepEqual(player.lantern, { model: 'fancy', hand: 'right' });
  assert.deepEqual(
    sent('propDestroy').map((e) => e.id),
    [first],
  );
  assert.match(sent('propSpawn').at(-1).model, /lamp_new_wearable_fancy/);
  assert.equal(sent('propSpawn').at(-1).joint, 'RightHandSlot');
  player.setLantern(null);
  assert.equal(player.lantern, null);
  assert.equal(sent('propDestroy').length, 2);
  // Out already: nothing more to send.
  player.setLantern(null);
  assert.equal(sent('propDestroy').length, 2);
});

test('setLantern refuses a model or hand it does not know', async () => {
  const { player, sent } = await started();
  assert.throws(() => player.setLantern({ model: 'torch' }), TypeError);
  assert.throws(() => player.setLantern({ hand: 'both' }), TypeError);
  assert.throws(() => player.setLantern('leather'), TypeError);
  assert.equal(player.lantern, null);
  assert.equal(sent('propSpawn').length, 0);
});

// The reserved client events kcds:lantern the player's client got, their pose in order.
function holds(sent) {
  return sent('event')
    .filter((e) => e.name === 'kcds:lantern')
    .map((e) => JSON.parse(e.payload).pose);
}

test('the left hand gets the game lamp hold and the lamp as it is; the right a quarter turn', async () => {
  const { player, sent } = await started();
  player.setLantern({ hand: 'left' });
  const left = sent('propSpawn').at(-1).rotation;
  assert.ok(Math.abs(left.w - 1) < 1e-6 && Math.abs(left.y) < 1e-6);
  assert.deepEqual(holds(sent), [true]);
  // The game holds a lamp in the left hand only: in the right it hangs, turned, without a hold.
  player.setLantern({ hand: 'right' });
  const right = sent('propSpawn').at(-1).rotation;
  const half = Math.SQRT1_2;
  assert.ok(Math.abs(right.w - half) < 1e-6 && Math.abs(right.y - half) < 1e-6);
  assert.deepEqual(holds(sent), [true, false]);
  player.setLantern(null);
  assert.deepEqual(holds(sent), [true, false, false]);
});

test('a rider carries the lantern in the right hand, the reins in the left, and back on foot', async () => {
  const { host, player, sent } = await started();
  player.setLantern({ hand: 'left' });
  const horse = host.api.Horse.spawn({ x: 1, y: 2, z: 3 });
  const report = (riderId) =>
    host.updateHorse({
      id: horse.id,
      position: { x: 1, y: 2, z: 3 },
      rotation: { x: 0, y: 0, z: 0, w: 1 },
      dimension: 0xffffffff,
      riderId,
    });
  report(7);
  assert.equal(sent('propSpawn').at(-1).joint, 'RightHandSlot');
  assert.equal(sent('propDestroy').length, 1);
  // No hold in the saddle (the hold is for the left hand).
  assert.deepEqual(holds(sent), [true, false]);
  // The script's choice stays what it asked for.
  assert.deepEqual(player.lantern, { model: 'leather', hand: 'left' });
  // Lit while riding, it goes to the right hand at once.
  player.setLantern({ model: 'fancy' });
  assert.equal(sent('propSpawn').at(-1).joint, 'RightHandSlot');
  report(0);
  assert.equal(sent('propSpawn').at(-1).joint, 'LeftHandSlot');
  assert.equal(holds(sent).at(-1), true);
  // A right hand lantern stays where it is on a horse.
  player.setLantern({ hand: 'right' });
  const spawned = sent('propSpawn').length;
  report(7);
  assert.equal(sent('propSpawn').length, spawned);
});

test("the game's torch drawn moves the lantern to the right hand, and back when put away", async () => {
  const { host, player, sent } = await started();
  player.setLantern({ hand: 'left' });
  const torch = (drawn) => host.lanterns.receive(player, 'kcds:lantern', { op: 'torch', drawn });
  assert.equal(torch(true), true);
  assert.equal(sent('propSpawn').at(-1).joint, 'RightHandSlot');
  assert.deepEqual(holds(sent), [true, false]);
  assert.deepEqual(player.lantern, { model: 'leather', hand: 'left' });
  // Lit with the torch out, it goes to the right hand at once.
  player.setLantern({ model: 'fancy' });
  assert.equal(sent('propSpawn').at(-1).joint, 'RightHandSlot');
  torch(false);
  assert.equal(sent('propSpawn').at(-1).joint, 'LeftHandSlot');
  assert.equal(holds(sent).at(-1), true);
  // A right hand lantern stays where it is.
  player.setLantern({ hand: 'right' });
  const spawned = sent('propSpawn').length;
  torch(true);
  assert.equal(sent('propSpawn').length, spawned);
  // Other events are not the lanterns'.
  assert.equal(host.lanterns.receive(player, 'kcds:seat', { op: 'use' }), false);
});

test('a lantern whose resource stopped ends its hold too', async () => {
  const { host, player, sent } = await started();
  player.setLantern();
  // The resource's props go with it.
  const lantern = host.lanterns.carried.get(player);
  host.props.destroy(lantern.prop);
  host.lanterns.resourceStopped();
  assert.equal(player.lantern, null);
  assert.deepEqual(holds(sent), [true, false]);
});

test('a player who leaves takes the lantern along', async () => {
  const { host, player, sent } = await started();
  player.setLantern({ model: 'plain' });
  const prop = sent('propSpawn').at(-1).id;
  host.disconnect(7);
  assert.ok(sent('propDestroy').some((e) => e.id === prop));
  assert.equal(host.lanterns.carried.size, 0);
});
