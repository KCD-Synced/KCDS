'use strict';

// Vendor: shelves, buy lists and purses, and the atomic settlement of a basket
// confirmed on the game's own shop screen.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { lookupItemClass } = require('./inventory/catalog.cjs');

const MONEY = lookupItemClass('money');
const APPLE = lookupItemClass('apple');
const SWORD = lookupItemClass('longSwordDuel');

async function startedHost() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-vendor-'));
  const output = [];
  const host = new ResourceHost(root, {
    entityIds: { first: 4097, count: 2048 },
    send: (message) => output.push(message),
    log: () => {},
  });
  await host.startAll();
  const sent = (op) => output.filter((message) => message.op === op);
  const events = [];
  host.on('vendorTrade', (vendor, player, bought, sold, balance) => {
    events.push(['trade', vendor, player.id, bought, sold, balance]);
  });
  host.on('vendorClosed', (vendor, player, reason) => {
    events.push(['closed', vendor, player.id, reason]);
  });
  const api = host.makeAPI();
  return {
    host,
    Vendor: api.Vendor,
    Inventory: api.Inventory,
    Items: api.Items,
    sent,
    output,
    events,
  };
}

function join(host, id) {
  return host.updatePlayer({
    id,
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
}

function rowsOf(Inventory, player) {
  return Inventory.get(player).items.map((row) => [row.name, row.amount]);
}

function rowOf(Inventory, player, name) {
  return Inventory.get(player).items.find((row) => row.name === name);
}

test('open shows the shelf, its prices, the buy list and the purse on the player screen', async () => {
  const { host, Vendor, sent } = await startedHost();
  join(host, 1);
  const vendor = Vendor.create({ name: 'Grocer', purse: 500 });
  assert.equal(
    Vendor.setShelf(vendor, [
      { item: 'apple', amount: 10, price: 15 },
      { item: SWORD.id, amount: 1, price: 4000, metadata: { quality: 2 } },
    ]),
    true,
  );
  assert.equal(Vendor.setOffers(vendor, [{ item: 'apple', price: 5 }]), true);
  const session = Vendor.open(vendor, 1);
  assert.ok(session > 0);
  assert.equal(Vendor.activeSession(1), session);
  assert.equal(Vendor.shopOf(session), vendor);
  assert.equal(Vendor.purseOf(vendor), 500);
  const [show] = sent('vendorShow');
  assert.equal(show.id, 1);
  assert.equal(show.session, session);
  assert.equal(show.open, true);
  assert.equal(show.buys, true);
  assert.equal(show.npc, 0);
  assert.equal(show.revision, 3);
  assert.equal(show.purse, 500);
  assert.equal(show.money, MONEY.id);
  assert.deepEqual(
    show.rows.map((row) => [row[0], row[1], row[2], row[3]]),
    [
      ['s1', APPLE.id, 10, 1],
      ['s2', SWORD.id, 1, 2],
    ],
  );
  assert.deepEqual(show.prices, [15, 4000]);
  assert.deepEqual(show.buyPrices, [[APPLE.id, 5]]);
  // Unknown vendors and players answer as declared.
  assert.equal(Vendor.open(99, 1), 0);
  assert.equal(Vendor.open(vendor, 42), 0);
  assert.equal(Vendor.setShelf(99, []), false);
  assert.equal(Vendor.purseOf(99), null);
  assert.throws(() => Vendor.setShelf(vendor, [{ item: 'apple', amount: 0, price: 1 }]));
  assert.throws(() =>
    Vendor.setShelf(vendor, [
      { item: 'apple', amount: 1, price: 1 },
      { item: 'apple', amount: 2, price: 1 },
    ]),
  );
});

test('a row without a price takes the server price: the game price or the item config price', async () => {
  const { host, Vendor, Inventory, Items, sent } = await startedHost();
  join(host, 1);
  const vendor = Vendor.create({ name: 'Grocer' });
  const gamePrice = Inventory.itemPrice('apple').pristine;
  Vendor.setShelf(vendor, [{ item: 'apple', amount: 4 }]);
  Vendor.setOffers(vendor, [{ item: 'apple' }]);
  Vendor.open(vendor, 1);
  assert.deepEqual(sent('vendorShow').at(-1).prices, [gamePrice]);
  assert.deepEqual(sent('vendorShow').at(-1).buyPrices, [[APPLE.id, gamePrice]]);
  // Overridden, a row set again takes the override's price; a given price stays as given.
  Items.override('apple', { price: 990 });
  const overridden = Inventory.itemPrice('apple').pristine;
  assert.notEqual(overridden, gamePrice);
  Vendor.setShelf(vendor, [
    { item: 'apple', amount: 4 },
    { item: 'longSwordDuel', amount: 1, price: 4000 },
  ]);
  Vendor.setOffers(vendor, [{ item: 'apple' }]);
  assert.deepEqual(sent('vendorShow').at(-1).prices, [overridden, 4000]);
  assert.deepEqual(sent('vendorShow').at(-1).buyPrices, [[APPLE.id, overridden]]);
  assert.throws(() => Vendor.setShelf(vendor, [{ item: 'apple', amount: 1, price: -1 }]));
});

test('Inventory.beforeCommit sees a basket as it would settle: lines, prices and money', async () => {
  const { host, Vendor, Inventory } = await startedHost();
  const player = join(host, 1);
  Inventory.add(player, { item: 'money', amount: 100, notify: false });
  Inventory.add(player, { item: 'apple', amount: 4, notify: false });
  const vendor = Vendor.create({ purse: 50 });
  Vendor.setShelf(vendor, [{ item: SWORD.id, amount: 2, price: 60 }]);
  Vendor.setOffers(vendor, [{ item: 'apple', price: 5 }]);
  const session = Vendor.open(vendor, 1);
  const apples = rowOf(Inventory, player, 'apple');
  const revision = host.vendors.vendors.get(vendor).revision;
  const described = host.vendors.describeTrade(
    player,
    session,
    revision,
    [['s1', 1]],
    [[apples.id, 4]],
  );
  assert.deepEqual(described, {
    kind: 'vendor',
    vendor,
    money: -40,
    lines: [
      {
        item: SWORD.id,
        name: 'longSwordDuel',
        amount: 1,
        metadata: {},
        price: 60,
        from: 'vendor',
        to: 'player',
      },
      {
        metadata: {},
        item: APPLE.id,
        name: 'apple',
        amount: 4,
        price: 5,
        from: 'player',
        to: 'vendor',
      },
    ],
  });
  // A basket that cannot settle as it is has nothing to describe.
  assert.equal(host.vendors.describeTrade(player, session, revision + 1, [['s1', 1]], []), null);
  assert.equal(host.vendors.describeTrade(player, session, revision, [['s9', 1]], []), null);
});

test('a basket settles in one trade commit: money, sold units and bought units move together', async () => {
  const { host, Vendor, Inventory, events } = await startedHost();
  const player = join(host, 1);
  Inventory.add(player, { item: 'money', amount: 100, notify: false });
  Inventory.add(player, { item: 'apple', amount: 4, notify: false });
  const changes = [];
  host.on('playerInventoryChanged', (who, change) => changes.push(change));
  host.playerInventories.flush();
  changes.length = 0;
  const vendor = Vendor.create({ purse: 50 });
  Vendor.setShelf(vendor, [{ item: SWORD.id, amount: 2, price: 60 }]);
  Vendor.setOffers(vendor, [{ item: 'apple', price: 5 }]);
  const session = Vendor.open(vendor, 1);
  const apples = rowOf(Inventory, player, 'apple');
  const revision = host.vendors.vendors.get(vendor).revision;
  const result = host.vendors.trade(player, session, revision, [['s1', 1]], [[apples.id, 4]]);
  assert.deepEqual(result, { ok: true, code: '' });
  assert.deepEqual(rowsOf(Inventory, player).sort(), [
    ['longSwordDuel', 1],
    ['money', 60],
  ]);
  // Bought 60, sold 20: the player paid 40, which joins the purse; the sword left the shelf.
  assert.equal(Vendor.purseOf(vendor), 90);
  assert.deepEqual(
    host.vendors.vendors.get(vendor).shelf.map((row) => row.amount),
    [1],
  );
  const [trade] = events.filter((event) => event[0] === 'trade');
  assert.equal(trade[1], vendor);
  assert.equal(trade[2], 1);
  assert.deepEqual(trade[3], [
    { metadata: {}, item: SWORD.id, name: 'longSwordDuel', amount: 1, price: 60 },
  ]);
  assert.deepEqual(trade[4], [
    { metadata: {}, item: APPLE.id, name: 'apple', amount: 4, price: 5 },
  ]);
  assert.equal(trade[5], -40);
  host.playerInventories.flush();
  assert.equal(changes.length, 1, 'one commit');
  assert.equal(changes[0].reason, 'trade');
  // The shelf changed: a basket against the old revision is refused and nothing moves.
  const again = host.vendors.trade(player, session, revision, [['s1', 1]], []);
  assert.deepEqual(again, { ok: false, code: 'staleRevision' });
  assert.deepEqual(rowsOf(Inventory, player).sort(), [
    ['longSwordDuel', 1],
    ['money', 60],
  ]);
});

test('a basket the player or the purse cannot pay is refused whole', async () => {
  const { host, Vendor, Inventory, events } = await startedHost();
  const player = join(host, 1);
  Inventory.add(player, { item: 'money', amount: 10, notify: false });
  Inventory.add(player, { item: 'apple', amount: 20, notify: false });
  const vendor = Vendor.create({ purse: 30 });
  Vendor.setShelf(vendor, [{ item: SWORD.id, amount: 1, price: 500 }]);
  Vendor.setOffers(vendor, [{ item: 'apple', price: 2 }]);
  const session = Vendor.open(vendor, 1);
  const revision = host.vendors.vendors.get(vendor).revision;
  const apples = rowOf(Inventory, player, 'apple');
  assert.deepEqual(host.vendors.trade(player, session, revision, [['s1', 1]], []), {
    ok: false,
    code: 'insufficientMoney',
  });
  assert.deepEqual(host.vendors.trade(player, session, revision, [], [[apples.id, 20]]), {
    ok: false,
    code: 'vendorCannotPay',
  });
  assert.deepEqual(host.vendors.trade(player, session, revision, [['s9', 1]], []), {
    ok: false,
    code: 'unknownItem',
  });
  assert.deepEqual(host.vendors.trade(player, session, revision, [['s1', 2]], []), {
    ok: false,
    code: 'insufficientStock',
  });
  const money = rowOf(Inventory, player, 'money');
  assert.deepEqual(host.vendors.trade(player, session, revision, [], [[money.id, 1]]), {
    ok: false,
    code: 'notBought',
  });
  assert.deepEqual(rowsOf(Inventory, player).sort(), [
    ['apple', 20],
    ['money', 10],
  ]);
  assert.equal(Vendor.purseOf(vendor), 30);
  assert.equal(events.filter((event) => event[0] === 'trade').length, 0);
  // Another player cannot settle on this session.
  const other = join(host, 2);
  assert.deepEqual(host.vendors.trade(other, session, revision, [], [[apples.id, 1]]), {
    ok: false,
    code: 'noSession',
  });
  // A vendor that does not buy refuses every sale; a purse that never runs out pays anything.
  const shop = Vendor.create({ buys: false });
  const shopSession = Vendor.open(shop, 1);
  const shopRevision = host.vendors.vendors.get(shop).revision;
  assert.deepEqual(host.vendors.trade(player, shopSession, shopRevision, [], [[apples.id, 1]]), {
    ok: false,
    code: 'notBuying',
  });
  const bank = Vendor.create();
  Vendor.setOffers(bank, [{ item: 'apple', price: 1000 }]);
  const bankSession = Vendor.open(bank, 1);
  const bankRevision = host.vendors.vendors.get(bank).revision;
  assert.equal(Vendor.purseOf(bank), null);
  assert.deepEqual(host.vendors.trade(player, bankSession, bankRevision, [], [[apples.id, 20]]), {
    ok: true,
    code: '',
  });
  assert.deepEqual(rowsOf(Inventory, player), [['money', 20010]]);
});

test('sessions end with the reasons of vendorClosed and tell the client when it did not', async () => {
  const { host, Vendor, sent, events } = await startedHost();
  const player = join(host, 1);
  const first = Vendor.create();
  const second = Vendor.create();
  const a = Vendor.open(first, 1);
  // Another vendor replaces the session (2) without a close for the client: the new screen does.
  const b = Vendor.open(second, 1);
  assert.deepEqual(events.pop(), ['closed', first, 1, 2]);
  assert.equal(sent('vendorClose').length, 0);
  assert.equal(Vendor.shopOf(a), 0);
  // The player closed it (0); a second report is dropped.
  assert.equal(host.vendors.closed(player, b, 0), true);
  assert.deepEqual(events.pop(), ['closed', second, 1, 0]);
  assert.equal(host.vendors.closed(player, b, 0), false);
  // The client could not bring it up (3).
  const c = Vendor.open(first, 1);
  host.vendors.closed(player, c, 3);
  assert.deepEqual(events.pop(), ['closed', first, 1, 3]);
  // The server closes it (1) and tells the client.
  const d = Vendor.open(first, 1);
  assert.equal(Vendor.close(d), true);
  assert.equal(Vendor.close(d), false);
  assert.deepEqual(events.pop(), ['closed', first, 1, 1]);
  assert.deepEqual(sent('vendorClose').pop(), { op: 'vendorClose', id: 1, session: d });
  // Destroying the vendor closes its sessions (1).
  const e = Vendor.open(second, 1);
  assert.equal(Vendor.destroy(second), true);
  assert.equal(Vendor.destroy(second), false);
  assert.deepEqual(events.pop(), ['closed', second, 1, 1]);
  assert.equal(Vendor.activeSession(1), 0);
  assert.equal(Vendor.shopOf(e), 0);
  // The player leaving ends it (4).
  Vendor.open(first, 1);
  host.vendors.left(player);
  assert.deepEqual(events.pop(), ['closed', first, 1, 4]);
});

test('the shelf a session shows follows setShelf, setOffers and fillPurse', async () => {
  const { host, Vendor, sent } = await startedHost();
  join(host, 1);
  const vendor = Vendor.create({ purse: 10 });
  Vendor.open(vendor, 1);
  Vendor.setShelf(vendor, [{ item: 'apple', amount: 3, price: 2 }]);
  Vendor.fillPurse(vendor, null);
  const shows = sent('vendorShow');
  assert.equal(shows.length, 3);
  assert.equal(shows[1].open, false);
  assert.equal(shows[1].rows.length, 1);
  assert.equal(shows[2].purse, null);
  assert.equal(shows[2].revision, shows[1].revision, 'a purse leaves baskets valid');
  assert.throws(() => Vendor.fillPurse(vendor, -1));
});
