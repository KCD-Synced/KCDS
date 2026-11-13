'use strict';

// Stash and searching a knocked-out player's body.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');

function hostWith(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-stash-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'chests');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'chests', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  const chat = () => output.filter((e) => e.op === 'chat').map((e) => e.text);
  return { host, chat, output };
}

function join(host, id, position) {
  host.updatePlayer({ id, ready: true, position, rotation: { w: 1, x: 0, y: 0, z: 0 } });
  return host.players.get(id);
}

function stock(host, player, items) {
  host.playerInventories.operate(player, (inventory) => inventory.set({ items }));
}

function rowsOf(player) {
  return player.getInventory().items.map((row) => [row.name, row.amount]);
}

test('Stash.spawn stands a container every client builds; scripts fill and destroy it', async () => {
  const { host, chat, output } = hostWith(`
    kcds.core.Events.on('stashSpawn', (s) => kcds.core.Chat.sendToAll('spawn ' + s.stashId));
    kcds.core.Events.on('stashInventoryChanged', (s, p, change) =>
      kcds.core.Chat.sendToAll('changed ' + s.stashId + ' ' + (p ? p.id : 'script') + ' ' + change.reason));
    kcds.core.Events.on('stashDestroy', (s) => kcds.core.Chat.sendToAll('destroy ' + s.stashId));
    const chest = kcds.world.Stash.spawn({ x: 1, y: 2, z: 3 }, {}, 5);
    chest.addItem({ item: 'apple', amount: 1 });
    chest.addItem({ item: 'arrow_crude', amount: 12 });
  `);
  await host.startAll();
  const api = host.makeAPI();
  const [chest] = api.Stash.all();
  const spawn = output.find((e) => e.op === 'stashSpawn');
  assert.deepEqual(
    { ...spawn, position: { ...spawn.position } },
    {
      op: 'stashSpawn',
      id: chest.id,
      position: { x: 1, y: 2, z: 3 },
      rotation: spawn.rotation,
      dimension: 5,
      bodyOf: 0,
      placedGuid: '',
      model: '',
    },
  );
  assert.equal(chest.stashId, 1);
  assert.equal(chest.name, 'spawned_stash_1');
  assert.equal(chest.itemCount, 2);
  assert.equal(chest.isOpen, false);
  assert.equal(chest.isStorage, false);
  assert.equal(chest.guid, '0000000000000001');
  assert.equal(chest.body, null);
  assert.equal(new api.Entity(chest.id), chest);
  assert.equal(api.Stash.getById(chest.id), chest);
  assert.equal(api.Stash.nearest({ x: 1, y: 2, z: 4 }, 5, 5), chest);
  assert.equal(api.Stash.nearest({ x: 1, y: 2, z: 4 }, 5), null, 'another world');
  assert.deepEqual(
    chest.getInventory().items.map((row) => [row.name, row.amount]),
    [
      ['apple', 1],
      ['arrow_crude', 12],
    ],
  );
  assert.equal(
    chest.removeItem({ units: [{ id: chest.getInventory().items[0].id, amount: 1 }] }).ok,
    true,
  );
  assert.equal(chest.open(), false, 'no player');
  assert.match(String(chest), /^Stash\(1, 1 rows\)$/);
  assert.equal(api.Stash.destroyAll(5), 1);
  assert.equal(chest.getInventory().items.length, 1, 'a destroyed handle still reads');
  assert.equal(api.Stash.getById(chest.id), null);
  assert.deepEqual(chat(), [
    'spawn 1',
    'changed 1 script add',
    'changed 1 script add',
    'changed 1 script remove',
    'destroy 1',
  ]);
  assert.equal(output.filter((e) => e.op === 'stashDestroy').length, 1);
});

test('a chest tells every client when its stock runs empty or fills', async () => {
  const { host, output } = hostWith(`
    const chest = kcds.world.Stash.spawn({ x: 0, y: 0, z: 0 });
    chest.addItem({ item: 'arrow_crude', amount: 2 });
    chest.addItem({ item: 'arrow_crude', amount: 1 });
    const [row] = chest.getInventory().items;
    chest.removeItem({ units: [{ id: row.id, amount: row.amount }] });
  `);
  await host.startAll();
  const words = output.filter((e) => e.op === 'stashEmpty').map((e) => e.empty);
  assert.deepEqual(words, [false, true]);
});

