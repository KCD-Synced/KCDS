'use strict';

// Door: the level's doors, whose state the server owns.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const GUID = '00000000deadbeef';

function entry(fields = {}) {
  return {
    guid: GUID,
    name: 'house_door',
    position: { x: 1, y: 2, z: 3 },
    open: false,
    side: false,
    locked: false,
    canBePicked: true,
    key: '',
    builtLocked: false,
    ...fields,
  };
}

async function setup(t) {
  const f = serverFixture(t);
  await f.host.startAll();
  const api = f.host.makeAPI();
  const a = f.report(7);
  const b = f.report(8);
  f.host.doors.report(a, [entry()]);
  f.sent.length = 0;
  const sets = () => f.sent.filter((m) => m.op === 'doorSet').map((m) => [m.id, m.open, m.locked]);
  return { f, api, a, b, sets, Door: api.Door, door: api.Door.find(GUID) };
}

test('a reported door is found by guid and id, once', async (t) => {
  const { f, a, Door, door } = await setup(t);
  assert.equal(door.guid, GUID);
  assert.equal(Door.find('0xDEADBEEF'), door);
  assert.equal(Door.getById(door.id), door);
  assert.equal(new Door(door.id), door);
  assert.equal(door.name, 'house_door');
  assert.equal(door.position.z, 3);
  f.host.doors.report(a, [entry()]);
  assert.equal(Door.all().length, 1);
  assert.equal(Door.find(GUID, 5), null);
  assert.throws(() => Door.find('xyz'), TypeError);
  assert.match(String(door), /closed, unlocked/);
});

test('Door.find takes the level files order of World.raycast too', async (t) => {
  const { f, a, Door } = await setup(t);
  f.host.doors.report(a, [entry({ guid: '4abcdc64ac536f17', name: 'shed_door' })]);
  const door = Door.find('4abcdc64ac536f17');
  assert.equal(door.name, 'shed_door');
  // The client's entityGuid and the level files write the same key in another order.
  assert.equal(Door.find('ac536f17dc644abc'), door);
  assert.equal(Door.find('ac536f17-dc64-4abc'), door);
  assert.equal(Door.find('AC536F17DC644ABC'), door);
  // The door's own form is looked for first.
  assert.equal(Door.find(GUID).guid, GUID);
  assert.equal(Door.find('ffffffffdc644abc'), null);
});

test('setOpen and locked reach the players once and only on change', async (t) => {
  const { f, sets, door } = await setup(t);
  assert.equal(door.setOpen(true), true);
  assert.equal(door.setOpen(true), false);
  assert.deepEqual(sets(), [
    [7, true, false],
    [8, true, false],
  ]);
  f.sent.length = 0;
  door.setOpen(false);
  f.sent.length = 0;
  door.locked = true;
  door.locked = true;
  assert.equal(door.lockedByScript, true);
  assert.equal(sets().length, 2);
  // Opening a locked door unlocks it.
  f.sent.length = 0;
  assert.equal(door.setOpen(true), true);
  assert.equal(door.locked, false);
  assert.equal(door.lockedByScript, false);
  assert.deepEqual(sets()[0], [7, true, false]);
});

test('an accepted use goes to the others; doorInteract can refuse', async (t) => {
  const { f, a, sets, door } = await setup(t);
  const seen = [];
  let allow = true;
  const off = f.host.on('doorInteract', (player, d, action, keySide) => {
    seen.push([player.id, d.guid, action, keySide]);
    return allow;
  });
  f.host.doors.use(a, GUID, 'open', true);
  assert.equal(door.open, true);
  assert.equal(door.side, true);
  assert.deepEqual(sets(), [[8, true, false]]);
  assert.deepEqual(seen, [[7, GUID, 'open', true]]);
  allow = false;
  f.sent.length = 0;
  f.host.doors.use(a, GUID, 'close', false);
  assert.equal(door.open, true);
  assert.deepEqual(sets(), [
    [7, true, false],
    [8, true, false],
  ]);
  off?.();
});

