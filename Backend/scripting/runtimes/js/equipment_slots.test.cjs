'use strict';

// Equipment slots: the slot of each item class (Inventory.classes) and the pieces
// Inventory.set refuses to leave equipped together.
const test = require('node:test');
const assert = require('node:assert/strict');
const { Inventory } = require('./inventory/inventories.cjs');
const { equipmentConflicts } = require('./inventory/equipment_rules.cjs');
const { itemClasses, lookupItemClass, lookupSlot } = require('./inventory/catalog.cjs');

// The knight's kit the game dresses whole in one Inventory.set: 15 pieces.
const KNIGHT_KIT = [
  'LegsPadded01_m07_C3',
  'GambesonLong01_m09_C3',
  'CollarPadded01_m08_C1',
  'CoifSmall02_m07_E1',
  'MailLong02_m01_C2',
  'Cuirass07_m03_A4',
  'ArmPlate03_m02_D1',
  'LegsPlate02_m01_B4',
  'Gauntlets05_m01_A5',
  'BascinetVisor04_m02_A5',
  'Waffenrock08_mPisek_B',
  'Spurs01_m01_C',
  'belt_4slot',
  'ring_golden_cross',
  'necklace_silver_cross_insignia',
];

const HOSE = 'HoseJoined01_m03_C';
const LEGS_PADDED = 'LegsPadded01_m07_C3';
const TUNIC = 'TunicShort05_m09_D';
const OTHER_TUNIC = 'TunicShort03_m04_D';
const GAMBESON = 'GambesonLong01_m09_C3';
const COIF_CAP = 'CoifCap01_m01_C';
const COIF_PADDED = 'CoifSmall02_m07_E1';
const CAP = 'Cap24_m02_B';
const HELMET = 'BascinetVisor04_m02_A5';
const BOOTS = 'BootsKnee01_m04_C';
const PLATE_LEGS_WITH_FEET = 'LegsPlate02_m01_B4';
const PLATE_LEGS = 'LegsPlate01_m03_C3';
const SPURS = 'Spurs01_m01_C';

function wearing(names) {
  return names.map((name) => ({ item: name, amount: 1, equipped: 1 }));
}

// What Inventory.set answers for a body that wears exactly these classes.
function setWorn(names) {
  return new Inventory().set({ items: wearing(names) }).result;
}

function conflictsOf(names) {
  return equipmentConflicts(
    names.map((name) => ({ itemClass: lookupItemClass(name), equipped: 1 })),
  );
}

function slotOfClass(name) {
  return itemClasses().find((entry) => entry.name === name).slot;
}

test('Inventory.classes gives the slot of a worn class and null for every other', () => {
  const expected = {
    [HOSE]: 'leg_trousers',
    [LEGS_PADDED]: 'leg_trousers_padded',
    [TUNIC]: 'body_cloth',
    [GAMBESON]: 'body_cloth_padded',
    [COIF_CAP]: 'head_coif',
    [COIF_PADDED]: 'head_coif_padded',
    [CAP]: 'head_cap',
    [HELMET]: 'head_helmet',
    Hood07_m04_C: 'head_hood',
    CollarPadded01_m08_C1: 'collar',
    MailLong02_m01_C2: 'body_chainmail',
    Cuirass07_m03_A4: 'body_plate',
    Waffenrock08_mPisek_B: 'body_coat',
    ArmPlate03_m02_D1: 'sleeves',
    Gauntlets05_m01_A5: 'gloves',
    [PLATE_LEGS_WITH_FEET]: 'leg_armor',
    [BOOTS]: 'boot',
    [SPURS]: 'spur',
    Cap01_mApprentice: 'belt',
    belt_4slot: 'belt',
    Pouch_test_2slot: 'pouch',
    ring_golden_cross: 'ring',
    necklace_silver_cross_insignia: 'necklace',
  };
  for (const [name, slot] of Object.entries(expected)) {
    assert.equal(slotOfClass(name), slot, name);
  }
  // A leather apron is a TunicShort that the game puts in the same slot as a tunic.
  assert.equal(slotOfClass('TunicShort05_m01_D'), 'body_cloth');
  // A horse's tack goes in the horse slots.
  assert.equal(slotOfClass('BasicBridle01_m01'), 'horse_head');
  assert.equal(slotOfClass('BasicSaddle01_m01'), 'horse_saddle');
  assert.equal(slotOfClass('NobleCaparison04_m08'), 'horse_torso');
  assert.equal(slotOfClass('horseshoeNoble'), 'horse_shoe');
  // Weapons, food and armour types no slot takes (spectacles, bells, scabbards) have none.
  for (const name of [
    'longSwordDuel',
    'apple',
    'Spectacles01_m01',
    'BootsAnkle06_m01_C',
    'prop_Scabbard_LongSword01',
  ]) {
    assert.equal(slotOfClass(name), null, name);
  }
  for (const entry of itemClasses()) {
    assert.ok(entry.slot === null || typeof entry.slot === 'string', entry.name);
  }
});

