'use strict';

// Injuries, alive and ableToAct: read from the owner's reports.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { ClientEntities } = require('../../../../Client/resources/entities.cjs');
const { directoryStats } = require('./stats/player_stats.cjs');

const INJURED_HEAD = 'c48e48e2-ae85-4429-9dd6-4fb94c388001';
const INJURED_LEFT_LEG = '10fc25ca-c095-44c6-b88b-d54ad58ab0a6';
const VITALS = {
  health: 100,
  stamina: 100,
  exhaust: 100,
  hunger: 100,
  bleeding: 0,
  sleeping: 0,
  consciousness: 1,
  drunkenness: 0,
  poisoning: 0,
};

function hostWith(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-injuries-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'medic');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'medic', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  const chat = () => output.filter((e) => e.op === 'chat').map((e) => e.text);
  return { host, chat, output };
}

function report(host, vitals) {
  return host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    vitals,
  });
}

test('injuries follow the injury buffs the owner reports, with their events', async () => {
  const { host, chat } = hostWith(`
    kcds.core.Events.on('playerInjured', (p, limb) => kcds.core.Chat.sendToAll('injured ' + limb));
    kcds.core.Events.on('playerInjuryHealed', (p, limb) => kcds.core.Chat.sendToAll('healed ' + limb));
  `);
  await host.startAll();
  const p = report(host, VITALS);
  const none = {
    head: false,
    torso: false,
    leftArm: false,
    rightArm: false,
    leftLeg: false,
    rightLeg: false,
  };
  assert.deepEqual({ ...p.injuries }, none, 'all false before a report');
  host.updateBuffs(7, [
    [INJURED_HEAD, '1', false],
    [INJURED_LEFT_LEG, '2', false],
  ]);
  assert.deepEqual({ ...p.injuries }, { ...none, head: true, leftLeg: true });
  assert.ok(Object.isFrozen(p.injuries));
  // Hit again while injured: nothing new.
  host.updateBuffs(7, [
    [INJURED_HEAD, '1', false],
    [INJURED_LEFT_LEG, '2', false],
  ]);
  host.updateBuffs(7, [[INJURED_LEFT_LEG, '2', false]]);
  assert.deepEqual({ ...p.injuries }, { ...none, leftLeg: true });
  assert.deepEqual(chat(), ['injured head', 'injured leftLeg', 'healed head']);
});

test('alive and ableToAct read health, consciousness and sleep', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const p = report(host, null);
  assert.equal(p.alive, false, 'no report yet');
  assert.equal(p.ableToAct, false);
  report(host, VITALS);
  assert.equal(p.alive, true);
  assert.equal(p.ableToAct, true);
  report(host, { ...VITALS, consciousness: 0 });
  assert.equal(p.alive, false, 'unconscious with health left is not alive');
  assert.equal(p.ableToAct, false);
  report(host, { ...VITALS, sleeping: 1 });
  assert.equal(p.ableToAct, false, 'asleep');
  report(host, { ...VITALS, health: 0 });
  assert.equal(p.alive, false);
  assert.equal(p.ableToAct, false);
});

test('a client reads alive and ableToAct of every player the same way', () => {
  const registry = new ClientEntities();
  const api = registry.facade();
  const entry = (id, vitals) => ({
    id,
    nativeId: id,
    kind: 'player',
    dimension: 0,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    ...directoryStats({ vitals }),
  });
  registry.updateDirectory({
    localPlayerId: 1,
    entities: [entry(1, VITALS), entry(2, { ...VITALS, consciousness: 0 })],
  });
  const me = api.LocalPlayer;
  const other = new api.Player(2);
  assert.deepEqual([me.alive, me.ableToAct], [true, true]);
  assert.deepEqual([other.alive, other.ableToAct], [false, false]);
});

test('a knocked-out player is not alive whatever health the game keeps, until revive', async () => {
  const { host } = hostWith('');
  await host.startAll();
  // The game keeps a knocked-out player at the immortality minimum, so health stays above 0.
  const p = report(host, { ...VITALS, health: 0.01 });
  assert.equal(p.alive, true);
  assert.equal(p.ableToAct, true);
  assert.equal(p.consciousness, 1);
  host.playerDied(7);
  assert.equal(p.alive, false, 'knocked out is not alive');
  assert.equal(p.health, 0.01);
  assert.equal(p.knockedOut, true);
  assert.equal(p.ableToAct, false);
  assert.equal(p.consciousness, 0);
  assert.equal(p.revive(), true);
  assert.equal(p.knockedOut, false);
  assert.equal(p.alive, true);
  assert.equal(p.ableToAct, true);
  assert.equal(p.consciousness, 1);
});

test('revive cures injuries, poisons and bleeding unless keepEffects is asked for', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  const p = report(host, { ...VITALS, health: 0.01 });
  const orders = () => output.filter((e) => e.op === 'buffOrder' && e.id === 7).length;
  host.playerDied(7);
  const before = orders();
  assert.equal(p.revive({ keepEffects: true }), true);
  assert.equal(orders(), before, 'keepEffects sends no cure');
  host.playerDied(7);
  assert.equal(p.revive(), true);
  assert.ok(orders() > before, 'the plain revive sends the cures');
  assert.throws(() => p.revive({ keepEffects: 'yes' }), TypeError);
  assert.throws(() => p.revive(null), TypeError);
});

test('revive on a disconnected player returns false instead of throwing', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const p = report(host, { ...VITALS, health: 0.01 });
  host.playerDied(7);
  assert.equal(p.knockedOut, true);
  host.disconnect(7);
  assert.equal(p.revive(), false);
});

test('server, local client and remote client agree on published life and action states', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const registry = new ClientEntities();
  const api = registry.facade();
  const entry = (id, vitals) => ({
    id,
    nativeId: id,
    kind: 'player',
    dimension: 0,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    ...directoryStats({ vitals }),
  });
  const states = [
    { name: 'not published', vitals: null, expected: [false, false] },
    { name: 'awake', vitals: VITALS, expected: [true, true] },
    {
      name: 'positive-health knockout',
      vitals: { ...VITALS, health: 1, consciousness: 0 },
      down: true,
      expected: [false, false],
    },
    {
      name: 'zero health',
      vitals: { ...VITALS, health: 0, consciousness: 0 },
      expected: [false, false],
    },
    {
      name: 'asleep with health left',
      vitals: { ...VITALS, sleeping: 1 },
      expected: [true, false],
    },
    { name: 'awake after revive', vitals: VITALS, expected: [true, true] },
  ];
  for (const state of states) {
    const player = report(host, state.vitals);
    if (state.down) {
      assert.equal(host.playerDied(7), true);
    }
    registry.updateDirectory({
      localPlayerId: 7,
      entities: [entry(7, state.vitals), entry(8, state.vitals)],
    });
    const local = api.LocalPlayer;
    const remote = new api.Player(8);
    for (const [side, handle] of [
      ['server', player],
      ['local client', local],
      ['remote client', remote],
    ]) {
      assert.deepEqual([handle.alive, handle.ableToAct], state.expected, `${state.name}: ${side}`);
      assert.equal(handle.health, state.vitals?.health ?? 0, `${state.name}: ${side} health`);
    }
    if (state.down) {
      assert.equal(player.revive(), true);
      // A revive request does not invent a new owner snapshot or bypass unconsciousness: the
      // player stays not alive until their game reports them conscious.
      assert.equal(player.alive, false);
      assert.equal(player.ableToAct, false);
      assert.equal(player.consciousness, 0);
    }
  }
});