test('the server refuses what the rules forbid before any handler runs', async (t) => {
  const { f, a, sets, door } = await setup(t);
  let asked = 0;
  f.host.on('doorInteract', () => {
    asked += 1;
  });
  door.locked = true;
  f.sent.length = 0;
  f.host.doors.use(a, GUID, 'open', false);
  // A script lock is no use for a key, but a lockpick works.
  f.host.doors.use(a, GUID, 'unlock', true);
  assert.equal(asked, 0);
  assert.equal(door.locked, true);
  assert.equal(sets().length, 4);
  f.sent.length = 0;
  f.host.doors.use(a, GUID, 'lockpick', false);
  assert.equal(asked, 1);
  assert.equal(door.locked, false);
  assert.deepEqual(sets(), [[8, false, false]]);
});

test('a lockpick on a door that cannot be picked is refused', async (t) => {
  const { f, a, b } = await setup(t);
  f.host.doors.report(a, [entry({ guid: '2', canBePicked: false, locked: true })]);
  const door = f.host.makeAPI().Door.find('2');
  f.host.doors.use(a, '2', 'lockpick', false);
  assert.equal(door.locked, true);
  f.host.doors.use(a, '2', 'unlock', true);
  assert.equal(door.locked, false);
  assert.ok(b);
});

test('a player who reports a stale state is set right', async (t) => {
  const { f, sets, door } = await setup(t);
  door.setOpen(true);
  f.sent.length = 0;
  const late = f.report(9);
  f.host.doors.report(late, [entry()]);
  assert.deepEqual(sets(), [[9, true, false]]);
});

test('closestTo asks the player and resolves from the reply', async (t) => {
  const { f, a, Door } = await setup(t);
  const pending = Door.closestTo(a);
  const ask = f.sent.find((m) => m.op === 'doorNearest');
  assert.equal(ask.id, 7);
  f.host.doors.nearestReply(a, ask.request, entry({ guid: '9', name: 'gate_a' }));
  const answer = await pending;
  assert.equal(answer.found, true);
  assert.equal(answer.guid, '0000000000000009');
  assert.equal(answer.name, 'gate_a');
  assert.equal(answer.door, Door.find('9'));
  const none = Door.closestTo(a);
  f.host.doors.nearestReply(a, f.sent.at(-1).request, null, 'nothing in sight');
  assert.deepEqual(await none, { found: false, reason: 'nothing in sight', door: null });
  const left = Door.closestTo(a);
  f.host.doors.left(a);
  assert.equal((await left).reason, 'player left');
});

test("lockLevel overrides the level door's own for every player, and gives it back", async (t) => {
  const { f, a, door } = await setup(t);
  // The game reported none: the door's own is the game's default.
  assert.equal(door.lockLevel, 0.5);
  door.lockLevel = 0.9;
  door.lockLevel = 0.9;
  const difficulties = () =>
    f.sent.filter((m) => m.op === 'doorSet').map((m) => [m.id, m.difficulty]);
  assert.deepEqual(difficulties(), [
    [7, 0.9],
    [8, 0.9],
  ]);
  assert.equal(door.lockLevel, 0.9);
  // A player whose game shows the level's own is set right when it reports the door.
  f.sent.length = 0;
  f.host.doors.report(a, [entry({ difficulty: 0.3 })]);
  assert.deepEqual(difficulties(), [[7, 0.9]]);
  assert.throws(() => (door.lockLevel = 2), RangeError);
  f.sent.length = 0;
  door.lockLevel = null;
  assert.deepEqual(difficulties(), [
    [7, -1],
    [8, -1],
  ]);
  assert.equal(door.lockLevel, 0.5);
});

// Door.remove, restore, removed and door.destroy(): through level edits.
test('a door is taken out of a world and put back through a level edit', async (t) => {
  const { f, a, Door, door } = await setup(t);
  const edits = () => f.sent.filter((m) => m.op === 'levelEditApply' || m.op === 'levelEditRemove');
  Door.remove(GUID, 5);
  assert.equal(Door.find(GUID), door, 'only world 5 lost it');
  assert.deepEqual(
    edits().map((m) => [m.op, m.kind, m.guid, m.hidden, m.dimension]),
    // The entity key in the level files' order, as LevelEdit names it.
    [['levelEditApply', 'entity', 'deadbeef00000000', true, 5]],
  );
  assert.deepEqual(
    Door.removed().map((row) => [row.guid, row.name, row.position.z, row.dimension]),
    [[GUID, 'house_door', 3, 5]],
  );
  assert.equal(Door.restore(GUID, 6), null, 'world 6 had no removal');
  assert.equal(Door.restore(GUID, 5), door);
  assert.deepEqual(Door.removed(), []);
  // destroy() takes it out of its own world, the global one: out of every world.
  door.destroy();
  assert.equal(Door.find(GUID), null);
  assert.deepEqual(Door.all(), []);
  f.sent.length = 0;
  f.host.doors.use(a, GUID, 'open', false);
  assert.deepEqual(
    f.sent.filter((m) => m.op === 'doorSet'),
    [],
    'a removed door is not worked',
  );
  assert.equal(Door.restore(GUID), door);
  assert.equal(Door.find(GUID), door);
  assert.throws(() => Door.remove('00000000cafebabe'), /reported/);
  assert.throws(() => Door.remove('xyz'), TypeError);
  assert.throws(() => Door.removed(-1), TypeError);
});

