'use strict';

// Queue and the whitelist: the native host's queue as the resources see and order
// it, and who the whitelist lets in.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const { ResourceHost } = require('./host.cjs');
const { LicenceGate } = require('./licence/licence_gate.cjs');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

test('Queue lists the waiting players in order and sends a script order to the host', async (t) => {
  const { host, sent } = serverFixture(t);
  await host.startAll();
  const { Queue } = host.makeAPI();
  const events = [];
  host.on('playerQueued', (entry) => events.push(['queued', entry.id, entry.nickname]));
  host.on('playerUnqueued', (entry, reason) => events.push(['unqueued', entry.id, reason]));
  host.joinQueue.queued({
    op: 'joinQueued',
    id: 1,
    ip: '10.0.0.1',
    nickname: 'Henry',
    steamId: '',
  });
  host.joinQueue.queued({
    op: 'joinQueued',
    id: 2,
    ip: '10.0.0.2',
    nickname: 'Hans',
    steamId: '7',
  });
  assert.deepEqual(
    Queue.list().map((entry) => entry.id),
    [1, 2],
  );
  Queue.setPriority(2, 10);
  assert.deepEqual(
    Queue.list().map((entry) => entry.id),
    [2, 1],
  );
  Queue.setStatus(2, { line: 'VIP lane', fields: [{ label: 'priority', value: 'VIP' }] });
  Queue.reject(1, 'Full for tonight');
  const orders = sent.filter((message) => message.op === 'joinQueueSet');
  assert.deepEqual(orders, [
    { op: 'joinQueueSet', id: 2, priority: 10 },
    { op: 'joinQueueSet', id: 2, line: 'VIP lane', fields: [['priority', 'VIP']] },
    { op: 'joinQueueSet', id: 1, reject: true, reason: 'Full for tonight' },
  ]);
  host.joinQueue.unqueued({ op: 'joinUnqueued', id: 1, reason: 'left' });
  host.joinQueue.unqueued({ op: 'joinUnqueued', id: 2, reason: 'admitted' });
  assert.deepEqual(events, [
    ['queued', 1, 'Henry'],
    ['queued', 2, 'Hans'],
    ['unqueued', 1, 'rejected'],
    ['unqueued', 2, 'admitted'],
  ]);
  assert.throws(() => Queue.setPriority(9, 1), /no player waits/);
  assert.throws(
    () =>
      Queue.setStatus(2, {
        fields: [{ label: 'a' }, { label: 'b' }, { label: 'c' }, { label: 'd' }],
      }),
    /no player waits|at most 3/,
  );
});

test('the whitelist refuses who it does not list, in the queue and at playerConnecting', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-whitelist-'));
  fs.writeFileSync(
    path.join(root, 'whitelist.json'),
    JSON.stringify({
      enabled: true,
      nicknames: ['henry'],
      steamIds: ['7656'],
      reason: 'Members only',
    }),
  );
  const sent = [];
  const host = new ResourceHost(root, { send: (message) => sent.push(message), log: () => {} });
  const { privateKey, publicKey } = crypto.generateKeyPairSync('ed25519');
  const now = Math.floor(Date.now() / 1000);
  const binding = '0123456789abcdef';
  host.licence = new LicenceGate(host, {
    fetch: async () =>
      Response.json({
        keys: [
          {
            kid: 'test',
            ed25519: Buffer.from(publicKey.export({ format: 'jwk' }).x, 'base64url').toString(
              'base64',
            ),
          },
        ],
      }),
  });
  host.licence.setListing('abc123');
  const licensed = (identity) => {
    const store = identity.steamId ? 'steam' : 'epic';
    const encoded = Buffer.from(
      JSON.stringify({
        v: 1,
        kid: 'test',
        store,
        account: identity.steamId || 'epicAccount',
        server: 'abc123',
        binding,
        iat: now,
        exp: now + 300,
        strength: 'licence',
        nonce: crypto.randomBytes(16).toString('base64url'),
      }),
    ).toString('base64url');
    return {
      ...identity,
      licenceBinding: binding,
      licence: `${encoded}.${crypto
        .sign(null, Buffer.from(encoded), privateKey)
        .toString('base64url')}`,
    };
  };
  await host.startAll();
  const seen = [];
  host.on('playerConnecting', (connection) => {
    seen.push([connection.nickname, connection.steamId, connection.ip, connection.isPending()]);
    connection.update('Checking your ticket');
  });
  assert.deepEqual(
    await host.admitConnection(1, licensed({ nickname: 'Stranger', steamId: '', ip: '1.2.3.4' })),
    {
      allowed: false,
      reason: 'Members only',
    },
  );
  assert.deepEqual(
    await host.admitConnection(2, licensed({ nickname: 'HENRY', steamId: '', ip: '1.2.3.5' })),
    {
      allowed: true,
      reason: '',
    },
  );
  assert.deepEqual(
    await host.admitConnection(3, licensed({ nickname: '', steamId: '7656', ip: '1.2.3.6' })),
    {
      allowed: true,
      reason: '',
    },
  );
  assert.deepEqual(seen, [
    ['HENRY', '', '1.2.3.5', true],
    ['', '7656', '1.2.3.6', true],
  ]);
  assert.deepEqual(
    sent
      .filter((message) => message.op === 'playerConnectingUpdate')
      .map((m) => [m.request, m.message]),
    [
      [2, 'Checking your ticket'],
      [3, 'Checking your ticket'],
    ],
  );
  host.joinQueue.queued({
    op: 'joinQueued',
    id: 5,
    ip: '1.1.1.1',
    nickname: 'Stranger',
    steamId: '',
  });
  assert.deepEqual(sent.filter((m) => m.op === 'joinQueueSet').at(-1), {
    op: 'joinQueueSet',
    id: 5,
    reject: true,
    reason: 'Members only',
  });
  await host.shutdown();
  fs.rmSync(root, { recursive: true, force: true });
});
