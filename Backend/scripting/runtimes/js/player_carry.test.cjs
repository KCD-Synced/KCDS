'use strict';

// Items carried in the arms: Player.holdItem, dropHeldItem, heldItem, the
// playerCarryingItem, playerCarryItem and playerPutDownItem events, and carryable ground items.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { CARRYABLE, carriedName, carryClassOf } = require('./carry/player_carry.cjs');

const BASKET = CARRYABLE.basket_full_wood.item;
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

async function fixture() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-carry-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'porter');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'porter', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(
    path.join(dir, 'index.js'),
    `const idOf = (ground) => (ground ? ground.id : '');
    kcds.core.Events.on('playerCarryingItem', (p, item, ground) => {
      kcds.core.Chat.sendToAll(['asking', p.id, item, idOf(ground)].join(' '));
      if (p.id === 9) return false;
    });
    kcds.core.Events.on('playerCarryItem', (p, item, ground) =>
      kcds.core.Chat.sendToAll(['carry', p.id, item, idOf(ground), p.heldItem].join(' ')));
    kcds.core.Events.on('playerPutDownItem', (p, item, ground) =>
      kcds.core.Chat.sendToAll(['down', p.id, item, idOf(ground), p.heldItem].join(' ')));
    kcds.core.Events.on('groundItemPickup', (g, p) => kcds.core.Chat.sendToAll('pickup ' + g.id + ' ' + p.id));
    kcds.core.Events.on('groundItemDestroy', (g) => kcds.core.Chat.sendToAll('gone ' + g.id));
    kcds.core.Events.on('playerDisconnect', (p) => kcds.core.Chat.sendToAll('left ' + p.id));`,
  );
  await host.startAll();
  const report = (id, fields = {}) => {
    return host.updatePlayer({
      id,
      ready: true,
      position: { x: 0, y: 0, z: 0 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
      vitals: VITALS,
      ...fields,
    });
  };
  const chat = () => output.filter((e) => e.op === 'chat').map((e) => e.text);
  const orders = () => output.filter((e) => e.op === 'carryOrder');
  return { host, output, report, chat, orders };
}

test('holdItem orders a carryable item by name; the events follow the client', async () => {
  const { host, report, chat, orders } = await fixture();
  const p = report(7);
  assert.equal(p.heldItem, null);
  assert.equal(p.holdItem('basket_full_wood'), true);
  assert.deepEqual(orders().at(-1), { op: 'carryOrder', id: 7, kind: 1, item: BASKET });
  // Nothing is carried until the client says so.
  assert.equal(p.heldItem, null);
  report(7, { carry: BASKET.toUpperCase() });
  assert.equal(p.heldItem, 'basket_full_wood');
  // Already carrying.
  assert.equal(p.holdItem('waterBucket'), false);
  assert.equal(p.dropHeldItem(), true);
  assert.deepEqual(orders().at(-1), { op: 'carryOrder', id: 7, kind: 2 });
  assert.equal(p.dropHeldItem({ immediate: true }), true);
  assert.deepEqual(orders().at(-1), { op: 'carryOrder', id: 7, kind: 3 });
  report(7, { carry: '' });
  assert.equal(p.heldItem, null);
  assert.equal(p.dropHeldItem(), false);
  assert.deepEqual(chat(), [
    'carry 7 basket_full_wood  basket_full_wood',
    'down 7 basket_full_wood  ',
  ]);
  host.disconnect(7);
});

test('holdItem refuses what has no carrying walk and players who cannot carry', async () => {
  const { host, report, orders } = await fixture();
  const p = report(7);
  assert.throws(() => p.holdItem('apple'), TypeError);
  assert.throws(() => p.holdItem({ id: 1 }), TypeError);
  assert.throws(() => p.dropHeldItem(null), TypeError);
  const woman = report(8, { appearance: { gender: 'female' } });
  assert.throws(() => woman.holdItem('sack'), TypeError);
  assert.equal(woman.holdItem('eggbasket'), true);
  const hurt = report(9, { vitals: { ...VITALS, health: 0 } });
  assert.equal(hurt.holdItem('eggbasket'), false);
  const loading = report(10, { ready: false });
  assert.equal(loading.holdItem('eggbasket'), false);
  assert.deepEqual(
    orders().map((order) => order.id),
    [8],
  );
  host.disconnect(7);
});

