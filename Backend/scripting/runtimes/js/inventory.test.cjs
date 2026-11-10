'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { Inventory, transfer } = require('./inventory/inventories.cjs');
const { itemClasses, lookupItemClass, qualityBand } = require('./inventory/catalog.cjs');

// longSwordDuel: a sword made in four qualities.
const SWORD = '00cca9e3-8ef2-46db-8cbf-86ec51930919';

function firstOf(category, test) {
  return itemClasses().find(
    (entry) => entry.category === category && test(lookupItemClass(entry.id)),
  );
}

test('catalog finds classes by GUID in any case and by table name', () => {
  assert.equal(lookupItemClass(SWORD).name, 'longSwordDuel');
  assert.equal(lookupItemClass(SWORD.toUpperCase()).name, 'longSwordDuel');
  assert.equal(lookupItemClass('longSwordDuel').id, SWORD);
  assert.equal(lookupItemClass('nothing like it'), undefined);
  assert.ok(itemClasses().length > 4000);
});

test('quality bands follow the game: tops q(q+1)/(M(M+1)), bottoms a quarter into the band below', () => {
  const sword = lookupItemClass(SWORD);
  const tops = [1, 2, 3, 4].map((quality) => qualityBand(sword, quality).high);
  assert.deepEqual(tops, [0.1, 0.3, 0.6, 1]);
  assert.equal(qualityBand(sword, 1).low, 0);
  assert.ok(Math.abs(qualityBand(sword, 3).low - 0.3 * 0.75) < 1e-9);
  const food = firstOf('food', (entry) => !entry.qualityBands);
  assert.deepEqual(qualityBand(lookupItemClass(food.id), 1), { low: 0, high: 1 });
});

test('add creates a row at quality 1 and full condition, and joins it on a second add', () => {
  const inventory = new Inventory();
  const first = inventory.add({ item: 'longSwordDuel', amount: 1 });
  assert.equal(first.result.ok, true);
  assert.equal(first.result.revision, 1);
  const id = first.result.items[0].id;
  const row = inventory.snapshot().items[0];
  assert.equal(row.quality, 1);
  assert.equal(row.health, 0.1);
  assert.equal(row.condition, 1);
  assert.deepEqual(row.metadata, {});
  assert.deepEqual(first.change.items[0].before, null);
  const second = inventory.add({ item: SWORD, amount: 2 });
  assert.deepEqual(second.result.items, [{ id, amount: 2 }]);
  assert.equal(inventory.snapshot().items[0].amount, 3);
  assert.equal(second.change.items[0].before.amount, 1);
  assert.equal(second.change.items[0].after.amount, 3);
});

test('add with other properties makes another row', () => {
  const inventory = new Inventory();
  inventory.add({ item: SWORD, amount: 1 });
  const better = inventory.add({
    item: SWORD,
    amount: 1,
    metadata: { quality: 3, condition: 0.5 },
  });
  assert.equal(better.result.ok, true);
  assert.equal(inventory.rows.size, 2);
  const row = inventory.snapshot().items[1];
  assert.equal(row.quality, 3);
  assert.ok(Math.abs(row.condition - 0.5) < 1e-9);
  assert.ok(Math.abs(row.health - (0.225 + 0.5 * (0.6 - 0.225))) < 1e-9);
  assert.equal(row.metadata.quality, 3);
});

test('a transient row is its own row, reads back, and leaves with dropTransient', () => {
  const inventory = new Inventory();
  inventory.add({ item: SWORD, amount: 1 });
  assert.equal(
    inventory.add({ item: SWORD, amount: 1, metadata: { transient: true } }).result.ok,
    true,
  );
  assert.equal(inventory.rows.size, 2);
  assert.deepEqual(
    inventory.snapshot().items.map((row) => row.metadata.transient),
    [undefined, true],
  );
  // false is a kept row, the default.
  assert.equal(
    inventory.add({ item: SWORD, amount: 1, metadata: { transient: false } }).result.ok,
    true,
  );
  assert.equal(inventory.rows.size, 2);
  assert.equal(
    inventory.add({ item: SWORD, amount: 1, metadata: { transient: 'yes' } }).result.code,
    'invalidMetadata',
  );
  const change = inventory.dropTransient();
  assert.equal(change.reason, 'transient');
  assert.equal(inventory.rows.size, 1);
  assert.equal(inventory.snapshot().items[0].amount, 2);
  assert.equal(inventory.dropTransient(), null);
});

