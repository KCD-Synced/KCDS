'use strict';

// Haggling with a server vendor: the vendor's own rule (the game's negotiation
// without its dice), script answers through vendorHaggle, and a basket settled at the agreed price.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const haggling = require('./vendors/haggle.cjs');

const OPTIONS = { tolerance: 0.1, rounds: 5 };

async function startedHost() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-vendor-haggle-'));
  const output = [];
  const host = new ResourceHost(root, {
    entityIds: { first: 4097, count: 2048 },
    send: (message) => output.push(message),
    log: () => {},
  });
  await host.startAll();
  const sent = (op) => output.filter((message) => message.op === op);
  const trades = [];
  host.on('vendorTrade', (vendor, player, bought, sold, balance, haggled) => {
    trades.push({ vendor, player: player.id, bought, sold, balance, haggled });
  });
  const api = host.makeAPI();
  return { host, Vendor: api.Vendor, Inventory: api.Inventory, sent, trades };
}

function join(host, id) {
  return host.updatePlayer({
    id,
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
}

// A vendor selling one sword at 1000 to a player with 2000 money, its session and shelf revision.
async function swordShop(haggle = true) {
  const started = await startedHost();
  const { host, Vendor, Inventory } = started;
  const player = join(host, 1);
  Inventory.add(player, { item: 'money', amount: 2000, notify: false });
  const vendor = Vendor.create({ name: 'Armourer', haggle });
  Vendor.setShelf(vendor, [{ item: 'longSwordDuel', amount: 1, price: 1000 }]);
  const session = Vendor.open(vendor, 1);
  const revision = started.sent('vendorShow').at(-1).revision;
  const offer = (round, offered) => {
    return host.vendors.haggle(player, session, revision, round, offered, [['s1', 1]], []);
  };
  return { ...started, player, vendor, session, revision, offer };
}

test('haggle options: true takes the defaults; bad tolerances and rounds throw', async () => {
  assert.equal(haggling.haggleOptionsOf(undefined), null);
  assert.equal(haggling.haggleOptionsOf(false), null);
  assert.deepEqual(haggling.haggleOptionsOf(true), { tolerance: 0.1, rounds: 5 });
  assert.deepEqual(haggling.haggleOptionsOf({ rounds: 3 }), { tolerance: 0.1, rounds: 3 });
  assert.throws(() => haggling.haggleOptionsOf({ tolerance: 0.5 }), RangeError);
  assert.throws(() => haggling.haggleOptionsOf({ rounds: 0 }), RangeError);
  assert.throws(() => haggling.haggleOptionsOf('yes'), TypeError);
  const { Vendor } = await startedHost();
  assert.throws(() => Vendor.create({ haggle: { tolerance: -1 } }), RangeError);
});

test('the rule: an offer at the asked price is taken; a far one gets a counter, never below the floor', () => {
  // A 1000 basket: steps of 50, the floor at 900. Dominance 0 (Speech 15).
  const haggle = haggling.startHaggle('k', 1000, 1000);
  haggle.round = 1;
  assert.deepEqual(haggling.ruleAnswer(haggle, 1000, OPTIONS, 0, null), {
    answer: haggling.ACCEPT,
    price: 1000,
    mood: 0,
  });
  const counter = haggling.ruleAnswer(haggle, 800, OPTIONS, 0, null);
  assert.equal(counter.answer, haggling.COUNTER);
  assert.equal(counter.price, 950);
  assert.equal(counter.mood, 68);
  // The second round: the vendor came down to 950; an offer under the floor gets the floor.
  haggle.asked = 950;
  haggle.lastOffer = 800;
  haggle.impatience = counter.mood / 100;
  haggle.round = 2;
  const floor = haggling.ruleAnswer(haggle, 850, OPTIONS, 0, null);
  assert.equal(floor.answer, haggling.COUNTER);
  assert.equal(floor.price, 900);
  // A step from the vendor's price, at or above the floor: taken.
  assert.equal(haggling.ruleAnswer(haggle, 900, OPTIONS, 0, null).answer, haggling.ACCEPT);
  // The same offer again is mocked, here past the trader's patience: refused.
  assert.equal(haggling.ruleAnswer(haggle, 800, OPTIONS, 0, null).answer, haggling.REFUSE);
});

test('the rule: the last round refuses, and the purse bounds what the vendor pays', () => {
  const haggle = haggling.startHaggle('k', 1000, 1000);
  haggle.round = 2;
  assert.equal(
    haggling.ruleAnswer(haggle, 800, { tolerance: 0.1, rounds: 2 }, 0, null).answer,
    haggling.REFUSE,
  );
  // The player sells for 1000 (price -1000); the vendor's purse holds 950.
  const selling = haggling.startHaggle('k', -1000, 1000);
  selling.round = 1;
  const answer = haggling.ruleAnswer(selling, -1200, OPTIONS, 0, 950);
  assert.equal(answer.answer, haggling.COUNTER);
  assert.ok(answer.price >= -950);
  assert.equal(haggling.dominanceOf(0), -1);
  assert.equal(haggling.dominanceOf(15), 0);
  assert.equal(haggling.dominanceOf(30), 1);
});

test('a vendor that haggles says so on the screen of a session kept by an NPC', async () => {
  const { host, Vendor, sent } = await startedHost();
  join(host, 1);
  const npc = host.npcs.Npc.create({ name: 'Armourer', position: { x: 0, y: 0, z: 0 } });
  const haggles = Vendor.create({ haggle: { rounds: 3 } });
  const plain = Vendor.create({});
  Vendor.open(haggles, 1);
  assert.equal(sent('vendorShow').at(-1).haggles, false);
  assert.ok(Vendor.open(haggles, 1, npc) > 0);
  assert.equal(sent('vendorShow').at(-1).haggles, true);
  Vendor.open(plain, 1);
  assert.equal(sent('vendorShow').at(-1).haggles, false);
});

test('an agreed haggle settles the same basket at the agreed price', async () => {
  const { offer, sent, trades, host, player, session, revision, Inventory } = await swordShop();
  // Speech 0 without a progression report: dominance -1. 950 is a step below the price: taken.
  const agreed = offer(1, 950);
  assert.equal(agreed.answer, haggling.ACCEPT);
  assert.equal(agreed.price, 950);
  const reply = sent('vendorHaggleReply').at(-1);
  assert.deepEqual(
    [reply.id, reply.session, reply.round, reply.answer, reply.price],
    [1, session, 1, 1, 950],
  );
  assert.deepEqual(host.vendors.trade(player, session, revision, [['s1', 1]], []), {
    ok: true,
    code: '',
  });
  assert.equal(trades.length, 1);
  assert.equal(trades[0].balance, -950);
  assert.equal(trades[0].haggled, true);
  assert.equal(trades[0].bought[0].price, 1000);
  const money = Inventory.get(player).items.find((row) => row.name === 'money');
  assert.equal(money.amount, 1050);
});

test('a refused haggle ends the haggling of the session; the list price still trades', async () => {
  const { offer, trades, host, player, session, revision } = await swordShop();
  assert.equal(offer(1, 600).answer, haggling.REFUSE);
  assert.equal(offer(2, 990).answer, haggling.REFUSE);
  host.vendors.trade(player, session, revision, [['s1', 1]], []);
  assert.equal(trades[0].balance, -1000);
  assert.equal(trades[0].haggled, false);
});

test('a vendor without haggle refuses; another basket starts a new haggle', async () => {
  const plain = await swordShop(false);
  assert.equal(plain.offer(1, 990).answer, haggling.REFUSE);
  const { host, player, session, revision, offer, Vendor, vendor } = await swordShop();
  Vendor.setShelf(vendor, [
    { item: 'longSwordDuel', amount: 1, price: 1000 },
    { item: 'apple', amount: 5, price: 10 },
  ]);
  const newRevision = revision + 1;
  // The old shelf's basket no longer prices: refused.
  assert.equal(offer(1, 990).answer, haggling.REFUSE);
  const result = host.vendors.haggle(player, session, newRevision, 1, 2000, [['s3', 1]], []);
  assert.equal(result.answer, haggling.ACCEPT);
  assert.equal(result.price, 2000);
});

test('vendorHaggle answers first: accept, counter with a mood, refuse', async () => {
  const { host, offer, sent } = await swordShop();
  const heard = [];
  const answers = [{ counter: 990, mood: 0.5 }, true];
  host.on('vendorHaggle', (vendor, player, basket, offered) => {
    heard.push([basket.price, basket.balance, basket.round, basket.asked, offered]);
    return answers.shift();
  });
  const counter = offer(1, 700);
  assert.deepEqual(counter, { answer: haggling.COUNTER, price: 990, mood: 50 });
  assert.equal(sent('vendorHaggleReply').at(-1).mood, 50);
  const accept = offer(2, 980);
  assert.deepEqual(accept, { answer: haggling.ACCEPT, price: 980, mood: 50 });
  assert.deepEqual(heard, [
    [1000, -1000, 1, 1000, 700],
    [1000, -1000, 2, 990, 980],
  ]);
});

test('vendorHaggle false refuses; an unusable answer leaves the offer to the rule', async () => {
  const refusing = await swordShop();
  refusing.host.on('vendorHaggle', () => false);
  assert.equal(refusing.offer(1, 990).answer, haggling.REFUSE);
  const { host, offer } = await swordShop();
  host.on('vendorHaggle', () => ({ counter: 'lots' }));
  assert.equal(offer(1, 950).answer, haggling.ACCEPT);
});
