'use strict';

// Bans for server scripts: player.ban and the Bans global, with a reason and no end; the same
// list KCDS-Admin keeps (bans.json).

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');

async function hostWith(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-bans-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const sent = [];
  const host = new ResourceHost(root, { send: (message) => sent.push(message), log: () => {} });
  // The licence is checked before the bans; these players have one.
  host.licence.check = async () => ({ allowed: true, reason: '', pass: null });
  t.after(() => host.shutdown());
  await host.startAll();
  return { host, root, sent, api: host.api };
}

function joined(host, id, identity) {
  host.playerIdentities.set(id, Object.freeze(identity));
  return host.updatePlayer({
    id,
    nickname: identity.nickname,
    playerIndex: id,
    ready: true,
    dimension: 0,
    position: { x: 10, y: 20, z: 30 },
    rotation: { x: 0, y: 0, z: 0, w: 1 },
  });
}

const IDENTITY = {
  nickname: 'Hans',
  steamId: '',
  store: '',
  storeAccount: '',
  discordId: '1563839669496578088',
  deviceId: 'cd'.repeat(32),
  ip: '10.0.0.5',
};

test('player.ban bans with a reason and no end, sends the player away and refuses them', async (t) => {
  const { host, root, sent, api } = await hostWith(t);
  const player = joined(host, 5, IDENTITY);
  const ban = player.ban('Cheating');
  assert.deepEqual(ban.identifiers, ['discord:1563839669496578088', `hardware:${'cd'.repeat(32)}`]);
  assert.equal(ban.reason, 'Cheating');
  assert.equal(ban.by, 'script');
  assert.equal(ban.until, 0);
  // They leave at once with the ban's text, without "kicked".
  const leave = sent.find((entry) => entry.op === 'kick' && entry.id === 5);
  assert.equal(leave.kind, 'ban');
  assert.equal(leave.reason, 'You are banned from this server: Cheating.');
  assert.ok(fs.existsSync(path.join(root, 'bans.json')));
  // A new account on the same machine is refused.
  const again = await host.admitConnection(41, { ...IDENTITY, discordId: '', nickname: 'Hans2' });
  assert.equal(again.allowed, false);
  assert.equal(again.reason, 'You are banned from this server: Cheating.');
  assert.equal(api.Bans.find(`hardware:${'cd'.repeat(32)}`).id, ban.id);
  assert.equal(api.Bans.list().length, 1);
  // Lifted by its id: let in again.
  assert.equal(api.Bans.remove(ban.id), true);
  assert.equal(api.Bans.find(`hardware:${'cd'.repeat(32)}`), null);
  assert.deepEqual(await host.admitConnection(42, IDENTITY), { allowed: true, reason: '' });
});

test('Bans.add bans identifiers of a player who is not there; remove lifts by identifier', async (t) => {
  const { host, sent, api } = await hostWith(t);
  const ban = api.Bans.add(['steam:76561198000000001', 'discord:42'], 'Ban evasion');
  assert.equal(ban.reason, 'Ban evasion');
  assert.equal(api.Bans.find('discord:42').id, ban.id);
  // A player any of them names leaves at once.
  joined(host, 6, { ...IDENTITY, discordId: '7' });
  api.Bans.add('discord:7');
  const leave = sent.find((entry) => entry.op === 'kick' && entry.id === 6);
  assert.equal(leave.reason, 'You are banned from this server: Banned from this server.');
  // Every ban naming the identifier goes.
  assert.equal(api.Bans.remove('discord:42'), true);
  assert.equal(api.Bans.remove('discord:42'), false);
  assert.equal(api.Bans.list().length, 1);
  assert.throws(() => api.Bans.add([]), TypeError);
  assert.throws(() => api.Bans.add(['nonsense']), TypeError);
  assert.throws(() => api.Bans.add('steam:1', 'x'.repeat(301)), TypeError);
  assert.throws(() => api.Bans.remove(''), TypeError);
  assert.throws(() => api.Bans.find('nonsense'), TypeError);
});
