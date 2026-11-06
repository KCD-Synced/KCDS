'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { lookupItemClass } = require('./inventory/catalog.cjs');

// Harvesting a dead animal and using an NPC: the harvest table, the client's
// finished-butchering report, the events, and the use press.

async function startedHost() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-npc-harvest-'));
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

// A player with a health report, so the server counts it alive.
const VITALS = {
  health: 100,
  stamina: 100,
  exhaust: 100,
  hunger: 100,
  bleeding: 0,
  sleeping: 0,
  consciousness: 1,
  drunkenness: 0,
  poisoning: 0,
};

function join(host, id, position, extra = {}) {
  host.updatePlayer({
    id,
    ready: true,
    position,
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    vitals: VITALS,
    ...extra,
  });
  return host.players.get(id);
}

function rowsOf(npc) {
  return npc
    .getInventory()
    .snapshot()
    .items.map((row) => [row.name, row.amount]);
}

const TABLE = {
  Boar: [
    { item: 'boarMeat', amount: 3 },
    { item: 'boarKidney', amount: 1 },
  ],
};

// A dead boar at the origin, a harvest table for boars and a player two metres from it.
function setup({ host, Npc }) {
  Npc.setHarvestTable(TABLE);
  const boar = Npc.create({ class: 'Boar', position: { x: 0, y: 0, z: 0 } });
  const player = join(host, 8, { x: 2, y: 0, z: 0 });
  return { boar, player };
}

function report(host, npc, player) {
  return host.npcs.harvests.harvest({ id: npc.id, playerId: player.id });
}

test('setHarvestTable replaces the table, validates it and returns a copy', async () => {
  const { Npc } = await startedHost();
  assert.deepEqual(Npc.harvestTable, {});
  const given = Npc.setHarvestTable(TABLE);
  assert.deepEqual(given, {
    Boar: [
      { item: lookupItemClass('boarMeat').id, amount: 3 },
      { item: lookupItemClass('boarKidney').id, amount: 1 },
    ],
  });
  given.Boar.length = 0;
  assert.equal(Npc.harvestTable.Boar.length, 2, 'the copy is detached');
  for (const bad of [
    null,
    [],
    { NPC: [] },
    { Boar: 'meat' },
    { Boar: [{ item: 'noSuchItem', amount: 1 }] },
    { Boar: [{ item: 'boarMeat', amount: 0 }] },
    { Boar: [{ item: 'boarMeat', amount: 1.5 }] },
    { Boar: [{ item: 'boarMeat', amount: 101 }] },
    { Boar: new Array(17).fill({ item: 'boarMeat', amount: 1 }) },
  ]) {
    assert.throws(() => Npc.setHarvestTable(bad), JSON.stringify(bad));
  }
  assert.equal(Npc.harvestTable.Boar.length, 2, 'a refused table changes nothing');
  Npc.setHarvestTable({});
  assert.deepEqual(Npc.harvestTable, {});
});

test('a dead animal is harvested once: the server table fills its inventory and both events follow', async () => {
  const started = await startedHost();
  const { host } = started;
  const { boar, player } = setup(started);
  const events = [];
  host.on('npcHarvest', (who, npc) => events.push(['ask', who.id, npc.id]));
  host.on('npcHarvested', (npc, who) => events.push(['done', npc.id, who.id]));
  host.on('npcInventoryChanged', (npc, change) =>
    events.push(['changed', npc.id, change.reason, change.items.length]),
  );
  assert.equal(boar.harvested, false);
  assert.equal(boar.harvestable, false, 'a living animal cannot be harvested');
  assert.equal(report(host, boar, player), false);
  boar.kill();
  assert.equal(boar.harvestable, true);
  assert.equal(report(host, boar, player), true);
  assert.deepEqual(rowsOf(boar), [
    ['boarMeat', 3],
    ['boarKidney', 1],
  ]);
  assert.equal(boar.harvested, true);
  assert.equal(boar.harvestable, false);
  assert.equal(boar.getInventory().snapshot().harvested, true);
  assert.deepEqual(events, [
    ['ask', 8, boar.id],
    ['changed', boar.id, 'harvest', 1],
    ['changed', boar.id, 'harvest', 1],
    ['done', boar.id, 8],
  ]);
  assert.equal(report(host, boar, player), false, 'a second report yields nothing');
  assert.deepEqual(rowsOf(boar), [
    ['boarMeat', 3],
    ['boarKidney', 1],
  ]);
  assert.equal(events.length, 4, 'a second report raises no event');
});

