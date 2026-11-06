'use strict';

// A server horse's saddlebags held by the server.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');

function setup() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-saddlebags-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const join = (id, position) => {
    host.updatePlayer({ id, ready: true, position, rotation: { w: 1, x: 0, y: 0, z: 0 } });
    return host.players.get(id);
  };
  const stock = (player, items) => {
    host.playerInventories.operate(player, (inventory) => inventory.set({ items }));
  };
  const shows = () => output.filter((e) => e.op === 'stashShow');
  return { host, output, join, stock, shows };
}

const rowsOf = (inventory) => inventory.items.map((row) => [row.name, row.amount]);

test('a horse has saddlebags the server holds, shown to its owner only', async () => {
  const { host, output, join, shows } = setup();
  await host.startAll();
  const api = host.makeAPI();
  const owner = join(7, { x: 0, y: 0, z: 0 });
  join(8, { x: 1, y: 0, z: 0 });
  const horse = api.Horse.spawn({ x: 2, y: 0, z: 0 });
  const bags = horse.storage;
  assert.equal(api.Stash.getById(bags.id), bags);
  assert.equal(horse.storage, bags);
  // Nothing stands for them on any client.
  assert.equal(output.filter((e) => e.op === 'stashSpawn').length, 0);
  assert.equal(bags.guid, '');
  horse.giveTo(owner);
  assert.deepEqual(
    shows().map((e) => [e.id, e.stash, e.horse, e.open, e.closed]),
    [[7, bags.id, horse.id, false, false]],
  );
  output.length = 0;
  bags.addItem({ item: 'apple', amount: 2 });
  assert.deepEqual(
    shows().map((e) => [e.id, e.horse, e.rows.length]),
    [[7, horse.id, 1]],
  );
  // They open no transfer screen and stay with the horse.
  assert.equal(bags.open(owner), false);
  bags.destroy();
  assert.equal(host.stashes.stashes.has(bags.id), true);
  // A new owner sees them; the old owner's game lets them go.
  output.length = 0;
  horse.giveTo(host.players.get(8));
  assert.deepEqual(
    shows().map((e) => [e.id, e.closed]),
    [
      [7, true],
      [8, false],
    ],
  );
  horse.destroy();
  assert.equal(host.stashes.stashes.has(bags.id), false);
});

test('only the owner near the horse moves items in and out, within its carry limit', async () => {
  const { host, join, stock } = setup();
  await host.startAll();
  const api = host.makeAPI();
  const owner = join(7, { x: 0, y: 0, z: 0 });
  const other = join(8, { x: 1, y: 0, z: 0 });
  const horse = api.Horse.spawn({ x: 3, y: 0, z: 0 }, {}, '', '', undefined, {
    maxCarryWeight: 2,
  });
  horse.giveTo(owner);
  const bags = horse.storage;
  stock(owner, [
    { id: 'blade', item: 'longSwordDuel', amount: 1 },
    { id: 'apples', item: 'apple', amount: 10 },
  ]);
  stock(other, [{ id: 'apples', item: 'apple', amount: 3 }]);
  assert.equal(host.stashes.put(owner, bags.id, 'apples', 4).ok, true);
  assert.equal(host.stashes.put(other, bags.id, 'apples', 1).ok, false, 'not the owner');
  // 1.2 kg of apples and a 1.9 kg sword is more than 2 kg.
  assert.equal(host.stashes.put(owner, bags.id, 'blade', 1).ok, false, 'too heavy');
  assert.deepEqual(rowsOf(bags.getInventory()), [['apple', 4]]);
  const apples = bags.getInventory().items[0].id;
  assert.equal(host.stashes.take(other, bags.id, apples, 1).ok, false, 'not the owner');
  assert.equal(host.stashes.take(owner, bags.id, apples, 3).ok, true);
  assert.deepEqual(rowsOf(bags.getInventory()), [['apple', 1]]);
  // Out of reach of the horse.
  host.updatePlayer({ id: 7, ready: true, position: { x: 40, y: 0, z: 0 } });
  assert.equal(host.stashes.take(owner, bags.id, apples, 1).ok, false, 'out of reach');
});

test('a player whose world became ready sees their horses saddlebags again', async () => {
  const { host, join, shows, output } = setup();
  await host.startAll();
  const api = host.makeAPI();
  const owner = join(7, { x: 0, y: 0, z: 0 });
  const horse = api.Horse.spawn({ x: 2, y: 0, z: 0 });
  horse.giveTo(owner);
  output.length = 0;
  host.stashes.arrived(owner);
  assert.deepEqual(
    shows().map((e) => [e.id, e.horse]),
    [[7, horse.id]],
  );
});
