'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http');
const { ResourceFileServer } = require('./web/file_server.cjs');

const HASH = 'a'.repeat(64);

function get(port, path, method = 'GET', token = null) {
  return new Promise((resolve, reject) => {
    const headers = token ? { 'x-kcds-token': token } : {};
    const request = http.request({ host: '127.0.0.1', port, path, method, headers }, (response) => {
      const chunks = [];
      response.on('data', (chunk) => chunks.push(chunk));
      response.on('end', () =>
        resolve({
          status: response.statusCode,
          headers: response.headers,
          body: Buffer.concat(chunks),
        }),
      );
    });
    request.on('error', reject);
    request.end();
  });
}

// the catalog's files by their hash on the game's port, nothing else.
test('the file server serves only the catalog files by their hash', async () => {
  const bytes = Buffer.from('client file bytes');
  const store = { blobs: new Map([[HASH, bytes]]) };
  const server = new ResourceFileServer({ store, port: 0, bind: '127.0.0.1' });
  // Port 0 is no port: the server stays off.
  await server.start();
  assert.equal(server.listening, false);
  server.port = 0;
  // A free port of this machine for the test.
  const probe = http.createServer();
  await new Promise((resolve) => probe.listen(0, '127.0.0.1', resolve));
  const port = probe.address().port;
  await new Promise((resolve) => probe.close(resolve));
  const live = new ResourceFileServer({ store, port, bind: '127.0.0.1' });
  await live.start();
  try {
    assert.equal(live.listening, true);
    // Only a connected player's game, with its own token, is answered.
    assert.equal((await get(port, `/kcds/files/${HASH}`)).status, 403);
    assert.equal((await get(port, `/kcds/files/${HASH}`, 'GET', 'x'.repeat(43))).status, 403);
    const token = live.tokenFor(7);
    assert.equal(live.tokenFor(7), token);
    assert.notEqual(live.tokenFor(8), token);
    const file = await get(port, `/kcds/files/${HASH}`, 'GET', token);
    assert.equal(file.status, 200);
    assert.deepEqual(file.body, bytes);
    assert.equal(file.headers['content-length'], String(bytes.length));
    assert.match(file.headers['cache-control'], /no-store/);
    const head = await get(port, `/kcds/files/${HASH}`, 'HEAD', token);
    assert.equal(head.status, 200);
    assert.equal(head.body.length, 0);
    assert.equal((await get(port, `/kcds/files/${'b'.repeat(64)}`, 'GET', token)).status, 404);
    assert.equal((await get(port, '/kcds/files/../secret')).status, 404);
    assert.equal((await get(port, `/kcds/files/${HASH}`, 'POST')).status, 404);
    // A refreshed catalog: its files only.
    store.blobs = new Map();
    assert.equal((await get(port, `/kcds/files/${HASH}`, 'GET', token)).status, 404);
    // The player left: its token is answered no more.
    store.blobs = new Map([[HASH, bytes]]);
    live.forget(7);
    assert.equal((await get(port, `/kcds/files/${HASH}`, 'GET', token)).status, 403);
  } finally {
    await live.close();
  }
  assert.equal(live.listening, false);
});

test('a big file is streamed from the disk while it is the one hashed', async (t) => {
  const fs = require('node:fs');
  const os = require('node:os');
  const path = require('node:path');
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-files-'));
  t.after(() => fs.rmSync(folder, { recursive: true, force: true }));
  const file = path.join(folder, 'movie.bin');
  fs.writeFileSync(file, Buffer.alloc(100000, 3));
  const info = fs.statSync(file);
  const { digest } = require('./bundles.cjs');
  const blob = {
    path: file,
    size: info.size,
    mtimeMs: info.mtimeMs,
    hash: digest(Buffer.alloc(100000, 3)),
    file: 'ui/movie.bin',
    resource: 'cinema',
  };
  const store = { blobs: new Map([[HASH, blob]]) };
  const probe = http.createServer();
  await new Promise((resolve) => probe.listen(0, '127.0.0.1', resolve));
  const port = probe.address().port;
  await new Promise((resolve) => probe.close(resolve));
  // What the owner is told about changed files (the server's start is logged too).
  const logged = [];
  const live = new ResourceFileServer({
    store,
    port,
    bind: '127.0.0.1',
    log: (text) => {
      if (text.includes('changed on the disk')) {
        logged.push(text);
      }
    },
  });
  await live.start();
  try {
    const token = live.tokenFor(1);
    const answer = await get(port, `/kcds/files/${HASH}`, 'GET', token);
    assert.equal(answer.status, 200);
    assert.deepEqual(answer.body, Buffer.alloc(100000, 3));
    // Touched, or written again with the same bytes (a git pull, a deploy): still served, and the
    // new time is remembered.
    const later = new Date(Date.now() + 60000);
    fs.utimesSync(file, later, later);
    assert.equal((await get(port, `/kcds/files/${HASH}`, 'GET', token)).status, 200);
    assert.equal(blob.mtimeMs, fs.statSync(file).mtimeMs);
    fs.writeFileSync(file, Buffer.alloc(100000, 3));
    assert.equal((await get(port, `/kcds/files/${HASH}`, 'GET', token)).status, 200);
    assert.equal(logged.length, 0);
    // Other bytes of the same size, or another size: refused, and the owner is told once which
    // resource to start again.
    fs.writeFileSync(file, Buffer.alloc(100000, 4));
    assert.equal((await get(port, `/kcds/files/${HASH}`, 'GET', token)).status, 410);
    fs.writeFileSync(file, Buffer.alloc(10, 4));
    assert.equal((await get(port, `/kcds/files/${HASH}`, 'GET', token)).status, 410);
    assert.equal(logged.length, 1);
    assert.match(logged[0], /ui\/movie\.bin of resource cinema .*ensure cinema/);
  } finally {
    await live.close();
  }
});
