'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('../host.cjs');
const { arcOf, KINDS, onArc, shareOf } = require('./ballistics.cjs');

// SiegeEngine: the cycle, the shot the server decides, the blast and the wreck.

const SOUL = '210170d8-afb2-4cbd-9df5-da2a8e722af4';

async function started() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-siege-'));
  const output = [];
  const host = new ResourceHost(root, {
    entityIds: { first: 4097, count: 2048 },
    send: (e) => output.push(e),
    log: () => {},
  });
  await host.startAll();
  const api = host.makeAPI();
  const sent = (...ops) => output.filter((e) => ops.includes(e.op));
  return { host, api, output, sent };
}

function player(host, id, x, y = 0, world = 0xffffffff) {
  return host.updatePlayer({
    id,
    nickname: 'P' + id,
    ready: true,
    position: { x, y, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    dimension: world,
  });
}

const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

test('ballistics: ranges, falloff and the arc check', () => {
  const trebuchet = KINDS.trebuchet;
  const arc = arcOf(trebuchet, { x: 0, y: 0, z: 6 }, { x: 100, y: 0, z: 0 });
  assert.ok(arc.flight > 0 && arc.apex > 0);
  assert.equal(shareOf(100, 10, 0), 100);
  assert.equal(shareOf(100, 10, 10), 25);
  assert.equal(shareOf(100, 10, 11), 0);
  const from = { x: 0, y: 0 };
  const to = { x: 100, y: 0 };
  assert.equal(onArc(from, to, { x: 90, y: 2 }), true);
  assert.equal(onArc(from, to, { x: 50, y: 30 }), false);
  assert.equal(onArc(from, to, { x: 160, y: 0 }), false);
});

test('spawn replicates the engine, change-only, and destroy takes it away', async () => {
  const { api, host, sent } = await started();
  const engine = api.SiegeEngine.spawn('cannon', { x: 1, y: 2, z: 3 }, undefined, 7);
  assert.equal(engine.kind, 'cannon');
  assert.equal(engine.dimension, 7);
  assert.equal(engine.cycle, 'idle');
  assert.equal(engine.range, 120);
  assert.equal(engine.crewSeats, 0);
  assert.equal(api.SiegeEngine.getById(engine.id), engine);
  assert.deepEqual(api.SiegeEngine.all(8), []);
  const [spawn] = sent('siegeSpawn');
  assert.equal(spawn.kind, 'cannon');
  assert.equal(spawn.dimension, 7);
  assert.deepEqual(spawn.position, { x: 1, y: 2, z: 3 });
  engine.speed = 1;
  engine.usable = false;
  engine.showAimLine(false);
  assert.deepEqual(sent('siegeUpdate'), []);
  engine.usable = true;
  assert.equal(sent('siegeUpdate').length, 1);
  assert.throws(() => api.SiegeEngine.spawn('catapult', {}), RangeError);
  engine.destroy();
  assert.equal(sent('siegeDestroy').length, 1);
  assert.equal(api.SiegeEngine.getById(engine.id), null);
});

test('load runs the cycle on its timer; fire is refused until loaded and in range', async () => {
  const { api, sent } = await started();
  const engine = api.SiegeEngine.spawn('cannon', { x: 0, y: 0, z: 0 });
  engine.speed = 20;
  assert.equal(engine.fire({ x: 50, y: 0, z: 0 }).reason, 'is not loaded');
  const result = engine.load();
  assert.equal(result.accepted, true);
  assert.ok(Math.abs(result.seconds - 0.6) < 1e-9);
  assert.equal(engine.cycle, 'loading');
  assert.equal(engine.load().reason, 'is busy');
  await wait(750);
  assert.equal(engine.loaded, true);
  assert.equal(engine.load().reason, 'is already loaded');
  assert.match(engine.inReach({ x: 500, y: 0, z: 0 }), /further/);
  assert.match(engine.inReach({ x: 2, y: 0, z: 0 }), /closer/);
  assert.equal(engine.fire({ x: 500, y: 0, z: 0 }).accepted, false);
  assert.equal(sent('siegeShot').length, 0);
});

test('ammo is spent per load and an empty engine refuses', async () => {
  const { api } = await started();
  const engine = api.SiegeEngine.spawn('cannon', { x: 0, y: 0, z: 0 });
  engine.speed = 20;
  engine.ammo = 1;
  assert.equal(engine.load().accepted, true);
  assert.equal(engine.ammo, 0);
  await wait(750);
  engine.fire({ x: 50, y: 0, z: 0 });
  await wait(100);
  assert.equal(engine.load().reason, 'is out of ammunition');
});

test('a shot with nobody to see it lands on the target: players and NPCs take their share, engines wreck', async () => {
  const { api, host, sent } = await started();
  const origin = { x: 0, y: 0, z: 0 };
  const cannon = api.SiegeEngine.spawn('cannon', origin);
  const victim = api.SiegeEngine.spawn('cannon', { x: 100, y: 0, z: 0 });
  victim.maxHealth = 50;
  cannon.speed = 20;
  cannon.damage = 100;
  cannon.blastRadius = 20;
  const npc = host.npcs.Npc.create({ soul: SOUL, position: { x: 100, y: 5, z: 0 } });
  const before = npc.health;
  const events = [];
  const emit = host.emit.bind(host);
  host.emit = (name, ...args) => {
    events.push([name, ...args]);
    return emit(name, ...args);
  };
  cannon.load();
  await wait(750);
  assert.equal(cannon.fire({ x: 100, y: 0, z: 0 }).accepted, true);
  assert.equal(cannon.cycle, 'firing');
  await wait(100);
  assert.equal(cannon.cycle, 'idle');
  assert.equal(victim.wrecked, true);
  assert.equal(victim.health, 0);
  assert.ok(npc.health < before);
  const names = events.map((e) => e[0]);
  assert.ok(names.indexOf('siegeFire') < names.indexOf('siegeImpact'));
  assert.ok(names.includes('siegeEngineDamage') && names.includes('siegeEngineWreck'));
  const impact = events.find((e) => e[0] === 'siegeImpact');
  assert.equal(impact[1], cannon);
  assert.equal(impact[2].x, 100);
  // Wrecked: nothing loads it until repaired.
  assert.equal(victim.load().reason, 'is wrecked');
  victim.repair();
  assert.equal(victim.wrecked, false);
  assert.equal(victim.health, 50);
  assert.equal(sent('siegeShot').length, 0);
});

test('a player near the target is asked to report; the report is checked and then blasts players', async () => {
  const { api, host, sent } = await started();
  const watcher = player(host, 1, 100, 0);
  const bystander = player(host, 2, 105, 0);
  player(host, 3, 100, 0, 9);
  const cannon = api.SiegeEngine.spawn('cannon', { x: 0, y: 0, z: 0 });
  cannon.speed = 20;
  cannon.damage = 40;
  cannon.blastRadius = 10;
  cannon.load();
  await wait(750);
  cannon.fire({ x: 100, y: 0, z: 0 }, watcher);
  await wait(100);
  const [shot] = sent('siegeShot');
  assert.equal(shot.reporter, watcher.id);
  assert.equal(shot.id, cannon.id);
  assert.ok(shot.flight > 0);
  // Not the reporter: ignored.
  host.siege.impactReport(bystander, { shot: shot.shot, engine: cannon.id, position: shot.target });
  assert.equal(sent('addStat').length, 0);
  // A landing far off the arc is not believed: the shot comes down on the target.
  host.siege.impactReport(watcher, {
    shot: shot.shot,
    engine: cannon.id,
    position: { x: 50, y: 60, z: 0 },
  });
  const stats = sent('addStat');
  assert.deepEqual(stats.map((e) => e.id).sort(), [1, 2]);
  assert.ok(stats.every((e) => e.delta < 0));
  const near = stats.find((e) => e.id === 1);
  const far = stats.find((e) => e.id === 2);
  assert.ok(near.delta < far.delta);
  // A shot lands once.
  host.siege.impactReport(watcher, { shot: shot.shot, engine: cannon.id, position: shot.target });
  assert.equal(sent('addStat').length, 2);
});

test('operators: take the place, fire where they look, a handler may refuse, leaving lets go', async () => {
  const { api, host, sent } = await started();
  const who = player(host, 1, 1, 0);
  const cannon = api.SiegeEngine.spawn('cannon', { x: 0, y: 0, z: 0 });
  cannon.speed = 20;
  host.siege.use(who, { engine: cannon.id, action: 'operate' });
  assert.equal(cannon.operator, null);
  cannon.usable = true;
  let refuse = true;
  const emit = host.emit.bind(host);
  host.emit = (name, ...args) =>
    name === 'siegeEngineUse' && refuse ? false : emit(name, ...args);
  host.siege.use(who, { engine: cannon.id, action: 'operate' });
  assert.equal(cannon.operator, null);
  refuse = false;
  host.siege.use(who, { engine: cannon.id, action: 'operate' });
  assert.equal(cannon.operator, who);
  assert.equal(sent('siegeUpdate').at(-1).operator, 1);
  host.siege.use(who, { engine: cannon.id, action: 'load' });
  await wait(750);
  host.siege.use(who, { engine: cannon.id, action: 'fire', look: { x: 0, y: 1, z: 0 } });
  assert.equal(cannon.cycle, 'firing');
  assert.equal(sent('siegeUpdate').at(-1).target.y, 120);
  host.siege.use(who, { engine: cannon.id, action: 'leave' });
  assert.equal(cannon.operator, null);
  host.disconnect(1);
});

test('health: setting it to 0 wrecks and frees the operator; maxHealth heals', async () => {
  const { api, host } = await started();
  const who = player(host, 1, 1, 0);
  const trebuchet = api.SiegeEngine.spawn('trebuchet', { x: 0, y: 0, z: 0 });
  assert.equal(trebuchet.crew.length, 2);
  trebuchet.health = 0;
  assert.equal(trebuchet.wrecked, false);
  trebuchet.maxHealth = 30;
  assert.equal(trebuchet.health, 30);
  trebuchet.assignGunner(who);
  trebuchet.health = 0;
  assert.equal(trebuchet.wrecked, true);
  assert.equal(trebuchet.operator, null);
  assert.equal(trebuchet.aim({ x: 100, y: 0, z: 0 }).reason, 'is wrecked');
  trebuchet.maxHealth = 40;
  assert.equal(trebuchet.wrecked, false);
  assert.equal(trebuchet.load().accepted, true);
  assert.equal(trebuchet.cycle, 'drawing');
  trebuchet.destroy();
});