test("Door.spawn hangs a door for its world, and it is worked like the level's own", async (t) => {
  const { f, a, Door } = await setup(t);
  assert.equal(Door.models().length > 60, true);
  const row = Door.models().find((model) => model.name === 'lvl1_door_a_left');
  assert.deepEqual(
    { ...row },
    {
      path: 'objects/characters/assets/doors/lvl1_door_a_left.cdf',
      name: 'lvl1_door_a_left',
      hinge: 'left',
      placements: row.placements,
    },
  );
  const paths = Door.models().map((model) => model.path);
  assert.deepEqual(paths, [...paths].sort());
  const door = Door.spawn({
    model: 'lvl1_door_a_left',
    position: [10, 20, 30],
    rotation: { x: 0, y: 0, z: 0, w: 2 },
    locked: true,
    lock: 'key',
    lockLevel: 0.75,
  });
  assert.equal(door.guid, '6b63647300000001');
  assert.equal(door.name, 'spawned_door_1');
  assert.equal(door.spawned, true);
  assert.equal(door.model, 'objects/characters/assets/doors/lvl1_door_a_left.cdf');
  assert.equal(door.locked, true);
  assert.equal(door.open, false);
  assert.equal(Door.find(door.guid, 0xffffffff), door);
  assert.equal(Door.all().includes(door), true);
  const [hang] = f.sent.filter((m) => m.op === 'doorSpawn');
  assert.deepEqual(hang, {
    op: 'doorSpawn',
    guid: '6b63647300000001',
    model: 'objects/characters/assets/doors/lvl1_door_a_left.cdf',
    position: { x: 10, y: 20, z: 30 },
    rotation: { x: 0, y: 0, z: 0, w: 1 },
    animSet: 'VillageLeft',
    helpers: 'DoorLeft',
    smartObjectType: '49515779-1a9d-330c-4768-c9326ea1ca96',
    rigid: true,
    pushable: true,
    locked: true,
    lockTypes: 'key/key',
    canBePicked: true,
    difficulty: 0.75,
    key: '',
    dimension: 0xffffffff,
  });
  assert.deepEqual(door.toJSON(), {
    model: 'objects/characters/assets/doors/lvl1_door_a_left.cdf',
    position: { x: 10, y: 20, z: 30 },
    rotation: { x: 0, y: 0, z: 0, w: 1 },
    name: 'spawned_door_1',
    locked: true,
    lock: 'key',
    canBePicked: true,
    lockLevel: 0.75,
  });
  // A player's game reports it as one of its doors: the same handle, set right.
  f.sent.length = 0;
  f.host.doors.report(a, [entry({ guid: door.guid, locked: false })]);
  assert.deepEqual(
    f.sent.filter((m) => m.op === 'doorSet').map((m) => [m.guid, m.locked]),
    [[door.guid, true]],
  );
  // A lockpick works it as on the level's doors.
  f.sent.length = 0;
  f.host.doors.use(a, door.guid, 'lockpick', false);
  assert.equal(door.locked, false);
  // A player whose world became ready hangs it again.
  f.sent.length = 0;
  f.host.doors.arrived(a);
  assert.deepEqual(
    f.sent.map((m) => [m.op, m.to, m.guid, m.locked]),
    [['doorSpawn', a.id, door.guid, false]],
  );
  // Taken down for good.
  f.sent.length = 0;
  door.destroy();
  assert.deepEqual(f.sent, [{ op: 'doorDespawn', guid: door.guid }]);
  assert.equal(Door.find(door.guid, 0xffffffff), null);
  assert.equal(Door.all().includes(door), false);
  assert.throws(() => Door.remove(door.guid), /reported/);
  // A level door has no definition.
  assert.equal(Door.find(GUID).toJSON(), null);
  assert.equal(Door.find(GUID).spawned, false);
});

