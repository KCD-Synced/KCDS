'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');

// Dog: what a resource's calls send the native server, the orders the modes give and
// the events the dogs raise.

const SOUL = '210170d8-afb2-4cbd-9df5-da2a8e722af4';

async function startedHost() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-dogs-'));
  const output = [];
  const host = new ResourceHost(root, {
    entityIds: { first: 4097, count: 2048 },
    send: (message) => output.push(message),
    log: () => {},
  });
  await host.startAll();
  const sent = (op) => output.filter((message) => message.op === op);
  const intents = () => sent('npcIntent').map((message) => message.intent);
  const events = [];
  for (const name of ['dogSpawn', 'dogDestroy', 'dogOwnerChanged', 'dogModeChanged']) {
    host.on(name, (...args) => events.push([name, ...args]));
  }
  return { host, Dog: host.dogs.Dog, Npc: host.npcs.Npc, sent, intents, events };
}

function somePlayer(host, id, position = { x: 10, y: 20, z: 30 }) {
  return host.updatePlayer({
    id,
    nickname: `P${id}`,
    ready: true,
    position,
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
}

test('spawn makes a Dog NPC at the owner, owned and following them', async () => {
  const { host, Dog, Npc, sent, intents, events } = await startedHost();
  const owner = somePlayer(host, 1);
  const dog = Dog.spawn(owner);
  const [spawn] = sent('npcSpawn');
  assert.equal(spawn.id, dog.id);
  assert.equal(spawn.actorClass, 'Dog');
  assert.deepEqual(spawn.position, { x: 10, y: 20, z: 30 });
  assert.equal(dog.owner, owner);
  assert.equal(dog.ownerId, 1);
  assert.equal(dog.mode, 1);
  assert.equal(dog.removedWithOwner, true);
  assert.equal(dog.objective, 'following');
  assert.equal(dog.morale, 0);
  assert.equal(dog.soul, '');
  assert.equal(dog.name, '');
  assert.deepEqual({ ...dog.position }, { x: 10, y: 20, z: 30 });
  assert.equal(Npc.getById(dog.id).intent, 'follow');
  assert.deepEqual(intents(), [`1|follow|1|jog|3`]);
  assert.equal(Dog.getById(dog.id), dog);
  assert.equal(new Dog(dog.id), dog);
  assert.deepEqual(Dog.all(), [dog]);
  assert.equal(Dog.getById(999), null);
  assert.throws(() => new Dog(999), /Unknown dog/);
  assert.equal(dog.toString(), `Dog(${dog.id}, soul none, owner 1, mode 1)`);
  assert.deepEqual(events, [['dogSpawn', dog]]);
});

test('spawn takes a position, rotation, soul and name', async () => {
  const { host, Dog, sent } = await startedHost();
  const owner = somePlayer(host, 1);
  const dog = Dog.spawn(owner, { x: 1, z: 3 }, { x: 0, y: 0, z: 90 }, SOUL, 'Vlcak');
  const [spawn] = sent('npcSpawn');
  assert.deepEqual(spawn.position, { x: 1, y: 20, z: 3 });
  assert.equal(spawn.soul, SOUL);
  assert.equal(spawn.name, 'Vlcak');
  assert.equal(dog.soul, SOUL);
  assert.equal(dog.name, 'Vlcak');
  assert.notEqual(spawn.rotation.z, 0);
  dog.name = 'Pes';
  assert.equal(dog.name, 'Pes');
  assert.equal(sent('npcName').at(-1).name, 'Pes');
  dog.position = { x: 5, y: 6, z: 7 };
  assert.deepEqual(sent('npcTeleport').at(-1).position, { x: 5, y: 6, z: 7 });
  // The shared state is the NPC's.
  dog.state.set('tag', 'a');
  assert.equal(host.npcs.Npc.getById(dog.id).state.get('tag'), 'a');
});

test('spawn refuses what it cannot use and spawns nothing', async () => {
  const { host, Dog, sent } = await startedHost();
  const owner = somePlayer(host, 1);
  assert.throws(() => Dog.spawn(null), /connected player/);
  assert.throws(() => Dog.spawn({ id: 77 }), /connected player/);
  assert.throws(() => Dog.spawn(owner, 5), TypeError);
  assert.throws(() => Dog.spawn(owner, null), TypeError);
  assert.throws(() => Dog.spawn(owner, undefined, 'north'), TypeError);
  assert.throws(() => Dog.spawn(owner, undefined, undefined, 'guard'), RangeError);
  assert.throws(() => Dog.spawn(owner, undefined, undefined, undefined, 5), TypeError);
  assert.equal(sent('npcSpawn').length, 0);
  assert.deepEqual(Dog.all(), []);
});

test('each mode sends its order and dogModeChanged follows a change only', async () => {
  const { host, Dog, sent, intents, events } = await startedHost();
  const owner = somePlayer(host, 1);
  const dog = Dog.spawn(owner);
  events.length = 0;
  // Wait: holds, which empties the intent field and freezes the body.
  dog.mode = 0;
  assert.equal(dog.mode, 0);
  assert.equal(intents().at(-1), '');
  assert.deepEqual(sent('npcFrozen').at(-1), { op: 'npcFrozen', id: dog.id, frozen: true });
  // Follow again frees the body and trails the owner.
  dog.mode = 1;
  assert.deepEqual(sent('npcFrozen').at(-1), { op: 'npcFrozen', id: dog.id, frozen: false });
  assert.equal(intents().at(-1), '3|follow|1|jog|3');
  // Free, Aggressive, Guard and Ambush are dogMode orders for the client to carry out.
  for (const mode of [2, 3, 6, 7]) {
    dog.mode = mode;
    assert.equal(dog.mode, mode);
    assert.match(intents().at(-1), new RegExp(`^\\d+\\|dogMode\\|${mode}\\|1$`));
    assert.equal(host.npcs.Npc.getById(dog.id).intent, 'hold');
    assert.equal(dog.objective, 'idle');
  }
  assert.deepEqual(
    events.map(([name, , mode]) => [name, mode]),
    [0, 1, 2, 3, 6, 7].map((mode) => ['dogModeChanged', mode]),
  );
  // The same mode again is a fresh order and no event.
  const before = intents().length;
  events.length = 0;
  dog.mode = 7;
  assert.equal(intents().length, before + 1);
  assert.deepEqual(events, []);
});

test('mode refuses a value outside 0 to 7', async () => {
  const { host, Dog, intents } = await startedHost();
  const dog = Dog.spawn(somePlayer(host, 1));
  for (const bad of [-1, 8, 1.5, '1', null, NaN]) {
    assert.throws(() => (dog.mode = bad), RangeError, String(bad));
  }
  assert.equal(dog.mode, 1);
  assert.equal(intents().length, 1);
  assert.throws(() => (dog.removedWithOwner = 'yes'), TypeError);
});

test('giveTo hands the dog over, orders it for the new owner and raises dogOwnerChanged', async () => {
  const { host, Dog, intents, events } = await startedHost();
  const first = somePlayer(host, 1);
  const second = somePlayer(host, 2);
  const dog = Dog.spawn(first);
  events.length = 0;
  dog.giveTo(second);
  assert.equal(dog.owner, second);
  assert.equal(dog.ownerId, 2);
  assert.equal(intents().at(-1), '2|follow|2|jog|3');
  assert.deepEqual(events, [['dogOwnerChanged', dog, second]]);
  // The same owner changes nothing.
  dog.giveTo(second);
  assert.equal(events.length, 1);
  // Nobody: masterless, holding where it stands.
  dog.giveTo(null);
  assert.equal(dog.owner, null);
  assert.equal(dog.ownerId, 0);
  assert.equal(intents().at(-1), '');
  assert.deepEqual(events.at(-1), ['dogOwnerChanged', dog, null]);
  // A masterless dog keeps its mode and takes the order of it again from the next owner.
  dog.giveTo(first);
  assert.equal(intents().at(-1), '4|follow|1|jog|3');
  assert.throws(() => dog.giveTo({ id: 99 }), /connected player/);
  assert.throws(() => dog.giveTo(undefined), /connected player/);
  assert.equal(dog.ownerId, 1);
});

test('an owner who leaves takes the dog along, unless removedWithOwner is false', async () => {
  const { host, Dog, sent, events } = await startedHost();
  const leaving = somePlayer(host, 1);
  const staying = somePlayer(host, 2);
  const gone = Dog.spawn(leaving);
  const kept = Dog.spawn(leaving);
  const other = Dog.spawn(staying);
  kept.removedWithOwner = false;
  events.length = 0;
  host.disconnect(1);
  assert.equal(Dog.getById(gone.id), null);
  assert.equal(Dog.getById(kept.id), kept);
  assert.equal(Dog.getById(other.id), other);
  assert.deepEqual(
    sent('npcDestroy').map((message) => message.id),
    [gone.id],
  );
  assert.deepEqual(events, [
    ['dogDestroy', gone],
    ['dogOwnerChanged', kept, null],
  ]);
  assert.equal(kept.owner, null);
  assert.equal(kept.ownerId, 0);
  kept.giveTo(staying);
  assert.equal(kept.owner, staying);
});

test('destroy raises dogDestroy while the handle resolves, then despawns the NPC', async () => {
  const { host, Dog, Npc, sent, events } = await startedHost();
  const owner = somePlayer(host, 1);
  const dog = Dog.spawn(owner, undefined, undefined, SOUL, 'Pes');
  const seen = [];
  host.on('dogDestroy', (who) => {
    seen.push([Dog.getById(who.id) === who, who.name, who.owner === owner]);
    // A handler that destroys it again changes nothing.
    who.destroy();
  });
  events.length = 0;
  dog.destroy();
  assert.deepEqual(seen, [[true, 'Pes', true]]);
  assert.deepEqual(events, [['dogDestroy', dog]]);
  assert.equal(sent('npcDestroy').length, 1);
  assert.equal(Dog.getById(dog.id), null);
  assert.equal(Npc.getById(dog.id), null);
  assert.deepEqual(Dog.all(), []);
  // A removed dog sends nothing more and its setters throw.
  const messages = sent('npcIntent').length;
  dog.destroy();
  assert.equal(sent('npcDestroy').length, 1);
  assert.throws(() => (dog.mode = 0), /Unknown dog/);
  assert.throws(() => (dog.name = 'x'), /Unknown dog/);
  assert.throws(() => dog.giveTo(null), /Unknown dog/);
  assert.equal(sent('npcIntent').length, messages);
});

test('removing the NPC removes the dog, with dogDestroy', async () => {
  const { host, Dog, Npc, events } = await startedHost();
  const dog = Dog.spawn(somePlayer(host, 1));
  events.length = 0;
  Npc.getById(dog.id).remove();
  assert.deepEqual(events, [['dogDestroy', dog]]);
  assert.equal(Dog.getById(dog.id), null);
  const second = Dog.spawn(somePlayer(host, 2));
  events.length = 0;
  Npc.removeAll();
  assert.deepEqual(events, [['dogDestroy', second]]);
});

test('a Dog is an Entity and the generic lookup finds it', async () => {
  const { host, Dog } = await startedHost();
  const dog = Dog.spawn(somePlayer(host, 1));
  assert.ok(dog instanceof host.api.Entity);
  assert.equal(new host.api.Entity(dog.id), dog);
  assert.equal(dog.dimension, host.npcs.Npc.getById(dog.id).dimension);
});

test('Player.dog follows spawn, giveTo and despawn, and is null without a dog', async () => {
  const { host, Dog } = await startedHost();
  const first = somePlayer(host, 1);
  const second = somePlayer(host, 2);
  assert.equal(first.dog, null);
  const dog = Dog.spawn(first);
  assert.equal(first.dog, dog);
  assert.equal(second.dog, null);
  // Handed over: the old owner has none, the new one has it.
  dog.giveTo(second);
  assert.equal(first.dog, null);
  assert.equal(second.dog, dog);
  // Masterless: nobody's.
  dog.giveTo(null);
  assert.equal(second.dog, null);
  dog.giveTo(first);
  assert.equal(first.dog, dog);
  // One dog per player: the oldest is the one read, and the next shows once it is gone.
  const other = Dog.spawn(first);
  assert.equal(first.dog, dog);
  dog.destroy();
  assert.equal(first.dog, other);
  other.destroy();
  assert.equal(first.dog, null);
});

test('an owner who leaves loses the dog from the index, kept or not', async () => {
  const { host, Dog } = await startedHost();
  const owner = somePlayer(host, 1);
  const gone = Dog.spawn(owner);
  const kept = Dog.spawn(owner);
  kept.removedWithOwner = false;
  host.disconnect(1);
  assert.equal(Dog.getById(gone.id), null);
  assert.equal(kept.ownerId, 0);
  assert.equal(host.dogs.dogOf({ id: 1 }), null);
  assert.equal(host.dogs.byOwner.size, 0);
  const next = somePlayer(host, 2);
  kept.giveTo(next);
  assert.equal(next.dog, kept);
});

// Search roams around the owner; Hunt goes for the nearest living animal and stays with
// the owner while there is none.
test('Search sends the dog to places around its owner', async () => {
  const { host, Dog, intents } = await startedHost();
  const owner = somePlayer(host, 1, { x: 100, y: 100, z: 5 });
  const dog = Dog.spawn(owner);
  dog.mode = 4;
  assert.equal(dog.objective, 'searching');
  const [, kind, x, y, z, speed] = intents().at(-1).split('|');
  assert.equal(kind, 'moveTo');
  assert.ok(Math.hypot(Number(x) - 100, Number(y) - 100) <= 12);
  assert.equal(Number(z), 5);
  assert.equal(speed, 'jog');
  const sent = intents().length;
  host.dogs.behave(dog, Date.now());
  assert.equal(intents().length, sent, 'no new place within four seconds');
  host.dogs.behave(dog, Date.now() + 5000);
  assert.equal(intents().length, sent + 1);
});

test('Hunt chases the nearest living animal, bites it within reach and follows the owner after', async () => {
  const { host, Dog, Npc, intents } = await startedHost();
  const owner = somePlayer(host, 1, { x: 0, y: 0, z: 0 });
  const dog = Dog.spawn(owner);
  dog.mode = 5;
  assert.equal(dog.objective, 'following');
  assert.match(intents().at(-1), /\|follow\|1\|/);
  // The global world's game (the population's) is hunted in every world.
  Npc.create({ class: 'Boar', position: { x: 30, y: 0, z: 0 }, dimension: 0xffffffff });
  const near = Npc.create({ class: 'RoeDeerHind', position: { x: 10, y: 0, z: 0 } });
  Npc.create({ class: 'Hare', position: { x: 60, y: 0, z: 0 } });
  const bites = [];
  host.on('npcDamage', (npc, attacker, amount, part, reason) =>
    bites.push([npc.id, attacker?.id, amount, reason]),
  );
  const now = Date.now();
  host.dogs.behave(dog, now);
  assert.equal(dog.objective, 'hunting');
  const [, kind, x, , , speed] = intents().at(-1).split('|');
  assert.equal(kind, 'moveTo');
  assert.equal(Number(x), 10);
  assert.equal(speed, 'run');
  // Within reach it bites: the dog is the attacker.
  near.teleport({ x: 1, y: 0, z: 0 });
  host.dogs.behave(dog, now + 1000);
  assert.deepEqual(bites, [[near.id, dog.id, 12, 'scripted']]);
  near.kill();
  host.dogs.behave(dog, now + 2000);
  assert.equal(dog.objective, 'hunting', 'the boar is next');
});

// a player who locks on to a dog makes it fight them, so the game's lock-on takes it;
// not its owner, nor an animal that flees.
test('a lock-on makes a dog fight the player, not its owner, and leaves a deer alone', async () => {
  const { host, Dog, Npc, intents } = await startedHost();
  const owner = somePlayer(host, 1, { x: 0, y: 0, z: 0 });
  // A health report: the server counts the stranger alive, so the dog can reach them.
  const stranger = host.updatePlayer({
    id: 2,
    nickname: 'P2',
    ready: true,
    position: { x: 2, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    vitals: {
      health: 100,
      stamina: 100,
      exhaust: 100,
      hunger: 100,
      bleeding: 0,
      sleeping: 0,
      consciousness: 1,
      drunkenness: 0,
      poisoning: 0,
    },
  });
  const dog = Dog.spawn(owner);
  const deer = Npc.create({ class: 'RoeDeerHind', position: { x: 1, y: 0, z: 0 } });
  assert.equal(host.npcs.engaged({ id: dog.id, playerId: owner.id }), false);
  assert.equal(host.npcs.engaged({ id: deer.id, playerId: stranger.id }), false);
  assert.equal(host.npcs.engaged({ id: dog.id, playerId: stranger.id }), true);
  assert.match(intents().at(-1), new RegExp(String.raw`^\d+\|attack\|${stranger.id}\|`));
  assert.equal(host.npcs.engaged({ id: 99999, playerId: stranger.id }), false);
});
