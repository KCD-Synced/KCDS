'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { classBytesHex, lookupItemClass } = require('./inventory/catalog.cjs');

// An NPC's own inventory and the search of its body: the container a dead, lootable
// NPC leaves, what a looter takes from it and when it goes.

async function startedHost() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-npc-loot-'));
  const output = [];
  const host = new ResourceHost(root, {
    entityIds: { first: 4097, count: 2048 },
    send: (message) => output.push(message),
    log: () => {},
  });
  await host.startAll();
  const sent = (op) => output.filter((message) => message.op === op);
  return { host, Npc: host.npcs.Npc, sent };
}

function join(host, id, position) {
  host.updatePlayer({ id, ready: true, position, rotation: { w: 1, x: 0, y: 0, z: 0 } });
  return host.players.get(id);
}

function stock(host, player, items) {
  host.playerInventories.operate(player, (inventory) => inventory.set({ items }));
}

function rowsOf(inventory) {
  return inventory.snapshot().items.map((row) => [row.name, row.amount]);
}

function playerRows(player) {
  return player.getInventory().items.map((row) => [row.name, row.amount]);
}

// An NPC at the origin that carries 300 arrows and a sword, and a player close to it.
function setup({ host, Npc }, options = {}) {
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 }, ...options });
  npc.getInventory().set({
    items: [
      { id: 'arrows', item: 'arrow_crude', amount: 300 },
      { id: 'blade', item: 'longSwordDuel', amount: 1 },
    ],
  });
  const looter = join(host, 8, { x: 1, y: 0, z: 0 });
  stock(host, looter, [{ id: 'apple', item: 'apple', amount: 1 }]);
  return { npc, looter };
}

function containers(host) {
  return host.makeAPI().Stash.all();
}

test('getInventory is an Inventory that lives as long as the NPC and holds what is put in it', async () => {
  const started = await startedHost();
  const { npc } = setup(started);
  const inventory = npc.getInventory();
  assert.equal(npc.getInventory(), inventory);
  assert.deepEqual(rowsOf(inventory), [
    ['arrow_crude', 300],
    ['longSwordDuel', 1],
  ]);
  assert.equal(inventory.remove({ units: [{ id: 'arrows', amount: 100 }] }).result.ok, true);
  assert.deepEqual(rowsOf(npc.getInventory()), [
    ['arrow_crude', 200],
    ['longSwordDuel', 1],
  ]);
  npc.kill();
  npc.revive();
  assert.equal(npc.getInventory(), inventory, 'a revived NPC keeps what it carries');
});

test('lootable is true by default, comes from the create option and takes only booleans', async () => {
  const { Npc } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  assert.equal(npc.lootable, true);
  npc.lootable = false;
  assert.equal(npc.lootable, false);
  assert.equal(Npc.create({ position: { x: 0, y: 0, z: 0 }, lootable: false }).lootable, false);
  assert.equal(Npc.create({ position: { x: 0, y: 0, z: 0 }, lootable: true }).lootable, true);
  for (const bad of [1, 0, 'true', null, undefined, {}]) {
    assert.throws(() => {
      npc.lootable = bad;
    }, TypeError);
  }
  assert.throws(() => Npc.create({ position: { x: 0, y: 0, z: 0 }, lootable: 'no' }), TypeError);
  assert.equal(npc.lootable, false, 'a refused value changes nothing');
});

test('a dead lootable NPC is a container at its body; a looter takes through the server', async () => {
  const started = await startedHost();
  const { host } = started;
  const { npc, looter } = setup(started);
  const events = [];
  host.on('npcLoot', (who, victim, line) => {
    events.push(['loot', who.id, victim.id, line.item, line.amount]);
    return line.amount < 100;
  });
  host.on('npcLooted', (who, victim, items) => {
    events.push(['looted', who.id, victim.id, items]);
  });
  assert.equal(containers(host).length, 0, 'a living NPC is no container');
  npc.kill();
  const [body] = containers(host);
  assert.equal(body.itemCount, 2, 'the body shows the NPC inventory');
  assert.deepEqual(
    [body.position.x, body.position.y, body.position.z, body.dimension],
    [0, 0, 0, npc.dimension],
  );
  assert.equal(host.stashes.open(looter, body.id), true);
  assert.equal(host.stashes.take(looter, body.id, 'blade', 1).ok, true);
  assert.equal(host.stashes.take(looter, body.id, 'arrows', 150).ok, false, 'a handler refused');
  assert.equal(host.stashes.put(looter, body.id, 'apple', 1).ok, false, 'nothing goes into a body');
  assert.equal(body.addItem({ item: 'apple', amount: 1 }).ok, false);
  body.destroy();
  assert.equal(containers(host).length, 1, 'a script cannot destroy a body');
  assert.deepEqual(rowsOf(npc.getInventory()), [['arrow_crude', 300]]);
  assert.deepEqual(playerRows(looter), [
    ['apple', 1],
    ['longSwordDuel', 1],
  ]);
  const blade = lookupItemClass('longSwordDuel').id;
  const arrow = lookupItemClass('arrow_crude').id;
  assert.deepEqual(events, [
    ['loot', 8, npc.id, blade, 1],
    ['looted', 8, npc.id, [{ item: blade, amount: 1 }]],
    ['loot', 8, npc.id, arrow, 150],
  ]);
  const shown = started.sent('stashShow').pop();
  assert.deepEqual(
    shown.rows.map((row) => [row[0], row[2]]),
    [['arrows', 300]],
  );
  host.playerInventories.flush();
});

