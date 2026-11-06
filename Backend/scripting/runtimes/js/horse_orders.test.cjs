'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');

// The orders of a horse a resource spawned: moveTo, patrol and hold travel to every
// client in the horse's intent field as "<sequence>|<kind>|<arguments>".

async function startedHost() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-horse-orders-'));
  const output = [];
  const host = new ResourceHost(root, {
    entityIds: { first: 500, count: 1025 },
    send: (message) => output.push(message),
    log: () => {},
  });
  await host.startAll();
  const sent = (op) => output.filter((message) => message.op === op);
  const intents = () => sent('horseIntent').map((message) => message.intent);
  return { host, Horse: host.api.Horse, sent, intents };
}

test('moveTo sends the order with its defaults and options', async () => {
  const { Horse, intents } = await startedHost();
  const horse = Horse.spawn({ x: 1, y: 2, z: 3 });
  assert.equal(horse.travelStatus, 'idle');
  assert.equal(horse.moveTo({ x: 10, y: 20, z: 30 }), true);
  assert.equal(horse.travelStatus, 'running');
  assert.equal(horse.moveTo({ x: 11.234, y: 21, z: 31 }, { speed: 'run', radius: 3 }), true);
  // A component left out keeps the horse's own.
  assert.equal(horse.moveTo({ x: 5 }), true);
  assert.deepEqual(intents(), [
    '1|moveTo|10|20|30|walk|1.5|server',
    '2|moveTo|11.23|21|31|run|3|server',
    '3|moveTo|5|2|3|walk|1.5|server',
  ]);
});

test('moveTo refuses what it cannot use', async () => {
  const { Horse, intents } = await startedHost();
  const horse = Horse.spawn({ x: 1, y: 2, z: 3 });
  assert.throws(() => horse.moveTo({ x: 1, y: 1, z: 1 }, { speed: 'sprint' }), RangeError);
  assert.throws(() => horse.moveTo({ x: 1, y: 1, z: 1 }, { radius: 0 }), RangeError);
  assert.throws(() => horse.moveTo({ x: 1, y: 1, z: 1 }, { radius: '2' }), TypeError);
  assert.throws(() => horse.moveTo({ x: 1, y: 1, z: 1 }, null), TypeError);
  assert.equal(horse.moveTo(null), false);
  assert.equal(horse.moveTo({ x: Number.NaN, y: 1, z: 1 }), false);
  assert.equal(horse.moveTo({ x: -5, y: 1, z: 1 }), false);
  assert.deepEqual(intents(), []);
  assert.equal(horse.travelStatus, 'idle');
  // A ridden horse takes no order.
  horse.mounted = true;
  assert.equal(horse.moveTo({ x: 1, y: 1, z: 1 }), false);
  horse.mounted = false;
  // A horse that is gone takes none either.
  horse.destroy();
  assert.equal(horse.moveTo({ x: 1, y: 1, z: 1 }), false);
  assert.equal(horse.patrol([{ x: 1, y: 1, z: 1 }]), false);
  assert.deepEqual(intents(), []);
});

test('patrol sends the route with its mode, speed and wait', async () => {
  const { Horse, intents } = await startedHost();
  const horse = Horse.spawn({ x: 1, y: 2, z: 3 });
  const points = [
    { x: 10, y: 20, z: 30 },
    { x: 11.5, y: 21, z: 31 },
  ];
  assert.equal(horse.patrol(points), true);
  assert.equal(horse.patrol(points, { speed: 'jog', loop: false, waitSeconds: 4.5 }), true);
  assert.equal(horse.patrol([{ x: 7 }], { loop: true }), true);
  assert.deepEqual(intents(), [
    '1|patrol|walk|loop|1.5|0|server|10,20,30;11.5,21,31',
    '2|patrol|jog|once|1.5|4.5|server|10,20,30;11.5,21,31',
    '3|patrol|walk|loop|1.5|0|server|7,2,3',
  ]);
  assert.equal(horse.travelStatus, 'running');
});

test('patrol refuses what it cannot use', async () => {
  const { Horse, intents } = await startedHost();
  const horse = Horse.spawn({ x: 1, y: 2, z: 3 });
  const point = { x: 1, y: 1, z: 1 };
  assert.throws(() => horse.patrol('route'), TypeError);
  assert.throws(() => horse.patrol([point], { speed: 'sprint' }), RangeError);
  assert.throws(() => horse.patrol([point], { loop: 'yes' }), TypeError);
  assert.throws(() => horse.patrol([point], { waitSeconds: 3601 }), RangeError);
  assert.throws(() => horse.patrol([point], 5), TypeError);
  assert.equal(horse.patrol([]), false);
  assert.equal(horse.patrol(Array(65).fill(point)), false);
  assert.equal(horse.patrol([point, null]), false);
  assert.equal(horse.patrol([point], { waitSeconds: 'soon' }), false);
  assert.deepEqual(intents(), []);
  assert.equal(horse.patrol(Array(64).fill(point)), true);
  assert.equal(intents().length, 1);
});

