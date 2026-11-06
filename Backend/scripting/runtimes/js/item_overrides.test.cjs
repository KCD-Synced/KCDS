'use strict';

// The server's item config: weights and prices of the game's own items.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');

const APPLE = '2264f217-590e-4c0f-a4c6-f50c6532b9f6';
const BREAD = '86e4ff24-88db-4024-abe6-46545fa0fbd1';

function hostWith(config) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-item-config-'));
  if (config !== undefined) {
    const text = typeof config === 'string' ? config : JSON.stringify(config);
    fs.writeFileSync(path.join(root, 'item-config.json'), text);
  }
  const output = [];
  const logs = [];
  const host = new ResourceHost(root, {
    send: (e) => output.push(e),
    log: (who, text) => logs.push(text),
  });
  return { host, output, logs };
}

function arrive(host, id) {
  host.updatePlayer({
    id,
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
}

test('the item config overrides weights and prices, naming each wrong entry', () => {
  const { host, output, logs } = hostWith({
    apple: { weight: 1.5, price: 20 },
    [BREAD]: { price: 100, max: 3 },
    'no-such-item': { weight: 1 },
    lockpick_nope: { price: -1 },
    [APPLE.toUpperCase()]: { colour: 'red' },
  });
  assert.ok(logs.some((line) => line.includes('unknown item "no-such-item"')));
  assert.ok(logs.some((line) => line.includes('unknown item "lockpick_nope"')));
  assert.ok(logs.some((line) => line.includes('apple: colour cannot be overridden')));
  assert.ok(logs.some((line) => line.includes('2 items overridden')));
  const api = host.makeAPI();
  assert.deepEqual(
    { ...api.Items.effective('apple') },
    { id: APPLE, name: 'apple', weight: 1.5, price: 20, max: null, stats: {}, overridden: true },
  );
  assert.deepEqual(
    { ...api.Items.effective(BREAD) },
    { id: BREAD, name: 'bread', weight: 0.6, price: 100, max: 3, stats: {}, overridden: true },
  );
  // A player coming into the world hears the whole set.
  arrive(host, 7);
  const sent = output.filter((e) => e.op === 'itemOverrides');
  assert.equal(sent.length, 1);
  assert.equal(sent[0].id, 7);
  assert.deepEqual(sent[0].items, [
    [APPLE, 1.5, 20, null, []],
    [BREAD, null, 100, 3, []],
  ]);
  assert.equal(sent[0].maxTooltip, 'Most you can carry');
});

test('Items.override and clearOverride change the set for everyone at run time', () => {
  const { host, output } = hostWith();
  const api = host.makeAPI();
  arrive(host, 7);
  assert.equal(output.filter((e) => e.op === 'itemOverrides').length, 0, 'no set, nothing sent');
  const before = api.Inventory.itemPrice('bread');
  const bread = api.Items.override('bread', { price: 540 });
  assert.equal(bread.price, 540);
  assert.equal(bread.weight, 0.6);
  const sent = output.filter((e) => e.op === 'itemOverrides');
  assert.deepEqual(sent.at(-1), {
    op: 'itemOverrides',
    id: 7,
    items: [[BREAD, null, 540, null, []]],
    maxTooltip: 'Most you can carry',
    equippedWeightPercent: 100,
  });
  // The server's prices follow the override.
  const after = api.Inventory.itemPrice('bread');
  assert.equal(after.pristine, before.pristine * 10);
  assert.throws(() => api.Items.override('bread', { weight: Number.NaN }), /weight is a number/);
  assert.throws(() => api.Items.override('bread', { price: 1.5 }), /price is a whole number/);
  assert.throws(() => api.Items.override('bread', {}), /changes nothing/);
  assert.throws(() => api.Items.override('nothing', { price: 1 }), /unknown item/);
  assert.equal(api.Items.overrides().length, 1);
  assert.equal(api.Items.clearOverride('bread'), true);
  assert.equal(api.Items.clearOverride('bread'), false);
  assert.deepEqual(output.filter((e) => e.op === 'itemOverrides').at(-1).items, []);
  assert.deepEqual(api.Inventory.itemPrice('bread'), before);
  assert.equal(api.Items.effective('bread').overridden, false);
});

test('stats are overridden by what the item category carries', () => {
  const { host, output, logs } = hostWith({
    longSwordDuel: { attack: 200, attackModStab: 1.5, strengthRequired: 10 },
    SkullCap02_m02_C2: { defenseStab: 150, noise: 0.1 },
    apple: { health: 10, nutrition: 6 },
    cuman_bow_a: { power: 250 },
    bread: { attack: 5 },
    TunicShort05_m09_D: { strengthRequired: 2.5 },
  });
  assert.ok(logs.some((line) => line.includes('bread: attack cannot be overridden')));
  assert.ok(logs.some((line) => line.includes('strengthRequired is a whole number 0 to 1000')));
  const api = host.makeAPI();
  assert.deepEqual(
    { ...api.Items.effective('longSwordDuel').stats },
    { attack: 200, attackModStab: 1.5, strengthRequired: 10 },
  );
  arrive(host, 7);
  const items = new Map(
    output
      .filter((e) => e.op === 'itemOverrides')
      .at(-1)
      .items.map((row) => [row[0], row[4]]),
  );
  // The wire ids (kcd2s/integration/item_overrides.hpp): attack 20, attackModStab 21,
  // weapon strengthRequired 26, defenseStab 10, noise 14, health 30, nutrition 32, power 20.
  assert.deepEqual(items.get('00cca9e3-8ef2-46db-8cbf-86ec51930919'), [
    [20, 200],
    [21, 1.5],
    [26, 10],
  ]);
  assert.deepEqual(items.get('02e23cb9-05aa-4aab-bbb8-4023a72e22f9'), [
    [10, 150],
    [14, 0.1],
  ]);
  assert.deepEqual(items.get(APPLE), [
    [30, 10],
    [32, 6],
  ]);
  assert.deepEqual(items.get('0077dfa2-b9be-4ae3-ae59-2803ba49dfcb'), [[20, 250]]);
  // The server's own effects of eating follow the food stats.
  const { lookupItemClass } = require('./inventory/catalog.cjs');
  const apple = host.itemOverrides.foodOf(lookupItemClass('apple'));
  assert.equal(apple.baseEffects.health, 10);
  assert.equal(apple.baseEffects.nourishment, 6);
  assert.equal(apple.baseEffects.energy, 0);
});

test("a drink's alcohol and decay and a bolt's damage are overridden too", () => {
  const { host, output } = hostWith({
    dlc3_beer: { alcohol: 20, decayTime: 60, shortTermNutrition: 0.5 },
    bolt_enh_precise: { attackStab: 300, powerMod: 1.5, attackSlash: 40, attackSmash: 25 },
  });
  arrive(host, 7);
  const items = new Map(
    output
      .filter((e) => e.op === 'itemOverrides')
      .at(-1)
      .items.map((row) => [row[0], row[4]]),
  );
  // Wire ids: alcohol 33, decayTime 34, shortTermNutrition 35, ammo attackStab 40, powerMod 41, attackSlash 42,
  // attackSmash 43.
  assert.deepEqual(items.get('18920886-b5cf-45a6-86f9-eb92268f48bc'), [
    [33, 20],
    [34, 60],
    [35, 0.5],
  ]);
  assert.deepEqual(items.get('081fc4a1-25e9-4492-8dc8-2d9d6668c07a'), [
    [40, 300],
    [41, 1.5],
    [42, 40],
    [43, 25],
  ]);
  // Drinking follows the overridden alcohol.
  const { lookupItemClass } = require('./inventory/catalog.cjs');
  assert.equal(host.itemOverrides.foodOf(lookupItemClass('dlc3_beer')).baseEffects.alcohol, 20);
});

test('a broken item config is named and leaves the game values', () => {
  const { host, logs } = hostWith('{ "apple": ');
  assert.ok(logs.some((line) => line.startsWith('item-config.json: ')));
  assert.equal(host.makeAPI().Items.effective('apple').price, 8);
});

test('a registered type keeps its own values', () => {
  const { host } = hostWith();
  const api = host.makeAPI();
  api.Items.register('village:timber', { name: 'Timber', visual: 'bsmt_woodExotic', weight: 2 });
  assert.throws(() => api.Items.override('village:timber', { price: 5 }), /registered type/);
});

test('a limit keeps a player from holding more of an item, and tells them in their language', () => {
  const { host, output } = hostWith({ apple: { max: 5 } });
  const api = host.makeAPI();
  host.updatePlayer({
    id: 7,
    ready: true,
    language: 'czech',
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const p = host.players.get(7);
  api.Locales.set('czech', {
    'items.maxTooltip': 'Nejvíc uneseš',
    'items.limitReached': 'Víc než {max} neuneseš',
  });
  host.itemOverrides.send(0);
  assert.equal(output.filter((e) => e.op === 'itemOverrides').at(-1).maxTooltip, 'Nejvíc uneseš');
  assert.equal(api.Inventory.add(p, { item: 'apple', amount: 4 }).ok, true);
  const over = api.Inventory.add(p, { item: 'apple', amount: 2 });
  assert.equal(over.ok, false);
  assert.equal(over.code, 'itemLimit');
  const notices = () => output.filter((e) => e.op === 'itemLimitNotice');
  assert.deepEqual(notices(), [{ op: 'itemLimitNotice', id: 7, text: 'Víc než 5 neuneseš' }]);
  // Told once in a while, not at every try.
  assert.equal(api.Inventory.add(p, { item: 'apple', amount: 2 }).ok, false);
  assert.equal(notices().length, 1);
  assert.equal(api.Inventory.add(p, { item: 'apple', amount: 1 }).ok, true);
  // What the game picked up itself over the limit is not taken; the game is shown the stock.
  const entry = host.playerInventories.entryOf(p);
  const report = entry.inventory.applyNative([{ kind: 'gained', item: APPLE, amount: 3 }]);
  assert.equal(report.corrected, true);
  assert.equal(
    entry.inventory.unitsOfClass(entry.inventory.rows.values().next().value.itemClass),
    5,
  );
  // A container's units over the limit stay in it.
  const { Inventory, transfer } = require('./inventory/inventories.cjs');
  const crate = new Inventory();
  crate.add({ item: 'apple', amount: 2 });
  const moved = transfer(crate, entry.inventory, {
    units: [{ id: crate.rows.keys().next().value, amount: 1 }],
  });
  assert.equal(moved.result.code, 'itemLimit');
  assert.equal(crate.unitsOfClass(crate.rows.values().next().value.itemClass), 2);
  // Nor can a deal at a vendor bring more.
  const { lookupItemClass } = require('./inventory/catalog.cjs');
  const apple = lookupItemClass('apple');
  const bought = entry.inventory.settle([], lookupItemClass('money'), 0, [
    { itemClass: apple, props: { quality: 1, health: 1 }, amount: 1 },
  ]);
  assert.equal(bought.result.code, 'itemLimit');
  // Without the limit the player takes them.
  api.Items.clearOverride('apple');
  assert.equal(api.Inventory.add(p, { item: 'apple', amount: 2 }).ok, true);
});

// a player's own limit, over the server's item config.
test("a player's own item limit stands over the server's and is shown only to that player", () => {
  const { host, output } = hostWith({ apple: { max: 5 } });
  const api = host.makeAPI();
  arrive(host, 7);
  arrive(host, 8);
  const seven = host.players.get(7);
  const eight = host.players.get(8);
  assert.equal(seven.itemLimit('apple'), 5);
  assert.equal(seven.setItemLimit('apple', 2), 2);
  assert.equal(seven.setItemLimit('bread', 1), 1);
  assert.equal(eight.itemLimit('apple'), 5);
  assert.equal(api.Inventory.add(seven, { item: 'apple', amount: 2 }).ok, true);
  assert.equal(api.Inventory.add(seven, { item: 'apple', amount: 1 }).code, 'itemLimit');
  assert.equal(api.Inventory.add(eight, { item: 'apple', amount: 5 }).ok, true);
  // Player 7's game is sent the server's set with their own limits over it.
  const toSeven = output.filter((e) => e.op === 'itemOverrides' && e.id === 7).at(-1);
  const limits = Object.fromEntries(toSeven.items.map((entry) => [entry[0], entry[3]]));
  assert.deepEqual(limits, { [APPLE]: 2, [BREAD]: 1 });
  const toEight = output.filter((e) => e.op === 'itemOverrides' && e.id === 8).at(-1);
  assert.deepEqual(
    toEight.items.map((entry) => [entry[0], entry[3]]),
    [[APPLE, 5]],
  );
  // Cleared, the server's limit stands again; wrong limits throw.
  assert.equal(seven.setItemLimit('apple', null), 5);
  assert.equal(seven.setItemLimit('bread', null), null);
  assert.throws(() => seven.setItemLimit('apple', 0), TypeError);
  assert.throws(() => seven.setItemLimit('no-such-item', 3), TypeError);
  // A leaving player's limits go with them.
  seven.setItemLimit('apple', 1);
  host.disconnect(7);
  assert.equal(host.itemOverrides.playerLimits.has(7), false);
});

// the share of equipped items' weight in a player's load.
test("the equipped weight share goes to every game, a player's own over the server's", () => {
  const { host, output } = hostWith();
  const api = host.makeAPI();
  arrive(host, 7);
  arrive(host, 8);
  const seven = host.players.get(7);
  assert.equal(api.Items.equippedWeightPercent(), 100);
  api.Items.setEquippedWeightPercent(40);
  const last = (id) => output.filter((e) => e.op === 'itemOverrides' && e.id === id).at(-1);
  assert.equal(last(7).equippedWeightPercent, 40);
  assert.equal(last(8).equippedWeightPercent, 40);
  assert.equal(seven.setEquippedWeightPercent(0), 0);
  assert.equal(last(7).equippedWeightPercent, 0);
  assert.equal(host.players.get(8).equippedWeightPercent, 40);
  assert.equal(seven.setEquippedWeightPercent(null), 40);
  assert.throws(() => api.Items.setEquippedWeightPercent(101), TypeError);
  assert.throws(() => seven.setEquippedWeightPercent(1.5), TypeError);
});

// Inventory.overrideStats and itemStats: a class's combat numbers by the names
// of that call, over the item overrides.
test('overrideStats replaces the combat numbers whole and itemStats reads them back', () => {
  const { host, output } = hostWith({ longSwordDuel: { weight: 2, durability: 500 } });
  const api = host.makeAPI();
  assert.equal(api.Inventory.itemStats('longSwordDuel'), null);
  assert.equal(api.Inventory.overrideStats('longSwordDuel', { attack: 210, strReq: 12 }), true);
  assert.deepEqual(api.Inventory.itemStats('longSwordDuel'), { attack: 210, strReq: 12 });
  // The weight and durability of the config stay; the stats go out as overrides.
  assert.deepEqual(
    { ...api.Items.effective('longSwordDuel').stats },
    { durability: 500, attack: 210, strengthRequired: 12 },
  );
  assert.equal(api.Items.effective('longSwordDuel').weight, 2);
  // A new set replaces the previous one whole.
  api.Inventory.overrideStats('longSwordDuel', { defense: 40 });
  assert.deepEqual(api.Inventory.itemStats('longSwordDuel'), { defense: 40 });
  // null gives the authored numbers back, the config's own fields still standing.
  api.Inventory.overrideStats('longSwordDuel', null);
  assert.equal(api.Inventory.itemStats('longSwordDuel'), null);
  assert.deepEqual({ ...api.Items.effective('longSwordDuel').stats }, { durability: 500 });
  // A class with no other override is left as authored altogether.
  api.Inventory.overrideStats('cuman_bow_a', { power: 250, agiReq: 5 });
  assert.deepEqual(api.Inventory.itemStats('cuman_bow_a'), { power: 250, agiReq: 5 });
  api.Inventory.overrideStats('cuman_bow_a', {});
  assert.equal(api.Items.overrides().length, 1);
  // Names a kind lacks, values out of range and classes without combat numbers throw.
  assert.throws(() => api.Inventory.overrideStats('longSwordDuel', { noise: 1 }), /noise/);
  assert.throws(() => api.Inventory.overrideStats('longSwordDuel', { attack: -1 }), /attack/);
  assert.throws(() => api.Inventory.overrideStats('cuman_bow_a', { limbResistance: 1 }), /limb/);
  assert.throws(() => api.Inventory.overrideStats('apple', { attack: 1 }), /combat/);
  assert.throws(() => api.Inventory.overrideStats('nothing_at_all', { attack: 1 }), TypeError);
  arrive(host, 7);
  assert.ok(output.some((e) => e.op === 'itemOverrides'));
});