test('every slot a class has is a slot of the game, and so is every slot it needs', () => {
  const slots = new Set(itemClasses().map((entry) => entry.slot));
  slots.delete(null);
  assert.ok(slots.has('ring'));
  for (const name of slots) {
    const slot = lookupSlot(name);
    assert.ok(slot, name);
    for (const needed of [slot.requiresFilled, slot.requiresEmpty]) {
      assert.ok(needed === null || lookupSlot(needed), `${name} needs ${needed}`);
    }
  }
  assert.equal(lookupSlot('head_cap').requiresEmpty, 'head_coif_padded');
  assert.equal(lookupSlot('spur').requiresFilled, 'boot');
  assert.equal(lookupSlot('boot').requiresFilled, null);
});

test('slots that share a place on the body share a place in the table', () => {
  const pairs = [
    ['body_cloth', 'body_cloth_padded'],
    ['leg_trousers', 'leg_trousers_padded'],
    ['head_coif', 'head_coif_padded'],
    ['head_cap', 'head_helmet'],
  ];
  for (const [first, second] of pairs) {
    assert.equal(lookupSlot(first).place, lookupSlot(second).place, `${first} ${second}`);
  }
  assert.notEqual(lookupSlot('head_cap').place, lookupSlot('head_coif').place);
  assert.notEqual(lookupSlot('ring').place, lookupSlot('gloves').place);
});

test('only plate legs that cover the feet say so', () => {
  const covering = itemClasses()
    .filter((entry) => lookupItemClass(entry.id).coversFeet)
    .map((entry) => entry.slot);
  assert.ok(covering.length > 0);
  assert.ok(covering.every((slot) => slot === 'leg_armor'));
  assert.equal(lookupItemClass(PLATE_LEGS_WITH_FEET).coversFeet, true);
  assert.equal(lookupItemClass(PLATE_LEGS).coversFeet, false);
  assert.equal(lookupItemClass(BOOTS).coversFeet, false);
});

test('the knight kit of 15 pieces is accepted, in any row order', () => {
  assert.equal(KNIGHT_KIT.length, 15);
  // Every piece, the belt too, fills a slot of its own.
  const slots = KNIGHT_KIT.map(slotOfClass).filter((slot) => slot !== null);
  assert.equal(new Set(slots).size, 15);
  assert.deepEqual(conflictsOf(KNIGHT_KIT), []);
  assert.equal(setWorn(KNIGHT_KIT).ok, true);
  assert.equal(setWorn([...KNIGHT_KIT].reverse()).ok, true);
  const inventory = new Inventory();
  assert.equal(inventory.set({ items: wearing(KNIGHT_KIT) }).result.ok, true);
  assert.equal(inventory.snapshot().items.filter((row) => row.equipped > 0).length, 15);
});