test('a player at a chest another has open is told, in their language, not every press', async () => {
  const { host, output } = hostWith(`
    kcds.core.Locales.set('czech', { 'stash.inUse': 'Obsazeno' });
    kcds.world.Stash.spawn({ x: 0, y: 0, z: 0 });
  `);
  await host.startAll();
  const api = host.makeAPI();
  const [chest] = api.Stash.all();
  const holder = join(host, 7, { x: 1, y: 0, z: 0 });
  const other = join(host, 8, { x: -1, y: 0, z: 0 });
  host.updatePlayer({ id: 9, ready: true, language: 'czech', position: { x: 0, y: 1, z: 0 } });
  const czech = host.players.get(9);
  const notices = () => output.filter((e) => e.op === 'itemLimitNotice');
  assert.equal(host.stashes.open(holder, chest.id), true);
  output.length = 0;

  assert.equal(host.stashes.open(other, chest.id), false);
  assert.equal(host.stashes.open(other, chest.id), false);
  assert.deepEqual(notices(), [
    { op: 'itemLimitNotice', id: 8, text: 'Someone else is using this', center: true },
  ]);
  assert.equal(
    output.filter((e) => e.op === 'stashShow' && e.id === 8 && e.closed).length,
    2,
    'each press is answered',
  );
  output.length = 0;

  assert.equal(host.stashes.open(czech, chest.id), false);
  assert.deepEqual(notices(), [{ op: 'itemLimitNotice', id: 9, text: 'Obsazeno', center: true }]);
  output.length = 0;

  // A script's open returns false and tells nobody.
  assert.equal(chest.open(other), false);
  assert.equal(notices().length, 0);
});

test('a player in reach opens a container, takes and puts units, and closes it', async () => {
  const { host, chat, output } = hostWith(`
    kcds.core.Events.on('stashOpen', (s, p) => kcds.core.Chat.sendToAll('open ' + p.id));
    kcds.core.Events.on('stashClose', (s, p, reason) => kcds.core.Chat.sendToAll('close ' + p.id + ' ' + reason));
    kcds.core.Events.on('stashInventoryChanged', (s, p, change) =>
      kcds.core.Chat.sendToAll('changed ' + (p ? p.id : 'script') + ' ' + change.reason));
    const chest = kcds.world.Stash.spawn({ x: 0, y: 0, z: 0 });
    chest.addItem({ item: 'arrow_crude', amount: 20 });
  `);
  await host.startAll();
  const [chest] = host.makeAPI().Stash.all();
  const near = join(host, 7, { x: 2, y: 0, z: 0 });
  const other = join(host, 8, { x: 1, y: 0, z: 0 });
  const far = join(host, 9, { x: 40, y: 0, z: 0 });
  stock(host, near, [{ id: 'blade', item: 'longSwordDuel', amount: 1 }]);
  output.length = 0;

  assert.equal(host.stashes.open(far, chest.id), false, 'out of reach');
  assert.equal(host.stashes.open(near, chest.id), true);
  assert.equal(chest.heldById, 7);
  assert.equal(host.stashes.open(other, chest.id), false, 'someone holds it');
  const shows = output.filter((e) => e.op === 'stashShow');
  assert.deepEqual(
    shows.map((e) => [e.id, e.open, e.closed, e.rows.length]),
    [
      [9, false, true, 0],
      [7, true, false, 1],
      [8, false, true, 0],
    ],
  );
  const arrows = chest.getInventory().items[0].id;
  assert.equal(host.stashes.take(other, chest.id, arrows, 5).ok, false, 'not the holder');
  assert.equal(host.stashes.take(near, chest.id, arrows, 5).ok, true);
  assert.equal(host.stashes.take(near, chest.id, arrows, 50).ok, false, 'more than it holds');
  assert.equal(host.stashes.put(near, chest.id, 'blade', 1).ok, true);
  assert.deepEqual(rowsOf(near), [['arrow_crude', 5]]);
  assert.deepEqual(
    chest.getInventory().items.map((row) => [row.name, row.amount]),
    [
      ['arrow_crude', 15],
      ['longSwordDuel', 1],
    ],
  );
  host.playerInventories.flush();
  const reasons = output
    .filter((e) => e.op === 'inventoryShow' && e.id === 7)
    .map((e) => e.rows.map((row) => row[2]));
  assert.ok(reasons.length >= 2, 'the holder sees their own stock after each move');
  // Walking away loses it.
  host.updatePlayer({ id: 7, ready: true, position: { x: 30, y: 0, z: 0 } });
  assert.equal(host.stashes.take(near, chest.id, arrows, 1).ok, false);
  assert.equal(chest.heldById, 0);
  assert.equal(host.stashes.open(other, chest.id), true);
  assert.equal(host.stashes.close(other, chest.id), true);
  assert.deepEqual(chat(), [
    'open 7',
    'changed 7 withdraw',
    'changed 7 deposit',
    'close 7 lostAccess',
    'open 8',
    'close 8 closed',
  ]);
});