test('a harvest report is refused for the wrong carcass, player, place or an unknown id', async () => {
  const started = await startedHost();
  const { host, Npc } = started;
  const { boar, player } = setup(started);
  boar.kill();
  const person = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  person.kill();
  const far = join(host, 9, { x: 50, y: 0, z: 0 });
  const elsewhere = join(host, 10, { x: 1, y: 0, z: 0 }, { dimension: 5 });
  assert.equal(report(host, person, player), false, 'a person is no animal');
  assert.equal(report(host, boar, far), false, 'too far from the carcass');
  const inWorld = Npc.create({ class: 'Boar', position: { x: 0, y: 0, z: 0 }, dimension: 3 });
  inWorld.kill();
  assert.equal(report(host, inWorld, elsewhere), false, 'another dimension');
  assert.equal(host.npcs.harvests.harvest({ id: boar.id, playerId: 999 }), false);
  assert.equal(host.npcs.harvests.harvest({ id: 123456, playerId: player.id }), false);
  Npc.setHarvestTable({});
  assert.equal(report(host, boar, player), false, 'a class outside the table yields nothing');
  assert.deepEqual(rowsOf(boar), []);
  assert.equal(boar.harvested, false);
});

test('an npcHarvest handler decides who may harvest; refusing leaves the carcass whole', async () => {
  const started = await startedHost();
  const { host } = started;
  const { boar, player } = setup(started);
  let allowed = false;
  const harvested = [];
  host.on('npcHarvest', () => allowed);
  host.on('npcHarvested', (npc) => harvested.push(npc.id));
  boar.kill();
  assert.equal(report(host, boar, player), false);
  assert.deepEqual(rowsOf(boar), []);
  assert.equal(boar.harvestable, true, 'a refusal does not spend the harvest');
  allowed = true;
  assert.equal(report(host, boar, player), true);
  assert.deepEqual(harvested, [boar.id]);
});

test('the yield is lootable and a full inventory still counts as harvested', async () => {
  const started = await startedHost();
  const { host } = started;
  const { boar, player } = setup(started);
  boar.kill();
  report(host, boar, player);
  const [body] = host.makeAPI().Stash.all();
  assert.equal(body.itemCount, 2);
  assert.equal(host.stashes.open(player, body.id), true);
  const meat = boar.getInventory().snapshot().items[0];
  assert.equal(host.stashes.take(player, body.id, meat.id, 3).ok, true);
  assert.deepEqual(rowsOf(boar), [['boarKidney', 1]]);
});

test('revive starts a new life: the flag clears and the next death can be harvested', async () => {
  const started = await startedHost();
  const { host } = started;
  const { boar, player } = setup(started);
  boar.kill();
  report(host, boar, player);
  assert.equal(boar.harvested, true);
  boar.revive();
  assert.equal(boar.harvested, false);
  assert.equal(boar.harvestable, false, 'alive again');
  boar.kill();
  assert.equal(report(host, boar, player), true);
  assert.deepEqual(rowsOf(boar), [
    ['boarMeat', 6],
    ['boarKidney', 2],
  ]);
});

test('Inventory.set restores the harvested flag of a corpse, and only an NPC has one', async () => {
  const started = await startedHost();
  const { host, Npc } = started;
  const { boar, player } = setup(started);
  boar.kill();
  const restored = boar.getInventory().set({
    items: [{ id: 'meat', item: 'boarMeat', amount: 2 }],
    harvested: true,
  });
  assert.equal(restored.result.ok, true);
  assert.equal(boar.harvested, true);
  assert.equal(report(host, boar, player), false, 'a restored carcass is not harvested again');
  boar.getInventory().set({ items: [] });
  assert.equal(boar.harvested, true, 'absent keeps the flag');
  assert.equal(
    boar.getInventory().set({ items: [], harvested: 'yes' }).result.code,
    'invalidRequest',
  );
  const other = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  assert.equal(other.getInventory().snapshot().harvested, false);
  assert.equal(player.getInventory().harvested, undefined);
});

