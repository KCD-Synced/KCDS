'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { progressionCatalog } = require('./progression/catalog.cjs');

// Progression's catalog: tracks, perks and the XP curve from the game's
// tables, on the server and in the client's resource runtime.

const SKILLS = [
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
];

test('tracks: the fifteen skills, then strength, agility, vitality, speech and prestige', () => {
  const tracks = progressionCatalog.tracks();
  assert.deepEqual(
    tracks.map((track) => track.name),
    [...SKILLS, 'strength', 'agility', 'vitality', 'speech', 'prestige'],
  );
  assert.deepEqual(tracks[0], { name: 'stealth', kind: 'skill', cap: 30, induced: false });
  assert.deepEqual(tracks[15], { name: 'strength', kind: 'stat', cap: 30, induced: false });
  // Fencing alone is derived from the weapon skills.
  assert.deepEqual(
    tracks.filter((track) => track.induced).map((track) => track.name),
    ['fencing'],
  );
  // Fresh objects: changing one changes nothing.
  tracks[0].cap = 99;
  assert.equal(progressionCatalog.tracks()[0].cap, 30);
});

test('perkNamed by name or GUID; perks filtered by tree and visibility', () => {
  const pacifist = progressionCatalog.perkNamed('Pacifist');
  assert.deepEqual(pacifist, {
    name: 'Pacifist',
    id: '2b9f3c56-d5f4-49b0-a259-8a89a67c8f0d',
    uiName: 'perk_pacifist_name',
    track: 'main',
    level: 14,
    parent: null,
    visibility: 'visible',
    kinds: ['buff'],
    autolearnable: false,
  });
  assert.deepEqual(progressionCatalog.perkNamed('2B9F3C56-D5F4-49B0-A259-8A89A67C8F0D'), pacifist);
  assert.equal(progressionCatalog.perkNamed('no such perk'), null);
  assert.throws(() => progressionCatalog.perkNamed(7), TypeError);
  const all = progressionCatalog.perks();
  assert.equal(all.length, 945);
  assert.deepEqual(
    all.map((perk) => perk.name),
    [...all.map((perk) => perk.name)].sort(),
  );
  const screen = progressionCatalog.perks({ visibleOnly: true });
  assert.ok(screen.length > 200 && screen.every((perk) => perk.visibility === 'visible'));
  const swords = progressionCatalog.perks({ track: 'weapon_sword' });
  assert.ok(swords.length > 0 && swords.every((perk) => perk.track === 'weapon_sword'));
  const main = progressionCatalog.perks({ track: 'main', visibleOnly: true });
  assert.ok(main.some((perk) => perk.name === 'Pacifist'));
  // A child perk names its parent.
  assert.ok(all.some((perk) => perk.parent !== null && progressionCatalog.perkNamed(perk.parent)));
  assert.throws(() => progressionCatalog.perks({ track: 'cooking' }), TypeError);
  assert.throws(() => progressionCatalog.perks({ visibleOnly: 'yes' }), TypeError);
});

test('xpNeeded: the game curve, base + diff * n for each level n', () => {
  // Skills: base 20, diff 30.
  assert.equal(progressionCatalog.xpNeeded('weapon_sword', 0), 0);
  assert.equal(progressionCatalog.xpNeeded('weapon_sword', 1), 50);
  assert.equal(progressionCatalog.xpNeeded('weapon_sword', 2), 50 + 80);
  assert.equal(progressionCatalog.xpNeeded('weapon_sword', 30), 30 * 20 + (30 * 30 * 31) / 2);
  // Stats have their own curves: strength 20/40, vitality 25/35.
  assert.equal(progressionCatalog.xpNeeded('strength', 1), 60);
  assert.equal(progressionCatalog.xpNeeded('vitality', 2), 60 + 95);
  assert.throws(() => progressionCatalog.xpNeeded('cooking', 1), TypeError);
  for (const level of [-1, 31, 1.5, '2']) {
    assert.throws(() => progressionCatalog.xpNeeded('stealth', level), RangeError);
  }
});

test('Progression is a global of server resources', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcd2s-progression-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'reader');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'reader', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(
    path.join(dir, 'index.js'),
    `kcds.core.Events.on('playerSpawned', (p) =>
      kcds.core.Chat.sendToPlayer(p, kcds.players.Progression.tracks().length + ' ' + kcds.players.Progression.xpNeeded('stealth', 1)));`,
  );
  await host.startAll();
  host.updatePlayer({
    id: 3,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  assert.equal(output.findLast((e) => e.op === 'chat').text, '20 50');
});

