'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceBundleStore, digest } = require('./bundles.cjs');
const { normalizeManifest } = require('./manifest.cjs');
const { BundleCache, DiskFile } = require('../../../../Client/resources/bundle_cache.cjs');

function setup(t) {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-acquire-'));
  t.after(() => fs.rmSync(folder, { recursive: true, force: true }));
  fs.writeFileSync(path.join(folder, 'client.js'), 'exports.message="čau";');
  const manifest = normalizeManifest({ name: 'mode', kcds: { clientScripts: ['client.js'] } });
  const server = new ResourceBundleStore({ chunkBytes: 4 });
  server.publish([{ folder, manifest }]);
  const client = new BundleCache(async (bytes) => digest(bytes), { chunkBytes: 4 });
  let requests = 0;
  const download = async ({ revision, hash, offset, length }) => {
    requests++;
    const chunk = server.chunk(revision, hash, offset, length);
    return { ...chunk, bytes: Uint8Array.from(Buffer.from(chunk.data, 'base64')) };
  };
  return { server, client, download, requests: () => requests };
}

test('client activates only fully verified downloaded files and reuses cache on reconnect', async (t) => {
  const f = setup(t);
  await f.client.acquire(f.server.manifest(), f.download);
  assert.equal(
    new TextDecoder().decode(f.client.read('mode', 'client.js')),
    'exports.message="čau";',
  );
  const requests = f.requests();
  f.client.disconnect();
  assert.throws(() => f.client.read('mode', 'client.js'));
  await f.client.acquire(f.server.manifest(), f.download);
  assert.equal(f.requests(), requests);
  const copy = f.client.read('mode', 'client.js');
  copy.fill(0);
  assert.notEqual(f.client.read('mode', 'client.js')[0], 0);
});

test('corrupt content is rejected without replacing an existing working activation', async (t) => {
  const f = setup(t);
  await f.client.acquire(f.server.manifest(), f.download);
  const original = f.client.active;
  f.client.files.clear();
  await assert.rejects(
    f.client.acquire(f.server.manifest(), async (request) => {
      const chunk = await f.download(request);
      chunk.bytes.fill(0);
      return chunk;
    }),
    /hash mismatch/,
  );
  assert.equal(f.client.active, original);
});

test('wrong chunk offsets, revisions and lengths cannot activate a resource', async (t) => {
  for (const patch of [{ offset: 99 }, { revision: 99 }, { bytes: new Uint8Array(0) }]) {
    const f = setup(t);
    await assert.rejects(
      f.client.acquire(f.server.manifest(), async (request) => ({
        ...(await f.download(request)),
        ...patch,
      })),
      /Invalid resource download/,
    );
    assert.equal(f.client.active, null);
  }
});

test('disconnect cancels an in-flight download and prevents stale activation', async (t) => {
  const f = setup(t);
  let release;
  let entered;
  const ready = new Promise((resolve) => {
    entered = resolve;
  });
  const pending = f.client.acquire(f.server.manifest(), async (request) => {
    entered();
    await new Promise((resolve) => {
      release = resolve;
    });
    return f.download(request);
  });
  await ready;
  f.client.disconnect();
  release();
  await assert.rejects(pending, /session changed/);
  assert.equal(f.client.active, null);
});

test('catalog changes and malicious file paths are rejected before downloading', async (t) => {
  const f = setup(t);
  const changed = f.server.manifest();
  changed.resources[0].manifest.version = 'changed';
  await assert.rejects(f.client.acquire(changed, f.download), /hash mismatch/);
  for (const name of ['../private.key', '/private.key', 'C:/private.key', 'a\\b.js', 'a//b.js']) {
    const catalog = f.server.manifest();
    catalog.resources[0].files[0].name = name;
    await assert.rejects(f.client.acquire(catalog, f.download), /Invalid client resource file/);
  }
  assert.equal(f.requests(), 0);
});