test('hold empties the intent and ends the order', async () => {
  const { Horse, intents, sent } = await startedHost();
  const horse = Horse.spawn({ x: 1, y: 2, z: 3 });
  horse.moveTo({ x: 10, y: 20, z: 30 });
  horse.hold();
  assert.equal(horse.travelStatus, 'idle');
  assert.deepEqual(intents(), ['1|moveTo|10|20|30|walk|1.5|server', '']);
  assert.equal(sent('horseIntent')[1].id, horse.id);
  // The sequence keeps rising across a hold.
  horse.moveTo({ x: 1, y: 1, z: 1 });
  assert.equal(intents().at(-1).startsWith('3|moveTo|'), true);
  horse.destroy();
  horse.hold();
  assert.equal(sent('horseIntent').length, 3);
});

test('the sequence belongs to each horse', async () => {
  const { Horse, intents } = await startedHost();
  const first = Horse.spawn({ x: 1, y: 2, z: 3 });
  const second = Horse.spawn({ x: 1, y: 2, z: 3 });
  first.moveTo({ x: 1, y: 1, z: 1 });
  first.moveTo({ x: 2, y: 1, z: 1 });
  second.moveTo({ x: 3, y: 1, z: 1 });
  assert.deepEqual(
    intents().map((intent) => intent.split('|')[0]),
    ['1', '2', '1'],
  );
});

test('a rider taking the saddle cancels a running order', async () => {
  const { host, Horse, intents } = await startedHost();
  const horse = Horse.spawn({ x: 1, y: 2, z: 3 });
  horse.moveTo({ x: 10, y: 20, z: 30 });
  host.updateHorse({
    id: horse.id,
    position: { x: 1, y: 2, z: 3 },
    rotation: { x: 0, y: 0, z: 0, w: 1 },
    dimension: 0xffffffff,
    riderId: 7,
  });
  assert.equal(horse.travelStatus, 'idle');
  assert.deepEqual(intents(), ['1|moveTo|10|20|30|walk|1.5|server', '']);
});

test('intentDone raises horseIntentDone once the order ended', async () => {
  const { host, Horse } = await startedHost();
  const horse = Horse.spawn({ x: 1, y: 2, z: 3 });
  const done = [];
  host.on('horseIntentDone', (who, status) => done.push([who, status, who.travelStatus]));
  horse.moveTo({ x: 5, y: 5, z: 5 });
  // A report of an earlier order, an unknown status and an unknown horse raise nothing.
  host.horseIntentDone({ id: horse.id, status: 'reached', sequence: 7 });
  host.horseIntentDone({ id: horse.id, status: 'arrived' });
  host.horseIntentDone({ id: horse.id + 1000, status: 'reached' });
  assert.deepEqual(done, []);
  assert.equal(horse.travelStatus, 'running');
  host.horseIntentDone({ id: horse.id, status: 'reached', sequence: 1 });
  assert.deepEqual(done, [[horse, 'reached', 'reached']]);
  horse.patrol([{ x: 6, y: 6, z: 6 }]);
  host.horseIntentDone({ id: horse.id, status: 'blocked' });
  host.horseIntentDone({ id: horse.id, status: 'failed', sequence: 2 });
  assert.deepEqual(
    done.map((entry) => entry[1]),
    ['reached', 'blocked', 'failed'],
  );
  // A hold makes a late report of the earlier order stale.
  horse.moveTo({ x: 6, y: 6, z: 6 });
  horse.hold();
  host.horseIntentDone({ id: horse.id, status: 'reached', sequence: 3 });
  assert.equal(done.length, 3);
  assert.equal(horse.travelStatus, 'idle');
});

