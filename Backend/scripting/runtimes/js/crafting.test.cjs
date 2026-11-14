'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { CraftingSessions, parseStation, STATIONS } = require('./crafting/crafting.cjs');
const recipes = require('./recipes/recipes.cjs');

// Crafting.stations, Crafting.occupancy and Crafting.session, and craftingStarting,
// craftingStarted and craftingEnded.

const TACHOV_SMITHY = 'trosecko/445fca90-c85a-4808';
const TACHOV = { x: 1997.12, y: 2498.7451, z: 115.0716 };
const TROSKOVICE_TABLE = 'trosecko/2229e312-da70-017e';
const TROSKOVICE = { x: 2299.5569, y: 2016.377, z: 106.4078 };

// Sessions whose events and verdicts are written down; `refuse` makes craftingStarting refuse.
function crafting({ refuse = false } = {}) {
  const events = [];
  const verdicts = [];
  const sessions = new CraftingSessions({
    emit: (name, player, payload) => {
      events.push({ name, player: player.id, payload });
      return !(refuse && name === 'craftingStarting');
    },
    sendVerdict: (player, request, allowed) =>
      verdicts.push({ player: player.id, request, allowed }),
  });
  const noCustom = {
    stationInfos: () => [],
    activeSession: () => null,
    occupancyOf: () => null,
    cancelFor: () => false,
  };
  return { sessions, events, verdicts, Crafting: sessions.define((player) => player, noCustom) };
}

test('Crafting.stations: every smithy and alchemy table of the levels', () => {
  const { Crafting } = crafting();
  const smithies = Crafting.stations('smithing');
  const tables = Crafting.stations('alchemy');
  assert.ok(smithies.length >= 20 && tables.length >= 15);
  assert.equal(Crafting.stations().length, smithies.length + tables.length);
  const tachov = smithies.find((station) => station.id === TACHOV_SMITHY);
  assert.deepEqual(tachov.position, { x: 1997.12, y: 2498.745, z: 115.072 });
  assert.equal(tachov.level, 'trosecko');
  assert.match(tachov.layer, /tachov/);
  assert.ok(Math.abs(Math.hypot(tachov.forward.x, tachov.forward.y) - 1) < 1e-3);
  tachov.position.x = 0;
  assert.equal(
    Crafting.stations('smithing').find((s) => s.id === TACHOV_SMITHY).position.x,
    1997.12,
  );
  assert.throws(() => Crafting.stations('cooking'), TypeError);
  assert.equal(new Set(STATIONS.map((station) => station.id)).size, STATIONS.length);
});

test('Crafting.recipes: the game recipes with their native keys, materials and products', () => {
  const { Crafting } = crafting();
  const player = { id: 1 };
  const smithing = Crafting.recipes(player, 'smithing');
  const alchemy = Crafting.recipes(player, 'alchemy');
  assert.equal(smithing.length, 61);
  assert.equal(alchemy.length, 31);
  assert.equal(Crafting.recipes(player).length, 92);
  const horseshoe = smithing.find((recipe) => recipe.key.id === 'r_horseshoe');
  assert.deepEqual(horseshoe, {
    key: { kind: 'smithing', id: 'r_horseshoe' },
    name: 'ui_nm_horseshoeFarmer',
    dlcId: 0,
    minSkill: 1,
    perk: '3190f739-995e-4c60-9aa7-d34ca3428494',
    base: '',
    ingredients: [
      {
        item: '94d8f5f1-20a7-4840-98ad-1d198d381389',
        name: 'bsmt_ironBad',
        amount: 1,
        quest: false,
      },
    ],
    products: [{ item: '549ab26e-df73-43d6-ac9b-f4f74afec67f', threshold: 40, maxYield: 1 }],
    source: 'game',
  });
  assert.ok(Object.isFrozen(horseshoe) && Object.isFrozen(horseshoe.ingredients[0]));
  const potion = alchemy.find((recipe) => recipe.key.id === 1);
  assert.deepEqual(potion.key, { kind: 'alchemy', id: 1 });
  assert.equal(potion.base, 'Water');
  assert.equal(potion.perk, '');
  assert.equal(potion.minSkill, 0);
  assert.deepEqual(potion.products[0], {
    item: '7a1e8447-4449-473b-aac9-63f7a324fa0b',
    threshold: 1,
    maxYield: 2,
  });
  assert.ok(smithing.some((recipe) => recipe.dlcId > 0));
  assert.throws(() => Crafting.recipes(player, 'cooking'), TypeError);
});