test('another piece for a place the kit fills is refused, and nothing changes', () => {
  for (const extra of [HOSE, TUNIC, BOOTS]) {
    const inventory = new Inventory();
    inventory.set({ items: wearing(KNIGHT_KIT) });
    const before = inventory.snapshot();
    const refused = inventory.set({ items: wearing([...KNIGHT_KIT, extra]) });
    assert.equal(refused.result.ok, false, extra);
    assert.equal(refused.result.code, 'invalidEquipment', extra);
    assert.equal(refused.change, null, extra);
    assert.deepEqual(inventory.snapshot(), before, extra);
  }
});

test('the kit is accepted with the pieces that would conflict carried but not worn', () => {
  const rows = [
    ...wearing(KNIGHT_KIT),
    { item: HOSE, amount: 1 },
    { item: TUNIC, amount: 1, equipped: 0 },
    { item: BOOTS, amount: 1, equipped: 0 },
  ];
  assert.equal(new Inventory().set({ items: rows }).result.ok, true);
});

test('two slots with one place hold one piece', () => {
  // A helmet needs a padded coif under it, so the last set has one; the cap and the helmet then
  // still share their place.
  const pairs = [
    [TUNIC, GAMBESON],
    [HOSE, LEGS_PADDED],
    [COIF_CAP, COIF_PADDED],
    [CAP, HELMET, COIF_PADDED],
  ];
  for (const names of pairs) {
    for (const order of [names, [...names].reverse()]) {
      const sharing = conflictsOf(order).filter((conflict) => conflict.rule === 'samePlace');
      assert.equal(sharing.length, 1, order.join(' '));
      assert.equal(setWorn(order).code, 'invalidEquipment', order.join(' '));
    }
  }
  assert.deepEqual(conflictsOf([TUNIC, GAMBESON]), [
    { rule: 'samePlace', slots: ['body_cloth', 'body_cloth_padded'] },
  ]);
});

test('two pieces of one slot are one place, rings too', () => {
  assert.equal(setWorn([TUNIC, OTHER_TUNIC]).code, 'invalidEquipment');
  assert.equal(setWorn(['ring_golden_cross', 'ring_gem']).code, 'invalidEquipment');
  const threeRings = ['ring_golden_cross', 'ring_gem', 'ring_golden_large'];
  assert.equal(setWorn(threeRings).code, 'invalidEquipment');
  assert.equal(setWorn(['ring_golden_cross']).ok, true);
  const units = new Inventory().set({
    items: [{ item: HOSE, amount: 2, equipped: 2 }],
  });
  assert.equal(units.result.code, 'invalidEquipment');
  const oneOfTwo = new Inventory().set({ items: [{ item: HOSE, amount: 2, equipped: 1 }] });
  assert.equal(oneOfTwo.result.ok, true);
});

test('pieces for different places are worn together', () => {
  const together = [
    [TUNIC, 'Waffenrock08_mPisek_B', HOSE, 'Shoes03_m06_C'],
    [CAP, 'Hood07_m04_C', COIF_CAP],
    [TUNIC, 'CollarPadded01_m08_C1', 'Gauntlets05_m01_A5', 'necklace_silver_cross_insignia'],
    [LEGS_PADDED, PLATE_LEGS, BOOTS, SPURS],
  ];
  for (const names of together) {
    assert.equal(setWorn(names).ok, true, names.join(' '));
  }
});

test('a piece that goes over another is refused while the other is missing', () => {
  const missing = [
    [HELMET, 'head_coif_padded'],
    ['MailLong02_m01_C2', 'body_cloth_padded'],
    ['Cuirass07_m03_A4', 'body_cloth_padded'],
    ['ArmPlate03_m02_D1', 'body_cloth_padded'],
    [PLATE_LEGS, 'leg_trousers_padded'],
    [SPURS, 'boot'],
  ];
  for (const [piece, needs] of missing) {
    assert.deepEqual(
      conflictsOf([piece]),
      [{ rule: 'needsFilled', slot: slotOfClass(piece), needs }],
      piece,
    );
    assert.equal(setWorn([piece]).code, 'invalidEquipment', piece);
  }
  // The wrong layer underneath does not do: a tunic is not a gambeson.
  assert.equal(setWorn([TUNIC, 'MailLong02_m01_C2']).code, 'invalidEquipment');
  assert.equal(setWorn([GAMBESON, 'MailLong02_m01_C2']).ok, true);
  assert.equal(setWorn([COIF_PADDED, HELMET]).ok, true);
  assert.equal(setWorn([BOOTS, SPURS]).ok, true);
  assert.equal(setWorn([LEGS_PADDED, PLATE_LEGS]).ok, true);
});

