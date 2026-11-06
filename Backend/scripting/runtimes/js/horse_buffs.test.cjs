'use strict';

// The game's buffs a server puts on a horse: the list, its limits and the horseBuffs
// op every client's body of the horse follows; Horse.unconscious.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { MAX_HORSE_BUFFS } = require('./horses/horse_buffs.cjs');
const { BUFFS } = require('./buffs/catalog.cjs');

const MARIGOLD = '8503216a-a34c-49f0-aefa-54d4502046f9';

async function startedHost() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-horse-buffs-'));
  const output = [];
  const host = new ResourceHost(root, {
    entityIds: { first: 500, count: 1025 },
    send: (message) => output.push(message),
    log: () => {},
  });
  await host.startAll();
  const sent = () => output.filter((message) => message.op === 'horseBuffs');
  return { host, api: host.api, sent };
}

test('addBuff, hasBuff, removeBuff and clearBuffs keep the list and send it whole', async () => {
  const { api, sent } = await startedHost();
  const horse = api.Horse.spawn({ x: 1, y: 2, z: 3 });
  assert.deepEqual(horse.buffs, []);
  assert.equal(horse.addBuff('hangover'), true);
  assert.equal(horse.addBuff(MARIGOLD.toUpperCase()), true);
  assert.equal(horse.addBuff('hangover'), true, 'already on the list');
  assert.deepEqual(horse.buffs, ['hangover', 'potion_marigold_decoction']);
  assert.equal(horse.hasBuff(MARIGOLD), true);
  assert.equal(horse.hasBuff('drunk'), false);
  assert.equal(sent().length, 2, 'a buff already there sends nothing');
  assert.deepEqual(sent()[1], {
    op: 'horseBuffs',
    id: horse.id,
    buffs: ['7e252c71-5e41-472c-ad32-f223a664faab', MARIGOLD],
  });
  assert.equal(horse.removeBuff('hangover'), true);
  assert.equal(horse.removeBuff('hangover'), false);
  assert.deepEqual(sent()[2].buffs, [MARIGOLD]);
  assert.equal(horse.clearBuffs(), true);
  assert.equal(horse.clearBuffs(), false);
  assert.deepEqual(sent()[3].buffs, []);
  assert.deepEqual(horse.buffs, []);
});

test('a horse carries at most sixteen; anything that is no game buff throws', async () => {
  const { api } = await startedHost();
  const horse = api.Horse.spawn({ x: 1, y: 2, z: 3 });
  const names = BUFFS.slice(0, MAX_HORSE_BUFFS + 1).map((buff) => buff.name);
  for (const name of names.slice(0, MAX_HORSE_BUFFS)) {
    assert.equal(horse.addBuff(name), true);
  }
  assert.equal(horse.addBuff(names[MAX_HORSE_BUFFS]), false, 'the seventeenth');
  assert.equal(horse.addBuff(names[0]), true, 'one already there still counts');
  assert.equal(horse.buffs.length, MAX_HORSE_BUFFS);
  assert.throws(() => horse.addBuff('no_such_buff'), /not a buff/);
  assert.throws(() => horse.addBuff(7), /GUID or name/);
  api.Buffs.register('test:warm', { name: 'Warm', visual: 'hangover', duration: 10 });
  assert.throws(() => horse.addBuff('test:warm'), /custom buff/);
  assert.throws(() => horse.hasBuff('no_such_buff'), /not a buff/);
});

test('a destroyed horse takes no buffs', async () => {
  const { api, sent } = await startedHost();
  const horse = api.Horse.spawn({ x: 1, y: 2, z: 3 });
  horse.addBuff('hangover');
  horse.destroy();
  assert.equal(horse.addBuff('potion_marigold_decoction'), false);
  assert.equal(horse.clearBuffs(), true);
  assert.equal(sent().length, 1, 'nothing goes out for a horse that is gone');
});

test('unconscious reads like knockedOut', async () => {
  const { api } = await startedHost();
  const horse = api.Horse.spawn({ x: 1, y: 2, z: 3 }, {}, '', '', undefined, { knockOut: true });
  assert.equal(horse.unconscious, false);
  assert.equal(horse.knockDown(), true);
  assert.equal(horse.unconscious, true);
  assert.equal(horse.knockedOut, true);
  assert.equal(horse.wake(), true);
  assert.equal(horse.unconscious, false);
});