test('a body out of reach cannot be searched, and an unknown row gives nothing', async () => {
  const started = await startedHost();
  const { host } = started;
  const { npc, looter } = setup(started);
  const far = join(host, 9, { x: 50, y: 0, z: 0 });
  npc.kill();
  const [body] = containers(host);
  assert.equal(host.stashes.open(far, body.id), false);
  assert.equal(host.stashes.open(looter, body.id), true);
  assert.equal(host.stashes.take(looter, body.id, 'nothing', 1).ok, false);
  assert.equal(host.stashes.open(far, body.id), false, 'one holder at a time');
  assert.deepEqual(rowsOf(npc.getInventory()), [
    ['arrow_crude', 300],
    ['longSwordDuel', 1],
  ]);
  host.playerInventories.flush();
});

test('reviving the NPC takes its body away', async () => {
  const started = await startedHost();
  const { host } = started;
  const { npc, looter } = setup(started);
  npc.kill();
  const [body] = containers(host);
  assert.equal(host.stashes.open(looter, body.id), true);
  assert.equal(npc.revive(), true);
  assert.equal(containers(host).length, 0);
  assert.equal(host.stashes.take(looter, body.id, 'blade', 1).ok, false);
  assert.equal(started.sent('stashDestroy').length, 1);
  npc.kill();
  assert.equal(containers(host).length, 1, 'a second death leaves a new body');
  host.playerInventories.flush();
});

test('an NPC that is not lootable leaves no body; setting it while dead changes the body', async () => {
  const started = await startedHost();
  const { host } = started;
  const { npc } = setup(started, { lootable: false });
  npc.kill();
  assert.equal(containers(host).length, 0);
  npc.lootable = true;
  assert.equal(containers(host).length, 1, 'made lootable while dead');
  npc.lootable = true;
  assert.equal(containers(host).length, 1);
  npc.lootable = false;
  assert.equal(containers(host).length, 0, 'made unlootable while dead');
  npc.lootable = true;
  const [body] = containers(host);
  npc.teleport({ x: 20, y: 0, z: 0 });
  assert.equal(containers(host).length, 1);
  assert.equal(host.stashes.stashes.has(body.id), false, 'moved: made anew where it lies');
  assert.equal(containers(host)[0].position.x, 20);
});

test('removing the NPC takes its body away', async () => {
  const started = await startedHost();
  const { host } = started;
  const { npc, looter } = setup(started);
  npc.kill();
  const [body] = containers(host);
  assert.equal(host.stashes.open(looter, body.id), true);
  npc.remove();
  assert.equal(containers(host).length, 0);
  assert.equal(host.stashes.take(looter, body.id, 'blade', 1).ok, false);
  host.playerInventories.flush();
});

// An NPC at the origin that a resource dressed in a sword, a bow and two arrows (one worn row of
// two), and a player close to it.
function dressed(started) {
  const npc = started.Npc.create({ position: { x: 0, y: 0, z: 0 } });
  assert.equal(npc.setOutfit(['longSwordDuel', 'bow_a', 'arrow_crude', 'arrow_crude']), true);
  const looter = join(started.host, 8, { x: 1, y: 0, z: 0 });
  return { npc, looter };
}

// The worn rows of an inventory: [name, amount, equipped], in name order.
function wornRows(inventory) {
  return inventory
    .snapshot()
    .items.filter((row) => row.equipped > 0)
    .map((row) => [row.name, row.amount, row.equipped])
    .sort((a, b) => a[0].localeCompare(b[0]));
}

function rowIdOf(inventory, name) {
  return inventory.snapshot().items.find((row) => row.name === name).id;
}

// The class bytes npcOutfit sends for these item names.
function outfitBytes(...names) {
  return names.map((name) => classBytesHex(lookupItemClass(name)));
}

function outfitItems(npc) {
  return npc.outfit.map((piece) => piece.item);
}

test('setOutfit puts its pieces in the NPC inventory as worn rows; an undressed NPC has none', async () => {
  const started = await startedHost();
  const { npc } = dressed(started);
  assert.deepEqual(wornRows(npc.getInventory()), [
    ['arrow_crude', 2, 2],
    ['bow_a', 1, 1],
    ['longSwordDuel', 1, 1],
  ]);
  assert.equal(npc.outfit.length, 4);
  const bare = started.Npc.create({ position: { x: 0, y: 0, z: 0 } });
  assert.equal(bare.outfit, null);
  assert.deepEqual(rowsOf(bare.getInventory()), [], 'the clothes of its soul are no rows');
  npc.kill();
  assert.equal(containers(started.host)[0].itemCount, 3, 'the body shows the worn rows');
});