test('a session runs from the station field to its end, with its events', () => {
  const { sessions, events, Crafting } = crafting();
  const player = { id: 7 };
  assert.equal(Crafting.isAvailable(), false);
  assert.equal(Crafting.session(player), null);
  assert.deepEqual(Crafting.occupancy(TACHOV_SMITHY), {
    station: TACHOV_SMITHY,
    dimension: 4294967295,
    occupied: false,
    occupant: null,
  });
  // A smithy held: no workpiece yet, so no event.
  sessions.observe(player, 'smith S1997.1200,2498.7451,115.0716', 0, 1000);
  const session = sessions.activeSession(7, 61000);
  assert.equal(session.kind, 'smithing');
  assert.equal(session.station, TACHOV_SMITHY);
  assert.equal(session.phase, 'working');
  assert.equal(session.expiresInMs, 30 * 60 * 1000 - 60000);
  assert.deepEqual(events, []);
  const occupancy = sessions.occupancyOf(TACHOV_SMITHY, 0, 61000);
  assert.equal(occupancy.occupant.playerId, 7);
  assert.equal(occupancy.occupant.entryId, session.id);
  assert.equal(Crafting.occupancy(TACHOV_SMITHY, 3).occupied, false);
  // The same station, reported again or by the game's own report, keeps its session.
  sessions.observe(player, 'smith S1997.1300,2498.7451,115.0716', 0, 2000);
  sessions.report(player, { report: 'started', kind: 'smithing', station: TACHOV }, 0);
  assert.equal(Crafting.session(player).id, session.id);
  // An alchemy table: its first batch starts at once.
  sessions.observe(player, 'alchemy S2299.5569,2016.3770,106.4078', 0, 3000);
  assert.equal(Crafting.session(player).station, TROSKOVICE_TABLE);
  assert.equal(Crafting.session(player).phase, 'brewing');
  assert.deepEqual(
    events.map((e) => [e.name, e.payload.station]),
    [['craftingStarted', TROSKOVICE_TABLE]],
  );
  // The game's own end names why; the field emptying after it changes nothing.
  sessions.report(player, { report: 'ended', kind: 'alchemy', reason: 'interrupted' }, 0);
  sessions.observe(player, '', 0, 4000);
  // It settles once the game had a moment to give materials back.
  assert.equal(events.length, 1);
  sessions.tick(Date.now() + 5000);
  assert.equal(events[1].name, 'craftingRefunding');
  assert.equal(events[1].payload.outcome, 'cancelled');
  assert.deepEqual(events[1].payload.materials, []);
  const ended = events.at(-1);
  assert.equal(ended.name, 'craftingEnded');
  assert.deepEqual(ended.payload, {
    kind: 'alchemy',
    session: events[0].payload.session,
    station: TROSKOVICE_TABLE,
    dimension: 0,
    outcome: 'cancelled',
    reason: 'interrupted',
    refunded: [],
  });
  assert.equal(events.length, 3);
  assert.equal(Crafting.session(player), null);
  // A station the catalog does not know is no session; leaving ends a batch as disconnected.
  sessions.observe(player, 'alchemy S0,0,0', 0, 5000);
  assert.equal(Crafting.session(player), null);
  sessions.observe(player, 'alchemy S2299.5569,2016.3770,106.4078', 0, 6000);
  sessions.left(player);
  assert.equal(events.at(-1).payload.reason, 'disconnected');
  assert.equal(Crafting.occupancy('nowhere/0'), null);
  assert.throws(() => Crafting.occupancy(TACHOV_SMITHY, -1), TypeError);
});

// An inventory change as the server's inventory raises it.
function change(reason, item, amount, rowId = 'row') {
  const row = { id: rowId, item, amount: Math.abs(amount), quality: 1 };
  return amount < 0
    ? { revision: 1, reason, items: [{ id: rowId, before: row, after: null }] }
    : { revision: 1, reason, items: [{ id: rowId, before: null, after: row }] };
}

test("a product the run's materials cannot have made is taken back", () => {
  const removed = [];
  const events = [];
  const sessions = new CraftingSessions({
    emit: (name, player, payload) => {
      events.push({ name, payload });
      return true;
    },
    sendVerdict: () => {},
    give: (player, units) => units.map((unit, index) => ({ id: `g${index}`, amount: unit.amount })),
    remove: (player, units) => {
      removed.push(...units);
      return true;
    },
  });
  const player = { id: 7 };
  const all = recipes.define();
  const horseshoe = all.get('r_horseshoe');
  // A recipe one horseshoe's iron cannot make (it takes more, or other materials).
  const sword = all
    .all('smithing')
    .find((recipe) =>
      recipe.ingredients.some(
        (ingredient) => ingredient.item !== horseshoe.ingredients[0].item || ingredient.amount > 1,
      ),
    );
  const ended = () => events.filter((e) => e.name === 'craftingEnded').at(-1).payload;
  // A horseshoe's iron, and the game says a different recipe's product came of it.
  sessions.observe(player, 'smith S1997.1200,2498.7451,115.0716', 0, 1000);
  sessions.inventoryChanged(player, change('drop', horseshoe.ingredients[0].item, -1, 'iron'));
  sessions.inventoryChanged(player, change('loot', sword.products[0].item, 1, 'blade'));
  assert.deepEqual([ended().outcome, ended().reason], ['failed', 'clientError']);
  assert.deepEqual(removed, [{ id: 'blade', amount: 1 }]);
  assert.equal(events.filter((e) => e.name === 'craftingCompleted').length, 0);
  // More potions than the recipe yields, and a potion with no herbs spent.
  const potion = all.all('alchemy')[0];
  const brew = new CraftingSessions({
    emit: (name, who, payload) => {
      events.push({ name, payload });
      return true;
    },
    sendVerdict: () => {},
    give: () => [],
    remove: (who, units) => {
      removed.push(...units);
      return true;
    },
  });
  brew.observe(player, 'alchemy S2299.5569,2016.3770,106.4078', 0, 1000);
  for (const ingredient of potion.ingredients) {
    brew.inventoryChanged(player, change('drop', ingredient.item, -ingredient.amount));
  }
  const most = potion.products[0].maxYield;
  brew.inventoryChanged(player, change('loot', potion.products[0].item, most + 1, 'many'));
  assert.deepEqual([ended().outcome, ended().reason], ['failed', 'clientError']);
  brew.inventoryChanged(player, change('loot', potion.products[0].item, 1, 'free'));
  assert.deepEqual([ended().outcome, ended().reason], ['failed', 'clientError']);
  assert.deepEqual(
    removed.slice(1).map((unit) => unit.id),
    ['many', 'free'],
  );
});

