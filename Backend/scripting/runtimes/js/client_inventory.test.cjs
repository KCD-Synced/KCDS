'use strict';

// The client Inventory global (Client/resources/inventory.cjs): whenUsed and stopWhenUsed, what the game
// client hears, and which handler a use goes to.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceInventory } = require('../../../../Client/resources/inventory.cjs');
const { guidOf } = require('./inventory/custom_items.cjs');

const APPLE = 'f3aa4492-0000-4ce3-9e41-784cbac8801f';

function setup() {
  const sent = [];
  const inventory = new ResourceInventory({ send: (message) => sent.push(message) });
  const invoke = (fn, args) => fn(...args);
  return { inventory, sent, facade: (owner) => inventory.facade(owner, invoke) };
}

test('whenUsed by name, GUID or UID tells the game client what is overridden', () => {
  const { sent, facade } = setup();
  const shop = facade('shop');
  const apple = shop.whenUsed('apple', () => {});
  shop.whenUsed({ uid: '144115188075855872' }, () => {});
  assert.equal(typeof apple, 'number');
  const last = sent.at(-1);
  assert.equal(last.op, 'inventoryUseOverrides');
  assert.equal(last.classes.length, 1);
  assert.deepEqual(last.uids, ['144115188075855872']);
  assert.throws(() => shop.whenUsed('no such item', () => {}), TypeError);
  assert.throws(() => shop.whenUsed({ uid: 'x' }, () => {}), TypeError);
  assert.throws(() => shop.whenUsed('apple', 'not a function'), TypeError);
});

test('a use goes to the newest instance handler, else the newest class handler', () => {
  const { inventory, facade } = setup();
  const calls = [];
  const shop = facade('shop');
  const other = facade('other');
  shop.whenUsed('longSwordDuel', (use) => calls.push(['old class', use.id]));
  other.whenUsed('longSwordDuel', (use) => calls.push(['new class', use.id]));
  shop.whenUsed({ uid: '7' }, (use) => calls.push(['instance', use.id]));
  const sword = '00cca9e3-8ef2-46db-8cbf-86ec51930919';
  inventory.receiveUse({
    item: sword,
    uid: '7',
    id: 'i1',
    revision: 3,
    amount: 1,
    action: 'secondary',
  });
  inventory.receiveUse({
    item: sword,
    uid: '8',
    id: 'i2',
    revision: 3,
    amount: 1,
    action: 'secondary',
  });
  inventory.receiveUse({
    item: APPLE,
    uid: '9',
    id: 'i3',
    revision: 3,
    amount: 1,
    action: 'secondary',
  });
  assert.deepEqual(calls, [
    ['instance', 'i1'],
    ['new class', 'i2'],
  ]);
});

test("stopWhenUsed takes only the caller's own handler; a stopped resource loses its handlers", () => {
  const { inventory, sent, facade } = setup();
  const shop = facade('shop');
  const other = facade('other');
  const id = shop.whenUsed('apple', () => {});
  assert.equal(other.stopWhenUsed(id), false);
  assert.equal(shop.stopWhenUsed(id), true);
  assert.equal(shop.stopWhenUsed(id), false);
  assert.deepEqual(sent.at(-1).classes, []);
  other.whenUsed('apple', () => {});
  inventory.removeOwner('other');
  assert.deepEqual(sent.at(-1).classes, []);
});

test('a type the server registered is a selector by id or GUID once the game client names it', () => {
  const { inventory, sent, facade } = setup();
  const village = facade('village');
  const letter = guidOf('village:letter');
  assert.throws(() => village.whenUsed('village:letter', () => {}), TypeError);
  inventory.receiveItemTypes({ types: [{ id: 'village:letter', item: letter }] });
  const calls = [];
  village.whenUsed('village:letter', (use) => calls.push(['by id', use.id]));
  village.whenUsed(letter.toUpperCase(), (use) => calls.push(['by GUID', use.id]));
  assert.deepEqual(sent.at(-1).classes, [letter]);
  inventory.receiveUse({
    item: letter,
    uid: '12',
    id: 'row-1',
    revision: 4,
    amount: 1,
    action: 'activate',
  });
  assert.deepEqual(calls, [['by GUID', 'row-1']]);
  // A new set without the type: it is no selector any more, the handlers stay.
  inventory.receiveItemTypes({ types: [] });
  assert.throws(() => village.whenUsed('village:letter', () => {}), TypeError);
  assert.deepEqual(sent.at(-1).classes, [letter]);
});

test("a registered type's own icon is shipped to the game client", () => {
  const shipped = [];
  const inventory = new ResourceInventory({
    send: () => {},
    shipIcon: (resource, path) => {
      if (path === 'missing.dds') throw new Error('this resource does not ship missing.dds');
      shipped.push([resource, path]);
    },
  });
  inventory.receiveItemTypes({
    types: [
      { id: 'village:letter', item: guidOf('village:letter'), iconFile: 'village/icons/a.dds' },
      { id: 'village:timber', item: guidOf('village:timber'), iconFile: '' },
      { id: 'village:lost', item: guidOf('village:lost'), iconFile: 'village/missing.dds' },
    ],
  });
  assert.deepEqual(shipped, [['village', 'icons/a.dds']]);
  // Again once the client has the files: the shipping itself skips what it sent.
  inventory.shipIcons();
  assert.deepEqual(shipped, [
    ['village', 'icons/a.dds'],
    ['village', 'icons/a.dds'],
  ]);
});