test('Door.spawn refuses what it cannot hang', async (t) => {
  const { Door } = await setup(t);
  const hang = (change) => () =>
    Door.spawn({ model: 'lvl1_door_a_right', position: { x: 1, y: 2, z: 3 }, ...change });
  assert.throws(hang({ model: 'barrel_a' }), /unknown door model/);
  assert.throws(hang({ position: [1, 2] }), /position/);
  assert.throws(hang({ position: [1, 2, 20000] }), /position/);
  assert.throws(hang({ rotation: [0, 0, 0, 0] }), /rotation/);
  assert.throws(hang({ rotation: [0, 0, Number.NaN, 1] }), /rotation/);
  assert.throws(hang({ lock: 'padlock' }), /lock must be/);
  assert.throws(hang({ locked: 'yes' }), /locked is true or false/);
  assert.throws(hang({ lockLevel: 2 }), /lockLevel/);
  assert.throws(hang({ lockKey: 'no such key' }), /not an item class/);
  assert.throws(hang({ name: 'x'.repeat(65) }), /name/);
  assert.throws(
    () => Door.spawn({ model: 'lvl1_door_a_right', position: [1, 2, 3] }, -1),
    /dimension/,
  );
  const door = Door.spawn(
    {
      model: 'objects/characters/assets/doors/lvl1_door_a_right.cdf',
      position: [1, 2, 3],
      name: 'gate',
    },
    4,
  );
  assert.equal(door.name, 'gate');
  assert.equal(Door.find(door.guid, 4), door);
  assert.equal(Door.find(door.guid), null, 'found in the world it hangs in');
});

// Issue 016: a door an NPC's brain worked in its simulator's game reaches every other game.
test('an NPC that its sender simulates opens and closes a door for everyone', async (t) => {
  const { f, a, b, sets, api, door } = await setup(t);
  const npc = api.Npc.create({
    soul: '210170d8-afb2-4cbd-9df5-da2a8e722af4',
    position: { x: 2, y: 2, z: 3 },
  });
  // Not simulated by the sender yet: refused.
  assert.equal(f.host.doors.npcUse(a, npc.id, GUID, 'open'), false);
  f.host.npcs.simulatorChanged({ id: npc.id, simulatorId: 7 });
  // From another player than its simulator: refused.
  assert.equal(f.host.doors.npcUse(b, npc.id, GUID, 'open'), false);
  assert.equal(f.host.doors.npcUse(a, npc.id, GUID, 'open'), true);
  assert.equal(door.open, true);
  assert.deepEqual(sets(), [[8, true, false]]);
  // Locks are not the brain's to work.
  assert.equal(f.host.doors.npcUse(a, npc.id, GUID, 'unlock'), false);
  f.sent.length = 0;
  assert.equal(f.host.doors.npcUse(a, npc.id, GUID, 'close'), true);
  assert.equal(door.open, false);
  // A door a script locked stays shut for NPCs: refused, and the simulator gets it closed back.
  door.locked = true;
  f.sent.length = 0;
  assert.equal(f.host.doors.npcUse(a, npc.id, GUID, 'open'), false);
  assert.equal(door.open, false);
  assert.deepEqual(sets(), [[7, false, true]]);
  door.locked = false;
  // Too far from the door: refused.
  npc.position = { x: 40, y: 2, z: 3 };
  assert.equal(f.host.doors.npcUse(a, npc.id, GUID, 'open'), false);
});

test('doorDiscovered fires once per door and dimension, and a lock set there reaches the player', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  const api = f.host.makeAPI();
  const a = f.report(7);
  const met = [];
  api.Events.on('doorDiscovered', (door, player) => {
    met.push([door.guid, player.id]);
    door.locked = true;
  });
  f.host.doors.report(a, [entry()]);
  f.host.doors.report(a, [entry()]);
  assert.deepEqual(met, [[GUID, 7]]);
  assert.equal(api.Door.find(GUID).locked, true);
  const sets = f.sent.filter((m) => m.op === 'doorSet' && m.id === 7);
  assert.equal(sets.at(-1).locked, true);
});
