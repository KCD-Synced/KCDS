'use strict';

// Acceptance: resource lifecycle hot reload.
// The checks run against the real server ResourceHost and the real browser
// BrowserResourceRuntime. A hot reload on either side must clear the replaced
// resource's callbacks, event subscriptions, timers, imports, exports and
// pending inter-resource messages. Dependency cycles must be rejected before
// any resource starts, and outbound event queues must stay bounded on both
// sides. CPU isolation of resource bodies is architectural (the client worker
// runs off the engine thread; trusted server resources are not CPU-sandboxed)
// and is not asserted here.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');

const { ResourceHost } = require('./host.cjs');
const { normalizeManifest } = require('./manifest.cjs');
const { BrowserResourceRuntime } = require('../../../../Client/resources/runtime.cjs');

function serverFixture(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-lifecycle-accept-'));
  const logs = [];
  const sent = [];
  const host = new ResourceHost(root, {
    send: (value) => sent.push(value),
    log: (resource, text) => logs.push([resource, text]),
  });
  const write = (name, code, options = {}) => {
    const folder = path.join(root, name);
    fs.mkdirSync(folder, { recursive: true });
    fs.writeFileSync(
      path.join(folder, 'package.json'),
      JSON.stringify({ name, kcds: { serverScripts: ['main.js'], ...options } }),
    );
    fs.writeFileSync(path.join(folder, 'main.js'), code);
  };
  t.after(async () => {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  });
  return { host, write, logs, sent };
}