// a horse follows a player, stopping short of them, at a pace by the distance.
test('follow walks the horse up to its target, until hold or the target leaves', async () => {
  const { host, Horse, intents } = await startedHost();
  const moveTarget = (x) =>
    host.updatePlayer({
      id: 7,
      ready: true,
      position: { x, y: 2, z: 3 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
    });
  moveTarget(31);
  const leader = host.players.get(7);
  const horse = Horse.spawn({ x: 1, y: 2, z: 3 });
  assert.equal(horse.follow(leader), true);
  assert.equal(horse.following, leader);
  // 30 m away: it runs to 3 m short of the leader.
  assert.deepEqual(intents(), ['1|moveTo|28|2|3|run|1|server']);
  // Nothing new while the leader stands still.
  host.horseOrders.stepAll();
  assert.equal(intents().length, 1);
  moveTarget(11);
  host.horseOrders.stepAll();
  assert.equal(intents().at(-1), '2|moveTo|8|2|3|jog|1|server');
  assert.throws(() => horse.follow(leader, { speed: 'gallop' }), RangeError);
  assert.equal(horse.follow(horse), false);
  horse.hold();
  assert.equal(horse.following, null);
  assert.equal(horse.follow(leader, { radius: 5, speed: 'walk' }), true);
  assert.equal(intents().at(-1), '4|moveTo|6|2|3|walk|1|server');
  host.removePlayer?.(7);
});

// A horse led behind a walking player keeps walking: it aims where the player will be a second
// on, is looked at four times a second, and jogs after a player moving fast.
test('follow aims ahead of a moving target and paces by its speed', async (t) => {
  const { host, Horse, intents } = await startedHost();
  let now = 1000000;
  t.mock.method(Date, 'now', () => now);
  const moveTarget = (x) =>
    host.updatePlayer({
      id: 7,
      ready: true,
      position: { x, y: 2, z: 3 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
    });
  moveTarget(6);
  const leader = host.players.get(7);
  const horse = Horse.spawn({ x: 1, y: 2, z: 3 });
  assert.equal(horse.follow(leader), true);
  assert.deepEqual(intents(), ['1|moveTo|3|2|3|walk|1|server']);
  // Walking 2 m/s for a quarter second: aimed a second ahead, 3 m short of 6.5 + 2.
  now += 250;
  moveTarget(6.5);
  host.horseOrders.stepAll();
  assert.equal(intents().at(-1), '2|moveTo|5.5|2|3|walk|1|server');
  // Running 5 m/s: a 5 m lead, and the horse runs, though the distance alone says jog.
  now += 250;
  moveTarget(7.75);
  host.horseOrders.stepAll();
  assert.equal(intents().at(-1), '3|moveTo|9.75|2|3|run|1|server');
  horse.hold();
});

// follow with a rope names its leader to every client until it ends.
test('follow with rope sends the leader, and the rope goes with the follow', async () => {
  const { host, Horse, sent } = await startedHost();
  host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 20, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const leader = host.players.get(7);
  const horse = Horse.spawn({ x: 1, y: 2, z: 3 });
  assert.equal(horse.follow(leader, { rope: true }), true);
  assert.deepEqual(
    sent('horseLead').map((m) => m.leader),
    [7],
  );
  assert.throws(() => horse.follow(leader, { rope: 'yes' }), TypeError);
  horse.hold();
  assert.deepEqual(
    sent('horseLead').map((m) => m.leader),
    [7, 0],
  );
  // Without a rope nothing is sent.
  horse.follow(leader);
  horse.hold();
  assert.equal(sent('horseLead').length, 2);
});

// a horse's dirt, which every client puts on its coat and tack.
test('horse dirt, soil and clean send the dirt every client shows', async () => {
  const { Horse, sent } = await startedHost();
  const horse = Horse.spawn({ x: 1, y: 2, z: 3 });
  assert.equal(horse.dirt, null);
  assert.equal(horse.soil(), 0.25);
  assert.equal(horse.soil(0.9), 1);
  horse.clean();
  assert.equal(horse.dirt, 0);
  horse.dirt = null;
  assert.deepEqual(
    sent('horseDirt').map((m) => m.dirt),
    [0.25, 1, 0, -1],
  );
  assert.throws(() => horse.soil(2), RangeError);
  assert.throws(() => {
    horse.dirt = 1.5;
  }, RangeError);
});

// a horse slower (or faster) than the game's speeds allow, in every gait.
test('horse speedScale sends the share of its pace every client applies', async () => {
  const { Horse, sent } = await startedHost();
  const horse = Horse.spawn({ x: 1, y: 2, z: 3 });
  assert.equal(horse.speedScale, null);
  horse.speedScale = 0.4;
  assert.equal(horse.speedScale, 0.4);
  horse.speedScale = null;
  assert.deepEqual(
    sent('horseSpeedScale').map((m) => m.scale),
    [0.4, -1],
  );
  for (const wrong of [0, 0.05, 2.5, '1', Number.NaN]) {
    assert.throws(() => {
      horse.speedScale = wrong;
    }, RangeError);
  }
});
