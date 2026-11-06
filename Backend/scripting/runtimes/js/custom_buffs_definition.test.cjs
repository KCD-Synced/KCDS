'use strict';

// Buffs.register(id, definition): the GUID, the stacks, the
// events and the modifiers held while an instance lasts.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');

async function hostWith() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-custom-buffs-ref-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  let now = 100;
  host.buffClock = () => now;
  await host.startAll();
  const events = [];
  for (const name of ['playerCustomBuffApply', 'playerCustomBuffTick', 'playerCustomBuffRemove']) {
    host.on(name, (player, info, reason) => events.push([name, info.name, reason]));
  }
  return { host, output, events, advance: (seconds) => (now += seconds) };
}

function readyPlayer(host, id = 7) {
  return host.updatePlayer({
    id,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
}

const BLESSING = {
  name: 'Blessing',
  description: 'A priest blessed you.',
  visual: 'hangover',
  duration: 30,
  modifiers: { stamina: 2 },
};

function statChanges(output) {
  return output.filter((op) => op.op === 'addStat').map((op) => op.delta);
}

test('register(id, definition) returns a GUID, the same one again, and checks the definition', async () => {
  const { host } = await hostWith();
  const { Buffs } = host.api;
  const guid = Buffs.register('test:blessing', BLESSING);
  assert.match(guid, /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/);
  assert.equal(Buffs.register('test:blessing', { ...BLESSING }), guid);
  assert.throws(() => Buffs.register('test:blessing', { ...BLESSING, duration: 10 }), /another/);
  assert.deepEqual(Buffs.find(guid), {
    name: 'test:blessing',
    class: 'custom',
    aiTag: null,
    duration: 30,
    custom: true,
    displayName: 'Blessing',
    description: 'A priest blessed you.',
  });
  assert.equal(Buffs.find('hangover').custom, false);
  assert.throws(() => Buffs.register('test:x', { ...BLESSING, colour: 'red' }), /unknown key/);
  assert.throws(() => Buffs.register('test:x', { ...BLESSING, visual: 'nope' }), /visual/);
  assert.throws(
    () => Buffs.register('test:x', { ...BLESSING, tickInterval: 0.01 }),
    /tickInterval/,
  );
  assert.throws(() => Buffs.register('test:x', { ...BLESSING, maxStacks: 17 }), /maxStacks/);
  assert.throws(
    () =>
      Buffs.register('test:x', {
        ...BLESSING,
        modifiers: { stamina: { operation: 'divide', value: 1 } },
      }),
    /'add', 'multiply' or 'set'/,
  );
  assert.throws(
    () =>
      Buffs.register('test:x', {
        ...BLESSING,
        modifiers: { stamina: { operation: 'multiply', value: -1 } },
      }),
    /0 or more/,
  );
  assert.throws(
    () =>
      Buffs.register('test:x', {
        ...BLESSING,
        modifiers: { healthRegeneration: { operation: 'set', value: 1 } },
      }),
    /rate per second/,
  );
  assert.throws(
    () => Buffs.register('test:x', { ...BLESSING, modifiers: { morale: 1 } }),
    /derived stat/,
  );
  assert.throws(() => Buffs.register('hangover', BLESSING), /game buff/);
});

test('an instance holds its modifiers, ticks and raises apply, tick and remove', async () => {
  const { host, output, events, advance } = await hostWith();
  const { Buffs } = host.api;
  const guid = Buffs.register('test:warm', { ...BLESSING, tickInterval: 10 });
  const p = readyPlayer(host);
  assert.equal(p.addBuff(guid), true);
  assert.equal(p.hasBuff('test:warm'), true);
  assert.deepEqual(statChanges(output), [2]);
  const state = p.buffs.find((entry) => entry.name === 'test:warm');
  assert.equal(state.custom, true);
  assert.equal(state.displayName, 'Blessing');
  for (const step of [10, 10, 15]) {
    advance(step);
    host.customBuffs.process();
  }
  assert.deepEqual(events, [
    ['playerCustomBuffApply', 'test:warm', undefined],
    ['playerCustomBuffTick', 'test:warm', undefined],
    ['playerCustomBuffTick', 'test:warm', undefined],
    ['playerCustomBuffRemove', 'test:warm', 'expired'],
  ]);
  assert.deepEqual(statChanges(output), [2, -2], 'the modifier is taken back at the end');
  assert.equal(p.hasBuff('test:warm'), false);
});

test('stacks: up to maxStacks, then the oldest is refreshed or the new one ignored', async () => {
  const { host, output, events } = await hostWith();
  const { Buffs } = host.api;
  const stacking = Buffs.register('test:stack', { ...BLESSING, stackable: true, maxStacks: 2 });
  const capped = Buffs.register('test:capped', {
    ...BLESSING,
    stackable: true,
    maxStacks: 2,
    refresh: false,
  });
  const p = readyPlayer(host);
  p.addBuff(stacking);
  p.addBuff(stacking);
  p.addBuff(stacking);
  assert.equal(p.buffs.filter((entry) => entry.name === 'test:stack').length, 2);
  assert.deepEqual(
    events.filter(([, name]) => name === 'test:stack').map(([event, , reason]) => [event, reason]),
    [
      ['playerCustomBuffApply', undefined],
      ['playerCustomBuffApply', undefined],
      ['playerCustomBuffRemove', 'refreshed'],
      ['playerCustomBuffApply', undefined],
    ],
  );
  assert.equal(p.addBuff(capped), true);
  assert.equal(p.addBuff(capped), true);
  assert.equal(p.addBuff(capped), false, 'at the limit without refresh');
  output.length = 0;
  events.length = 0;
  assert.equal(p.removeBuff('test:stack'), true);
  assert.deepEqual(statChanges(output), [-2, -2]);
  assert.deepEqual(events, [
    ['playerCustomBuffRemove', 'test:stack', 'server'],
    ['playerCustomBuffRemove', 'test:stack', 'server'],
  ]);
});

test('multiply and set hold the change they made; healthRegeneration gives health every second', async (t) => {
  t.mock.timers.enable({ apis: ['setInterval'] });
  const { host, output } = await hostWith();
  const { Buffs } = host.api;
  const p = host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    vitals: {
      health: 80,
      stamina: 50,
      exhaust: 90,
      hunger: 70,
      bleeding: 0,
      sleeping: 0,
      consciousness: 1,
      drunkenness: 0,
      poisoning: 0,
    },
  });
  const strong = Buffs.register('test:strong', {
    ...BLESSING,
    modifiers: {
      stamina: { operation: 'multiply', value: 1.5 },
      hunger: { operation: 'set', value: 100 },
      healthRegeneration: 2,
    },
  });
  assert.equal(p.addBuff(strong), true);
  const changes = () => output.filter((op) => op.op === 'addStat').map((op) => [op.stat, op.delta]);
  // stamina 50 * 1.5 adds 25; hunger set to 100 adds 30 (the stat indexes of addStat).
  assert.deepEqual(changes(), [
    [1, 25],
    [3, 30],
  ]);
  t.mock.timers.tick(3000);
  assert.deepEqual(changes().slice(2), [
    [0, 2],
    [0, 2],
    [0, 2],
  ]);
  assert.equal(p.removeBuff(strong), true);
  t.mock.timers.tick(2000);
  // Exactly what the instance added is taken back; the health it gave stays.
  assert.deepEqual(changes().slice(5), [
    [1, -25],
    [3, -30],
  ]);
  assert.ok(Buffs.modifiers.includes('healthRegeneration'));
});

