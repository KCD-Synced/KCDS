'use strict';

// Sealed client files: the server's seal (resource_seal.cjs), the resource runtime's
// opening (Client/resources/sealed_files.cjs) and the game client's container
// (Client/runtime/assets/asset_container.cpp) agree byte for byte. test_fixtures/
// native_sealed.kcdsasset was sealed by the game client's kcds-asset-seal; the game client's own
// test (runtime.asset_container) opens a file this module sealed.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {
  seal,
  open,
  secretFor,
  stableSecretFor,
  serverSealKey,
  overhead,
} = require('./resource_seal.cjs');
const { openSealed } = require('../../../../Client/resources/sealed_files.cjs');
const { ResourceBundleStore, digest } = require('./bundles.cjs');
const { BundleCache } = require('../../../../Client/resources/bundle_cache.cjs');
const { normalizeManifest } = require('./manifest.cjs');

const FIXTURE = path.join(__dirname, 'test_fixtures', 'native_sealed.kcdsasset');
const RESOURCE = 'kcds-test-resource';
const SECRET = 'fixture-secret-0123456789';

test('a file the game client sealed opens on the server and in the resource runtime', async () => {
  const sealed = fs.readFileSync(FIXTURE);
  const want = 'KCDS sealed file test: čau, a few bytes of a resource file.\n';
  assert.equal(open(sealed, RESOURCE, SECRET).toString('utf8'), want);
  assert.equal(
    Buffer.from(await openSealed(new Uint8Array(sealed), RESOURCE, SECRET)).toString('utf8'),
    want,
  );
});

test('what the server seals the resource runtime opens; another secret, name or byte does not', async () => {
  const plain = Buffer.from('exports.secret = "only for this run";');
  const secret = secretFor();
  const sealed = seal(plain, 'market', secret);
  assert.equal(sealed.length, plain.length + overhead('market'));
  assert.ok(!sealed.includes(Buffer.from('only for this run')));
  assert.deepEqual(Buffer.from(await openSealed(new Uint8Array(sealed), 'market', secret)), plain);
  assert.equal(await openSealed(new Uint8Array(sealed), 'market', secretFor()), null);
  assert.equal(await openSealed(new Uint8Array(sealed), 'bazaar', secret), null);
  const changed = Buffer.from(sealed);
  changed[changed.length - 3] ^= 1;
  assert.equal(await openSealed(new Uint8Array(changed), 'market', secret), null);
  assert.equal(open(changed, 'market', secret), null);
  // One file seals to the same bytes (its hash stays for the download cache); another file gets
  // another nonce.
  assert.ok(seal(plain, 'market', secret).equals(sealed));
  const other = seal(Buffer.from('exports.secret = "another file";'), 'market', secret);
  assert.ok(!other.subarray(12, 24).equals(sealed.subarray(12, 24)));
});

