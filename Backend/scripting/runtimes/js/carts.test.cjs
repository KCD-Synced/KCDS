'use strict';

// Cart: the carts and wagons the server puts in the world.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

async function setup(t) {
  const f = serverFixture(t);
  await f.host.startAll();
  const api = f.host.makeAPI();
  const a = f.report(7);
  const b = f.report(8);
  const log = [];
  for (const name of ['cartSpawn', 'cartDestroy', 'cartEnter', 'cartExit']) {
    api.Events.on(name, (cart, player, seat) => log.push([name, cart.id, player?.id, seat]));
  }
  const ops = (op) => f.sent.filter((m) => m.op === op);
  // The player's game reports that their current animation (a climb) ended.
  const finish = (player) => {
    const request = f.host.playerAnimations.stateOf(player).current;
    f.host.playerAnimations.report(player, { request: request.id, reason: 'finished' });
  };
  return { f, api, a, b, log, ops, finish, Cart: api.Cart };
}

test('spawn replicates a cart, which is found and listed per world', async (t) => {
  const { log, ops, Cart } = await setup(t);
  const cart = Cart.spawn('wagon_b_covered', { x: 1, y: 2, z: 3 }, { x: 0, y: 0, z: 90 }, 4);
  assert.equal(cart.blueprint, 'wagon_b_covered');
  assert.equal(cart.horseCount, 2);
  assert.deepEqual(cart.seats, [
    'driver',
    'bench',
    'rightBack',
    'leftBack',
    'rightFront',
    'leftFront',
    'rightMiddle',
    'leftMiddle',
  ]);
  assert.equal(cart.pace, 'stand');
  assert.equal(cart.maxSpeed, 7);
  assert.equal(cart.dimension, 4);
  assert.equal(cart.position.z, 3);
  assert.equal(cart.driver, null);
  assert.equal(Cart.getById(cart.id), cart);
  assert.equal(new Cart(cart.id), cart);
  assert.equal(Cart.all(4).length, 1);
  assert.equal(Cart.all(5).length, 0);
  const [spawn] = ops('cartSpawn');
  assert.equal(spawn.id, cart.id);
  assert.equal(spawn.dimension, 4);
  assert.deepEqual(log, [['cartSpawn', cart.id, undefined, undefined]]);
  assert.equal(Cart.spawn().blueprint, 'wagon_b_covered');
  assert.equal(Cart.spawn('cart_a').horseCount, 0);
  assert.equal(Cart.spawn('wagon_b_covered_empty').horseCount, 2);
  assert.equal(Cart.spawn('cart_b').horseCount, 1);
  assert.deepEqual(Cart.spawn('cart_b').seats, ['driver']);
  assert.throws(() => Cart.spawn('hovercraft'), RangeError);
  assert.throws(() => Cart.spawn('cart_a', { x: NaN }), TypeError);
  // Every cart and wagon prefab of the game, and cart_a.
  const blueprints = Cart.blueprints();
  assert.equal(blueprints.length, 28);
  for (const name of ['cart_b', 'wagon_a_empty', 'wagon_b', 'wagon_c_hay_large', 'cart_a']) {
    assert.ok(blueprints.includes(name), name);
  }
});

test('seatPlayer seats one player per seat and unseatPlayer lets them out', async (t) => {
  const { a, b, log, ops, Cart } = await setup(t);
  const cart = Cart.spawn('cart_a');
  log.length = 0;
  assert.equal(cart.seatPlayer(a, 'driver'), true);
  assert.equal(cart.driver, a);
  assert.equal(cart.seatOf(a), 'driver');
  assert.equal(cart.whoSits('driver'), a);
  assert.equal(cart.seatPlayer(b, 'driver'), false);
  assert.throws(() => cart.seatPlayer(b, 'bench'), RangeError);
  assert.equal(cart.seatPlayer(b, 'leftBack'), true);
  assert.deepEqual(
    ops('cartSeat').map((m) => [m.player, m.seat]),
    [
      [7, 'driver'],
      [8, 'leftBack'],
    ],
  );
  assert.equal(cart.unseatPlayer(a), true);
  assert.equal(cart.unseatPlayer(a), false);
  assert.equal(cart.driver, null);
  assert.equal(ops('cartLeave').length, 1);
  assert.deepEqual(
    log.map((e) => e[0]),
    ['cartEnter', 'cartEnter', 'cartExit'],
  );
});

