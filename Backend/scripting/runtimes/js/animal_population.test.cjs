'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { bucketOf } = require('./animals/population.cjs');
const { actorClassOf } = require('./npcs/npc_values.cjs');
const spawnData = require('./animals/spawn_points.json');

// The server's animal population: its settings, which spawn points it uses, when it
// spawns and removes the animals around players, and the respawn wait after a death.

const SOUL = '210170d8-afb2-4cbd-9df5-da2a8e722af4';
// The default soul of a kind, as the header of spawn_points.json has them (defaultSoulGuids).
const DEFAULT_SOULS = {
  RoeDeerHind: '44568aec-c7f1-44af-a154-ac832ff7c1c7',
  Wolf: 'f9418bb6-2f37-48f9-bae9-5a382aaa3533',
};
const START = 1_000_000;
const TICK = 2000;

// A host started on an empty folder, with the interval and the clock under the test's control.
async function startedHost(t, { enable = true } = {}) {
  t.mock.timers.enable({ apis: ['setInterval', 'Date'], now: START });
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-animals-'));
  const output = [];
  const host = new ResourceHost(root, {
    entityIds: { first: 4097, count: 2048 },
    send: (message) => output.push(message),
    log: () => {},
  });
  await host.startAll();
  const sent = (op) => output.filter((message) => message.op === op);
  const population = host.npcs.population;
  // A population starts switched off; most tests run it.
  if (enable) {
    host.npcs.Npc.setAnimalPopulation({ enabled: true });
  }
  const tick = (times = 1) => t.mock.timers.tick(TICK * times);
  // The player (id 1) at (x, y); calling it again moves him.
  const player = (x, y, extra = {}) =>
    host.updatePlayer({
      id: 1,
      nickname: 'P1',
      ready: true,
      position: { x, y, z: 10 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
      ...extra,
    });
  return { host, Npc: host.npcs.Npc, sent, population, tick, player };
}

// A spawn point entry as spawn_points.json has them.
function entry(index, kind, x, y, extra = {}) {
  const guid = `00000000-0000-0000-0000-${String(index).padStart(12, '0')}`;
  return { level: 'trosecko', type: 'spawner', kind, pos: [x, y, 10], count: 1, guid, ...extra };
}

// `count` points of one kind, 3 m apart in a row starting at (1000, 1000).
function row(kind, count, firstIndex = 0, extra = {}) {
  const entries = [];
  for (let index = 0; index < count; index += 1) {
    entries.push(entry(firstIndex + index, kind, 1000 + index * 3, 1000, extra));
  }
  return entries;
}

test('the settings start at their defaults, read as a frozen copy', async (t) => {
  const { Npc } = await startedHost(t, { enable: false });
  // Off until a resource switches it on.
  const settings = Npc.animalPopulation;
  assert.equal(settings.enabled, false);
  assert.equal(settings.level, null);
  assert.equal(settings.activationRadius, 120);
  assert.equal(settings.despawnRadius, 160);
  assert.equal(settings.maxAnimals, 300);
  assert.deepEqual(settings.kinds.Wolf, { enabled: true, percent: 100, respawnMinutes: 30 });
  assert.equal(Object.keys(settings.kinds).length, 15);
  assert.equal(Object.isFrozen(settings), true);
  assert.equal(Object.isFrozen(settings.kinds), true);
  assert.equal(Object.isFrozen(settings.kinds.Wolf), true);
  assert.throws(() => {
    settings.kinds.Wolf.percent = 1;
  }, TypeError);
  assert.notEqual(Npc.animalPopulation, settings);
});

test('setAnimalPopulation merges partial updates and returns the settings', async (t) => {
  const { Npc } = await startedHost(t);
  const first = Npc.setAnimalPopulation({ activationRadius: 80, kinds: { Wolf: { percent: 25 } } });
  assert.equal(first.activationRadius, 80);
  assert.equal(first.despawnRadius, 160);
  assert.deepEqual(first.kinds.Wolf, { enabled: true, percent: 25, respawnMinutes: 30 });
  Npc.setAnimalPopulation({ kinds: { Wolf: { respawnMinutes: 5 }, Hare: { enabled: false } } });
  const settings = Npc.animalPopulation;
  assert.deepEqual(settings.kinds.Wolf, { enabled: true, percent: 25, respawnMinutes: 5 });
  assert.equal(settings.kinds.Hare.enabled, false);
  assert.equal(settings.kinds.Boar.enabled, true);
  assert.equal(settings.activationRadius, 80);
  // Both radii in one call may move past the old ones.
  Npc.setAnimalPopulation({ activationRadius: 300, despawnRadius: 400, level: 'trosecko' });
  assert.equal(Npc.animalPopulation.despawnRadius, 400);
  assert.equal(Npc.animalPopulation.level, 'trosecko');
  Npc.setAnimalPopulation({ level: null, maxAnimals: 0 });
  assert.equal(Npc.animalPopulation.level, null);
  assert.equal(Npc.animalPopulation.maxAnimals, 0);
});

test('setAnimalPopulation refuses what it cannot use and changes nothing then', async (t) => {
  const { Npc } = await startedHost(t);
  const before = Npc.animalPopulation;
  for (const given of [null, 'wolves', 5, []]) {
    assert.throws(() => Npc.setAnimalPopulation(given), TypeError);
  }
  assert.throws(() => Npc.setAnimalPopulation({ enabled: 'yes' }), TypeError);
  assert.throws(() => Npc.setAnimalPopulation({ activationRadius: '5' }), TypeError);
  assert.throws(() => Npc.setAnimalPopulation({ kinds: [] }), TypeError);
  assert.throws(() => Npc.setAnimalPopulation({ kinds: { Wolf: null } }), TypeError);
  assert.throws(() => Npc.setAnimalPopulation({ kinds: { Wolf: { enabled: 1 } } }), TypeError);
  assert.throws(() => Npc.setAnimalPopulation({ colour: 'red' }), RangeError);
  assert.throws(() => Npc.setAnimalPopulation({ activationRadius: 0 }), RangeError);
  assert.throws(() => Npc.setAnimalPopulation({ activationRadius: NaN }), RangeError);
  assert.throws(() => Npc.setAnimalPopulation({ despawnRadius: Infinity }), RangeError);
  // The despawn radius must reach as far as the activation radius.
  assert.throws(() => Npc.setAnimalPopulation({ activationRadius: 200 }), RangeError);
  assert.throws(() => Npc.setAnimalPopulation({ despawnRadius: 100 }), RangeError);
  assert.throws(() => Npc.setAnimalPopulation({ maxAnimals: 1.5 }), RangeError);
  assert.throws(() => Npc.setAnimalPopulation({ maxAnimals: -1 }), RangeError);
  assert.throws(() => Npc.setAnimalPopulation({ level: 'klaster' }), RangeError);
  assert.throws(() => Npc.setAnimalPopulation({ kinds: { Unicorn: {} } }), RangeError);
  assert.throws(() => Npc.setAnimalPopulation({ kinds: { Chickens: {} } }), RangeError);
  assert.throws(() => Npc.setAnimalPopulation({ kinds: { Wolf: { percent: 101 } } }), RangeError);
  assert.throws(() => Npc.setAnimalPopulation({ kinds: { Wolf: { percent: -1 } } }), RangeError);
  assert.throws(() => Npc.setAnimalPopulation({ kinds: { Wolf: { speed: 1 } } }), RangeError);
  assert.throws(
    () => Npc.setAnimalPopulation({ kinds: { Wolf: { respawnMinutes: -1 } } }),
    RangeError,
  );
  // One bad part spoils the whole call.
  assert.throws(
    () => Npc.setAnimalPopulation({ enabled: false, kinds: { Wolf: { percent: 500 } } }),
    RangeError,
  );
  assert.deepEqual(Npc.animalPopulation, before);
});

test('the animal classes are accepted and the flock classes are not', () => {
  const kinds = new Set();
  for (const spawn of require('./animals/spawn_points.json').entries) {
    if (spawn.type !== 'flock') {
      kinds.add(spawn.kind);
    }
  }
  assert.ok(kinds.size >= 15);
  for (const kind of kinds) {
    assert.equal(actorClassOf(kind), kind);
  }
  assert.equal(actorClassOf('NPC'), 'NPC');
  assert.equal(actorClassOf('NPC_Female'), 'NPC_Female');
  assert.equal(actorClassOf(undefined), 'NPC');
  assert.throws(() => actorClassOf('Chickens'), RangeError);
  assert.throws(() => actorClassOf('Unicorn'), RangeError);
});

test('the hash of a point is stable and spreads over 0 to 100', () => {
  assert.equal(bucketOf('c0d4f2a8-d045-4f2b'), bucketOf('c0d4f2a8-d045-4f2b'));
  const buckets = [];
  for (let index = 0; index < 1000; index += 1) {
    const bucket = bucketOf(`point-${index}`);
    assert.ok(bucket >= 0 && bucket < 100);
    buckets.push(bucket);
  }
  const below = buckets.filter((bucket) => bucket < 30).length;
  assert.ok(below > 230 && below < 370, `${below} of 1000 below 30`);
});

test('percent 0 spawns nothing of a kind and 100 spawns every point', async (t) => {
  const { Npc, sent, population, tick, player } = await startedHost(t);
  population.setSpawnpoints([...row('Wolf', 40), ...row('Hare', 5, 100)]);
  Npc.setAnimalPopulation({ kinds: { Wolf: { percent: 0 } } });
  player(1000, 1000);
  tick();
  const classes = sent('npcSpawn').map((spawn) => spawn.actorClass);
  assert.deepEqual(classes, ['Hare', 'Hare', 'Hare', 'Hare', 'Hare']);
  Npc.setAnimalPopulation({ kinds: { Wolf: { percent: 100 } } });
  tick();
  const wolves = sent('npcSpawn').filter((spawn) => spawn.actorClass === 'Wolf');
  assert.equal(wolves.length, 40);
  assert.equal(Npc.animals().length, 45);
});

test('a percent in between uses the points whose hash is below it, and keeps them as it grows', async (t) => {
  const { Npc, population, tick, player } = await startedHost(t);
  const entries = row('Wolf', 40);
  population.setSpawnpoints(entries);
  player(1000, 1000);
  const spawnedAt = (percent) => {
    Npc.setAnimalPopulation({ kinds: { Wolf: { percent } } });
    tick();
    return Npc.animals().length;
  };
  const expected = (percent) => entries.filter((e) => bucketOf(e.guid) < percent).length;
  const quarter = spawnedAt(25);
  assert.equal(quarter, expected(25));
  const half = spawnedAt(50);
  assert.equal(half, expected(50));
  assert.ok(quarter > 0 && quarter < half && half < 40);
  // A lower percent takes the points above it away at the next tick.
  assert.equal(spawnedAt(10), expected(10));
  Npc.setAnimalPopulation({ kinds: { Wolf: { enabled: false } } });
  tick();
  assert.equal(Npc.animals().length, 0);
});

test('only spawner and placed points that are always there are used', async (t) => {
  const { Npc, population, sent, tick, player } = await startedHost(t);
  population.setSpawnpoints([
    entry(1, 'Wolf', 1000, 1000),
    entry(2, 'Wolf', 1000, 1000, { type: 'placed' }),
    entry(3, 'Wolf', 1000, 1000, { type: 'flock' }),
    entry(4, 'Wolf', 1000, 1000, { streamed: true }),
    // A spawner that shares a master's area spawns at its own position.
    entry(5, 'Hare', 1010, 1000, { master: 'd3195fd2-ab77-0f07' }),
    // Chickens are a flock class: no animal class, so no point.
    entry(6, 'Chickens', 1000, 1000),
    entry(7, 'Wolf', 20000, 1000),
  ]);
  player(1000, 1000);
  tick();
  const spawns = sent('npcSpawn');
  assert.deepEqual(spawns.map((spawn) => spawn.actorClass).sort(), ['Hare', 'Wolf', 'Wolf']);
  const hare = spawns.find((spawn) => spawn.actorClass === 'Hare');
  assert.deepEqual(hare.position, { x: 1010, y: 1000, z: 10 });
  assert.equal(Npc.animals().length, 3);
});

test('a level setting keeps the points of the other level silent', async (t) => {
  const { Npc, population, tick, player } = await startedHost(t);
  population.setSpawnpoints([
    entry(1, 'Wolf', 1000, 1000),
    entry(2, 'Boar', 1005, 1000, { level: 'kutnohorsko' }),
  ]);
  player(1000, 1000);
  Npc.setAnimalPopulation({ level: 'kutnohorsko' });
  tick();
  assert.deepEqual(
    Npc.animals().map((npc) => npc.actorClass),
    ['Boar'],
  );
  Npc.setAnimalPopulation({ level: 'trosecko' });
  tick();
  assert.deepEqual(
    Npc.animals().map((npc) => npc.actorClass),
    ['Wolf'],
  );
  Npc.setAnimalPopulation({ level: null });
  tick();
  assert.equal(Npc.animals().length, 2);
});

test('a player near a point spawns its animals; moving away removes them', async (t) => {
  const { Npc, population, sent, tick, player } = await startedHost(t);
  population.setSpawnpoints(
    [
      entry(1, 'RoeDeerHind', 1000, 1000, { count: 12, soul: 'animal_roe_deer_hind' }),
      entry(2, 'Wolf', 1030, 1000, { soulGuid: SOUL }),
    ],
    DEFAULT_SOULS,
  );
  player(1000, 1130);
  // 130 m from the deer and 133 m from the wolf: both outside the activation radius of 120 m.
  tick();
  assert.equal(sent('npcSpawn').length, 0);
  player(1000, 1110);
  tick();
  const spawns = sent('npcSpawn');
  // The group is capped at 8; the wolf at (1030, 1000) is 114 m away.
  assert.equal(spawns.filter((spawn) => spawn.actorClass === 'RoeDeerHind').length, 8);
  assert.equal(spawns.filter((spawn) => spawn.actorClass === 'Wolf').length, 1);
  for (const spawn of spawns) {
    assert.equal(spawn.name, '');
    assert.equal(spawn.dimension, 0xffffffff);
    if (spawn.actorClass === 'RoeDeerHind') {
      // A soul that is only a name is not used: the kind's default soul.
      assert.equal(spawn.soul, DEFAULT_SOULS.RoeDeerHind);
      const distance = Math.hypot(spawn.position.x - 1000, spawn.position.y - 1000);
      assert.ok(distance <= 2, `${distance} m from the point`);
    } else {
      assert.equal(spawn.soul, SOUL);
      assert.deepEqual(spawn.position, { x: 1030, y: 1000, z: 10 });
    }
  }
  assert.equal(Npc.animals().length, 9);
  assert.ok(Npc.animals().every((npc) => Npc.getById(npc.id) === npc));
  // Another tick with the player still there spawns nothing more.
  tick(3);
  assert.equal(sent('npcSpawn').length, 9);
  // Between the activation and the despawn radius (140 m) the animals stay.
  player(1000, 1140);
  tick();
  assert.equal(sent('npcDestroy').length, 0);
  // Beyond the despawn radius of 160 m from the deer (and the wolf) they go.
  const ids = Npc.animals().map((npc) => npc.id);
  player(1000, 1170);
  tick();
  assert.deepEqual(
    sent('npcDestroy')
      .map((destroy) => destroy.id)
      .sort(),
    ids.sort(),
  );
  assert.equal(Npc.animals().length, 0);
  // Back near, they are spawned again at once (no respawn wait), the same way.
  player(1000, 1100);
  tick();
  assert.equal(Npc.animals().length, 9);
  const second = sent('npcSpawn').slice(9);
  assert.deepEqual(
    second.map((spawn) => [spawn.actorClass, spawn.position]),
    spawns.map((spawn) => [spawn.actorClass, spawn.position]),
  );
});

test('an animal spawns with the soulGuid of its entry', async (t) => {
  const { population, sent, tick, player } = await startedHost(t);
  population.setSpawnpoints(
    [
      entry(1, 'Wolf', 1000, 1000, { soulGuid: SOUL.toUpperCase() }),
      // No soulGuid: the default soul of the kind.
      entry(2, 'Wolf', 1010, 1000),
      // Not a guid: also the default.
      entry(3, 'Wolf', 1020, 1000, { soulGuid: 'animal_wolf' }),
    ],
    DEFAULT_SOULS,
  );
  player(1000, 1000);
  tick();
  const soulAt = (x) => sent('npcSpawn').find((spawn) => spawn.position.x === x).soul;
  assert.equal(soulAt(1000), SOUL);
  assert.equal(soulAt(1010), DEFAULT_SOULS.Wolf);
  assert.equal(soulAt(1020), DEFAULT_SOULS.Wolf);
});

test('a population animal is not lootable: its carcass is gutted, not searched', async (t) => {
  const { Npc, population, tick, player } = await startedHost(t);
  population.setSpawnpoints([entry(1, 'Wolf', 1000, 1000)], DEFAULT_SOULS);
  player(1000, 1000);
  tick();
  const animals = Npc.animals();
  assert.equal(animals.length > 0, true);
  assert.equal(
    animals.every((npc) => npc.lootable === false),
    true,
  );
  // A script may still let players search one.
  animals[0].lootable = true;
  assert.equal(animals[0].lootable, true);
});

test('a placed point spawns with the default soul of its kind', async (t) => {
  const { population, sent, tick, player } = await startedHost(t);
  population.setSpawnpoints([entry(1, 'Wolf', 1000, 1000, { type: 'placed' })], DEFAULT_SOULS);
  player(1000, 1000);
  tick();
  const spawns = sent('npcSpawn');
  assert.equal(spawns.length, 1);
  assert.equal(spawns[0].soul, DEFAULT_SOULS.Wolf);
});

test('a kind without any soul is skipped, with one log line per kind', async (t) => {
  const { host, population, sent, tick, player } = await startedHost(t);
  const logs = [];
  host.bridge.log = (source, text) => logs.push(text);
  population.setSpawnpoints(
    [
      entry(1, 'Boar', 1000, 1000),
      entry(2, 'Boar', 1010, 1000, { type: 'placed' }),
      // A kind without a default still spawns where the entry names a soul.
      entry(3, 'Boar', 1020, 1000, { soulGuid: SOUL }),
      entry(4, 'Wolf', 1030, 1000),
    ],
    DEFAULT_SOULS,
  );
  player(1000, 1000);
  tick(3);
  const spawns = sent('npcSpawn');
  assert.deepEqual(
    spawns.map((spawn) => [spawn.actorClass, spawn.soul]),
    [
      ['Boar', SOUL],
      ['Wolf', DEFAULT_SOULS.Wolf],
    ],
  );
  const boarLogs = logs.filter((text) => text.includes('Boar'));
  assert.equal(boarLogs.length, 1);
  assert.ok(boarLogs[0].includes('no soul guid'));
  assert.equal(logs.filter((text) => text.includes('Wolf')).length, 0);
});

test('a player in another dimension or none at all spawns nothing', async (t) => {
  const { host, Npc, population, sent, tick, player } = await startedHost(t);
  population.setSpawnpoints(row('Wolf', 3));
  tick(2);
  assert.equal(sent('npcSpawn').length, 0);
  player(1000, 1000, { dimension: 5 });
  tick();
  assert.equal(sent('npcSpawn').length, 0);
  player(1000, 1000, { ready: false });
  tick();
  assert.equal(sent('npcSpawn').length, 0);
  player(1000, 1000);
  tick();
  assert.equal(Npc.animals().length, 3);
  // The last player leaves: everything goes.
  host.players.delete(1);
  tick();
  assert.equal(Npc.animals().length, 0);
  assert.equal(sent('npcDestroy').length, 3);
});

test('a disabled population spawns nothing and removes its animals', async (t) => {
  const { Npc, population, tick, player } = await startedHost(t);
  population.setSpawnpoints(row('Wolf', 3));
  player(1000, 1000);
  tick();
  assert.equal(Npc.animals().length, 3);
  Npc.setAnimalPopulation({ enabled: false });
  tick();
  assert.equal(Npc.animals().length, 0);
  tick(5);
  assert.equal(Npc.animals().length, 0);
  Npc.setAnimalPopulation({ enabled: true });
  tick();
  assert.equal(Npc.animals().length, 3);
});

test('after all animals of a point died it waits respawnMinutes before it spawns again', async (t) => {
  const { Npc, population, sent, tick, player } = await startedHost(t);
  population.setSpawnpoints([entry(1, 'Wolf', 1000, 1000, { count: 3 })]);
  Npc.setAnimalPopulation({ kinds: { Wolf: { respawnMinutes: 2 } } });
  player(1000, 1000);
  tick();
  const [first, second, third] = Npc.animals();
  assert.equal(sent('npcSpawn').length, 3);
  // Two of three dead: the group is still there and nothing is waiting.
  first.kill();
  second.kill();
  tick(5);
  assert.equal(sent('npcSpawn').length, 3);
  // The last one dies: the wait of two minutes starts.
  third.kill();
  tick(59);
  assert.equal(sent('npcSpawn').length, 3);
  assert.equal(sent('npcDestroy').length, 0);
  assert.equal(Npc.animals().length, 3);
  assert.equal(
    Npc.animals().every((npc) => !npc.alive),
    true,
  );
  // 2 minutes after the death: the bodies go and the group is spawned anew.
  tick(1);
  assert.equal(sent('npcSpawn').length, 6);
  assert.deepEqual(
    sent('npcDestroy').map((destroy) => destroy.id),
    [first.id, second.id, third.id],
  );
  assert.equal(Npc.animals().length, 3);
  assert.ok(Npc.animals().every((npc) => npc.alive));
  assert.equal(Npc.getById(first.id), null);
});

test('a revived animal ends the wait; bodies of a waiting point go when its player leaves', async (t) => {
  const { Npc, population, sent, tick, player } = await startedHost(t);
  population.setSpawnpoints([entry(1, 'Hare', 1000, 1000, { count: 2 })]);
  Npc.setAnimalPopulation({ kinds: { Hare: { respawnMinutes: 10 } } });
  player(1000, 1000);
  tick();
  const [first, second] = Npc.animals();
  first.kill();
  second.kill();
  assert.equal(first.revive(), true);
  // One is alive again, so the point does not wait: killing it starts the wait anew.
  tick(5);
  assert.equal(sent('npcSpawn').length, 2);
  first.kill();
  // The player leaves for a while: the bodies are removed, the wait stays.
  player(1000, 1200);
  tick();
  assert.equal(Npc.animals().length, 0);
  assert.equal(sent('npcDestroy').length, 2);
  player(1000, 1000);
  tick(60);
  assert.equal(sent('npcSpawn').length, 2);
  // 10 minutes after the death: a new group.
  tick(240);
  assert.equal(sent('npcSpawn').length, 4);
  assert.equal(Npc.animals().length, 2);
});

test('a respawn time of 0 spawns again at the next tick', async (t) => {
  const { Npc, population, tick, player } = await startedHost(t);
  population.setSpawnpoints([entry(1, 'Pig', 1000, 1000)]);
  Npc.setAnimalPopulation({ kinds: { Pig: { respawnMinutes: 0 } } });
  player(1000, 1000);
  tick();
  const [pig] = Npc.animals();
  pig.kill();
  tick();
  const [next] = Npc.animals();
  assert.notEqual(next.id, pig.id);
  assert.equal(next.alive, true);
});

test('at most maxAnimals animals live at a time; the dead do not count', async (t) => {
  const { Npc, population, tick, player } = await startedHost(t);
  population.setSpawnpoints(row('Boar', 10, 0, { count: 3 }));
  Npc.setAnimalPopulation({ maxAnimals: 7 });
  player(1000, 1000);
  tick();
  // Two groups of three fit; the third would make nine.
  assert.equal(Npc.animals().length, 6);
  tick(3);
  assert.equal(Npc.animals().length, 6);
  // Three die: they make room for another group (their bodies stay until it respawns).
  for (const npc of Npc.animals().slice(0, 3)) {
    npc.kill();
  }
  tick();
  assert.equal(Npc.animals().length, 9);
  assert.equal(Npc.animals().filter((npc) => npc.alive).length, 6);
  Npc.setAnimalPopulation({ maxAnimals: 100 });
  tick();
  assert.equal(Npc.animals().filter((npc) => npc.alive).length, 3 * 9);
  Npc.setAnimalPopulation({ maxAnimals: 0 });
  tick();
  assert.equal(Npc.animals().filter((npc) => npc.alive).length, 3 * 9);
});

test('Npc.animals lists only what the population spawned; removing one frees its point', async (t) => {
  const { Npc, population, sent, tick, player } = await startedHost(t);
  population.setSpawnpoints([entry(1, 'Wolf', 1000, 1000)]);
  const own = Npc.create({ position: { x: 1, y: 2, z: 3 } });
  player(1000, 1000);
  tick();
  const [wolf] = Npc.animals();
  assert.equal(wolf.actorClass, 'Wolf');
  assert.equal(Npc.animals().includes(own), false);
  assert.equal(Npc.all().length, 2);
  wolf.remove();
  assert.equal(Npc.animals().length, 0);
  tick();
  assert.equal(Npc.animals().length, 1);
  assert.equal(sent('npcSpawn').length, 3);
  // A resource that stops takes none of them: they belong to no resource.
  population.registry.resourceStopped('some-resource');
  assert.equal(Npc.animals().length, 1);
});

test('the game spawn points feed the population', async (t) => {
  const { Npc, sent, tick, player } = await startedHost(t);
  const entries = Npc.animalSpots('trosecko');
  const point = entries.find((e) => e.type === 'placed' && e.streamed !== true && e.kind === 'Dog');
  assert.ok(point);
  Npc.setAnimalPopulation({ level: 'trosecko' });
  player(point.pos[0], point.pos[1]);
  tick();
  const spawns = sent('npcSpawn');
  assert.ok(spawns.length > 0);
  assert.ok(spawns.some((spawn) => spawn.actorClass === 'Dog'));
  assert.ok(spawns.length <= 300);
  // The game's data gives every spawned animal a soul guid (a placed dog the default of its kind).
  assert.ok(spawns.every((spawn) => spawn.soul.length === 36));
  const dog = spawns.find((spawn) => spawn.actorClass === 'Dog');
  assert.equal(dog.soul, spawnData.defaultSoulGuids.Dog);
});

test('animalSpots returns copies, all or of one level', async (t) => {
  const { Npc } = await startedHost(t);
  const all = Npc.animalSpots();
  const trosecko = Npc.animalSpots('trosecko');
  const kutnohorsko = Npc.animalSpots('kutnohorsko');
  assert.equal(all.length, require('./animals/spawn_points.json').entries.length);
  assert.ok(trosecko.length > 0 && kutnohorsko.length > 0);
  assert.equal(trosecko.length + kutnohorsko.length, all.length);
  assert.ok(trosecko.every((e) => e.level === 'trosecko'));
  assert.ok(kutnohorsko.every((e) => e.level === 'kutnohorsko'));
  // Flock and streamed entries are in the list too: it is the game's data.
  assert.ok(all.some((e) => e.type === 'flock'));
  assert.ok(all.some((e) => e.streamed === true));
  all[0].kind = 'Changed';
  all[0].pos[0] = -1;
  assert.notEqual(Npc.animalSpots()[0].kind, 'Changed');
  assert.notEqual(Npc.animalSpots()[0].pos[0], -1);
  assert.throws(() => Npc.animalSpots('klaster'), RangeError);
  assert.throws(() => Npc.animalSpots(5), RangeError);
});

test('the host shutdown stops the tick; the animals stay', async (t) => {
  const { host, Npc, population, tick, player } = await startedHost(t);
  population.setSpawnpoints(row('Wolf', 2));
  player(1000, 1000);
  tick();
  assert.equal(Npc.animals().length, 2);
  await host.shutdown();
  assert.equal(population.timer, null);
  host.players.delete(1);
  tick(5);
  assert.equal(Npc.animals().length, 2);
});

// A host started on a folder that holds the owner's settings file (or none), with the log kept.
async function hostWithFile(t, content) {
  t.mock.timers.enable({ apis: ['setInterval', 'Date'], now: START });
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-animals-'));
  if (content !== undefined) {
    fs.writeFileSync(path.join(root, 'animal-population.json'), content);
  }
  const lines = [];
  const host = new ResourceHost(root, {
    entityIds: { first: 4097, count: 2048 },
    send: () => {},
    log: (resource, line) => lines.push(`${resource}: ${line}`),
  });
  await host.startAll();
  return { host, Npc: host.npcs.Npc, lines };
}

test('the settings file in the resources folder sets the population at start', async (t) => {
  const file = JSON.stringify({
    enabled: true,
    level: 'trosecko',
    maxAnimals: 120,
    maxPerPoint: 3,
    respawnMinutes: 10,
    kinds: { Wolf: 20, Hare: { percent: 50, respawnMinutes: 2 }, Boar: { enabled: false } },
  });
  const { Npc, lines } = await hostWithFile(t, file);
  const settings = Npc.animalPopulation;
  assert.equal(settings.enabled, true);
  assert.equal(settings.level, 'trosecko');
  assert.equal(settings.maxAnimals, 120);
  assert.equal(settings.maxPerPoint, 3);
  assert.deepEqual(settings.kinds.Wolf, { enabled: true, percent: 20, respawnMinutes: 10 });
  assert.deepEqual(settings.kinds.Hare, { enabled: true, percent: 50, respawnMinutes: 2 });
  assert.deepEqual(settings.kinds.Boar, { enabled: false, percent: 100, respawnMinutes: 10 });
  assert.deepEqual(settings.kinds.RoeDeerHind, { enabled: true, percent: 100, respawnMinutes: 10 });
  assert.deepEqual(lines, []);
});

test('without a settings file the defaults stay', async (t) => {
  const { Npc, lines } = await hostWithFile(t);
  assert.equal(Npc.animalPopulation.enabled, false);
  assert.equal(Npc.animalPopulation.maxPerPoint, 8);
  assert.deepEqual(lines, []);
});

test('a broken or refused settings file is logged and changes nothing', async (t) => {
  const broken = ['{ nope', '{"maxAnimals": -3, "enabled": true}', '{"kinds": {"Dragon": 5}}'];
  for (const content of broken) {
    await t.test(content, async (sub) => {
      const { Npc, lines } = await hostWithFile(sub, content);
      assert.equal(Npc.animalPopulation.enabled, false);
      assert.equal(Npc.animalPopulation.maxAnimals, 300);
      assert.equal(lines.length, 1);
      assert.match(lines[0], /animal population: animal-population\.json/);
    });
  }
});

test('a resource can change what the file set', async (t) => {
  const { Npc } = await hostWithFile(t, '{"enabled": true, "kinds": {"Wolf": 20}}');
  assert.equal(Npc.setAnimalPopulationEnabled(false).enabled, false);
  assert.equal(Npc.animalPopulation.kinds.Wolf.percent, 20);
});

test('setAnimalPopulationEnabled and setAnimalSpecies change only what they name', async (t) => {
  const { Npc } = await startedHost(t, { enable: false });
  assert.equal(Npc.setAnimalPopulationEnabled(true).enabled, true);
  assert.equal(Npc.setAnimalPopulationEnabled(false).enabled, false);
  assert.throws(() => Npc.setAnimalPopulationEnabled('yes'), TypeError);
  const wolf = Npc.setAnimalSpecies('Wolf', 25).kinds.Wolf;
  assert.deepEqual(wolf, { enabled: true, percent: 25, respawnMinutes: 30 });
  const next = Npc.setAnimalSpecies('Wolf', { enabled: false, respawnMinutes: 4 });
  assert.deepEqual(next.kinds.Wolf, { enabled: false, percent: 25, respawnMinutes: 4 });
  assert.equal(next.kinds.Hare.percent, 100);
  assert.throws(() => Npc.setAnimalSpecies('Dragon', 5), RangeError);
  assert.throws(() => Npc.setAnimalSpecies(5, 5), TypeError);
  assert.throws(() => Npc.setAnimalSpecies('Wolf', 101), RangeError);
  assert.throws(() => Npc.setAnimalSpecies('Wolf', null), TypeError);
  assert.deepEqual(Npc.animalPopulation.kinds.Wolf, next.kinds.Wolf);
});

test('respawnMinutes and maxPerPoint are checked', async (t) => {
  const { Npc } = await startedHost(t, { enable: false });
  assert.throws(() => Npc.setAnimalPopulation({ respawnMinutes: -1 }), RangeError);
  assert.throws(() => Npc.setAnimalPopulation({ respawnMinutes: '5' }), TypeError);
  assert.throws(() => Npc.setAnimalPopulation({ maxPerPoint: 0 }), RangeError);
  assert.throws(() => Npc.setAnimalPopulation({ maxPerPoint: 9 }), RangeError);
  assert.throws(() => Npc.setAnimalPopulation({ maxPerPoint: 2.5 }), RangeError);
  assert.equal(Npc.animalPopulation.maxPerPoint, 8);
});

test('maxPerPoint caps the group of a spawn point', async (t) => {
  const { Npc, population, tick, player } = await startedHost(t);
  population.setSpawnpoints(row('Hare', 2, 0, { count: 6 }));
  Npc.setAnimalPopulation({ maxPerPoint: 2 });
  player(1000, 1000);
  tick();
  assert.equal(Npc.animals().length, 4);
  // The cap also counts against maxAnimals: two points of 2 do not fit into 3.
  population.clearAll();
  Npc.setAnimalPopulation({ maxAnimals: 3 });
  tick();
  assert.equal(Npc.animals().length, 2);
});