test('acquire reports resources and bytes as they are verified, cached files included', async (t) => {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-progress-'));
  t.after(() => fs.rmSync(folder, { recursive: true, force: true }));
  const resources = ['alpha', 'beta'].map((name) => {
    const dir = path.join(folder, name);
    fs.mkdirSync(dir);
    fs.writeFileSync(path.join(dir, 'client.js'), `exports.name = '${name}';`);
    return {
      folder: dir,
      manifest: normalizeManifest({ name, kcds: { clientScripts: ['client.js'] } }),
    };
  });
  const server = new ResourceBundleStore({ chunkBytes: 4 });
  server.publish(resources);
  const client = new BundleCache(async (bytes) => digest(bytes), { chunkBytes: 4 });
  const download = async ({ revision, hash, offset, length }) => {
    const chunk = server.chunk(revision, hash, offset, length);
    return { ...chunk, bytes: Uint8Array.from(Buffer.from(chunk.data, 'base64')) };
  };
  const reports = [];
  await client.acquire(server.manifest(), download, (progress) => reports.push(progress));
  // The bytes that travel: the sealed files, as the catalog sizes them.
  const bytesTotal = server
    .manifest()
    .resources.flatMap((resource) => resource.files)
    .reduce((sum, file) => sum + file.size, 0);
  // Each opens to its plain client file.
  assert.equal(
    Buffer.from(client.read('alpha', 'client.js')).toString(),
    "exports.name = 'alpha';",
  );
  assert.deepEqual(reports[0], { resourcesTotal: 2, bytesTotal, resourcesDone: 0, bytesDone: 0 });
  assert.deepEqual(reports.at(-1), {
    resourcesTotal: 2,
    bytesTotal,
    resourcesDone: 2,
    bytesDone: bytesTotal,
  });
  // Monotonic, and one resource is reported done before the second finishes.
  for (let i = 1; i < reports.length; i++) {
    assert.ok(reports[i].bytesDone >= reports[i - 1].bytesDone);
    assert.ok(reports[i].resourcesDone >= reports[i - 1].resourcesDone);
  }
  assert.ok(reports.some((report) => report.resourcesDone === 1 && report.bytesDone < bytesTotal));

  // A reconnect with everything cached reports completion without downloading.
  client.disconnect();
  const again = [];
  await client.acquire(server.manifest(), download, (progress) => again.push(progress));
  assert.deepEqual(again.at(-1), {
    resourcesTotal: 2,
    bytesTotal,
    resourcesDone: 2,
    bytesDone: bytesTotal,
  });
});

test('a loading page and its files arrive first, and the page is told while the rest is missing', async (t) => {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-loading-'));
  t.after(() => fs.rmSync(folder, { recursive: true, force: true }));
  fs.mkdirSync(path.join(folder, 'ui', 'loading'), { recursive: true });
  fs.writeFileSync(path.join(folder, 'a-first-by-name.bin'), 'the rest');
  fs.writeFileSync(path.join(folder, 'client.js'), 'exports.x = 1;');
  fs.writeFileSync(path.join(folder, 'ui', 'page.html'), '<p>page</p>');
  fs.writeFileSync(path.join(folder, 'ui', 'loading', 'art.png'), 'art');
  const manifest = normalizeManifest({
    name: 'mode',
    kcds: {
      clientScripts: ['client.js'],
      files: ['*.bin'],
      loadingScreen: { page: 'ui/page.html', files: ['ui/loading/**'] },
    },
  });
  const server = new ResourceBundleStore({ chunkBytes: 64 });
  server.publish([{ folder, manifest }]);
  const client = new BundleCache(async (bytes) => digest(bytes), { chunkBytes: 64 });
  const asked = [];
  let told = null;
  await client.acquire(
    server.manifest(),
    async ({ revision, hash, offset, length }) => {
      asked.push(hash);
      const chunk = server.chunk(revision, hash, offset, length);
      return { ...chunk, bytes: Uint8Array.from(Buffer.from(chunk.data, 'base64')) };
    },
    () => {},
    (page) => {
      told = { page, askedThen: asked.length };
    },
  );
  assert.equal(told.page.resource, 'mode');
  assert.equal(told.page.page, 'ui/page.html');
  assert.equal(told.askedThen, 2, 'only the page and its picture came before');
  assert.equal(Buffer.from(told.page.read('ui/loading/art.png')).toString(), 'art');
  assert.equal(told.page.read('client.js'), null);
  assert.equal(asked.length, 4);
});

