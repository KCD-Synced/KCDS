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
    health: null,
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

test('metrics are Prometheus text, and the status health comes from the last interval', async (t) => {
  const { host, base } = await statusHost(t, {});
  host.players.set(7, { id: 7, nickname: 'Henry' });
  t.after(() => host.players.clear());
  // Before the first interval: the host's own gauges only.
  let text = await (await fetch(`${base}/metrics`)).text();
  assert.match(text, /^# TYPE kcds_players gauge\nkcds_players 1\n/);
  assert.match(text, /\nkcds_max_players 64\n/);
  // The resource host's own CPU and memory (the native numbers leave it out).
  assert.match(text, /\nkcds_host_cpu_percent_of_one_core \d+(\.\d+)?\n/);
  const rss = Number(/\nkcds_host_rss_bytes (\d+)\n/.exec(text)?.[1]);
  assert.ok(rss > 1024 * 1024);
  assert.match(
    text,
    /\nkcds_players_by_kind\{kind="player"\} 1\nkcds_players_by_kind\{kind="bot"\} 0\n/,
  );
  // A load-testing bot (kcds-bots) is counted apart.
  host.players.set(8, { id: 8, nickname: 'bot-1' });
  host.playerIdentities.set(8, Object.freeze({ bot: true }));
  t.after(() => host.playerIdentities.delete(8));
  text = await (await fetch(`${base}/metrics`)).text();
  assert.match(text, /\nkcds_players 2\n/);
  assert.match(
    text,
    /\nkcds_players_by_kind\{kind="player"\} 1\nkcds_players_by_kind\{kind="bot"\} 1\n/,
  );
  assert.equal((await (await fetch(`${base}/dynamic.json`)).json()).health, null);

  host.statusRoutes.setMetrics(
    JSON.stringify({
      schema: 'kcds.server.metrics.v1',
      unix_ms: 1,
      interval_s: 10,
      tick: {
        ticks: 600,
        work: { count: 600, mean_ms: 9.5, p50_ms: 9, p95_ms: 15.5, p99_ms: 19.7, max_ms: 25 },
        missed_deadlines: 2,
      },
      udp: { interval: { bytes_out: 1000 } },
      queues: { reliable_retransmits_total: 4 },
      resources: { running: true },
      process: { cpu_percent_of_one_core: 412.6 },
      checks: { melee_refusals_total: 1 },
    }),
  );
  host.statusRoutes.setMetrics('not json');
  const response = await fetch(`${base}/metrics`);
  assert.equal(response.headers.get('content-type'), 'text/plain; version=0.0.4');
  text = await response.text();
  assert.match(text, /\nkcds_tick_work_count 600\n/);
  assert.match(text, /\nkcds_tick_work_ms\{stat="p95"\} 15\.5\n/);
  assert.match(text, /\nkcds_tick_missed_deadlines 2\n/);
  assert.match(text, /\nkcds_udp_interval_bytes_out 1000\n/);
  assert.match(text, /\n# TYPE kcds_queues_reliable_retransmits_total counter\n/);
  assert.match(text, /\nkcds_resources_running 1\n/);
  assert.match(text, /\nkcds_checks_melee_refusals_total 1\n/);
  assert.ok(!text.includes('schema') && !text.includes('unix_ms'));
  assert.deepEqual((await (await fetch(`${base}/dynamic.json`)).json()).health, {
    tickMs: 15.5,
    missedPerMinute: 12,
    cpuPercent: 413,
  });
  assert.equal((await fetch(`${base}/metrics`, { method: 'POST' })).status, 405);
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