test('a workpiece: its materials start it, its product completes it', () => {
  const { sessions, events, Crafting } = crafting();
  const player = { id: 7 };
  const horseshoe = recipes.define().get('r_horseshoe');
  const iron = horseshoe.ingredients[0].item;
  const product = horseshoe.products[0].item;
  sessions.observe(player, 'smith S1997.1200,2498.7451,115.0716', 0, 1000);
  // A resource's own change does not count; the game's drop of the materials does.
  sessions.inventoryChanged(player, change('script', iron, -1));
  assert.equal(events.length, 0);
  sessions.inventoryChanged(player, change('drop', iron, -horseshoe.ingredients[0].amount));
  assert.equal(events.length, 1);
  assert.equal(events[0].name, 'craftingStarted');
  const workpiece = events[0].payload.session;
  assert.deepEqual(Crafting.session(player).materials, [
    { item: iron, amount: horseshoe.ingredients[0].amount },
  ]);
  sessions.inventoryChanged(player, change('loot', product, 1, 'shoe'));
  assert.deepEqual(
    events.slice(1).map((e) => e.name),
    ['craftingCompleting', 'craftingCompleted', 'craftingEnded'],
  );
  assert.deepEqual(events[1].payload, {
    kind: 'smithing',
    session: workpiece,
    station: TACHOV_SMITHY,
    outcome: 'success',
    recipe: 'r_horseshoe',
    grade: 1,
    quality: 1,
    // The game's BlacksmithingFinishingXP.
    xp: 135,
    lockpicks: 0,
    product,
    amount: 1,
  });
  const completed = events[2].payload;
  assert.equal(completed.session, workpiece);
  assert.equal(completed.recipe, 'r_horseshoe');
  assert.equal(completed.outcome, 'success');
  // A horseshoe has one quality tier.
  assert.equal(completed.grade, 1);
  assert.equal(completed.quality, 1);
  assert.deepEqual(completed.outputs, [{ id: 'shoe', amount: 1 }]);
  assert.equal(events[3].payload.outcome, 'success');
  assert.equal(events[3].payload.reason, '');
  // Held still, nothing running: leaving raises nothing more.
  assert.equal(Crafting.session(player).materials, undefined);
  sessions.observe(player, '', 0, 2000);
  sessions.tick(Date.now() + 5000);
  assert.equal(events.length, 4);
});

test('grindstoneStarting decides whether a player sharpens; a grindstone keeps no session', () => {
  const allowed = crafting();
  const player = { id: 7 };
  const station = { x: 1995.5, y: 2501.25, z: 115 };
  allowed.sessions.report(player, { report: 'ask', request: 4, kind: 'grinding', station }, 0);
  assert.deepEqual(allowed.verdicts, [{ player: 7, request: 4, allowed: true }]);
  assert.equal(allowed.events.length, 1);
  assert.equal(allowed.events[0].name, 'grindstoneStarting');
  assert.deepEqual(allowed.events[0].payload, { position: station, dimension: 0 });
  assert.ok(Object.isFrozen(allowed.events[0].payload));
  assert.equal(allowed.Crafting.session(player), null);
  // A refusal: the game does not start the sharpening.
  const refused = crafting();
  refused.sessions.events.emit = (name, who, payload) => {
    refused.events.push({ name, player: who.id, payload });
    return name !== 'grindstoneStarting';
  };
  refused.sessions.report(player, { report: 'ask', request: 5, kind: 'grinding', station }, 0);
  assert.deepEqual(refused.verdicts, [{ player: 7, request: 5, allowed: false }]);
  // Only an ask is taken from a grindstone.
  refused.sessions.report(player, { report: 'ended', kind: 'grinding', station }, 0);
  assert.equal(refused.events.length, 1);
});

test('alchemy: a potion ends a batch and the next one follows it', () => {
  const { sessions, events } = crafting();
  const player = { id: 7 };
  const potion = recipes.define().all('alchemy')[0];
  sessions.observe(player, 'alchemy S2299.5569,2016.3770,106.4078', 0, 1000);
  const first = events[0].payload.session;
  // The brewer puts the recipe's herbs on the table (the game drops them from the inventory).
  const herbs = () => {
    for (const ingredient of potion.ingredients) {
      sessions.inventoryChanged(player, change('drop', ingredient.item, -ingredient.amount));
    }
  };
  herbs();
  sessions.inventoryChanged(player, change('loot', potion.products[0].item, 2, 'potions'));
  assert.deepEqual(
    events.map((e) => e.name),
    [
      'craftingStarted',
      'craftingCompleting',
      'craftingCompleted',
      'craftingEnded',
      'craftingStarted',
    ],
  );
  assert.equal(events[2].payload.recipe, potion.id);
  assert.equal(events[2].payload.lockpicks, undefined);
  assert.equal(events[2].payload.xp, potion.xp);
  assert.ok(potion.xp > 0);
  assert.equal(events[4].payload.continuationOf, first);
  // The kept table asked about again: craftingStarting follows the last batch.
  const second = events[4].payload.session;
  herbs();
  sessions.inventoryChanged(player, change('loot', potion.products[0].item, 2, 'potions'));
  sessions.report(player, { report: 'ask', request: 9, kind: 'alchemy', station: TROSKOVICE }, 0);
  const asked = events.filter((e) => e.name === 'craftingStarting').at(-1);
  assert.equal(asked.payload.continuationOf, second);
  // Another world's table is a fresh entry.
  sessions.report(player, { report: 'ask', request: 10, kind: 'alchemy', station: TROSKOVICE }, 3);
  assert.equal(
    events.filter((e) => e.name === 'craftingStarting').at(-1).payload.continuationOf,
    '',
  );
});

