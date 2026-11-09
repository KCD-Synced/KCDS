'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { normalizeManifest } = require('./manifest.cjs');
const { ResourceBundleStore, digest } = require('./bundles.cjs');
const { open } = require('./resource_seal.cjs');

function fixture(t, options = {}) {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-bundle-'));
  t.after(() => fs.rmSync(folder, { recursive: true, force: true }));
  fs.mkdirSync(path.join(folder, 'client'));
  fs.mkdirSync(path.join(folder, 'server'));
  fs.writeFileSync(path.join(folder, 'client/main.js'), 'console.log("čau");');
  fs.writeFileSync(path.join(folder, 'shared.js'), 'exports.answer=42;');
  fs.writeFileSync(path.join(folder, 'server/main.js'), 'SECRET SERVER IMPLEMENTATION');
  fs.writeFileSync(path.join(folder, 'server/helper.js'), 'PRIVATE SERVER IMPORT');
  fs.writeFileSync(path.join(folder, 'private.key'), 'SERVER SECRET');
  const source = {
    name: 'test',
    password: 'private-host-password',
    dependencies: { 'server-private-module': '1' },
    kcds: {
      serverScripts: ['server/main.js'],
      clientScripts: ['client/main.js'],
      sharedScripts: ['shared.js'],
      ...options,
    },
  };
  fs.writeFileSync(path.join(folder, 'package.json'), JSON.stringify(source));
  return { folder, manifest: normalizeManifest(source) };
}

test('catalog ships selected client/shared files and sanitized metadata, never server paths', (t) => {
  const definition = fixture(t);
  const store = new ResourceBundleStore();
  assert.equal(store.publish([definition]), true);
  const catalog = store.manifest();
  const resource = catalog.resources[0];
  assert.deepEqual(
    resource.files.map((file) => file.name),
    ['client/main.js', 'shared.js'],
  );
  assert.deepEqual(resource.manifest.kcds.serverScripts, []);
  assert.equal(resource.manifest.password, undefined);
  assert.equal(resource.manifest.dependencies, undefined);
  assert.equal(JSON.stringify(catalog).includes('server/main.js'), false);
  assert.throws(() => store.chunk(1, digest(Buffer.from('SERVER SECRET')), 0));
  assert.throws(() => store.chunk(1, '../../private.key', 0));
});

test('chunked download reconstructs exact bytes and hash, preserving UTF-8', (t) => {
  const store = new ResourceBundleStore({ chunkBytes: 5 });
  store.publish([fixture(t)]);
  const { revision, resources } = store.manifest();
  const file = resources[0].files.find((entry) => entry.name === 'client/main.js');
  const chunks = [];
  for (let offset = 0; offset < file.size; offset += 5) {
    const chunk = store.chunk(revision, file.hash, offset);
    assert.equal(chunk.offset, offset);
    chunks.push(Buffer.from(chunk.data, 'base64'));
  }
  const bytes = Buffer.concat(chunks);
  assert.equal(digest(bytes), file.hash);
  // Sealed with the resource's secret: the plain file is not what is sent.
  assert.ok(!bytes.toString().includes('console.log'));
  assert.equal(open(bytes, 'test', resources[0].seal).toString(), 'console.log("čau");');
  assert.throws(() => store.chunk(revision, file.hash, -1));
  assert.throws(() => store.chunk(revision, file.hash, file.size + 1));
  assert.throws(() => store.chunk(revision, file.hash, 0, 6));
});

test('each resource run seals its files with a secret of its own, kept while it runs', (t) => {
  const definition = fixture(t);
  const store = new ResourceBundleStore();
  store.publish([definition]);
  const first = store.manifest().resources[0];
  assert.match(first.seal, /^[A-Za-z0-9_-]{43}$/);
  // A refresh of the same run: the same secret and sealed files, nothing new to download.
  fs.writeFileSync(path.join(definition.folder, 'shared.js'), 'exports.answer=43;');
  store.publish([definition]);
  const refreshed = store.manifest().resources[0];
  assert.equal(refreshed.seal, first.seal);
  const main = (resource) => resource.files.find((file) => file.name === 'client/main.js').hash;
  assert.equal(main(refreshed), main(first));
  // The resource started again (a new generation): a new secret, every file sealed anew.
  store.publish([{ ...definition, generation: 1 }]);
  const restarted = store.manifest().resources[0];
  assert.notEqual(restarted.seal, first.seal);
  assert.notEqual(main(restarted), main(first));
  const bytes = Buffer.concat(
    Array.from({ length: Math.ceil(restarted.files[0].size / 16384) }, (_, index) =>
      Buffer.from(
        store.chunk(store.revision, restarted.files[0].hash, index * 16384).data,
        'base64',
      ),
    ),
  );
  // Only that run's secret opens it.
  assert.equal(open(bytes, 'test', first.seal), null);
  assert.ok(open(bytes, 'test', restarted.seal));
});

