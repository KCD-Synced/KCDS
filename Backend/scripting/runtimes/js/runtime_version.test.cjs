'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const { ResourceHost } = require('./host.cjs');
const { BrowserResourceRuntime } = require('../../../../Client/resources/runtime.cjs');

test('the server scripts read the KCDS version the native server gave', () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-version-'));
  const host = new ResourceHost(root, { send() {}, log() {}, version: '1.0.5' });
  assert.equal(host.services.environment.serverVersion, '1.0.5');
  assert.equal(host.services.environment.apiVersion, '1.0.5');
  // A server that gave none: both empty, not a made-up version.
  const unknown = new ResourceHost(root, { send() {}, log() {} });
  assert.equal(unknown.services.environment.serverVersion, '');
  assert.equal(unknown.services.environment.apiVersion, '');
});

test("client scripts read their game client's version", () => {
  const runtime = new BrowserResourceRuntime({ version: '1.0.5' });
  assert.equal(runtime.services.environment.serverVersion, '1.0.5');
  assert.equal(runtime.services.environment.apiVersion, '1.0.5');
});

test('the client bundle is stamped with the version it was built for', () => {
  const out = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-bundle-')), 'page.hpp');
  const script = path.resolve(__dirname, '../../../../Dev/tools/build_resource_runtime.py');
  execFileSync('python', [script, out, '--version', '1.0.5']);
  // The page holds the worker's script as a JSON string, cut into raw-string pieces.
  const page = fs.readFileSync(out, 'utf8').split(')KCDS"\nR"KCDS(').join('');
  assert.ok(page.includes('const KCDS_BUILD_VERSION = \\"1.0.5\\";'));
});
