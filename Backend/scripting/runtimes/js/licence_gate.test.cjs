'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { LicenceGate } = require('./licence/licence_gate.cjs');

const NOW = 1_800_000_000_000;
const BINDING = '0123456789abcdef';
const IDENTITY = { steamId: '76561198000000001', licenceBinding: BINDING };

function master() {
  const { privateKey, publicKey } = crypto.generateKeyPairSync('ed25519');
  const raw = Buffer.from(publicKey.export({ format: 'jwk' }).x, 'base64url');
  const keys = { keys: [{ kid: 'k1', ed25519: raw.toString('base64') }] };
  const sign = (fields = {}) => {
    const pass = {
      v: 1,
      kid: 'k1',
      store: 'steam',
      account: IDENTITY.steamId,
      server: 'abc123',
      iat: NOW / 1000,
      exp: NOW / 1000 + 300,
      nonce: crypto.randomBytes(16).toString('base64url'),
      binding: BINDING,
      strength: 'licence',
      ...fields,
    };
    const encoded = Buffer.from(JSON.stringify(pass)).toString('base64url');
    const signature = crypto.sign(null, Buffer.from(encoded), privateKey).toString('base64url');
    return `${encoded}.${signature}`;
  };
  let fetches = 0;
  const fetch = async (url, options) => {
    fetches += 1;
    assert.equal(url, 'https://master.example/v1/licence/keys');
    assert.equal(options.redirect, 'error');
    assert.ok(options.signal instanceof AbortSignal);
    return Response.json(keys);
  };
  return { sign, fetch, keys, fetches: () => fetches };
}

function gate(fetch, config = {}, options = {}) {
  const logs = [];
  const host = { bridge: { log: (owner, text) => logs.push(text) } };
  const licence = new LicenceGate(host, { fetch, now: () => NOW, ...options });
  Object.assign(licence.config, { master: 'https://master.example' }, config);
  return { licence, logs };
}

function configDirectory(t, text) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-licence-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  fs.writeFileSync(path.join(root, 'licence.json'), text);
  return root;
}

test('every server requires a licence: listed, unlisted, and whatever licence.json says', async (t) => {
  const { fetch } = master();
  const unlisted = gate(fetch).licence;
  assert.equal((await unlisted.check(undefined)).why, 'missing');
  assert.equal((await unlisted.check('')).why, 'missing');
  unlisted.setListing('abc123');
  assert.equal((await unlisted.check(undefined)).why, 'missing');

  for (const required of [false, true]) {
    const { licence, logs } = gate(fetch);
    licence.load(configDirectory(t, JSON.stringify({ required })));
    assert.equal((await licence.check(undefined)).why, 'missing', `required:${required}`);
    assert.ok(logs.some((line) => /"required" is not read/.test(line)));
  }
});

// A server that is not listed: its UDP port, two network interfaces and the address the Master
// saw it at; a pass names the address the player's game dialled.
function unlistedGate(t, options = {}) {
  const signer = master();
  const fetch = async (url, init) => {
    const answer = await signer.fetch(url, init);
    const body = await answer.json();
    return Response.json({ ...body, address: options.masterAddress ?? '203.0.113.7' });
  };
  const { licence, logs } = gate(
    fetch,
    {},
    {
      interfaces: () => ({
        lo: [{ address: '127.0.0.1' }],
        eth0: [{ address: '192.168.1.20' }, { address: 'fe80::1%eth0' }],
      }),
      publicAddress: options.publicAddress ?? '',
      lookup: options.lookup ?? (async () => []),
    },
  );
  licence.setPort(7841);
  return { licence, logs, sign: signer.sign };
}

test('an unlisted server accepts passes naming its port and one of its own addresses', async (t) => {
  for (const server of [
    'addr:127.0.0.1:7841',
    'addr:192.168.1.20:7841',
    'addr:203.0.113.7:7841',
    'addr:[fe80::1]:7841',
  ]) {
    const { licence, sign } = unlistedGate(t);
    const checked = await licence.check(sign({ server }), IDENTITY);
    assert.equal(checked.allowed, true, server);
    assert.equal(checked.pass.account, IDENTITY.steamId);
  }
});

