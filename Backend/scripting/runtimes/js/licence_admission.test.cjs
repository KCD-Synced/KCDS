'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const { ResourceHost } = require('./host.cjs');
const { LicenceGate } = require('./licence/licence_gate.cjs');
const { ServerPassword } = require('./admission/server_password.cjs');
const { ServerBans } = require('./admission/server_bans.cjs');

const NOW = 1_800_000_000_000;
const BINDING = '0123456789abcdef';
const STEAM_ID = '76561198000000001';

function fixture() {
  const { privateKey, publicKey } = crypto.generateKeyPairSync('ed25519');
  const key = Buffer.from(publicKey.export({ format: 'jwk' }).x, 'base64url').toString('base64');
  const observations = [];
  const host = {
    bridge: { log: () => {}, send: () => {} },
    admittedIdentities: new Map(),
    // No server password: the licence alone decides here (the password: server_password.test.cjs).
    serverPassword: new ServerPassword(undefined),
    // No bans file: nobody is banned here (the bans: admin.test.cjs).
    serverBans: new ServerBans(''),
    joinQueue: {
      whitelist: { reason: 'not on the whitelist' },
      admits: (connection) => {
        observations.push(['whitelist', connection]);
        return true;
      },
    },
    emitAsync: async (event, connection) => {
      assert.equal(event, 'playerConnecting');
      observations.push(['handler', connection]);
    },
  };
  host.licence = new LicenceGate(host, {
    now: () => NOW,
    fetch: async () => Response.json({ keys: [{ kid: 'k1', ed25519: key }] }),
  });
  host.licence.setListing('abc123');
  const sign = (fields = {}) => {
    const body = Buffer.from(
      JSON.stringify({
        v: 1,
        kid: 'k1',
        store: 'steam',
        account: STEAM_ID,
        server: 'abc123',
        iat: NOW / 1000,
        exp: NOW / 1000 + 300,
        nonce: crypto.randomBytes(16).toString('base64url'),
        binding: BINDING,
        strength: 'licence',
        ...fields,
      }),
    ).toString('base64url');
    return `${body}.${crypto.sign(null, Buffer.from(body), privateKey).toString('base64url')}`;
  };
  const admit = (identity) => ResourceHost.prototype.admitConnection.call(host, 1, identity);
  return { host, sign, observations, admit };
}

test('Steam identity comes from the signed pass before whitelists and handlers run', async () => {
  const { host, sign, admit, observations } = fixture();
  assert.equal(
    (
      await admit({
        nickname: 'Henry',
        ip: '127.0.0.1',
        steamId: '',
        epicId: 'forged-epic',
        store: 'xbox',
        storeAccount: 'forged',
        licence: sign(),
        licenceBinding: BINDING,
      })
    ).allowed,
    true,
  );
  assert.deepEqual(
    observations.map(([stage]) => stage),
    ['whitelist', 'handler'],
  );
  for (const [, connection] of observations) {
    assert.equal(connection.steamId, STEAM_ID);
    // Only an Epic pass gives an epicId; what the client says is never taken.
    assert.equal(connection.epicId, '');
    assert.equal(connection.store, 'steam');
    assert.equal(connection.storeAccount, STEAM_ID);
    assert.equal(connection.nickname, 'Henry');
    assert.equal(connection.ip, '127.0.0.1');
    assert.equal(Object.isFrozen(connection), true);
  }
  // The player this join becomes has the pass's identity.
  assert.equal(host.admittedIdentities.get(1).steamId, STEAM_ID);
  assert.equal(host.admittedIdentities.get(1).store, 'steam');
});

test('a Discord id in the pass is verified and wins over the reported one', async () => {
  const { host, sign, admit, observations } = fixture();
  const reported = { nickname: 'Henry', discordId: '999', licenceBinding: BINDING };
  assert.equal(
    (await admit({ ...reported, licence: sign({ discord: '1234567890' }) })).allowed,
    true,
  );
  const [, connection] = observations.at(-1);
  assert.equal(connection.discordId, '1234567890');
  assert.equal(connection.discordVerified, true);
  assert.deepEqual(connection.getIdentifier('discord'), '1234567890');
  assert.equal(
    connection.identifiers.find((identifier) => identifier.kind === 'discord').verified,
    true,
  );
  assert.equal(host.admittedIdentities.get(1).discordVerified, true);
  // Without one in the pass: the reported id, unverified.
  assert.equal((await admit({ ...reported, licence: sign() })).allowed, true);
  const [, plain] = observations.at(-1);
  assert.equal(plain.discordId, '999');
  assert.equal(plain.discordVerified, false);
  // A pass whose Discord id is not a Discord id is malformed.
  assert.equal((await admit({ ...reported, licence: sign({ discord: 'x1' }) })).allowed, false);
});