test('a loading page over the size limit is dropped, and the resource still goes out', (t) => {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-loading-'));
  t.after(() => fs.rmSync(folder, { recursive: true, force: true }));
  fs.writeFileSync(path.join(folder, 'page.html'), Buffer.alloc(64));
  const manifest = normalizeManifest({ name: 'mode', kcds: { loadingScreen: 'page.html' } });
  const server = new ResourceBundleStore({ loadingScreenBytes: 16 });
  server.publish([{ folder, manifest }]);
  const [resource] = server.manifest().resources;
  assert.equal(resource.manifest.loadingScreen, undefined);
  assert.deepEqual(
    resource.files.map((file) => file.name),
    ['page.html'],
  );
});

test('a resource with a file over the limit is left out alone, with an error', (t) => {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-limit-'));
  t.after(() => fs.rmSync(folder, { recursive: true, force: true }));
  const make = (name, bytes) => {
    const own = path.join(folder, name);
    fs.mkdirSync(own);
    fs.writeFileSync(path.join(own, 'client.js'), Buffer.alloc(bytes, 32));
    return {
      folder: own,
      manifest: normalizeManifest({ name, kcds: { clientScripts: ['client.js'] } }),
    };
  };
  const server = new ResourceBundleStore({ fileBytes: 64 });
  const errors = [];
  server.report = (text) => errors.push(text);
  assert.equal(server.publish([make('big', 128), make('small', 8)]), true);
  assert.deepEqual(
    server.manifest().resources.map((resource) => resource.manifest.name),
    ['small'],
  );
  assert.equal(errors.length, 1);
  assert.match(errors[0], /^big: not sent to players: .*limit/);
});

test('a big file stays in the game cache and is handed out by its hash', async (t) => {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-acquire-'));
  t.after(() => fs.rmSync(folder, { recursive: true, force: true }));
  fs.writeFileSync(path.join(folder, 'client.js'), 'exports.big = true; // a script stays here');
  fs.writeFileSync(path.join(folder, 'movie.bin'), Buffer.alloc(64, 7));
  const manifest = normalizeManifest({
    name: 'mode',
    kcds: { clientScripts: ['client.js'], files: ['client.js', 'movie.bin'] },
  });
  // Over 32 bytes is big on both sides: the server sends it unsealed (`plain`), and only such a
  // file may stay in the game's cache.
  const server = new ResourceBundleStore({ chunkBytes: 16, inMemoryBytes: 32 });
  server.publish([{ folder, manifest }]);
  const client = new BundleCache(async (bytes) => digest(bytes), {
    chunkBytes: 16,
    inMemoryBytes: 32,
  });
  const chunked = new Set();
  const download = async ({ revision, hash, offset, length }) => {
    chunked.add(hash);
    const chunk = server.chunk(revision, hash, offset, length);
    return { ...chunk, bytes: Uint8Array.from(Buffer.from(chunk.data, 'base64')) };
  };
  const ensured = [];
  await client.acquire(
    server.manifest(),
    download,
    () => {},
    null,
    async ({ hash, size }) => {
      ensured.push({ hash, size });
      return { ok: true };
    },
  );
  const big = client.read('mode', 'movie.bin');
  assert.equal(big instanceof DiskFile, true);
  assert.equal(big.length, 64);
  assert.deepEqual(ensured, [{ hash: big.hash, size: 64 }]);
  assert.equal(chunked.has(big.hash), false);
  // The script is held here whatever its size.
  assert.match(new TextDecoder().decode(client.read('mode', 'client.js')), /big = true/);

  // A game that cannot keep it (no HTTP files) takes it in chunks while it is small enough.
  const fallback = new BundleCache(async (bytes) => digest(bytes), {
    chunkBytes: 16,
    inMemoryBytes: 32,
  });
  await fallback.acquire(
    server.manifest(),
    download,
    () => {},
    null,
    async () => ({ ok: false }),
  );
  assert.equal(fallback.read('mode', 'movie.bin').length, 64);
  assert.equal(fallback.read('mode', 'movie.bin') instanceof Uint8Array, true);

  // Larger than that, the resource cannot go out without the server's HTTP files.
  const refused = new BundleCache(async (bytes) => digest(bytes), {
    chunkBytes: 16,
    inMemoryBytes: 32,
    chunkedFileBytes: 48,
  });
  await assert.rejects(
    refused.acquire(
      server.manifest(),
      download,
      () => {},
      null,
      async () => ({ ok: false }),
    ),
    /needs the server's HTTP files/,
  );
});
