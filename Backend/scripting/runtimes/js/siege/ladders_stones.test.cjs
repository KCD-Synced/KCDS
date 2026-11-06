'use strict';

// Siege ladders and stone piles.
const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../../Dev/testkit/guide_scenarios/fixtures.cjs');
const { damageAt } = require('./stones.cjs');

// A host with the given server-side events listened to; `events` collects [name, ...args].
async function started(t) {
  const fixture = serverFixture(t);
  await fixture.host.startAll();
  const api = fixture.host.makeAPI();
  const events = [];
  const listen = (...names) => {
    for (const name of names) {
      fixture.host.handlers.set(name, [
        { active: true, owner: null, fn: (...args) => events.push([name, ...args]) },
      ]);
    }
  };
  const ops = (...names) => fixture.sent.filter((m) => names.includes(m.op));
  return { ...fixture, api, events, listen, ops };
}

test('a ladder is placed with its kind, pushed, felled and raised again', async (t) => {
  const { api, host, report, events, listen, ops } = await started(t);
  listen('siegeLadderSpawn', 'siegeLadderFall', 'siegeLadderRaise', 'siegeLadderDestroy');
  report(7);
  const player = host.players.get(7);
  const ladder = api.SiegeLadder.spawn('tall', { x: 1, y: 2, z: 3 }, { x: 0, y: 0, z: 90 });
  assert.equal(ladder.kind, 'tall');
  assert.equal(ladder.height, 5.5);
  assert.equal(ladder.pose, 'standing');
  assert.equal(ladder.climbable, true);
  assert.equal(ladder.usable, true);
  assert.equal(api.SiegeLadder.getById(ladder.id), ladder);
  assert.equal(new api.Entity(ladder.id), ladder);
  assert.equal(ops('siegeLadderSpawn')[0].dimension, 0xffffffff);
  assert.equal(api.SiegeLadder.describe('siege').height, 9.75);
  assert.equal(api.SiegeLadder.describe('nope'), null);
  assert.throws(() => api.SiegeLadder.spawn('nope', {}), RangeError);

  const pushed = ladder.push(player);
  assert.deepEqual(pushed, { accepted: true, reason: '', seconds: 4.5 });
  assert.equal(ladder.pose, 'pushed');
  assert.equal(ladder.climbable, false);
  assert.equal(ladder.pusher, player);
  assert.equal(ladder.push(player).reason, 'is not standing');
  assert.equal(ladder.raise(player).reason, 'is not down');

  // A report before the fall is due is ignored; one at the due time ends it.
  host.siegeLadders.settled(player, ladder.id, 'down');
  assert.equal(ladder.pose, 'pushed');
  host.siegeLadders.now = () => Date.now() + 5000;
  host.siegeLadders.settled(player, ladder.id, 'down');
  assert.equal(ladder.pose, 'down');
  assert.deepEqual(
    events.filter((e) => e[0] === 'siegeLadderFall').map((e) => [e[1], e[2]]),
    [[ladder, player]],
  );

  assert.equal(ladder.raise(player).seconds, 5.7);
  host.siegeLadders.now = () => Date.now() + 20000;
  host.siegeLadders.settled(player, ladder.id, 'standing');
  assert.equal(ladder.pose, 'standing');
  assert.equal(events.filter((e) => e[0] === 'siegeLadderRaise').length, 1);
  assert.deepEqual(
    ops('siegeLadderPose').map((m) => [m.pose, m.actor]),
    [
      ['pushed', 7],
      ['down', 7],
      ['raising', 7],
      ['standing', 7],
    ],
  );
  assert.equal(api.SiegeLadder.destroyAll(), 1);
  assert.equal(ops('siegeLadderDestroy').length, 1);
  api.SiegeLadder.destroyAll();
  assert.equal(ops('siegeLadderDestroy').length, 1);
});

