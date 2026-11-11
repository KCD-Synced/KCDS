'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path'),
  vm = require('node:vm');
const { ResourceHost } = require('./host.cjs');
const { BrowserResourceRuntime } = require('../../../../Client/resources/runtime.cjs');
const { normalizeManifest } = require('./manifest.cjs');

// The shared fixture exercises the timer, console and Runtime
// contract with no Node APIs, so the exact same assertions run inside the
// trusted server vm and inside the browser client runtime.
const fixturePath = path.resolve(__dirname, '../../../../Dev/testkit/timer_contract.cjs');
const verifyTimerContract = require(fixturePath);

test('server resource vm satisfies the shared timer/console/environment contract', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-contract-'));
  // A real server gives its KCDS version, which apiVersion carries.
  const host = new ResourceHost(root, { send() {}, log() {}, version: '1.0.5' });
  const dir = path.join(root, 'contract');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'contract', kcds: { serverScripts: ['index.js'], exports: ['run'] } }),
  );
  fs.writeFileSync(
    path.join(dir, 'index.js'),
    `const verify = require(${JSON.stringify(fixturePath)});
     kcds.core.Exports.register('run', () =>
       verify({ setTimeout, setInterval, clearTimeout, clearInterval, queueMicrotask, console, kcds }));`,
  );
  try {
    await host.startAll();
    const sender = host.services.facade(host.services.owners.get('contract'));
    const checks = await sender.Exports.get('contract', 'run')();
    assert.ok(checks >= 20, `expected contract checks, saw ${checks}`);
    const environment = host.services.environment;
    assert.equal(environment.isServer, true);
    assert.equal(environment.isClient, false);
  } finally {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('browser client runtime satisfies the shared timer/console/environment contract', async (t) => {
  let captured = null;
  const runtime = new BrowserResourceRuntime({
    version: '1.0.5',
    send() {},
    evaluate(code, globals, module, localRequire, file) {
      captured = globals;
      const context = vm.createContext({
        ...globals,
        module,
        require: localRequire,
        exports: module.exports,
      });
      new vm.Script(code, { filename: file }).runInContext(context, { timeout: 1000 });
    },
  });
  t.after(() => runtime.disconnect());
  const files = new Map([['contract/main.js', new TextEncoder().encode('')]]);
  const definition = {
    hash: 'contract',
    manifest: normalizeManifest({ name: 'contract', kcds: { clientScripts: ['main.js'] } }),
    files: [{ name: 'main.js' }],
  };
  const read = (name, file) => files.get(name + '/' + file);
  await runtime.apply({ resources: [definition] }, read);
  assert.ok(captured, 'client runtime exposed resource globals');
  const checks = await verifyTimerContract(captured);
  assert.ok(checks >= 20, `expected contract checks, saw ${checks}`);
  assert.equal(captured.kcds.core.Runtime.isClient, true);
  assert.equal(captured.kcds.core.Runtime.isServer, false);
});
