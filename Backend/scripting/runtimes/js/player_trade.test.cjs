'use strict';

// Trade: a player's inventory as a shop for another player on the game's own shop
// screen; both inventories change at once or not at all.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { lookupItemClass } = require('./inventory/catalog.cjs');

const APPLE = lookupItemClass('apple');
const SWORD = lookupItemClass('longSwordDuel');

async function startedHost() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-trade-'));
  const output = [];
  const host = new ResourceHost(root, {
    entityIds: { first: 4097, count: 2048 },
    send: (message) => output.push(message),
    log: () => {},
  });
  await host.startAll();
  const events = [];
  for (const name of ['tradeOpen', 'tradeItem', 'tradeDone', 'tradeClosed']) {
    host.on(name, (seller, buyer, ...rest) => {
      events.push([name, seller.id, buyer.id, ...rest]);
    });
  }
  const api = host.makeAPI();
  const sent = (op) => output.filter((message) => message.op === op);
  return { host, Trade: api.Trade, Inventory: api.Inventory, sent, events };
}

function join(host, id) {
  return host.updatePlayer({
    id,
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
}

function amountOf(Inventory, player, name) {
  return Inventory.get(player)
    .items.filter((row) => row.name === name)
    .reduce((sum, row) => sum + row.amount, 0);
}

function rowOf(Inventory, player, name) {
  return Inventory.get(player).items.find((row) => row.name === name);
}

async function twoPlayers() {
  const fixture = await startedHost();
  const { host, Inventory } = fixture;
  const seller = join(host, 1);
  const buyer = join(host, 2);
  Inventory.add(seller, { item: 'apple', amount: 5, notify: false });
  Inventory.add(seller, { item: SWORD.id, amount: 1, notify: false });
  Inventory.add(seller, { item: 'money', amount: 30, notify: false });
  Inventory.add(buyer, { item: 'money', amount: 100, notify: false });
  Inventory.add(buyer, { item: 'apple', amount: 2, notify: false });
  host.playerInventories.flush();
  return { ...fixture, seller, buyer };
}

test("the buyer's shop screen shows the seller's rows at the given or the server's price", async () => {
  const { Trade, sent, events, seller, buyer } = await twoPlayers();
  const session = Trade.open(seller, buyer, { prices: { apple: 7 } });
  assert.ok(session > 0);
  assert.equal(Trade.activeSession(seller), session);
  assert.equal(Trade.activeSession(buyer), session);
  const show = sent('vendorShow').at(-1);
  assert.equal(show.id, buyer.id);
  assert.equal(show.open, true);
  assert.equal(show.buys, false);
  // Every row but money; the purse is the seller's money.
  assert.deepEqual(
    show.rows.map((row) => row[1]),
    [APPLE.id, SWORD.id],
  );
  assert.equal(show.prices[0], 7);
  assert.ok(show.prices[1] > 0);
  assert.equal(show.purse, 30);
  assert.deepEqual(events, [['tradeOpen', 1, 2]]);
  assert.equal(Trade.open(seller, seller), 0);
});

test('a basket moves the items and the money between both inventories at once', async () => {
  const { host, Trade, Inventory, events, seller, buyer } = await twoPlayers();
  const session = Trade.open(seller, buyer, {
    prices: { apple: 7 },
    buys: true,
    buyPrices: { apple: 3 },
  });
  const vendor = host.vendors.sessions.get(session).vendor;
  const apples = rowOf(Inventory, seller, 'apple');
  const ownApples = rowOf(Inventory, buyer, 'apple');
  events.length = 0;
  const result = host.vendors.trade(
    buyer,
    session,
    vendor.revision,
    [[apples.id, 3]],
    [[ownApples.id, 2]],
  );
  assert.deepEqual(result, { ok: true, code: '' });
  // The buyer paid 21 and got 6: 85 money; 3 apples bought, 2 sold.
  assert.equal(amountOf(Inventory, buyer, 'money'), 85);
  assert.equal(amountOf(Inventory, buyer, 'apple'), 3);
  assert.equal(amountOf(Inventory, seller, 'money'), 45);
  assert.equal(amountOf(Inventory, seller, 'apple'), 4);
  assert.deepEqual(
    events.map((event) => [event[0], event.at(-1)]),
    [
      ['tradeItem', -15],
      ['tradeDone', -15],
    ],
  );
  // The screen shows the new shelf; the old basket is stale.
  assert.ok(vendor.revision > 1);
  assert.equal(host.vendors.trade(buyer, session, 1, [[apples.id, 1]], []).code, 'staleRevision');
});

test('a refused or unaffordable basket moves nothing on either side', async () => {
  const { host, Trade, Inventory, seller, buyer } = await twoPlayers();
  const session = Trade.open(seller, buyer, { prices: { [SWORD.id]: 500 } });
  const vendor = host.vendors.sessions.get(session).vendor;
  const sword = rowOf(Inventory, seller, SWORD.name);
  assert.equal(
    host.vendors.trade(buyer, session, vendor.revision, [[sword.id, 1]], []).code,
    'insufficientMoney',
  );
  host.on('tradeItem', () => false);
  const apples = rowOf(Inventory, seller, 'apple');
  assert.equal(
    host.vendors.trade(buyer, session, vendor.revision, [[apples.id, 1]], []).code,
    'refused',
  );
  assert.equal(amountOf(Inventory, buyer, 'money'), 100);
  assert.equal(amountOf(Inventory, seller, 'apple'), 5);
  // Selling is off unless the trade buys.
  const own = rowOf(Inventory, buyer, 'apple');
  assert.equal(
    host.vendors.trade(buyer, session, vendor.revision, [], [[own.id, 1]]).code,
    'notBuying',
  );
});

test('the trade ends with the seller leaving, a tradeOpen refusal opens none, Trade.close ends it', async () => {
  const { host, Trade, events, seller, buyer, sent } = await twoPlayers();
  let session = Trade.open(seller, buyer);
  host.vendors.knockedOut(seller);
  assert.equal(Trade.activeSession(buyer), 0);
  assert.deepEqual(events.at(-1), ['tradeClosed', 1, 2, 1]);
  assert.equal(sent('vendorClose').at(-1).session, session);
  session = Trade.open(seller, buyer);
  assert.equal(Trade.close(session), true);
  assert.equal(Trade.close(session), false);
  host.on('tradeOpen', () => false);
  assert.equal(Trade.open(seller, buyer), 0);
});

test('a change of either inventory shows the screen anew', async () => {
  const { host, Trade, Inventory, sent, seller, buyer } = await twoPlayers();
  const session = Trade.open(seller, buyer);
  const vendor = host.vendors.sessions.get(session).vendor;
  const before = vendor.revision;
  Inventory.add(seller, { item: 'apple', amount: 1, notify: false });
  assert.equal(vendor.revision, before + 1);
  const apples = sent('vendorShow')
    .at(-1)
    .rows.find((row) => row[1] === APPLE.id);
  assert.equal(apples[2], 6);
  assert.throws(() => Trade.open(seller, buyer, { prices: { apple: -1 } }), /whole number/);
  assert.throws(() => Trade.open(seller, buyer, { buys: 'yes' }), /boolean/);
});