test('native modifiers travel to the player game as the buff params text of the game', async () => {
  const { host, output } = await hostWith();
  const { Buffs } = host.api;
  const p = readyPlayer(host);
  const guid = Buffs.register('test:giant', {
    ...BLESSING,
    modifiers: {
      maxStamina: { operation: 'multiply', value: 1.5 },
      carryCapacity: 100,
      charisma: -2.5,
      strength: { operation: 'set', value: 20 },
      stamina: 2,
    },
  });
  assert.equal(p.addBuff(guid), true);
  const show = output.find((op) => op.op === 'customBuff' && op.kind === 'show');
  assert.equal(show.params, 'mst*1.5,cap+100,charisma-2.5,strength=20');
  // Only the writable stat is changed by the server; the native ones are the game's.
  assert.deepEqual(statChanges(output), [2]);
  assert.ok(Buffs.modifiers.includes('carryCapacity'));
  assert.throws(
    () =>
      Buffs.register('test:bad', {
        ...BLESSING,
        modifiers: { maxStamina: { operation: 'multiply', value: -1 } },
      }),
    /0 or more/,
  );
});

test('a broken arm slows attacks and a broken leg stops sprinting', async () => {
  const { host, output } = await hostWith();
  const { Buffs } = host.api;
  const p = readyPlayer(host);
  const guid = Buffs.register('test:fracture', {
    ...BLESSING,
    modifiers: {
      attackSpeed: { operation: 'multiply', value: 0.8 },
      noSprint: true,
    },
  });
  assert.equal(p.addBuff(guid), true);
  const show = output.find((op) => op.op === 'customBuff' && op.kind === 'show');
  assert.equal(show.params, 'asp*0.8,LimitSprint');
  assert.deepEqual(statChanges(output), []);
  assert.ok(Buffs.modifiers.includes('attackSpeed'));
  assert.ok(Buffs.modifiers.includes('noSprint'));
  // Armour that rattles: louder and easier to notice.
  const rattle = Buffs.register('test:rattle', {
    ...BLESSING,
    modifiers: { noise: { operation: 'multiply', value: 1.4 }, conspicuousness: 0.2 },
  });
  assert.equal(p.addBuff(rattle), true);
  const rattling = output.filter((op) => op.op === 'customBuff' && op.kind === 'show').at(-1);
  assert.equal(rattling.params, 'noi*1.4,con+0.2');
  const off = Buffs.register('test:healed', { ...BLESSING, modifiers: { noSprint: false } });
  assert.equal(p.addBuff(off), true);
  const shows = output.filter((op) => op.op === 'customBuff' && op.kind === 'show');
  assert.equal(shows.at(-1).params, '');
  assert.throws(
    () => Buffs.register('test:bad-flag', { ...BLESSING, modifiers: { noSprint: 1 } }),
    /true or false/,
  );
});