test('stashInteract can refuse an open', async () => {
  const { host } = hostWith(`
    kcds.core.Events.on('stashInteract', (player, stash, action) => action !== 'open' || player.id !== 7);
    kcds.world.Stash.spawn({ x: 0, y: 0, z: 0 });
  `);
  await host.startAll();
  const [chest] = host.makeAPI().Stash.all();
  assert.equal(host.stashes.open(join(host, 7, { x: 1, y: 0, z: 0 }), chest.id), false);
  assert.equal(host.stashes.open(join(host, 8, { x: 1, y: 0, z: 0 }), chest.id), true);
});

test('Player.dropAll moves the rows into a container at the body', async () => {
  const { host, chat } = hostWith(`
    kcds.core.Events.on('stashInventoryChanged', (s, p, change) =>
      kcds.core.Chat.sendToAll('changed ' + p.id + ' ' + change.reason + ' ' + change.items.length));
  `);
  await host.startAll();
  const p = join(host, 7, { x: 5, y: 6, z: 7 });
  stock(host, p, [
    { id: 'arrows', item: 'arrow_crude', amount: 30 },
    { id: 'blade', item: 'longSwordDuel', amount: 1 },
  ]);
  host.playerInventories.operate(p, (inventory) => inventory.wearing('blade', 1));
  const kept = p.dropAll({ keepEquipped: true });
  assert.deepEqual(rowsOf(p), [['longSwordDuel', 1]], 'what the body holds stays');
  assert.deepEqual(
    kept.getInventory().items.map((row) => [row.name, row.amount]),
    [['arrow_crude', 30]],
  );
  assert.equal(kept.position.x, 5);
  const all = p.dropAll();
  assert.deepEqual(rowsOf(p), []);
  assert.equal(all.itemCount, 1);
  assert.equal(p.dropAll(), null, 'nothing left to drop');
  assert.deepEqual(chat(), ['changed 7 drop 1', 'changed 7 drop 1']);
});

test('a knocked-out player is searched through the server; reviving them ends it', async () => {
  const { host, chat, output } = hostWith(`
    kcds.core.Events.on('playerLoot', (looter, victim, line) => line.amount < 100);
    kcds.core.Events.on('playerLooted', (looter, victim, items) =>
      kcds.core.Chat.sendToAll('looted ' + looter.id + ' ' + victim.id + ' ' + items[0].amount));
    kcds.core.Events.on('stashClose', (s, p, reason) => kcds.core.Chat.sendToAll('close ' + p.id + ' ' + reason));
  `);
  await host.startAll();
  const victim = join(host, 7, { x: 0, y: 0, z: 0 });
  const looter = join(host, 8, { x: 1, y: 0, z: 0 });
  stock(host, victim, [
    { id: 'arrows', item: 'arrow_crude', amount: 300 },
    { id: 'blade', item: 'longSwordDuel', amount: 1 },
  ]);
  stock(host, looter, [{ id: 'apple', item: 'apple', amount: 1 }]);
  assert.equal(victim.knockedOut, false);
  assert.equal(victim.lootable, true);
  host.playerDied(7);
  assert.equal(victim.knockedOut, true);
  const [body] = host.makeAPI().Stash.all();
  assert.equal(body.body, victim);
  assert.equal(output.find((e) => e.op === 'stashSpawn').bodyOf, 7);
  assert.equal(body.itemCount, 2, 'a body shows its player inventory');
  assert.equal(host.stashes.open(victim, body.id), false, 'not by its own player');
  assert.equal(host.stashes.open(looter, body.id), true);
  assert.equal(host.stashes.take(looter, body.id, 'blade', 1).ok, true);
  assert.equal(host.stashes.take(looter, body.id, 'arrows', 150).ok, false, 'a handler refused');
  assert.equal(host.stashes.put(looter, body.id, 'apple', 1).ok, false, 'nothing goes into a body');
  assert.deepEqual(rowsOf(victim), [['arrow_crude', 300]]);
  assert.deepEqual(rowsOf(looter), [
    ['apple', 1],
    ['longSwordDuel', 1],
  ]);
  assert.equal(body.addItem({ item: 'apple', amount: 1 }).ok, false);
  victim.revive();
  assert.equal(host.makeAPI().Stash.all().length, 0, 'the body is no container once up');
  assert.deepEqual(chat(), ['looted 8 7 1', 'close 8 destroyed']);
  const last = output.filter((e) => e.op === 'stashShow').pop();
  assert.deepEqual([last.id, last.closed], [8, true]);
  // An unlootable player's body offers nothing.
  victim.lootable = false;
  host.playerDied(7);
  assert.equal(host.makeAPI().Stash.all().length, 0);
  victim.lootable = true;
  assert.equal(host.makeAPI().Stash.all().length, 1, 'made lootable while down');
  assert.throws(() => {
    victim.lootable = 1;
  }, /boolean/);
  host.playerInventories.flush();
});

