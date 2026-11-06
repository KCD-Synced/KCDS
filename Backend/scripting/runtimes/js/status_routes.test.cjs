'use strict';

// the server's status over HTTP (web/status_routes.cjs).

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { ResourceHost } = require('./host.cjs');
const { serverName, serverImage } = require('./web/status_routes.cjs');

async function statusHost(t, env) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-status-'));
  const host = new ResourceHost(root, {
    send: () => {},
    log: () => {},
    httpPort: 0,
    httpBind: '127.0.0.1',
  });
  t.after(async () => {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  });
  const folder = path.join(root, 'shop');
  fs.mkdirSync(folder, { recursive: true });
  fs.writeFileSync(
    path.join(folder, 'package.json'),
    JSON.stringify({ name: 'shop', kcds: { serverScripts: ['main.js'] } }),
  );
  // A resource's own route under the same name as a status path does not shadow it.
  fs.writeFileSync(
    path.join(folder, 'main.js'),
    "kcds.core.Http.route('GET', '/info.json', () => ({ body: { shop: true } }));",
  );
  host.statusRoutes.env = env;
  host.statusRoutes.configure({ map: 'trosecko', maxPeers: 64, version: '0.3.2', protocol: 2 });
  await host.startAll();
  const base = `http://127.0.0.1:${host.httpRoutes.address().port}`;
  return { host, base };
}

test('info.json, dynamic.json and players.json tell the public status', async (t) => {
  const { host, base } = await statusHost(t, {
    KCDS_SERVER_NAME: 'Rattay Roleplay',
    KCDS_SERVER_IMAGE_URL: 'https://example.com/card.png',
    KCDS_SERVER_PASSWORD_SECRET_NEVER: 'not read',
  });
  host.players.set(7, { id: 7, nickname: 'Henry' });
  host.players.set(3, { id: 3, nickname: 'Hans' });
  t.after(() => host.players.clear());

  const info = await fetch(`${base}/info.json`);
  assert.equal(info.status, 200);
  assert.equal(info.headers.get('cache-control'), 'no-store');
  assert.equal(info.headers.get('access-control-allow-origin'), '*');
  const body = await info.json();
  assert.equal(body.name, 'Rattay Roleplay');
  assert.equal(body.image, 'https://example.com/card.png');
  assert.equal(body.level, 'trosecko');
  assert.equal(body.players, 2);
  assert.equal(body.maxPlayers, 64);
  assert.equal(body.version, '0.3.2');
  assert.equal(body.protocol, 2);
  assert.equal(body.password, false);
  assert.equal(body.playerList, true);
  assert.deepEqual(body.licence, { stores: ['steam', 'gog', 'epic', 'xbox'], familySharing: true });
  assert.deepEqual(body.resources, ['shop']);
  assert.ok(Number.isInteger(body.uptime) && body.uptime >= 0);
  // Not listed on the Master (yet): no join link.
  assert.equal(body.joinLink, '');
  host.licence.setListing('6180b06a1f1bdefb');
  const listed = await (await fetch(`${base}/info.json`)).json();
  assert.equal(listed.joinLink, 'kcds://server/6180b06a1f1bdefb');
  // Nothing of the environment beyond the public fields.
  assert.ok(!JSON.stringify(body).includes('not read'));

  const dynamic = await (await fetch(`${base}/dynamic.json`)).json();
  assert.deepEqual(dynamic, {
    name: 'Rattay Roleplay',
    level: 'trosecko',
    players: 2,
    maxPlayers: 64,
  });

  const players = await (await fetch(`${base}/players.json`)).json();
  assert.deepEqual(players, [
    { id: 3, name: 'Hans' },
    { id: 7, name: 'Henry' },
  ]);

  assert.equal((await fetch(`${base}/info.json`, { method: 'POST' })).status, 405);
  const head = await fetch(`${base}/dynamic.json`, { method: 'HEAD' });
  assert.equal(head.status, 200);
  // The resource's own routes still answer under its name.
  assert.deepEqual(await (await fetch(`${base}/shop/info.json`)).json(), { shop: true });
});

test('a private player list answers 403 and info.json says so', async (t) => {
  const { base } = await statusHost(t, { KCDS_PUBLIC_PLAYER_LIST: 'off' });
  const players = await fetch(`${base}/players.json`);
  assert.equal(players.status, 403);
  assert.deepEqual(await players.json(), { error: 'the player list is private' });
  const info = await (await fetch(`${base}/info.json`)).json();
  assert.equal(info.playerList, false);
  assert.equal(info.name, 'KCDS server');
});

test('the name and the card art are checked as the Master listing checks them', () => {
  assert.equal(serverName(''), 'KCDS server');
  assert.equal(serverName('x'.repeat(65)), 'KCDS server');
  assert.equal(serverName('line\nbreak'), 'KCDS server');
  assert.equal(serverName('Kuttenberg'), 'Kuttenberg');
  assert.equal(serverImage('http://example.com/a.png'), '');
  assert.equal(serverImage('not a url'), '');
  assert.equal(serverImage('https://example.com/a.png'), 'https://example.com/a.png');
});