test('add refuses what the game would not make', () => {
  const inventory = new Inventory();
  const code = (request) => inventory.add(request).result.code;
  assert.equal(code({ item: 'no such item', amount: 1 }), 'invalidItem');
  assert.equal(code({ item: SWORD, amount: 0 }), 'invalidAmount');
  assert.equal(code({ item: SWORD, amount: 10001 }), 'invalidAmount');
  assert.equal(code({ item: SWORD, amount: 1.5 }), 'invalidAmount');
  assert.equal(code({ item: SWORD, amount: 1, metadata: { quality: 5 } }), 'invalidQuality');
  assert.equal(code({ item: SWORD, amount: 1, metadata: { health: 0.9 } }), 'invalidItemHealth');
  assert.equal(
    code({ item: SWORD, amount: 1, metadata: { quality: 4, health: 0.9, condition: 0.1 } }),
    'contradictoryItemHealth',
  );
  assert.equal(code({ item: SWORD, amount: 1, metadata: { colour: 'red' } }), 'invalidMetadata');
  assert.equal(
    code({ item: SWORD, amount: 1, metadata: { poison: SWORD } }),
    'unsupportedPoisonProperties',
  );
  const quest = itemClasses().find((entry) => entry.quest);
  assert.equal(code({ item: quest.id, amount: 1 }), 'questItem');
  assert.equal(inventory.revision, 0);
  assert.equal(inventory.rows.size, 0);
});

test('a stale revision refuses the operation and changes nothing', () => {
  const inventory = new Inventory();
  inventory.add({ item: SWORD, amount: 1 });
  const result = inventory.add({ item: SWORD, amount: 1, revision: 0 });
  assert.equal(result.result.code, 'staleRevision');
  assert.equal(inventory.snapshot().items[0].amount, 1);
  assert.equal(inventory.add({ item: SWORD, amount: 1, revision: 1 }).result.ok, true);
});

test('remove takes exact units from named rows, all or none', () => {
  const inventory = new Inventory();
  const sword = inventory.add({ item: SWORD, amount: 3 }).result.items[0].id;
  const fine = inventory.add({ item: SWORD, amount: 1, metadata: { quality: 2 } }).result.items[0]
    .id;
  assert.equal(
    inventory.remove({
      units: [
        { id: sword, amount: 2 },
        { id: fine, amount: 2 },
      ],
    }).result.code,
    'insufficientItems',
  );
  assert.equal(inventory.remove({ units: [{ id: 'nope', amount: 1 }] }).result.code, 'unknownItem');
  assert.equal(inventory.revision, 2);
  const result = inventory.remove({
    units: [
      { id: sword, amount: 2 },
      { id: fine, amount: 1 },
    ],
  });
  assert.equal(result.result.ok, true);
  assert.deepEqual(result.result.items, []);
  assert.equal(result.change.items.find((item) => item.id === fine).after, null);
  assert.deepEqual(
    inventory.snapshot().items.map((row) => [row.id, row.amount]),
    [[sword, 1]],
  );
});

test('editRow replaces the properties and keeps identity and amount', () => {
  const inventory = new Inventory();
  const id = inventory.add({ item: SWORD, amount: 2, metadata: { quality: 2 } }).result.items[0].id;
  const result = inventory.editRow({ id, metadata: { quality: 4, condition: 0.25 } });
  assert.equal(result.result.ok, true);
  const row = inventory.snapshot().items[0];
  assert.equal(row.id, id);
  assert.equal(row.amount, 2);
  assert.equal(row.quality, 4);
  assert.equal(result.change.reason, 'properties');
});

test('editCustom changes only the named custom fields', () => {
  const inventory = new Inventory();
  const id = inventory.add({ item: SWORD, amount: 1 }).result.items[0].id;
  inventory.editCustom({ id, name: 'Oathkeeper', data: { owner: 'A' } });
  assert.deepEqual(inventory.snapshot().items[0].metadata.custom, {
    name: 'Oathkeeper',
    data: { owner: 'A' },
  });
  inventory.editCustom({ id, name: null, description: 'Old steel' });
  assert.deepEqual(inventory.snapshot().items[0].metadata.custom, {
    description: 'Old steel',
    data: { owner: 'A' },
  });
  assert.equal(inventory.editCustom({ id, name: 'bad\nname' }).result.code, 'invalidMetadata');
});