const { EntitySender } = require('./entity_sender.cjs');
const { EntityStateStore } = require('./shared_state.cjs');
const { EntityTransfer } = require('../../../../Client/resources/entity_transfer.cjs');
const { ClientEntities } = require('../../../../Client/resources/entities.cjs');

const PACIFIST = '2b9f3c56-d5f4-49b0-a259-8a89a67c8f0d';
const HAGGLE = '06940cc6-0c96-421a-ae90-1c4079f87c0f';

// A native progression report: every track at level 5, survival 19 with 50 XP and 7 points.
function nativeProgression(change = {}) {
  const tracks = progressionCatalog.tracks().map((track) => {
    if (track.name === 'survival') return [19, 50, 7];
    return [5, 0, 0];
  });
  for (const [name, value] of Object.entries(change.tracks ?? {})) {
    const index = progressionCatalog.tracks().findIndex((track) => track.name === name);
    tracks[index] = value;
  }
  return { tracks, mainPoints: change.mainPoints ?? 1, perks: change.perks ?? [PACIFIST] };
}

function hostWith(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcd2s-progression-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'watcher');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'watcher', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  return { host, output };
}

test('server Player: levels, perks, progression, trackOf and hasPerk from the report', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const p = host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  assert.equal(p.levels.survival, 0);
  assert.deepEqual(p.perks, []);
  assert.equal(p.progression, null);
  assert.equal(p.trackOf('survival'), null);
  assert.equal(host.updateProgression(99, nativeProgression()), null);
  host.updateProgression(7, nativeProgression());
  assert.equal(p.levels.survival, 19);
  assert.equal(p.levels.strength, 5);
  assert.deepEqual(p.perks, ['Pacifist']);
  assert.equal(p.hasPerk('Pacifist'), true);
  assert.equal(p.hasPerk(PACIFIST.toUpperCase()), true);
  assert.equal(p.hasPerk('haggle_generic'), false);
  assert.equal(p.hasPerk('no such perk'), false);
  assert.throws(() => p.hasPerk(3), TypeError);
  // Survival 19 -> 20 costs 20 + 30 * 20.
  assert.deepEqual(p.trackOf('survival'), { level: 19, xp: 50, xpToNext: 620, perkPoints: 7 });
  assert.throws(() => p.trackOf('cooking'), TypeError);
  const snapshot = p.progression;
  assert.deepEqual(snapshot.tracks.survival, { level: 19, xp: 50 });
  assert.equal(snapshot.perkPoints.survival, 7);
  assert.equal(snapshot.perkPoints.main, 1);
  assert.deepEqual(snapshot.perks, ['Pacifist']);
  host.disconnect(7);
  assert.equal(p.progression, null);
  assert.deepEqual(p.perks, []);
});

test('progression events: a first report is a baseline; XP, level-ups and perks after', async () => {
  const { host, output } = hostWith(
    `const log = (...args) => kcds.core.Chat.sendToAll(JSON.stringify(args));
    kcds.core.Events.on('playerXpGained', (p, track, xp) => log('xp', track, xp));
    kcds.core.Events.on('playerLevelUp', (p, track, level, points) => log('level', track, level, points));
    kcds.core.Events.on('playerPerkAdded', (p, perk, source) => log('added', perk, source));
    kcds.core.Events.on('playerPerkRemoved', (p, perk, source) => log('removed', perk, source));`,
  );
  await host.startAll();
  host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const seen = () => output.filter((e) => e.op === 'chat').map((e) => JSON.parse(e.text));
  host.updateProgression(7, nativeProgression());
  assert.deepEqual(seen(), []);
  // 25 XP on stealth; survival crosses two levels (620 + 650 XP) and lands 10 XP into 21.
  host.updateProgression(
    7,
    nativeProgression({
      tracks: { stealth: [5, 25, 0], survival: [21, 10, 9] },
      perks: [HAGGLE],
    }),
  );
  assert.deepEqual(seen(), [
    ['xp', 'stealth', 25],
    ['xp', 'survival', 620 - 50 + 650 + 10],
    ['level', 'survival', 20, 9],
    ['level', 'survival', 21, 9],
    ['added', 'haggle_generic', 'native'],
    ['removed', 'Pacifist', 'native'],
  ]);
});

