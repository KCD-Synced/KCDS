'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');

// PatrolRoute and the NPC patrol: the routes a resource registers, the legs the server
// orders one after another, and the events that tell how a patrol went.

const A = { x: 1, y: 0, z: 0 };
const B = { x: 2, y: 0, z: 0 };
const C = { x: 3, y: 0, z: 0 };

async function startedHost() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-patrol-'));
  const output = [];
  const host = new ResourceHost(root, {
    entityIds: { first: 4097, count: 2048 },
    send: (message) => output.push(message),
    log: () => {},
  });
  await host.startAll();
  const intents = () => output.filter((m) => m.op === 'npcIntent').map((m) => m.intent);
  const npc = host.npcs.Npc.create({ position: { x: 0, y: 0, z: 0 } });
  const PatrolRoute = host.npcs.patrols.routes.define();
  // The simulator reporting that the leg the NPC is on ended.
  const report = (status = 'reached') => host.npcs.intentDone({ id: npc.id, status });
  // What the events said, as [waypointIndex, lap, status] and [status, waypointIndex].
  const waypoints = [];
  const finished = [];
  const announced = [];
  host.on('patrolWaypoint', (who, info) => {
    if (who === npc) {
      waypoints.push([info.waypointIndex, info.lap, info.status]);
    }
  });
  host.on('patrolFinished', (who, info) => {
    if (who === npc) {
      finished.push([info.status, info.waypointIndex]);
    }
  });
  host.on('npcIntentDone', (who, status) => announced.push(status));
  return { host, npc, PatrolRoute, intents, report, waypoints, finished, announced };
}

test('a route is made from points or a definition and found by id or name', async () => {
  const { PatrolRoute } = await startedHost();
  const guard = PatrolRoute.create({
    id: 'town.guard-loop',
    name: 'Guard loop',
    points: [{ position: A, speed: 'run', waitSeconds: 4 }, B],
    mode: 'pingPong',
    radius: 3,
  });
  assert.equal(guard.exists, true);
  assert.equal(guard.id, 'town.guard-loop');
  assert.equal(guard.name, 'Guard loop');
  assert.equal(PatrolRoute.getById('town.guard-loop'), guard);
  assert.equal(PatrolRoute.getByName('Guard loop'), guard);
  assert.equal(PatrolRoute.getById('Guard loop'), null);
  const json = guard.toJSON();
  assert.equal(json.mode, 'pingPong');
  assert.equal(json.radius, 3);
  assert.equal(json.speed, undefined);
  assert.equal(json.points[0].speed, 'run');
  assert.equal(json.points[0].waitSeconds, 4);
  assert.equal(json.points[1].position.x, 2);
  // The copy is independent.
  json.points.length = 0;
  assert.equal(guard.toJSON().points.length, 2);

  // Points alone: the name stands in for the id, and no name gets a made-up id.
  const sentry = PatrolRoute.create([A, B, C], 'sentry');
  assert.equal(sentry.id, 'sentry');
  assert.equal(sentry.toJSON().points.length, 3);
  const nameless = PatrolRoute.create([A]);
  assert.match(nameless.id, /^route-\d+$/);
  assert.deepEqual(PatrolRoute.all(), [guard, sentry, nameless]);
});

test('a route refuses what it cannot use', async () => {
  const { PatrolRoute } = await startedHost();
  const point = { x: 1, y: 1, z: 1 };
  assert.throws(() => new PatrolRoute(), TypeError);
  assert.throws(() => PatrolRoute.create(null), TypeError);
  assert.throws(() => PatrolRoute.create({ id: 'a' }), TypeError);
  assert.throws(() => PatrolRoute.create({ id: 'a', points: [] }), RangeError);
  assert.throws(() => PatrolRoute.create(new Array(257).fill(point)), RangeError);
  assert.throws(() => PatrolRoute.create([{ x: 1 }]), TypeError);
  assert.throws(() => PatrolRoute.create([null]), TypeError);
  assert.throws(() => PatrolRoute.create([{ x: -5, y: 1, z: 1 }]), RangeError);
  assert.throws(() => PatrolRoute.create({ id: 5, points: [point] }), TypeError);
  assert.throws(() => PatrolRoute.create({ id: '', points: [point] }), RangeError);
  assert.throws(() => PatrolRoute.create({ id: 'a'.repeat(129), points: [point] }), RangeError);
  assert.throws(() => PatrolRoute.create({ points: [point], mode: 'twice' }), RangeError);
  assert.throws(() => PatrolRoute.create({ points: [point], speed: 'sprint' }), RangeError);
  assert.throws(() => PatrolRoute.create({ points: [point], radius: 0 }), RangeError);
  assert.throws(() => PatrolRoute.create({ points: [point], waitSeconds: 3601 }), RangeError);
  assert.throws(() => PatrolRoute.create({ points: [point], waitSeconds: 'soon' }), TypeError);
  assert.throws(
    () => PatrolRoute.create({ points: [{ position: point, radius: -1 }] }),
    RangeError,
  );
  assert.deepEqual(PatrolRoute.all(), [], 'a refused route is not registered');
  PatrolRoute.create({ id: 'taken', points: [point] });
  assert.throws(() => PatrolRoute.create({ id: 'taken', points: [point] }), RangeError);
});