test('set replaces everything, keeps given ids and checks equipment and quickslots', () => {
  const inventory = new Inventory();
  inventory.add({ item: SWORD, amount: 1 });
  const food = itemClasses().find((entry) => entry.category === 'food' && !entry.quest);
  assert.equal(
    inventory.set({ items: [{ item: food.id, amount: 2, equipped: 1 }] }).result.code,
    'invalidEquipment',
  );
  assert.equal(
    inventory.set({
      items: [
        { id: 'a', item: SWORD, amount: 1 },
        { id: 'a', item: SWORD, amount: 1 },
      ],
    }).result.code,
    'invalidItems',
  );
  const quickslots = {
    weapons: [
      { primary: 'blade', secondary: null },
      { primary: null, secondary: null },
      { primary: null, secondary: null },
      { primary: null, secondary: null },
    ],
    items: [null, null, null, null],
    activeWeapon: 0,
    activeItem: 0,
  };
  assert.equal(
    inventory.set({ items: [{ id: 'other', item: SWORD, amount: 1 }], quickslots }).result.code,
    'invalidQuickslots',
  );
  const result = inventory.set({
    items: [
      { id: 'blade', item: SWORD, amount: 1, equipped: 1 },
      { item: food.id, amount: 5 },
    ],
    quickslots,
  });
  assert.equal(result.result.ok, true);
  assert.equal(result.change.reason, 'set');
  const state = inventory.snapshot();
  assert.deepEqual(state.items.map((row) => row.id)[0], 'blade');
  assert.equal(state.items[0].equipped, 1);
  assert.deepEqual(state.quickslots, quickslots);
  assert.equal(inventory.set({ items: [] }).result.ok, true);
  assert.equal(inventory.rows.size, 0);
});

test('transfer moves units with their properties into matching target rows', () => {
  const source = new Inventory();
  const target = new Inventory();
  const id = source.add({ item: SWORD, amount: 2, metadata: { quality: 3 } }).result.items[0].id;
  const landing = target.add({ item: SWORD, amount: 1, metadata: { quality: 3 } }).result.items[0]
    .id;
  assert.equal(
    transfer(source, source, { units: [{ id, amount: 1 }] }).result.code,
    'sameInventory',
  );
  assert.equal(
    transfer(source, target, { units: [{ id, amount: 3 }] }).result.code,
    'insufficientItems',
  );
  const moved = transfer(source, target, { units: [{ id, amount: 1 }], targetRevision: 1 });
  assert.equal(moved.result.ok, true);
  assert.deepEqual(moved.result.items, [{ id: landing, amount: 1 }]);
  assert.equal(moved.result.revision, source.revision);
  assert.equal(source.snapshot().items[0].amount, 1);
  assert.equal(target.snapshot().items[0].amount, 2);
  assert.equal(moved.change.reason, 'transfer');
  assert.equal(moved.targetChange.reason, 'transfer');
  assert.equal(
    transfer(source, target, { units: [{ id, amount: 1 }], targetRevision: 1 }).result.code,
    'staleRevision',
  );
});

test('repeated and concurrent requests never make an item twice', () => {
  const a = new Inventory();
  const b = new Inventory();
  const id = a.add({ item: SWORD, amount: 1 }).result.items[0].id;
  const revision = a.revision;
  const first = transfer(a, b, { units: [{ id, amount: 1 }], sourceRevision: revision });
  const again = transfer(a, b, { units: [{ id, amount: 1 }], sourceRevision: revision });
  assert.equal(first.result.ok, true);
  assert.equal(again.result.ok, false);
  const total = [...a.snapshot().items, ...b.snapshot().items].reduce(
    (sum, row) => sum + row.amount,
    0,
  );
  assert.equal(total, 1);
});

