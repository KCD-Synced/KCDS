'use strict';

// Horse passengers: a player behind a horse's rider (horses/passengers.cjs).

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');
const {
  afterClip,
  beforeClip,
  passengerSeat,
  MOUNT,
  DISMOUNT,
  BEHIND_RIDER,
  ABOVE_RIDER,
} = require('./horses/passengers.cjs');

const HORSE_AT = { x: 10, y: 20, z: 3 };
const ANY_WORLD = 0xffffffff;

async function setup(t) {
  const f = serverFixture(t);
  await f.host.startAll();
  const api = f.host.makeAPI();
  // The rider sits 1.5 m above the horse's root; the other player stands beside it.
  const rider = f.report(7, { position: { x: 10, y: 19.95, z: 4.5 } });
  const other = f.report(8, { position: { x: 8.8, y: 20, z: 3 } });
  const horse = api.Horse.spawn(HORSE_AT);
  const log = [];
  for (const name of ['horsePassengerBoarded', 'horsePassengerLeft']) {
    api.Events.on(name, (h, player) => log.push([name, h.id, player.id]));
  }
  const ride = (riderId) =>
    f.host.updateHorse({
      id: horse.id,
      position: HORSE_AT,
      rotation: { w: 1, x: 0, y: 0, z: 0 },
      dimension: ANY_WORLD,
      riderId,
    });
  const ops = (op) => f.sent.filter((m) => m.op === op);
  const lastAttach = () => ops('playerAttach').at(-1);
  const lastAnimation = (player) =>
    ops('animation')
      .filter((m) => m.id === player.id && m.kind === 'play')
      .at(-1);
  // The player's game reports that their current animation (a climb) ended.
  const finish = (player) => {
    const request = f.host.playerAnimations.stateOf(player).current;
    f.host.playerAnimations.report(player, { request: request.id, reason: 'finished' });
  };
  const passengers = f.host.horsePassengers;
  const board = () => passengers.boarding.get(horse);
  return {
    f,
    api,
    rider,
    other,
    horse,
    log,
    ride,
    lastAttach,
    lastAnimation,
    finish,
    passengers,
    board,
  };
}

test('the climbs end where the seat is and start beside the horse', () => {
  const seat = { right: 0, forward: -0.5, up: 1.47, yaw: 0 };
  const start = beforeClip(seat, MOUNT);
  const end = afterClip(start, MOUNT);
  for (const axis of ['right', 'forward', 'up', 'yaw']) {
    assert.ok(Math.abs(end[axis] - seat[axis]) < 1e-9, axis);
  }
  // The mount starts on the ground on the horse's left, facing its side.
  assert.ok(start.right < -0.8);
  assert.ok(Math.abs(start.up) < 0.05);
  assert.ok(start.yaw < -60 && start.yaw > -80);
  // The dismount ends on the ground on the left, a little behind the seat.
  const down = afterClip(seat, DISMOUNT);
  assert.ok(down.right < -0.7);
  assert.ok(down.forward < seat.forward);
  assert.ok(Math.abs(down.up) < 0.1);
});

test('the seat is behind where the rider sits', () => {
  const horse = { position: HORSE_AT, rotation: { w: 1, x: 0, y: 0, z: 0 } };
  const seat = passengerSeat(horse, { position: { x: 10, y: 19.95, z: 4.5 } });
  assert.ok(Math.abs(seat.forward - (-0.05 - BEHIND_RIDER)) < 1e-6);
  // On the horse's back behind the saddle, higher than the rider's seat in it.
  assert.ok(Math.abs(seat.up - (1.5 + ABOVE_RIDER)) < 1e-6);
  // A rider's place that is no seat (still on the ground) leaves the seat where a rider sits.
  const fallback = passengerSeat(horse, { position: HORSE_AT });
  assert.ok(fallback.up > 1.5);
});

test('Ride behind shows while someone rides, and takes a player on with the mount clip', async (t) => {
  const { horse, rider, other, log, ride, lastAttach, lastAnimation, finish, board, f } =
    await setup(t);
  assert.equal(board(), undefined);
  ride(7);
  const hint = board();
  assert.ok(hint);
  assert.equal(hint.enabled, true);
  // Its rider does not see it.
  assert.equal(f.host.worldInteractions.visibleTo(hint, rider), false);
  assert.equal(f.host.worldInteractions.visibleTo(hint, other), true);
  f.host.worldInteractions.fired(other, hint.id);
  assert.equal(horse.passenger, other);
  const order = lastAttach();
  assert.equal(order.id, 8);
  assert.equal(order.target, horse.id);
  assert.equal(order.body, true);
  assert.equal(order.from.ms, MOUNT.ms);
  assert.ok(order.from.right < -0.8);
  // The climb starts on the ground the horse stands on.
  assert.equal(order.from.up, 0);
  assert.ok(Math.abs(order.forward - (-0.05 - BEHIND_RIDER)) < 1e-6);
  assert.equal(lastAnimation(other).fragment, 'Mount');
  assert.equal(lastAnimation(other).tags, 'player+sideLeft');
  // The hint goes while the seat is taken.
  assert.equal(hint.enabled, false);
  finish(other);
  assert.equal(lastAnimation(other).fragment, 'MotionIdle');
  assert.equal(lastAnimation(other).tags, 'horse+player');
  assert.deepEqual(log, [['horsePassengerBoarded', horse.id, 8]]);
});

