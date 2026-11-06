'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { ClientEntities } = require('../../../../Client/resources/entities.cjs');
const { directoryStats } = require('./stats/player_stats.cjs');

// Player stats: getStat, derivedStat, scaledStats, setStat and
// playerStatsChanged on the server; the same reads on a client. addStat and fillStat (KCDS).

function fixture() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcd2s-stats-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  // The needs orders every arriving player gets while the needs are off (their default, issue)
  // are not what these test.
  host.api.Needs.setEnabled(true);
  return { root, output, host };
}

function resource(root, name, code) {
  const dir = path.join(root, name);
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name, kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
}

const VITALS = {
  health: 100,
  stamina: 87.26,
  exhaust: 95,
  hunger: 80.5,
  bleeding: 0,
  sleeping: 0,
  consciousness: 1,
  drunkenness: 0,
  poisoning: 0,
};
const SAMPLED = {
  charisma: 12.5,
  visibility: 0.75,
  conspicuousness: 1,
  noise: 0.2,
  dirtiness: 0.1,
  bloodiness: 0,
  smell: 0.3,
  smellIntensity: 0.4,
  fragrance: 0,
  carriedWeight: 41.25,
  inventoryCapacity: 120,
  encumbrance: 0.34,
  hangover: 0,
  alcoholism: 0,
  armorRating: 25,
  overallArmorDefense: 30,
  overallWeaponAttack: 18,
  normalizedRunSpeed: 1,
  runSpeedBase: 4.5,
  morale: -2,
};
const RELATIVE = { strength: 0.2, agility: 0.1667, vitality: 0.3333 };

