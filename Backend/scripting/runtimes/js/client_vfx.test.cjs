'use strict';

// The client Vfx (Client/resources/vfx.cjs): effects a client script plays in this
// game only, with the server's checks.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceVfx, FIRST_ID } = require('../../../../Client/resources/vfx.cjs');

function setup() {
  const sent = [];
  const vfx = new ResourceVfx({ send: (message) => sent.push(message) });
  return { vfx, sent, archery: vfx.facade('archery'), other: vfx.facade('other') };
}

test('spawn places an effect with a local id, destroy and the resource stop remove it', () => {
  const { vfx, sent, archery, other } = setup();
  const sparks = archery.spawn('WH_Particels.other.magic', { x: 1, y: 2, z: 3 }, { scale: 99 });
  assert.equal(sparks.id, FIRST_ID);
  assert.deepEqual(sent[0], {
    op: 'vfxLocal',
    kind: 'spawn',
    id: FIRST_ID,
    effect: 'WH_Particels.other.magic',
    position: { x: 1, y: 2, z: 3 },
    rotation: { x: 0, y: 0, z: 0, w: 1 },
    prime: false,
    scale: 32,
    countScale: 1,
    speedScale: 1,
    timeScale: 1,
    strength: -1,
    pulsePeriod: 0,
  });
  assert.equal(sparks.destroy(), true);
  assert.equal(sparks.destroy(), false);
  assert.deepEqual(sent.at(-1), { op: 'vfxLocal', kind: 'destroy', id: FIRST_ID });
  archery.spawn('WH_Particels.other.magic', { x: 1, y: 2, z: 3 });
  const kept = other.spawn('WH_Particels.other.magic', { x: 1, y: 2, z: 3 });
  vfx.removeOwner('archery');
  assert.deepEqual(sent.at(-1), { op: 'vfxLocal', kind: 'destroy', id: FIRST_ID + 1 });
  assert.equal(kept.destroy(), true);
});

test('burst and burstFrom play once; bad effects, points and entities are refused', () => {
  const { sent, archery } = setup();
  assert.equal(archery.burst('WH_Particels.other.gravel', { x: 5, y: 5, z: 5 }), true);
  assert.equal(sent.at(-1).kind, 'burst');
  assert.equal(sent.at(-1).durationMs, 2000);
  assert.equal(
    archery.burstFrom('WH_Particels.other.magic', { id: 7 }, { offset: { x: 0, y: 0, z: 2 } }),
    true,
  );
  assert.equal(sent.at(-1).entity, 7);
  assert.deepEqual(sent.at(-1).offset, { x: 0, y: 0, z: 2 });
  assert.equal(sent.at(-1).joint, '');
  // On a joint of the body.
  archery.burstFrom('WH_Particels.other.magic', 7, { joint: 'Bip01 R Hand' });
  assert.equal(sent.at(-1).joint, 'Bip01 R Hand');
  const count = sent.length;
  assert.throws(() => archery.burstFrom('WH_Particels.other.magic', 7, { joint: 3 }), TypeError);
  assert.throws(() => archery.burstFrom('WH_Particels.other.magic', 7, { joint: '' }), RangeError);
  assert.throws(() => archery.burst('sparks', { x: 0, y: 0, z: 0 }), TypeError);
  assert.throws(
    () => archery.burst('WH_Particels.other.gravel', { x: 1e7, y: 0, z: 0 }),
    RangeError,
  );
  assert.throws(() => archery.burstFrom('WH_Particels.other.magic', null), TypeError);
  assert.throws(
    () => archery.burstFrom('WH_Particels.other.magic', 7, { offset: { x: 9, y: 0, z: 0 } }),
    RangeError,
  );
  assert.equal(sent.length, count);
});

test("list gives the game's effect names, the server's catalog, by prefix", () => {
  const { archery } = setup();
  const all = archery.list();
  assert.ok(all.length > 500);
  assert.ok(all.includes('WH_Particels.other.magic'));
  assert.ok(
    archery.list('WH_Particels.fires').every((name) => name.startsWith('WH_Particels.fires')),
  );
  assert.ok(archery.list('WH_Particels.fires').length > 0);
  assert.throws(() => archery.list(5), TypeError);
});

test('stop, stopAll, isPlaying and count keep to the resource', () => {
  const { archery, other, sent } = setup();
  const name = archery.list()[0];
  const first = archery.spawn(name, { x: 1, y: 2, z: 3 });
  const second = archery.spawn(name, { x: 1, y: 2, z: 3 });
  const theirs = other.spawn(name, { x: 1, y: 2, z: 3 });
  assert.equal(archery.has(name), true);
  assert.equal(archery.has('no such effect'), false);
  assert.equal(archery.count(), 2);
  assert.equal(archery.isPlaying(first), true);
  assert.equal(archery.isPlaying(theirs), false);
  assert.equal(archery.stop(theirs), false, 'another resource keeps its effect');
  assert.equal(other.isPlaying(theirs), true);
  assert.equal(archery.stop(first.id), true);
  assert.equal(sent.pop().kind, 'destroy');
  assert.equal(archery.isPlaying(first), false);
  assert.equal(archery.stop(first), false);
  assert.equal(archery.stopAll(), 1);
  assert.equal(archery.isPlaying(second), false);
  assert.equal(archery.count(), 0);
  assert.equal(other.count(), 1);
});

test('client Vfx.attach hangs an effect on a body until it is stopped', () => {
  const { ResourceVfx } = require('../../../../Client/resources/vfx.cjs');
  const sent = [];
  const registry = new ResourceVfx({ send: (message) => sent.push(message) });
  const Vfx = registry.facade('mine');
  const effect = Vfx.list()[0];
  const handle = Vfx.attach(effect, { id: 7 }, { offset: { x: 0, y: 0, z: 1 }, joint: 'Head' });
  const order = sent.at(-1);
  assert.equal(order.kind, 'spawn');
  assert.equal(order.id, handle.id);
  assert.equal(order.entity, 7);
  assert.equal(order.joint, 'Head');
  assert.deepEqual(order.position, { x: 0, y: 0, z: 1 });
  assert.equal(Vfx.isPlaying(handle), true);
  assert.equal(Vfx.count(), 1);
  assert.equal(Vfx.attach(effect, 7).id > handle.id, true, 'a network id names the body too');
  assert.equal(Vfx.stopAll(), 2);
  assert.equal(Vfx.isPlaying(handle), false);
  const count = sent.length;
  assert.throws(() => Vfx.attach(effect, undefined), TypeError);
  assert.throws(() => Vfx.attach(effect, 7, { offset: { x: 9, y: 0, z: 0 } }), RangeError);
  assert.throws(() => Vfx.attach(effect, 7, { durationMs: 0 }), RangeError);
  assert.equal(sent.length, count);
});
