'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { POTS } = require('./cookpot/cookpots.cjs');

// CookPot: the stew pots' stock per world, pot meals reserving and spending it.

const POT = [...POTS.values()][0];
const GOULASH = '0dc9d98b-c59c-495d-b8e4-316448719026';
const SOUP = 'ba16dff8-4c13-417e-bbeb-d5ab435be20d';
const LENTIL = 'ffa8c4dd-5d4b-4fbc-ac40-38b50536b454';
const BANE = '42e54d97-6e63-4e50-a09d-325ef4dd2286';
const BANE_BUFF = '58138b68-0c86-4d5e-8823-417106d08d3c';
const WORLD = 4294967295;
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

function hostWith(code = '') {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcd2s-cookpot-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'pots');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'pots', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  return { host, output };
}

function player(host, position = POT.position, id = 7) {
  host.updatePlayer({
    id,
    ready: true,
    position,
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    vitals: VITALS,
  });
  return host.players.get(id);
}

const verdicts = (output) =>
  output.filter((e) => e.op === 'inventoryConsumeReply').map((e) => [e.request, e.verdict]);

test("a pot nobody used serves the level's meal; writes change its stock", async () => {
  const { host } = hostWith();
  await host.startAll();
  const { CookPot } = host.api;
  // The global world lists every catalogued pot from the start, with the level's meal, full or
  // empty as the level starts it.
  assert.equal(POT.meal, 'lentil');
  assert.equal(CookPot.all().length, 322);
  assert.ok(CookPot.all().some((each) => each.guid === POT.guid && each.food === 'lentil'));
  const empty = [...POTS.values()].find((pot) => pot.full === false);
  assert.deepEqual([CookPot.get(empty.guid).portions, CookPot.get(empty.guid).state], [0, 'empty']);
  assert.deepEqual(CookPot.all(3), []);
  const pot = CookPot.get(POT.guid);
  assert.deepEqual(pot, {
    food: 'lentil',
    nutrition: 10,
    poison: null,
    guid: POT.guid,
    position: POT.position,
    portions: 4,
    capacity: 4,
    enabled: true,
    state: 'full',
    fireLit: null,
    dimension: WORLD,
  });
  assert.ok(Object.isFrozen(pot));
  assert.equal(CookPot.all().length, 322);
  assert.equal(CookPot.get('00000000-0000-0000-0000-000000000000'), null);
  assert.equal(CookPot.setState(POT.guid, 'half'), true);
  assert.equal(CookPot.get(POT.guid).state, 'half');
  assert.equal(CookPot.refill(POT.guid, 10), true);
  assert.deepEqual([CookPot.get(POT.guid).portions, CookPot.get(POT.guid).capacity], [10, 10]);
  assert.equal(CookPot.setFood(POT.guid, 'goulash'), true);
  assert.equal(CookPot.get(POT.guid).nutrition, 25);
  // Each world has its own stock.
  assert.equal(CookPot.get(POT.guid, 3).portions, 4);
  assert.throws(() => CookPot.setFood(POT.guid, 'cake'), TypeError);
  assert.throws(() => CookPot.refill(POT.guid, 1001), TypeError);
  assert.throws(() => CookPot.get('fireplace'), TypeError);
  assert.equal(CookPot.poisons().length, 15);
});

test('a pot meal reserves a meal, the receipt spends it, a meal not eaten goes back', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const { CookPot } = host.api;
  const p = player(host);
  const meal = { item: LENTIL, pot: POT.guid };
  host.playerInventories.consumePot(p, { request: 1, ...meal });
  assert.equal(CookPot.get(POT.guid).portions, 3, 'reserved');
  // Eaten: the pot's look goes to the player's game again, its kettle told the server's fill.
  const looksBefore = output.filter((e) => e.op === 'potLooks').length;
  host.playerInventories.consumed(p, { request: 1, used: true });
  assert.equal(CookPot.get(POT.guid).portions, 3);
  const again = output.filter((e) => e.op === 'potLooks').slice(looksBefore);
  assert.deepEqual(
    again.map((e) => [e.id, e.looks]),
    [[7, [[POT.cauldron, true, 5, POT.fireplace, 0]]]],
  );
  host.playerInventories.consumePot(p, { request: 2, ...meal });
  host.playerInventories.consumed(p, { request: 2, used: false });
  assert.equal(CookPot.get(POT.guid).portions, 3, 'not eaten: back in the pot');
  // Disabled, then empty: refused.
  CookPot.setEnabled(POT.guid, false);
  host.playerInventories.consumePot(p, { request: 3, ...meal });
  CookPot.setEnabled(POT.guid, true);
  CookPot.setState(POT.guid, 'empty');
  host.playerInventories.consumePot(p, { request: 4, ...meal });
  assert.deepEqual(verdicts(output), [
    [1, 'native'],
    [2, 'native'],
    [3, 'refused'],
    [4, 'refused'],
  ]);
  host.playerInventories.flush();
});

