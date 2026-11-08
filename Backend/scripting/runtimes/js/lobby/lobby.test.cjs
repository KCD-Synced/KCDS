'use strict';

// A player held out of the world (Player.setLobby) and let in again.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { ResourceHost } = require('../host.cjs');
const { LOBBY_WORLD_BASE } = require('./lobby.cjs');

async function hostWithPlayer() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-lobby-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  await host.startAll();
  const report = (extra) =>
    host.updatePlayer({
      id: 7,
      nickname: 'P7',
      ready: true,
      gameplayReady: true,
      position: { x: 100, y: 200, z: 30 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
      ...extra,
    });
  report();
  return { host, output, report, player: host.players.get(7) };
}

test('a held player is alone, frozen and out of voice; let in, all comes back', async () => {
  const { host, output, player } = await hostWithPlayer();
  const { Voice } = host.api;
  const changes = [];
  host.on('playerLobbyChanged', (who, held) => changes.push([who.id, held]));
  player.setDimension(3);
  host.confirmPlayerWorld({ id: 7, request: host.nextWorldRequest, dimension: 3 });
  Voice.setPlayerMuted(player, true);

  assert.equal(player.setLobby(true), true);
  assert.equal(player.inLobby, true);
  assert.equal(player.setLobby(true), false, 'held already');
  assert.equal(player.frozen, true);
  assert.equal(Voice.isPlayerMuted(player), true);
  assert.equal(Voice.isPlayerDeaf(player), true);
  const world = output.filter((e) => e.op === 'entityWorld').at(-1);
  assert.equal(world.dimension, LOBBY_WORLD_BASE + (player.playerIndex % 0xffff));
  assert.deepEqual(output.filter((e) => e.op === 'playerLobby').at(-1), {
    op: 'playerLobby',
    id: 7,
    lobby: true,
  });

  assert.equal(player.setLobby(false), true);
  assert.equal(player.inLobby, false);
  assert.equal(player.frozen, false);
  assert.equal(Voice.isPlayerMuted(player), true, 'muted by the script before: still muted');
  assert.equal(Voice.isPlayerDeaf(player), false);
  assert.equal(output.filter((e) => e.op === 'entityWorld').at(-1).dimension, 3);
  assert.deepEqual(output.filter((e) => e.op === 'playerLobby').at(-1).lobby, false);
  assert.deepEqual(changes, [
    [7, true],
    [7, false],
  ]);
  assert.throws(() => player.setLobby('yes'), TypeError);
});

test('a held player whose world loads again hears it is held', async () => {
  const { host, output, report, player } = await hostWithPlayer();
  player.setLobby(true);
  const before = output.filter((e) => e.op === 'playerLobby').length;
  report({ ready: false });
  report({ ready: true });
  assert.equal(output.filter((e) => e.op === 'playerLobby').length, before + 1);
  host.disconnect(7);
  assert.equal(host.lobby.held.size, 0, 'nothing kept of a player who left');
});