test("alchemy: the game's failed potion ends a batch as failed, with no recipe or XP", () => {
  const { sessions, events } = crafting();
  const player = { id: 7 };
  sessions.observe(player, 'alchemy S2299.5569,2016.3770,106.4078', 0, 1000);
  const failedPotion = '006ec8d6-1ce1-4e90-8267-7c349812ddcd';
  sessions.inventoryChanged(player, change('loot', failedPotion, 1, 'ruined'));
  assert.deepEqual(
    events.map((e) => e.name),
    [
      'craftingStarted',
      'craftingCompleting',
      'craftingCompleted',
      'craftingEnded',
      'craftingStarted',
    ],
  );
  const [, completing, completed, ended] = events;
  assert.deepEqual(
    [completing.payload.outcome, completing.payload.recipe, completing.payload.product],
    ['failed', '', failedPotion],
  );
  assert.deepEqual([completed.payload.outcome, completed.payload.xp], ['failed', 0]);
  assert.deepEqual([ended.payload.outcome, ended.payload.reason], ['failed', '']);
  // At a smithy the failed potion is nothing to the workpiece.
  const smith = crafting();
  smith.sessions.observe(player, 'smithing S1997.1200,2498.7451,115.0716', 0, 1000);
  smith.sessions.inventoryChanged(player, change('loot', failedPotion, 1, 'ruined'));
  assert.ok(!smith.events.some((e) => e.name === 'craftingCompleted'));
});

test('alchemy: the session lists what lies on the table and counts its changes', () => {
  const { sessions, Crafting } = crafting();
  const player = { id: 7 };
  const herb = '2264f217-590e-4c0f-a4c6-f50c6532b9f6';
  const water = '00000000-0000-0000-0000-0000000000aa';
  sessions.observe(player, 'alchemy S2299.5569,2016.3770,106.4078', 0, 1000);
  assert.deepEqual(
    [Crafting.session(player).sequence, Crafting.session(player).resources],
    [0, []],
  );
  // The game takes the herb from a row, then the client reports the table.
  sessions.inventoryChanged(player, change('drop', herb, -1, 'herbs'));
  sessions.table(player, [
    [6, water, true, false, false],
    [2, herb, false, false, false],
  ]);
  let session = Crafting.session(player);
  assert.deepEqual(
    session.resources.map((r) => [r.id, r.position, r.item, r.itemId, r.base, r.milled]),
    [
      [1, 6, water, '', true, false],
      [2, 2, herb, 'herbs', false, false],
    ],
  );
  assert.equal(session.sequence, 2);
  // The herb is milled where it lies: the same id, one more step; the same table again is none.
  sessions.table(player, [
    [6, water, true, false, false],
    [2, herb, false, true, false],
  ]);
  sessions.table(player, [
    [6, water, true, false, false],
    [2, herb, false, true, false],
  ]);
  session = Crafting.session(player);
  assert.deepEqual(
    session.resources.map((r) => [r.id, r.milled]),
    [
      [1, false],
      [2, true],
    ],
  );
  assert.equal(session.sequence, 3);
  // The herb goes into the pot (another position): it went and came, two steps.
  sessions.table(player, [
    [6, water, true, false, false],
    [7, herb, false, true, false],
  ]);
  assert.equal(Crafting.session(player).sequence, 5);
});

// Sessions whose inventory orders are written down; `handlers` answer events by name.
function settling(handlers = {}, random = () => 0.9) {
  const events = [];
  const given = [];
  const removed = [];
  let next = 1;
  const sessions = new CraftingSessions(
    {
      emit: (name, player, payload, refund) => {
        events.push({ name, payload });
        return handlers[name] ? handlers[name](payload, refund) !== false : true;
      },
      give: (player, units) => {
        given.push(...units);
        return units.map((unit) => ({ id: `back${next++}`, amount: unit.amount }));
      },
      remove: (player, units) => {
        removed.push(...units);
        return true;
      },
    },
    random,
  );
  return { sessions, events, given, removed };
}

// The game's drop of units, with their properties.
function dropped(item, amount, rowId, metadata = { quality: 2 }) {
  const before = { id: rowId, item, amount: 5, quality: 2, metadata };
  const after = amount === 5 ? null : { ...before, amount: 5 - amount };
  return { revision: 1, reason: 'drop', items: [{ id: rowId, before, after }] };
}

test('a refused workpiece: its product goes back out, a failed workpiece keeps its share', () => {
  const { sessions, events, given, removed } = settling({ craftingCompleting: () => false });
  const player = { id: 7 };
  const sword = recipes.define().get('basic_sword');
  const [steel, leather] = sword.ingredients;
  sessions.observe(player, 'smith S1997.1200,2498.7451,115.0716', 0, 1000);
  sessions.inventoryChanged(player, dropped(steel.item, 5, 'steel'));
  sessions.inventoryChanged(player, dropped(leather.item, 2, 'leather'));
  const lockpick = '8d76f58e-a521-4205-a7e8-9ac077eee5f0';
  sessions.inventoryChanged(player, {
    revision: 2,
    reason: 'loot',
    items: [
      {
        id: 'blade',
        before: null,
        after: { id: 'blade', item: sword.products[0].item, amount: 1 },
      },
      { id: 'picks', before: null, after: { id: 'picks', item: lockpick, amount: 2 } },
    ],
  });
  const completing = events.find((e) => e.name === 'craftingCompleting').payload;
  assert.equal(completing.recipe, 'basic_sword');
  assert.equal(completing.lockpicks, 2);
  assert.deepEqual(removed, [
    { id: 'blade', amount: 1 },
    { id: 'picks', amount: 2 },
  ]);
  // Five units: two spent, three back with their properties; two units: one spent, one back.
  assert.deepEqual(given, [
    { item: steel.item, amount: 3, metadata: { quality: 2 } },
    { item: leather.item, amount: 1, metadata: { quality: 2 } },
  ]);
  const ended = events.at(-1);
  assert.equal(ended.name, 'craftingEnded');
  assert.equal(ended.payload.outcome, 'failed');
  assert.equal(ended.payload.reason, 'craftingCompletingRejected');
  assert.equal(ended.payload.refunded.length, 2);
  assert.equal(events.filter((e) => e.name === 'craftingCompleted').length, 0);
});

