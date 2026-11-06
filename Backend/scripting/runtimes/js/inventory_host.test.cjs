'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');

const SWORD = '00cca9e3-8ef2-46db-8cbf-86ec51930919';

function hostWith(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcd2s-inventory-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'shop');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'shop', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  const chat = () => output.filter((e) => e.op === 'chat').map((e) => e.text);
  return { host, chat, output };
}

function player(host, id = 7) {
  host.updatePlayer({
    id,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  return host.players.get(id);
}

test('an inventory exists from playerConnect, so a handler can restore a saved one there', async () => {
  const { host } = hostWith(
    `kcds.core.Events.on('playerConnect', (p) => {
      kcds.players.Inventory.set(p, { items: [{ id: 'blade', item: 'longSwordDuel', amount: 1, equipped: 1 }] });
    });`,
  );
  await host.startAll();
  const p = player(host);
  const state = host.api.Inventory.get(p);
  assert.equal(state.revision, 1);
  assert.equal(state.ready, false);
  assert.equal(state.harvested, undefined);
  assert.deepEqual(
    state.items.map((row) => [row.id, row.name, row.amount, row.equipped]),
    [['blade', 'longSwordDuel', 1, 1]],
  );
  assert.deepEqual(p.getInventory(), state);
});

test('playerInventoryChanged is raised on the next tick, in order, once per inventory touched', async () => {
  const { host, chat } = hostWith(
    `kcds.core.Events.on('playerInventoryChanged', (p, change) =>
      kcds.core.Chat.sendToAll(p.id + ':' + change.reason + ':' + change.revision + ':' + change.items.length));`,
  );
  await host.startAll();
  const a = player(host, 7);
  const b = player(host, 8);
  const { Inventory } = host.api;
  const id = Inventory.add(a, { item: SWORD, amount: 2 }).items[0].id;
  Inventory.transfer(a, b, { units: [{ id, amount: 1 }] });
  assert.deepEqual(chat(), []);
  host.playerInventories.flush();
  assert.deepEqual(chat(), ['7:add:1:1', '7:transfer:2:1', '8:transfer:1:1']);
});

test('giveItem and takeItem work by name, all or none', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const p = player(host);
  assert.equal(p.giveItem('longSwordDuel', 3), true);
  assert.equal(p.giveItem('no such thing'), false);
  assert.equal(p.giveItem(SWORD, 0), false);
  assert.deepEqual(await p.takeItem(SWORD, 5), {
    removed: 0,
    requested: 5,
    ok: false,
    reason: 'insufficientItems',
  });
  assert.deepEqual(await p.takeItem('longSwordDuel', 2), {
    removed: 2,
    requested: 2,
    ok: true,
    reason: '',
  });
  assert.equal(p.getInventory().items[0].amount, 1);
});

test('a disconnecting player has their changes raised before playerDisconnect', async () => {
  const { host, chat } = hostWith(
    `kcds.core.Events.on('playerInventoryChanged', (p, change) => kcds.core.Chat.sendToAll('changed:' + change.reason));
    kcds.core.Events.on('playerDisconnect', (p) => kcds.core.Chat.sendToAll('gone:' + (kcds.players.Inventory.get(p) ? 'kept' : 'none')));`,
  );
  await host.startAll();
  const p = player(host);
  p.giveItem(SWORD);
  host.disconnect(7);
  assert.deepEqual(chat().slice(-2), ['changed:add', 'gone:kept']);
  assert.equal(host.api.Inventory.get(p), null);
  assert.equal(host.api.Inventory.add(p, { item: SWORD, amount: 1 }).code, 'inventoryUnavailable');
});

test('rows that are not kept leave before playerDisconnect, so a save there skips them', async () => {
  const { host, chat } = hostWith(
    `kcds.core.Events.on('playerDisconnect', (p) =>
      kcds.core.Chat.sendToAll('saved:' + kcds.players.Inventory.get(p).items.map((row) => row.amount).join(',')));`,
  );
  await host.startAll();
  const p = player(host);
  host.api.Inventory.add(p, { item: SWORD, amount: 1 });
  const sack = host.api.Inventory.add(p, { item: SWORD, amount: 2, metadata: { transient: true } });
  assert.equal(sack.ok, true);
  const rows = host.api.Inventory.get(p).items;
  assert.equal(rows.length, 2);
  assert.deepEqual(
    rows.map((row) => row.metadata.transient === true),
    [false, true],
  );
  host.disconnect(7);
  assert.deepEqual(chat().slice(-1), ['saved:1']);
});

test('playerInventoryReady fires once the player game shows the latest revision', async () => {
  const { host, chat } = hostWith(
    `kcds.core.Events.on('playerInventoryReady', (p) => kcds.core.Chat.sendToAll('ready'));`,
  );
  await host.startAll();
  const p = player(host);
  p.giveItem(SWORD);
  host.playerInventories.shown(p, 0);
  assert.equal(p.getInventory().ready, false);
  host.playerInventories.shown(p, 1);
  assert.equal(p.getInventory().ready, true);
  p.giveItem(SWORD);
  host.playerInventories.shown(p, 2);
  assert.deepEqual(chat(), ['ready']);
});

test('the join census becomes the stock of an untouched inventory and goes back to the game', async () => {
  const { host, chat, output } = hostWith(
    `kcds.core.Events.on('playerInventoryChanged', (p, change) => kcds.core.Chat.sendToAll(change.reason));`,
  );
  await host.startAll();
  const p = player(host);
  assert.deepEqual(
    output.filter((e) => e.op === 'inventoryShow'),
    [],
    'an untouched inventory waits for the census',
  );
  const census = [
    { item: SWORD, amount: 1, quality: 3, health: 0.5, equipped: 1 },
    { item: SWORD, amount: 1, quality: 3, health: 0.5, equipped: 0 },
    { item: '00000000-0000-0000-0000-000000000000', amount: 1, quality: 1, health: 1, equipped: 0 },
  ];
  host.playerInventories.census(p, census);
  const state = p.getInventory();
  assert.equal(state.revision, 1);
  assert.deepEqual(
    state.items.map((row) => [row.amount, row.quality, row.equipped]),
    [[2, 3, 1]],
  );
  const shows = output.filter((e) => e.op === 'inventoryShow');
  assert.equal(shows.length, 1);
  assert.equal(shows[0].revision, 1);
  assert.equal(
    shows[0].replace,
    false,
    'a stock from the census does not replace the inventory of the game',
  );
  host.playerInventories.census(p, census);
  assert.equal(p.getInventory().revision, 1, 'a second census changes nothing');
  host.playerInventories.flush();
  assert.deepEqual(chat(), ['census']);
});

test('a restored inventory goes to the game when it is ready, and its census is ignored', async () => {
  const { host, output } = hostWith(
    `kcds.core.Events.on('playerConnect', (p) => kcds.players.Inventory.set(p, { items: [{ item: 'longSwordDuel', amount: 1 }] }));`,
  );
  await host.startAll();
  host.updatePlayer({
    id: 7,
    ready: false,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  assert.equal(output.filter((e) => e.op === 'inventoryShow').length, 0);
  host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const p = host.players.get(7);
  const shows = output.filter((e) => e.op === 'inventoryShow');
  assert.equal(shows.length, 1);
  assert.equal(shows[0].rows.length, 1);
  assert.equal(shows[0].replace, true, 'a restored stock replaces the inventory of the game');
  host.playerInventories.census(p, [
    { item: SWORD, amount: 5, quality: 1, health: 0.1, equipped: 0 },
  ]);
  assert.equal(p.getInventory().items[0].amount, 1);
});

test('playerItemUsed follows the playerInventoryChanged that removed the units', async () => {
  const { host, chat } = hostWith(
    `kcds.core.Events.on('playerInventoryChanged', (p, change) => kcds.core.Chat.sendToAll('changed:' + change.reason));
    kcds.core.Events.on('playerItemUsed', (p, item, kind, amount) => kcds.core.Chat.sendToAll('used:' + kind + ':' + amount));`,
  );
  await host.startAll();
  const p = player(host);
  const food = host.api.Inventory.classes().find(
    (entry) => entry.category === 'food' && !entry.quest,
  );
  const id = host.api.Inventory.add(p, { item: food.id, amount: 2 }).items[0].id;
  host.playerInventories.native(p, [{ kind: 'lost', id, amount: 1 }]);
  host.playerInventories.flush();
  assert.deepEqual(chat(), ['changed:add', 'changed:use', 'used:food:1']);
  assert.equal(p.getInventory().items[0].amount, 1);
});

test('Player.equipment and the hand items come from what their client reports', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const p = player(host);
  assert.deepEqual(p.equipment, []);
  assert.equal(p.rightHandItem, '');
  const sword = '46db8ef200cca9e319099351ec86bf8c';
  host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    equipment: [sword],
    hands: [sword, ''],
  });
  assert.deepEqual(p.equipment, [sword]);
  assert.equal(p.rightHandItem, sword);
  assert.equal(p.leftHandItem, '');
  p.equipment.push('x');
  assert.deepEqual(p.equipment, [sword], 'a copy');
});

