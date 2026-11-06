'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { lookupBuff } = require('./buffs/catalog.cjs');
const { buffStates, isKnockOutBuff } = require('./buffs/player_buffs.cjs');

const MARIGOLD = '8503216a-a34c-49f0-aefa-54d4502046f9';
const FOOD_POISON = '04218e40-e756-476b-914e-03d67a24e733';
const HANGOVER = '7e252c71-5e41-472c-ad32-f223a664faab';

function hostWith(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcd2s-buffs-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'watcher');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'watcher', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  let now = 100;
  host.buffClock = () => now;
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

test('Buffs: find by name or GUID, the claimable classes and the families', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const { Buffs } = host.api;
  assert.deepEqual(Buffs.find('hangover'), {
    name: 'hangover',
    class: 'hangover',
    aiTag: 'alcohol_hangover',
    duration: 6.6667,
    custom: false,
    displayName: null,
    description: null,
  });
  assert.deepEqual(Buffs.find(MARIGOLD.toUpperCase()), {
    name: 'potion_marigold_decoction',
    class: 'potion',
    aiTag: null,
    duration: 60,
    custom: false,
    displayName: null,
    description: null,
  });
  assert.equal(Buffs.find('Hangover'), null);
  assert.throws(() => Buffs.find(3), TypeError);
  assert.equal(Buffs.classes.length, 15);
  assert.ok(Buffs.classes.includes('alcohol') && !Buffs.classes.includes('perk'));
  assert.equal(Buffs.tags.length, 23);
  // The stats a custom buff's modifiers may name, a fresh array each time.
  assert.deepEqual(
    [...Buffs.modifiers],
    [
      'health',
      'stamina',
      'exhaust',
      'hunger',
      'healthRegeneration',
      'hungerDrain',
      'energyDrain',
      'maxStamina',
      'carryCapacity',
      'staminaRegeneration',
      'charisma',
      'strength',
      'agility',
      'vitality',
      'speech',
      'runSpeed',
      'attackSpeed',
      'noise',
      'visibility',
      'conspicuousness',
      'clothDirtying',
      'stealth',
      'horse_riding',
      'fencing',
      'thievery',
      'alchemy',
      'craftsmanship',
      'drinking',
      'survival',
      'weapon_sword',
      'heavy_weapons',
      'marksmanship',
      'weapon_large',
      'weapon_unarmed',
      'scholarship',
      'houndmaster',
      'noSprint',
    ],
  );
  Buffs.modifiers.push('luck');
  assert.equal(Buffs.modifiers.length, 37);
  assert.ok(
    ['poison', 'bleed', 'alcohol_drunk', 'unconscious'].every((t) => Buffs.tags.includes(t)),
  );
});

test('reports: Player.buffs, hasBuff, playerBuffAdded for everything at the first report', async () => {
  const { host, output, advance } = hostWith(
    `kcds.core.Events.on('playerBuffAdded', (p, buff, source) => kcds.core.Chat.sendToAll('+' + buff + ':' + source));
    kcds.core.Events.on('playerBuffRemoved', (p, buff, reason) => kcds.core.Chat.sendToAll('-' + buff + ':' + reason));`,
  );
  await host.startAll();
  const p = readyPlayer(host);
  const events = () => output.filter((e) => e.op === 'chat').map((e) => e.text);
  assert.deepEqual(p.buffs, []);
  assert.equal(p.hasBuff('hangover'), false);
  host.updateBuffs(7, [[HANGOVER, '4', false]]);
  advance(2.5);
  host.updateBuffs(7, [
    [HANGOVER, '4', false],
    [MARIGOLD, '9', true],
    ['00000000-0000-0000-0000-000000000000', '10', false],
  ]);
  assert.deepEqual(events(), ['+hangover:native', '+potion_marigold_decoction:server']);
  advance(1);
  assert.deepEqual(p.buffs, [
    {
      name: 'hangover',
      class: 'hangover',
      aiTag: 'alcohol_hangover',
      duration: 6.6667,
      custom: false,
      displayName: null,
      description: null,
      since: 3.5,
      source: 'native',
    },
    {
      name: 'potion_marigold_decoction',
      class: 'potion',
      aiTag: null,
      duration: 60,
      custom: false,
      displayName: null,
      description: null,
      since: 1,
      source: 'server',
    },
  ]);
  assert.equal(p.hasBuff('hangover'), true);
  assert.equal(p.hasBuff(MARIGOLD), true);
  assert.equal(p.hasBuff('no such buff'), false);
  assert.throws(() => p.hasBuff(null), TypeError);
  // The hangover ran out.
  host.updateBuffs(7, [[MARIGOLD, '9', true]]);
  assert.deepEqual(events().at(-1), '-hangover:expired');
  assert.throws(() => host.updateBuffs(7, [[MARIGOLD, '-1', true]]), TypeError);
});

