'use strict';

// The client LocalNpc global (Client/resources/local_npcs.cjs): NPC bodies only this machine
// shows, and what each call sends the game client.
const test = require('node:test');
const assert = require('node:assert/strict');
const {
  ResourceLocalNpcs,
  MAX_LOCAL_NPCS,
} = require('../../../../Client/resources/local_npcs.cjs');
const { Vector3 } = require('./math.cjs');

const SOUL = '210170d8-afb2-4cbd-9df5-da2a8e722af4';

function setup() {
  const sent = [];
  const npcs = new ResourceLocalNpcs({ send: (message) => sent.push(message), Vector3 });
  const ops = (op) => sent.filter((message) => message.op === op);
  return { npcs, sent, ops, LocalNpc: npcs.facade('guide') };
}

test('create sends a body without AI unless asked, and the look at once', () => {
  const { LocalNpc, ops } = setup();
  const figure = LocalNpc.create({
    soul: SOUL,
    name: 'Sprievodca',
    position: { x: 1, y: 2, z: 3 },
    appearance: { head: 'm_head_004' },
  });
  const living = LocalNpc.create({ position: { x: 0, y: 0, z: 0 }, ai: true });
  const [first, second] = ops('localNpcCreate');
  assert.equal(first.id, figure.id);
  assert.equal(first.actorClass, 'NPC_NAI');
  assert.equal(first.soul, SOUL);
  assert.equal(first.name, 'Sprievodca');
  assert.deepEqual(first.position, { x: 1, y: 2, z: 3 });
  assert.deepEqual(first.rotation, { x: 0, y: 0, z: 0, w: 1 });
  assert.equal(second.actorClass, 'NPC');
  assert.notEqual(second.id, first.id);
  assert.deepEqual(ops('localNpcAppearance'), [
    { op: 'localNpcAppearance', id: figure.id, appearance: '|m_head_004||' },
  ]);
  assert.deepEqual(LocalNpc.all(), [figure, living]);
  assert.equal(figure.name, 'Sprievodca');
  assert.deepEqual(figure.position, new Vector3(1, 2, 3));
  // The game has no female NPC without AI.
  LocalNpc.create({ position: { x: 0, y: 0, z: 0 }, class: 'NPC_Female' });
  assert.equal(ops('localNpcCreate').at(-1).actorClass, 'NPC_Female');
});

test('the calls send what a server NPC sends, and stop once the NPC is removed', () => {
  const { LocalNpc, ops } = setup();
  const npc = LocalNpc.create({ position: { x: 0, y: 0, z: 0 } });
  assert.equal(npc.setPosition({ x: 5, y: 6, z: 7 }), true);
  assert.equal(npc.setPosition({ x: 5, y: 6, z: 7 }, { x: 0, y: 0, z: 180 }), true);
  assert.equal(npc.setAppearance({ hair: 'm_hair_001_blonde' }), true);
  assert.equal(npc.setAppearance({ head: 'no_such_head' }), false);
  assert.equal(npc.playAnimation('', { clip: 'reading_stand_inv_in', loop: true }), true);
  assert.equal(npc.playAnimation('Greetings', { tags: 'bowBig' }), true);
  assert.equal(npc.stopAnimation(), true);
  assert.equal(npc.say('Poď sem!', { duration: 4000 }), true);
  assert.equal(npc.say(''), true);
  const places = ops('localNpcPlace');
  assert.deepEqual(places[0].position, { x: 5, y: 6, z: 7 });
  assert.deepEqual(places[0].rotation, { x: 0, y: 0, z: 0, w: 1 });
  assert.ok(Math.abs(Math.abs(places[1].rotation.z) - 1) < 1e-6);
  assert.deepEqual(
    ops('localNpcAppearance').map((message) => message.appearance),
    ['||m_hair_001_blonde|'],
  );
  assert.deepEqual(
    ops('localNpcAnimation').map((message) => message.animation),
    ['1|1|reading_stand_inv_in', '2|0||Greetings|bowBig', ''],
  );
  assert.deepEqual(
    ops('localNpcSay').map((message) => message.speech),
    ['1|4000|Poď sem!', ''],
  );
  npc.remove();
  assert.equal(npc.exists, false);
  assert.deepEqual(ops('localNpcRemove'), [{ op: 'localNpcRemove', id: npc.id }]);
  assert.equal(npc.setPosition({ x: 0, y: 0, z: 0 }), false);
  assert.equal(npc.say('a'), false);
  assert.equal(npc.playAnimation('', { clip: 'a' }), false);
  npc.remove();
  assert.equal(ops('localNpcRemove').length, 1);
});

test('a stopped resource takes its local NPCs with it, and only its own', () => {
  const { npcs, ops } = setup();
  const guide = npcs.facade('guide');
  const shop = npcs.facade('shop');
  guide.create({ position: { x: 0, y: 0, z: 0 } });
  const kept = shop.create({ position: { x: 0, y: 0, z: 0 } });
  assert.equal(guide.all().length, 1);
  npcs.removeOwner('guide');
  assert.equal(ops('localNpcRemove').length, 1);
  assert.deepEqual(guide.all(), []);
  assert.deepEqual(shop.all(), [kept]);
});

test('create refuses bad options and more than MAX_LOCAL_NPCS bodies', () => {
  const { LocalNpc, sent } = setup();
  assert.throws(() => LocalNpc.create(null), TypeError);
  assert.throws(() => LocalNpc.create({ position: { x: 0, y: 0 } }), TypeError);
  assert.throws(() => LocalNpc.create({ position: { x: 0, y: 0, z: 0 }, soul: 'x' }), RangeError);
  assert.throws(() => LocalNpc.create({ position: { x: 0, y: 0, z: 0 }, ai: 1 }), TypeError);
  assert.throws(
    () => LocalNpc.create({ position: { x: 0, y: 0, z: 0 }, appearance: { head: 'x' } }),
    RangeError,
  );
  assert.equal(sent.length, 0);
  for (let i = 0; i < MAX_LOCAL_NPCS; i += 1) {
    LocalNpc.create({ position: { x: 0, y: 0, z: 0 } });
  }
  assert.throws(() => LocalNpc.create({ position: { x: 0, y: 0, z: 0 } }), RangeError);
});