test('looting a worn piece takes it off every body of the NPC', async () => {
  const started = await startedHost();
  const { host, sent } = started;
  const { npc, looter } = dressed(started);
  npc.kill();
  const [body] = containers(host);
  const inventory = npc.getInventory();
  assert.equal(host.stashes.open(looter, body.id), true);
  assert.equal(sent('npcOutfit').length, 1);
  assert.equal(host.stashes.take(looter, body.id, rowIdOf(inventory, 'longSwordDuel'), 1).ok, true);
  assert.equal(sent('npcOutfit').length, 2);
  assert.deepEqual(
    sent('npcOutfit')[1].pieces.map((piece) => piece.itemClass),
    outfitBytes('bow_a', 'arrow_crude', 'arrow_crude'),
  );
  assert.equal(sent('npcOutfit')[1].id, npc.id);
  assert.deepEqual(outfitItems(npc), [
    lookupItemClass('bow_a').id,
    lookupItemClass('arrow_crude').id,
    lookupItemClass('arrow_crude').id,
  ]);
  assert.equal(host.stashes.take(looter, body.id, rowIdOf(inventory, 'arrow_crude'), 1).ok, true);
  assert.deepEqual(
    sent('npcOutfit')[2].pieces.map((piece) => piece.itemClass),
    outfitBytes('bow_a', 'arrow_crude'),
  );
  assert.deepEqual(wornRows(inventory), [
    ['arrow_crude', 1, 1],
    ['bow_a', 1, 1],
  ]);
  assert.deepEqual(playerRows(looter).sort(), [
    ['arrow_crude', 1],
    ['longSwordDuel', 1],
  ]);
  host.playerInventories.flush();
});

test('a second setOutfit replaces the earlier worn rows and keeps every other row', async () => {
  const started = await startedHost();
  const { sent } = started;
  const { npc } = dressed(started);
  const inventory = npc.getInventory();
  assert.equal(inventory.add({ item: 'apple', amount: 3 }).result.ok, true);
  assert.equal(npc.setOutfit(['bow_a', 'ArmPlate01_m01_C2']), true);
  assert.deepEqual(wornRows(inventory), [
    ['ArmPlate01_m01_C2', 1, 1],
    ['bow_a', 1, 1],
  ]);
  assert.deepEqual(rowsOf(inventory).sort(), [
    ['ArmPlate01_m01_C2', 1],
    ['apple', 3],
    ['bow_a', 1],
  ]);
  assert.equal(sent('npcOutfit').length, 2);
  assert.deepEqual(
    sent('npcOutfit')[1].pieces.map((piece) => piece.itemClass),
    outfitBytes('bow_a', 'ArmPlate01_m01_C2'),
  );
  assert.equal(npc.setOutfit([]), true);
  assert.deepEqual(rowsOf(inventory), [['apple', 3]]);
  assert.deepEqual(npc.outfit, []);
});

test('a script that removes a worn row or replaces the inventory re-sends the outfit', async () => {
  const started = await startedHost();
  const { sent } = started;
  const { npc } = dressed(started);
  const inventory = npc.getInventory();
  assert.equal(inventory.add({ item: 'apple', amount: 3 }).result.ok, true);
  const apple = rowIdOf(inventory, 'apple');
  assert.equal(inventory.remove({ units: [{ id: apple, amount: 1 }] }).result.ok, true);
  assert.equal(sent('npcOutfit').length, 1, 'a row that is no outfit changes nothing');
  const sword = rowIdOf(inventory, 'longSwordDuel');
  assert.equal(inventory.remove({ units: [{ id: sword, amount: 1 }] }).result.ok, true);
  assert.equal(sent('npcOutfit').length, 2);
  assert.deepEqual(
    sent('npcOutfit')[1].pieces.map((piece) => piece.itemClass),
    outfitBytes('bow_a', 'arrow_crude', 'arrow_crude'),
  );
  assert.equal(npc.outfit.length, 3);
  // A set that gives back only the apples leaves nothing of the outfit.
  assert.equal(inventory.set({ items: [{ id: apple, item: 'apple', amount: 2 }] }).result.ok, true);
  assert.equal(sent('npcOutfit').length, 3);
  assert.deepEqual(sent('npcOutfit')[2].pieces, []);
  assert.deepEqual(npc.outfit, []);
  // A removed NPC sends nothing more.
  const other = started.Npc.create({ position: { x: 0, y: 0, z: 0 } });
  assert.equal(other.setOutfit(['bow_a']), true);
  assert.equal(sent('npcOutfit').length, 4);
  other.remove();
  assert.equal(other.getInventory().set({ items: [] }).result.ok, true);
  assert.equal(sent('npcOutfit').length, 4);
});