test('what the game did on its own: spent, worn, gained', () => {
  const inventory = new Inventory();
  const food = itemClasses().find((entry) => entry.category === 'food' && !entry.quest);
  const bread = inventory.add({ item: food.id, amount: 3 }).result.items[0].id;
  const swords = inventory.add({ item: SWORD, amount: 2, metadata: { quality: 3 } }).result.items[0]
    .id;
  const { changes, used } = inventory.applyNative([
    { kind: 'lost', id: bread, amount: 1 },
    { kind: 'worn', id: swords, amount: 1, quality: 3, health: 0.4 },
    { kind: 'gained', item: SWORD, amount: 1, quality: 3, health: 0.4 },
    { kind: 'lost', id: 'no such row', amount: 1 },
    {
      kind: 'gained',
      item: '00000000-0000-0000-0000-000000000000',
      amount: 1,
      quality: 1,
      health: 1,
    },
  ]);
  assert.deepEqual(
    changes.map((change) => change.reason),
    ['use', 'wear', 'loot'],
  );
  assert.deepEqual(used, [{ item: food.id, kind: 'food', amount: 1 }]);
  const rows = inventory.snapshot().items;
  assert.equal(rows.find((row) => row.id === bread).amount, 2);
  assert.equal(rows.find((row) => row.id === swords).amount, 1);
  const worn = rows.find((row) => row.id !== swords && row.item === SWORD);
  assert.equal(worn.amount, 2, 'the worn sword and the picked-up one share a row');
  assert.ok(Math.abs(worn.health - 0.4) < 1e-9);
  assert.equal(inventory.revision, 5);
});

test('a mended item is a repair, and a row a change empties goes', () => {
  const inventory = new Inventory();
  const id = inventory.add({ item: SWORD, amount: 1, metadata: { quality: 2, condition: 0.2 } })
    .result.items[0].id;
  const { changes } = inventory.applyNative([
    { kind: 'worn', id, amount: 1, quality: 2, health: 0.3 },
  ]);
  assert.equal(changes[0].reason, 'repair');
  assert.equal(inventory.rows.has(id), false);
  assert.equal(inventory.snapshot().items[0].condition, 1);
  const shot = inventory.applyNative([
    { kind: 'lost', id: inventory.snapshot().items[0].id, amount: 5 },
  ]);
  assert.equal(shot.changes[0].reason, 'drop');
  assert.equal(inventory.rows.size, 0);
});

test('equip readies weapons in the selected set and ammunition with its bow, as the game does', () => {
  const inventory = new Inventory();
  const empty = { primary: null, secondary: null };
  const quickslots = {
    weapons: [empty, empty, empty, empty],
    items: [null, null, null, null],
    activeWeapon: 0,
    activeItem: 0,
  };
  assert.equal(inventory.set({ items: [], quickslots }).result.ok, true);
  inventory.takeQuickslotsRestore();
  const equip = (name) => inventory.equip(lookupItemClass(name), {}).result.items[0].id;

  const sword = equip('longSwordDuel');
  assert.deepEqual(inventory.snapshot().quickslots.weapons[0], { primary: sword, secondary: null });
  assert.ok(inventory.takeQuickslotsRestore(), 'the set goes to the game');

  const bow = equip('bow_b');
  const arrows = equip('arrow_normal');
  const sets = inventory.snapshot().quickslots.weapons;
  assert.deepEqual(sets[0], { primary: bow, secondary: arrows }, 'the bow with its arrows, selected');
  assert.deepEqual(sets.slice(1), [empty, empty, empty]);

  equip('longswordCommon');
  assert.equal(inventory.snapshot().quickslots.weapons[0].secondary, null, 'a sword takes no arrows');
});

test('equip leaves the weapon sets alone while the game has not said what they hold', () => {
  const inventory = new Inventory();
  inventory.equip(lookupItemClass('bow_b'), {});
  assert.equal(inventory.snapshot().quickslots, null);
  assert.equal(inventory.takeQuickslotsRestore(), undefined);
});