test('the server seal key is kept in the resources folder and gives each resource a stable secret', (t) => {
  const root = fs.mkdtempSync(path.join(require('node:os').tmpdir(), 'kcds-seal-key-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const key = serverSealKey(root);
  assert.equal(key.length, 32);
  assert.match(fs.readFileSync(path.join(root, 'resource-seal.key'), 'utf8'), /^[0-9a-f]{64}\n$/);
  // A restart reads the same key.
  assert.ok(serverSealKey(root).equals(key));
  assert.equal(stableSecretFor(key, 'market'), stableSecretFor(serverSealKey(root), 'market'));
  assert.notEqual(stableSecretFor(key, 'market'), stableSecretFor(key, 'bazaar'));
  assert.notEqual(stableSecretFor(key, 'market'), stableSecretFor(Buffer.alloc(32, 7), 'market'));
  // A damaged key file is replaced and reported.
  fs.writeFileSync(path.join(root, 'resource-seal.key'), 'not a key');
  const told = [];
  const replaced = serverSealKey(root, (text) => told.push(text));
  assert.equal(replaced.length, 32);
  assert.ok(!replaced.equals(key));
  assert.equal(told.length, 1);
  assert.ok(serverSealKey(root).equals(replaced));
  // No folder: a key for this run only.
  assert.equal(serverSealKey('').length, 32);
});

test('with the server seal key a restarted server serves unchanged files under the same hashes', (t) => {
  const root = fs.mkdtempSync(path.join(require('node:os').tmpdir(), 'kcds-seal-restart-'));
  const folder = fs.mkdtempSync(path.join(require('node:os').tmpdir(), 'kcds-seal-market-'));
  t.after(() => {
    fs.rmSync(root, { recursive: true, force: true });
    fs.rmSync(folder, { recursive: true, force: true });
  });
  fs.writeFileSync(path.join(folder, 'client.js'), 'exports.price = 3;');
  fs.writeFileSync(path.join(folder, 'shop.js'), 'exports.open = true;');
  const manifest = normalizeManifest({
    name: 'market',
    kcds: { clientScripts: ['client.js', 'shop.js'] },
  });
  const hashes = (store) =>
    new Map(store.catalog.resources[0].files.map((file) => [file.name, file.hash]));
  const first = new ResourceBundleStore({}, serverSealKey(root));
  first.publish([{ folder, manifest }]);
  // The next run, and a new start of the resource: the same files, the same hashes.
  const second = new ResourceBundleStore({}, serverSealKey(root));
  second.publish([{ folder, manifest, generation: 1 }]);
  assert.deepEqual(hashes(second), hashes(first));
  assert.equal(second.catalog.resources[0].seal, first.catalog.resources[0].seal);
  // A changed file gets a new hash; the other keeps its own.
  fs.writeFileSync(path.join(folder, 'client.js'), 'exports.price = 4;');
  const third = new ResourceBundleStore({}, serverSealKey(root));
  third.publish([{ folder, manifest }]);
  assert.notEqual(hashes(third).get('client.js'), hashes(first).get('client.js'));
  assert.equal(hashes(third).get('shop.js'), hashes(first).get('shop.js'));
  // Without the key every run seals anew.
  const keyless = new ResourceBundleStore();
  keyless.publish([{ folder, manifest }]);
  assert.notEqual(hashes(keyless).get('shop.js'), hashes(first).get('shop.js'));
});

test('a join opens every file in memory; a file that does not open stops it', async (t) => {
  const folder = fs.mkdtempSync(path.join(require('node:os').tmpdir(), 'kcds-sealed-join-'));
  t.after(() => fs.rmSync(folder, { recursive: true, force: true }));
  fs.writeFileSync(path.join(folder, 'client.js'), 'exports.hello = "čau";');
  const manifest = normalizeManifest({ name: 'market', kcds: { clientScripts: ['client.js'] } });
  const server = new ResourceBundleStore();
  server.publish([{ folder, manifest }]);
  const download =
    (store) =>
    async ({ revision, hash, offset, length }) => {
      const chunk = store.chunk(revision, hash, offset, length);
      const bytes = Uint8Array.from(Buffer.from(chunk.data, 'base64'));
      return { ...chunk, bytes };
    };
  const client = new BundleCache(async (bytes) => digest(bytes));
  await client.acquire(server.manifest(), download(server));
  assert.equal(
    Buffer.from(client.read('market', 'client.js')).toString(),
    'exports.hello = "čau";',
  );
  // What the cache keeps is the sealed file, not the plain one.
  const kept = [...client.files.values()][0];
  assert.ok(!Buffer.from(kept).toString().includes('hello'));
  // A catalog whose secret does not open its files (another run's, say) is refused.
  const catalog = server.manifest();
  catalog.resources[0].seal = secretFor();
  const { hash, ...resource } = catalog.resources[0];
  catalog.resources[0].hash = digest(Buffer.from(JSON.stringify(resource)));
  catalog.hash = digest(Buffer.from(JSON.stringify(catalog.resources)));
  client.disconnect();
  await assert.rejects(
    client.acquire(catalog, download(server)),
    /Resource file did not open: market\/client\.js/,
  );
});