test('update replaces fields whole or not at all; destroy removes the route', async () => {
  const { npc, PatrolRoute } = await startedHost();
  const route = PatrolRoute.create({ id: 'r', points: [A, B], radius: 2 });
  assert.equal(route.update({ radius: 5, points: [C] }), true);
  assert.equal(route.toJSON().radius, 5);
  assert.equal(route.toJSON().points.length, 1);
  assert.equal(route.update({ id: 'r', mode: 'once' }), true);
  assert.equal(route.toJSON().mode, 'once');
  assert.throws(() => route.update({ id: 'other' }), RangeError);
  assert.throws(() => route.update({ mode: 'twice', radius: 9 }), RangeError);
  assert.equal(route.toJSON().radius, 5, 'a refused update changes nothing');
  assert.throws(() => route.update(null), TypeError);

  assert.equal(route.destroy(), true);
  assert.equal(route.exists, false);
  assert.equal(route.id, '');
  assert.equal(route.name, '');
  assert.equal(route.toJSON(), null);
  assert.equal(route.update({ radius: 1 }), false);
  assert.equal(route.destroy(), false);
  assert.equal(PatrolRoute.getById('r'), null);
  assert.equal(npc.patrol(route), false);
  assert.equal(npc.patrol('r'), false);
  PatrolRoute.create({ id: 'r', points: [A] });
});

test('a route is started by handle, id or name, and the call overrides its defaults', async () => {
  const { npc, PatrolRoute, intents, report } = await startedHost();
  const route = PatrolRoute.create({
    id: 'town.loop',
    name: 'Town loop',
    points: [{ position: A, speed: 'run' }, B, { position: C, radius: 0.5, waitSeconds: 0 }],
    mode: 'once',
    speed: 'jog',
    radius: 3,
  });
  assert.equal(npc.patrol(route), true);
  assert.deepEqual(npc.routeState, {
    routeId: 'town.loop',
    waypointIndex: 0,
    lap: 0,
    direction: 1,
    phase: 'moving',
  });
  report();
  report();
  assert.deepEqual(intents(), [
    '1|moveTo|1|0|0|run|3|auto',
    '2|moveTo|2|0|0|jog|3|auto',
    '3|moveTo|3|0|0|jog|0.5|auto',
  ]);
  // The id and the name name the same route; the call's options win over the route's.
  assert.equal(npc.patrol('town.loop', { speed: 'walk', radius: 2, pathfinding: 'game' }), true);
  report();
  assert.equal(npc.patrol('Town loop'), true);
  assert.deepEqual(intents().slice(3), [
    '4|moveTo|1|0|0|run|2|game',
    '5|moveTo|2|0|0|walk|2|game',
    '6|moveTo|1|0|0|run|3|auto',
  ]);
});

test('a running patrol keeps the route it started from', async () => {
  const { npc, PatrolRoute, intents, report, finished } = await startedHost();
  const route = PatrolRoute.create({ id: 'r', points: [A, B], mode: 'once' });
  npc.patrol(route);
  route.update({ points: [C] });
  report();
  assert.equal(intents().at(-1), '2|moveTo|2|0|0|walk|1.5|auto');
  route.destroy();
  report();
  assert.deepEqual(finished, [['completed', 1]]);
});