test('setOutfit takes worn clothing off, keeps it, and dresses the body in the outfit', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const p = player(host);
  const { Inventory, Player } = host.api;
  const male = Player.outfitSets('male');
  const female = Player.outfitSets('female');
  assert.ok(male.length > 1000 && female.length > 100);
  assert.throws(() => Player.outfitSets('other'), TypeError);
  const first = male.find((name) => !female.includes(name));
  const second = male.find((name) => name !== first && !female.includes(name));
  assert.equal(p.setOutfit(female.find((name) => !male.includes(name))), false);
  assert.equal(p.setOutfit('no such outfit'), false);
  assert.equal(p.setOutfit(first), true);
  const worn = Inventory.get(p).items.filter((row) => row.equipped > 0);
  assert.ok(worn.length > 0);
  assert.equal(p.setOutfit(second), true);
  const state = Inventory.get(p);
  for (const row of worn) {
    const kept = state.items.find((item) => item.item === row.item);
    assert.ok(kept, 'the old clothing stays in the inventory');
  }
  assert.ok(state.items.some((row) => row.equipped > 0));
  host.playerInventories.flush();
});

test('renameItem renames every class of one name, for everyone and for whoever comes in later', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  const { Inventory } = host.api;
  const renamed = Inventory.renameItem('longSwordDuel', 'Oathkeeper');
  assert.ok(renamed.includes('00cca9e3-8ef2-46db-8cbf-86ec51930919'));
  const sent = output.filter((e) => e.op === 'itemNames');
  assert.deepEqual(sent.at(-1), {
    op: 'itemNames',
    id: 0,
    full: false,
    names: [['ui_nm_longSwordDuel', 'Oathkeeper']],
  });
  player(host, 9);
  assert.deepEqual(output.filter((e) => e.op === 'itemNames').at(-1), {
    op: 'itemNames',
    id: 9,
    full: true,
    names: [['ui_nm_longSwordDuel', 'Oathkeeper']],
  });
  assert.ok(Inventory.renameItem('longSwordDuel', null).length > 0);
  assert.deepEqual(output.filter((e) => e.op === 'itemNames').at(-1).names, [
    ['ui_nm_longSwordDuel', ''],
  ]);
  assert.deepEqual(Inventory.renameItem('no such item', 'x'), []);
  assert.throws(() => Inventory.renameItem('apple', 'line\nbreak'), TypeError);
  assert.throws(() => Inventory.renameItem('apple', 'x'.repeat(129)), TypeError);
});

