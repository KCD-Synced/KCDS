'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');
const { createRequire } = require('node:module');
const { webcrypto } = require('node:crypto');
const { ResourceBundleStore } = require('./bundles.cjs');
const { normalizeManifest } = require('./manifest.cjs');
const { BrowserResourceRuntime } = require('../../../../Client/resources/runtime.cjs');

function fixture(t, source = "kcds.core.Events.toServer('started');") {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-worker-session-'));
  fs.writeFileSync(path.join(folder, 'client.cjs'), source);
  const store = new ResourceBundleStore();
  store.publish([
    {
      folder,
      manifest: normalizeManifest({ name: 'session', kcds: { clientScripts: ['client.cjs'] } }),
    },
  ]);
  const messages = [];
  const timers = new Set();
  let runtime;
  let holdDownloads = false;
  let applyBarrier = null;
  const self = {
    postMessage(message) {
      messages.push(message);
      if (holdDownloads) return;
      if (message.op === 'catalogChunk') {
        queueMicrotask(() =>
          receive({
            op: 'catalogData',
            requestId: message.requestId,
            ...store.catalogChunk(message.revision, message.offset, message.length),
          }),
        );
      } else if (message.op === 'fileRequest') {
        queueMicrotask(() =>
          receive({
            op: 'fileData',
            requestId: message.requestId,
            ...store.chunk(message.revision, message.hash, message.offset, message.length),
          }),
        );
      }
    },
  };
  const workerPath = path.resolve(__dirname, '../../../../Client/resources/worker.cjs');
  const workerRequire = createRequire(workerPath);
  class TestRuntime extends BrowserResourceRuntime {
    constructor(options) {
      super({
        ...options,
        evaluate(code, globals, module, localRequire, file) {
          const context = vm.createContext({
            ...globals,
            module,
            exports: module.exports,
            require: localRequire,
          });
          new vm.Script(code, { filename: file }).runInContext(context, { timeout: 1000 });
        },
      });
      runtime = this;
    }
    async apply(...args) {
      await super.apply(...args);
      if (applyBarrier) await applyBarrier;
    }
  }
  const context = vm.createContext({
    self,
    crypto: webcrypto,
    TextEncoder,
    TextDecoder,
    Uint8Array,
    atob: globalThis.atob,
    setTimeout(callback, delay) {
      const timer = setTimeout(callback, delay);
      timers.add(timer);
      return timer;
    },
    clearTimeout(timer) {
      clearTimeout(timer);
      timers.delete(timer);
    },
    setInterval() {
      return 0;
    },
    require(name) {
      return name === './runtime.cjs'
        ? { BrowserResourceRuntime: TestRuntime }
        : workerRequire(name);
    },
  });
  new vm.Script(fs.readFileSync(workerPath, 'utf8'), { filename: workerPath }).runInContext(
    context,
  );
  function receive(data) {
    self.onmessage({ data });
  }
  const count = (op, name) =>
    messages.filter((message) => message.op === op && (!name || message.name === name)).length;
  async function wait(predicate) {
    const deadline = Date.now() + 2000;
    while (!predicate()) {
      if (Date.now() > deadline)
        throw new Error('Worker expectation timed out: ' + JSON.stringify(messages));
      await new Promise((resolve) => setImmediate(resolve));
    }
  }
  t.after(async () => {
    for (const timer of timers) clearTimeout(timer);
    await runtime.disconnect();
    fs.rmSync(folder, { recursive: true, force: true });
  });
  return {
    messages,
    count,
    wait,
    receive,
    runtime,
    catalog() {
      receive({ op: 'catalogInfo', ...store.info() });
    },
    hold(value) {
      holdDownloads = value;
    },
    pauseApply(value) {
      applyBarrier = value;
    },
  };
}

test('duplicate active and in-flight catalogs acknowledge readiness without rerunning resources', async (t) => {
  const f = fixture(t);
  f.catalog();
  f.catalog();
  await f.wait(() => f.count('resourcesReady') === 2);
  const downloaded = f.count('catalogChunk') + f.count('fileRequest');
  f.catalog();
  await f.wait(() => f.count('resourcesReady') === 3);
  assert.equal(f.count('event', 'started'), 1);
  assert.equal(f.count('catalogChunk') + f.count('fileRequest'), downloaded);
});

test('a verified catalog with failed code never receives a ready acknowledgment on retry', async (t) => {
  const f = fixture(t, "throw new Error('start failed');");
  f.catalog();
  await f.wait(
    () =>
      f.messages.filter((message) => message.op === 'log' && message.resource === 'host').length ===
      1,
  );
  f.catalog();
  await f.wait(
    () =>
      f.messages.filter((message) => message.op === 'log' && message.resource === 'host').length ===
      2,
  );
  assert.equal(f.count('resourcesReady'), 0);
  assert.equal(f.runtime.resources.size, 0);
});

test('disconnect cancels both downloading and queued catalogs before another session starts', async (t) => {
  const f = fixture(t);
  f.hold(true);
  f.catalog();
  f.catalog();
  await f.wait(() => f.count('catalogChunk') === 1);
  f.receive({ op: 'disconnect' });
  await f.wait(() => f.count('runtimeStopped') === 1);
  assert.equal(f.count('resourcesReady'), 0);
  assert.equal(f.count('catalogChunk'), 1);
  f.hold(false);
  f.catalog();
  await f.wait(() => f.count('resourcesReady') === 1);
  assert.equal(f.count('event', 'started'), 1);
});