test('a cap is refused over a padded coif', () => {
  assert.deepEqual(conflictsOf([COIF_PADDED, CAP]), [
    { rule: 'needsEmpty', slot: 'head_cap', needs: 'head_coif_padded' },
  ]);
  assert.equal(setWorn([COIF_PADDED, CAP]).code, 'invalidEquipment');
  assert.equal(setWorn([COIF_CAP, CAP]).ok, true);
  assert.equal(setWorn([CAP]).ok, true);
});

test('plate legs with feet leave no room for boots but take spurs', () => {
  assert.deepEqual(conflictsOf([LEGS_PADDED, PLATE_LEGS_WITH_FEET, BOOTS]), [
    { rule: 'feet', slot: 'boot' },
  ]);
  assert.equal(setWorn([LEGS_PADDED, PLATE_LEGS_WITH_FEET, BOOTS]).code, 'invalidEquipment');
  assert.equal(setWorn([BOOTS, LEGS_PADDED, PLATE_LEGS_WITH_FEET]).code, 'invalidEquipment');
  // Spurs still go on: the plate legs fill the boot slot for them.
  assert.equal(setWorn([LEGS_PADDED, PLATE_LEGS_WITH_FEET, SPURS]).ok, true);
  assert.deepEqual(conflictsOf([LEGS_PADDED, PLATE_LEGS_WITH_FEET, SPURS]), []);
  // Plate legs without feet leave the feet to boots.
  assert.equal(setWorn([LEGS_PADDED, PLATE_LEGS, BOOTS]).ok, true);
  assert.equal(setWorn([LEGS_PADDED, PLATE_LEGS, SPURS]).code, 'invalidEquipment');
});

test('weapons, ammunition and classes without a slot are not pieces; a belt is', () => {
  const sword = 'longSwordDuel';
  const ammo = itemClasses().find((entry) => entry.category === 'ammo');
  const rows = [
    ...wearing(KNIGHT_KIT),
    { item: sword, amount: 2, equipped: 2 },
    { item: ammo.id, amount: 20, equipped: 20 },
    { item: 'Spectacles01_m01', amount: 2, equipped: 1 },
  ];
  assert.equal(new Inventory().set({ items: rows }).result.ok, true);
  assert.equal(setWorn([sword, 'belt_4slot', 'Spectacles01_m01']).ok, true);
  // One belt and one pouch: a second belt is refused, a pouch beside the belt is not.
  assert.equal(setWorn(['belt_4slot', 'belt_3slot']).code, 'invalidEquipment');
  assert.equal(setWorn(['belt_4slot', 'Pouch_test_2slot']).ok, true);
  // And they fill no slot: a belt does not stand for the boots spurs need.
  assert.equal(setWorn([SPURS, 'belt_4slot']).code, 'invalidEquipment');
});

test('the checks of Inventory.set that came before still refuse first', () => {
  const food = itemClasses().find((entry) => entry.category === 'food' && !entry.quest);
  const inventory = new Inventory();
  assert.equal(
    inventory.set({ items: [{ item: food.id, amount: 1, equipped: 1 }] }).result.code,
    'invalidEquipment',
  );
  assert.equal(
    inventory.set({ items: [{ item: HOSE, amount: 1, equipped: 2 }] }).result.code,
    'invalidEquipment',
  );
  assert.equal(
    inventory.set({ items: wearing(KNIGHT_KIT), revision: 99 }).result.code,
    'staleRevision',
  );
});
