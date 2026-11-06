'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { ClientEntities } = require('../../../../Client/resources/entities.cjs');
const {
  NO_POSTURE,
  postureFromNative,
  directoryPosture,
  postureFromDirectory,
} = require('./posture/player_posture.cjs');

// How a player's body stands: inAir, crouched and physicsProfile on
// the server and every client; movePace, moveDirection and bodyStance on a client.

function host() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcd2s-posture-'));
  return new ResourceHost(root, { send: () => {}, log: () => {} });
}

function report(server, extra = {}) {
  return server.updatePlayer({
    id: 7,
    nickname: 'Henry',
    playerIndex: 0,
    ready: true,
    position: { x: 10, y: 20, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    ...extra,
  });
}

function entry(id, extra = {}) {
  return {
    id,
    nativeId: id,
    kind: 'player',
    dimension: 0,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    ...extra,
  };
}

test('the directory number: bit 0 in the air, bit 1 crouched, guard, fists up, the profile', () => {
  for (const inAir of [false, true]) {
    for (const crouched of [false, true]) {
      for (const [guard, guardUp] of [
        [0, false],
        [1, true],
      ]) {
        for (const physicsProfile of [0, 1, 2, 6, 255]) {
          for (const fightZone of [-1, 0, 5, 15, 254]) {
            const posture = { inAir, crouched, guard, guardUp, physicsProfile, fightZone };
            assert.deepEqual({ ...postureFromDirectory(directoryPosture(posture)) }, posture);
          }
        }
      }
    }
  }
  assert.equal(postureFromDirectory(undefined), NO_POSTURE);
  for (const bad of [-1, 16, 0x1000000, 1.5, '1', null]) {
    assert.throws(() => postureFromDirectory(bad), TypeError);
  }
  // A native report without a posture, or with a profile out of range, reads the defaults.
  assert.deepEqual({ ...postureFromNative({}) }, { ...NO_POSTURE });
  assert.equal(postureFromNative({ physicsProfile: 300 }).physicsProfile, 255);
  assert.equal(postureFromNative({ fightZone: 255 }).fightZone, -1);
});

test('server Player: the posture from the report, the defaults without one', async () => {
  const server = host();
  await server.startAll();
  const player = report(server);
  assert.equal(player.inAir, false);
  assert.equal(player.crouched, false);
  assert.equal(player.physicsProfile, 255);
  report(server, { inAir: true, crouched: true, physicsProfile: 1 });
  assert.equal(player.inAir, true);
  assert.equal(player.crouched, true);
  assert.equal(player.physicsProfile, 1);
  report(server, { physicsProfile: 2 });
  assert.equal(player.inAir, false);
  assert.equal(player.physicsProfile, 2);
  // The guard and the raised weapon.
  assert.deepEqual([player.guard, player.guardUp], [0, false]);
  report(server, { guard: true, guardUp: true });
  assert.deepEqual([player.guard, player.guardUp], [1, true]);
  // The combat-star direction they aim at, -1 for none.
  assert.equal(player.fightZone, -1);
  report(server, { guardUp: true, fightZone: 5 });
  assert.equal(player.fightZone, 5);
});

test('client Player: the posture from the directory, a malformed one refused', () => {
  const registry = new ClientEntities();
  const api = registry.facade();
  registry.updateDirectory({
    localPlayerId: 1,
    entities: [
      entry(1, { posture: directoryPosture({ inAir: true, crouched: false, physicsProfile: 1 }) }),
      entry(2),
    ],
  });
  assert.equal(api.LocalPlayer.inAir, true);
  assert.equal(api.LocalPlayer.crouched, false);
  assert.equal(api.LocalPlayer.physicsProfile, 1);
  const other = new api.Player(2);
  assert.equal(other.inAir, false);
  assert.equal(other.physicsProfile, 255);
  assert.deepEqual([other.guard, other.guardUp], [0, false]);
  assert.throws(
    () => registry.updateDirectory({ localPlayerId: 0, entities: [entry(3, { posture: 16 })] }),
    TypeError,
  );
});

test("client Player: this machine's Mannequin tags, 255 for none, a malformed message dropped", () => {
  const registry = new ClientEntities();
  const api = registry.facade();
  registry.updateDirectory({ localPlayerId: 1, entities: [entry(1), entry(2)] });
  const local = api.LocalPlayer;
  const other = new api.Player(2);
  assert.deepEqual([local.movePace, local.moveDirection, local.bodyStance], [255, 255, 255]);
  registry.updateBodyTags({
    tags: [
      [1, 0, 4, 255],
      [2, 255, 255, 4],
    ],
  });
  assert.deepEqual([local.movePace, local.moveDirection, local.bodyStance], [0, 4, 255]);
  assert.equal(other.bodyStance, 4);
  for (const bad of [
    { tags: [[2, 0, 0]] },
    { tags: [[2, 0, 0, 256]] },
    { tags: [[0, 0, 0, 0]] },
    {},
  ]) {
    registry.updateBodyTags(bad);
    assert.equal(other.bodyStance, 4);
  }
  registry.disconnect();
  assert.equal(registry.bodyTags.size, 0);
});

test("the server's bodyStance comes from the player's own game, with playerStanceChanged", () => {
  const server = host();
  const seen = [];
  server.on('playerStanceChanged', (player, stance, previous) => {
    seen.push([player.id, stance, previous]);
  });
  report(server, { stance: 3 });
  const player = server.players.get(7);
  // The first report after joining sets it without an event.
  assert.equal(player.bodyStance, 3);
  assert.deepEqual(seen, []);
  report(server, { stance: 3 });
  report(server, { stance: 12 });
  report(server, {});
  assert.equal(player.bodyStance, 255);
  assert.deepEqual(seen, [
    [7, 12, 3],
    [7, 255, 12],
  ]);
});

test('playerTeleported comes once the player own game reports the body at the target', () => {
  const server = host();
  const arrived = [];
  server.on('playerTeleported', (player, position) => arrived.push([player.id, position.x]));
  report(server);
  const player = server.players.get(7);
  assert.equal(player.teleport({ x: 500, y: 20, z: 3 }), true);
  // Asked at once: the target.
  assert.equal(player.position.x, 500);
  // The body still at the old place in the next report: no event, the real place.
  report(server);
  assert.equal(player.position.x, 10);
  assert.deepEqual(arrived, []);
  // There now (within 3 m): once.
  report(server, { position: { x: 501.5, y: 20, z: 3 } });
  report(server, { position: { x: 501.5, y: 20, z: 3 } });
  assert.deepEqual(arrived, [[7, 501.5]]);
});
