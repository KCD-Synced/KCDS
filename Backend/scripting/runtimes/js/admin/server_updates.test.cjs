'use strict';

// one-click server updates from the signed KCDS update feed.

const test = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { ServerUpdates, releaseLabel } = require('./server_updates.cjs');

const ORIGIN = 'https://updates.example';
const sha = (bytes) => crypto.createHash('sha256').update(bytes).digest('hex');

function feed() {
  const { publicKey, privateKey } = crypto.generateKeyPairSync('ed25519');
  const keys = { test: publicKey.export({ type: 'spki', format: 'pem' }) };
  const artifacts = new Map();
  const channels = new Map();
  const publish = (channel, release, sequence, files, sign = privateKey) => {
    const listed = Object.entries(files).map(([file, text]) => {
      const bytes = Buffer.from(text);
      artifacts.set(sha(bytes), bytes);
      return { path: file, sha256: sha(bytes), size: bytes.length };
    });
    const payload = Buffer.from(
      JSON.stringify({ schemaVersion: 1, release, sequence, protocolVersion: 1, files: listed }),
    );
    channels.set(channel, {
      schemaVersion: 1,
      algorithm: 'Ed25519',
      keyId: 'test',
      payload: payload.toString('base64'),
      signature: crypto.sign(null, payload, sign).toString('base64'),
    });
  };
  const fetched = [];
  const fetch = async (url) => {
    fetched.push(url);
    const reply = (status, bytes) => ({
      ok: status === 200,
      status,
      text: async () => bytes.toString('utf8'),
      arrayBuffer: async () => bytes,
    });
    const manifest = url.match(/\/v1\/updates\/([^/]+)\/manifest$/);
    if (manifest && channels.has(manifest[1])) {
      return reply(200, Buffer.from(JSON.stringify(channels.get(manifest[1]))));
    }
    const artifact = url.match(/\/v1\/artifacts\/([a-f0-9]{64})$/);
    if (artifact && artifacts.has(artifact[1])) {
      return reply(200, artifacts.get(artifact[1]));
    }
    return reply(404, Buffer.alloc(0));
  };
  return { keys, publish, fetch, fetched, artifacts };
}

function install(files) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-update-'));
  for (const [file, text] of Object.entries(files)) {
    const target = path.join(root, ...file.split('/'));
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.writeFileSync(target, text);
  }
  return root;
}

const read = (root, file) => fs.readFileSync(path.join(root, ...file.split('/')), 'utf8');

test('a hotfix version is labelled as the feed labels it', () => {
  assert.equal(releaseLabel('0.3.2'), '0.3.2');
  assert.equal(releaseLabel('0.3.1.1'), '0.3.1-hotfix.1');
});

test('the shipped publisher keys are the release tool keys', () => {
  const shipped = JSON.parse(fs.readFileSync(path.join(__dirname, 'update_keys.json'), 'utf8'));
  const release = JSON.parse(
    fs.readFileSync(
      path.join(
        __dirname,
        '..',
        '..',
        '..',
        '..',
        '..',
        'Dev/tools/release/update-publisher-keys.json',
      ),
      'utf8',
    ),
  );
  assert.deepEqual(shipped, release);
});

