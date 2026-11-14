'use strict';

// Player.setLantern: the game's NPC lamp in a player's hand, with its light.
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

test('setLantern hangs the game lamp in the hand slot with a lantern light on the same joint', async () => {
  const { player, sent } = await started();
  assert.equal(player.lantern, null);
  player.setLantern();
  assert.deepEqual(player.lantern, { model: 'leather', hand: 'left' });
  const prop = sent('propSpawn').at(-1);
  assert.match(prop.model, /lamp_leather_wearable/);
  assert.equal(prop.body, 7);
  assert.equal(prop.joint, 'LeftHandSlot');
  const light = sent('lightSpawn').at(-1);
  assert.equal(light.body, 7);
  assert.equal(light.joint, 'LeftHandSlot');
  assert.equal(light.radius, 6);
  assert.deepEqual(light.color, { x: 0.888, y: 0.337, z: 0.125 });
});

test('a new lantern replaces the old one, and null puts it out', async () => {
  const { player, sent } = await started();
  player.setLantern({ model: 'leather' });
  const first = { prop: sent('propSpawn').at(-1).id, light: sent('lightSpawn').at(-1).id };
  player.setLantern({ model: 'fancy', hand: 'right' });
  assert.deepEqual(player.lantern, { model: 'fancy', hand: 'right' });
  assert.deepEqual(
    sent('propDestroy').map((e) => e.id),
    [first.prop],
  );
  assert.deepEqual(
    sent('lightDestroy').map((e) => e.id),
    [first.light],
  );
  assert.match(sent('propSpawn').at(-1).model, /lamp_new_wearable_fancy/);
  assert.equal(sent('lightSpawn').at(-1).joint, 'RightHandSlot');
  player.setLantern(null);
  assert.equal(player.lantern, null);
  assert.equal(sent('propDestroy').length, 2);
  assert.equal(sent('lightDestroy').length, 2);
  // Out already: nothing more to send.
  player.setLantern(null);
  assert.equal(sent('lightDestroy').length, 2);
});

test('setLantern refuses a model or hand it does not know', async () => {
  const { player, sent } = await started();
  assert.throws(() => player.setLantern({ model: 'torch' }), TypeError);
  assert.throws(() => player.setLantern({ hand: 'both' }), TypeError);
  assert.throws(() => player.setLantern('leather'), TypeError);
  assert.equal(player.lantern, null);
  assert.equal(sent('propSpawn').length, 0);
});

test('a player who leaves takes the lantern and its light along', async () => {
  const { host, player, sent } = await started();
  player.setLantern({ model: 'plain' });
  const light = sent('lightSpawn').at(-1).id;
  host.disconnect(7);
  assert.ok(sent('lightDestroy').some((e) => e.id === light));
});