test('an unfinished workpiece settles after the game gave back its share; refund() the rest', () => {
  let late = null;
  const { sessions, events, given } = settling({
    craftingRefunding: (proposal, refund) => {
      late = refund;
      refund();
    },
  });
  const player = { id: 7 };
  const sword = recipes.define().get('basic_sword');
  const [steel] = sword.ingredients;
  sessions.observe(player, 'smith S1997.1200,2498.7451,115.0716', 0, 1000);
  sessions.inventoryChanged(player, dropped(steel.item, 5, 'steel'));
  sessions.report(player, { report: 'ended', kind: 'smithing', reason: 'cancelled' }, 0);
  // The game gives two units back while the workpiece waits to settle.
  sessions.inventoryChanged(player, {
    revision: 3,
    reason: 'loot',
    items: [
      {
        id: 'steel',
        before: { id: 'steel', item: steel.item, amount: 0 },
        after: { id: 'steel', item: steel.item, amount: 2 },
      },
    ],
  });
  assert.equal(events.at(-1).name, 'craftingStarted');
  sessions.tick(Date.now() + 5000);
  const refunding = events.find((e) => e.name === 'craftingRefunding').payload;
  assert.equal(refunding.outcome, 'cancelled');
  assert.equal(refunding.reason, 'cancelled');
  assert.deepEqual(refunding.materials, [
    { item: steel.item, amount: 3, metadata: { quality: 2 } },
  ]);
  assert.ok(Object.isFrozen(refunding) && Object.isFrozen(refunding.materials));
  assert.deepEqual(given, [{ item: steel.item, amount: 3, metadata: { quality: 2 } }]);
  const ended = events.at(-1).payload;
  assert.equal(ended.outcome, 'cancelled');
  assert.deepEqual(ended.refunded, [
    { id: 'steel', amount: 2 },
    { id: 'back1', amount: 3 },
  ]);
  // The callback expired on return.
  late();
  assert.equal(given.length, 1);
});

test('a broken workpiece ends failed and settles; the smith keeps the smithy', () => {
  const { sessions, events } = settling({});
  const player = { id: 7 };
  const sword = recipes.define().get('basic_sword');
  const [steel] = sword.ingredients;
  sessions.observe(player, 'smith S1997.1200,2498.7451,115.0716', 0, 1000);
  sessions.inventoryChanged(player, dropped(steel.item, 5, 'steel'));
  assert.equal(events.at(-1).name, 'craftingStarted');
  // Only the smith's own workpiece can break.
  sessions.report(player, { report: 'failed', kind: 'alchemy' }, 0);
  assert.equal(events.at(-1).name, 'craftingStarted');
  sessions.report(player, { report: 'failed', kind: 'smithing' }, 0);
  sessions.tick(Date.now() + 5000);
  const refunding = events.find((e) => e.name === 'craftingRefunding').payload;
  assert.equal(refunding.outcome, 'failed');
  assert.equal(refunding.reason, 'craftingFailed');
  const ended = events.at(-1);
  assert.equal(ended.name, 'craftingEnded');
  assert.equal(ended.payload.outcome, 'failed');
  assert.equal(ended.payload.reason, 'craftingFailed');
  assert.ok(sessions.sessions.has(player.id), 'the smithy is still held');
  // A second break with no workpiece running says nothing.
  const count = events.length;
  sessions.report(player, { report: 'failed', kind: 'smithing' }, 0);
  assert.equal(events.length, count);
});

test('without refund() the game rules stand; a leaving player settles at once', () => {
  const { sessions, events, given } = settling();
  const player = { id: 7 };
  const [steel] = recipes.define().get('basic_sword').ingredients;
  sessions.observe(player, 'smith S1997.1200,2498.7451,115.0716', 0, 1000);
  sessions.inventoryChanged(player, dropped(steel.item, 5, 'steel'));
  sessions.left(player);
  assert.deepEqual(
    events.map((e) => e.name),
    ['craftingStarted', 'craftingRefunding', 'craftingEnded'],
  );
  assert.equal(events[2].payload.reason, 'disconnected');
  assert.deepEqual(events[2].payload.refunded, []);
  assert.deepEqual(given, []);
});

test('Crafting.cancel closes a game station: the game is told, the session settles', () => {
  const { sessions, events, verdicts, Crafting } = crafting();
  const player = { id: 7 };
  assert.equal(Crafting.cancel(player), false);
  sessions.observe(player, 'alchemy S2299.5569,2016.3770,106.4078', 0, 1000);
  assert.equal(Crafting.cancel(player), true);
  assert.deepEqual(verdicts, [{ player: 7, request: 0, allowed: false }]);
  assert.equal(Crafting.session(player), null);
  // The field still shows the table until the game has closed it: no new session meanwhile.
  sessions.observe(player, 'alchemy S2299.5569,2016.3770,106.4078', 0, 1500);
  assert.equal(Crafting.session(player), null);
  sessions.tick(Date.now() + 5000);
  assert.deepEqual(
    events.map((e) => e.name),
    ['craftingStarted', 'craftingRefunding', 'craftingEnded'],
  );
  assert.equal(events[2].payload.reason, 'cancelled');
  // Once the field emptied, the table can be used again.
  sessions.observe(player, '', 0, 2000);
  sessions.observe(player, 'alchemy S2299.5569,2016.3770,106.4078', 0, 3000);
  assert.equal(Crafting.session(player).station, TROSKOVICE_TABLE);
});