test('interactable is on by default, comes from the create option and is told to the clients', async () => {
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  assert.equal(npc.interactable, true);
  assert.equal(sent('npcInteractable').length, 0, 'the default needs no message');
  npc.interactable = false;
  assert.equal(npc.interactable, false);
  assert.deepEqual(sent('npcInteractable').pop(), {
    op: 'npcInteractable',
    id: npc.id,
    interactable: false,
  });
  const quiet = Npc.create({ position: { x: 0, y: 0, z: 0 }, interactable: false });
  assert.equal(quiet.interactable, false);
  assert.equal(sent('npcInteractable').pop().id, quiet.id);
  for (const bad of [1, 'true', null, undefined]) {
    assert.throws(() => {
      npc.interactable = bad;
    }, TypeError);
  }
  assert.throws(
    () => Npc.create({ position: { x: 0, y: 0, z: 0 }, interactable: 'no' }),
    TypeError,
  );
});

test('a use press raises npcInteract only for an interactable NPC the player stands near', async () => {
  const started = await startedHost();
  const { host, Npc } = started;
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  const near = join(host, 8, { x: 2, y: 0, z: 0 });
  const far = join(host, 9, { x: 50, y: 0, z: 0 });
  const seen = [];
  host.on('npcInteract', (target, by) => seen.push([target.id, by.id]));
  const press = (player, id = npc.id) => host.npcs.harvests.interact({ id, playerId: player.id });
  assert.equal(press(near), true);
  assert.equal(press(far), false, 'the server does not agree with the claim');
  assert.equal(press(near, 123456), false);
  assert.equal(host.npcs.harvests.interact({ id: npc.id, playerId: 999 }), false);
  npc.interactable = false;
  assert.equal(press(near), false);
  npc.interactable = true;
  npc.kill();
  assert.equal(press(near), true, 'a corpse can be used');
  assert.deepEqual(seen, [
    [npc.id, 8],
    [npc.id, 8],
  ]);
  assert.ok(Npc.getById(npc.id));
});

test('opening a dead animal carcass harvests it once, before its rows go out', async () => {
  const started = await startedHost();
  const { host } = started;
  const { boar, player } = setup(started);
  const harvested = [];
  host.on('npcHarvested', (npc, who) => harvested.push([npc.id, who.id]));
  boar.kill();
  const [carcass] = host.makeAPI().Stash.all();
  assert.equal(host.stashes.open(player, carcass.id), true);
  assert.deepEqual(rowsOf(boar), [
    ['boarMeat', 3],
    ['boarKidney', 1],
  ]);
  const shown = started.sent('stashShow').pop();
  assert.equal(shown.rows.length, 2, 'the opener sees the yield');
  assert.equal(boar.harvested, true);
  host.stashes.close?.(player, carcass.id);
  host.stashes.open(player, carcass.id);
  assert.deepEqual(harvested, [[boar.id, player.id]], 'harvested once per life');
  assert.equal(rowsOf(boar).length, 2, 'no second yield');
});

// a handler's own yield for this player's harvest; a wrong one gives the table's.
test('an npcHarvest handler may return a yield of its own for this harvest', async () => {
  const started = await startedHost();
  const { host } = started;
  const { boar, player } = setup(started);
  const logged = [];
  host.bridge.log = (resource, text) => logged.push(text);
  host.on('npcHarvest', (who) =>
    who.id === 8
      ? {
          items: [
            { item: 'boarMeat', amount: 5 },
            { item: 'boarKidney', amount: 2 },
          ],
        }
      : undefined,
  );
  boar.kill();
  assert.equal(report(host, boar, player), true);
  assert.deepEqual(rowsOf(boar), [
    ['boarMeat', 5],
    ['boarKidney', 2],
  ]);
  const second = started.Npc.create({ class: 'Boar', position: { x: 0, y: 0, z: 0 } });
  host.handlers.get('npcHarvest').length = 0;
  host.on('npcHarvest', () => ({ items: [{ item: 'noSuchItem', amount: 1 }] }));
  second.kill();
  assert.equal(report(host, second, player), true);
  assert.deepEqual(rowsOf(second), [
    ['boarMeat', 3],
    ['boarKidney', 1],
  ]);
  assert.ok(logged.some((line) => line.includes('npcHarvest yield refused')));
});