test('orders: addBuff, removeBuff with the instances reported, clearBuffs by family', async () => {
  const { host, output } = hostWith(
    `kcds.core.Events.on('playerBuffRemoved', (p, buff, reason) => kcds.core.Chat.sendToAll(buff + ':' + reason));`,
  );
  await host.startAll();
  const p = readyPlayer(host);
  host.updateBuffs(7, [
    [MARIGOLD, '3', false],
    [MARIGOLD, '5', false],
    [FOOD_POISON, '6', false],
  ]);
  const orders = () => output.filter((e) => e.op === 'buffOrder');
  assert.equal(p.addBuff('potion_marigold_decoction'), true);
  assert.equal(p.addBuff('no such buff'), false);
  assert.equal(p.removeBuff(MARIGOLD), true);
  assert.equal(p.removeBuff('hangover'), true);
  assert.equal(p.clearBuffs('poison'), true);
  assert.equal(p.clearBuffs('not a family'), false);
  assert.deepEqual(orders(), [
    { op: 'buffOrder', id: 7, kind: 'add', buff: MARIGOLD },
    { op: 'buffOrder', id: 7, kind: 'remove', buff: MARIGOLD, instances: ['3', '5'] },
    { op: 'buffOrder', id: 7, kind: 'remove', buff: HANGOVER, instances: [] },
    { op: 'buffOrder', id: 7, kind: 'clear', tag: 3 },
  ]);
  host.updateBuffs(7, []);
  assert.deepEqual(
    output.filter((e) => e.op === 'chat').map((e) => e.text),
    ['potion_marigold_decoction:server', 'potion_marigold_decoction:server', 'food_poison:server'],
  );
  // A player still loading has no world to apply them in.
  const loading = host.updatePlayer({
    id: 8,
    ready: false,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  assert.equal(loading.addBuff('hangover'), false);
});

test('Buffs.claim: every client at once and on arrival; playerBuffBlocked twice a second', async () => {
  const { host, output, advance } = hostWith(
    `kcds.core.Events.on('playerBuffBlocked', (p, buff) => kcds.core.Chat.sendToAll(buff));`,
  );
  await host.startAll();
  readyPlayer(host, 7);
  const { Buffs } = host.api;
  const claims = () => output.filter((e) => e.op === 'buffOrder' && e.kind === 'claim');
  assert.deepEqual(Buffs.claim(['alcohol', 'potion']), ['alcohol', 'potion']);
  assert.deepEqual(claims(), [{ op: 'buffOrder', id: 7, kind: 'claim', classes: [10, 13] }]);
  assert.throws(() => Buffs.claim(['perk']), TypeError);
  assert.throws(() => Buffs.claim('alcohol'), TypeError);
  readyPlayer(host, 8);
  assert.deepEqual(
    claims().map((e) => e.id),
    [7, 8],
  );
  for (let i = 0; i < 4; ++i) host.buffBlocked({ id: 7, buff: MARIGOLD });
  host.buffBlocked({ id: 7, buff: 'not-a-buff' });
  advance(1.5);
  host.buffBlocked({ id: 7, buff: MARIGOLD });
  assert.deepEqual(
    output.filter((e) => e.op === 'chat').map((e) => e.text),
    ['potion_marigold_decoction', 'potion_marigold_decoction', 'potion_marigold_decoction'],
  );
  assert.deepEqual(Buffs.claim([]), []);
  assert.deepEqual(claims().at(-1).classes, []);
});

test('a disconnect clears the buffs before playerDisconnect runs', async () => {
  const { host, output } = hostWith(
    `kcds.core.Events.on('playerDisconnect', (p) => kcds.core.Chat.sendToAll(String(p.buffs.length)));`,
  );
  await host.startAll();
  readyPlayer(host);
  host.updateBuffs(7, [[MARIGOLD, '3', false]]);
  host.disconnect(7);
  assert.deepEqual(
    output.filter((e) => e.op === 'chat').map((e) => e.text),
    ['0'],
  );
});

test("the native client's status classes are the catalog's claimable classes", () => {
  const header = fs.readFileSync(
    path.join(__dirname, '../../../integration/include/kcd2s/integration/buffs.hpp'),
    'utf8',
  );
  const list = /kStatusBuffClasses = \{([^}]*)\}/.exec(header)[1];
  const native = list
    .split(',')
    .map((id) => Number(id.trim()))
    .sort((a, b) => a - b);
  const catalog = require('./buffs/catalog.json');
  const claimable = catalog.classes.filter((entry) => entry.claimable).map((entry) => entry.id);
  assert.deepEqual(native, claimable);
});

test('heal: the poisons and injuries its cures wipe are removed by the server', async () => {
  const { host, output } = hostWith(
    `kcds.core.Events.on('playerBuffRemoved', (p, buff, reason) => kcds.core.Chat.sendToAll(buff + ':' + reason));`,
  );
  await host.startAll();
  const p = readyPlayer(host);
  host.updateBuffs(7, [
    ['58138b68-0c86-4d5e-8823-417106d08d3c', '3', false],
    [HANGOVER, '4', false],
  ]);
  assert.equal(p.heal({ health: false }), true);
  host.updateBuffs(7, [[HANGOVER, '4', false]]);
  host.updateBuffs(7, []);
  assert.deepEqual(
    output.filter((e) => e.op === 'chat').map((e) => e.text),
    ['potion_bane:server', 'hangover:expired'],
  );
});

test('a knock-out is not a buff: not listed, and not added by a script', async (t) => {
  const unconscious = lookupBuff('unconscious_permanent');
  assert.equal(isKnockOutBuff(unconscious), true);
  assert.equal(isKnockOutBuff(lookupBuff('hangover')), false);
  const kept = [{ buff: unconscious, instance: 1, server: false, since: 0 }];
  assert.deepEqual(buffStates(kept, 1), []);
});