test("a body does not show or give away its player's keyring", async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  const victim = join(host, 7, { x: 0, y: 0, z: 0 });
  const looter = join(host, 8, { x: 1, y: 0, z: 0 });
  stock(host, victim, [
    { id: 'keys', item: 'keyring', amount: 1 },
    { id: 'blade', item: 'longSwordDuel', amount: 1 },
  ]);
  host.playerDied(7);
  const [body] = host.makeAPI().Stash.all();
  assert.equal(body.itemCount, 1);
  assert.equal(host.stashes.open(looter, body.id), true);
  const shown = output.filter((e) => e.op === 'stashShow').pop();
  assert.deepEqual(
    shown.rows.map((row) => row[0]),
    ['blade'],
  );
  assert.equal(host.stashes.take(looter, body.id, 'keys', 1).ok, false);
  assert.deepEqual(rowsOf(victim), [
    ['keyring', 1],
    ['longSwordDuel', 1],
  ]);
  host.playerInventories.flush();
});

test('the body is the container: searched where it lies after a carry, empty or not', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  const victim = join(host, 7, { x: 0, y: 0, z: 0 });
  const looter = join(host, 8, { x: 1, y: 0, z: 0 });
  stock(host, victim, [{ id: 'keys', item: 'keyring', amount: 1 }]);
  host.playerDied(7);
  const [body] = host.makeAPI().Stash.all();
  assert.equal(body.itemCount, 0, 'nothing shown but the body is still a container');
  // Carried 20 m away and searched there.
  host.updatePlayer({ id: 7, ready: true, position: { x: 20, y: 0, z: 0 } });
  host.updatePlayer({ id: 8, ready: true, position: { x: 21, y: 0, z: 0 } });
  assert.equal(body.position.x, 20);
  assert.equal(host.stashes.open(looter, body.id), true);
  const shown = output.filter((e) => e.op === 'stashShow').pop();
  assert.deepEqual([shown.open, shown.closed, shown.rows], [true, false, []]);
  host.playerInventories.flush();
});

test('a virtual stash has no chest, opens from anywhere in its world and keeps its stock', async () => {
  const { host, chat, output } = hostWith(`
    kcds.core.Events.on('stashOpen', (s, p) => kcds.core.Chat.sendToAll('open ' + s.isStorage + ' ' + p.id));
    const bank = kcds.world.Stash.createStorage(3);
    bank.addItem({ item: 'arrow_crude', amount: 4 });
    kcds.core.Chat.sendToAll('guid [' + bank.guid + '] near ' + (kcds.world.Stash.nearest({ x: 0, y: 0, z: 0 }, 50, 3) === null));
  `);
  await host.startAll();
  const [bank] = host.makeAPI().Stash.all();
  assert.equal(bank.isStorage, true);
  assert.equal(
    output.some((e) => e.op === 'stashSpawn'),
    false,
    'no client builds a chest',
  );
  const far = join(host, 7, { x: 900, y: 0, z: 0 });
  assert.equal(bank.open(far), false, 'wrong world');
  host.updatePlayer({ id: 7, ready: true, position: { x: 900, y: 0, z: 0 }, dimension: 3 });
  assert.equal(bank.open(far), true);
  assert.equal(bank.heldById, 7);
  const arrows = bank.getInventory().items[0].id;
  assert.equal(host.stashes.take(far, bank.id, arrows, 1).ok, true, 'no distance limit');
  assert.deepEqual(chat(), ['guid [] near true', 'open true 7']);
});