test('a player moves between carts, and a refusing handler blocks the seat', async (t) => {
  const { api, a, log, ops, Cart } = await setup(t);
  const one = Cart.spawn();
  const two = Cart.spawn();
  one.seatPlayer(a, 'bench');
  assert.equal(two.seatPlayer(a, 'driver'), true);
  assert.equal(one.seatOf(a), null);
  assert.equal(two.seatOf(a), 'driver');
  api.Events.on('cartEntering', (cart, player, seat) => seat !== 'bench');
  log.length = 0;
  assert.equal(one.seatPlayer(a, 'bench'), false);
  assert.equal(two.seatOf(a), 'driver');
  assert.deepEqual(log, []);
  assert.equal(ops('cartDeny').length, 0);
});

test('the player game asks over the bridge, and a refusal is answered', async (t) => {
  const { f, a, ops, finish, Cart } = await setup(t);
  const cart = Cart.spawn();
  f.host.carts.enter(a, cart.id, 'driver');
  assert.equal(cart.driver, a);
  const other = f.host.players.get(8);
  f.host.carts.enter(other, cart.id, 'driver');
  assert.deepEqual(
    ops('cartDeny').map((m) => [m.id, m.player, m.seat]),
    [[cart.id, 8, 'driver']],
  );
  f.host.carts.drive(a, cart.id, { x: 5, y: 6, z: 7 }, { x: 0, y: 0, z: 0, w: 1 }, 'trot');
  assert.equal(cart.pace, 'trot');
  assert.equal(cart.position.x, 5);
  f.host.carts.drive(other, cart.id, { x: 9, y: 9, z: 9 }, { w: 1 }, 'walk');
  assert.equal(cart.pace, 'trot');
  // The other games follow the driver's report, sent once per change.
  f.host.carts.drive(a, cart.id, { x: 5, y: 6, z: 7 }, { x: 0, y: 0, z: 0, w: 1 }, 'trot');
  assert.deepEqual(
    ops('cartPose').map((m) => [m.id, m.position.x, m.pace, m.driver]),
    [[cart.id, 5, 'trot', 7]],
  );
  // The driver climbs down first (WagonOut), then is let down.
  f.host.carts.leave(a, cart.id);
  assert.equal(cart.driver, a);
  finish(a);
  assert.equal(cart.driver, null);
  assert.equal(cart.pace, 'stand');
  // Every game is told, the leaving player's own too (it climbs down on the order).
  assert.equal(ops('cartLeave').length, 1);
});

test('teleport and maxSpeed validate and send only on change', async (t) => {
  const { ops, Cart } = await setup(t);
  const cart = Cart.spawn();
  assert.equal(cart.teleport({ x: 1, y: 2, z: 3 }), true);
  assert.equal(cart.position.y, 2);
  assert.equal(cart.teleport({ x: Infinity, y: 0, z: 0 }), false);
  assert.equal(ops('cartTeleport').length, 1);
  cart.maxSpeed = 20;
  cart.maxSpeed = 7;
  assert.equal(cart.maxSpeed, 7);
  assert.equal(ops('cartSpeed').length, 0);
  cart.maxSpeed = 10;
  cart.maxSpeed = 10;
  assert.equal(cart.maxSpeed, 10);
  assert.equal(ops('cartSpeed').length, 1);
});

test('load: only the loads of its blueprint, sent on change and to an arriving player', async (t) => {
  const { f, a, ops, Cart } = await setup(t);
  const wagon = Cart.spawn();
  assert.deepEqual(wagon.loads, []);
  assert.throws(() => {
    wagon.load = 'goods';
  }, RangeError);
  const cart = Cart.spawn('cart_b');
  assert.deepEqual(cart.loads, ['goods', 'goods2', 'sacks']);
  assert.equal(cart.load, '');
  cart.load = 'sacks';
  cart.load = 'sacks';
  assert.equal(cart.load, 'sacks');
  assert.deepEqual(
    ops('cartLoad').map((m) => [m.id, m.load]),
    [[cart.id, 'sacks']],
  );
  assert.throws(() => {
    cart.load = 'things';
  }, RangeError);
  f.host.carts.arrived(a);
  assert.deepEqual(
    ops('cartLoad').map((m) => [m.load, m.to]),
    [
      ['sacks', undefined],
      ['sacks', a.id],
    ],
  );
  cart.load = '';
  assert.equal(ops('cartLoad').at(-1).load, '');
});