test('an ask: a held station is refused, then craftingStarting decides', () => {
  const { sessions, events, verdicts } = crafting();
  const smith = { id: 7 };
  const other = { id: 8 };
  // A smithy is asked about with its recipe, later: opening one only checks who holds it.
  sessions.report(smith, { report: 'ask', request: 1, kind: 'smithing', station: TACHOV }, 0);
  assert.deepEqual(verdicts, [{ player: 7, request: 1, allowed: true }]);
  assert.deepEqual(events, []);
  sessions.report(smith, { report: 'started', kind: 'smithing', station: TACHOV }, 0);
  // Another player at the same smithy in the same world is refused.
  sessions.report(other, { report: 'ask', request: 2, kind: 'smithing', station: TACHOV }, 0);
  assert.deepEqual(verdicts[1], { player: 8, request: 2, allowed: false });
  // In another world it is free.
  sessions.report(other, { report: 'ask', request: 3, kind: 'smithing', station: TACHOV }, 5);
  assert.deepEqual(verdicts[2], { player: 8, request: 3, allowed: true });
  // An alchemy table is asked about as it opens.
  sessions.report(other, { report: 'ask', request: 4, kind: 'alchemy', station: TROSKOVICE }, 0);
  assert.deepEqual(events[0], {
    name: 'craftingStarting',
    player: 8,
    payload: {
      kind: 'alchemy',
      station: TROSKOVICE_TABLE,
      dimension: 0,
      recipe: undefined,
      continuationOf: '',
    },
  });
  assert.ok(Object.isFrozen(events[0].payload));

  const refusing = crafting({ refuse: true });
  refusing.sessions.report(
    other,
    { report: 'ask', request: 5, kind: 'alchemy', station: TROSKOVICE },
    0,
  );
  assert.deepEqual(refusing.verdicts, [{ player: 8, request: 5, allowed: false }]);
});

test('a smithing recipe chosen: craftingStarting with it, then the workpiece or a closed smithy', () => {
  const { sessions, events, Crafting } = crafting();
  const smith = { id: 7 };
  sessions.observe(smith, 'smith S1997.1200,2498.7451,115.0716', 0, 1000);
  sessions.report(smith, { report: 'recipe', kind: 'smithing', reason: 'no_such_recipe' }, 0);
  assert.deepEqual(events, []);
  sessions.report(smith, { report: 'recipe', kind: 'smithing', reason: 'r_horseshoe' }, 0);
  assert.deepEqual(
    events.map((e) => [e.name, e.payload.recipe]),
    [
      ['craftingStarting', 'r_horseshoe'],
      ['craftingStarted', 'r_horseshoe'],
    ],
  );
  assert.equal(events[0].payload.station, TACHOV_SMITHY);
  assert.equal(Crafting.session(smith).recipe, 'r_horseshoe');

  const refusing = crafting({ refuse: true });
  refusing.sessions.observe(smith, 'smith S1997.1200,2498.7451,115.0716', 0, 1000);
  refusing.sessions.report(smith, { report: 'recipe', kind: 'smithing', reason: 'r_horseshoe' }, 0);
  // Refused: the smithy closes and nothing started.
  assert.deepEqual(refusing.verdicts, [{ player: 7, request: 0, allowed: false }]);
  assert.deepEqual(
    refusing.events.map((e) => e.name),
    ['craftingStarting'],
  );
  assert.equal(refusing.Crafting.session(smith), null);
  refusing.sessions.observe(smith, 'smith S1997.1200,2498.7451,115.0716', 0, 1500);
  assert.equal(refusing.Crafting.session(smith), null);
});

test('parseStation reads the field kind and station words', () => {
  assert.deepEqual(parseStation('smith S1,2,3'), {
    kind: 'smithing',
    position: { x: 1, y: 2, z: 3 },
  });
  assert.equal(parseStation(''), null);
  assert.equal(parseStation('bake S1,2,3'), null);
  assert.equal(parseStation('smith S1,2'), null);
  assert.equal(parseStation(undefined), null);
});

test('the host follows reports, raises the events and answers an ask', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcd2s-crafting-'));
  const dir = path.join(root, 'workshop');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'workshop', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(
    path.join(dir, 'index.js'),
    [
      "kcds.core.Events.on('craftingStarting', (player, proposal) => proposal.kind !== 'alchemy');",
      "kcds.core.Events.on('craftingStarted', (player, e) => kcds.core.Chat.sendToAll('started ' + e.station));",
      "kcds.core.Events.on('craftingEnded', (player, e) => kcds.core.Chat.sendToAll('ended ' + e.reason));",
    ].join('\n'),
  );
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  await host.startAll();
  const report = (station) =>
    host.updatePlayer({
      id: 9,
      ready: true,
      position: { x: 2298.6, y: 2014.0, z: 106.5 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
      station,
    });
  const player = report('');
  host.crafting.report(
    player,
    { report: 'ask', request: 5, kind: 'alchemy', station: TROSKOVICE },
    0,
  );
  host.crafting.report(player, { report: 'ask', request: 6, kind: 'smithing', station: TACHOV }, 0);
  assert.deepEqual(
    output.filter((e) => e.op === 'craftVerdict'),
    [
      { op: 'craftVerdict', id: 9, request: 5, allowed: false },
      { op: 'craftVerdict', id: 9, request: 6, allowed: true },
    ],
  );
  report('smith S1997.1200,2498.7451,115.0716');
  const Crafting = host.api.Crafting;
  assert.equal(Crafting.session(player).station, TACHOV_SMITHY);
  assert.throws(() => Crafting.session({ id: 9 }), TypeError);
  // The game takes the iron, then gives the horseshoe: a workpiece from start to end.
  const horseshoe = recipes.define().get('r_horseshoe');
  host.playerInventories.operate(player, (inventory) =>
    inventory.set({ items: [{ id: 'iron', item: horseshoe.ingredients[0].item, amount: 3 }] }),
  );
  host.playerInventories.flush();
  host.playerInventories.native(player, [{ kind: 'lost', id: 'iron', amount: 1 }]);
  host.playerInventories.native(player, [
    { kind: 'gained', item: horseshoe.products[0].item, amount: 1, quality: 1, health: 1 },
  ]);
  host.playerInventories.flush();
  report('');
  assert.equal(Crafting.session(player), null);
  const chat = output.filter((e) => e.op === 'chat').map((e) => e.text);
  assert.deepEqual(chat, [`started ${TACHOV_SMITHY}`, 'ended ']);
});

