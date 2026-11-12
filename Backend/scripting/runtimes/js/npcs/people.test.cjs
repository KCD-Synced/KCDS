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
  assert.ok(host.api.Npc.all().some((npc) => npc.name === 'rat_konrad_hagen'));
});