test('unchanged refresh preserves revision; changed files invalidate stale download requests', (t) => {
  const definition = fixture(t);
  const store = new ResourceBundleStore();
  store.publish([definition]);
  const before = store.manifest();
  assert.equal(store.publish([definition]), false);
  assert.equal(store.revision, 1);
  before.resources.length = 0;
  assert.equal(store.manifest().resources.length, 1);
  const file = store.manifest().resources[0].files[0];
  fs.writeFileSync(path.join(definition.folder, file.name), 'exports.changed=true;');
  assert.equal(store.publish([definition]), true);
  assert.equal(store.revision, 2);
  assert.throws(() => store.chunk(1, file.hash, 0));
  assert.throws(() => store.chunk(2, file.hash, 0));
});

test('failed or oversized refresh leaves the working catalog intact', (t) => {
  const definition = fixture(t);
  const store = new ResourceBundleStore({ fileBytes: 100 });
  store.publish([definition]);
  const before = store.manifest();
  fs.writeFileSync(path.join(definition.folder, 'client/main.js'), 'x'.repeat(101));
  const errors = [];
  store.report = (text) => errors.push(text);
  assert.equal(store.publish([definition]), false);
  assert.match(errors[0], /not updated: .*limit/);
  assert.deepEqual(store.manifest(), before);
  assert.equal(
    store.chunk(1, before.resources[0].files[0].hash, 0).size,
    before.resources[0].files[0].size,
  );
});

test('TypeScript entries must be compiled and server-only files remain excluded even under globs', (t) => {
  const definition = fixture(t, { files: ['**/*.js', 'package.json'] });
  const store = new ResourceBundleStore();
  store.publish([definition]);
  assert.equal(
    store.manifest().resources[0].files.some((file) => file.name.startsWith('server/')),
    false,
  );
  assert.equal(
    store.manifest().resources[0].files.some((file) => file.name === 'package.json'),
    false,
  );
  definition.manifest.kcds.clientScripts = ['client/main.ts'];
  const errors = [];
  store.report = (text) => errors.push(text);
  store.publish([definition]);
  assert.match(errors[0], /not updated: Compile/);
});

test('a Lua client side ships its .lua scripts and modules to players', (t) => {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-bundle-lua-'));
  t.after(() => fs.rmSync(folder, { recursive: true, force: true }));
  fs.mkdirSync(path.join(folder, 'client'));
  fs.writeFileSync(path.join(folder, 'client/main.lua'), 'print(require("client.util").x)');
  fs.writeFileSync(path.join(folder, 'client/util.lua'), 'return { x = 1 }');
  fs.writeFileSync(path.join(folder, 'server.lua'), 'SERVER ONLY');
  const source = { name: 'lua', kcds: { server: 'server.lua', client: 'client/main.lua' } };
  const store = new ResourceBundleStore();
  const errors = [];
  store.report = (text) => errors.push(text);
  assert.equal(store.publish([{ folder, manifest: normalizeManifest(source) }]), true);
  assert.deepEqual(errors, []);
  const resource = store.manifest().resources[0];
  assert.deepEqual(
    resource.files.map((file) => file.name),
    ['client/main.lua', 'client/util.lua'],
  );
  // Players learn their side's language, never the server's.
  assert.deepEqual(resource.manifest.kcds.language, { client: 'lua' });
});

test('catalog rejects duplicate resources and excessive aggregate size', (t) => {
  const definition = fixture(t);
  assert.throws(() => new ResourceBundleStore().publish([definition, definition]), /Duplicate/);
  {
    const errors = [];
    const store = new ResourceBundleStore({ totalBytes: 1 });
    store.report = (text) => errors.push(text);
    store.publish([definition]);
    assert.equal(store.manifest().resources.length, 0);
    assert.match(errors[0], /not sent to players/);
  }
  assert.throws(() => new ResourceBundleStore({ resources: 0 }).publish([definition]), /limit/);
  {
    const errors = [];
    const store = new ResourceBundleStore({ files: 0 });
    store.report = (text) => errors.push(text);
    store.publish([definition]);
    assert.equal(store.manifest().resources.length, 0);
    assert.match(errors[0], /not sent to players/);
  }
  assert.throws(() => new ResourceBundleStore({ catalogBytes: 1 }).publish([definition]), /limit/);
});

// A big file read over the game connection: served while its bytes are the hashed ones, even
// touched or written again; refused with the resource to start again once they differ.
test('a big file touched on the disk is still read, a changed one names its resource', (t) => {
  const { readBlob } = require('./bundles.cjs');
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-blob-'));
  t.after(() => fs.rmSync(folder, { recursive: true, force: true }));
  const file = path.join(folder, 'map.bin');
  const bytes = Buffer.alloc(5000, 7);
  fs.writeFileSync(file, bytes);
  const info = fs.statSync(file);
  const blob = {
    path: file,
    size: info.size,
    mtimeMs: info.mtimeMs,
    hash: digest(bytes),
    file: 'ui/map.bin',
    resource: 'atlas',
  };
  assert.deepEqual(readBlob(blob, 0, 10), bytes.subarray(0, 10));
  const later = new Date(Date.now() + 60000);
  fs.utimesSync(file, later, later);
  assert.deepEqual(readBlob(blob, 10, 10), bytes.subarray(10, 20));
  assert.equal(blob.mtimeMs, fs.statSync(file).mtimeMs);
  fs.writeFileSync(file, Buffer.alloc(5000, 8));
  assert.throws(() => readBlob(blob, 0, 10), /ui\/map\.bin of resource atlas .*ensure atlas/);
});