test('alchemy: each step is checked against what the inventory gave up', () => {
  const { sessions, events } = crafting();
  const player = { id: 7 };
  const herb = 'a314b580-bc97-4802-ae1f-8f4803e34503'; // herb_belladona
  const water = '00000000-0000-0000-0000-0000000000aa';
  sessions.observe(player, 'alchemy S2299.5569,2016.3770,106.4078', 0, 1000);
  // One herb taken from the inventory, one on the table: fine. The water is the table's own.
  sessions.inventoryChanged(player, change('drop', herb, -1, 'herbs'));
  sessions.table(
    player,
    [
      [6, water, true, false, false],
      [2, herb, false, false, false],
    ],
    1100,
  );
  // A second herb shows before its inventory report: no refusal within the grace...
  sessions.table(
    player,
    [
      [6, water, true, false, false],
      [2, herb, false, false, false],
      [3, herb, false, false, false],
    ],
    1200,
  );
  sessions.tick(2000);
  assert.ok(!events.some((e) => e.name === 'craftingEnded'));
  // ...and the inventory's report comes: the table holds what the server holds.
  sessions.inventoryChanged(player, change('drop', herb, -1, 'herbs'));
  sessions.tick(4000);
  assert.ok(!events.some((e) => e.name === 'craftingEnded'));
  // A third herb the inventory never gave: past the grace the batch ends clientError.
  sessions.table(
    player,
    [
      [2, herb, false, false, false],
      [3, herb, false, false, false],
      [4, herb, false, false, false],
    ],
    5000,
  );
  sessions.tick(6000);
  assert.ok(!events.some((e) => e.name === 'craftingEnded'));
  sessions.tick(7100);
  sessions.tick(7100 + 3000);
  const ended = events.find((e) => e.name === 'craftingEnded');
  assert.ok(ended, 'the batch ended');
  assert.deepEqual([ended.payload.outcome, ended.payload.reason], ['cancelled', 'clientError']);
  assert.equal(sessions.activeSession(7), null, 'the table is closed');
});

// a resource that settles refunds stopping and starting again neither duplicates nor
// loses material: while it is stopped the game's own rule stands, after its restart it refunds once.
test('a refunding resource stopped and restarted: no doubled and no lost material', async (t) => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcd2s-refund-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const dir = path.join(root, 'workshop');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'workshop', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(
    path.join(dir, 'index.js'),
    "kcds.core.Events.on('craftingRefunding', (player, proposal, refund) => { refund(); refund(); });",
  );
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  t.after(() => host.shutdown());
  await host.startAll();
  const report = (station) =>
    host.updatePlayer({
      id: 9,
      ready: true,
      position: { x: 1997.1, y: 2498.7, z: 115.1 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
      station,
    });
  const player = report('');
  const [iron] = recipes.define().get('r_horseshoe').ingredients;
  const ironAmount = () =>
    host.playerInventories
      .operate(player, (inventory) => ({ result: inventory.snapshot(), change: null }))
      .items.filter((row) => row.item === iron.item)
      .reduce((sum, row) => sum + row.amount, 0);
  host.playerInventories.operate(player, (inventory) =>
    inventory.set({ items: [{ id: 'iron', item: iron.item, amount: 3 }] }),
  );
  host.playerInventories.flush();
  // One unfinished workpiece, cancelled while the resource is running and again while it is not.
  const cancelled = () => {
    report('smith S1997.1200,2498.7451,115.0716');
    host.playerInventories.native(player, [{ kind: 'lost', id: 'iron', amount: 1 }]);
    host.playerInventories.flush();
    host.crafting.report(player, { report: 'ended', kind: 'smithing', reason: 'cancelled' }, 0);
    host.crafting.tick(Date.now() + 5000);
    report('');
    host.playerInventories.flush();
  };
  await host.stop('workshop');
  cancelled();
  assert.equal(ironAmount(), 2, 'stopped: the game keeps what it took, nothing is given');
  assert.equal(host.crafting.sessions.has(player.id), false);
  await host.start('workshop');
  cancelled();
  assert.equal(ironAmount(), 2, 'restarted: what the game took is given back exactly once');
});

// the server checks the smith's progression before any resource is asked.
test('a smithing recipe the player has not learned or lacks the skill for is refused', () => {
  for (const [mayForge, refused] of [
    [() => false, true],
    [(player, recipe) => recipe.id === 'r_horseshoe' && recipe.minSkill <= 1, false],
  ]) {
    const events = [];
    const verdicts = [];
    const sessions = new CraftingSessions({
      emit: (name, player, payload) => {
        events.push(name);
        return true;
      },
      sendVerdict: (player, request, allowed) => verdicts.push(allowed),
      mayForge,
    });
    const smith = { id: 7 };
    sessions.observe(smith, 'smith S1997.1200,2498.7451,115.0716', 0, 1000);
    sessions.report(smith, { report: 'recipe', kind: 'smithing', reason: 'r_horseshoe' }, 0);
    if (refused) {
      assert.deepEqual(events, [], 'no resource is asked');
      assert.deepEqual(verdicts, [false]);
      assert.equal(sessions.sessions.has(smith.id), false);
    } else {
      assert.deepEqual(events, ['craftingStarting', 'craftingStarted']);
    }
  }
});