test('quickslots a resource restores go to the game once; what the game reports is the state', async () => {
  const quickslots = {
    weapons: [
      { primary: 'blade', secondary: null },
      { primary: null, secondary: null },
      { primary: null, secondary: null },
      { primary: null, secondary: null },
    ],
    items: [null, null, 'apples', null],
    activeWeapon: 0,
    activeItem: 2,
  };
  const { host, output } = hostWith(
    `kcds.core.Events.on('playerConnect', (p) => kcds.players.Inventory.set(p, {
       items: [{ id: 'blade', item: 'longSwordDuel', amount: 1, equipped: 1 },
               { id: 'apples', item: 'apple', amount: 3 }],
       quickslots: ${JSON.stringify(quickslots)},
     }));`,
  );
  await host.startAll();
  const p = player(host);
  const shows = output.filter((e) => e.op === 'inventoryShow');
  assert.ok(shows.slice(1).every((show) => show.quickslots === undefined));
  assert.deepEqual(shows[0].quickslots, [
    'blade',
    '',
    '',
    '',
    '',
    '',
    '',
    '',
    '',
    '',
    'apples',
    '',
    0,
    2,
  ]);
  assert.deepEqual(p.getInventory().quickslots, quickslots);
  // Another stock carries no quickslots: the game keeps what the player arranged.
  p.giveItem('apple', 1);
  assert.equal(output.filter((e) => e.op === 'inventoryShow').at(-1).quickslots, undefined);
  // The game reports what its slots hold; a row the stock lacks reads as empty.
  host.playerInventories.quickslots(p, [
    '',
    '',
    'blade',
    '',
    '',
    '',
    '',
    '',
    'gone',
    '',
    '',
    '',
    1,
    0,
  ]);
  const state = p.getInventory().quickslots;
  assert.deepEqual(state.weapons[1], { primary: 'blade', secondary: null });
  assert.deepEqual(state.items, [null, null, null, null]);
  assert.equal(state.activeWeapon, 1);
  host.playerInventories.quickslots(p, ['too short']);
  assert.equal(p.getInventory().quickslots.activeWeapon, 1, 'a malformed report changes nothing');
  // A weapon slot keeps its weapon whether or not the body wears it: the game counts only the
  // selected slot's weapon as worn.
  host.playerInventories.native(p, [{ kind: 'equipped', id: 'blade', amount: 0 }]);
  assert.deepEqual(p.getInventory().quickslots.weapons[1], { primary: 'blade', secondary: null });
  host.playerInventories.native(p, [{ kind: 'equipped', id: 'blade', amount: 1 }]);
  assert.deepEqual(p.getInventory().quickslots.weapons[1], { primary: 'blade', secondary: null });
  // What the game reports is what the slots hold.
  host.playerInventories.native(p, [{ kind: 'equipped', id: 'blade', amount: 0 }]);
  host.playerInventories.quickslots(p, ['blade', '', '', '', '', '', '', '', '', '', '', '', 0, 0]);
  assert.deepEqual(p.getInventory().quickslots.weapons[0], { primary: 'blade', secondary: null });
  assert.deepEqual(p.getInventory().quickslots.weapons[1], { primary: null, secondary: null });
  // A row that goes leaves its slots empty.
  removeRow(host, p, 'blade');
  assert.deepEqual(p.getInventory().quickslots.weapons[0], { primary: null, secondary: null });
});

