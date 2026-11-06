'use strict';

// the server's built-in KCDS-Admin (admin/) and its bans (admission/server_bans.cjs).

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { ResourceHost } = require('./host.cjs');
const { ServerBans } = require('./admission/server_bans.cjs');

const OWNER_PASSWORD = 'owner-pass-116';

async function adminHost(t, password = OWNER_PASSWORD) {
  // The server folder: its resources, server.json and backups/ side by side.
  const server = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-admin-'));
  const root = path.join(server, 'resources');
  fs.mkdirSync(root);
  const serverConfig = path.join(server, 'server.json');
  const logs = [];
  const sent = [];
  const host = new ResourceHost(root, {
    send: (message) => sent.push(message),
    log: (resource, text) => logs.push(`${resource} ${text}`),
    adminPort: 0,
    adminBind: '127.0.0.1',
    adminPassword: password,
    serverConfig,
  });
  host.licence.check = async () => ({ allowed: true, reason: '', pass: null });
  const commands = [];
  host.consoleCommand = async (text) => commands.push(text);
  t.after(async () => {
    await host.shutdown();
    fs.rmSync(server, { recursive: true, force: true });
  });
  await host.startAll();
  const address = host.adminServer.address();
  const base = address ? `http://127.0.0.1:${address.port}` : '';
  return { host, root, server, logs, sent, commands, base, serverConfig };
}