test('directories: everyone sees levels and perks; only the player sees its XP and points', () => {
  const store = new EntityStateStore();
  const messages = [];
  const sender = new EntitySender({
    store,
    session: () => '0123456789abcdef0123456789abcdef',
    send: (peer, message) => messages.push({ peer, message }),
  });
  const clients = new Map();
  for (const peer of [1, 2]) {
    const entities = new ClientEntities();
    const receiver = new EntityTransfer((kind, value) => {
      if (kind === 'directory') entities.updateDirectory(value);
      else entities.applyState(value);
    });
    clients.set(peer, { entities, receiver });
  }
  const record = (id) => ({
    id,
    nativeId: id,
    kind: 'player',
    dimension: 0,
    position: { x: id, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  let time = 0;
  const drain = () => {
    for (let step = 0; step < 60; ++step) {
      sender.pump(time);
      time += 0.05;
      while (messages.length) {
        const { peer, message } = messages.shift();
        const { receiver } = clients.get(peer);
        if (message.op === 'entitySession') receiver.reset(message.session);
        else receiver.receive(message);
        sender.acknowledge(peer, { session: message.session, delivery: message.delivery });
      }
    }
  };
  sender.applyRecords({ entities: [record(1), record(2)] });
  for (const peer of [1, 2]) {
    sender.applyViewDelta({
      peer,
      localPlayerId: peer,
      added: [1, 2],
      removed: [],
      owned: [peer],
      final: true,
    });
  }
  const { progressionFromReport } = require('./progression/player_progression.cjs');
  sender.setProgression(1, progressionFromReport(nativeProgression()));
  drain();
  const own = clients.get(1).entities.facade().LocalPlayer;
  assert.equal(own.levels.survival, 19);
  assert.deepEqual(own.perks, ['Pacifist']);
  assert.deepEqual(own.trackOf('survival'), {
    level: 19,
    xp: 50,
    xpToNext: 620,
    perkPoints: 7,
  });
  const api2 = clients.get(2).entities.facade();
  const seenByOther = new api2.Player(1);
  assert.equal(seenByOther.levels.survival, 19);
  assert.equal(seenByOther.hasPerk('Pacifist'), true);
  assert.equal(seenByOther.trackOf('survival'), null);
  // A player with no report yet: zeros, no perks.
  assert.equal(api2.LocalPlayer.levels.survival, 0);
  // The progression changes: both directories follow.
  sender.setProgression(1, progressionFromReport(nativeProgression({ perks: [] })));
  drain();
  assert.deepEqual(seenByOther.perks, []);
  assert.deepEqual(own.perks, []);
});

test('server orders: what goes out, what is refused, and the perk source they set', async () => {
  const { host, output } = hostWith(
    `kcds.core.Events.on('playerPerkAdded', (p, perk, source) => kcds.core.Chat.sendToAll(perk + ':' + source));
    kcds.core.Events.on('playerPerkRemoved', (p, perk, source) => kcds.core.Chat.sendToAll('-' + perk + ':' + source));`,
  );
  await host.startAll();
  const p = host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  host.updateProgression(7, nativeProgression());
  const sent = () => output.filter((e) => e.op === 'progressionOrder');
  assert.equal(p.addXp('weapon_sword', 125.5), true);
  assert.equal(p.setLevel('strength', 10), true);
  assert.equal(p.setLevel('strength', 31), false);
  assert.equal(p.addPerk('haggle_generic'), true);
  assert.equal(p.removePerk(PACIFIST), true);
  assert.equal(p.addPerkPoints('main', 2), true);
  assert.equal(p.addPerkPoints('survival', 1), true);
  assert.equal(p.resetPerks(), true);
  assert.equal(
    p.loadProgression({
      tracks: { survival: { level: 21, xp: 10 } },
      perks: ['Pacifist'],
      perkPoints: { survival: 3, main: 1 },
    }),
    true,
  );
  const orders = sent();
  assert.deepEqual(orders[0], {
    op: 'progressionOrder',
    id: 7,
    kind: 'addXp',
    track: 8,
    xp: 125.5,
    multipliers: true,
  });
  assert.deepEqual(orders[1], {
    op: 'progressionOrder',
    id: 7,
    kind: 'setLevel',
    track: 15,
    level: 10,
  });
  assert.deepEqual(orders[2], { op: 'progressionOrder', id: 7, kind: 'addPerk', perk: HAGGLE });
  assert.deepEqual(orders[3], {
    op: 'progressionOrder',
    id: 7,
    kind: 'removePerk',
    perk: PACIFIST,
  });
  assert.deepEqual(orders[4], {
    op: 'progressionOrder',
    id: 7,
    kind: 'addPoints',
    track: 255,
    count: 2,
  });
  assert.deepEqual(orders[5], {
    op: 'progressionOrder',
    id: 7,
    kind: 'addPoints',
    track: 7,
    count: 1,
  });
  assert.deepEqual(orders[6], { op: 'progressionOrder', id: 7, kind: 'respec' });
  const restore = orders[7].snapshot;
  assert.deepEqual(restore.tracks[7], [21, 10, 3]);
  assert.deepEqual(restore.tracks[0], [0, 0, 0]);
  assert.equal(restore.mainPoints, 1);
  assert.deepEqual(restore.perks, [PACIFIST]);
  // Refused arguments throw and send nothing.
  for (const bad of [
    () => p.addXp('fencing', 10),
    () => p.addXp('cooking', 10),
    () => p.addXp('stealth', 0),
    () => p.addXp('stealth', NaN),
    () => p.addXp('stealth', 2000000),
    () => p.setLevel('fencing', 12),
    () => p.setLevel('stealth', 1.5),
    () => p.addPerk('no such perk'),
    () => p.addPerkPoints('stealth', 0),
    () => p.addPerkPoints('cooking', 1),
    () => p.loadProgression(null),
    () => p.loadProgression({ tracks: { stealth: { level: 40, xp: 0 } } }),
    () => p.loadProgression({ perks: ['no such perk'] }),
  ]) {
    assert.throws(bad);
  }
  assert.equal(sent().length, 8);
  // The report that shows the server's perk changes names the server; a native one stays native.
  host.updateProgression(7, nativeProgression({ perks: [HAGGLE] }));
  host.updateProgression(7, nativeProgression({ perks: [PACIFIST, HAGGLE] }));
  const said = output.filter((e) => e.op === 'chat').map((e) => e.text);
  assert.deepEqual(said, ['haggle_generic:server', '-Pacifist:server', 'Pacifist:native']);
  // No orders for a player still loading or gone.
  host.updatePlayer({
    id: 7,
    ready: false,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  assert.equal(p.resetPerks(), false);
  host.disconnect(7);
  assert.equal(p.resetPerks(), false);
});

test('XP from play: playerXpGaining, rates, budgets and caps decide what is granted', async () => {
  const { host, output } = hostWith(
    `kcds.core.Events.on('playerXpGaining', (p, track, xp, source) => {
      kcds.core.Chat.sendToAll(track + ':' + xp + ':' + source);
      if (source === 'PickpocketingFinished') return false;
    });`,
  );
  await host.startAll();
  let now = 0;
  host.progressionRules.clock = () => now;
  host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  host.updateProgression(7, nativeProgression());
  const grants = () => output.filter((e) => e.op === 'progressionOrder' && e.kind === 'addXp');
  const said = () => output.filter((e) => e.op === 'chat').map((e) => e.text);
  const { Progression } = host.api;
  // Granted as asked, with the player's multipliers as the game would have applied them.
  host.gateXp({ id: 7, track: 8, xp: 12, multipliers: true, source: 'Attack' });
  assert.deepEqual(grants()[0], {
    op: 'progressionOrder',
    id: 7,
    kind: 'addXp',
    track: 8,
    xp: 12,
    multipliers: true,
  });
  assert.deepEqual(said(), ['weapon_sword:12:Attack']);
  // A handler's false refuses it.
  host.gateXp({ id: 7, track: 4, xp: 5, multipliers: true, source: 'PickpocketingFinished' });
  assert.equal(grants().length, 1);
  // A rate scales it; '*' is every track, a track's own rate wins.
  Progression.setXpGain('*', 0.5);
  Progression.setXpGain('stealth', 2);
  host.gateXp({ id: 7, track: 8, xp: 12, multipliers: true, source: 'Attack' });
  host.gateXp({ id: 7, track: 0, xp: 10, multipliers: true, source: 'PotentialPlayerSneaking' });
  assert.deepEqual(
    grants()
      .slice(1)
      .map((e) => e.xp),
    [6, 20],
  );
  Progression.setXpGain('*', 1);
  Progression.setXpGain('stealth', 1);
  // The same rule as a percentage: 25 grants a quarter of the gain.
  Progression.setXpPercent('vitality', 25);
  assert.equal(Progression.xpPercent('vitality'), 25);
  assert.equal(Progression.xpPercent('stealth'), 100);
  host.gateXp({ id: 7, track: 17, xp: 8, multipliers: true, source: 'Sprint' });
  assert.equal(grants().at(-1).xp, 2);
  assert.throws(() => Progression.setXpPercent('vitality', -1), RangeError);
  assert.throws(() => Progression.setXpPercent('vitality', 'x'), RangeError);
  Progression.setXpPercent('vitality', 100);
  // A budget holds a minute's worth and refills steadily.
  Progression.setXpCap('alchemy', 60);
  host.gateXp({ id: 7, track: 4, xp: 50, multipliers: true, source: 'AlchemyFinished' });
  host.gateXp({ id: 7, track: 4, xp: 50, multipliers: true, source: 'AlchemyFinished' });
  host.gateXp({ id: 7, track: 4, xp: 50, multipliers: true, source: 'AlchemyFinished' });
  now += 30;
  host.gateXp({ id: 7, track: 4, xp: 50, multipliers: true, source: 'AlchemyFinished' });
  assert.deepEqual(
    grants()
      .slice(4)
      .map((e) => e.xp),
    [50, 10, 30],
  );
  // A cap: survival is 19 with 50 XP; capped at 20 (570 XP away), a gain a multiplier could
  // carry past it goes without multipliers, one larger than what is left gets exactly that, and
  // nothing once the cap is reached.
  Progression.setMaxLevel('survival', 20);
  host.gateXp({ id: 7, track: 7, xp: 400, multipliers: true, source: 'HerbsCollected' });
  assert.deepEqual([grants().at(-1).xp, grants().at(-1).multipliers], [400, false]);
  host.gateXp({ id: 7, track: 7, xp: 700, multipliers: true, source: 'HerbsCollected' });
  assert.deepEqual(grants().at(-1), {
    op: 'progressionOrder',
    id: 7,
    kind: 'addXp',
    track: 7,
    xp: 620 - 50,
    multipliers: false,
  });
  host.updateProgression(7, nativeProgression({ tracks: { survival: [20, 0, 8] } }));
  const before = grants().length;
  host.gateXp({ id: 7, track: 7, xp: 5, multipliers: true, source: 'HerbsCollected' });
  assert.equal(grants().length, before);
  assert.throws(() => Progression.setXpGain('cooking', 1), TypeError);
  assert.throws(() => Progression.setXpGain('*', -1), RangeError);
  assert.throws(() => Progression.setXpCap('*', NaN), RangeError);
  assert.throws(() => Progression.setMaxLevel('stealth', 31), RangeError);
});

test('setGameXp: players are told at once and on arrival; nothing is asked while it is off', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  const report = (id, ready) =>
    host.updatePlayer({
      id,
      ready,
      position: { x: 1, y: 2, z: 3 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
    });
  report(7, true);
  report(8, false);
  const native = () => output.filter((e) => e.op === 'progressionOrder' && e.kind === 'gameXp');
  const { Progression } = host.api;
  assert.equal(Progression.gameXp, true);
  Progression.setGameXp(false);
  assert.equal(Progression.gameXp, false);
  assert.deepEqual(native(), [{ op: 'progressionOrder', id: 7, kind: 'gameXp', enabled: false }]);
  // The same value again tells nobody; a player arriving is told.
  Progression.setGameXp(false);
  report(8, true);
  assert.deepEqual(
    native().map((e) => e.id),
    [7, 8],
  );
  host.gateXp({ id: 7, track: 8, xp: 12, multipliers: true, source: 'Attack' });
  assert.equal(output.filter((e) => e.kind === 'addXp').length, 0);
  Progression.setGameXp(true);
  assert.equal(native().length, 4);
  assert.throws(() => Progression.setGameXp('no'), TypeError);
});

test('the ledger rejects progression the server never granted', async () => {
  const { host, output } = hostWith(
    `kcds.core.Events.on('playerProgressionRejected', (p, reason) => kcds.core.Chat.sendToAll(reason));`,
  );
  await host.startAll();
  const p = host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  host.updateProgression(7, nativeProgression());
  const reasons = () => output.filter((e) => e.op === 'chat').map((e) => e.text);
  // Granted from play: 10 XP, with multipliers up to three times that.
  host.gateXp({ id: 7, track: 0, xp: 10, multipliers: true, source: 'PotentialPlayerSneaking' });
  host.updateProgression(7, nativeProgression({ tracks: { stealth: [5, 25, 0] } }));
  // The server's own order: strength from level 5 to 6 (260 XP).
  p.setLevel('strength', 6);
  host.updateProgression(
    7,
    nativeProgression({ tracks: { stealth: [5, 25, 0], strength: [6, 0, 1] } }),
  );
  assert.deepEqual(reasons(), []);
  // Nobody granted these: a level written by the client, and XP beyond any multiplier.
  host.updateProgression(
    7,
    nativeProgression({ tracks: { stealth: [5, 25, 0], strength: [6, 0, 1], alchemy: [9, 0, 0] } }),
  );
  host.gateXp({ id: 7, track: 13, xp: 5, multipliers: true, source: 'PowerReadingFinished' });
  host.updateProgression(
    7,
    nativeProgression({
      tracks: {
        stealth: [5, 25, 0],
        strength: [6, 0, 1],
        alchemy: [9, 0, 0],
        scholarship: [5, 40, 0],
      },
    }),
  );
  assert.deepEqual(reasons(), [
    'alchemy rose by 980 XP, more than this server granted',
    'scholarship rose by 40 XP, more than this server granted',
  ]);
});

const WANDERER = 'd8b59369-03a5-4c53-adcf-78530a00dcca';
const WARMONGER = '5ebc0272-8ffe-4fc9-b6e7-34ea88d07d26';

function readyPlayer(host, id = 7) {
  return host.updatePlayer({
    id,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
}

test('perk screen: the block list and playerPerkLearning decide; a learnt perk is "learned"', async () => {
  const { host, output } = hostWith(
    `kcds.core.Events.on('playerPerkLearning', (p, perk) => perk !== 'Warmonger');
    kcds.core.Events.on('playerPerkAdded', (p, perk, source) => kcds.core.Chat.sendToAll(perk + ':' + source));`,
  );
  await host.startAll();
  readyPlayer(host);
  host.updateProgression(7, nativeProgression());
  const { Progression } = host.api;
  Progression.lockPerks(['Flower power']);
  const orders = () =>
    output
      .filter((e) => e.op === 'progressionOrder' && e.kind !== 'lockPerks')
      .map(({ kind, perk, reason }) => (reason ? `${kind} ${perk} ${reason}` : `${kind} ${perk}`));
  host.gatePerk({ id: 7, perk: WANDERER });
  host.gatePerk({ id: 7, perk: WARMONGER });
  host.gatePerk({ id: 7, perk: '2dee44cb-616e-47f4-a1f0-82e77a198a36' });
  host.gatePerk({ id: 7, perk: HAGGLE });
  // Unknown perks and players are dropped.
  host.gatePerk({ id: 7, perk: 'not-a-perk' });
  host.gatePerk({ id: 9, perk: WANDERER });
  assert.deepEqual(orders(), [
    `learnPerk ${WANDERER}`,
    `refusePerk ${WARMONGER} script`,
    'refusePerk 2dee44cb-616e-47f4-a1f0-82e77a198a36 blocked',
    `refusePerk ${HAGGLE} hidden`,
  ]);
  // The game learnt it: a survival point spent, the perk added with the server's leave.
  host.updateProgression(
    7,
    nativeProgression({ tracks: { survival: [19, 50, 6] }, perks: [PACIFIST, WANDERER] }),
  );
  assert.deepEqual(
    output.filter((e) => e.op === 'chat').map((e) => e.text),
    ['Wanderer:learned'],
  );
});

test('lockPerks: players are told at once and on arrival; unknown perks throw', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  readyPlayer(host, 7);
  const blocked = () => output.filter((e) => e.op === 'progressionOrder' && e.kind === 'lockPerks');
  const { Progression } = host.api;
  assert.deepEqual(Progression.lockedPerks, []);
  Progression.lockPerks(['Wanderer', PACIFIST]);
  assert.deepEqual(Progression.lockedPerks, ['Pacifist', 'Wanderer']);
  assert.deepEqual(blocked(), [
    { op: 'progressionOrder', id: 7, kind: 'lockPerks', perks: [PACIFIST, WANDERER] },
  ]);
  // The same perks again tell nobody; a player arriving is told.
  Progression.lockPerks(['Wanderer']);
  readyPlayer(host, 8);
  assert.deepEqual(
    blocked().map((e) => e.id),
    [7, 8],
  );
  assert.throws(() => Progression.lockPerks(['Wanderer', 'no such perk']), TypeError);
  assert.throws(() => Progression.unlockPerks('Wanderer'), TypeError);
  assert.deepEqual(Progression.lockedPerks, ['Pacifist', 'Wanderer']);
  Progression.unlockPerks(['Pacifist', 'Wanderer']);
  assert.deepEqual(blocked().at(-1).perks, []);
  assert.equal(blocked().length, 4);
});

test('perks the server never allowed are rejected: blocked ones, perk-screen ones learnt alone', async () => {
  const { host, output } = hostWith(
    `kcds.core.Events.on('playerProgressionRejected', (p, reason) => kcds.core.Chat.sendToAll(reason));`,
  );
  await host.startAll();
  const p = readyPlayer(host);
  host.updateProgression(7, nativeProgression({ perks: [] }));
  host.api.Progression.lockPerks(['Pacifist']);
  const reasons = () => output.filter((e) => e.op === 'chat').map((e) => e.text);
  // The game granted a blocked perk by itself.
  host.updateProgression(7, nativeProgression({ perks: [PACIFIST] }));
  // A perk-screen perk that cost a survival point, learnt without asking.
  host.updateProgression(
    7,
    nativeProgression({ tracks: { survival: [19, 50, 6] }, perks: [PACIFIST, WANDERER] }),
  );
  // The game granting a perk-screen perk without a point (a reward), and the server's own orders,
  // are not rejected.
  host.updateProgression(
    7,
    nativeProgression({
      tracks: { survival: [19, 50, 6] },
      perks: [PACIFIST, WANDERER, '2dee44cb-616e-47f4-a1f0-82e77a198a36'],
    }),
  );
  p.resetPerks();
  host.updateProgression(7, nativeProgression({ tracks: { survival: [19, 50, 6] }, perks: [] }));
  p.loadProgression({ perks: ['Pacifist', 'Wanderer'] });
  host.updateProgression(
    7,
    nativeProgression({ tracks: { survival: [19, 50, 6] }, perks: [PACIFIST, WANDERER] }),
  );
  assert.deepEqual(reasons(), [
    'Pacifist is blocked on this server',
    'Wanderer was learnt without this server',
  ]);
});

test('quiet orders: { notify: false } holds back the level-up notice', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  const p = host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  host.updateProgression(7, nativeProgression());
  const sent = () => output.filter((e) => e.op === 'progressionOrder');
  assert.equal(p.setLevel('strength', 12, { notify: false }), true);
  assert.equal(p.addXp('stealth', 5, { notify: false }), true);
  assert.equal(
    p.loadProgression({ tracks: { survival: { level: 22, xp: 0 } } }, { notify: false }),
    true,
  );
  assert.equal(p.setLevel('strength', 13, { notify: true }), true);
  assert.equal(p.setLevel('strength', 14), true);
  const orders = sent();
  assert.deepEqual(
    orders.map((order) => order.notify),
    [false, false, false, undefined, undefined],
  );
  for (const bad of [
    () => p.setLevel('strength', 15, { notify: 'no' }),
    () => p.addXp('stealth', 5, 'quiet'),
    () => p.loadProgression({}, null),
  ]) {
    assert.throws(bad);
  }
  assert.equal(sent().length, 5);
});

test('lowering: setLevel, setXp and removeXp go down with the exact order, a restore can too', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  const p = host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const sent = () => output.filter((e) => e.op === 'progressionOrder');
  // Before the first report the track is unknown: setXp and removeXp send nothing.
  assert.equal(p.setXp('stealth', 10), false);
  assert.equal(p.removeXp('stealth', 10), false);
  host.updateProgression(7, nativeProgression({ tracks: { stealth: [5, 120, 0] } }));
  const stealth = progressionCatalog.tracks().findIndex((t) => t.name === 'stealth');
  const exact = (level, xp) => ({
    op: 'progressionOrder',
    id: 7,
    kind: 'setTrack',
    track: stealth,
    level,
    xp,
  });

  // Lowering a level: the exact order at the new level with no XP, and no notice option.
  assert.equal(p.setLevel('stealth', 2, { notify: true }), true);
  assert.deepEqual(sent()[0], exact(2, 0));
  // Raising stays the game's own level-up, notice option included.
  assert.equal(p.setLevel('stealth', 8, { notify: false }), true);
  assert.deepEqual(sent()[1], {
    op: 'progressionOrder',
    id: 7,
    kind: 'setLevel',
    track: stealth,
    level: 8,
    notify: false,
  });

  // setXp takes the total XP from level 0, the level following. Reaching level n
  // costs 20 + 30 * n: level 1 at 50, level 2 at 130.
  assert.equal(p.setXp('stealth', 140.004), true);
  assert.deepEqual(sent()[2], exact(2, 10));
  assert.equal(p.setXp('stealth', 50), true);
  assert.deepEqual(sent()[3], exact(1, 0));
  assert.equal(p.setXp('stealth', 0), true);
  assert.deepEqual(sent()[4], exact(0, 0));
  // Past the track's cap: nothing goes.
  assert.equal(p.setXp('stealth', 1e9), false);
  for (const bad of [
    () => p.setXp('stealth', -1),
    () => p.setXp('stealth', NaN),
    () => p.setXp('stealth', '5'),
    () => p.setXp('fencing', 5),
    () => p.setXp('cooking', 5),
  ]) {
    assert.throws(bad);
  }
  assert.equal(sent().length, 5);

  // removeXp takes XP off the reported total (level 5 with 120: 550 + 120 = 670).
  assert.equal(p.removeXp('stealth', 100), true);
  assert.deepEqual(sent()[5], exact(5, 20));
  assert.equal(p.removeXp('stealth', 100000), true);
  assert.deepEqual(sent()[6], exact(0, 0));
  assert.throws(() => p.removeXp('stealth', 0), TypeError);
  assert.throws(() => p.removeXp('stealth', 1 / 512), RangeError);
  assert.throws(() => p.removeXp('fencing', 5), TypeError);
  assert.equal(sent().length, 7);

  // A restore is raise-only unless exact; exact also holds the notice back.
  const snapshot = { tracks: { stealth: { level: 1, xp: 3 } }, perks: [], perkPoints: {} };
  assert.equal(p.loadProgression(snapshot), true);
  assert.equal(sent()[7].exact, undefined);
  assert.equal(p.loadProgression(snapshot, { exact: true }), true);
  assert.equal(sent()[8].exact, true);
  assert.equal(sent()[8].notify, false);
  assert.deepEqual(sent()[8].snapshot.tracks[stealth], [1, 3, 0]);
  assert.throws(() => p.loadProgression(snapshot, { exact: 'yes' }));
  assert.equal(sent().length, 9);
});

test('lowering: the ledger allows nothing for a lowering and the rise of a raise', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const p = host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  host.updateProgression(7, nativeProgression({ tracks: { stealth: [5, 10, 0] } }));
  const rules = host.progressionRules;
  const allowed = () => rules.allowances.get(7)?.get('stealth') ?? 0;
  p.setXp('stealth', 5);
  p.setLevel('stealth', 3);
  assert.equal(allowed(), 0);
  // Up inside the level (setXp takes the total: level 5 is 550, so 650 is 100 into
  // it): the 90 XP over the reported 10 are the server's own, and allowed.
  p.setXp('stealth', 650);
  assert.equal(allowed(), 90);
});

// a perk's name, description and lore as every player's game shows them.
test('Progression.setPerkText renames a perk and rewrites its texts for everyone', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  const { Progression } = host.api;
  const names = () => output.filter((e) => e.op === 'itemNames').map((e) => e.names);
  assert.equal(
    Progression.setPerkText('Against all odds', {
      name: "Racek's legacy",
      description: 'You fight on when others fall.',
    }),
    true,
  );
  assert.deepEqual(names().at(-1), [
    ['perk_against_all_odds_name', "Racek's legacy"],
    ['perk_against_all_odds_desc', 'You fight on when others fall.'],
  ]);
  // A player who joins later hears them with the rest.
  assert.equal(
    host.playerInventories.itemNames.get('perk_against_all_odds_name'),
    "Racek's legacy",
  );
  // null gives the game's own text back.
  assert.equal(Progression.setPerkText('Against all odds', { name: null }), true);
  assert.deepEqual(names().at(-1), [['perk_against_all_odds_name', '']]);
  assert.equal(host.playerInventories.itemNames.has('perk_against_all_odds_name'), false);
  assert.throws(() => Progression.setPerkText('no such perk', { name: 'x' }), TypeError);
  assert.throws(() => Progression.setPerkText('Against all odds', { name: '' }), TypeError);
  assert.throws(() => Progression.setPerkText('Against all odds', null), TypeError);
});