test('requireDiscord admits only a pass with a verified Discord account', async () => {
  const { host, sign, admit, observations } = fixture();
  host.requireDiscord = true;
  const base = { nickname: 'Henry', discordId: '999', licenceBinding: BINDING };
  const refused = await admit({ ...base, licence: sign() });
  assert.equal(refused.allowed, false);
  assert.equal(refused.discord, true);
  assert.match(refused.reason, /verified Discord account/);
  // No handler hears of a refused one.
  assert.equal(observations.length, 0);
  const verified = await admit({ ...base, licence: sign({ discord: '1234567890' }) });
  assert.equal(verified.allowed, true);
  assert.equal(observations.at(-1)[1].discordId, '1234567890');
});

test('GOG, Epic and Xbox passes cannot inject a Steam identity', async () => {
  for (const store of ['gog', 'epic', 'xbox']) {
    const { sign, admit, observations } = fixture();
    assert.equal(
      (
        await admit({
          steamId: STEAM_ID,
          store: 'steam',
          storeAccount: 'forged',
          licence: sign({ store, account: `${store}-account` }),
          licenceBinding: BINDING,
        })
      ).allowed,
      true,
    );
    for (const [, connection] of observations) {
      assert.equal(connection.steamId, '');
      assert.equal(connection.epicId, store === 'epic' ? 'epic-account' : '');
      assert.equal(connection.store, store);
      assert.equal(connection.storeAccount, `${store}-account`);
    }
  }
});

test('missing proof, an account mismatch and a different handshake never reach admission handlers', async () => {
  for (const identity of [
    {},
    { steamId: '76561198000000002', licenceBinding: BINDING },
    { steamId: STEAM_ID, licenceBinding: '1111111111111111' },
  ]) {
    const { sign, admit, observations } = fixture();
    const proof = Object.keys(identity).length ? { licence: sign() } : {};
    assert.equal((await admit({ ...proof, ...identity })).allowed, false);
    assert.deepEqual(observations, []);
  }
});

test('an unlisted server refuses client identity claims without a pass', async () => {
  const { host, admit, observations } = fixture();
  host.licence.listing = '';
  const refused = await admit({ steamId: STEAM_ID, store: 'steam', storeAccount: 'forged' });
  assert.equal(refused.allowed, false);
  assert.match(refused.reason, /legally owned game/);
  assert.deepEqual(observations, [], 'no whitelist or handler saw the claims');
});

test("an unlisted server's pass for its own address gives the verified Steam identity", async () => {
  const { host, sign, admit, observations } = fixture();
  host.licence.listing = '';
  host.licence.setPort(7841);
  const allowed = await admit({
    licence: sign({ server: 'addr:127.0.0.1:7841' }),
    licenceBinding: BINDING,
    steamId: STEAM_ID,
  });
  assert.equal(allowed.allowed, true);
  assert.equal(observations.at(-1)[1].steamId, STEAM_ID);
});

test("a pass's KCD2 DLCs become the player's read-only entitlements", async () => {
  const { host, sign, admit, observations } = fixture();
  const allowed = await admit({
    licence: sign({ apps: [3368620, 999, 3118100] }),
    licenceBinding: BINDING,
  });
  assert.equal(allowed.allowed, true);
  const connection = observations.at(-1)[1];
  // In the pass's order; an app this server doesn't know is left out.
  assert.deepEqual([...connection.entitlements.kcd2Dlc], ['mysteria_ecclesiae', 'quest_for_valor']);
  assert.equal(connection.hasEntitlement('kcd2_dlc:mysteria_ecclesiae'), true);
  assert.equal(connection.hasEntitlement('kcd2_dlc:forge_tycoon'), false);
  assert.equal(connection.hasEntitlement('mysteria_ecclesiae'), false);
  assert.throws(() => connection.hasEntitlement(3368620), TypeError);
  assert.equal(Object.isFrozen(connection.entitlements.kcd2Dlc), true);
  assert.deepEqual(
    [...host.admittedIdentities.get(1).entitlements.kcd2Dlc],
    ['mysteria_ecclesiae', 'quest_for_valor'],
  );
  // A pass without apps: no entitlements.
  await admit({ licence: sign(), licenceBinding: BINDING });
  assert.deepEqual([...observations.at(-1)[1].entitlements.kcd2Dlc], []);
});

test('a server that starts on klaster admits only owners of Mysteria Ecclesiae', async () => {
  const { host, sign, admit, observations } = fixture();
  host.maps = ['klaster', 'kutnohorsko'];
  const refused = await admit({ licence: sign({ apps: [3118100] }), licenceBinding: BINDING });
  assert.equal(refused.allowed, false);
  assert.match(refused.reason, /Mysteria Ecclesiae/);
  assert.deepEqual(observations, [], 'no whitelist or handler saw the refused join');
  const allowed = await admit({ licence: sign({ apps: [3368620] }), licenceBinding: BINDING });
  assert.equal(allowed.allowed, true);
});

test('malformed pass apps are refused; unknown app ids are not', async () => {
  const { sign, admit } = fixture();
  for (const apps of [
    [3368620, 3368620],
    ['3368620'],
    [-1],
    3368620,
    new Array(33).fill(0).map((_, i) => i + 1),
  ]) {
    const refused = await admit({ licence: sign({ apps }), licenceBinding: BINDING });
    assert.equal(refused.allowed, false, JSON.stringify(apps));
  }
  const newer = await admit({ licence: sign({ apps: [4000000] }), licenceBinding: BINDING });
  assert.equal(newer.allowed, true);
});