test("an unlisted server refuses passes for another server's address or port", async (t) => {
  for (const server of [
    'addr:198.51.100.9:7841',
    'addr:127.0.0.1:7842',
    'addr:192.168.1.21:7841',
    'abc123',
  ]) {
    const { licence, sign } = unlistedGate(t);
    assert.equal((await licence.check(sign({ server }), IDENTITY)).why, 'wrongServer', server);
  }
  for (const server of ['addr:127.0.0.1', 'addr:localhost:7841', 'addr:127.0.0.1:70000', 'addr:']) {
    const { licence, sign } = unlistedGate(t);
    assert.equal((await licence.check(sign({ server }), IDENTITY)).why, 'malformed', server);
  }
  // Without its port the server names no address.
  const { licence, sign } = unlistedGate(t);
  licence.port = 0;
  assert.equal(
    (await licence.check(sign({ server: 'addr:127.0.0.1:7841' }), IDENTITY)).why,
    'wrongServer',
  );
});

test('KCDS_SERVER_PUBLIC_ADDRESS: an IP, or a host name resolved and kept for ten minutes', async (t) => {
  const direct = unlistedGate(t, { publicAddress: '198.51.100.4:7841', masterAddress: '' });
  assert.equal(
    (await direct.licence.check(direct.sign({ server: 'addr:198.51.100.4:7841' }), IDENTITY))
      .allowed,
    true,
  );
  let lookups = 0;
  const named = unlistedGate(t, {
    publicAddress: 'play.example.com:7841',
    masterAddress: '',
    lookup: async (host) => {
      lookups += 1;
      assert.equal(host, 'play.example.com');
      return [{ address: '198.51.100.5', family: 4 }];
    },
  });
  for (let i = 0; i < 2; i++) {
    const checked = await named.licence.check(
      named.sign({ server: 'addr:198.51.100.5:7841' }),
      IDENTITY,
    );
    assert.equal(checked.allowed, true);
  }
  assert.equal(lookups, 1);
  const unresolved = unlistedGate(t, {
    publicAddress: 'play.example.com',
    masterAddress: '',
    lookup: async () => {
      throw new Error('ENOTFOUND');
    },
  });
  assert.equal(
    (
      await unresolved.licence.check(
        unresolved.sign({ server: 'addr:198.51.100.5:7841' }),
        IDENTITY,
      )
    ).why,
    'wrongServer',
  );
});

test('network interfaces are read once for many joins and again after 30 seconds', async () => {
  const signer = master();
  let reads = 0;
  let clock = NOW;
  let address = '192.168.1.20';
  const { licence } = gate(
    signer.fetch,
    {},
    {
      now: () => clock,
      interfaces: () => {
        reads += 1;
        return { eth0: [{ address }] };
      },
    },
  );
  licence.setPort(7841);
  for (let i = 0; i < 20; i++) {
    const checked = await licence.check(
      signer.sign({ server: 'addr:192.168.1.20:7841' }),
      IDENTITY,
    );
    assert.equal(checked.allowed, true);
  }
  assert.equal(reads, 1);

  // A new address is seen once the kept list is 30 seconds old.
  address = '192.168.1.30';
  clock += 29 * 1000;
  const early = await licence.check(signer.sign({ server: 'addr:192.168.1.30:7841' }), IDENTITY);
  assert.equal(early.why, 'wrongServer');
  clock += 2 * 1000;
  const later = await licence.check(signer.sign({ server: 'addr:192.168.1.30:7841' }), IDENTITY);
  assert.equal(later.allowed, true);
  assert.equal(reads, 2);
});

test('a listed server also accepts a pass naming its own address', async (t) => {
  const { licence, sign } = unlistedGate(t);
  licence.setListing('abc123');
  assert.equal((await licence.check(sign({ server: 'abc123' }), IDENTITY)).allowed, true);
  assert.equal(
    (await licence.check(sign({ server: 'addr:192.168.1.20:7841' }), IDENTITY)).allowed,
    true,
  );
  assert.equal((await licence.check(sign({ server: 'xyz789' }), IDENTITY)).why, 'wrongServer');
});

test('malformed configuration cannot disable the gate', async (t) => {
  for (const raw of [
    '{',
    'null',
    '[]',
    '{"required":false,"stores":"steam"}',
    '{"required":false,"stores":["unknown"]}',
    '{"required":false,"familySharing":0}',
    '{"required":false,"master":null}',
    '{"required":false,"master":"http://master.example"}',
  ]) {
    const { licence } = gate(master().fetch, { required: false });
    licence.load(configDirectory(t, raw));
    assert.equal((await licence.check(undefined)).why, 'configuration', raw);
  }
});

test('production requires HTTPS; only an explicitly injected loopback transport may use HTTP', (t) => {
  const log = { bridge: { log: () => {} } };
  const root = configDirectory(t, '{"master":"http://127.0.0.1:12345"}');
  const production = new LicenceGate(log);
  production.load(root);
  assert.equal(production.configInvalid, true);
  const fixture = new LicenceGate(log, { fetch: async () => Response.json({}) });
  fixture.load(root);
  assert.equal(fixture.configInvalid, false);
  assert.equal(fixture.config.master, 'http://127.0.0.1:12345');
});