test('check, download and apply a release; only managed, changed files; no rollback', async () => {
  const { keys, publish, fetch, fetched } = feed();
  const root = install({
    'kcd2s-server': 'old binary',
    'runtime/main.cjs': 'same',
    'runtime/gone.cjs': 'old',
    'server.json': '{"name":"mine"}',
  });
  const logged = [];
  const updates = new ServerUpdates({
    install: root,
    version: () => '0.3.1.1',
    platform: 'linux',
    env: { KCDS_UPDATE_ORIGIN: ORIGIN + '/' },
    publicKeys: keys,
    log: (text) => logged.push(text),
    fetch,
  });
  assert.deepEqual(updates.state(), {
    available: true,
    reason: '',
    current: '0.3.1.1',
    channel: 'server-linux-stable',
    staged: null,
  });
  publish('server-linux-stable', '0.3.2', 5, {
    'kcd2s-server': 'new binary',
    'runtime/main.cjs': 'same',
    'runtime/added.cjs': 'added',
    'server.json': '{"name":"theirs"}',
    'resources/freeroam/x.js': 'resource',
  });
  assert.deepEqual(await updates.check(), { release: '0.3.2', sequence: 5, newer: true });

  const staged = await updates.stage();
  assert.deepEqual(staged, { release: '0.3.2', files: 2 }, 'the binary and the new module');
  assert.equal(updates.state().staged, '0.3.2');
  assert.equal(
    fetched.filter((url) => url.includes('/artifacts/')).length,
    2,
    'unchanged files are not downloaded',
  );
  assert.equal(read(root, 'kcd2s-server'), 'old binary', 'nothing changes before the restart');

  assert.equal(updates.apply(), true);
  assert.equal(read(root, 'kcd2s-server'), 'new binary');
  assert.equal(read(root, 'runtime/added.cjs'), 'added');
  assert.equal(read(root, 'server.json'), '{"name":"mine"}', "the owner's settings stay");
  assert.ok(!fs.existsSync(path.join(root, 'resources')), 'resources are not managed');
  assert.equal(read(root, 'kcd2s-server.kcds-old'), 'old binary', 'set aside, not overwritten');
  assert.equal(updates.state().staged, null);
  assert.equal(updates.apply(), false, 'nothing staged any more');
  updates.cleanup();
  assert.ok(!fs.existsSync(path.join(root, 'kcd2s-server.kcds-old')));
  assert.match(logged.at(-1), /update 0\.3\.2 applied: 2 files/);

  // An older sequence on the feed is refused.
  publish('server-linux-stable', '0.3.1', 4, { 'kcd2s-server': 'older binary' });
  await assert.rejects(updates.check(), /older release/);
});

test('a forged manifest, a tampered file and a release without a binary are refused', async () => {
  const { keys, publish, fetch, artifacts } = feed();
  const root = install({ 'kcd2s-server': 'old', 'runtime/a.cjs': 'a' });
  const updates = new ServerUpdates({
    install: root,
    version: () => '0.3.1',
    platform: 'linux',
    env: { KCDS_UPDATE_ORIGIN: ORIGIN },
    publicKeys: keys,
    fetch,
  });
  const { privateKey: forger } = crypto.generateKeyPairSync('ed25519');
  publish('server-linux-stable', '0.3.2', 1, { 'kcd2s-server': 'evil' }, forger);
  await assert.rejects(updates.check(), /signature does not verify/);

  publish('server-linux-stable', '0.3.2', 2, { 'kcd2s-server': 'new' });
  artifacts.set(sha(Buffer.from('new')), Buffer.from('nex'));
  await assert.rejects(updates.stage(), /does not match the signed release/);
  assert.equal(read(root, 'kcd2s-server'), 'old');

  publish('server-linux-stable', '0.3.2', 3, { 'runtime/a.cjs': 'b' });
  await assert.rejects(updates.stage(), /no server binary/);
});

test('off in Docker and outside a server package', async () => {
  const { keys, fetch } = feed();
  const packaged = install({ 'kcd2s-server': 'binary' });
  const docker = new ServerUpdates({
    install: packaged,
    platform: 'linux',
    env: { KCDS_DOCKER: '1' },
    publicKeys: keys,
    fetch,
  });
  assert.equal(docker.state().available, false);
  assert.match(docker.state().reason, /Docker/);
  await assert.rejects(docker.check(), /Docker/);
  const source = new ServerUpdates({
    install: install({ 'runtime/a.cjs': 'a' }),
    platform: 'linux',
    env: {},
    publicKeys: keys,
    fetch,
  });
  assert.match(source.state().reason, /server package/);
});

test('off on Windows: updates are for the Linux server package', async () => {
  const { keys, fetch, fetched } = feed();
  const windows = new ServerUpdates({
    install: install({ 'kcd2s-server.exe': 'binary', 'runtime/a.cjs': 'a' }),
    platform: 'win32',
    env: {},
    publicKeys: keys,
    fetch,
  });
  assert.equal(windows.state().available, false);
  assert.match(windows.state().reason, /Linux server/);
  await assert.rejects(windows.check(), /Linux server/);
  await assert.rejects(windows.stage(), /Linux server/);
  assert.equal(windows.apply(), false);
  assert.deepEqual(fetched, [], 'the feed is never asked');
});