test('a loop steps leg after leg and counts its laps', async () => {
  const { npc, intents, report, waypoints, finished, announced } = await startedHost();
  assert.equal(npc.patrol([A, B, C]), true);
  report();
  report();
  report();
  assert.deepEqual(waypoints, [
    [0, 0, 'reached'],
    [1, 0, 'reached'],
    [2, 0, 'reached'],
  ]);
  assert.deepEqual(announced, ['reached', 'reached', 'reached']);
  assert.deepEqual(intents(), [
    '1|moveTo|1|0|0|walk|1.5|auto',
    '2|moveTo|2|0|0|walk|1.5|auto',
    '3|moveTo|3|0|0|walk|1.5|auto',
    '4|moveTo|1|0|0|walk|1.5|auto',
  ]);
  assert.equal(npc.routeState.lap, 1);
  assert.equal(npc.routeState.waypointIndex, 0);
  assert.equal(npc.intent, 'moveTo');
  assert.equal(npc.status, 'running');
  assert.deepEqual(finished, []);
});

test('a patrol that runs once completes after its last leg', async () => {
  const { host, npc, report, waypoints, finished } = await startedHost();
  const infos = [];
  host.on('patrolFinished', (who, info) => infos.push(info));
  npc.patrol([A, B], { loop: false });
  report();
  assert.equal(npc.routeState.waypointIndex, 1);
  report();
  assert.equal(waypoints.length, 2);
  assert.deepEqual(finished, [['completed', 1]]);
  assert.deepEqual(infos, [
    { routeId: null, waypointIndex: 1, lap: 0, direction: 1, phase: 'moving', status: 'completed' },
  ]);
  assert.equal(npc.routeState, null);
  assert.equal(npc.status, 'reached');
});

test('a ping-pong patrol turns at both ends', async () => {
  const { npc, report } = await startedHost();
  npc.patrol([A, B, C], { mode: 'pingPong' });
  const seen = [];
  for (let leg = 0; leg < 6; leg += 1) {
    report();
    const { waypointIndex, direction, lap } = npc.routeState;
    seen.push([waypointIndex, direction, lap]);
  }
  assert.deepEqual(seen, [
    [1, 1, 0],
    [2, 1, 0],
    [1, -1, 0],
    [0, -1, 0],
    [1, 1, 1],
    [2, 1, 1],
  ]);
});

test('a patrol waits at a waypoint before the next leg', async (t) => {
  const { npc, intents, report, finished } = await startedHost();
  t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 1000 });
  npc.patrol([A, B], { mode: 'once', waitSeconds: 3 });
  report();
  assert.equal(npc.routeState.phase, 'waiting');
  assert.equal(npc.routeState.waypointIndex, 0);
  report();
  assert.equal(intents().length, 1, 'a report during the wait is dropped');
  t.mock.timers.tick(2999);
  assert.equal(intents().length, 1);
  t.mock.timers.tick(1);
  assert.equal(intents().length, 2);
  assert.deepEqual(npc.routeState, {
    routeId: null,
    waypointIndex: 1,
    lap: 0,
    direction: 1,
    phase: 'moving',
  });
  // The last waypoint is waited at too, and then the patrol is complete.
  report();
  assert.deepEqual(finished, []);
  t.mock.timers.tick(3000);
  assert.deepEqual(finished, [['completed', 1]]);
  assert.equal(npc.routeState, null);
});

test('a blocked or failed leg is skipped no sooner than two seconds after its order', async (t) => {
  const { npc, intents, report, waypoints, announced } = await startedHost();
  t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 1000 });
  npc.patrol([A, B, C]);
  t.mock.timers.tick(500);
  report('blocked');
  assert.deepEqual(waypoints, [[0, 0, 'blocked']]);
  assert.deepEqual(announced, ['blocked']);
  assert.equal(npc.status, 'blocked');
  t.mock.timers.tick(1499);
  assert.equal(intents().length, 1);
  t.mock.timers.tick(1);
  assert.equal(intents().length, 2);
  assert.equal(npc.routeState.waypointIndex, 1);
  assert.equal(npc.status, 'running');
  // A leg that ended after the two seconds is skipped at once.
  t.mock.timers.tick(2500);
  report('failed');
  assert.equal(intents().length, 3);
  assert.equal(npc.routeState.waypointIndex, 2);
});

