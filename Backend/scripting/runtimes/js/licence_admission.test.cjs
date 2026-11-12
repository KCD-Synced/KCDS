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

test('a bot the native server let in skips only the licence gate', async () => {
  const { host, admit, observations } = fixture();
  const bot = { nickname: 'bot-1', ip: '127.0.0.1', licence: 'kcds-bot', licenceBinding: BINDING };
  // Without the native server's bot flag the text is no pass.
  assert.equal((await admit(bot)).allowed, false);
  assert.equal(observations.length, 0);
  // With it: in, through the whitelist and the handlers as every player, and known as a bot.
  assert.deepEqual(await admit({ ...bot, bot: true }), { allowed: true, reason: '' });
  assert.deepEqual(
    observations.map(([stage]) => stage),
    ['whitelist', 'handler'],
  );
  assert.equal(observations.at(-1)[1].steamId, '');
  assert.equal(host.admittedIdentities.get(1).bot, true);
});

test('a bot still meets the server password', async () => {
  const { host, admit } = fixture();
  host.serverPassword = new ServerPassword('secret');
  const refused = await admit({ nickname: 'bot-1', licence: 'kcds-bot', bot: true });
  assert.equal(refused.allowed, false);
  assert.equal(refused.password, true);
});