// A signed-in client: cookie kept, the admin header on every change.
function client(base) {
  let cookie = '';
  const call = async (method, route, body, headers = { 'x-kcds-admin': '1' }) => {
    const response = await fetch(base + route, {
      method,
      headers: {
        ...(method === 'GET' ? {} : headers),
        ...(cookie ? { cookie } : {}),
        ...(body === undefined ? {} : { 'content-type': 'application/json' }),
      },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    const set = response.headers.get('set-cookie');
    if (set) {
      cookie = set.split(';')[0];
    }
    const text = await response.text();
    let data = text;
    try {
      data = JSON.parse(text);
    } catch {
      // the page
    }
    return { status: response.status, data, headers: response.headers };
  };
  return call;
}

function joinedPlayer(host, id, identity) {
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

test('the page and sign-in: header required, wrong passwords limited, a strict cookie', async (t) => {
  const { base, logs } = await adminHost(t);
  const call = client(base);
  const page = await call('GET', '/');
  assert.equal(page.status, 200);
  assert.match(page.headers.get('content-security-policy'), /script-src 'nonce-[A-Za-z0-9+/=]+'/);
  assert.equal(page.headers.get('x-frame-options'), 'DENY');
  assert.ok(!String(page.data).includes('{{nonce}}'));
  assert.equal((await call('GET', '/api/status')).status, 401);
  // Another site's form cannot add the header.
  assert.equal(
    (await call('POST', '/api/login', { name: 'owner', password: OWNER_PASSWORD }, {})).status,
    403,
  );
  // Each wrong try says how many are left; then the address waits, told for how long.
  for (let i = 0; i < 5; i += 1) {
    const wrong = await call('POST', '/api/login', { name: 'owner', password: 'wrong-password' });
    assert.equal(wrong.status, 401);
    assert.deepEqual(wrong.data, { error: 'wrong name or password', triesLeft: 4 - i });
  }
  const locked = await call('POST', '/api/login', { name: 'owner', password: OWNER_PASSWORD });
  assert.equal(locked.status, 429);
  assert.equal(locked.data.error, 'too many failed sign-ins; wait 5 minutes');
  assert.ok(locked.data.retryAfter > 290 && locked.data.retryAfter <= 300);
  assert.equal(locked.headers.get('retry-after'), String(locked.data.retryAfter));
  assert.ok(logs.some((line) => line.includes('a failed sign-in as "owner"')));
  assert.ok(!logs.some((line) => line.includes(OWNER_PASSWORD)));
});

test('the owner signs in and sees the status, the log and the players', async (t) => {
  const { host, base, sent } = await adminHost(t);
  const call = client(base);
  const login = await call('POST', '/api/login', { name: 'owner', password: OWNER_PASSWORD });
  assert.equal(login.status, 200);
  // The session: who, until when (12 hours), and where this admin listens.
  const port = host.adminServer.address().port;
  assert.equal(login.data.name, 'owner');
  assert.equal(login.data.role, 'owner');
  assert.ok(Math.abs(login.data.expires - (Date.now() + 12 * 3600 * 1000)) < 60000);
  assert.deepEqual(login.data.listen, { scheme: 'http', bind: '127.0.0.1', port });
  assert.match(
    login.headers.get('set-cookie'),
    /HttpOnly; SameSite=Strict; Path=\/; Max-Age=43200/,
  );
  assert.deepEqual((await call('GET', '/api/session')).data, login.data);
  assert.equal((await call('GET', '/api/status')).data.name, 'KCDS server');
  host.bridge.log('freeroam', 'hello from a resource');
  const log = await call('GET', '/api/log?after=0');
  const line = log.data.lines.find((entry) => entry.text === 'hello from a resource');
  assert.equal(line.source, 'freeroam');
  assert.deepEqual(
    (await call('GET', `/api/log?after=${line.seq}`)).data.lines.filter(
      (entry) => entry.text === 'hello from a resource',
    ),
    [],
  );

  joinedPlayer(host, 4, {
    nickname: 'Henry',
    steamId: '7656',
    store: 'steam',
    storeAccount: '7656',
    discordId: '',
    deviceId: 'ab'.repeat(32),
    ip: '10.0.0.4',
  });
  const players = await call('GET', '/api/players');
  assert.equal(players.data.length, 1);
  assert.equal(players.data[0].name, 'Henry');
  assert.deepEqual(
    players.data[0].identifiers.map(({ kind }) => kind),
    ['steam', 'hardware', 'ip'],
  );
  const message = await call('POST', '/api/players/4/message', { text: 'Welcome to Rattay' });
  assert.equal(message.status, 200);
  assert.ok(
    sent.some(
      (entry) => entry.op === 'chat' && entry.target === 4 && entry.text === 'Welcome to Rattay',
    ),
  );
  assert.equal((await call('POST', '/api/players/99/kick', { reason: 'x' })).status, 404);
  // A long reason is cut to the 200 bytes a kick carries, not refused.
  const kicked = await call('POST', '/api/players/4/kick', { reason: 'ř'.repeat(150) });
  assert.equal(kicked.status, 200);
  const kick = sent.find((entry) => entry.op === 'kick' && entry.id === 4);
  assert.equal(kick.reason, 'ř'.repeat(100));
  assert.equal((await call('POST', '/api/players/4/teleport', { x: 'a' })).status, 400);
});

test('a ban by the admin kicks the player and refuses them at the next join', async (t) => {
  const { host, base, root, sent } = await adminHost(t);
  const call = client(base);
  await call('POST', '/api/login', { name: 'owner', password: OWNER_PASSWORD });
  const identity = {
    nickname: 'Hans',
    steamId: '',
    store: '',
    storeAccount: '',
    discordId: '1563839669496578088',
    deviceId: 'cd'.repeat(32),
    ip: '10.0.0.5',
  };
  joinedPlayer(host, 5, identity);
  const banned = await call('POST', '/api/players/5/ban', { reason: 'Griefing', minutes: 60 });
  assert.equal(banned.status, 200);
  assert.ok(
    sent.some(
      (entry) => entry.op === 'kick' && entry.id === 5 && entry.reason.includes('Griefing'),
    ),
  );
  const bans = (await call('GET', '/api/bans')).data;
  assert.equal(bans.length, 1);
  // The address is never banned: strangers behind the same NAT would be.
  assert.deepEqual(bans[0].identifiers, [
    'discord:1563839669496578088',
    `hardware:${'cd'.repeat(32)}`,
  ]);
  assert.equal(bans[0].by, 'owner');
  assert.ok(fs.existsSync(path.join(root, 'bans.json')));
  // A new account on the same machine is refused with the reason and the end.
  const again = await host.admitConnection(41, { ...identity, discordId: '', nickname: 'Hans2' });
  assert.equal(again.allowed, false);
  assert.match(
    again.reason,
    /You are banned from this server: Griefing\. Until \d{4}-\d\d-\d\d \d\d:\d\d UTC\./,
  );
  // Lifted: let in again.
  assert.equal((await call('DELETE', `/api/bans/${bans[0].id}`)).status, 200);
  assert.deepEqual(await host.admitConnection(42, identity), { allowed: true, reason: '' });
  // A ban by identifiers without a player.
  assert.equal(
    (
      await call('POST', '/api/bans', {
        identifiers: ['steam:76561198000000001'],
        reason: 'Cheating',
      })
    ).status,
    200,
  );
  assert.equal(
    (await call('POST', '/api/bans', { identifiers: ['nonsense'], reason: 'x' })).status,
    400,
  );
});

test('moderators: added by the owner, players yes, console, resources and accounts no', async (t) => {
  const { base, commands, root } = await adminHost(t);
  const owner = client(base);
  await owner('POST', '/api/login', { name: 'owner', password: OWNER_PASSWORD });
  assert.equal(
    (await owner('POST', '/api/accounts', { name: 'marek', password: 'short' })).status,
    400,
  );
  assert.equal(
    (await owner('POST', '/api/accounts', { name: 'marek', password: 'moderator-pass' })).status,
    200,
  );
  const stored = fs.readFileSync(path.join(root, 'admin-accounts.json'), 'utf8');
  assert.ok(!stored.includes('moderator-pass'));
  assert.match(stored, /"hash": "scrypt\$[0-9a-f]{32}\$[0-9a-f]{64}"/);
  assert.deepEqual((await owner('GET', '/api/accounts')).data, [
    { name: 'owner', role: 'owner' },
    { name: 'marek', role: 'moderator' },
  ]);
  assert.equal((await owner('POST', '/api/command', { text: 'restart freeroam' })).status, 200);
  assert.deepEqual(commands, ['restart freeroam']);

  const moderator = client(base);
  const login = await moderator('POST', '/api/login', {
    name: 'marek',
    password: 'moderator-pass',
  });
  assert.equal(login.data.name, 'marek');
  assert.equal(login.data.role, 'moderator');
  assert.equal((await moderator('GET', '/api/players')).status, 200);
  // A moderator sees a planned restart (the banner) but cannot plan or call one off.
  assert.deepEqual((await moderator('GET', '/api/server')).data, { pending: null });
  assert.equal((await moderator('POST', '/api/server/restart', { minutes: 5 })).status, 403);
  assert.equal((await moderator('GET', '/api/bans')).status, 200);
  assert.equal((await moderator('POST', '/api/command', { text: 'stop dice' })).status, 403);
  assert.equal((await moderator('GET', '/api/resources')).status, 403);
  assert.equal((await moderator('GET', '/api/accounts')).status, 403);
  assert.deepEqual(commands, ['restart freeroam']);
  // Removed: their session ends at once.
  assert.equal((await owner('DELETE', '/api/accounts/marek')).status, 200);
  assert.equal((await moderator('GET', '/api/players')).status, 401);
  // Signing out ends the owner's session.
  assert.equal((await owner('POST', '/api/logout')).status, 200);
  assert.equal((await owner('GET', '/api/status')).status, 401);
});

test('without an owner password of 8 or more characters the admin does not listen', async (t) => {
  const { host, base, logs } = await adminHost(t, 'short');
  assert.equal(base, '');
  assert.equal(host.adminServer.address(), null);
  assert.ok(logs.some((line) => line.includes('KCDS-Admin off')));
});

test('server bans: lengths, matching by any identifier and ended bans', () => {
  let now = 1_800_000_000_000;
  const bans = new ServerBans(
    '',
    () => {},
    () => now,
  );
  const ban = bans.add({
    identifiers: ['steam:1', 'hardware:ab'],
    reason: 'x',
    by: 'owner',
    minutes: 10,
  });
  assert.equal(bans.match([{ kind: 'hardware', id: 'ab' }]).id, ban.id);
  assert.equal(bans.match([{ kind: 'steam', id: '2' }]), null);
  now += 11 * 60000;
  assert.equal(bans.match([{ kind: 'steam', id: '1' }]), null);
  assert.equal(bans.list()[0].active, false);
  assert.throws(() => bans.add({ identifiers: [], reason: 'x', by: 'owner' }), TypeError);
  assert.throws(
    () => bans.add({ identifiers: ['steam:1'], reason: 'line\nbreak', by: 'owner' }),
    TypeError,
  );
  assert.throws(
    () => bans.add({ identifiers: ['steam:1'], reason: 'x', by: 'owner', minutes: -1 }),
    TypeError,
  );
});

test('restarts: warnings at 15, 5 and 1 minutes and 10 seconds, then serverStop; calling off', () => {
  const { ServerSchedule, parseTimes } = require('./admin/server_schedule.cjs');
  let now = Date.UTC(2026, 9, 26, 10, 0, 0);
  const timers = [];
  const sent = [];
  const host = { bridge: { send: (message) => sent.push(message) } };
  const schedule = new ServerSchedule(host, {
    now: () => now,
    timers: {
      setTimeout: (fn, delay) => {
        const timer = { fn, at: now + delay, live: true };
        timers.push(timer);
        return timer;
      },
      clearTimeout: (timer) => {
        timer.live = false;
      },
    },
  });
  const advance = (ms) => {
    now += ms;
    for (const timer of timers.filter((entry) => entry.live && entry.at <= now)) {
      timer.live = false;
      timer.fn();
    }
  };
  schedule.request({ restart: true, minutes: 20, by: 'owner' });
  assert.equal(schedule.pending().restart, true);
  advance(5 * 60000);
  assert.deepEqual(
    sent.map((message) => message.text),
    ['The server restarts in 15 minutes.'],
  );
  advance(14 * 60000 + 50000);
  assert.deepEqual(
    sent.map((message) => message.text),
    [
      'The server restarts in 15 minutes.',
      'The server restarts in 5 minutes.',
      'The server restarts in 1 minute.',
      'The server restarts in 10 seconds.',
    ],
  );
  advance(10000);
  assert.deepEqual(sent.at(-1), {
    op: 'serverStop',
    reason: 'The server is restarting. Join again in a minute.',
    restart: true,
  });
  assert.equal(schedule.pending(), null);

  sent.length = 0;
  schedule.request({ restart: false, minutes: 2, by: 'owner' });
  assert.equal(schedule.cancel('owner'), true);
  assert.equal(sent.at(-1).text, 'The server shutdown was called off.');
  advance(5 * 60000);
  assert.ok(!sent.some((message) => message.op === 'serverStop'));
  assert.throws(() => schedule.request({ restart: true, minutes: -1, by: 'owner' }), RangeError);
  // "In 1 minute" warns at once that it is 1 minute.
  sent.length = 0;
  schedule.request({ restart: true, minutes: 1, by: 'owner' });
  advance(0);
  assert.equal(sent[0].text, 'The server restarts in 1 minute.');
  schedule.cancel('owner');
  assert.deepEqual(parseTimes('06:00, 18:30,25:00,x'), {
    times: [
      [6, 0],
      [18, 30],
    ],
    invalid: ['25:00', 'x'],
  });
});

test('the owner restarts from the admin; a moderator may not', async (t) => {
  const { base, sent } = await adminHost(t);
  const owner = client(base);
  await owner('POST', '/api/login', { name: 'owner', password: OWNER_PASSWORD });
  const planned = await owner('POST', '/api/server/restart', { minutes: 0 });
  assert.equal(planned.status, 200);
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.ok(sent.some((message) => message.op === 'serverStop' && message.restart === true));
  assert.equal((await owner('POST', '/api/server/cancel')).status, 404);
  await owner('POST', '/api/accounts', { name: 'marek', password: 'moderator-pass' });
  const moderator = client(base);
  await moderator('POST', '/api/login', { name: 'marek', password: 'moderator-pass' });
  assert.equal((await moderator('POST', '/api/server/stop', { minutes: 5 })).status, 403);
});

test('the owner edits server.json: only one JSON object is saved', async (t) => {
  const { base, serverConfig } = await adminHost(t);
  const owner = client(base);
  await owner('POST', '/api/login', { name: 'owner', password: OWNER_PASSWORD });
  const empty = await owner('GET', '/api/config');
  assert.equal(empty.data.exists, false);
  assert.equal(empty.data.path, serverConfig);
  assert.equal((await owner('PUT', '/api/config', { text: '{ "port": 7777,' })).status, 400);
  assert.equal((await owner('PUT', '/api/config', { text: '[1, 2]' })).status, 400);
  const saved = await owner('PUT', '/api/config', { text: '{ "name": "Rattay" }' });
  assert.equal(saved.status, 200);
  assert.equal(fs.readFileSync(serverConfig, 'utf8'), '{ "name": "Rattay" }\n');
  assert.equal((await owner('GET', '/api/config')).data.text, '{ "name": "Rattay" }\n');
  await owner('POST', '/api/accounts', { name: 'marek', password: 'moderator-pass' });
  const moderator = client(base);
  await moderator('POST', '/api/login', { name: 'marek', password: 'moderator-pass' });
  assert.equal((await moderator('GET', '/api/config')).status, 403);
});

test('backups: the resources and server.json, made now and before a planned restart', async (t) => {
  const { base, root, server, serverConfig } = await adminHost(t);
  fs.writeFileSync(serverConfig, '{ "name": "Rattay" }');
  fs.mkdirSync(path.join(root, 'shop', 'node_modules', 'big'), { recursive: true });
  fs.writeFileSync(path.join(root, 'shop', 'stock.json'), '{"bread": 3}');
  fs.writeFileSync(path.join(root, 'shop', 'node_modules', 'big', 'index.js'), '// left out');
  const owner = client(base);
  await owner('POST', '/api/login', { name: 'owner', password: OWNER_PASSWORD });
  const made = await owner('POST', '/api/backups');
  assert.equal(made.status, 200);
  const folder = path.join(server, 'backups', made.data.name);
  assert.equal(
    fs.readFileSync(path.join(folder, 'resources', 'shop', 'stock.json'), 'utf8'),
    '{"bread": 3}',
  );
  assert.equal(fs.readFileSync(path.join(folder, 'server.json'), 'utf8'), '{ "name": "Rattay" }');
  assert.ok(!fs.existsSync(path.join(folder, 'resources', 'shop', 'node_modules')));
  // A second one in the same second gets its own name.
  const again = await owner('POST', '/api/backups');
  assert.notEqual(again.data.name, made.data.name);
  assert.equal((await owner('GET', '/api/backups')).data.length, 2);
  // A restart backs up first.
  await owner('POST', '/api/server/restart', { minutes: 0 });
  await new Promise((resolve) => setTimeout(resolve, 30));
  assert.equal((await owner('GET', '/api/backups')).data.length, 3);
});

test('the resources tab: each resource with its package.json version', async (t) => {
  const { host, root, base } = await adminHost(t);
  fs.mkdirSync(path.join(root, 'freeroam'));
  fs.writeFileSync(
    path.join(root, 'freeroam', 'package.json'),
    JSON.stringify({ name: 'freeroam', version: '1.2.0', kcds: { server: 'server.js' } }),
  );
  fs.writeFileSync(path.join(root, 'freeroam', 'server.js'), '');
  host.refresh();
  await host.start('freeroam');
  const owner = client(base);
  await owner('POST', '/api/login', { name: 'owner', password: OWNER_PASSWORD });
  assert.deepEqual((await owner('GET', '/api/resources')).data, [
    { name: 'freeroam', version: '1.2.0', running: true },
  ]);
});
