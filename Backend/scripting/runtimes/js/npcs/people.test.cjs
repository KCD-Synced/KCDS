'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const { ResourceHost } = require('../host.cjs');
const { placementsOf } = require('./people.cjs');

async function hostAt(position, settings) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-people-'));
  if (settings) {
    fs.writeFileSync(path.join(root, 'people-population.json'), JSON.stringify(settings));
  }
  const host = new ResourceHost(root, { send: () => {}, log: () => {} });
  await host.startAll();
  host.areas.level = 'trosecko';
  const player = host.updatePlayer({
    id: 7,
    nickname: 'P7',
    ready: true,
    position,
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  return { host, player, Npc: host.api.Npc };
}

const GERTA = placementsOf('trosecko').find((row) => row.name === 'ttkc_gerta');

test('the level people near a player get bodies, up to the cap, and lose them far away', async (t) => {
  const { host, player, Npc } = await hostAt(GERTA.position);
  t.after(() => host.people.stop());
  host.people.tick();
  const near = Npc.all();
  assert.ok(near.length > 0);
  assert.ok(near.length <= 48);
  assert.ok(near.some((npc) => npc.name === 'ttkc_gerta'));
  assert.ok(
    near.some((npc) => npc.routeState !== null),
    'they walk a round',
  );
  const count = near.length;
  host.people.tick();
  assert.equal(Npc.all().length, count, 'no second body');

  Npc.setPopulation({ maxPeople: 2 });
  host.updatePlayer({
    id: 7,
    nickname: 'P7',
    ready: true,
    position: { x: 10, y: 10, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  assert.equal(player.position.x, 10);
  host.people.tick();
  assert.equal(Npc.all().length, 0);
});

test('the people a quest stages are not everyday people, and a script can still adopt them', async (t) => {
  const staged = placementsOf('trosecko').find((row) => row.layer.includes('closedCastle'));
  const { host, Npc } = await hostAt(staged.position);
  t.after(() => host.people.stop());
  host.people.tick();
  assert.ok(!Npc.all().some((npc) => npc.name === staged.name));
  assert.equal(Npc.adopt(staged.guid).name, staged.name);
});

test('a storage crowd gets two bodies and a stand-in none; a script can still adopt them', async (t) => {
  // Trosky's lower courtyard: 30 people kept in a line, and a replacement guard beside them.
  const crowd = placementsOf('trosecko').filter((row) =>
    row.layer.endsWith('lowerCastle/courtyard4/_script/npc/other'),
  );
  const standIn = placementsOf('trosecko').find((row) => row.layer.endsWith('/replacementGuards'));
  assert.ok(crowd.length >= 30 && standIn);
  const { host, Npc } = await hostAt(standIn.position, { maxPeople: 200 });
  t.after(() => host.people.stop());
  host.people.tick();
  const names = new Set(Npc.all().map((npc) => npc.name));
  const bodies = crowd.filter((row) => names.has(row.name));
  assert.ok(bodies.length > 0 && bodies.length <= 10, `${bodies.length} of the courtyard's 30`);
  // No two bodies of one crowd within 2 m of each other beyond the second.
  for (const row of bodies) {
    const close = bodies.filter(
      (other) =>
        Math.hypot(other.position.x - row.position.x, other.position.y - row.position.y) < 2,
    );
    assert.ok(close.length <= 2);
  }
  assert.ok(!names.has(standIn.name));
  assert.equal(Npc.adopt(standIn.guid).name, standIn.name);
});

test('Npc.setPopulation share gives bodies to that percent of the people, the same ones each time', async (t) => {
  const { host, Npc } = await hostAt(GERTA.position, {
    maxPeople: 500,
    activationRadius: 150,
    despawnRadius: 180,
  });
  t.after(() => host.people.stop());
  host.people.tick();
  const all = new Set(Npc.all().map((npc) => npc.name));
  assert.ok(all.size >= 20, `${all.size} people near Gerta`);
  Npc.setPopulation({ share: 50 });
  const half = new Set(Npc.all().map((npc) => npc.name));
  assert.ok(
    half.size > all.size * 0.3 && half.size < all.size * 0.7,
    `${half.size} of ${all.size}`,
  );
  assert.ok(
    [...half].every((name) => all.has(name)),
    'only some of the same people, none new',
  );
  // The same half again after a round at 100 %.
  Npc.setPopulation({ share: 100 });
  assert.equal(Npc.all().length, all.size);
  Npc.setPopulation({ share: 50 });
  assert.deepEqual(new Set(Npc.all().map((npc) => npc.name)), half);
  Npc.setPopulation({ share: 0 });
  assert.equal(Npc.all().length, 0);
  assert.throws(() => Npc.setPopulation({ share: 120 }), RangeError);
  assert.throws(() => Npc.setPopulation({ share: '50' }), RangeError);
});

test('Npc.setPopulation switches them off and refuses what it cannot use', async (t) => {
  const { host, Npc } = await hostAt(GERTA.position);
  t.after(() => host.people.stop());
  host.people.tick();
  assert.ok(Npc.all().length > 0);
  Npc.setPopulation({ enabled: false });
  assert.equal(Npc.all().length, 0);
  assert.throws(() => Npc.setPopulation(null), TypeError);
  assert.throws(() => Npc.setPopulation({ enabled: 1 }), TypeError);
  assert.throws(() => Npc.setPopulation({ maxPeople: -1 }), RangeError);
  assert.throws(() => Npc.setPopulation({ activationRadius: 500 }), RangeError);
});

test('the settings file switches them off from the start', async (t) => {
  const { host, Npc } = await hostAt(GERTA.position, { enabled: false });
  t.after(() => host.people.stop());
  host.people.tick();
  assert.equal(Npc.all().length, 0);
});

test('Npc.adopt gives one placement to a script, and it stays when players leave', async (t) => {
  const { host, Npc } = await hostAt({ x: 10, y: 10, z: 0 }, { enabled: false });
  t.after(() => host.people.stop());
  const npc = Npc.adopt(GERTA.guid, { name: 'Gerta' });
  assert.equal(npc.name, 'Gerta');
  assert.equal(npc.position.x, GERTA.position.x);
  assert.equal(npc.routeState, null, 'a script has it to itself');
  assert.equal(Npc.adopt(GERTA.guid), npc, 'the same one');
  assert.equal(Npc.adopt('0x' + GERTA.guid.toUpperCase()), npc);
  assert.equal(Npc.adopt('ffffffffffffffff'), null);
  assert.throws(() => Npc.adopt(5), TypeError);
  host.people.tick();
  assert.equal(host.npcs.npcs.get(npc.id), npc);
});

test("the level's women get a woman's body, so their name and look are one person", async (t) => {
  const { host, Npc } = await hostAt(GERTA.position);
  t.after(() => host.people.stop());
  host.people.tick();
  assert.equal(GERTA.actorClass, 'NPC_Female');
  const gerta = Npc.all().find((npc) => npc.name === 'ttkc_gerta');
  assert.ok(gerta);
  assert.equal(gerta.actorClass, 'NPC_Female');
  // Every body has its placement's class.
  for (const npc of Npc.all()) {
    const row = Npc.placements().find((placement) => placement.name === npc.name);
    if (row) {
      assert.equal(npc.actorClass, row.actorClass === 'NPC_Female' ? 'NPC_Female' : 'NPC');
    }
  }
  // An adopted one keeps it too.
  assert.equal(Npc.adopt(GERTA.guid), gerta);
});

test('players cannot talk to the level people; an adopted one can be made interactable', async (t) => {
  const { host, Npc } = await hostAt(GERTA.position);
  t.after(() => host.people.stop());
  host.people.tick();
  const near = Npc.all();
  assert.ok(near.length > 0);
  assert.ok(near.every((npc) => npc.interactable === false));
  const talker = Npc.adopt(GERTA.guid);
  talker.interactable = true;
  assert.equal(talker.interactable, true);
  const other = Npc.placements().find(
    (row) => row.guid !== GERTA.guid && !Npc.all().some((npc) => npc.name === row.name),
  );
  if (other) {
    assert.equal(Npc.adopt(other.guid).interactable, false);
  }
});

test("with levelPeople on, each person names the level's own entity and walks no round of ours", async (t) => {
  process.env.KCDS_SERVER_LEVEL_PEOPLE = 'on';
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-people-'));
  const sent = [];
  const host = new ResourceHost(root, { send: (message) => sent.push(message), log: () => {} });
  delete process.env.KCDS_SERVER_LEVEL_PEOPLE;
  t.after(() => host.people.stop());
  await host.startAll();
  host.areas.level = 'trosecko';
  host.updatePlayer({
    id: 7,
    nickname: 'P7',
    ready: true,
    position: GERTA.position,
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  assert.equal(host.people.levelPeople, true);
  host.people.tick();
  const gerta = host.api.Npc.all().find((npc) => npc.name === GERTA.name);
  assert.ok(gerta);
  const named = sent.filter((message) => message.op === 'npcLevelGuid');
  assert.ok(named.some((message) => message.id === gerta.id && message.guid === GERTA.guid));
  assert.equal(named.length, host.api.Npc.all().length);
  assert.equal(gerta.routeState, null, 'the level schedule runs them, not a round of ours');
  assert.equal(gerta.interactable, false);
});

// A person the simulator reports far from its placement is not spawned there again every tick.
test('a person that walked far away is not spawned again at its placement', async (t) => {
  const { host, Npc } = await hostAt(GERTA.position);
  t.after(() => host.people.stop());
  host.people.tick();
  const gerta = Npc.all().find((npc) => npc.name === 'ttkc_gerta');
  assert.ok(gerta);
  // Its body's game reports it on its schedule, 500 m away.
  gerta.position = { x: GERTA.position.x + 500, y: GERTA.position.y, z: GERTA.position.z };
  host.people.tick();
  assert.ok(!Npc.all().some((npc) => npc.name === 'ttkc_gerta'), 'removed for being far');
  host.people.tick();
  host.people.tick();
  assert.ok(!Npc.all().some((npc) => npc.name === 'ttkc_gerta'), 'not spawned again');
});

test('people-placements.json adds people to a level the catalog has none on', async (t) => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-people-'));
  const soul = GERTA.soul;
  fs.writeFileSync(
    path.join(root, 'people-placements.json'),
    JSON.stringify({
      kcd1_rataje: [
        {
          guid: 'kcd1-442f497d17d91c00',
          name: 'rat_konrad_hagen',
          soul,
          actorClass: 'NPC_Female',
          layer: 'kcd1/merchant',
          outfit: 'f_villager_01',
          position: { x: 2565.3, y: 580.3, z: 84.8 },
          rotation: { w: 1, x: 0, y: 0, z: 0 },
        },
        { guid: 'not a placement' },
      ],
    }),
  );
  const host = new ResourceHost(root, { send: () => {}, log: () => {} });
  await host.startAll();
  t.after(() => host.people.stop());
  host.areas.level = 'kcd1_rataje';
  host.updatePlayer({
    id: 7,
    nickname: 'P7',
    ready: true,
    position: { x: 2570, y: 585, z: 85 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const rows = placementsOf('kcd1_rataje');
  assert.equal(rows.length, 1);
  assert.equal(rows[0].name, 'rat_konrad_hagen');
  host.people.tick();
  const person = host.api.Npc.all().find((npc) => npc.name === 'rat_konrad_hagen');
  assert.ok(person);
  // Dressed in its placement's outfit: the level's schedules do not dress added people.
  assert.ok(person.outfit.length > 0);
});

test('an added person with a bed lies in it at night and walks to work by day', async (t) => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-people-'));
  fs.writeFileSync(
    path.join(root, 'people-placements.json'),
    JSON.stringify({
      kcd1_rataje: [
        {
          guid: 'kcd1-4a1b2c3d4e5f6071',
          name: 'Miller',
          soul: GERTA.soul,
          actorClass: 'NPC_Female',
          interactable: true,
          position: { x: 2550, y: 460, z: 68 },
          work: { x: 2560, y: 470, z: 68 },
          bed: 'kcd1-bed-4174282e36cc0f00',
        },
      ],
    }),
  );
  fs.writeFileSync(
    path.join(root, 'level-places.json'),
    JSON.stringify({
      kcd1_rataje: [
        ['kcd1-bed-4174282e36cc0f00', 'Bed_1Place_High', 2552, 461, 68, 0, 0, -0.7071, 0.7071],
      ],
    }),
  );
  const host = new ResourceHost(root, { send: () => {}, log: () => {} });
  await host.startAll();
  t.after(() => host.people.stop());
  host.areas.level = 'kcd1_rataje';
  host.statusRoutes.configure({ ...host.statusRoutes.settings, map: 'kcd1_rataje' });
  host.updatePlayer({
    id: 7,
    nickname: 'P7',
    ready: true,
    position: { x: 2555, y: 465, z: 68 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  let hour = 23;
  host.people.night.hour = () => hour;
  host.people.tick();
  const miller = host.api.Npc.all().find((npc) => npc.name === 'Miller');
  assert.ok(miller);
  assert.equal(miller.interactable, true);
  // At night: on the way to (or in) their own bed.
  assert.equal(miller.usingObject?.object.guid, 'kcd1-bed-4174282e36cc0f00');
  // In the morning: up, and the round to work and back.
  hour = 9;
  host.people.tick();
  assert.equal(miller.usingObject, null);
  assert.ok(miller.routeState !== null);
});

test('someone who cannot reach their work place walks the neighbours round instead', async (t) => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-people-'));
  const person = (guid, x, y, work) => ({
    guid,
    name: guid,
    soul: GERTA.soul,
    actorClass: 'NPC_Female',
    position: { x, y, z: 68 },
    ...(work ? { work } : {}),
  });
  fs.writeFileSync(
    path.join(root, 'people-placements.json'),
    JSON.stringify({
      kcd1_rataje: [
        person('kcd1-0000000000000001', 2550, 460, { x: 2550, y: 520, z: 40 }),
        person('kcd1-0000000000000002', 2560, 470),
      ],
    }),
  );
  const host = new ResourceHost(root, { send: () => {}, log: () => {} });
  await host.startAll();
  t.after(() => host.people.stop());
  host.areas.level = 'kcd1_rataje';
  host.updatePlayer({
    id: 7,
    nickname: 'P7',
    ready: true,
    position: { x: 2555, y: 465, z: 68 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  let now = 1_000_000;
  t.mock.method(Date, 'now', () => now);
  const rounds = t.mock.method(host.people, 'walkRound');
  host.people.tick();
  const walker = host.api.Npc.all().find((npc) => npc.name === 'kcd1-0000000000000001');
  assert.ok(walker.routeState !== null);
  const before = rounds.mock.calls.length;
  now += 30_000;
  host.people.tick();
  // A pause is no reason to change the round.
  assert.equal(rounds.mock.calls.length, before);
  now += 61_000;
  host.people.tick();
  // Still where it stood after longer than any pause: its round without the work place.
  const fallback = rounds.mock.calls.slice(before).find((call) => call.arguments[0] === walker);
  assert.ok(fallback);
  assert.deepEqual(fallback.arguments[2], { withoutWork: true });
  // Still there after the neighbours' round too: about its own place, within a few metres of it.
  const patrols = t.mock.method(walker, 'patrol');
  now += 91_000;
  host.people.tick();
  const nearHome = rounds.mock.calls.findLast((call) => call.arguments[0] === walker);
  assert.deepEqual(nearHome.arguments[2], { withoutWork: true, nearHome: true });
  const route = patrols.mock.calls.at(-1).arguments[0];
  for (const spot of route) {
    assert.ok(Math.hypot(spot.x - 2550, spot.y - 460) <= 2.5);
  }
  // And it keeps to that.
  const walkerRounds = () =>
    rounds.mock.calls.filter((call) => call.arguments[0] === walker).length;
  const count = walkerRounds();
  now += 91_000;
  host.people.tick();
  assert.equal(walkerRounds(), count);
});

test('someone who cannot get to their bed walks their round that night instead', async (t) => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-people-'));
  fs.writeFileSync(
    path.join(root, 'people-placements.json'),
    JSON.stringify({
      kcd1_rataje: [
        {
          guid: 'kcd1-0000000000000003',
          name: 'Sleeper',
          soul: GERTA.soul,
          actorClass: 'NPC_Female',
          position: { x: 2550, y: 460, z: 68 },
          bed: 'kcd1-bed-0000000000000003',
        },
        {
          guid: 'kcd1-0000000000000004',
          name: 'Neighbour',
          soul: GERTA.soul,
          actorClass: 'NPC_Female',
          position: { x: 2560, y: 470, z: 68 },
        },
      ],
    }),
  );
  fs.writeFileSync(
    path.join(root, 'level-places.json'),
    JSON.stringify({
      kcd1_rataje: [
        ['kcd1-bed-0000000000000003', 'Bed_1Place_High', 2540, 450, 68, 0, 0, -0.7071, 0.7071],
      ],
    }),
  );
  const host = new ResourceHost(root, { send: () => {}, log: () => {} });
  await host.startAll();
  t.after(() => host.people.stop());
  host.areas.level = 'kcd1_rataje';
  host.statusRoutes.configure({ ...host.statusRoutes.settings, map: 'kcd1_rataje' });
  host.updatePlayer({
    id: 7,
    nickname: 'P7',
    ready: true,
    position: { x: 2555, y: 465, z: 68 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  let now = 2_000_000;
  t.mock.method(Date, 'now', () => now);
  host.people.night.hour = () => 23;
  host.people.tick();
  const sleeper = host.api.Npc.all().find((npc) => npc.name === 'Sleeper');
  assert.ok(sleeper.usingObject);
  // It never gets going (no body walks in a test): after longer than any pause it gives up.
  host.people.tick();
  now += 91_000;
  host.people.tick();
  assert.equal(sleeper.usingObject, null);
  assert.ok(sleeper.routeState !== null);
  // And it does not try again the same night.
  now += 5_000;
  host.people.tick();
  assert.equal(sleeper.usingObject, null);
});

test("the population share also thins people whose GUIDs are not hex (an added level's)", () => {
  const { inShare } = require('./people.cjs');
  const guids = Array.from(
    { length: 400 },
    (_, index) => `kcd1-${index.toString(16).padStart(16, '0')}`,
  );
  const kept = guids.filter((guid) => inShare(guid, 25)).length;
  assert.ok(kept > 60 && kept < 140, `kept ${kept} of 400`);
  // The same people each time.
  assert.equal(guids.filter((guid) => inShare(guid, 25)).length, kept);
});