// the server's inventory must hold a smithing recipe's materials before the game
// takes them.
test('a smithing recipe whose materials the server inventory lacks is refused', () => {
  for (const [held, refused] of [
    [0, true],
    [1, false],
  ]) {
    const events = [];
    const verdicts = [];
    const asked = [];
    const sessions = new CraftingSessions({
      emit: (name) => {
        events.push(name);
        return true;
      },
      sendVerdict: (player, request, allowed) => verdicts.push(allowed),
      holds: (player, units) => {
        asked.push(units);
        return units.every((unit) => unit.amount <= held);
      },
    });
    const smith = { id: 7 };
    sessions.observe(smith, 'smith S1997.1200,2498.7451,115.0716', 0, 1000);
    sessions.report(smith, { report: 'recipe', kind: 'smithing', reason: 'r_horseshoe' }, 0);
    const [iron] = recipes.define().get('r_horseshoe').ingredients;
    assert.deepEqual(asked, [[{ item: iron.item, amount: iron.amount }]]);
    if (refused) {
      assert.deepEqual(events, []);
      assert.deepEqual(verdicts, [false]);
    } else {
      assert.deepEqual(events, ['craftingStarting', 'craftingStarted']);
    }
  }
});

test('Crafting.cancel with refund gives back every material the workpiece held', () => {
  const { sessions, events, given } = settling();
  const Crafting = sessions.define((player) => player, {
    stationInfos: () => [],
    activeSession: () => null,
    occupancyOf: () => null,
    cancelFor: () => false,
  });
  const player = { id: 7 };
  const [steel] = recipes.define().get('basic_sword').ingredients;
  sessions.observe(player, 'smith S1997.1200,2498.7451,115.0716', 0, 1000);
  sessions.inventoryChanged(player, dropped(steel.item, 5, 'steel'));
  assert.equal(Crafting.cancel(player, { refund: true }), true);
  assert.deepEqual(
    events.map((e) => e.name),
    ['craftingStarted', 'craftingRefunding', 'craftingEnded'],
  );
  assert.deepEqual(given, [{ item: steel.item, amount: 5, metadata: { quality: 2 } }]);
  assert.equal(events.at(-1).payload.reason, 'cancelled');
  assert.throws(() => Crafting.cancel(player, 'yes'), TypeError);
});

test("a granted product is the server's: given from its inventory, the game's unit taken out", () => {
  const calls = [];
  let giveWorks = true;
  const events = [];
  const sessions = new CraftingSessions({
    emit: (name, player, payload) => {
      events.push({ name, payload });
      return true;
    },
    sendVerdict: () => {},
    give: (player, units) => {
      calls.push(['give', units]);
      return giveWorks ? [{ id: 'server-row', amount: units[0].amount }] : [];
    },
    remove: (player, units) => {
      calls.push(['remove', units]);
      return true;
    },
  });
  const player = { id: 7 };
  const horseshoe = recipes.define().get('r_horseshoe');
  const iron = horseshoe.ingredients[0].item;
  const product = horseshoe.products[0].item;
  sessions.observe(player, 'smith S1997.1200,2498.7451,115.0716', 0, 1000);
  sessions.inventoryChanged(player, change('drop', iron, -horseshoe.ingredients[0].amount));
  sessions.inventoryChanged(player, change('loot', product, 1, 'shoe'));
  assert.deepEqual(calls, [
    ['give', [{ item: product, amount: 1, metadata: {} }]],
    ['remove', [{ id: 'shoe', amount: 1 }]],
  ]);
  const completed = events.find((e) => e.name === 'craftingCompleted').payload;
  assert.deepEqual(completed.outputs, [{ id: 'server-row', amount: 1 }]);

  // The server cannot give it: the game's unit stays, nothing is taken.
  calls.length = 0;
  events.length = 0;
  giveWorks = false;
  sessions.inventoryChanged(player, change('drop', iron, -horseshoe.ingredients[0].amount));
  sessions.inventoryChanged(player, change('loot', product, 1, 'shoe2'));
  assert.deepEqual(
    calls.map((call) => call[0]),
    ['give'],
  );
  const kept = events.find((e) => e.name === 'craftingCompleted').payload;
  assert.deepEqual(kept.outputs, [{ id: 'shoe2', amount: 1 }]);
});

test('a player at a smithy, an alchemy table or a grindstone cannot be fought', () => {
  const sessions = new CraftingSessions();
  const smith = { id: 7 };
  const sharpener = { id: 8 };
  sessions.observe(smith, 'smith S1997.1200,2498.7451,115.0716', 0, 1000);
  sessions.observe(sharpener, 'grind S1997.6620,2501.0940,115.2578 F0 G00 E0 K0 Q0', 0, 1000);
  assert.deepEqual([...sessions.working], [7, 8]);

  const sent = [];
  const host = {
    players: new Map([
      [7, { id: 7, ready: true }],
      [8, { id: 8, ready: true }],
      [9, { id: 9, ready: true }],
    ]),
    dice: { byPlayer: new Map() },
    crafting: sessions,
    handlers: new Map(),
    services: { handles: () => false },
    hitRulesSent: false,
    bridge: { send: (op) => sent.push(op) },
    emit: () => true,
  };
  // Blows wait for the server's ruling while someone works at a station, and are refused on them
  // and by them.
  ResourceHost.prototype.pumpHitRules.call(host);
  assert.deepEqual(sent.pop(), { op: 'hitRules', enabled: true, harm: false });
  ResourceHost.prototype.playerHit.call(host, { id: 8, request: 1, attacker: 9, damage: 10 });
  assert.equal(sent.pop().ruling, 0);
  ResourceHost.prototype.playerHit.call(host, { id: 9, request: 2, attacker: 7, damage: 10 });
  assert.equal(sent.pop().ruling, 0);

  // Leaving the station (the field empties) or the server ends it.
  sessions.observe(sharpener, '', 0, 2000);
  sessions.left(smith);
  assert.equal(sessions.working.size, 0);
  ResourceHost.prototype.pumpHitRules.call(host);
  assert.deepEqual(sent.pop(), { op: 'hitRules', enabled: false, harm: false });
});