test('storage: a container on the cart, opened in its reach, gone with the cart', async (t) => {
  const { f, a, ops, Cart } = await setup(t);
  const far = { x: a.position.x + 20, y: a.position.y, z: a.position.z };
  const cart = Cart.spawn('cart_b', far);
  assert.equal(cart.storage, null);
  const stock = cart.addStorage();
  assert.equal(cart.addStorage(), stock);
  assert.equal(cart.storage, stock);
  // Every client puts it on the cart: its bodyOf names the cart; it has no chest of its own.
  assert.deepEqual(
    ops('stashSpawn').map((m) => [m.id, m.bodyOf]),
    [[stock.id, cart.id]],
  );
  assert.equal(stock.position.x, far.x);
  // A script cannot take it from its cart, and opens it only within reach of the cart.
  stock.destroy();
  assert.equal(f.host.stashes.stashes.has(stock.id), true);
  assert.equal(stock.open(a), false);
  cart.teleport(a.position);
  assert.equal(stock.position.x, a.position.x);
  assert.equal(stock.open(a), true);
  assert.ok(ops('stashShow').some((m) => m.stash === stock.id && m.open));
  cart.destroy();
  assert.equal(f.host.stashes.stashes.has(stock.id), false);
  assert.equal(ops('stashDestroy').length, 1);
  const wagon = Cart.spawn('wagon_b_covered');
  wagon.addStorage();
  wagon.removeStorage();
  assert.equal(wagon.storage, null);
  assert.equal(ops('stashDestroy').length, 2);
});

test('destroy lets everyone out first; a disconnect climbs down', async (t) => {
  const { f, a, b, log, ops, Cart } = await setup(t);
  const cart = Cart.spawn();
  cart.seatPlayer(a, 'driver');
  cart.seatPlayer(b, 'bench');
  log.length = 0;
  f.host.disconnect(8);
  assert.equal(cart.seatOf(b), null);
  assert.deepEqual(log, [['cartExit', cart.id, 8, 'bench']]);
  log.length = 0;
  cart.destroy();
  assert.deepEqual(
    log.map((e) => e[0]),
    ['cartExit', 'cartDestroy'],
  );
  assert.equal(ops('cartDestroy').length, 1);
  assert.equal(Cart.getById(cart.id), null);
  Cart.spawn();
  assert.equal(Cart.destroyAll(), 1);
});