test('a broken ladder cannot be pushed or raised until it is repaired; flags send only on change', async (t) => {
  const { api, ops } = await started(t);
  const ladder = api.SiegeLadder.spawn('low', {});
  ladder.break();
  assert.equal(ladder.pose, 'broken');
  assert.equal(ladder.push().reason, 'is broken');
  assert.equal(ladder.raise().reason, 'is broken');
  ladder.break();
  ladder.usable = false;
  ladder.usable = false;
  ladder.repair();
  ladder.repair();
  assert.equal(ladder.pose, 'standing');
  assert.equal(ops('siegeLadderPose').length, 3);
  ladder.destroy();
});

test('an overdue step is ended by the shared sweep, with no report', async (t) => {
  const { api, host, events, listen } = await started(t);
  listen('siegeLadderFall');
  const ladder = api.SiegeLadder.spawn('short', {});
  ladder.push();
  assert.ok(host.siegeLadders.timer, 'one fallback timer is armed');
  host.siegeLadders.sweep(Date.now() + 1000);
  assert.equal(ladder.pose, 'pushed');
  host.siegeLadders.sweep(Date.now() + 10000);
  assert.equal(ladder.pose, 'down');
  assert.equal(events[0][2], null);
  ladder.destroy();
  assert.equal(host.siegeLadders.timer, null);
});

test('a player asks to push; the event can refuse it, and a far player is refused', async (t) => {
  const { api, host, report, ops } = await started(t);
  report(7);
  report(8, { position: { x: 500, y: 0, z: 0 } });
  const ladder = api.SiegeLadder.spawn('low', { x: 0, y: 0, z: 0 });
  host.siegeLadders.use(host.players.get(8), ladder.id, 'push');
  assert.equal(ladder.pose, 'standing');
  assert.deepEqual(
    ops('siegeLadderRefused').map((m) => m.id),
    [8],
  );

  let refuse = true;
  host.handlers.set('siegeLadderUse', [{ active: true, owner: null, fn: () => !refuse }]);
  host.siegeLadders.use(host.players.get(7), ladder.id, 'push');
  assert.equal(ladder.pose, 'standing');
  refuse = false;
  host.siegeLadders.use(host.players.get(7), ladder.id, 'push');
  assert.equal(ladder.pose, 'pushed');
  assert.equal(ladder.pusher, host.players.get(7));
  assert.equal(ops('siegeLadderRefused').length, 2);
  ladder.destroy();
});

test('a stone pile counts its stones, offers a prompt, and refills when set', async (t) => {
  const { api, host, report, events, listen, ops } = await started(t);
  listen('stonePileSpawn', 'stonePileThrow', 'stonePileDestroy');
  report(7);
  const player = host.players.get(7);
  const pile = api.StonePile.spawn({ x: 0, y: 0, z: 0 }, {}, 2);
  assert.equal(pile.stones, -1);
  assert.equal(pile.damage, 60);
  assert.equal(pile.blastRadius, 1.5);
  assert.equal(new api.Entity(pile.id), pile);
  pile.stones = 2;
  pile.stones = 2;
  host.stonePiles.take(player, pile.id);
  // The player is in the global world, the pile in world 2: the take is not honoured.
  assert.equal(pile.stones, 2);
  const near = api.StonePile.spawn({ x: 0, y: 0, z: 0 });
  near.stones = 2;
  host.stonePiles.take(player, near.id);
  host.stonePiles.take(player, near.id);
  host.stonePiles.take(player, near.id);
  assert.equal(near.stones, 0);
  assert.equal(events.filter((e) => e[0] === 'stonePileThrow').length, 2);
  near.stones = -5;
  assert.equal(near.stones, -1);
  near.usable = false;
  host.stonePiles.take(player, near.id);
  assert.equal(events.filter((e) => e[0] === 'stonePileThrow').length, 2);
  assert.throws(() => (pile.damage = 5000), RangeError);
  assert.throws(() => (pile.blastRadius = 11), RangeError);
  // Spawn, 2, 1, 0, -1, off; three corrections for the refused takes were sent too.
  assert.ok(ops('stonePileUpdate').length >= 5);
  assert.equal(api.StonePile.destroyAll(2), 1);
  assert.equal(events.filter((e) => e[0] === 'stonePileDestroy').length, 1);
  assert.equal(api.StonePile.all().length, 1);
});

