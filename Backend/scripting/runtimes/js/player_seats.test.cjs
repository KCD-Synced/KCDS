'use strict';

// A player on a placed smart object's seat: their press of the game's seat key, the sit-down, the
// idle, the stand-up (smart_objects/player_seats.cjs).
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');

async function started() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-seats-'));
  const output = [];
  const host = new ResourceHost(root, {
    entityIds: { first: 4097, count: 2048 },
    send: (e) => output.push(e),
    log: () => {},
  });
  await host.startAll();
  const { SmartObject } = host.makeAPI();
  const player = host.updatePlayer({
    id: 7,
    nickname: 'Henry',
    ready: true,
    position: { x: 5.3, y: 1.2, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const orders = () => output.filter((e) => e.op === 'animation' && e.kind === 'play');
  const told = () =>
    output
      .filter((e) => e.op === 'clientEvent' || (e.op === 'event' && e.name === 'kcds:seat'))
      .map((e) => e);
  const finish = () => {
    const last = orders().at(-1);
    host.playerAnimations.report(player, {
      id: 7,
      request: last.request,
      reason: 'finished',
      exit: '',
    });
  };
  const press = (action = 'use_bed_sit') =>
    host.playerSeats.receive(player, 'kcds:seat', { op: 'use', action });
  return { host, output, SmartObject, player, orders, told, finish, press };
}

test('a press of the seat key near a placed bench seats the player with the game fragments', async () => {
  const { host, SmartObject, player, orders, finish, press } = await started();
  const bench = SmartObject.spawn({ blueprint: 'Bench/bench_2places_low', position: [5, 0, 0] });
  assert.equal(press(), true);
  // Put where the game's sit-down from the front starts (as an NPC is, npcs.test.cjs), then the
  // sit-down with the movement held.
  assert.ok(Math.abs(player.position.x - 5.35) < 1e-3 && Math.abs(player.position.y - 0.25) < 1e-3);
  let order = orders().at(-1);
  assert.equal(order.fragment, 'SitDown');
  assert.equal(order.tags, 'sitSideFront+sitPlaceBench+noTable');
  assert.equal(order.lockMovement, true);
  // The nearer seat (at 5.35, listed first) is taken, for NPCs as well.
  assert.deepEqual(
    host.objectUses.places(bench).map((place) => place.free),
    [false, true],
  );
  finish();
  order = orders().at(-1);
  assert.equal(order.fragment, 'SittingIdle');
  assert.equal(order.loop, true);
  // Seated: the player's game shows the stand-up hint.
  assert.equal(host.playerSeats.using.get(player).phase, 'using');
  // Standing up: the game's stand-up, then the seat is free again.
  assert.equal(host.playerSeats.receive(player, 'kcds:seat', { op: 'stand' }), true);
  assert.equal(orders().at(-1).fragment.length > 0, true);
  finish();
  assert.equal(host.playerSeats.using.has(player), false);
  assert.deepEqual(
    host.objectUses.places(bench).map((place) => place.free),
    [true, true],
  );
});

test('nothing happens far from a placed seat, for another key, or for a level seat', async () => {
  const { host, SmartObject, player, orders, press } = await started();
  SmartObject.spawn({ blueprint: 'Bench/bench_2places_low', position: [50, 0, 0] });
  assert.equal(press(), true, "the event is the module's own");
  assert.equal(host.playerSeats.using.has(player), false);
  assert.equal(host.playerSeats.receive(player, 'kcds:seat', { op: 'use', action: 'use' }), true);
  assert.equal(host.playerSeats.using.has(player), false);
  assert.equal(orders().length, 0);
  // Another built-in event is not this module's.
  assert.equal(host.playerSeats.receive(player, 'kcds:tabletop', { op: 'sit' }), false);
});

test('a placed seat holds one body, and its removal stands the player up', async () => {
  const { host, SmartObject, player, finish, press } = await started();
  const bench = SmartObject.spawn({ blueprint: 'Bench/bench_1place_low', position: [5, 0, 0] });
  const npc = host.npcs.Npc.create({ position: { x: 5, y: 3, z: 0 } });
  assert.equal(press(), true);
  finish();
  assert.equal(npc.useObject(bench), false, 'the player sits there');
  bench.destroy();
  assert.equal(host.playerSeats.using.has(player), false);
});

test('a player who leaves frees the seat', async () => {
  const { host, SmartObject, press } = await started();
  const bench = SmartObject.spawn({ blueprint: 'Bench/bench_1place_low', position: [5, 0, 0] });
  press();
  assert.deepEqual(
    host.objectUses.places(bench).map((place) => place.free),
    [false],
  );
  host.disconnect(7);
  assert.deepEqual(
    host.objectUses.places(bench).map((place) => place.free),
    [true],
  );
});