test('a player climbs onto their seat, takes its pose and climbs down', async (t) => {
  const { f, a, b, ops, finish, Cart, api } = await setup(t);
  const cart = Cart.spawn();
  const animations = () => ops('animation').map((m) => [m.id, m.kind, m.fragment, m.tags]);
  cart.seatPlayer(a, 'driver');
  cart.seatPlayer(b, 'bench');
  // Their games walk them to where the climb starts first.
  assert.deepEqual(animations(), []);
  f.host.carts.atStart(a, cart.id);
  f.host.carts.atStart(b, cart.id);
  // As the game's own wagons: the climb first, then the driver holds the reins and the bench is
  // the co-driver's; the driver's movement keys drive, the co-driver does not walk.
  assert.deepEqual(animations(), [
    [7, 'play', 'WagonIn', 'driver'],
    [8, 'play', 'WagonIn', 'player+codriver'],
  ]);
  // Held on the seat, and the reins, only once the climb has ended.
  assert.equal(ops('cartSettled').length, 0);
  finish(a);
  finish(b);
  assert.deepEqual(
    ops('cartSettled').map((m) => [m.player, m.seat]),
    [
      [7, 'driver'],
      [8, 'bench'],
    ],
  );
  assert.deepEqual(animations().slice(2), [
    [7, 'play', 'WagonIdle', 'cart+driver'],
    [8, 'play', 'WagonIdle', 'cart+player+rightBack'],
  ]);
  const [drive, sit] = ops('animation').slice(2);
  assert.equal(drive.lockMovement, true);
  assert.equal(sit.loop, true);
  assert.equal(sit.lockMovement, true);
  // A script lets them out at once; the pose stops.
  cart.unseatPlayer(b);
  assert.deepEqual(animations().at(-1), [8, 'stop', undefined, undefined]);
  // Their own game's leave: the climb down, then out.
  cart.seatPlayer(b, 'rightBack');
  f.host.carts.atStart(b, cart.id);
  finish(b);
  assert.deepEqual(animations().at(-1), [8, 'play', 'WagonIdle', 'cart+player+rightBack']);
  f.host.carts.leave(b, cart.id);
  assert.deepEqual(animations().at(-1), [8, 'play', 'WagonOut', 'cart+player+rightBack']);
  // The seat lets go as the climb down starts, not when it ends.
  assert.deepEqual(
    ops('cartClimbOff').map((m) => [m.player, m.seat]),
    [[8, 'rightBack']],
  );
  assert.equal(cart.seatOf(b), 'rightBack');
  finish(b);
  assert.equal(cart.seatOf(b), null);
  // A handler's own animation on entering stays when the passenger climbs down.
  api.Events.on('cartEnter', (entered, player) => player.playAnimation('Cheer'));
  cart.seatPlayer(b, 'rightBack');
  const before = ops('animation').length;
  cart.unseatPlayer(b);
  assert.equal(ops('animation').length, before);
  // The driver of a cart without horses leads it on foot.
  const empty = Cart.spawn('cart_a');
  empty.seatPlayer(b, 'driver');
  assert.deepEqual(animations().at(-1), [8, 'play', 'Cheer', '']);
});

test('a cart carries its own hints: the reins on its horses, getting on at its body', async (t) => {
  const { f, a, b, finish, Cart } = await setup(t);
  const registry = f.host.worldInteractions;
  const seen = (player) => registry.listFor(player).map((hint) => [hint.entity, hint.text]);
  const wagon = Cart.spawn('wagon_b_covered');
  const horse = (n) => `kcds_cart_${wagon.id}_horse_${n}`;
  const body = `kcds_cart_${wagon.id}_6`;
  assert.deepEqual(seen(a), [
    [horse(0), 'Drive'],
    [horse(1), 'Drive'],
    [body, 'Get on'],
  ]);
  const fire = (player, entity, text) => {
    const hint = registry.listFor(player).find((h) => h.entity === entity && h.text === text);
    registry.fired(player, hint.id);
  };
  // The reins: taken, then only the driver sees Let go and nobody sees Drive.
  fire(a, horse(0), 'Drive');
  assert.equal(wagon.driver, a);
  // The driver rides too: Get on is not for them.
  assert.deepEqual(seen(a), [
    [horse(0), 'Let go'],
    [horse(1), 'Let go'],
  ]);
  assert.deepEqual(seen(b), [[body, 'Get on']]);
  // Getting on: the first free passenger seat; the rider no longer sees Get on.
  fire(b, body, 'Get on');
  assert.equal(wagon.seatOf(b), 'bench');
  assert.deepEqual(seen(b), []);
  // A rider gets down with their use key (their game's report), the driver with Let go.
  f.host.carts.leave(b, wagon.id);
  finish(b);
  assert.equal(wagon.seatOf(b), null);
  fire(a, horse(1), 'Let go');
  finish(a);
  assert.equal(wagon.driver, null);
  assert.deepEqual(seen(b), seen(a));
  // A cart is led on foot and has no passenger seat.
  const cart = Cart.spawn('cart_b');
  assert.equal(cart.driverWalks, true);
  assert.ok(
    seen(a).some(([entity, text]) => entity === `kcds_cart_${cart.id}_horse_0` && text === 'Lead'),
  );
  // A loaded wagon seats only the driver and the co-driver.
  assert.deepEqual(Cart.spawn('wagon_b_covered_sacks').seats, ['driver', 'bench']);
  // Destroying the carts takes their hints away.
  Cart.destroyAll();
  assert.equal(registry.interactions.size, 0);
});