test("a pot serving another meal than the game's gives that meal's nutrition", async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const { CookPot } = host.api;
  const p = player(host);
  CookPot.setFood(POT.guid, 'goulash');
  host.playerInventories.consumePot(p, { request: 1, item: SOUP, pot: POT.guid });
  host.playerInventories.consumed(p, { request: 1, used: true });
  assert.deepEqual(verdicts(output), [[1, 'plain']]);
  const added = output.filter((e) => e.op === 'addStat');
  assert.equal(added.length, 1);
  assert.equal(added[0].id, 7);
  // The pot's own meal keeps the game's effects.
  host.playerInventories.consumePot(p, { request: 2, item: GOULASH, pot: POT.guid });
  assert.deepEqual(verdicts(output)[1], [2, 'native']);
  host.playerInventories.flush();
});

test('topUpFrom takes the ingredients; poison spends one unit and rides the meals', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const { CookPot, Inventory } = host.api;
  const p = player(host);
  CookPot.setState(POT.guid, 'empty');
  const recipe = { food: 'lentil', portions: 6, ingredients: [{ item: 'apple', amount: 3 }] };
  assert.deepEqual(CookPot.topUpFrom(p, POT.guid, recipe), {
    ok: false,
    code: 'insufficientItems',
  });
  p.giveItem('apple', 3);
  assert.deepEqual(CookPot.topUpFrom(p, POT.guid, recipe), { ok: true, code: '' });
  assert.deepEqual([CookPot.get(POT.guid).food, CookPot.get(POT.guid).portions], ['lentil', 6]);
  assert.equal(p.getInventory().items.filter((row) => row.name === 'apple').length, 0);
  assert.equal(CookPot.topUpFrom(p, POT.guid, recipe).code, 'notEmpty');
  // Poison: one unit of a poison row.
  const row = Inventory.add(p, { item: BANE, amount: 2 }).items[0].id;
  assert.deepEqual(CookPot.poison(p, POT.guid, row), { ok: true, code: '' });
  assert.equal(CookPot.get(POT.guid).poison, BANE_BUFF);
  assert.equal(p.getInventory().items.find((r) => r.id === row).amount, 1);
  assert.equal(CookPot.poison(p, POT.guid, row).code, 'alreadyPoisoned');
  host.playerInventories.consumePot(p, { request: 1, item: SOUP, pot: POT.guid });
  host.playerInventories.consumed(p, { request: 1, used: true });
  assert.deepEqual(
    output.filter((e) => e.op === 'buffOrder').map((e) => [e.kind, e.buff]),
    [['add', BANE_BUFF]],
    "the pot's poison rides a meal of the server's own too",
  );
  // Too far away.
  const far = player(host, { x: POT.position.x + 10, y: POT.position.y, z: POT.position.z }, 8);
  assert.equal(CookPot.poison(far, POT.guid, row).code, 'tooFar');
  assert.throws(() => CookPot.poison({}, POT.guid, row), TypeError);
  host.playerInventories.flush();
});

test("every player's game sees the pots of their world, and each change", async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const { CookPot } = host.api;
  const looks = () => output.filter((e) => e.op === 'potLooks');
  // Arriving: every catalogued pot's cauldron, home or camp, in its phase.
  player(host);
  const [all] = looks();
  assert.equal(all.id, 7);
  assert.equal(all.looks.length, 322);
  const lookOf = (entries, pot) => entries.find(([cauldron]) => cauldron === pot.cauldron);
  // A camp pot of lentil, full: phase 5.
  assert.deepEqual(lookOf(all.looks, POT), [POT.cauldron, true, 5, POT.fireplace, 0]);
  const empty = [...POTS.values()].find((pot) => pot.full === false);
  assert.equal(lookOf(all.looks, empty)[2], 1);
  // A change: half, then the meal, then empty; a write that keeps the look sends nothing.
  output.length = 0;
  CookPot.setState(POT.guid, 'half');
  CookPot.setFood(POT.guid, 'goulash');
  CookPot.setEnabled(POT.guid, false);
  CookPot.setState(POT.guid, 'half');
  CookPot.refill(POT.guid, 0);
  assert.deepEqual(
    looks().map((e) => [e.id, e.looks]),
    [
      [7, [[POT.cauldron, true, 4, POT.fireplace, 0]]],
      [7, [[POT.cauldron, true, 2, POT.fireplace, 0]]],
      [7, [[POT.cauldron, true, 1, POT.fireplace, 0]]],
    ],
  );
  // Another world's change is not this player's.
  output.length = 0;
  CookPot.setState(POT.guid, 'full', 3);
  assert.deepEqual(looks(), []);
  // The server's fire: held lit, put out, handed back to each game.
  assert.equal(CookPot.setFire(POT.guid, true), true);
  assert.equal(CookPot.get(POT.guid).fireLit, true);
  CookPot.setFire(POT.guid, true);
  CookPot.setFire(POT.guid, false);
  CookPot.setFire(POT.guid, null);
  assert.equal(CookPot.get(POT.guid).fireLit, null);
  assert.deepEqual(
    looks().map((e) => e.looks[0][4]),
    [1, 2, 0],
  );
  assert.throws(() => CookPot.setFire(POT.guid, 'yes'), TypeError);
  assert.equal(CookPot.setFire('00000000-0000-0000-0000-000000000000', true), false);
});