test('what the body wears: equipment changes no item and keeps the revision', () => {
  const inventory = new Inventory();
  const id = inventory.add({ item: SWORD, amount: 2 }).result.items[0].id;
  const { changes } = inventory.applyNative([{ kind: 'equipped', id, amount: 1 }]);
  assert.deepEqual(changes, [{ revision: 1, reason: 'equipment', items: [] }]);
  assert.equal(inventory.snapshot().items[0].equipped, 1);
  assert.deepEqual(inventory.applyNative([{ kind: 'equipped', id, amount: 1 }]).changes, []);
  assert.equal(inventory.applyNative([{ kind: 'equipped', id, amount: 0 }]).changes.length, 1);
  assert.equal(inventory.snapshot().items[0].equipped, 0);
  const food = itemClasses().find((entry) => entry.category === 'food' && !entry.quest);
  const bread = inventory.add({ item: food.id, amount: 1 }).result.items[0].id;
  assert.deepEqual(inventory.applyNative([{ kind: 'equipped', id: bread, amount: 1 }]).changes, []);
});

test('picked-up ammunition goes back into the quiver when that ammunition is worn', () => {
  const arrow = lookupItemClass('arrow_crude');
  const inventory = new Inventory();
  const id = inventory.add({ item: arrow.id, amount: 5 }).result.items[0].id;
  inventory.applyNative([{ kind: 'equipped', id, amount: 5 }]);
  inventory.applyNative([{ kind: 'gained', item: arrow.id, amount: 2, quality: 1, health: 1 }]);
  assert.deepEqual(
    inventory.snapshot().items.map((row) => [row.amount, row.equipped]),
    [[7, 7]],
  );
  // Ammunition the body does not wear stays off.
  inventory.applyNative([{ kind: 'equipped', id, amount: 0 }]);
  inventory.applyNative([{ kind: 'gained', item: arrow.id, amount: 1, quality: 1, health: 1 }]);
  assert.deepEqual(
    inventory.snapshot().items.map((row) => [row.amount, row.equipped]),
    [[8, 0]],
  );
});

test('itemPrice works the price out as the game does, in money units', () => {
  const { itemPrice } = require('./inventory/inventories.cjs');
  // longSwordDuel costs 36624; quality 1 tops out at 0.1 health, a factor of 0.11.
  assert.deepEqual(itemPrice('longSwordDuel'), { pristine: 4028, current: 4028 });
  // Quality 4 at full health: a factor of 1.
  assert.deepEqual(itemPrice('longSwordDuel', { quality: 4 }), { pristine: 36624, current: 36624 });
  const half = itemPrice('longSwordDuel', { quality: 4, condition: 0.5 });
  assert.equal(half.pristine, 36624);
  assert.ok(half.current < half.pristine && half.current > 0);
  assert.equal(itemPrice('apple').current, 8);
  assert.equal(itemPrice('no such item'), null);
  assert.equal(itemPrice('longSwordDuel', { quality: 9 }), null);
});

test('wear rates scale a drop in condition; a repair is never scaled', () => {
  const { WearRates } = require('./inventory/wear_rates.cjs');
  const rates = new WearRates();
  assert.deepEqual(rates.set({ default: 2, meleeWeapon: 0.5, longSwordDuel: 0 }), {
    default: 2,
    meleeWeapon: 0.5,
    longSwordDuel: 0,
  });
  assert.throws(() => rates.set({ nothingLikeIt: 1 }), TypeError);
  assert.throws(() => rates.set({ default: -1 }), RangeError);
  assert.equal(rates.of(lookupItemClass(SWORD)), 0);
  const inventory = new Inventory();
  const id = inventory.add({ item: SWORD, amount: 1 }).result.items[0].id;
  const health = () => inventory.snapshot().items[0].health;
  const worn = (value) => [
    { kind: 'worn', id: inventory.snapshot().items[0].id, amount: 1, quality: 1, health: value },
  ];
  assert.ok(id);
  // A sword worn half as fast: 0.02 off reported, 0.01 off kept, the game told.
  const start = health();
  const half = inventory.applyNative(worn(start - 0.02), () => 0.5);
  assert.equal(Math.round((start - health()) * 1e6) / 1e6, 0.01);
  assert.equal(half.corrected, true);
  // No wear at all: nothing changes, and the game is told its condition again.
  const before = health();
  const none = inventory.applyNative(worn(before - 0.02), () => 0);
  assert.equal(health(), before);
  assert.deepEqual(none.changes, []);
  assert.equal(none.corrected, true);
  // A repair goes as the game does it, whatever the rate.
  inventory.applyNative(worn(before + 0.005), () => 0);
  assert.equal(Math.round((health() - before) * 1e6) / 1e6, 0.005);
});
