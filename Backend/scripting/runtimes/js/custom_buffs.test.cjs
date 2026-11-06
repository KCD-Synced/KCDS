'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');

const PREFIX = 'kcds-custom-buffs-';

// A host running one resource with `code`; `advance(seconds)` moves the buff clock.
async function hostWith(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), PREFIX));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'custom');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'custom', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  let now = 100;
  host.buffClock = () => now;
  await host.startAll();
  return { host, output, advance: (seconds) => (now += seconds) };
}

function readyPlayer(host, id = 7) {
  return host.updatePlayer({
    id,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
}

const DEFINITION = { name: 'warmth', icon: 'hangover', duration: 30 };

function shown(output) {
  return output.filter((op) => op.op === 'customBuff');
}

test('register: validates the definition and returns a frozen handle', async () => {
  const { host } = await hostWith('');
  const { Buffs } = host.api;
  const handle = Buffs.register({ ...DEFINITION, label: 'Warmth' });
  assert.deepEqual(handle, {
    name: 'warmth',
    icon: 'hangover',
    label: 'Warmth',
    duration: 30,
    tickSeconds: null,
  });
  assert.ok(Object.isFrozen(handle));
  assert.throws(() => Buffs.register(DEFINITION), /already registered/);
  assert.throws(() => Buffs.register({ ...DEFINITION, name: 'hangover' }), /game buff/);
  assert.throws(() => Buffs.register({ ...DEFINITION, name: 'a b' }), /name/);
  assert.throws(() => Buffs.register({ ...DEFINITION, name: 'x1', icon: 'nope' }), /icon/);
  assert.throws(() => Buffs.register({ ...DEFINITION, name: 'x2', duration: 0 }), /duration/);
  assert.throws(() => Buffs.register({ ...DEFINITION, name: 'x3', duration: NaN }), /duration/);
  assert.equal(Buffs.register({ ...DEFINITION, name: 'x4', duration: -1 }).duration, -1);
  assert.throws(
    () => Buffs.register({ ...DEFINITION, name: 'x5', effects: { stats: { bogus: 1 } } }),
    TypeError,
  );
  assert.throws(
    () => Buffs.register({ ...DEFINITION, name: 'x6', tickSeconds: 5 }),
    /onTick or effects\.tickStats/,
  );
  assert.throws(
    () => Buffs.register({ ...DEFINITION, name: 'x7', onTick: () => {} }),
    /need tickSeconds/,
  );
  assert.throws(
    () => Buffs.register({ ...DEFINITION, name: 'x8', tickSeconds: 0.1, onTick: () => {} }),
    /tickSeconds/,
  );
});

test('addBuff and removeBuff take a handle or a name, with ops and events', async () => {
  const { host, output, advance } = await hostWith(
    `kcds.core.Events.on('playerBuffAdded', (p, buff, source) => kcds.core.Chat.sendToAll('+' + buff + ':' + source));
    kcds.core.Events.on('playerBuffRemoved', (p, buff, reason) => kcds.core.Chat.sendToAll('-' + buff + ':' + reason));`,
  );
  const handle = host.api.Buffs.register(DEFINITION);
  const player = readyPlayer(host);
  assert.equal(player.hasBuff(handle), false);
  assert.equal(player.addBuff(handle), true);
  assert.equal(player.hasBuff('warmth'), true);
  advance(10);
  assert.deepEqual(player.buffs, [
    {
      name: 'warmth',
      class: 'custom',
      aiTag: null,
      duration: 30,
      custom: true,
      displayName: 'warmth',
      description: null,
      since: 10,
      source: 'server',
    },
  ]);
  assert.deepEqual(shown(output), [
    {
      op: 'customBuff',
      kind: 'show',
      id: 7,
      buff: 'warmth',
      icon: 'hangover',
      iconId: '7e252c71-5e41-472c-ad32-f223a664faab',
      label: 'warmth',
      duration: 30,
      remaining: 30,
      params: '',
    },
  ]);
  assert.equal(player.removeBuff('warmth'), true);
  assert.equal(player.hasBuff(handle), false);
  assert.deepEqual(shown(output).at(-1), { op: 'customBuff', kind: 'hide', id: 7, buff: 'warmth' });
  const chat = output.filter((op) => op.op === 'chat').map((op) => op.text);
  assert.deepEqual(chat, ['+warmth:server', '-warmth:server']);
});

test('the one schedule ends buffs in order and a repeat restarts the timer', async () => {
  const { host, advance } = await hostWith('');
  const { Buffs } = host.api;
  const short = Buffs.register({ name: 'short', icon: 'hangover', duration: 5 });
  const long = Buffs.register({ name: 'long', icon: 'hangover', duration: 20 });
  const endless = Buffs.register({ name: 'endless', icon: 'hangover', duration: -1 });
  const player = readyPlayer(host);
  player.addBuff(long);
  player.addBuff(short);
  player.addBuff(endless);
  assert.deepEqual(
    host.customBuffs.schedule.map((entry) => entry.def.handle.name),
    ['short', 'long'],
  );
  advance(4);
  player.addBuff(short);
  advance(4);
  host.customBuffs.process();
  assert.equal(player.hasBuff(short), true);
  advance(2);
  host.customBuffs.process();
  assert.equal(player.hasBuff(short), false);
  assert.equal(player.hasBuff(long), true);
  advance(20);
  host.customBuffs.process();
  assert.equal(player.hasBuff(long), false);
  assert.equal(player.hasBuff(endless), true);
  assert.equal(host.customBuffs.schedule.length, 0);
  assert.equal(host.customBuffs.timer, null);
});

test('expiry raises playerBuffRemoved with the reason expired', async () => {
  const { host, output, advance } = await hostWith(
    `kcds.core.Events.on('playerBuffRemoved', (p, buff, reason) => kcds.core.Chat.sendToAll('-' + buff + ':' + reason));`,
  );
  const handle = host.api.Buffs.register({ ...DEFINITION, duration: 3 });
  const player = readyPlayer(host);
  player.addBuff(handle);
  advance(3);
  host.customBuffs.process();
  const chat = output.filter((op) => op.op === 'chat').map((op) => op.text);
  assert.deepEqual(chat, ['-warmth:expired']);
  assert.equal(shown(output).at(-1).kind, 'hide');
});

test('ticks call onTick and apply tick stats, at most once per due moment', async () => {
  const { host, output, advance } = await hostWith(
    `kcds.players.Buffs.register({
      name: 'bleeding_out', icon: 'hangover', duration: 10, tickSeconds: 2,
      effects: { stats: { health: -1 }, tickStats: { health: -5 } },
      onTick: (player, buff) => kcds.core.Chat.sendToAll('tick:' + buff.name),
    });`,
  );
  const player = readyPlayer(host);
  player.addBuff('bleeding_out');
  const changes = () => output.filter((op) => op.op === 'addStat');
  assert.deepEqual(
    changes().map((op) => op.delta),
    [-1],
  );
  advance(2);
  host.customBuffs.process();
  // Far behind: the missed ticks are not made up for.
  advance(5);
  host.customBuffs.process();
  const ticks = output.filter((op) => op.op === 'chat' && op.text.startsWith('tick:'));
  assert.equal(ticks.length, 2);
  assert.equal(changes().length, 3);
  advance(10);
  host.customBuffs.process();
  assert.equal(player.hasBuff('bleeding_out'), false);
});

test('a throwing onTick is logged and does not stop the schedule', async () => {
  const { host, advance } = await hostWith(
    `kcds.players.Buffs.register({ name: 'bad', icon: 'hangover', duration: 10, tickSeconds: 1,
      onTick: () => { throw new Error('boom'); } });`,
  );
  const logged = [];
  host.bridge.log = (resource, text) => logged.push([resource, text]);
  const player = readyPlayer(host);
  player.addBuff('bad');
  advance(1);
  host.customBuffs.process();
  assert.equal(logged.length, 1);
  assert.equal(logged[0][0], 'custom');
  assert.equal(host.customBuffs.schedule.length, 1);
});

test('disconnect and resource stop drop the buffs; game buffs still work', async () => {
  const { host, output } = await hostWith(
    `kcds.players.Buffs.register({ name: 'warmth', icon: 'hangover', duration: 30 });`,
  );
  const player = readyPlayer(host);
  player.addBuff('warmth');
  host.disconnect(7);
  assert.equal(host.customBuffs.schedule.length, 0);
  const again = readyPlayer(host, 8);
  again.addBuff('warmth');
  host.customBuffs.resourceStopped('custom');
  assert.equal(again.hasBuff('warmth'), false);
  assert.equal(again.addBuff('warmth'), false);
  assert.equal(shown(output).at(-1).kind, 'hide');
  assert.equal(again.addBuff('hangover'), true);
});

test('setGrimeRate: a hidden copy of quest_noDirt with cds*rate, off again at 1', async () => {
  const { host, output } = await hostWith('');
  const player = readyPlayer(host);
  assert.equal(player.grimeRate, 1);
  assert.equal(player.setGrimeRate(3), true);
  assert.equal(player.grimeRate, 3);
  const [show] = shown(output);
  assert.equal(show.kind, 'show');
  assert.equal(show.buff, 'kcds.grimeRate');
  assert.equal(show.icon, 'quest_noDirt');
  assert.equal(show.duration, -1);
  assert.equal(show.params, 'cds*3');
  assert.deepEqual(player.buffs, []);
  assert.equal(player.setGrimeRate(3), true);
  assert.equal(shown(output).length, 1);
  assert.equal(player.setGrimeRate(1), true);
  assert.equal(shown(output).at(-1).kind, 'hide');
  assert.equal(player.grimeRate, 1);
  assert.throws(() => player.setGrimeRate(-1), RangeError);
  assert.throws(() => player.setGrimeRate(101), RangeError);
  assert.throws(() => player.setGrimeRate('2'), RangeError);
});

test('the clothDirtying modifier travels as the game code cds', async () => {
  const { host, output } = await hostWith('');
  const { Buffs } = host.api;
  const id = Buffs.register('tannery', {
    name: 'Tannery work',
    visual: 'hangover',
    modifiers: { clothDirtying: { operation: 'multiply', value: 2 } },
  });
  const player = readyPlayer(host);
  assert.equal(player.addBuff(id), true);
  assert.match(shown(output).at(-1).params, /cds\*2/);
});
