'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const { ResourceHost } = require('../host.cjs');
const { lookupItemClass } = require('../inventory/catalog.cjs');

const HOME = lookupItemClass('key_home').id;
const SHOP = lookupItemClass('key_shop').id;
const AT = { x: 10, y: 10, z: 0 };

async function fixture() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-area-keys-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  await host.startAll();
  const { Area, Door, Inventory, Stash } = host.api;
  const area = Area.create({
    id: 'house',
    type: 'box',
    position: AT,
    min: { x: -10, y: -10, z: -5 },
    max: { x: 10, y: 10, z: 5 },
  });
  const player = host.updatePlayer({
    id: 7,
    nickname: 'P7',
    ready: true,
    position: { x: 9, y: 9, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  host.doors.report(player, [
    { guid: '00000000000000ab', name: 'door', position: AT, open: false, locked: false, key: '' },
  ]);
  const door = Door.find('00000000000000ab');
  return { host, output, area, door, player, Inventory, Stash };
}

test('an area key opens its doors; a door key of its own goes first', async (t) => {
  const { host, area, door, player, Inventory } = await fixture();
  t.after(() => host.areas.keys.stop());
  assert.equal(area.lockKey, '');
  assert.throws(() => {
    area.lockKey = 'no_such_key';
  }, TypeError);
  area.lockKey = 'key_home';
  assert.equal(area.lockKey, HOME);
  assert.equal(door.key, HOME);
  assert.equal(area.setLocked(true) >= 1, true);
  assert.equal(door.locked, true);

  host.doors.use(player, door.guid, 'unlock', false);
  assert.equal(door.locked, true, 'without the key');
  Inventory.add(player, { item: 'key_home', amount: 1 });
  door.setKey('key_shop');
  assert.equal(door.key, SHOP);
  host.doors.use(player, door.guid, 'unlock', false);
  assert.equal(door.locked, true, 'the door has a key of its own');
  door.setKey(null);
  assert.equal(door.key, HOME);
  host.doors.use(player, door.guid, 'unlock', false);
  assert.equal(door.locked, false, 'the area key');

  area.lockKey = null;
  assert.equal(area.lockKey, '');
  assert.equal(door.key, '');
});

test('player.keyBypass opens a locked door and shows it unlocked to them only', async (t) => {
  const { host, output, area, door, player } = await fixture();
  t.after(() => host.areas.keys.stop());
  area.lockKey = 'key_home';
  area.setLocked(true);
  const last = () => output.filter((e) => e.op === 'doorSet' && e.id === 7).at(-1);
  assert.equal(last().locked, true);
  assert.throws(() => {
    player.keyBypass = 'yes';
  }, TypeError);
  host.doors.use(player, door.guid, 'open', false);
  assert.equal(door.open, false, 'no bypass yet');
  player.keyBypass = true;
  assert.equal(last().locked, false);
  assert.deepEqual(output.filter((e) => e.op === 'playerKeyBypass').at(-1), {
    op: 'playerKeyBypass',
    id: 7,
    bypass: true,
  });
  host.doors.use(player, door.guid, 'open', false);
  assert.equal(door.open, true);
  assert.equal(door.locked, true, 'the lock stays for the others');
});

test('a locked door a bypass player opened is open in the other games, locked again when shut', async (t) => {
  const { host, output, area, door, player } = await fixture();
  t.after(() => host.areas.keys.stop());
  const other = host.updatePlayer({
    id: 8,
    nickname: 'P8',
    ready: true,
    position: { x: 8, y: 8, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  host.doors.report(other, [
    { guid: '00000000000000ab', name: 'door', position: AT, open: false, locked: false, key: '' },
  ]);
  area.lockKey = 'key_home';
  area.setLocked(true);
  const seenBy = (id) => output.filter((e) => e.op === 'doorSet' && e.id === id).at(-1);
  assert.equal(seenBy(8).locked, true);
  player.keyBypass = true;
  host.doors.use(player, door.guid, 'open', false);
  assert.equal(seenBy(8).open, true);
  assert.equal(seenBy(8).locked, false, 'the game cannot hold a door open and locked');
  assert.equal(door.locked, true);
  host.doors.use(player, door.guid, 'close', false);
  assert.equal(seenBy(8).open, false);
  assert.equal(seenBy(8).locked, true);
});

test('a chest in the area takes its key unless it has its own', async (t) => {
  const { host, area, Stash } = await fixture();
  t.after(() => host.areas.keys.stop());
  const chest = Stash.spawn({ x: 11, y: 11, z: 0 });
  const other = Stash.spawn({ x: 12, y: 12, z: 0 });
  other.keyClass = SHOP;
  area.lockKey = 'key_home';
  assert.equal(chest.keyClass, HOME);
  assert.equal(other.keyClass, SHOP);
  other.keyClass = '';
  assert.equal(other.keyClass, HOME);
});
