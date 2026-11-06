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
const { seal, open, secretFor, overhead } = require('./resource_seal.cjs');
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
  // Two seals of one file differ (a new nonce each time).
  assert.ok(!seal(plain, 'market', secret).equals(sealed));
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
