'use strict';

// Acceptance coverage (EventBus local and routed events). These
// tests drive the real server ResourceHost and the real client
// BrowserResourceRuntime so delivery, registration ordering, targeted and
// private scoping, and callback ownership cleanup across stop/reload are
// exercised through the production facades rather than a stub. Resource code
// runs in an isolated VM context, so each handler reports by emitting a tagged
// network event; the captured outbound ops give a deterministic, ordered trace
// that also proves payloads are serialized scalar text (never a pointer).

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');

const { ResourceHost } = require('./host.cjs');
const { normalizeManifest } = require('./manifest.cjs');
const { BrowserResourceRuntime } = require('../../../../Client/resources/runtime.cjs');

function serverFixture() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcd2s-events-'));
  const output = [];
  const logs = [];
  const host = new ResourceHost(root, {
    send: (event) => output.push(event),
    log: (...entry) => logs.push(entry),
  });
  return { root, output, logs, host };
}

function writeResource(root, name, code, extra = {}) {
  const dir = path.join(root, name);
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name, kcds: { serverScripts: ['index.js'], ...extra } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
}

function emittedNames(output) {
  return output.filter((event) => event.op === 'event').map((event) => event.name);
}

test('server EventBus: ordered local delivery across resources and bounded outbound event', async () => {
  const f = serverFixture();
  // Two handlers inside one resource, plus a handler in a second resource, must
  // all fire in registration order on a single shared broadcast.
  writeResource(
    f.root,
    'alpha',
    `
    kcds.core.Events.on('ping', () => kcds.core.Events.broadcast('alpha-1', { seq: 1 }));
    kcds.core.Events.on('ping', () => kcds.core.Events.broadcast('alpha-2'));
    `,
  );
  writeResource(
    f.root,
    'beta',
    `kcds.core.Events.on('ping', () => kcds.core.Events.broadcast('beta'));`,
  );
  await f.host.startAll();

  await f.host.emitAsync('ping');
  assert.deepEqual(emittedNames(f.output), ['alpha-1', 'alpha-2', 'beta']);

  // Networked payloads are serialized scalar text, never a live object/pointer.
  const first = f.output.find((event) => event.name === 'alpha-1');
  assert.equal(typeof first.payload, 'string');
  assert.deepEqual(JSON.parse(first.payload), { seq: 1 });

  await f.host.shutdown();
});

test('server veto: only a synchronous false refuses, and every handler still runs in order', async () => {
  const f = serverFixture();
  writeResource(
    f.root,
    'first',
    `
    kcds.core.Events.on('doorInteract', () => { kcds.core.Events.broadcast('first-async'); return Promise.resolve(false); });
    kcds.core.Events.on('doorInteract', () => { kcds.core.Events.broadcast('first-refuses'); return false; });
    `,
  );
  writeResource(
    f.root,
    'second',
    `kcds.core.Events.on('doorInteract', () => { kcds.core.Events.broadcast('second-runs'); return true; });`,
  );
  await f.host.startAll();
  assert.equal(f.host.emit('doorInteract'), false);
  assert.deepEqual(emittedNames(f.output), ['first-async', 'first-refuses', 'second-runs']);
  // Without the synchronous refusal a promise of false refuses nothing.
  await f.host.stop('first');
  writeResource(f.root, 'first', `kcds.core.Events.on('doorInteract', async () => false);`);
  await f.host.start('first');
  assert.equal(f.host.emit('doorInteract'), true);
  await f.host.shutdown();
});

test('server EventBus: toResource targets one resource and fire stays private', async () => {
  const f = serverFixture();
  writeResource(
    f.root,
    'router',
    `
    kcds.core.Events.on('route', () => kcds.core.Events.toResource('target', 'job'));
    kcds.core.Events.on('job', () => kcds.core.Events.broadcast('router-job'));
    kcds.core.Events.listen('secret', () => kcds.core.Events.broadcast('router-secret'));
    kcds.core.Events.on('fire', () => kcds.core.Events.fire('secret'));
    `,
  );
  writeResource(
    f.root,
    'target',
    `
    kcds.core.Events.on('job', () => kcds.core.Events.broadcast('target-job'));
    kcds.core.Events.listen('secret', () => kcds.core.Events.broadcast('target-secret'));
    `,
  );
  await f.host.startAll();

  // toResource('target', ...) must reach only the target resource's handler.
  await f.host.emitAsync('route');
  assert.deepEqual(emittedNames(f.output), ['target-job']);

  // fire fires only the emitting resource's own private handler.
  f.output.length = 0;
  await f.host.emitAsync('fire');
  assert.deepEqual(emittedNames(f.output), ['router-secret']);

  await f.host.shutdown();
});