function clientFixture(t) {
  const messages = [];
  const logs = [];
  const files = new Map();
  const runtime = new BrowserResourceRuntime({
    send: (value) => messages.push(value),
    log: (resource, text) => logs.push([resource, String(text)]),
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
  const resource = (name, source, options = {}, hash = name) => {
    files.set(name + '/main.js', new TextEncoder().encode(source));
    return {
      hash,
      manifest: normalizeManifest({ name, kcds: { clientScripts: ['main.js'], ...options } }),
      files: [{ name: 'main.js' }],
    };
  };
  const read = (name, file) => {
    const bytes = files.get(name + '/' + file);
    if (!bytes) throw new Error('Unknown file');
    return bytes;
  };
  t.after(() => runtime.disconnect());
  return { runtime, resource, read, messages, logs };
}

function countChat(sent, text) {
  return sent.filter((message) => message.op === 'chat' && message.text === text).length;
}

function countEvent(messages, name) {
  return messages.filter((message) => message.op === 'event' && message.name === name).length;
}

test('server hot reload clears timers, handlers and exports and rejects pending messages', async (t) => {
  const { host, write, sent } = serverFixture(t);
  write(
    'library',
    `
    kcds.core.Messages.handle('never', () => {});
    kcds.core.Exports.register('answer', 1);
    kcds.core.Events.on('ping', () => kcds.core.Chat.sendToAll('pong-v1'));
    setInterval(() => kcds.core.Chat.sendToAll('tick'), 5);
    `,
    { exports: ['answer'] },
  );
  write(
    'consumer',
    `kcds.core.Events.on('ping', () => kcds.core.Chat.sendToAll('consume-' + kcds.core.Exports.get('library', 'answer')));`,
    { requires: ['library'] },
  );
  await host.startAll();

  const consumer = host.services.owners.get('consumer');
  const pending = host.services.facade(consumer).Messages.request('library', 'never');
  const rejection = assert.rejects(pending, /stopped/);

  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.ok(countChat(sent, 'tick') > 0, 'the interval should fire before reload');

  write(
    'library',
    `
    kcds.core.Exports.register('answer', 2);
    kcds.core.Events.on('ping', () => kcds.core.Chat.sendToAll('pong-v2'));
    `,
    { exports: ['answer'] },
  );
  write(
    'consumer',
    `kcds.core.Events.on('ping', () => kcds.core.Chat.sendToAll('consume-' + kcds.core.Exports.get('library', 'answer')));`,
    { requires: ['library'] },
  );
  await host.refreshAll();
  await rejection;

  const ticksAfterReload = countChat(sent, 'tick');
  await new Promise((resolve) => setTimeout(resolve, 25));
  assert.equal(countChat(sent, 'tick'), ticksAfterReload, 'the old interval must not fire again');

  sent.length = 0;
  await host.services.emit('shared', 'ping', []);
  const texts = sent
    .filter((message) => message.op === 'chat')
    .map((message) => message.text)
    .sort();
  assert.deepEqual(texts, ['consume-2', 'pong-v2'], 'only the replaced handlers and export remain');
});

test('server rejects a resource dependency cycle without starting any resource', async (t) => {
  const { host, write } = serverFixture(t);
  write('alpha', `kcds.core.Chat.sendToAll('alpha');`, { requires: ['beta'] });
  write('beta', `kcds.core.Chat.sendToAll('beta');`, { requires: ['alpha'] });
  await assert.rejects(host.startAll(), /cycle/i);
  assert.equal(host.resources.size, 0);
});

test('server bounds oversized outbound event payloads', async (t) => {
  const { host, write, sent } = serverFixture(t);
  write(
    'big',
    `
    try {
      kcds.core.Events.broadcast('huge', 'x'.repeat(40000));
    } catch (error) {
      kcds.core.Chat.sendToAll(error.constructor.name);
    }
    `,
  );
  await host.startAll();
  assert.ok(!sent.some((message) => message.op === 'event' && message.name === 'huge'));
  assert.ok(sent.some((message) => message.op === 'chat' && message.text === 'RangeError'));
});

test('client hot reload clears timers, handlers and exports and rejects pending messages', async (t) => {
  const f = clientFixture(t);
  const libraryV1 = f.resource(
    'library',
    `
    kcds.core.Messages.handle('never', () => {});
    kcds.core.Exports.register('answer', 1);
    setInterval(() => kcds.core.Events.toServer('tick'), 5);
    `,
    { exports: ['answer'] },
    'library-v1',
  );
  const consumer = f.resource(
    'consumer',
    `kcds.core.Events.on('ping', () => kcds.core.Events.toServer('value', kcds.core.Exports.get('library', 'answer')));`,
    { requires: ['library'] },
    'consumer-v1',
  );
  await f.runtime.apply({ resources: [libraryV1, consumer] }, f.read);

  const consumerOwner = f.runtime.services.owners.get('consumer');
  const pending = f.runtime.services.facade(consumerOwner).Messages.request('library', 'never');
  const rejection = assert.rejects(pending, /stopped/);

  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.ok(countEvent(f.messages, 'tick') > 0, 'the client interval should fire before reload');

  const libraryV2 = f.resource(
    'library',
    `kcds.core.Exports.register('answer', 2);`,
    { exports: ['answer'] },
    'library-v2',
  );
  await f.runtime.apply({ resources: [libraryV2, consumer] }, f.read);
  await rejection;

  const ticksAfterReload = countEvent(f.messages, 'tick');
  await new Promise((resolve) => setTimeout(resolve, 25));
  assert.equal(
    countEvent(f.messages, 'tick'),
    ticksAfterReload,
    'the old client interval must not fire again',
  );

  const reloadedConsumer = f.runtime.services.facade(f.runtime.services.owners.get('consumer'));
  assert.equal(
    reloadedConsumer.Events.listenerCount('ping'),
    1,
    'exactly one ping handler remains',
  );

  f.messages.length = 0;
  await f.runtime.services.emit('shared', 'ping', []);
  const values = f.messages
    .filter((message) => message.op === 'event' && message.name === 'value')
    .map((message) => message.payload);
  assert.deepEqual(values, ['2'], 'the consumer observes the replaced export value');
});

test('client rejects a resource dependency cycle without starting any resource', async (t) => {
  const f = clientFixture(t);
  const alpha = f.resource('alpha', `kcds.core.Events.toServer('alpha');`, {
    requires: ['beta'],
  });
  const beta = f.resource('beta', `kcds.core.Events.toServer('beta');`, {
    requires: ['alpha'],
  });
  await assert.rejects(f.runtime.apply({ resources: [alpha, beta] }, f.read), /cycle/i);
  assert.equal(f.runtime.resources.size, 0);
});

test('client bounds oversized outbound event payloads', async (t) => {
  const f = clientFixture(t);
  const big = f.resource(
    'big',
    `
    try {
      kcds.core.Events.toServer('huge', 'x'.repeat(40000));
    } catch (error) {
      kcds.core.Events.toServer('rejected', error.constructor.name);
    }
    `,
  );
  await f.runtime.apply({ resources: [big] }, f.read);
  assert.ok(!f.messages.some((message) => message.op === 'event' && message.name === 'huge'));
  assert.ok(
    f.messages.some(
      (message) =>
        message.op === 'event' &&
        message.name === 'rejected' &&
        JSON.parse(message.payload) === 'RangeError',
    ),
  );
});

test('client runtime faults stay with the faulting resource and neighbours keep running', async (t) => {
  const f = clientFixture(t);
  const faulty = f.resource(
    'faulty',
    `
    setTimeout(() => {
      throw new Error('timeout fault');
    }, 1);
    setInterval(() => {
      throw new Error('interval fault');
    }, 5);
    queueMicrotask(() => {
      throw new Error('microtask fault');
    });
    kcds.core.Events.on('ping', () => {
      throw new Error('handler fault');
    });
    kcds.core.Events.on('ping', async () => {
      throw new Error('async handler fault');
    });
    kcds.core.Events.on('ping', () => kcds.core.Events.toServer('faulty-alive'));
    `,
  );
  const healthy = f.resource(
    'healthy',
    `
    setInterval(() => kcds.core.Events.toServer('healthy-tick'), 5);
    kcds.core.Events.on('ping', () => kcds.core.Events.toServer('healthy-alive'));
    `,
  );
  await f.runtime.apply({ resources: [faulty, healthy] }, f.read);
  await new Promise((resolve) => setTimeout(resolve, 40));

  const faultyLogs = f.logs.filter(([owner]) => owner === 'faulty').map(([, text]) => text);
  assert.ok(faultyLogs.some((text) => text.includes('timeout fault')));
  assert.ok(faultyLogs.some((text) => text.includes('interval fault')));
  assert.ok(faultyLogs.some((text) => text.includes('microtask fault')));
  assert.ok(
    !f.logs.some(([owner, text]) => owner === 'healthy' && text.includes('fault')),
    'faults are never charged to another resource',
  );

  const ticks = countEvent(f.messages, 'healthy-tick');
  assert.ok(ticks > 0, 'the neighbour interval keeps firing');
  await assert.rejects(f.runtime.services.emit('shared', 'ping', []), AggregateError);
  assert.equal(countEvent(f.messages, 'healthy-alive'), 1);
  assert.equal(countEvent(f.messages, 'faulty-alive'), 1, 'the faulting resource keeps running');
  assert.ok(f.runtime.resources.has('faulty'));
  assert.ok(f.runtime.resources.has('healthy'));

  await new Promise((resolve) => setTimeout(resolve, 25));
  assert.ok(countEvent(f.messages, 'healthy-tick') > ticks);
});

test('client start failure stops only the failing resource and its dependents', async (t) => {
  const f = clientFixture(t);
  const broken = f.resource('broken', `throw new Error('start fault');`);
  const dependent = f.resource('dependent', `kcds.core.Events.toServer('dependent-started');`, {
    requires: ['broken'],
  });
  const independent = f.resource(
    'independent',
    `kcds.core.Events.toServer('independent-started');`,
  );
  // The catalog still fails as a whole, so it is never acknowledged as ready.
  await assert.rejects(
    f.runtime.apply({ resources: [broken, dependent, independent] }, f.read),
    AggregateError,
  );

  assert.ok(f.logs.some(([owner, text]) => owner === 'broken' && text.includes('start fault')));
  assert.equal(countEvent(f.messages, 'independent-started'), 1);
  assert.equal(countEvent(f.messages, 'dependent-started'), 0);
  assert.ok(f.runtime.resources.has('independent'));
  assert.ok(!f.runtime.resources.has('broken'));
  assert.ok(!f.runtime.resources.has('dependent'));
});
