'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { ClientEntities } = require('../../../../Client/resources/entities.cjs');

// Player spawn, teleport and identity.

function fixture() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcd2s-identity-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  return { root, output, host };
}

function resource(root, name, code) {
  const dir = path.join(root, name);
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name, kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
}

function report(host, ready, extra = {}) {
  return host.updatePlayer({
    id: 7,
    nickname: 'Jindřich',
    playerIndex: 2,
    ready,
    position: { x: 10, y: 20, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    ...extra,
  });
}

const forced = (output, op) => output.filter((e) => e.op === op);

test('spawn from playerSpawning lands as the body comes into the world', async () => {
  const f = fixture();
  resource(
    f.root,
    'spawner',
    `kcds.core.Events.on('playerSpawning', (p) => {
      globalThis.accepted = p.spawn({ x: 1646.5, y: 2151, z: 35.4 }, new kcds.math.Vector3(0, 0, 90));
    });
    kcds.core.Events.on('playerSpawned', (p) => kcds.core.Chat.sendToPlayer(p, String(globalThis.accepted)));`,
  );
  await f.host.startAll();
  report(f.host, false);
  // Still loading: nothing moves yet.
  assert.equal(forced(f.output, 'entityPosition').length, 0);
  report(f.host, true);
  const positions = forced(f.output, 'entityPosition');
  assert.equal(positions.length, 1);
  assert.deepEqual(positions[0].position, { x: 1646.5, y: 2151, z: 35.4 });
  assert.equal(forced(f.output, 'entityRotation').length, 1);
  // Placed before playerSpawned is heard.
  assert.equal(f.output.at(-1).text, 'true');
  // It lands once.
  report(f.host, true);
  assert.equal(forced(f.output, 'entityPosition').length, 1);
});

test('spawn and teleport move a player in the world and refuse what is not a world position', async () => {
  const f = fixture();
  await f.host.startAll();
  const p = report(f.host, true);
  assert.equal(p.spawn({ x: 100, y: 200, z: 30 }), true);
  assert.equal(forced(f.output, 'entityPosition').length, 1);
  assert.equal(forced(f.output, 'entityRotation').length, 0);
  assert.equal(p.teleport({ x: 300, y: 400, z: 30 }, 'Mill'), true);
  assert.deepEqual(forced(f.output, 'entityPosition')[1].position, { x: 300, y: 400, z: 30 });
  for (const where of [
    { x: NaN, y: 0, z: 0 },
    { x: 1, y: 2 },
    { x: -5, y: 2, z: 3 },
    { x: 1, y: 20000, z: 3 },
    { x: 1, y: 2, z: 20000 },
    null,
    'here',
  ]) {
    assert.equal(p.spawn(where), false);
    assert.equal(p.teleport(where), false);
  }
  assert.throws(() => p.teleport({ x: 1, y: 2, z: 3 }, 42), TypeError);
  assert.throws(() => p.spawn({ x: 1, y: 2, z: 3 }, 'north'), TypeError);
  assert.equal(forced(f.output, 'entityPosition').length, 2);
  // A player no longer connected cannot be asked.
  f.host.disconnect(7);
  assert.equal(p.spawn({ x: 1, y: 2, z: 3 }), false);
  assert.equal(p.teleport({ x: 1, y: 2, z: 3 }), false);
});

test('identity: nickname, playerIndex, ready, toString; no name once gone', async () => {
  const f = fixture();
  await f.host.startAll();
  const p = report(f.host, false);
  assert.equal(p.nickname, 'Jindřich');
  assert.equal(p.playerIndex, 2);
  assert.equal(p.ready, false);
  assert.equal(String(p), 'Player(7, "Jindřich")');
  report(f.host, true);
  assert.equal(p.ready, true);
  assert.equal(f.host.api.Player.getById(7), p);
  assert.deepEqual(f.host.api.Player.all(), [p]);
  f.host.disconnect(7);
  assert.equal(p.nickname, '');
  assert.equal(String(p), 'Player(7, "")');
  assert.equal(f.host.api.Player.getById(7), null);
});

test('client Player: nickname, playerIndex, ready and toString from the native directory', () => {
  const registry = new ClientEntities();
  const api = registry.facade();
  const entry = (id, extra) => ({
    id,
    nativeId: id,
    kind: 'player',
    dimension: 0,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    ...extra,
  });
  registry.updateDirectory({
    localPlayerId: 1,
    entities: [
      entry(1, { nickname: 'Henry', playerIndex: 0, ready: true }),
      entry(2, { nickname: 'Hans', playerIndex: 1, ready: false }),
      entry(3, {}),
    ],
  });
  const local = api.LocalPlayer;
  assert.equal(local.nickname, 'Henry');
  assert.equal(local.playerIndex, 0);
  assert.equal(local.ready, true);
  assert.equal(String(local), 'Player(1, "Henry", local)');
  const other = new api.Player(2);
  assert.equal(other.nickname, 'Hans');
  assert.equal(other.ready, false);
  assert.equal(String(other), 'Player(2, "Hans")');
  // Defaults before the server sent them.
  const unnamed = new api.Player(3);
  assert.equal(unnamed.nickname, '');
  assert.equal(unnamed.playerIndex, 65535);
  assert.equal(unnamed.ready, false);
  // Gone: no name.
  registry.updateDirectory({ localPlayerId: 1, entities: [entry(1, { nickname: 'Henry' })] });
  assert.equal(other.nickname, '');
  // Malformed native entries are refused.
  for (const bad of [{ nickname: 7 }, { playerIndex: 70000 }, { ready: 'yes' }]) {
    assert.throws(
      () => registry.updateDirectory({ localPlayerId: 0, entities: [entry(4, bad)] }),
      TypeError,
    );
  }
});