test('a lap of failed legs in a row abandons the patrol with failed', async (t) => {
  const { npc, intents, report, waypoints, finished, announced } = await startedHost();
  t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 0 });
  npc.patrol([A, B, C]);
  report('blocked');
  t.mock.timers.tick(2000);
  report('failed');
  t.mock.timers.tick(2000);
  report('blocked');
  assert.deepEqual(waypoints, [
    [0, 0, 'blocked'],
    [1, 0, 'failed'],
    [2, 0, 'failed'],
  ]);
  assert.deepEqual(announced, ['blocked', 'failed', 'failed']);
  assert.deepEqual(finished, [['failed', 2]]);
  assert.equal(npc.routeState, null);
  assert.equal(npc.status, 'failed');
  assert.equal(intents().length, 3);
  t.mock.timers.tick(10000);
  assert.equal(intents().length, 3, 'nothing is ordered after the patrol gave up');
});

test('a leg that is reached starts the count of failed legs afresh', async (t) => {
  const { npc, report, finished } = await startedHost();
  t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 0 });
  npc.patrol([A, B, C]);
  report('blocked');
  t.mock.timers.tick(2000);
  report('reached');
  report('blocked');
  t.mock.timers.tick(2000);
  report('blocked');
  assert.deepEqual(finished, []);
  assert.notEqual(npc.routeState, null);
});

test('any other order cancels the patrol after the new order stands', async () => {
  const { host, npc, finished } = await startedHost();
  const seen = [];
  host.on('patrolFinished', (who, info) => {
    seen.push([who.routeState, who.intent, info.status]);
  });
  npc.patrol([A, B]);
  assert.equal(npc.moveTo(C), true);
  npc.patrol([A, B]);
  assert.equal(npc.patrol([B, C]), true);
  npc.patrol([A, B]);
  npc.follow(host.npcs.Npc.create({ position: { x: 5, y: 5, z: 5 } }));
  npc.patrol([A, B]);
  npc.hold();
  assert.deepEqual(finished, [
    ['cancelled', 0],
    ['cancelled', 0],
    ['cancelled', 0],
    ['cancelled', 0],
    ['cancelled', 0],
  ]);
  assert.equal(seen[0][0], null);
  assert.equal(seen[0][1], 'moveTo');
  assert.notEqual(seen[1][0], null, 'the patrol that replaced it is running');
  assert.equal(seen[4][1], 'hold');
});

test('a patrol that is not ordered anew is cancelled by death and removal', async () => {
  const { host, npc, finished, report, intents } = await startedHost();
  npc.patrol([A, B]);
  npc.kill();
  assert.deepEqual(finished, [['cancelled', 0]]);
  assert.equal(npc.routeState, null);
  report();
  assert.equal(intents().length, 1, 'a dead NPC is not ordered on');
  const other = host.npcs.Npc.create({ position: { x: 9, y: 9, z: 9 } });
  other.patrol([A]);
  // The handles still resolve while it is announced, and the patrol is over by then.
  const seen = [];
  host.on('patrolFinished', (who, info) => seen.push([who === other, info.status, who.routeState]));
  host.on('npcDestroy', (who) => seen.push([who === other, who.routeState]));
  other.remove();
  assert.deepEqual(seen, [
    [true, 'cancelled', null],
    [true, null],
  ]);
});

test('a handler that re-orders the NPC on a finished leg replaces the route', async () => {
  const { host, npc, intents, report, finished } = await startedHost();
  host.on('npcIntentDone', () => npc.moveTo(C));
  npc.patrol([A, B]);
  report();
  assert.deepEqual(intents(), ['1|moveTo|1|0|0|walk|1.5|auto', '2|moveTo|3|0|0|walk|1.5|auto']);
  assert.deepEqual(finished, [['cancelled', 0]]);
  assert.equal(npc.routeState, null);
});

test('a patrol is refused for what it cannot use', async () => {
  const { npc, PatrolRoute, intents } = await startedHost();
  assert.equal(npc.patrol([]), false);
  assert.equal(npc.patrol([A, null]), false);
  assert.equal(npc.patrol([A], { waitSeconds: 'soon' }), false);
  assert.equal(npc.patrol('nobody.knows'), false);
  assert.throws(() => npc.patrol(7), TypeError);
  assert.throws(() => npc.patrol({}), TypeError);
  assert.throws(() => npc.patrol([A], { mode: 'once', loop: true }), TypeError);
  assert.throws(() => npc.patrol([A], { waitSeconds: 3601 }), RangeError);
  const route = PatrolRoute.create({ id: 'r', points: [A] });
  assert.throws(() => npc.patrol(route, { speed: 'sprint' }), RangeError);
  assert.deepEqual(intents(), []);
  npc.kill();
  assert.equal(npc.patrol(route), false);
});