test('a correctly signed pass authorizes its server and handshake once', async () => {
  const { sign, fetch, fetches } = master();
  const { licence } = gate(fetch);
  licence.setListing('abc123');
  const pass = sign();
  assert.equal(
    (await licence.check(pass, { ...IDENTITY, licenceBinding: '1111111111111111' })).why,
    'wrongBinding',
  );
  const allowed = await licence.check(pass, IDENTITY);
  assert.equal(allowed.allowed, true);
  assert.equal(allowed.pass.account, IDENTITY.steamId);
  assert.equal((await licence.check(pass, IDENTITY)).why, 'reused');
  assert.equal(fetches(), 1);
});

test('simultaneous replay is consumed once and concurrent key fetches coalesce', async () => {
  const { sign, keys } = master();
  let release;
  const waiting = new Promise((resolve) => {
    release = resolve;
  });
  let fetches = 0;
  const { licence } = gate(async () => {
    fetches += 1;
    await waiting;
    return Response.json(keys);
  });
  licence.setListing('abc123');
  const pass = sign();
  const answers = Array.from({ length: 12 }, () => licence.check(pass, IDENTITY));
  release();
  const results = await Promise.all(answers);
  assert.equal(fetches, 1);
  assert.equal(results.filter((answer) => answer.allowed).length, 1);
  assert.equal(results.filter((answer) => answer.why === 'reused').length, 11);
});

test('tampering, account mismatch and another signing key are refused', async () => {
  const { sign, fetch } = master();
  const { licence } = gate(fetch);
  licence.setListing('abc123');
  const [encoded, signature] = sign().split('.');
  const body = JSON.parse(Buffer.from(encoded, 'base64url'));
  body.account = '76561198000000002';
  const changed = `${Buffer.from(JSON.stringify(body)).toString('base64url')}.${signature}`;
  assert.equal((await licence.check(changed, IDENTITY)).why, 'unsigned');
  assert.equal((await licence.check(master().sign(), IDENTITY)).why, 'unsigned');
  assert.equal(
    (await licence.check(sign(), { ...IDENTITY, steamId: '76561198000000002' })).why,
    'unsigned',
  );
  assert.equal((await licence.check(sign({ kid: 'unknown' }), IDENTITY)).why, 'unsigned');
  assert.equal((await licence.check(sign({ server: 'another' }), IDENTITY)).why, 'wrongServer');
});

test('passes require bounded ASCII identities and a valid handshake binding', async () => {
  const { sign, fetch } = master();
  const { licence } = gate(fetch);
  licence.setListing('abc123');
  for (const fields of [
    { kid: '' },
    { kid: 'x'.repeat(33) },
    { account: '' },
    { account: '玩家' },
    { store: 'steam\n' },
    { nonce: '' },
    { nonce: 'x'.repeat(129) },
    { nonce: 'space not allowed' },
    { owner: 'bad\nowner' },
    { store: 'gog', account: 'a'.repeat(129) },
    { server: '' },
  ]) {
    assert.equal(
      (await licence.check(sign(fields), IDENTITY)).why,
      'malformed',
      JSON.stringify(fields),
    );
  }
  for (const binding of [
    undefined,
    '',
    '0123456789abcde',
    '0123456789abcdeg',
    '0123456789ABCDEf',
  ]) {
    assert.equal((await licence.check(sign({ binding }), IDENTITY)).why, 'wrongBinding');
  }
  assert.equal((await licence.check(sign(), { steamId: IDENTITY.steamId })).why, 'wrongBinding');
  assert.equal((await licence.check('not-a-pass', IDENTITY)).why, 'malformed');
});

test('expired, future and overlong lifetimes fail; a 30 second clock skew is accepted', async () => {
  const { sign, fetch } = master();
  const { licence } = gate(fetch);
  licence.setListing('abc123');
  const seconds = NOW / 1000;
  for (const fields of [
    { iat: undefined },
    { iat: 0 },
    { iat: -1 },
    { iat: seconds + 0.5 },
    { iat: seconds + 31, exp: seconds + 300 },
    { exp: undefined },
    { exp: 0 },
    { exp: seconds },
    { exp: seconds - 1 },
    { exp: seconds + 301 },
    { exp: Number.MAX_SAFE_INTEGER + 1 },
  ]) {
    assert.equal(
      (await licence.check(sign(fields), IDENTITY)).why,
      'expired',
      JSON.stringify(fields),
    );
  }
  assert.equal(
    (await licence.check(sign({ iat: seconds + 30, exp: seconds + 300 }), IDENTITY)).allowed,
    true,
  );
});