function removeRow(host, p, id) {
  host.playerInventories.operate(p, (inventory) =>
    inventory.remove({ units: [{ id, amount: 1 }] }),
  );
}

test('a saved inventory comes back whole: ids, metadata, what the body wears, quickslots', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const p = player(host);
  const empty = { primary: null, secondary: null };
  const original = host.playerInventories.operate(p, (inventory) =>
    inventory.set({
      items: [
        {
          id: 'blade',
          item: 'longSwordDuel',
          amount: 1,
          equipped: 1,
          metadata: {
            quality: 3,
            condition: 0.5,
            custom: { name: 'Kit blade', data: { kills: 3 } },
          },
        },
        { id: 'apples', item: 'apple', amount: 3 },
      ],
      quickslots: {
        weapons: [{ primary: 'blade', secondary: null }, empty, empty, empty],
        items: ['apples', null, null, null],
        activeWeapon: 0,
        activeItem: 0,
      },
    }),
  );
  assert.equal(original.ok, true);
  const saved = p.getInventory();
  // What a server keeps: Inventory.get as it is, then Inventory.set of it on the way back.
  const kept = JSON.parse(JSON.stringify(saved));
  host.playerInventories.operate(p, (inventory) => inventory.set({ items: [] }));
  const restored = host.playerInventories.operate(p, (inventory) =>
    inventory.set({
      items: kept.items.map(({ id, item, amount, metadata, equipped }) => ({
        id,
        item,
        amount,
        metadata,
        equipped,
      })),
      quickslots: kept.quickslots,
    }),
  );
  assert.equal(restored.ok, true);
  const back = p.getInventory();
  assert.deepEqual(back.items, saved.items);
  assert.deepEqual(back.quickslots, saved.quickslots);
});