test('server EventBus: callbacks are released on stop and reload leaves no stale handler', async () => {
  const f = serverFixture();
  writeResource(
    f.root,
    'worker',
    `kcds.core.Events.on('tick', () => kcds.core.Events.broadcast('v1'));`,
  );
  await f.host.startAll();
  await f.host.emitAsync('tick');
  assert.deepEqual(emittedNames(f.output), ['v1']);

  // Stop removes ownership: the handler must not fire afterwards.
  await f.host.stop('worker');
  f.output.length = 0;
  await f.host.emitAsync('tick');
  assert.deepEqual(emittedNames(f.output), [], 'stopped resource handler stayed registered');

  // Reload with changed code: exactly one fresh handler, no duplicate from v1.
  writeResource(
    f.root,
    'worker',
    `kcds.core.Events.on('tick', () => kcds.core.Events.broadcast('v2'));`,
  );
  f.host.refresh();
  await f.host.start('worker');
  f.output.length = 0;
  await f.host.emitAsync('tick');
  assert.deepEqual(emittedNames(f.output), ['v2'], 'reload must replace, not stack, handlers');

  await f.host.shutdown();
});

function clientFixture(t) {
  const messages = [];
  const files = new Map();
  const runtime = new BrowserResourceRuntime({
    send: (value) => messages.push(value),
    evaluate(code, globals, module, localRequire, file) {
      const context = vm.createContext({
        ...globals,
        module,
        require: localRequire,
        exports: module.exports,
      });
      new vm.Script(code, { filename: file }).runInContext(context, { timeout: 1000 });
    },
  });
  const resource = (name, source, hash = name) => {
    files.set(name + '/main.js', new TextEncoder().encode(source));
    return {
      hash,
      manifest: normalizeManifest({ name, kcds: { clientScripts: ['main.js'] } }),
      files: [{ name: 'main.js' }],
    };
  };
  const read = (name, file) => {
    const bytes = files.get(name + '/' + file);
    if (!bytes) throw new Error('Unknown file');
    return bytes;
  };
  t.after(() => runtime.disconnect());
  return { runtime, resource, read, messages };
}

test('client EventBus: ordered local delivery and bounded toServer network event', async (t) => {
  const f = clientFixture(t);
  const mode = f.resource(
    'mode',
    `
    kcds.core.Events.on('hit', () => kcds.core.Events.toServer('a'));
    kcds.core.Events.on('hit', () => kcds.core.Events.toServer('b'));
    kcds.core.Events.on('hit', () => kcds.core.Events.toServer('ack', { ok: true }));
    `,
  );
  await f.runtime.apply({ resources: [mode] }, f.read);
  await f.runtime.services.emit('shared', 'hit', []);

  assert.deepEqual(emittedNames(f.messages), ['a', 'b', 'ack']);
  const ack = f.messages.find((m) => m.name === 'ack');
  assert.equal(typeof ack.payload, 'string');
  assert.deepEqual(JSON.parse(ack.payload), { ok: true });
});

test('client EventBus: reload drops the previous callback owner', async (t) => {
  const f = clientFixture(t);
  await f.runtime.apply(
    {
      resources: [
        f.resource(
          'ui',
          `kcds.core.Events.on('refresh', () => kcds.core.Events.toServer('v1'));`,
          'h1',
        ),
      ],
    },
    f.read,
  );
  await f.runtime.services.emit('shared', 'refresh', []);
  assert.deepEqual(emittedNames(f.messages), ['v1']);

  f.messages.length = 0;
  await f.runtime.apply(
    {
      resources: [
        f.resource(
          'ui',
          `kcds.core.Events.on('refresh', () => kcds.core.Events.toServer('v2'));`,
          'h2',
        ),
      ],
    },
    f.read,
  );
  await f.runtime.services.emit('shared', 'refresh', []);
  assert.deepEqual(emittedNames(f.messages), ['v2'], 'old client handler must not survive reload');
});