test('Get off on the rider lets the passenger down with the dismount clip', async (t) => {
  const {
    horse,
    other,
    rider,
    log,
    ride,
    lastAttach,
    lastAnimation,
    finish,
    board,
    f,
    passengers,
  } = await setup(t);
  ride(7);
  f.host.worldInteractions.fired(other, board().id);
  finish(other);
  const entry = passengers.seats.get(horse);
  const getOff = entry.getOff;
  assert.ok(getOff);
  // Only the passenger sees it.
  assert.equal(f.host.worldInteractions.visibleTo(getOff, other), true);
  f.host.worldInteractions.fired(rider, getOff.id);
  assert.equal(horse.passenger, other);
  f.host.worldInteractions.fired(other, getOff.id);
  assert.equal(lastAnimation(other).fragment, 'Dismount');
  const down = lastAttach();
  assert.equal(down.from.ms, DISMOUNT.ms);
  assert.ok(down.right < -0.7);
  finish(other);
  assert.equal(lastAttach().target, 0);
  assert.equal(horse.passenger, null);
  assert.equal(board().enabled, true);
  assert.deepEqual(log.at(-1), ['horsePassengerLeft', horse.id, 8]);
});

test('the passenger gets off when the rider does, and at once when the horse goes', async (t) => {
  const { api, horse, other, ride, lastAnimation, finish, board, f } = await setup(t);
  ride(7);
  f.host.worldInteractions.fired(other, board().id);
  finish(other);
  ride(0);
  assert.equal(lastAnimation(other).fragment, 'Dismount');
  finish(other);
  assert.equal(horse.passenger, null);
  // Seated by a script, then the horse is removed: off at once beside it.
  ride(7);
  assert.equal(horse.seatPassenger(other), true);
  horse.destroy();
  assert.equal(api.Horse.getById(horse.id), null);
  assert.equal(horse.passenger, null);
  // On the ground on the horse's left, where the dismount clip ends.
  const placed = f.sent.filter((m) => m.op === 'entityPosition' && m.id === 8).at(-1);
  assert.ok(placed);
  assert.ok(placed.position.x < HORSE_AT.x - 0.7);
  assert.ok(Math.abs(placed.position.z - HORSE_AT.z) < 0.2);
});

test('seatPassenger puts a player on at once; removePassenger lets them off', async (t) => {
  const { horse, other, rider, log, ride, lastAttach, lastAnimation } = await setup(t);
  ride(7);
  assert.equal(horse.seatPassenger(rider), false);
  assert.equal(horse.seatPassenger(other), true);
  assert.equal(horse.seatPassenger(other), false);
  assert.equal(lastAttach().from, undefined);
  assert.equal(lastAttach().body, true);
  assert.equal(lastAnimation(other).fragment, 'MotionIdle');
  assert.deepEqual(log, [['horsePassengerBoarded', horse.id, 8]]);
  assert.equal(horse.removePassenger(true), other);
  assert.equal(lastAttach().target, 0);
  assert.equal(horse.passenger, null);
  assert.equal(horse.removePassenger(), null);
  assert.throws(() => horse.seatPassenger({ id: 8 }), TypeError);
  assert.throws(() => horse.removePassenger('yes'), TypeError);
});

test('horsePassengerBoarding refuses, passengerAllowed hides the hint, a far player is not taken', async (t) => {
  const { api, horse, other, ride, board, f } = await setup(t);
  ride(7);
  let allow = false;
  api.Events.on('horsePassengerBoarding', () => allow);
  f.host.worldInteractions.fired(other, board().id);
  assert.equal(horse.passenger, null);
  allow = true;
  horse.passengerAllowed = false;
  assert.equal(horse.passengerAllowed, false);
  assert.equal(board().enabled, false);
  assert.equal(f.host.horsePassengers.boardRequested(horse, other), false);
  horse.passengerAllowed = true;
  assert.equal(board().enabled, true);
  f.report(8, { position: { x: 30, y: 20, z: 3 } });
  f.host.worldInteractions.fired(other, board().id);
  assert.equal(horse.passenger, null);
  assert.throws(() => (horse.passengerAllowed = 'no'), TypeError);
});

test('a passenger who gets a new look sits again on the new body', async (t) => {
  const { horse, other, ride, f, lastAnimation } = await setup(t);
  ride(7);
  horse.seatPassenger(other);
  const plays = () => f.sent.filter((m) => m.op === 'animation' && m.id === 8 && m.kind === 'play');
  const before = plays().length;
  t.mock.timers.enable({ apis: ['setTimeout'] });
  f.report(8, { position: { x: 8.8, y: 20, z: 3 }, appearance: { gender: 'female' } });
  assert.equal(plays().length, before);
  t.mock.timers.tick(2000);
  assert.equal(plays().length, before + 1);
  assert.equal(lastAnimation(other).fragment, 'MotionIdle');
  // The same look again changes nothing.
  f.report(8, { position: { x: 8.8, y: 20, z: 3 }, appearance: { gender: 'female' } });
  t.mock.timers.tick(2000);
  assert.equal(plays().length, before + 1);
});

test('a passenger a script attaches elsewhere is no passenger any more', async (t) => {
  const { horse, other, rider, ride, log } = await setup(t);
  ride(7);
  horse.seatPassenger(other);
  assert.equal(other.attachTo(rider, { offset: { y: -1 } }), true);
  assert.equal(horse.passenger, null);
  assert.deepEqual(log.at(-1), ['horsePassengerLeft', horse.id, 8]);
});
