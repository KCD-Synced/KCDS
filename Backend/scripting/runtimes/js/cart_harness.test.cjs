'use strict';

// Cart.harness and Cart.unharness (carts/harness.cjs): real horses in a cart's shafts.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');
const { HARNESSED_WORLD } = require('./carts/harness.cjs');

async function setup(t) {
  const f = serverFixture(t);
  await f.host.startAll();
  const api = f.host.makeAPI();
  const driver = f.report(7);
  const ops = (op) => f.sent.filter((m) => m.op === op);
  // The host's own Horse class, which its checks know.
  return { f, api, driver, ops, Cart: api.Cart, Horse: f.host.api.Horse };
}

test('a cart spawned without horses says so and cannot be driven', async (t) => {
  const { f, driver, ops, Cart } = await setup(t);
  const cart = Cart.spawn('cart_b', { x: 0, y: 0, z: 0 }, {}, undefined, { harnessed: false });
  assert.equal(cart.harnessed, false);
  assert.deepEqual(cart.horses, []);
  assert.deepEqual(
    ops('cartHarness').map((m) => [m.id, m.looks]),
    [[cart.id, []]],
  );
  f.host.carts.enter(driver, cart.id, 'driver');
  assert.equal(cart.driver, null);
  assert.equal(ops('cartDeny').length, 1);
  // A cart with its own horses sends no harness and is driven as before.
  const own = Cart.spawn('cart_b');
  assert.equal(own.harnessed, true);
  assert.equal(ops('cartHarness').length, 1);
  assert.equal(own.unharness(), false);
  assert.throws(() => Cart.spawn('cart_b', {}, {}, undefined, { harnessed: 'no' }), TypeError);
});

test('harness takes near, free horses into the shafts and out of the world', async (t) => {
  const { f, driver, ops, Cart, Horse, api } = await setup(t);
  const cart = Cart.spawn('cart_b', { x: 0, y: 0, z: 0 }, {}, undefined, { harnessed: false });
  const brown = Horse.spawn({ x: 4, y: 0, z: 0 }, {}, '', 'Brown', undefined, {
    look: { body: Horse.spawn().look.body },
  });
  const far = Horse.spawn({ x: 40, y: 0, z: 0 });
  const seen = [];
  api.Events.on('cartHarness', (harnessed, horses) => seen.push(horses.map((h) => h.id)));
  assert.equal(cart.harness(far), false, 'too far');
  assert.equal(cart.harness([brown, far]), false, 'a cart takes one horse');
  assert.throws(() => cart.harness('Brown'), TypeError);
  assert.equal(cart.harness(brown), true);
  assert.deepEqual(seen, [[brown.id]]);
  assert.equal(cart.harnessed, true);
  assert.deepEqual(cart.horses, [brown]);
  assert.equal(brown.harnessedTo, cart);
  assert.equal(brown.dimension, HARNESSED_WORLD);
  const [, sent] = ops('cartHarness');
  assert.equal(sent.looks.length, 1);
  assert.match(sent.looks[0], /\|/);
  assert.equal(cart.harness(brown), false, 'it has its horse');
  // Now it is driven, and while driven its horse stays in.
  f.host.carts.enter(driver, cart.id, 'driver');
  assert.equal(cart.driver, driver);
  assert.equal(cart.unharness(), false);
});

test('unharness puts the horses beside the cart in their world; refusals hold', async (t) => {
  const { ops, Cart, Horse, api } = await setup(t);
  const wagon = Cart.spawn('wagon_b_covered', { x: 10, y: 20, z: 0 }, {}, 3, {
    harnessed: false,
  });
  const left = Horse.spawn({ x: 12, y: 20, z: 0 });
  const right = Horse.spawn({ x: 8, y: 20, z: 0 });
  left.setDimension(3);
  right.setDimension(3);
  const refuse = () => false;
  api.Events.on('cartHarness', refuse);
  assert.equal(wagon.harness([left, right]), false);
  assert.equal(left.dimension, 3);
  api.Events.off('cartHarness', refuse);
  const out = [];
  api.Events.on('cartUnharness', (unharnessed, horses) => out.push(horses.length));
  assert.equal(wagon.harness([left, left]), false, 'one horse twice');
  assert.equal(wagon.harness([left, right]), true);
  assert.equal(right.dimension, HARNESSED_WORLD);
  assert.equal(wagon.unharness(), true);
  assert.deepEqual(out, [2]);
  assert.equal(left.dimension, 3);
  assert.equal(left.harnessedTo, null);
  assert.ok(Math.hypot(left.position.x - 10, left.position.y - 20) < 5);
  assert.ok(Math.hypot(right.position.x - 10, right.position.y - 20) < 5);
  assert.equal(wagon.harnessed, false);
  assert.equal(ops('cartHarness').at(-1).looks.length, 0);
  assert.equal(wagon.unharness(), false);
});

test('a harnessed horse removed leaves the cart without horses; a removed cart lets them out', async (t) => {
  const { driver, f, Cart, Horse, ops } = await setup(t);
  const cart = Cart.spawn('cart_b', { x: 0, y: 0, z: 0 }, {}, undefined, { harnessed: false });
  const horse = Horse.spawn({ x: 2, y: 0, z: 0 });
  cart.harness(horse);
  f.host.carts.enter(driver, cart.id, 'driver');
  horse.destroy();
  assert.equal(cart.harnessed, false);
  assert.deepEqual(ops('cartHarness').at(-1).looks, []);
  assert.equal(ops('cartClimbOff').length + ops('cartLeave').length > 0, true);
  const other = Cart.spawn('cart_b', { x: 0, y: 0, z: 0 }, {}, undefined, { harnessed: false });
  const pony = Horse.spawn({ x: 2, y: 0, z: 0 });
  other.harness(pony);
  other.destroy();
  assert.equal(pony.harnessedTo, null);
  assert.equal(pony.dimension, 0xffffffff);
});