test('a locked scripted chest refuses opens and tells the clients; the key class is checked', async () => {
  const { host, output } = hostWith(`kcds.world.Stash.spawn({ x: 0, y: 0, z: 0 });`);
  await host.startAll();
  const [chest] = host.makeAPI().Stash.all();
  const near = join(host, 7, { x: 1, y: 0, z: 0 });
  assert.match(chest.guid, /^[0-9a-f]{16}$/);
  assert.equal(host.makeAPI().Stash.find(chest.guid), chest);
  output.length = 0;
  chest.locked = true;
  chest.locked = true;
  assert.equal(output.filter((e) => e.op === 'stashLock').length, 1, 'change-only');
  assert.equal(chest.serverLocked, true);
  assert.equal(host.stashes.open(near, chest.id), false);
  chest.keyClass = 'not-a-class';
  assert.equal(chest.keyClass, '');
  chest.locked = false;
  assert.equal(chest.serverLocked, false);
  assert.equal(host.stashes.open(near, chest.id), true);
});

test('a lock picked in a player game unlocks the chest for everyone and lets the open through', async () => {
  const { host, chat, output } = hostWith(`
    kcds.core.Events.on('stashUnlocked', (s, p, method) => kcds.core.Chat.sendToAll('unlocked ' + p.id + ' ' + method));
    const chest = kcds.world.Stash.spawn({ x: 0, y: 0, z: 0 });
    chest.locked = true;
  `);
  await host.startAll();
  const [chest] = host.makeAPI().Stash.all();
  const near = join(host, 7, { x: 1, y: 0, z: 0 });
  const far = join(host, 8, { x: 40, y: 0, z: 0 });
  assert.equal(host.stashes.unlock(far, chest.id, 'pick'), false, 'out of reach');
  assert.equal(host.stashes.unlock(near, chest.id, 'shove'), false, 'unknown method');
  chest.canBePicked = false;
  assert.equal(host.stashes.unlock(near, chest.id, 'pick'), false, 'not canBePicked');
  chest.canBePicked = true;
  assert.equal(chest.locked, true);
  output.length = 0;
  assert.equal(host.stashes.unlock(near, chest.id, 'pick'), true);
  assert.equal(chest.locked, false);
  assert.equal(chest.serverLocked, false);
  assert.deepEqual(
    output.filter((e) => e.op === 'stashLock').map((e) => [e.id, e.locked]),
    [[chest.id, false]],
    'every client is told once',
  );
  assert.equal(host.stashes.unlock(near, chest.id, 'pick'), false, 'already unlocked');
  assert.equal(host.stashes.open(near, chest.id), true);
  assert.deepEqual(chat(), ['unlocked 7 pick']);
});

test('opening a chest with its key needs the key in the player inventory', async () => {
  const { host, chat } = hostWith(`
    kcds.core.Events.on('stashUnlocked', (s, p, method) => kcds.core.Chat.sendToAll('unlocked ' + p.id + ' ' + method));
    const chest = kcds.world.Stash.spawn({ x: 0, y: 0, z: 0 });
    chest.locked = true;
  `);
  await host.startAll();
  const [chest] = host.makeAPI().Stash.all();
  const near = join(host, 7, { x: 1, y: 0, z: 0 });
  stock(host, near, [{ item: 'key_home', amount: 1 }]);
  assert.equal(host.stashes.unlock(near, chest.id, 'key'), false, 'the lock has no key');
  chest.keyClass = 'bd20bdb4-4c50-4610-b023-30ceac635e7a';
  assert.equal(host.stashes.unlock(near, chest.id, 'key'), false, 'another key');
  stock(host, near, [{ item: 'key_shop', amount: 1 }]);
  assert.equal(host.stashes.open(near, chest.id), false, 'still locked');
  assert.equal(host.stashes.unlock(near, chest.id, 'key'), true);
  assert.equal(host.stashes.open(near, chest.id), true);
  assert.deepEqual(chat(), ['unlocked 7 key']);
});

