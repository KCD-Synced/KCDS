'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');

// Faction, Npc.aggroRange, Npc.hostile and Npc.hostileTo (npcs/factions.cjs): who an NPC attacks
// on sight and whom it helps.

async function startedHost() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-factions-'));
  const output = [];
  const host = new ResourceHost(root, {
    entityIds: { first: 4097, count: 2048 },
    send: (message) => output.push(message),
    log: () => {},
  });
  await host.startAll();
  const attacks = () =>
    output
      .filter((message) => message.op === 'npcIntent' && message.intent.includes('|attack|'))
      .map((message) => [message.id, Number(message.intent.split('|')[2])]);
  // One look around, as the timer does it.
  const look = () => host.npcs.factions.scan();
  return {
    host,
    Npc: host.npcs.Npc,
    Faction: host.npcs.factions.define(),
    attacks,
    look,
  };
}

function player(host, id, position) {
  return host.updatePlayer({
    id,
    nickname: `P${id}`,
    ready: true,
    position,
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    vitals: {
      health: 100,
      stamina: 100,
      exhaust: 100,
      hunger: 100,
      bleeding: 0,
      sleeping: 0,
      consciousness: 1,
      drunkenness: 0,
      poisoning: 0,
    },
  });
}

test('relations are both ways, a faction is its own friend and 0 is nobody', async () => {
  const { Faction } = await startedHost();
  assert.equal(Faction.relation(1, 2), 'neutral');
  Faction.setRelation(1, 2, 'hostile');
  Faction.setRelation(3, 1, 'friendly');
  assert.equal(Faction.relation(2, 1), 'hostile');
  assert.equal(Faction.relation(1, 3), 'friendly');
  assert.equal(Faction.relation(4, 4), 'friendly');
  assert.equal(Faction.relation(0, 2), 'neutral');
  assert.deepEqual(Faction.relations(), [
    { a: 1, b: 2, relation: 'hostile' },
    { a: 1, b: 3, relation: 'friendly' },
  ]);
  Faction.setRelation(2, 1, 'neutral');
  assert.equal(Faction.relation(1, 2), 'neutral');
  assert.equal(Faction.relations().length, 1);
  assert.throws(() => Faction.setRelation(1, 1, 'hostile'), Error);
  assert.throws(() => Faction.setRelation(0, 1, 'hostile'), RangeError);
  assert.throws(() => Faction.setRelation(1, 70000, 'hostile'), RangeError);
  assert.throws(() => Faction.setRelation(1, 2, 'angry'), RangeError);
});

test('an NPC with an aggro range attacks the nearest player of a hostile faction', async () => {
  const { host, Npc, Faction, attacks, look } = await startedHost();
  const bandit = Npc.create({ position: { x: 0, y: 0, z: 0 }, faction: 7 });
  const near = player(host, 1, { x: 8, y: 0, z: 0 });
  const nearer = player(host, 2, { x: 5, y: 0, z: 0 });
  const stranger = player(host, 3, { x: 2, y: 0, z: 0 });
  near.faction = 1;
  nearer.faction = 1;
  Faction.setRelation(7, 1, 'hostile');
  look();
  assert.deepEqual(attacks(), [], 'no aggro range: it looks for nobody');
  bandit.aggroRange = 10;
  assert.equal(bandit.isHostileTo(nearer), true);
  assert.equal(bandit.isHostileTo(stranger), false);
  look();
  assert.deepEqual(attacks(), [[bandit.id, 2]]);
  // Fighting already: it does not turn on another.
  look();
  assert.equal(attacks().length, 1);
  assert.throws(() => {
    bandit.aggroRange = 51;
  }, RangeError);
  assert.throws(() => {
    near.faction = -1;
  }, RangeError);
});

test('out of range or a refusing npcAggro: no attack', async () => {
  const { host, Npc, attacks, look } = await startedHost();
  const guard = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  guard.aggroRange = 10;
  guard.hostile = true;
  player(host, 1, { x: 15, y: 0, z: 0 });
  look();
  assert.deepEqual(attacks(), []);
  const seen = [];
  host.on('npcAggro', (npc, target) => {
    seen.push(target.id);
    return false;
  });
  player(host, 2, { x: 3, y: 0, z: 0 });
  look();
  look();
  assert.deepEqual(seen, [2], 'a refused attack is not offered again at once');
  assert.deepEqual(attacks(), []);
});

test('hostile makes every player a foe; hostileTo names one body', async () => {
  const { host, Npc, attacks, look } = await startedHost();
  const dog = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  const cat = Npc.create({ position: { x: 4, y: 0, z: 0 } });
  dog.aggroRange = 20;
  const someone = player(host, 1, { x: 6, y: 0, z: 0 });
  assert.equal(dog.isHostileTo(someone), false);
  assert.equal(dog.hostileTo(cat), true);
  assert.equal(dog.hostileTo(dog), false);
  look();
  assert.deepEqual(attacks(), [[dog.id, cat.id]]);
  dog.hostileTo(cat, false);
  assert.equal(dog.isHostileTo(cat), false);
  dog.hostile = true;
  assert.equal(dog.isHostileTo(someone), true);
});

test('struck in a fight, an NPC is helped by friends whose aggro range reaches it', async () => {
  const { host, Npc, Faction, attacks } = await startedHost();
  Faction.setRelation(1, 2, 'friendly');
  const victim = Npc.create({ position: { x: 0, y: 0, z: 0 }, faction: 1, fightsBack: false });
  const friend = Npc.create({ position: { x: 5, y: 0, z: 0 }, faction: 2 });
  const brother = Npc.create({ position: { x: 3, y: 0, z: 0 }, faction: 1 });
  const faraway = Npc.create({ position: { x: 40, y: 0, z: 0 }, faction: 1 });
  const stranger = Npc.create({ position: { x: 2, y: 0, z: 0 }, faction: 9 });
  for (const npc of [friend, brother, faraway, stranger]) {
    npc.aggroRange = 10;
  }
  player(host, 1, { x: 1, y: 0, z: 0 });
  host.npcs.hit({ id: victim.id, attackerId: 1, amount: 10, reason: 1, subpart: -1 });
  assert.deepEqual(
    attacks().sort(),
    [
      [friend.id, 1],
      [brother.id, 1],
    ].sort(),
  );
});

test("a stopped resource's relations end; a removed NPC watches no more", async () => {
  const { host, Npc, Faction } = await startedHost();
  host.npcs.factions.setRelation(1, 2, 'hostile');
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  npc.aggroRange = 5;
  assert.notEqual(host.npcs.factions.timer, null);
  npc.remove();
  assert.equal(host.npcs.factions.watchers.size, 0);
  assert.equal(host.npcs.factions.timer, null);
  // Set outside any resource: no resource owns it, so none stopping removes it.
  host.npcs.factions.resourceStopped('someone');
  assert.equal(Faction.relation(1, 2), 'hostile');
});