test('hungerDrain and energyDrain take hunger and tire the player every second', async (t) => {
  t.mock.timers.enable({ apis: ['setInterval'] });
  const { host, output } = await hostWith();
  const { Buffs } = host.api;
  const p = host.updatePlayer({
    id: 8,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    vitals: {
      health: 80,
      stamina: 50,
      exhaust: 10,
      hunger: 70,
      bleeding: 0,
      sleeping: 0,
      consciousness: 1,
      drunkenness: 0,
      poisoning: 0,
    },
  });
  const drain = Buffs.register('test:drain', {
    ...BLESSING,
    modifiers: { hungerDrain: 3, energyDrain: 2 },
  });
  assert.equal(p.addBuff(drain), true);
  const changes = () => output.filter((op) => op.op === 'addStat').map((op) => [op.stat, op.delta]);
  t.mock.timers.tick(2000);
  // The stat indexes of addStat: exhaust 2, hunger 3.
  assert.deepEqual(changes(), [
    [3, -3],
    [2, 2],
    [3, -3],
    [2, 2],
  ]);
  assert.equal(p.removeBuff(drain), true);
  t.mock.timers.tick(2000);
  assert.equal(changes().length, 4);
  assert.ok(Buffs.modifiers.includes('hungerDrain'));
  assert.ok(Buffs.modifiers.includes('energyDrain'));
  assert.throws(
    () => Buffs.register('test:bad', { ...BLESSING, modifiers: { hungerDrain: -1 } }),
    /0 or more/,
  );
});
