'use strict';

// The client's Follow global: targets in range, the game's answer, the events.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceFollow } = require('../../../../Client/resources/follow.cjs');

class Vector3 {
  constructor(x, y, z) {
    Object.assign(this, { x, y, z });
  }
}

function fixture(answer = true) {
  const asked = [];
  const events = [];
  const entities = {
    localId: 1,
    records: new Map([
      [1, { kind: 'player', nickname: 'Me', position: { x: 0, y: 0, z: 0 } }],
      [2, { kind: 'player', nickname: 'Hans', position: { x: 3, y: 4, z: 0 } }],
      [3, { kind: 'player', nickname: 'Far', position: { x: 50, y: 0, z: 0 } }],
      [4, { kind: 'npc', position: { x: 1, y: 0, z: 0 } }],
    ]),
    engineIds: new Map([
      [2, 30583],
      [3, 30584],
    ]),
  };
  const sync = {
    call(request) {
      asked.push(request);
      return answer;
    },
  };
  const follow = new ResourceFollow({
    entities,
    sync,
    Vector3,
    emit: (name, args) => events.push([name, ...args.map((value) => value?.id ?? value)]),
  });
  return { follow, asked, events, api: follow.facade('mode') };
}

test('client Follow: targets in range, a start the game takes, its events', () => {
  const { follow, asked, events, api } = fixture();
  assert.deepEqual(
    api.targets().map((target) => [target.id, target.name]),
    [[2, 'Hans']],
  );
  assert.equal(api.canStart({ id: 2 }), true);
  assert.equal(api.start({ id: 2 }), true);
  assert.equal(api.isPending(), true);
  assert.equal(api.getTarget(), null);
  follow.receiveEvent({ op: 'followEvent', event: 'started', reason: '' });
  assert.equal(api.getTarget().id, 2);
  api.stop();
  follow.receiveEvent({ op: 'followEvent', event: 'stopped', reason: 'script' });
  assert.equal(api.getTarget(), null);
  assert.deepEqual(asked, [
    { op: 'followCheck', entity: 30583 },
    {
      op: 'followStart',
      entity: 30583,
      hint: true,
      playerCanStop: true,
      run: false,
      distance: 0,
    },
    { op: 'followStop' },
  ]);
  assert.deepEqual(events, [
    ['followStarted', 2],
    ['followStopped', 2, 'script'],
  ]);
});

// a follow without the game's prompt that the player cannot end.
test('client Follow: hint and playerCanStop go to the game client', () => {
  const { asked, api } = fixture();
  assert.equal(api.start({ id: 2 }, { hint: false, playerCanStop: false }), true);
  assert.deepEqual(asked.at(-1), {
    op: 'followStart',
    entity: 30583,
    hint: false,
    playerCanStop: false,
    run: false,
    distance: 0,
  });
  assert.throws(() => api.start({ id: 2 }, { hint: 'no' }), TypeError);
  assert.throws(() => api.start({ id: 2 }, null), TypeError);
});

// pace 'run' lets a led prisoner keep up with a guard who runs.
test("client Follow: pace 'run' goes to the game client", () => {
  const { asked, api } = fixture();
  assert.equal(api.start({ id: 2 }, { pace: 'run' }), true);
  assert.equal(asked.at(-1).run, true);
  assert.equal(api.start({ id: 2 }, { pace: 'walk' }), true);
  assert.equal(asked.at(-1).run, false);
  assert.throws(() => api.start({ id: 2 }, { pace: 'sprint' }), TypeError);
});

// how close the follower keeps, 0.5 to 10 m; 0 is the game's own.
test('client Follow: distance goes to the game client', () => {
  const { asked, api } = fixture();
  assert.equal(api.start({ id: 2 }, { distance: 1.5 }), true);
  assert.equal(asked.at(-1).distance, 1.5);
  assert.equal(api.start({ id: 2 }), true);
  assert.equal(asked.at(-1).distance, 0);
  assert.throws(() => api.start({ id: 2 }, { distance: 0.2 }), RangeError);
  assert.throws(() => api.start({ id: 2 }, { distance: 11 }), RangeError);
  assert.throws(() => api.start({ id: 2 }, { distance: '2' }), RangeError);
});

test('client Follow: refusals, another resource, and a stopped resource', () => {
  const refused = fixture(false);
  assert.equal(refused.api.start({ id: 2 }), false);
  assert.equal(refused.api.isPending(), false);
  const { follow, events, api } = fixture();
  assert.equal(api.start({ id: 4 }), false);
  assert.equal(api.start({ id: 1 }), false);
  assert.throws(() => api.start(null), /Player or a target/);
  api.start({ id: 2 });
  follow.facade('other').stop();
  assert.equal(api.isPending(), true);
  follow.removeOwner('mode');
  assert.deepEqual(events.at(-1), ['followStopped', 2, 'resourceStopped']);
});

test('client Follow: a server NPC with a body here is a target by its engine id', () => {
  const { follow, asked, api } = fixture();
  follow.entities.records.set(7, { kind: 'entity', position: { x: 0, y: 6, z: 0 } });
  follow.entities.npcBodies = new Map([
    [7, { engineId: 41000, position: { x: 0, y: 6, z: 0 }, name: 'Bailiff' }],
    [8, { engineId: 41001, position: { x: 0, y: 60, z: 0 }, name: 'Far bailiff' }],
  ]);
  assert.deepEqual(
    api.targets().map((target) => [target.id, target.kind, target.name]),
    [
      [2, 'player', 'Hans'],
      [7, 'npc', 'Bailiff'],
    ],
  );
  assert.equal(api.start({ id: 7 }), true);
  assert.deepEqual(asked.at(-1), {
    op: 'followStart',
    entity: 41000,
    hint: true,
    playerCanStop: true,
    run: false,
    distance: 0,
  });
  follow.receiveEvent({ op: 'followEvent', event: 'started', reason: '' });
  assert.equal(api.getTarget().kind, 'npc');
});

test('client Follow: another world for the player or the target, or a dead target, ends it', () => {
  const world = fixture();
  world.entities = world.follow.entities;
  world.entities.records.get(1).dimension = 0;
  world.entities.records.get(2).dimension = 0;
  world.api.start({ id: 2 });
  world.follow.check();
  assert.deepEqual(world.events, []);
  world.entities.records.get(1).dimension = 5;
  world.follow.check();
  assert.deepEqual(world.events.at(-1), ['followStopped', 2, 'worldChanged']);
  assert.deepEqual(world.asked.at(-1), { op: 'followStop' });

  const dead = fixture();
  dead.follow.entities.records.get(1).dimension = 0;
  const target = dead.follow.entities.records.get(2);
  target.dimension = 0;
  dead.api.start({ id: 2 });
  target.dimension = 0xffffffff;
  dead.follow.check();
  assert.deepEqual(dead.events, []);
  target.statSnapshot = { vitals: { health: 0 } };
  dead.follow.check();
  assert.deepEqual(dead.events.at(-1), ['followStopped', 2, 'unavailable']);

  const npc = fixture();
  npc.follow.entities.npcBodies = new Map([
    [7, { engineId: 41000, position: { x: 0, y: 6, z: 0 }, name: 'Bailiff' }],
  ]);
  npc.api.start({ id: 7 });
  npc.follow.entities.npcBodies = new Map();
  npc.follow.check();
  assert.deepEqual(npc.events.at(-1), ['followStopped', 7, 'unavailable']);
});