function report(host, extra = {}) {
  return host.updatePlayer({
    id: 7,
    nickname: 'Henry',
    playerIndex: 0,
    ready: true,
    position: { x: 10, y: 20, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    vitals: VITALS,
    stats: SAMPLED,
    scaledStats: RELATIVE,
    ...extra,
  });
}

test('reads: zero before the first report, the reported values after it', async () => {
  const f = fixture();
  await f.host.startAll();
  const { PlayerStat, DerivedPlayerStat } = f.host.api;
  const p = report(f.host, { ready: false, vitals: null, stats: null, scaledStats: null });
  assert.equal(p.getStat(PlayerStat.Health), 0);
  assert.equal(p.derivedStat(DerivedPlayerStat.Morale), 0);
  assert.deepEqual(p.scaledStats, { strength: 0, agility: 0, vitality: 0 });
  report(f.host);
  assert.equal(p.getStat(PlayerStat.Stamina), 87.26);
  assert.equal(p.getStat('hunger'), 80.5);
  assert.equal(p.derivedStat(DerivedPlayerStat.Consciousness), 1);
  assert.equal(p.derivedStat(DerivedPlayerStat.CarriedWeight), 41.25);
  assert.equal(p.derivedStat('morale'), -2);
  assert.deepEqual(p.scaledStats, RELATIVE);
  // A fresh object every read: changing it changes nothing.
  p.scaledStats.strength = 9;
  assert.equal(p.scaledStats.strength, 0.2);
  // Vitals before the first sample: the sampled stats read 0.
  report(f.host, { stats: null, scaledStats: null });
  assert.equal(p.getStat('health'), 100);
  assert.equal(p.derivedStat('charisma'), 0);
  for (const bad of ['mana', 'charisma', undefined, 3]) {
    assert.throws(() => p.getStat(bad), TypeError);
  }
  for (const bad of ['health', 'luck', null]) {
    assert.throws(() => p.derivedStat(bad), TypeError);
  }
  assert.equal(Object.keys(DerivedPlayerStat).length, 25);
  assert.deepEqual(Object.values(PlayerStat), ['health', 'stamina', 'exhaust', 'hunger']);
  f.host.disconnect(7);
  assert.equal(p.getStat('health'), 0);
  assert.deepEqual(p.scaledStats, { strength: 0, agility: 0, vitality: 0 });
});

test('playerStatsChanged: a baseline first, then every change of a report in one event', async () => {
  const f = fixture();
  resource(
    f.root,
    'watcher',
    `kcds.core.Events.on('playerStatsChanged', (p, changes) => kcds.core.Chat.sendToPlayer(p, JSON.stringify(changes)));`,
  );
  await f.host.startAll();
  const events = () => f.output.filter((e) => e.op === 'chat').map((e) => JSON.parse(e.text));
  report(f.host, { stats: null, scaledStats: null });
  // The first sample is a baseline too.
  report(f.host);
  assert.deepEqual(events(), []);
  report(f.host, { vitals: { ...VITALS, health: 60, bleeding: 2.5 } });
  // An unchanged report raises nothing.
  report(f.host, { vitals: { ...VITALS, health: 60, bleeding: 2.5 } });
  report(f.host, {
    vitals: { ...VITALS, health: 60, bleeding: 2.5 },
    stats: { ...SAMPLED, dirtiness: 0.5 },
    // Relative attributes are not stats the event reports.
    scaledStats: { ...RELATIVE, strength: 0.5 },
  });
  assert.deepEqual(events(), [
    [
      { stat: 'health', previous: 100, current: 60 },
      { stat: 'bleeding', previous: 0, current: 2.5 },
    ],
    [{ stat: 'dirtiness', previous: 0.1, current: 0.5 }],
  ]);
  // A new body (the stats gone, then back) is a baseline again.
  report(f.host, { vitals: null, stats: null, scaledStats: null });
  report(f.host);
  assert.equal(events().length, 2);
});

test('setStat: the request goes to the player, refused values throw', async () => {
  const f = fixture();
  await f.host.startAll();
  const p = report(f.host);
  const sent = () => f.output.filter((e) => e.op === 'setStat');
  assert.equal(p.setStat('health', 80), true);
  assert.equal(p.setStat('stamina', 0), true);
  assert.equal(p.setStat('exhaust', 1000000), true);
  assert.equal(p.setStat('hunger', 55.5), true);
  assert.equal(p.setStat('health', 0), true);
  assert.deepEqual(sent(), [
    { op: 'setStat', id: 7, stat: 0, value: 80 },
    { op: 'setStat', id: 7, stat: 1, value: 0 },
    { op: 'setStat', id: 7, stat: 2, value: 1000000 },
    { op: 'setStat', id: 7, stat: 3, value: 55.5 },
    { op: 'setStat', id: 7, stat: 0, value: 0 },
  ]);
  assert.throws(() => p.setStat('bleeding', 1), /derived stat/);
  assert.throws(() => p.setStat('morale', 1), TypeError);
  assert.throws(() => p.setStat('mana', 1), TypeError);
  for (const value of [NaN, Infinity, '5', null]) {
    assert.throws(() => p.setStat('health', value), TypeError);
  }
  for (const value of [-1, 1000001, 0.00001, 1e-50]) {
    assert.throws(() => p.setStat('health', value), RangeError);
  }
  // Stamina may go as low as a float carries; only health has a floor above zero.
  assert.equal(p.setStat('stamina', 0.00001), true);
  assert.throws(() => p.setStat('stamina', 1e-50), RangeError);
  assert.equal(sent().length, 6);
  // No living body: still loading, knocked out, gone.
  report(f.host, { ready: false });
  assert.equal(p.setStat('health', 50), false);
  report(f.host);
  f.host.playerDied(7);
  assert.equal(p.setStat('health', 50), false);
  f.host.revivePlayer(p);
  assert.equal(p.setStat('health', 50), true);
  f.host.disconnect(7);
  assert.equal(p.setStat('health', 50), false);
  assert.equal(sent().length, 7);
});

test('addStat: the change goes to the player, its game adds it; refused changes throw', async () => {
  const f = fixture();
  await f.host.startAll();
  const p = report(f.host);
  const sent = () => f.output.filter((e) => e.op === 'addStat');
  assert.equal(p.addStat('exhaust', 25), true);
  assert.equal(p.addStat('hunger', -30.5), true);
  assert.equal(p.addStat('stamina', 0), true);
  assert.equal(p.addStat('health', -1000000), true);
  assert.deepEqual(sent(), [
    { op: 'addStat', id: 7, stat: 2, delta: 25 },
    { op: 'addStat', id: 7, stat: 3, delta: -30.5 },
    { op: 'addStat', id: 7, stat: 1, delta: 0 },
    { op: 'addStat', id: 7, stat: 0, delta: -1000000 },
  ]);
  // The read stays the owner's report: addStat changes nothing until the owner reports again.
  assert.equal(p.getStat('exhaust'), 95);
  assert.throws(() => p.addStat('sleeping', 1), /derived stat/);
  assert.throws(() => p.addStat('mana', 1), TypeError);
  for (const delta of [NaN, -Infinity, '5', null, undefined]) {
    assert.throws(() => p.addStat('hunger', delta), TypeError);
  }
  for (const delta of [1000001, -1000001, 1e-50, -1e-50]) {
    assert.throws(() => p.addStat('hunger', delta), RangeError);
  }
  assert.equal(sent().length, 4);
  // No living body: still loading, knocked out, gone.
  report(f.host, { ready: false });
  assert.equal(p.addStat('hunger', 10), false);
  report(f.host);
  f.host.playerDied(7);
  assert.equal(p.addStat('hunger', 10), false);
  f.host.revivePlayer(p);
  assert.equal(p.addStat('hunger', 10), true);
  f.host.disconnect(7);
  assert.equal(p.addStat('hunger', 10), false);
  assert.equal(sent().length, 5);
});

test('fillStat: setStat to the largest value, which the owner clamps to its maximum', async () => {
  const f = fixture();
  await f.host.startAll();
  const p = report(f.host);
  const sent = () => f.output.filter((e) => e.op === 'setStat');
  assert.equal(p.fillStat('exhaust'), true);
  assert.equal(p.fillStat('hunger'), true);
  assert.equal(p.fillStat('stamina'), true);
  assert.equal(p.fillStat('health'), true);
  assert.deepEqual(sent(), [
    { op: 'setStat', id: 7, stat: 2, value: 1000000 },
    { op: 'setStat', id: 7, stat: 3, value: 1000000 },
    { op: 'setStat', id: 7, stat: 1, value: 1000000 },
    { op: 'setStat', id: 7, stat: 0, value: 1000000 },
  ]);
  assert.throws(() => p.fillStat('poisoning'), /derived stat/);
  assert.throws(() => p.fillStat('mana'), TypeError);
  f.host.playerDied(7);
  assert.equal(p.fillStat('hunger'), false);
  assert.equal(sent().length, 4);
});

test('client Player: the same reads from the native directory (stats as arrays)', () => {
  const registry = new ClientEntities();
  const api = registry.facade();
  const entry = (id, extra) => ({
    id,
    nativeId: id,
    kind: 'player',
    dimension: 0,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    ...extra,
  });
  registry.updateDirectory({
    localPlayerId: 1,
    entities: [
      entry(1, directoryStats({ vitals: VITALS, stats: SAMPLED, scaledStats: RELATIVE })),
      entry(2, directoryStats({ vitals: { ...VITALS, drunkenness: 3 } })),
    ],
  });
  assert.equal(api.PlayerStat.Hunger, 'hunger');
  assert.equal(api.DerivedPlayerStat.SmellIntensity, 'smellIntensity');
  const local = api.LocalPlayer;
  assert.equal(local.getStat('stamina'), 87.26);
  assert.equal(local.derivedStat('armorRating'), 25);
  assert.deepEqual(local.scaledStats, RELATIVE);
  const other = new api.Player(2);
  assert.equal(other.derivedStat('drunkenness'), 3);
  assert.equal(other.derivedStat('morale'), 0);
  assert.deepEqual(other.scaledStats, { strength: 0, agility: 0, vitality: 0 });
  assert.throws(() => other.getStat('poisoning'), TypeError);
  // Gone from the view: no snapshot.
  registry.updateDirectory({ localPlayerId: 1, entities: [entry(1, {})] });
  assert.equal(other.getStat('health'), 0);
  assert.equal(local.getStat('health'), 0);
  // Malformed native entries are refused.
  for (const bad of [
    directoryStats({ vitals: { ...VITALS, health: 'full' } }),
    { vitals: [1] },
    { vitals: VITALS },
    { stats: 5 },
    { scaledStats: [NaN, 0, 0] },
  ]) {
    assert.throws(
      () => registry.updateDirectory({ localPlayerId: 0, entities: [entry(4, bad)] }),
      TypeError,
    );
  }
});

test('setPace: the whole rule goes to the player, a key left out off', async () => {
  const f = fixture();
  await f.host.startAll();
  const p = report(f.host);
  const sent = () => f.output.filter((e) => e.op === 'movementMode');
  assert.equal(p.setPace({ walkEnforced: true }), true);
  assert.equal(p.setPace({ walkByDefault: true, walkEnforced: false }), true);
  assert.equal(p.setPace({}), true);
  assert.deepEqual(sent(), [
    { op: 'movementMode', id: 7, walkByDefault: false, walkEnforced: true },
    { op: 'movementMode', id: 7, walkByDefault: true, walkEnforced: false },
    { op: 'movementMode', id: 7, walkByDefault: false, walkEnforced: false },
  ]);
  for (const bad of [null, true, 'walk', [true], { walkEnforced: 1 }, { walkByDefault: 'yes' }]) {
    assert.throws(() => p.setPace(bad), TypeError);
  }
  // Still loading counts: the rule is for the connection.
  report(f.host, { ready: false });
  assert.equal(p.setPace({ walkEnforced: true }), true);
  f.host.disconnect(7);
  assert.equal(p.setPace({ walkEnforced: true }), false);
  assert.equal(sent().length, 4);
});

// Player readings: the vitals by name, the abilities, healthPercent, skills.
const ABILITIES = {
  maxHealth: 120,
  maxStamina: 110,
  staminaCap: 95.5,
  maxExhaust: 100,
  maxHunger: 100,
  level: 7,
  strength: 6,
  agility: 5.5,
  vitality: 10,
  skills: {
    fencing: 4,
    survival: 19,
    defense: 3,
    sword: 12,
    heavyWeapons: 2,
    marksmanship: 5,
    dagger: 1,
    largeWeapons: 9,
    unarmed: 6,
  },
  scaledSkills: {
    fencing: 0.1333,
    survival: 0.6333,
    defense: 0.1,
    sword: 0.4,
    heavyWeapons: 0.0667,
    marksmanship: 0.1667,
    dagger: 0.0333,
    largeWeapons: 0.3,
    unarmed: 0.2,
  },
};

test('readings: vitals, maxima, healthPercent, attributes, skills and level', async () => {
  const { host } = fixture();
  await host.startAll();
  const p = host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 10, y: 20, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  assert.equal(p.health, 0);
  assert.equal(p.healthPercent, 0);
  assert.deepEqual(p.skills.survival, 0);
  report(host, { abilities: ABILITIES });
  assert.equal(p.health, 100);
  assert.equal(p.stamina, 87.26);
  assert.equal(p.hunger, 80.5);
  assert.equal(p.consciousness, 1);
  assert.equal(p.maxHealth, 120);
  assert.equal(p.staminaCap, 95.5);
  assert.equal(p.healthPercent, 83.33);
  assert.equal(p.strength, 6);
  assert.equal(p.vitality, 10);
  assert.equal(p.level, 7);
  assert.deepEqual(p.skills, ABILITIES.skills);
  assert.deepEqual(p.scaledSkills, ABILITIES.scaledSkills);
  // A fresh object each read.
  p.skills.sword = 30;
  assert.equal(p.skills.sword, 12);
  assert.throws(
    () => report(host, { abilities: { ...ABILITIES, skills: { fencing: 4 } } }),
    TypeError,
  );
});

test('staminaCap stays at or below maxStamina', async () => {
  const { host } = fixture();
  await host.startAll();
  const p = report(host, { abilities: { ...ABILITIES, maxStamina: 80, staminaCap: 95.5 } });
  assert.equal(p.maxStamina, 80);
  assert.equal(p.staminaCap, 80);
  report(host, { abilities: { ...ABILITIES, maxStamina: 0, staminaCap: 40 } });
  assert.equal(p.staminaCap, 0);
  report(host, { abilities: { ...ABILITIES, maxStamina: 110, staminaCap: 60 } });
  assert.equal(p.staminaCap, 60);
});

test('setHealth(0, { fall: true }) drops the body at once; only with health 0', async () => {
  const { host, output } = fixture();
  await host.startAll();
  const p = report(host, { abilities: ABILITIES });
  output.length = 0;
  assert.equal(p.setHealth(0, { fall: true }), true);
  const order = output.find((e) => e.op === 'setStat');
  assert.deepEqual([order.value, order.fall], [0, true]);
  assert.throws(() => p.setHealth(10, { fall: true }), RangeError);
  assert.throws(() => p.setHealth(0, { fall: 'yes' }), TypeError);
  assert.throws(() => p.setHealth(0, null), TypeError);
});

test('setHealth, setHunger and heal: the orders that go to the player', async () => {
  const { host, output } = fixture();
  await host.startAll();
  const p = report(host, { abilities: ABILITIES });
  output.length = 0;
  assert.equal(p.setHealth(90), true);
  assert.equal(p.setHunger(60), true);
  assert.throws(() => p.setHealth(-1), RangeError);
  assert.equal(p.heal(), true);
  assert.equal(p.heal({ injuries: false, poisons: false, bleeding: false, health: 50 }), true);
  assert.equal(p.heal({ health: 150, injuries: false, poisons: false, bleeding: false }), true);
  assert.throws(() => p.heal({ health: -1 }), TypeError);
  assert.throws(() => p.heal('all'), TypeError);
  const cures = output
    .filter((e) => e.op === 'buffOrder' || e.op === 'setStat')
    .map((e) => (e.op === 'setStat' ? `stat ${e.stat} ${e.value}` : `${e.kind} ${e.buff}`));
  assert.deepEqual(cures, [
    'stat 0 90',
    'stat 3 60',
    'add 46683e3b-e261-412f-b402-99ee17dda62a',
    'add de68e56a-a74c-4447-874b-487b03c3fc6e',
    'remove 0c903899-fcc9-4cf2-9ee3-1130ac08b0fc',
    'stat 0 1000000',
    // 50 is below the 100 the player has: health is never lowered.
    'stat 0 150',
  ]);
});

test('client Player: the readings from the native directory', () => {
  const registry = new ClientEntities();
  const api = registry.facade();
  registry.updateDirectory({
    localPlayerId: 1,
    entities: [
      {
        id: 1,
        nativeId: 1,
        kind: 'player',
        dimension: 0,
        position: { x: 1, y: 2, z: 3 },
        rotation: { w: 1, x: 0, y: 0, z: 0 },
        ...directoryStats({ vitals: VITALS, abilities: ABILITIES }),
      },
    ],
  });
  const local = api.LocalPlayer;
  assert.equal(local.health, 100);
  assert.equal(local.maxStamina, 110);
  assert.equal(local.healthPercent, 83.33);
  assert.equal(local.agility, 5.5);
  assert.deepEqual(local.skills, ABILITIES.skills);
  assert.deepEqual(local.scaledSkills, ABILITIES.scaledSkills);
  assert.equal(local.level, 7);
  assert.throws(
    () =>
      registry.updateDirectory({
        localPlayerId: 0,
        entities: [
          {
            id: 4,
            nativeId: 4,
            kind: 'player',
            dimension: 0,
            position: { x: 1, y: 2, z: 3 },
            rotation: { w: 1, x: 0, y: 0, z: 0 },
            abilities: [1, 2, 3],
          },
        ],
      }),
    TypeError,
  );
});

test('server Player: velocity and gaze from the report, zero without', async () => {
  const { host } = fixture();
  await host.startAll();
  const p = report(host);
  assert.deepEqual({ ...p.velocity }, { x: 0, y: 0, z: 0 });
  report(host, { velocity: { x: 1, y: 2, z: 0 }, gaze: { x: 0, y: 1, z: 0 } });
  assert.deepEqual({ ...p.velocity }, { x: 1, y: 2, z: 0 });
  assert.deepEqual({ ...p.gaze }, { x: 0, y: 1, z: 0 });
});
