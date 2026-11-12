'use strict';

// The platform packages of kcds-bots: one per system and processor, found by @kcds/bots.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { packPlatform } = require('../platforms/pack-platform.cjs');
const { locateEngine } = require('../src/engine/locate.cjs');

test('a platform package holds its engine and installs only on its system', () => {
  const work = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-bots-pack-'));
  const engine = path.join(work, 'engine');
  fs.writeFileSync(engine, 'engine');
  const mac = packPlatform({
    engine,
    platform: 'darwin',
    arch: 'arm64',
    version: '1.0.6',
    out: work,
  });
  const manifest = JSON.parse(fs.readFileSync(path.join(mac.folder, 'package.json'), 'utf8'));
  assert.equal(manifest.name, '@kcds/bots-darwin-arm64');
  assert.deepEqual(manifest.os, ['darwin']);
  assert.deepEqual(manifest.cpu, ['arm64']);
  assert.ok(fs.existsSync(path.join(mac.folder, 'bin', 'kcds-bots')));
  const windows = packPlatform({
    engine,
    platform: 'win32',
    arch: 'x64',
    version: '1.0.6',
    out: work,
  });
  assert.ok(fs.existsSync(path.join(windows.folder, 'bin', 'kcds-bots.exe')));
  assert.throws(() =>
    packPlatform({ engine, platform: 'beos', arch: 'x64', version: '1', out: work }),
  );
  assert.throws(
    () => packPlatform({ engine, platform: 'linux', arch: 'x64', version: 'one', out: work }),
    /semver/,
  );
  // @kcds/bots finds the engine of the installed platform package.
  const own = packPlatform({
    engine,
    platform: process.platform === 'win32' ? 'win32' : process.platform,
    arch: process.arch === 'arm64' ? 'arm64' : 'x64',
    version: '1.0.6',
    out: work,
  });
  const found = locateEngine({
    env: {},
    resolve: (request) => {
      assert.match(request, /^@kcds\/bots-/);
      return path.join(own.folder, 'package.json');
    },
  });
  assert.equal(path.dirname(found), path.join(own.folder, 'bin'));
  fs.rmSync(work, { recursive: true, force: true });
});