test('account history cannot satisfy any listed-server configuration', async (t) => {
  const { sign, fetch } = master();
  const { licence } = gate(fetch);
  licence.load(configDirectory(t, JSON.stringify({ required: false, minimumStrength: 'account' })));
  licence.setListing('abc123');
  assert.equal(licence.config.minimumStrength, 'licence');
  assert.equal(
    (await licence.check(sign({ store: 'xbox', account: 'x1', strength: 'account' }), IDENTITY))
      .why,
    'strength',
  );
  licence.config.minimumStrength = 'account';
  assert.equal((await licence.check(sign({ strength: 'account' }), IDENTITY)).why, 'strength');
  licence.config.stores = ['steam'];
  assert.equal((await licence.check(sign({ store: 'gog', account: 'g1' }), IDENTITY)).why, 'store');
  licence.config.familySharing = false;
  assert.equal(
    (await licence.check(sign({ owner: '76561198000000002' }), IDENTITY)).why,
    'familySharing',
  );
});

test('disallowing sharing refuses Xbox when unshared ownership cannot be established', async (t) => {
  const { sign, fetch } = master();
  const pass = sign({ store: 'xbox', account: 'signed-licence-owner' });
  const { licence: defaults } = gate(fetch);
  defaults.setListing('abc123');
  assert.equal(defaults.config.familySharing, true);
  assert.equal((await defaults.check(pass, IDENTITY)).allowed, true);

  const { licence: strict } = gate(fetch);
  strict.load(configDirectory(t, JSON.stringify({ familySharing: false })));
  strict.setListing('abc123');
  const refused = await strict.check(pass, IDENTITY);
  assert.equal(refused.allowed, false);
  assert.equal(refused.why, 'sharingUnknown');
  assert.match(refused.reason, /cannot confirm unshared ownership/);
  assert.equal(strict.nonces.size, 0);
  assert.equal((await strict.check(sign(), IDENTITY)).allowed, true);
});

test('the replay cache refuses new passes at capacity and reclaims only expired entries', async () => {
  const { sign, fetch } = master();
  const { licence } = gate(fetch);
  licence.setListing('abc123');
  for (let index = 0; index < 100000; index += 1) {
    licence.nonces.set(`existing-${index}`, NOW / 1000 + 360);
  }
  const pass = sign();
  assert.equal((await licence.check(pass, IDENTITY)).why, 'capacity');
  assert.equal(licence.nonces.size, 100000);
  licence.nonces.set('existing-0', NOW / 1000);
  assert.equal((await licence.check(pass, IDENTITY)).allowed, true);
  assert.equal(licence.nonces.size, 100000);
  assert.equal(licence.nonces.has('existing-0'), false);
});

test('missing, malformed, oversized or unavailable public keys fail closed', async () => {
  const { sign, keys } = master();
  for (const fetch of [
    async () => {
      throw new Error('offline');
    },
    async () => new Response('unavailable', { status: 503 }),
    async () => Response.json({ keys: [] }),
    async () => Response.json({ keys: [{ kid: 'k1', ed25519: 'wrong' }] }),
    async () => Response.json({ keys: [keys.keys[0], keys.keys[0]] }),
    async () => new Response('{'.repeat(65537)),
    async () => new Response('{}', { headers: { 'content-length': '65537' } }),
  ]) {
    const { licence } = gate(fetch);
    licence.setListing('abc123');
    assert.equal((await licence.check(sign(), IDENTITY)).why, 'unsigned');
    assert.equal(licence.nonces.size, 0);
  }
});

test('a hanging key request times out and aborts its transport', async () => {
  const { sign } = master();
  let signal;
  const { licence } = gate(
    async (_url, options) => {
      signal = options.signal;
      return new Promise(() => {});
    },
    {},
    { keyTimeoutMs: 10 },
  );
  licence.setListing('abc123');
  assert.equal((await licence.check(sign(), IDENTITY)).why, 'unsigned');
  assert.equal(signal.aborted, true);
});

test('expired cached keys are not trusted when refresh fails', async () => {
  const { sign, keys } = master();
  let now = NOW;
  let online = true;
  const { licence } = gate(
    async () => {
      if (!online) throw new Error('offline');
      return Response.json(keys);
    },
    {},
    { now: () => now },
  );
  licence.setListing('abc123');
  assert.equal((await licence.check(sign(), IDENTITY)).allowed, true);
  now += 600001;
  online = false;
  const seconds = Math.floor(now / 1000);
  assert.equal(
    (await licence.check(sign({ iat: seconds, exp: seconds + 300 }), IDENTITY)).why,
    'unsigned',
  );
});
