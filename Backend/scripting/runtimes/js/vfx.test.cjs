'use strict';

// Vfx: the particle effects the server puts in the world.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');

function hostWith(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-vfx-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'fx');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'fx', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  const chat = () => output.filter((e) => e.op === 'chat').map((e) => e.text);
  return { host, chat, output };
}

const MAGIC = 'WH_Particels.other.magic';

test('Vfx.spawn replicates an effect, and destroy takes it away', async () => {
  const { host, chat, output } = hostWith(`
    kcds.core.Events.on('vfxSpawn', (v) => kcds.core.Chat.sendToAll('spawn ' + v.group));
    kcds.core.Events.on('vfxDestroy', (v) => kcds.core.Chat.sendToAll('destroy ' + v.effect));
    kcds.world.Vfx.spawn('${MAGIC}', { x: 1, y: 2, z: 3 }, { scale: 99, prime: true }, 7);
  `);
  await host.startAll();
  const { Vfx } = host.makeAPI();
  const [vfx] = Vfx.all();
  assert.equal(vfx.effect, MAGIC);
  assert.equal(vfx.library, 'WH_Particels');
  assert.equal(vfx.group, 'other');
  assert.equal(vfx.scale, 32);
  assert.equal(vfx.strength, -1);
  assert.equal(vfx.dimension, 7);
  assert.equal(vfx.position.z, 3);
  assert.equal(Vfx.getById(vfx.id), vfx);
  assert.equal(new Vfx(vfx.id), vfx);
  assert.deepEqual(Vfx.all(8), []);
  const spawn = output.find((e) => e.op === 'vfxSpawn');
  assert.equal(spawn.id, vfx.id);
  assert.equal(spawn.effect, MAGIC);
  assert.deepEqual({ ...spawn.position }, { x: 1, y: 2, z: 3 });
  assert.equal(spawn.scale, 32);
  assert.equal(spawn.prime, true);
  assert.equal(spawn.dimension, 7);
  vfx.destroy();
  vfx.destroy();
  assert.equal(Vfx.getById(vfx.id), null);
  assert.equal(output.filter((e) => e.op === 'vfxDestroy').length, 1);
  assert.deepEqual(chat(), ['spawn other', `destroy ${MAGIC}`]);
});

test('Vfx.spawn on a body hangs on its joint and goes when the player leaves', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  const player = host.updatePlayer({
    id: 3,
    ready: true,
    position: { x: 0, y: 0, z: 10 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const { Vfx } = host.makeAPI();
  const flame = Vfx.spawn(
    'cinematics.fires.fire_torch_refr',
    { x: 0, y: 0, z: 0.45 },
    { entity: player, joint: 'RightHand' },
  );
  const spawn = output.filter((e) => e.op === 'vfxSpawn').at(-1);
  assert.equal(spawn.body, 3);
  assert.equal(spawn.joint, 'RightHand');
  assert.equal(flame.joint, 'RightHand');
  assert.throws(
    () => Vfx.spawn('cinematics.fires.fire_torch_refr', {}, { joint: 'Head' }),
    TypeError,
  );
  assert.throws(
    () => Vfx.spawn('cinematics.fires.fire_torch_refr', { x: 9, y: 0, z: 0 }, { entity: player }),
    RangeError,
  );
  host.vfx.leaving(3);
  assert.equal(output.filter((e) => e.op === 'vfxDestroy').at(-1).id, flame.id);
});

test('Vfx tuning restarts the effect only when a value changes', async () => {
  const { host, output } = hostWith(`kcds.world.Vfx.spawn('${MAGIC}');`);
  await host.startAll();
  const { Vfx } = host.makeAPI();
  const [vfx] = Vfx.all();
  vfx.countScale = 1;
  assert.equal(output.filter((e) => e.op === 'vfxUpdate').length, 0);
  vfx.pulsePeriod = 1000;
  vfx.strength = 0.5;
  const updates = output.filter((e) => e.op === 'vfxUpdate');
  assert.equal(updates.length, 2);
  assert.equal(updates[1].pulsePeriod, 600);
  assert.equal(updates[1].strength, 0.5);
  assert.throws(() => (vfx.scale = 'big'), TypeError);
});

test('Vfx.spawn refuses bad names and worlds; list reads the catalog', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const { Vfx } = host.makeAPI();
  assert.throws(() => Vfx.spawn('campfire'), TypeError);
  // a library the game has, but no such effect in it.
  assert.throws(() => Vfx.spawn('WH_Particels.fires.campfire_a'), RangeError);
  assert.throws(() => Vfx.spawn(MAGIC, {}, {}, -1), TypeError);
  assert.throws(() => Vfx.spawn(MAGIC, { x: 1e9 }), RangeError);
  assert.ok(Vfx.list().includes(MAGIC));
  assert.deepEqual(Vfx.list('nothing'), []);
});

test('Vfx.destroyAll clears one dimension or all of them', async () => {
  const { host, output } = hostWith(`
    kcds.world.Vfx.spawn('${MAGIC}', {}, {}, 1);
    kcds.world.Vfx.spawn('${MAGIC}', {}, {}, 1);
    kcds.world.Vfx.spawn('${MAGIC}', {}, {}, 2);
  `);
  await host.startAll();
  const { Vfx } = host.makeAPI();
  assert.equal(Vfx.destroyAll(1), 2);
  assert.equal(Vfx.all().length, 1);
  assert.equal(Vfx.destroyAll(), 1);
  assert.equal(output.filter((e) => e.op === 'vfxDestroy').length, 3);
});

test('Vfx.burst and burstFrom tell only the players in range', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  const { Vfx, Prop } = host.makeAPI();
  const near = { id: 1, dimension: 0xffffffff, position: { x: 0, y: 0, z: 0 } };
  const far = { id: 2, dimension: 0xffffffff, position: { x: 900, y: 0, z: 0 } };
  host.players.set(1, near);
  host.players.set(2, far);
  assert.equal(Vfx.burst(MAGIC, { x: 10, y: 0, z: 0 }, { durationMs: 1e9 }), 1);
  const burst = output.find((e) => e.op === 'vfxBurst');
  assert.deepEqual(burst.players, [1]);
  assert.equal(burst.durationMs, 60000);
  assert.equal(Vfx.burst(MAGIC, { x: 10 }, {}, 3), 0);
  assert.equal(output.filter((e) => e.op === 'vfxBurst').length, 1);
  const prop = Prop.spawn('barrel_a', { x: 5, y: 0, z: 0 });
  assert.equal(Vfx.burstFrom(MAGIC, prop, { offset: { z: 1 } }), 1);
  assert.equal(output.find((e) => e.op === 'vfxBurstOn').entity, prop.id);
  assert.equal(output.find((e) => e.op === 'vfxBurstOn').joint, '');
  // On a joint of the body.
  Vfx.burstFrom(MAGIC, prop, { joint: 'Bip01 R Hand' });
  assert.equal(output.filter((e) => e.op === 'vfxBurstOn').at(-1).joint, 'Bip01 R Hand');
  assert.throws(() => Vfx.burstFrom(MAGIC, prop, { joint: 'x'.repeat(65) }), RangeError);
  assert.throws(() => Vfx.burstFrom(MAGIC, prop, { offset: { z: 9 } }), RangeError);
  assert.throws(() => Vfx.burstFrom('WH_Particels.fires.campfire_a', prop), RangeError);
  assert.throws(() => Vfx.burstFrom(MAGIC, 987654321), Error);
});