test('two players at one chest never take the same units: nothing is made twice', async () => {
  const { host } = hostWith(`
    const chest = kcds.world.Stash.spawn({ x: 0, y: 0, z: 0 });
    chest.addItem({ item: 'apple', amount: 5 });
  `);
  await host.startAll();
  const [chest] = host.makeAPI().Stash.all();
  const a = join(host, 7, { x: 1, y: 0, z: 0 });
  const b = join(host, 8, { x: -1, y: 0, z: 0 });
  const apples = (player) =>
    player
      .getInventory()
      .items.filter((row) => row.name === 'apple')
      .reduce((sum, row) => sum + row.amount, 0);
  const inChest = () =>
    chest
      .getInventory()
      .items.filter((row) => row.name === 'apple')
      .reduce((sum, row) => sum + row.amount, 0);
  const total = () => apples(a) + apples(b) + inChest();
  const row = chest.getInventory().items[0].id;

  assert.equal(host.stashes.open(a, chest.id), true);
  // B, at the same chest at the same time: neither the screen nor a take.
  assert.equal(host.stashes.open(b, chest.id), false);
  assert.equal(host.stashes.take(b, chest.id, row, 5).ok, false);
  assert.equal(host.stashes.take(a, chest.id, row, 3).ok, true);
  assert.equal(host.stashes.take(a, chest.id, row, 3).ok, false, 'two are left, not three');
  assert.equal(host.stashes.take(a, chest.id, row, 2).ok, true);
  assert.equal(host.stashes.take(a, chest.id, row, 1).ok, false, 'the row is gone');
  assert.equal(total(), 5);
  // The hold passes to B: A's late takes are refused, B finds the chest empty.
  assert.equal(host.stashes.close(a, chest.id), true);
  assert.equal(host.stashes.open(b, chest.id), true);
  assert.equal(host.stashes.take(a, chest.id, row, 1).ok, false, 'A no longer holds it');
  assert.equal(host.stashes.take(b, chest.id, row, 1).ok, false, 'nothing left');
  // A puts two back while B holds it: refused; B's put and take keep the count.
  const aRow = a.getInventory().items.find((r) => r.name === 'apple').id;
  assert.equal(host.stashes.put(a, chest.id, aRow, 2).ok, false, 'A does not hold it');
  assert.equal(apples(a), 5);
  assert.equal(inChest(), 0);
  assert.equal(total(), 5);
  host.playerInventories.flush();
});

test("the level's own chests are listed, and an adopted one is the server's with the level's lock", async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  const { Stash } = host.makeAPI();
  // The pharmacist's bedside chest at Troskovice, which the level locks.
  const near = Stash.levelChests({ x: 2294.6, y: 2028.9, z: 104.8 }, 2);
  const bedside = near.find((chest) => chest.placedGuid === '010c6d3de8ab0cb9');
  assert.ok(bedside);
  assert.equal(bedside.lockedAtBoot, true);
  assert.equal(bedside.canBePicked, true);
  assert.equal(bedside.lockLevel, null); // the game makes one up as the chest resets
  // The level entity's own name tells it apart from the chests beside it.
  assert.equal(bedside.sourceName, 'stash[Chest.chest5_ac54cc60-f763-4610-89fd-4bac6b50a9de]');
  assert.ok(Stash.levelChests().some((row) => row.lockLevel === 0.7));
  assert.match(bedside.model, /chest_rustic_c/);
  assert.ok(Stash.levelChests().length > 500);
  assert.throws(() => Stash.levelChests({ x: 0, y: 0, z: 0 }, 0), RangeError);

  output.length = 0;
  const chest = Stash.adopt('0x010C6D3DE8AB0CB9');
  assert.equal(chest.placedGuid, '010c6d3de8ab0cb9');
  assert.equal(chest.name, 'level_chest_010c6d3de8ab0cb9');
  assert.equal(chest.lockedAtBoot, true);
  assert.equal(chest.locked, true);
  assert.equal(chest.serverLocked, false);
  assert.equal(chest.itemCount, 0);
  const spawn = output.find((e) => e.op === 'stashSpawn');
  assert.equal(spawn.placedGuid, '010c6d3de8ab0cb9');
  assert.deepEqual({ ...spawn.position }, { x: 2294.659, y: 2028.87, z: 104.762 });
  assert.equal(output.find((e) => e.op === 'stashLock').locked, true);
  // The same chest again is the same Stash; Stash.find knows it by its level GUID.
  assert.equal(Stash.adopt('010c6d3de8ab0cb9'), chest);
  assert.equal(Stash.find('010c6d3de8ab0cb9'), chest);
  assert.equal(Stash.adopt('ffffffffffffffff'), null);
  assert.throws(() => Stash.adopt('chest'), RangeError);

  // A script's lock-picking difficulty for every player; null gives the game's back.
  assert.equal(chest.lockLevel, null);
  output.length = 0;
  chest.lockLevel = 0.85;
  assert.equal(chest.lockLevel, 0.85);
  assert.equal(output.find((e) => e.op === 'stashLock').lockLevel, 0.85);
  assert.throws(() => (chest.lockLevel = -0.5), RangeError);
  chest.lockLevel = null;
  assert.equal(output.at(-1).lockLevel, -1);
  // Which lock the minigame shows for every player; null gives the level's back.
  assert.equal(chest.lockStyle, null);
  output.length = 0;
  chest.lockStyle = 'fancy';
  chest.lockStyle = 'fancy';
  assert.equal(chest.lockStyle, 'fancy');
  assert.equal(output.filter((e) => e.op === 'stashLock').length, 1);
  assert.equal(output.at(-1).lockStyle, 'fancy');
  assert.throws(() => (chest.lockStyle = 'golden'), RangeError);
  chest.lockStyle = null;
  assert.equal(output.at(-1).lockStyle, '');
  // Locked: a player in reach cannot open it; unlocked by a script, they can.
  const player = join(host, 7, { x: 2295, y: 2029, z: 104.8 });
  assert.equal(host.stashes.open(player, chest.id), false);
  chest.locked = false;
  assert.equal(host.stashes.open(player, chest.id), true);
  chest.destroy();
  assert.equal(Stash.find('010c6d3de8ab0cb9'), null);
});