test('disconnect during async activation suppresses stale readiness and reconnect restarts cached code', async (t) => {
  const f = fixture(t);
  let release;
  f.pauseApply(
    new Promise((resolve) => {
      release = resolve;
    }),
  );
  f.catalog();
  await f.wait(() => f.count('event', 'started') === 1);
  f.receive({ op: 'disconnect' });
  release();
  await f.wait(() => f.count('runtimeStopped') === 1);
  assert.equal(f.count('resourcesReady'), 0);
  assert.equal(f.runtime.resources.size, 0);
  const fileDownloads = f.count('fileRequest');
  f.pauseApply(null);
  f.catalog();
  await f.wait(() => f.count('resourcesReady') === 1);
  assert.equal(f.count('event', 'started'), 2);
  assert.equal(f.count('fileRequest'), fileDownloads);
});

test('production worker publishes entity chunks only on commit and rejects previous session', async (t) => {
  const f = fixture(t);
  const session = '0123456789abcdef0123456789abcdef';
  const next = 'abcdef0123456789abcdef0123456789';
  const directory = {
    localPlayerId: 1,
    entities: [
      {
        id: 1,
        nativeId: 1,
        kind: 'player',
        dimension: 0xffffffff,
        position: { x: 1, y: 2, z: 3 },
        rotation: { w: 1, x: 0, y: 0, z: 0 },
      },
    ],
  };
  const bytes = Buffer.from(JSON.stringify(directory));
  const identity = { session, kind: 'directory', revision: 1 };
  f.receive({ op: 'entitySession', session });
  f.receive({ op: 'entityBegin', ...identity, size: bytes.length });
  f.receive({ op: 'entityChunk', ...identity, offset: 0, data: bytes.toString('base64') });
  // Readiness drains the worker's serialized command queue without publishing.
  f.catalog();
  await f.wait(() => f.count('resourcesReady') === 1);
  assert.equal(f.runtime.entities.facade().LocalPlayer, null);
  f.receive({ op: 'entityCommit', ...identity });
  await f.wait(() => f.runtime.entities.facade().LocalPlayer !== null);
  const player = f.runtime.entities.facade().LocalPlayer;
  assert.equal(player.position.x, 1);
  f.receive({ op: 'entitySession', session: next });
  f.receive({ op: 'entityBegin', ...identity, size: bytes.length });
  f.receive({ op: 'entityChunk', ...identity, offset: 0, data: bytes.toString('base64') });
  f.receive({ op: 'entityCommit', ...identity });
  f.catalog();
  await f.wait(() => f.count('resourcesReady') === 2);
  assert.equal(f.runtime.entities.facade().LocalPlayer, null);
  assert.throws(() => player.position, /no longer/);
});

test('a quest the native client starts or stops following reaches the resources', async (t) => {
  const f = fixture(
    t,
    "kcds.core.Events.on('questTrackingChanged', (key, tracked) => kcds.core.Events.toServer('tracking', key, tracked));",
  );
  f.catalog();
  await f.wait(() => f.count('resourcesReady') === 1);
  f.receive({ op: 'questTrackingChanged', key: 'smith', tracked: 'yes' });
  f.receive({ op: 'questTrackingChanged', key: 'smith', tracked: true });
  await f.wait(() => f.count('event', 'tracking') === 1);
  const event = f.messages.find((message) => message.op === 'event' && message.name === 'tracking');
  assert.match(JSON.stringify(event), /smith/);
});

test('the game client voice state reaches LocalVoice and its transmitting event', async (t) => {
  const f = fixture(
    t,
    "kcds.core.Events.on('localVoiceTransmitting', (on) => kcds.core.Events.toServer('talking', on, kcds.ui.LocalVoice.isTransmitting()));",
  );
  f.catalog();
  await f.wait(() => f.count('resourcesReady') === 1);
  f.receive({ op: 'voiceState', enabled: true, tier: 1, transmitting: true, hasMicrophone: true });
  await f.wait(() => f.count('event', 'talking') === 1);
  const event = f.messages.find((message) => message.op === 'event' && message.name === 'talking');
  assert.match(JSON.stringify(event), /true/);
});

test('the server tree events (kcds:tree) reach client scripts with their arguments', async (t) => {
  const f = fixture(
    t,
    [
      "kcds.core.Events.on('treeFalling', (tree, fall) => kcds.core.Events.toServer('saw', [tree.id, fall.seconds]));",
      "kcds.core.Events.on('treeFelled', (tree, extra) => kcds.core.Events.toServer('down', [tree.state, extra === undefined]));",
    ].join('\n'),
  );
  f.catalog();
  await f.wait(() => f.count('resourcesReady') === 1);
  const tree = { id: 'birch@1,2,3', species: 'birch.cgf', position: { x: 1, y: 2, z: 3 } };
  const payload = (event, extra) =>
    JSON.stringify({ event, tree: { ...tree, state: event }, extra });
  f.receive({ op: 'event', name: 'kcds:tree', payload: payload('treeFalling', { seconds: 3 }) });
  f.receive({ op: 'event', name: 'kcds:tree', payload: payload('treeFelled', null) });
  await f.wait(() => f.count('event', 'down') === 1);
  const sent = (name) =>
    JSON.parse(
      f.messages.find((message) => message.op === 'event' && message.name === name).payload,
    );
  assert.deepEqual(sent('saw'), ['birch@1,2,3', 3]);
  assert.deepEqual(sent('down'), ['treeFelled', true]);
  assert.equal(f.count('event', 'kcds:tree'), 0, 'the reserved event is not forwarded as itself');
});