test('a use the server decides: playerConsuming, playerConsumptionEffects, playerConsumed', async () => {
  const { host, chat, output } = hostWith(`
    kcds.core.Events.on('playerConsuming', (p, c) => {
      kcds.core.Chat.sendToAll('consuming ' + c.kind + ' ' + c.rowId + ' ' + c.baseEffects.nourishment);
      return c.rowId !== 'locked';
    });
    kcds.core.Events.on('playerConsumptionEffects', (p, c) => c.kind !== 'potion');
    kcds.core.Events.on('playerConsumed', (p, c) => kcds.core.Chat.sendToAll('consumed ' + c.rowId + ' ' + c.nativeEffects));
  `);
  await host.startAll();
  const p = player(host);
  host.playerInventories.operate(p, (inventory) =>
    inventory.set({
      items: [
        { id: 'apple', item: 'apple', amount: 1 },
        { id: 'locked', item: 'apple', amount: 5 },
        { id: 'potion', item: 'potion_antiIflammatoryPotion_high', amount: 1 },
        { id: 'blade', item: 'longSwordDuel', amount: 1 },
      ],
    }),
  );
  const verdicts = () =>
    output.filter((e) => e.op === 'inventoryConsumeReply').map((e) => [e.request, e.verdict]);
  host.playerInventories.consume(p, { request: 1, row: 'apple' });
  host.playerInventories.consume(p, { request: 2, row: 'apple' });
  host.playerInventories.consume(p, { request: 3, row: 'locked' });
  host.playerInventories.consume(p, { request: 4, row: 'potion' });
  host.playerInventories.consume(p, { request: 5, row: 'blade' });
  host.playerInventories.consume(p, { request: 6, row: 'nothing' });
  assert.deepEqual(verdicts(), [
    [1, 'native'],
    [2, 'refused'], // its only unit is reserved for request 1
    [3, 'refused'], // a handler refused it
    [4, 'plain'], // a handler took the potion's own effects away
    [5, 'native'], // not a food: the game does it as it does
    [6, 'refused'],
  ]);
  assert.deepEqual(chat().slice(0, 3), [
    'consuming food apple 2',
    'consuming food locked 2',
    'consuming potion potion 0',
  ]);
  // Receipts: playerConsumed on the next tick, once; an unknown or repeated receipt is ignored.
  host.playerInventories.consumed(p, { request: 1, used: true });
  host.playerInventories.consumed(p, { request: 1, used: true });
  host.playerInventories.consumed(p, { request: 4, used: true });
  host.playerInventories.consumed(p, { request: 9, used: true });
  assert.equal(chat().filter((line) => line.startsWith('consumed')).length, 0);
  host.playerInventories.flush();
  assert.deepEqual(
    chat().filter((line) => line.startsWith('consumed')),
    ['consumed apple true', 'consumed potion false'],
  );
  // A use the game did not make releases its unit.
  host.playerInventories.consume(p, { request: 7, row: 'apple' });
  host.playerInventories.consumed(p, { request: 7, used: false });
  host.playerInventories.consume(p, { request: 8, row: 'apple' });
  assert.deepEqual(verdicts().slice(-2), [
    [7, 'native'],
    [8, 'native'],
  ]);
});

test('a serving from a pot or a barrel goes through the same consumption events', async () => {
  const { host, chat, output } = hostWith(`
    kcds.core.Events.on('playerConsuming', (p, c) => {
      kcds.core.Chat.sendToAll('consuming ' + c.source + ' ' + c.kind + ' ' + c.rowId + ' ' + c.potGuid);
      return c.item !== '${'5ef63059-322e-4e1b-abe8-926e100c770e'}';
    });
    kcds.core.Events.on('playerConsumed', (p, c) => kcds.core.Chat.sendToAll('consumed ' + c.source + ' ' + c.nativeEffects));
  `);
  await host.startAll();
  const p = player(host);
  const verdicts = () =>
    output.filter((e) => e.op === 'inventoryConsumeReply').map((e) => [e.request, e.verdict]);
  host.playerInventories.consumePot(p, {
    request: 1,
    item: '2264f217-590e-4c0f-a4c6-f50c6532b9f6',
  });
  host.playerInventories.consumePot(p, {
    request: 2,
    item: '00cca9e3-8ef2-46db-8cbf-86ec51930919',
  });
  assert.deepEqual(verdicts(), [
    [1, 'native'],
    [2, 'native'], // not a food: the game does it as it does, no events
  ]);
  assert.deepEqual(chat(), ['consuming cookPot food null null']);
  host.playerInventories.consumed(p, { request: 1, used: true });
  host.playerInventories.flush();
  assert.deepEqual(chat().at(-1), 'consumed cookPot true');
  assert.equal(p.getInventory().items.length, 0, 'a serving spends no row');
});