test("a translated level's chests come from the resources folder and are adopted the same way", async () => {
  const { host, output } = hostWith('');
  // A KCD1 shelf in Rataje, as Dev/tools/stash/build_kcd1_chests.py writes it.
  const shelf = [
    'kcd1_rataje',
    '49675f240aea18d6',
    2535.361,
    534.38,
    77.39,
    'objects/props/interiors/home_content/shelve_homeworks.cgf',
    false,
    true,
    null,
    'stash_4274cbfe-c62c-d4db-238d-30ca538c7fb0',
  ];
  // A row the catalog has already and a row without a GUID are left out.
  const known = ['trosecko', '010c6d3de8ab0cb9', 1, 2, 3, '', true, true, null, ''];
  const broken = ['kcd1_rataje', 'chest', 1, 2, 3, '', false, true, null, ''];
  fs.writeFileSync(
    path.join(host.root, 'level-chests.json'),
    JSON.stringify([shelf, known, broken]),
  );
  const lines = [];
  host.bridge.log = (_name, line) => lines.push(line);
  await host.startAll();
  assert.ok(lines.includes('level-chests.json: 1 chests added'));
  host.statusRoutes.settings.map = 'kcd1_rataje';
  const { Stash } = host.makeAPI();
  const near = Stash.levelChests({ x: 2535.3, y: 534.4, z: 77.4 }, 1);
  assert.deepEqual(
    near.map((chest) => chest.placedGuid),
    ['49675f240aea18d6'],
  );
  assert.equal(near[0].sourceName, shelf[9]);
  output.length = 0;
  const adopted = Stash.adopt('49675f240aea18d6');
  assert.equal(adopted.placedGuid, '49675f240aea18d6');
  assert.equal(output.find((e) => e.op === 'stashSpawn').placedGuid, '49675f240aea18d6');
  // A KCD2 level knows nothing of it.
  host.statusRoutes.settings.map = 'trosecko';
  assert.equal(Stash.levelChests({ x: 2535.3, y: 534.4, z: 77.4 }, 1).length, 0);
});

// Stash.model: the mesh Stash.spawn's options name, empty for the default chest and
// for a level's chest.
test('a stash is built as the model its script names, or the default chest', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  const { Stash } = host.makeAPI();
  const chest = Stash.spawn({ x: 0, y: 0, z: 0 });
  assert.equal(chest.model, '');
  const adopted = Stash.adopt('010c6d3de8ab0cb9');
  assert.equal(adopted.model, '');
  const cabinet = 'objects/manmade/common_furniture/cabinets/cabinet_fancy_d.cgf';
  const fancy = Stash.spawn({ x: 1, y: 0, z: 0 }, {}, undefined, { model: cabinet });
  assert.equal(fancy.model, cabinet);
  assert.equal(output.filter((e) => e.op === 'stashSpawn').at(-1).model, cabinet);
  const crate = Stash.spawn({ x: 2, y: 0, z: 0 }, {}, 0, { model: 'Objects/crate.CDF' });
  assert.equal(crate.model, 'Objects/crate.CDF');
  assert.equal(Stash.spawn({ x: 3, y: 0, z: 0 }, {}, 0, {}).model, '');
  // Any other path, or options that are not an object, throws before anything is made.
  const count = Stash.all().length;
  for (const model of [
    'objects/crate.dds',
    'crate',
    'C:/x.cgf',
    'a\\b.cgf',
    7,
    'x'.repeat(300) + '.cgf',
  ]) {
    assert.throws(() => Stash.spawn({ x: 0, y: 0, z: 0 }, {}, 0, { model }), TypeError);
  }
  assert.throws(() => Stash.spawn({ x: 0, y: 0, z: 0 }, {}, 0, 'cabinet'), TypeError);
  assert.equal(Stash.all().length, count);
});