test('a carry ends with playerPutDownItem when the player leaves; names come from the client', async () => {
  const { host, report, chat } = await fixture();
  report(7, { carry: CARRYABLE.sack.item });
  // A carry the player started themselves, of an item outside the table: its catalog name.
  report(8, { carry: 'b38c34b7-6016-4f64-9ba2-65e1ce31d4a1' });
  host.disconnect(7);
  assert.deepEqual(chat(), [
    'carry 7 sack  sack',
    `carry 8 ${carriedName('b38c34b7-6016-4f64-9ba2-65e1ce31d4a1')}  ${carriedName(
      'b38c34b7-6016-4f64-9ba2-65e1ce31d4a1',
    )}`,
    'down 7 sack  ',
    'left 7',
  ]);
  assert.equal(carriedName(''), null);
  assert.equal(
    carriedName('00000000-0000-0000-0000-00000000abcd'),
    '00000000-0000-0000-0000-00000000abcd',
  );
});

const ANY_WORLD = 0xffffffff;

test("a carryable ground item: the game's pick-up asks playerCarryingItem, the carry takes it", async () => {
  const { host, report, chat, orders } = await fixture();
  const { GroundItem } = host.makeAPI();
  const p = report(7);
  const refused = report(9);
  const other = report(8);
  const basket = GroundItem.spawn('basket_full_wood', { x: 1, y: 0, z: 0 }, {}, 1, ANY_WORLD, {
    carryable: true,
  });
  assert.equal(host.groundItems.pickup(refused, basket.id), false);
  assert.equal(host.groundItems.pickup(p, basket.id), true);
  assert.deepEqual(orders().at(-1), { op: 'carryOrder', id: 7, kind: 4, stack: basket.id });
  // It is being taken: nobody else takes it, and the player starts no other carry.
  assert.equal(other.holdItem(basket), false);
  assert.equal(p.holdItem('waterBucket'), false);
  report(7, { carry: BASKET });
  assert.equal(GroundItem.getById(basket.id), null);
  assert.deepEqual(chat(), [
    `asking 9 basket_full_wood ${basket.id}`,
    `asking 7 basket_full_wood ${basket.id}`,
    `carry 7 basket_full_wood ${basket.id} basket_full_wood`,
    `pickup ${basket.id} 7`,
    `gone ${basket.id}`,
  ]);
});

test('holdItem(groundItem) takes a carryable one up without asking; others are refused', async () => {
  const { host, report, chat, orders } = await fixture();
  const { GroundItem } = host.makeAPI();
  const p = report(7);
  const far = GroundItem.spawn('waterBucket', { x: 20, y: 0, z: 0 }, {}, 1, ANY_WORLD, {
    carryable: true,
  });
  const stock = GroundItem.spawn('apple', { x: 1, y: 0, z: 0 }, {}, 1, ANY_WORLD);
  const near = GroundItem.spawn('waterBucket', { x: 1, y: 0, z: 0 }, {}, 1, ANY_WORLD, {
    carryable: true,
  });
  assert.equal(p.holdItem(far), false);
  assert.equal(p.holdItem(stock), false);
  assert.equal(p.holdItem(near), true);
  assert.deepEqual(orders().at(-1), { op: 'carryOrder', id: 7, kind: 4, stack: near.id });
  assert.deepEqual(chat(), []);
});

test('a put-down lays a carryable ground item where the item fell', async () => {
  const { host, report, chat } = await fixture();
  const { GroundItem } = host.makeAPI();
  const p = report(7);
  report(7, { carry: BASKET });
  host.carries.putDown(p, { item: BASKET, position: { x: 1, y: 0.5, z: 0.2 } });
  const [prop] = GroundItem.all();
  assert.equal(prop.carryable, true);
  assert.equal(prop.leftBy, p);
  assert.deepEqual([prop.position.x, prop.position.y, prop.position.z], [1, 0.5, 0.2]);
  assert.equal(p.heldItem, null);
  // The client still names the item for a moment, then reports the carry over: nothing more.
  report(7, { carry: BASKET });
  report(7, { carry: '' });
  // Let go too far from the player: nothing is laid.
  report(7, { carry: BASKET });
  host.carries.putDown(p, { item: BASKET, position: { x: 50, y: 0, z: 0 } });
  assert.equal(GroundItem.all().length, 1);
  assert.deepEqual(chat(), [
    'carry 7 basket_full_wood  basket_full_wood',
    `down 7 basket_full_wood ${prop.id} `,
    'carry 7 basket_full_wood  basket_full_wood',
    'down 7 basket_full_wood  ',
  ]);
});

test('every carryable item of the game is laid as a ground item when put down', () => {
  const bloody = CARRYABLE.rasuvUcen_sackcarryable;
  assert.equal(bloody.item, 'c25a4e91-550e-4fa1-a46d-071ee4760f4d');
  assert.equal(carryClassOf(bloody.item).name, 'rasuvUcen_sackcarryable');
  assert.equal(carryClassOf('rasuvUcen_sackcarryable').id, bloody.item);
});