test("a pot meal names its fireplace and brings the pot's added poison", async () => {
  const POT = '29410bf0-15c5-4b8a-9d2e-7c3f1a6b5d40';
  const POISON = '04218e40-e756-476b-914e-03d67a24e733';
  const { host, chat, output } = hostWith(`
    kcds.core.Events.on('playerConsuming', (p, c) => {
      kcds.core.Chat.sendToAll('consuming ' + c.potGuid + ' ' + c.poison);
    });
    kcds.core.Events.on('playerPoisonAbsorbing', (p, c, buff) => {
      kcds.core.Chat.sendToAll('absorbing ' + buff + ' ' + c.potGuid);
    });
    kcds.core.Events.on('playerConsumed', (p, c) => kcds.core.Chat.sendToAll('consumed ' + c.poison));
  `);
  await host.startAll();
  const p = player(host);
  const orders = () => output.filter((e) => e.op === 'buffOrder').map((e) => [e.kind, e.buff]);
  const meal = { item: '2264f217-590e-4c0f-a4c6-f50c6532b9f6', pot: POT.toUpperCase() };
  host.playerInventories.consumePot(p, { request: 1, ...meal });
  host.playerInventories.consumed(p, { request: 1, used: true });
  host.playerInventories.flush();
  assert.deepEqual(chat(), [`consuming ${POT} null`, 'consumed null']);
  assert.deepEqual(orders(), [], 'a clean pot adds no poison');
  // The pots of 0.2.3 put poison in a pot: it comes with the meal, raises playerPoisonAbsorbing
  // on the receipt and goes on the player.
  host.playerInventories.potPoisons.set(POT, POISON);
  host.playerInventories.consumePot(p, { request: 2, ...meal });
  host.playerInventories.consumed(p, { request: 2, used: true });
  host.playerInventories.flush();
  assert.deepEqual(chat().slice(2), [
    `consuming ${POT} ${POISON}`,
    `absorbing ${POISON} ${POT}`,
    `consumed ${POISON}`,
  ]);
  assert.deepEqual(orders(), [['add', POISON]]);
  // No fireplace, or a malformed one: no pot, no poison.
  host.playerInventories.consumePot(p, { request: 3, item: meal.item, pot: 'nonsense' });
  assert.equal(chat().at(-1), 'consuming null null');
});

test("a player's game announces what the API gives or takes, unless notify is false", async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  const a = player(host, 7);
  const b = player(host, 8);
  const { Inventory } = host.api;
  const notices = () =>
    output
      .splice(0)
      .filter((e) => e.op === 'itemNotice')
      .map((e) => [e.id, e.amount, e.gained]);
  output.length = 0;
  const id = Inventory.add(a, { item: SWORD, amount: 2 }).items[0].id;
  assert.deepEqual(notices(), [[7, 2, true]]);
  Inventory.add(a, { item: SWORD, amount: 1, notify: false });
  assert.deepEqual(notices(), [], 'notify false: no notice');
  Inventory.transfer(a, b, { units: [{ id, amount: 1 }] });
  assert.deepEqual(notices(), [
    [7, 1, false],
    [8, 1, true],
  ]);
  Inventory.remove(a, { units: [{ id, amount: 1 }], notify: false });
  assert.deepEqual(notices(), []);
  assert.equal(a.giveItem('apple', 3), true);
  assert.deepEqual(notices(), [[7, 3, true]]);
  assert.equal((await a.takeItem('apple', 2)).ok, true);
  assert.deepEqual(notices(), [[7, 2, false]]);
  assert.equal(Inventory.add(a, { item: SWORD, amount: 1, notify: 'yes' }).code, 'invalidRequest');
  Inventory.set(a, { items: [{ item: 'apple', amount: 5 }] });
  assert.deepEqual(notices(), [], 'Inventory.set is never announced');
  assert.equal(a.showItemNotice('apple', 4), true);
  assert.equal(a.showItemNotice(SWORD, 1, false), true);
  assert.deepEqual(notices(), [
    [7, 4, true],
    [7, 1, false],
  ]);
  assert.equal(a.showItemNotice('no such thing'), false);
  assert.equal(a.showItemNotice('apple', 0), false);
  assert.equal(a.showItemNotice('apple', 1, 'yes'), false);
  assert.equal(a.getInventory().items.find((row) => row.name === 'apple').amount, 5, 'untouched');
  host.playerInventories.flush();
});