test('a thrown stone hurts by distance, is checked against its pile, and counts once', async (t) => {
  const { api, host, report, events, listen } = await started(t);
  listen('stoneImpact');
  report(7);
  report(8, { position: { x: 10, y: 0, z: 0 } });
  report(9, { position: { x: 11, y: 0, z: 0 } });
  const health = [];
  for (const id of [7, 8, 9]) {
    host.players.get(id).addStat = (stat, delta) => health.push([id, stat, delta]);
  }
  const pile = api.StonePile.spawn({ x: 0, y: 0, z: 0 });
  pile.blastRadius = 2;
  const thrower = host.players.get(7);

  // No stone in flight: nothing happens.
  host.stonePiles.impact(thrower, pile.id, { x: 10, y: 0, z: 0 });
  assert.equal(events.length, 0);

  host.stonePiles.take(thrower, pile.id);
  // Too far from the pile, or naming another pile: ignored, the stone stays in flight.
  host.stonePiles.impact(thrower, pile.id, { x: 100, y: 0, z: 0 });
  host.stonePiles.impact(thrower, pile.id + 99, { x: 10, y: 0, z: 0 });
  assert.equal(events.length, 0);
  host.stonePiles.impact(thrower, pile.id, { x: 10, y: 0, z: 0 });
  assert.equal(events.length, 1);
  assert.equal(events[0][1], pile);
  assert.equal(events[0][2].x, 10);
  assert.equal(events[0][3], thrower);
  // 9 is one metre from the point of a two metre radius; 8 stands on it; 7 is too far.
  assert.deepEqual(health, [
    [8, 'health', -60],
    [9, 'health', -37.5],
  ]);
  host.stonePiles.impact(thrower, pile.id, { x: 10, y: 0, z: 0 });
  assert.equal(events.length, 1);

  // A pile destroyed while its stone flies: the event names none, the stone still lands.
  host.stonePiles.take(thrower, pile.id);
  pile.destroy();
  host.stonePiles.impact(thrower, pile.id, { x: 10, y: 0, z: 0 });
  assert.equal(events.length, 2);
  assert.equal(events[1][1], null);
});

test('stones hurt the NPCs around the point and cost nothing at zero damage', async (t) => {
  const { api, host, report } = await started(t);
  report(7);
  const hurt = [];
  host.npcs.npcs.set(99, {
    dimension: 0xffffffff,
    position: { x: 3, y: 0, z: 0 },
    damage: (amount, attacker) => hurt.push([amount, attacker.id]),
  });
  host.players.get(7).addStat = () => assert.fail('a zero-damage pile hurts nobody');
  const pile = api.StonePile.spawn({ x: 0, y: 0, z: 0 });
  const thrower = host.players.get(7);
  host.stonePiles.take(thrower, pile.id);
  host.stonePiles.impact(thrower, pile.id, { x: 3.5, y: 0, z: 0 });
  assert.deepEqual(hurt, [[60 * (1 - 0.75 * (0.5 / 1.5)), 7]]);
  pile.damage = 0;
  host.stonePiles.take(thrower, pile.id);
  host.stonePiles.impact(thrower, pile.id, { x: 3, y: 0, z: 0 });
  assert.equal(hurt.length, 1);
  host.npcs.npcs.delete(99);
});

test('damage falls from the full amount at the point to a quarter at the edge', () => {
  assert.equal(damageAt(60, 2, 0), 60);
  assert.equal(damageAt(60, 2, 2), 15);
  assert.equal(damageAt(60, 2, 2.1), 0);
  assert.equal(damageAt(60, 0, 0), 60);
});