test('a server with the KCD1 map admits only a pass that vouches for KCD1', async () => {
  const { host, sign, admit, observations } = fixture();
  host.kcd1Map = true;
  const base = { nickname: 'Henry', licenceBinding: BINDING };
  const refused = await admit({ ...base, licence: sign() });
  assert.equal(refused.allowed, false);
  assert.equal(refused.kcd1, true);
  assert.match(refused.reason, /Kingdom Come: Deliverance/);
  assert.equal(observations.length, 0);
  const owner = await admit({ ...base, licence: sign({ apps: [379430] }) });
  assert.equal(owner.allowed, true);
  // A pass vouching for another app only is refused like one without it.
  assert.equal((await admit({ ...base, licence: sign({ apps: [3368620] }) })).kcd1, true);
  // A pass vouching for KCD1 twice, or with apps that aren't a list, is not a valid pass at all.
  for (const apps of [[379430, 379430], 'kcd1']) {
    const forged = await admit({ ...base, licence: sign({ apps }) });
    assert.equal(forged.allowed, false);
    assert.notEqual(forged.kcd1, true);
  }
});

test('a server without the KCD1 map takes a pass with or without it', async () => {
  const { sign, admit } = fixture();
  const base = { nickname: 'Henry', licenceBinding: BINDING };
  assert.equal((await admit({ ...base, licence: sign() })).allowed, true);
  assert.equal((await admit({ ...base, licence: sign({ apps: [379430] }) })).allowed, true);
});

test('a server with the KCD1 map among others admits anyone; only owners may be sent there', async () => {
  const { host, sign, admit } = fixture();
  host.kcd1Map = true;
  host.maps = ['trosecko', 'kcd1_rataje'];
  const base = { nickname: 'Henry', licenceBinding: BINDING };
  const plain = await admit({ ...base, licence: sign() });
  assert.equal(plain.allowed, true);
  const owner = await admit({ ...base, licence: sign({ apps: [379430] }) });
  assert.equal(owner.allowed, true);
  // Starting on that map still needs it.
  host.maps = ['kcd1_rataje', 'trosecko'];
  assert.equal((await admit({ ...base, licence: sign() })).kcd1, true);

  // Player.setMap: the KCD1 map only for a player whose pass vouched for it.
  const sent = [];
  const identities = new Map([
    [1, { apps: [] }],
    [2, { apps: [379430] }],
  ]);
  const server = {
    maps: ['trosecko', 'kcd1_rataje'],
    bridge: { log: () => {}, send: (message) => sent.push(message) },
    requireLivePlayerCall: () => {},
    identityOf: (id) => identities.get(id),
    mapOfPlayer: () => 'trosecko',
  };
  const setMap = (id) =>
    ResourceHost.prototype.setPlayerMap.call(server, { id }, 'kcd1_rataje', undefined);
  assert.equal(setMap(1), false);
  assert.equal(sent.length, 0);
  assert.equal(setMap(2), true);
  assert.equal(sent.at(-1).op, 'playerMap');
});

test('a pass with KCD1 and the klaster DLC: Player.setMap sends its owner to each map', async () => {
  const { host, sign, admit } = fixture();
  const base = { nickname: 'Henry', licenceBinding: BINDING };
  assert.equal(
    (await admit({ ...base, licence: sign({ apps: [379430, 3368620] }) })).allowed,
    true,
  );
  // A server starting on klaster refuses a KCD1 owner without the DLC (no rejoin would help).
  host.maps = ['klaster', 'trosecko'];
  const refused = await admit({ ...base, licence: sign({ apps: [379430] }) });
  assert.equal(refused.allowed, false);
  assert.notEqual(refused.kcd1, true);
  assert.match(refused.reason, /Mysteria Ecclesiae/);
  const sent = [];
  const identities = new Map([
    [1, { apps: [379430], entitlements: { kcd2Dlc: [] } }],
    [2, { apps: [3368620], entitlements: { kcd2Dlc: ['mysteria_ecclesiae'] } }],
  ]);
  const server = {
    maps: ['trosecko', 'klaster', 'kcd1_rataje'],
    bridge: { log: () => {}, send: (message) => sent.push(message) },
    requireLivePlayerCall: () => {},
    identityOf: (id) => identities.get(id),
    entitlementsOf: (id) => identities.get(id).entitlements,
    mapOfPlayer: () => 'trosecko',
  };
  const setMap = (id, map) =>
    ResourceHost.prototype.setPlayerMap.call(server, { id }, map, undefined);
  assert.equal(setMap(1, 'klaster'), false);
  assert.equal(sent.length, 0);
  assert.equal(setMap(2, 'klaster'), true);
  assert.equal(setMap(2, 'kcd1_rataje'), false);
  assert.equal(setMap(1, 'kcd1_rataje'), true);
});
