'use strict';

// Acceptance: EventBus events across the network.
// The server side runs the production host entry point (main.cjs) over its native IPC framing.
// The client side runs the production browser worker (worker.cjs) with the real runtime.
// Client Events.toServer reaches only server Events.fromClient handlers as
// (sender, payload) with the payload parsed from JSON; server Events.broadcast reaches
// client Events.on handlers with the payload parsed; a payload that is not JSON text arrives as
// that string. A player's loaded client resources raise playerResourcesReady on the server.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const readline = require('node:readline');
const vm = require('node:vm');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const { createRequire } = require('node:module');
const { webcrypto } = require('node:crypto');
const { encode, decode } = require('./codec.cjs');
const { ResourceBundleStore } = require('./bundles.cjs');
const { normalizeManifest } = require('./manifest.cjs');
const { BrowserResourceRuntime } = require('../../../../Client/resources/runtime.cjs');

const SERVER_RESOURCE = `
kcds.core.Events.on('playerDied', () => console.info('native:playerDied'));
kcds.core.Events.fromClient('hello', (sender, payload) => {
  console.info(
    'hello:' +
      JSON.stringify({
        player: sender instanceof kcds.players.Player && kcds.players.Player.getById(sender.id) === sender,
        id: sender.id,
        payload,
      }),
  );
});
kcds.core.Events.onceFromClient('once', () => console.info('once:called'));
kcds.core.Events.on('playerResourcesReady', (player) => console.info('resources:' + player.id));
const removed = () => console.info('removed:called');
kcds.core.Events.fromClient('removed', removed);
kcds.core.Events.offFromClient('removed', removed);
kcds.core.Events.on('consoleCommand', (command, args) => {
  if (command === 'tell') {
    kcds.players.Player.getById(Number(args[0])).emit('welcome', { n: 1 });
  }
  if (command === 'broadcast') {
    kcds.core.Events.broadcast('scores', { rows: [1, 2] });
    kcds.core.Events.broadcast('verbatim', '{"already":"json"}');
    kcds.core.Events.broadcast('empty');
  }
});
`;