test('a script opens a chest for a player within its reach', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const api = host.makeAPI();
  const chest = api.Stash.spawn({ x: 0, y: 0, z: 0 });
  const far = join(host, 7, { x: 50, y: 0, z: 0 });
  const near = join(host, 8, { x: 1, y: 0, z: 0 });
  assert.equal(chest.open(far), false, 'out of reach');
  assert.equal(chest.open(near), true);
  chest.locked = true;
  const other = join(host, 9, { x: 1, y: 1, z: 0 });
  assert.equal(chest.open(other), false, 'held or locked');
});

test("Stash.playerInventory opens a player's own inventory to another player, worn things too", async () => {
  const { host, chat, output } = hostWith(`
    globalThis.viewOf = (player, options) => kcds.world.Stash.playerInventory(player, options);
    kcds.core.Events.on('playerInventoryTake', (taker, owner, line) =>
      line.amount < 100);
    kcds.core.Events.on('playerInventoryTaken', (taker, owner, items) =>
      kcds.core.Chat.sendToAll('took ' + taker.id + ' ' + owner.id + ' ' + items[0].amount));
    kcds.core.Events.on('playerInventoryGiven', (giver, owner, items) =>
      kcds.core.Chat.sendToAll('gave ' + giver.id + ' ' + owner.id + ' ' + items[0].amount));
  `);
  await host.startAll();
  const vm = host.resources.get('chests').vm;
  // The owner is far away: a view opens from anywhere.
  const owner = join(host, 7, { x: 500, y: 500, z: 0 });
  const admin = join(host, 8, { x: 0, y: 0, z: 0 });
  stock(host, owner, [
    { id: 'arrows', item: 'arrow_crude', amount: 300 },
    { id: 'hose', item: 'HoseJoined01_m03_C', amount: 1, equipped: 1 },
  ]);
  stock(host, admin, [{ id: 'apple', item: 'apple', amount: 1 }]);
  assert.throws(() => vm.viewOf({ id: 99 }), TypeError);
  assert.throws(() => vm.viewOf(owner, { put: 'yes' }), TypeError);
  const view = vm.viewOf(owner, { put: true });
  assert.equal(view.inventoryOf, owner);
  assert.equal(view.isStorage, true);
  assert.equal(view.itemCount, 2);
  assert.equal(
    output.some((e) => e.op === 'stashSpawn'),
    false,
    'no chest stands for it',
  );
  assert.equal(view.open(owner), false, 'nobody opens a view of their own inventory');
  assert.equal(view.open(admin), true);
  // The worn hose comes off its owner; the handler keeps most of the arrows.
  assert.equal(host.stashes.take(admin, view.id, 'hose', 1).ok, true);
  assert.equal(host.stashes.take(admin, view.id, 'arrows', 150).ok, false, 'a handler refused');
  assert.equal(host.stashes.put(admin, view.id, 'apple', 1).ok, true, 'put: true gives');
  assert.deepEqual(rowsOf(owner), [
    ['arrow_crude', 300],
    ['apple', 1],
  ]);
  assert.deepEqual(rowsOf(admin), [['HoseJoined01_m03_C', 1]]);
  assert.equal(view.addItem({ item: 'apple', amount: 1 }).ok, false, 'scripts change the player');
  assert.deepEqual(chat(), ['took 8 7 1', 'gave 8 7 1']);
  // The owner's own changes show to the viewer at once.
  const shows = output.filter((e) => e.op === 'stashShow' && e.id === 8).length;
  stock(host, owner, [{ id: 'bread', item: 'bread', amount: 2 }]);
  host.playerInventories.flush();
  assert.ok(output.filter((e) => e.op === 'stashShow' && e.id === 8).length > shows);
  // Without put it takes nothing in; the owner leaving takes the view away.
  const plain = vm.viewOf(owner);
  host.stashes.close(admin, view.id);
  assert.equal(plain.open(admin), true);
  assert.equal(host.stashes.put(admin, plain.id, 'hose', 1).ok, false);
  host.disconnect(7);
  assert.equal(host.makeAPI().Stash.all().length, 0);
  host.playerInventories.flush();
});