async function startHost(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-network-events-'));
  const folder = path.join(root, 'mode');
  fs.mkdirSync(folder);
  fs.writeFileSync(
    path.join(folder, 'package.json'),
    JSON.stringify({ name: 'mode', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(folder, 'index.js'), SERVER_RESOURCE);
  const child = spawn(process.execPath, [path.join(__dirname, 'main.cjs'), root], {
    windowsHide: true,
    stdio: ['pipe', 'pipe', 'pipe'],
  });
  const frames = [];
  const errors = [];
  child.stderr.on('data', (chunk) => errors.push(String(chunk)));
  readline.createInterface({ input: child.stdout }).on('line', (line) => {
    frames.push(decode(Buffer.from(line, 'hex')));
  });
  const exited = once(child, 'exit');
  const deadline = setTimeout(() => child.kill(), 15000);
  t.after(async () => {
    clearTimeout(deadline);
    if (child.exitCode === null) {
      child.stdin.end();
      await exited;
    }
    fs.rmSync(root, { recursive: true, force: true });
  });
  const send = (value) => child.stdin.write(encode(value).toString('hex') + '\n');
  const waitFor = async (predicate) => {
    const end = Date.now() + 5000;
    while (!frames.some(predicate)) {
      assert.ok(Date.now() < end, 'expected production host frame before timeout');
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
  };
  const texts = (prefix) =>
    frames.filter((frame) => frame.op === 'log' && frame.text?.startsWith(prefix));
  await waitFor((frame) => frame.op === 'ready');
  return { child, frames, errors, exited, send, waitFor, texts };
}

function clientEvent(player, name, payload) {
  return {
    op: 'clientRequest',
    peer: 3,
    player,
    playerIndex: 0,
    message: { op: 'event', name, payload },
  };
}

test('server fromClient receives the sender Player and the parsed payload', async (t) => {
  const host = await startHost(t);
  const playerId = 5;
  host.send({
    op: 'player',
    id: playerId,
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  host.send(clientEvent(playerId, 'hello', '{"value":5,"tags":["a"]}'));
  await host.waitFor((frame) => frame.text?.startsWith('hello:'));
  const [hello] = host.texts('hello:');
  assert.deepEqual(JSON.parse(hello.text.slice('hello:'.length)), {
    player: true,
    id: playerId,
    payload: { value: 5, tags: ['a'] },
  });

  // A string that is not JSON text (an older client's) arrives as that string.
  host.send(clientEvent(playerId, 'hello', 'plain text'));
  await host.waitFor((frame) => frame.text?.includes('"payload":"plain text"'));

  // The player's client resources run: playerResourcesReady.
  host.send({
    op: 'clientRequest',
    peer: 3,
    player: playerId,
    playerIndex: 0,
    message: { op: 'resourcesReady' },
  });
  await host.waitFor((frame) => frame.text === `resources:${playerId}`);

  // A request without a known sender is dropped.
  host.send(clientEvent(9, 'hello', '{}'));
  await host.waitFor((frame) => frame.text?.includes('unknown sender'));

  // The client table is isolated from native and shared events.
  host.send(clientEvent(playerId, 'playerDied', '{}'));
  // onceFromClient fires once; offFromClient removed its handler.
  host.send(clientEvent(playerId, 'once', 'null'));
  host.send(clientEvent(playerId, 'once', 'null'));
  host.send(clientEvent(playerId, 'removed', 'null'));
  host.send(clientEvent(playerId, 'hello', '{"value":6}'));
  await host.waitFor((frame) => frame.text?.includes('"value":6'));

  assert.equal(host.texts('hello:').length, 3);
  assert.equal(host.texts('native:').length, 0);
  assert.equal(host.texts('once:').length, 1);
  assert.equal(host.texts('removed:').length, 0);

  host.child.stdin.end();
  const [code] = await host.exited;
  assert.equal(code, 0);
  assert.deepEqual(host.errors, []);
});

test('player.emit waits for the client resources of the player and then goes in order', async (t) => {
  const host = await startHost(t);
  host.send({
    op: 'player',
    id: 6,
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  host.send({ op: 'console', text: 'tell 6' });
  host.send({ op: 'console', text: 'tell 6' });
  await new Promise((resolve) => setTimeout(resolve, 200));
  const welcomes = () => host.frames.filter((f) => f.op === 'event' && f.name === 'welcome');
  assert.equal(welcomes().length, 0, 'held while the client resources download');
  host.send({
    op: 'clientRequest',
    peer: 3,
    player: 6,
    playerIndex: 0,
    message: { op: 'resourcesReady' },
  });
  await host.waitFor((frame) => frame.text === 'resources:6');
  assert.equal(welcomes().length, 2);
  assert.equal(welcomes()[0].player, 6);
  assert.equal(welcomes()[0].payload, '{"n":1}');
  // From now on at once.
  host.send({ op: 'console', text: 'tell 6' });
  await host.waitFor(() => welcomes().length === 3);
  host.child.stdin.end();
  await host.exited;
});

test('server broadcast serializes payloads for the wire', async (t) => {
  const host = await startHost(t);
  host.send({ op: 'console', text: 'broadcast' });
  await host.waitFor((frame) => frame.op === 'event' && frame.name === 'empty');
  const events = host.frames.filter((frame) => frame.op === 'event');
  assert.deepEqual(
    events.map((frame) => [frame.name, frame.payload]),
    [
      ['scores', '{"rows":[1,2]}'],
      ['verbatim', '{"already":"json"}'],
      ['empty', 'null'],
    ],
  );
  host.child.stdin.end();
  await host.exited;
});

function workerFixture(t, source) {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-network-worker-'));
  fs.writeFileSync(path.join(folder, 'client.cjs'), source);
  const store = new ResourceBundleStore();
  store.publish([
    {
      folder,
      manifest: normalizeManifest({ name: 'mode', kcds: { clientScripts: ['client.cjs'] } }),
    },
  ]);
  const messages = [];
  let runtime;
  const self = {
    postMessage(message) {
      messages.push(message);
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
  }
  const context = vm.createContext({
    self,
    crypto: webcrypto,
    TextEncoder,
    TextDecoder,
    Uint8Array,
    atob: globalThis.atob,
    setTimeout,
    clearTimeout,
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
  async function wait(predicate) {
    const deadline = Date.now() + 2000;
    while (!predicate()) {
      if (Date.now() > deadline)
        throw new Error('Worker expectation timed out: ' + JSON.stringify(messages));
      await new Promise((resolve) => setImmediate(resolve));
    }
  }
  t.after(async () => {
    await runtime?.disconnect();
    fs.rmSync(folder, { recursive: true, force: true });
  });
  receive({ op: 'catalogInfo', ...store.info() });
  return { messages, receive, wait };
}

test('client Events.on receives server events with the parsed payload', async (t) => {
  const f = workerFixture(
    t,
    `kcds.core.Events.on('scores', (payload) => {
      kcds.core.Events.toServer('seen', { array: Array.isArray(payload.rows), rows: payload.rows });
    });
    kcds.core.Events.toServer('started');`,
  );
  const sent = (name) =>
    f.messages.filter((message) => message.op === 'event' && message.name === name);
  await f.wait(() => sent('started').length === 1);

  f.receive({ op: 'event', name: 'scores', payload: '{"rows":[1,2]}' });
  await f.wait(() => sent('seen').length === 1);
  assert.equal(sent('seen')[0].payload, '{"array":true,"rows":[1,2]}');

  // A payload that is not JSON text (an older server's) arrives as that string.
  f.receive({ op: 'event', name: 'scores', payload: 'not json' });
  await f.wait(() => sent('seen').length === 2);
  assert.equal(sent('seen')[1].payload, '{"array":false}');
});
