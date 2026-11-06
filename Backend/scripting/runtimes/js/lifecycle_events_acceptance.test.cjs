'use strict';

// Acceptance: EventMap.resourceStart and EventMap.resourceStop on
// the production server ResourceHost and the production browser BrowserResourceRuntime.
// Contract:
// - resourceStart(resourceName) fires for every resource after the entry scripts ran and before
//   the resource is running; async handlers are awaited, dependents start only after they settle,
//   and a rejection fails the start and cleans up what the resource registered.
// - resourceStop(resourceName) fires for every resource first, while the stopping resource can
//   still use everything; dependents stop first; a rejecting handler is logged and cleanup
//   happens anyway.
// The 30 s start and 10 s stop limits are the constants passed to the runtimes' timeout helpers;
// they are not waited for here.

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
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-lifecycle-events-'));
  const logs = [];
  const sent = [];
  const host = new ResourceHost(root, {
    send: (value) => sent.push(value),
    log: (resource, text) => logs.push([resource, String(text)]),
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
  const trace = () =>
    sent.filter((message) => message.op === 'chat').map((message) => message.text);
  t.after(async () => {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  });
  return { host, write, trace, logs };
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
  const resource = (name, source, options = {}) => {
    files.set(name + '/main.js', new TextEncoder().encode(source));
    return {
      hash: name,
      manifest: normalizeManifest({ name, kcds: { clientScripts: ['main.js'], ...options } }),
      files: [{ name: 'main.js' }],
    };
  };
  const read = (name, file) => {
    const bytes = files.get(name + '/' + file);
    if (!bytes) {
      throw new Error('Unknown file');
    }
    return bytes;
  };
  const trace = () =>
    messages
      .filter((message) => message.op === 'event' && message.name === 'trace')
      // A plain string travels as a JSON string.
      .map((message) => JSON.parse(message.payload));
  t.after(() => runtime.disconnect());
  return { runtime, resource, read, trace, logs };
}

test('server resourceStart runs after entry scripts, reaches every resource and gates dependents', async (t) => {
  const f = serverFixture(t);
  f.write(
    'alpha',
    `
    kcds.core.Chat.sendToAll('alpha:script');
    kcds.core.Events.on('resourceStart', (name) => kcds.core.Chat.sendToAll('alpha:sees-start:' + name));
    `,
  );
  f.write(
    'beta',
    `
    kcds.core.Chat.sendToAll('beta:script');
    kcds.core.Events.on('resourceStart', async (name) => {
      if (name !== 'beta') {
        return;
      }
      await new Promise((resolve) => setTimeout(resolve, 30));
      kcds.core.Chat.sendToAll('beta:async-start-done');
    });
    `,
    { requires: ['alpha'] },
  );
  f.write(`gamma`, `kcds.core.Chat.sendToAll('gamma:script');`, { requires: ['beta'] });
  await f.host.startAll();

  const trace = f.trace();
  assert.ok(trace.indexOf('alpha:script') < trace.indexOf('alpha:sees-start:alpha'));
  assert.ok(trace.indexOf('beta:script') < trace.indexOf('alpha:sees-start:beta'));
  assert.ok(trace.includes('alpha:sees-start:gamma'), 'resourceStart reaches every resource');
  assert.ok(
    trace.indexOf('beta:async-start-done') < trace.indexOf('gamma:script'),
    'a dependent starts only after the async resourceStart handler settled',
  );
});

test('server resourceStart rejection fails the start and cleans up the resource', async (t) => {
  const f = serverFixture(t);
  f.write(
    'faulty',
    `
    setInterval(() => kcds.core.Chat.sendToAll('faulty:tick'), 5);
    kcds.core.Events.on('ping', () => kcds.core.Chat.sendToAll('faulty:ping'));
    kcds.core.Events.on('resourceStart', async (name) => {
      if (name === 'faulty') {
        throw new Error('start refused');
      }
    });
    `,
  );
  f.host.refresh();
  await assert.rejects(f.host.start('faulty'), (error) => {
    assert.match(String(error.errors?.[0]), /start refused/);
    return true;
  });
  assert.equal(f.host.resources.has('faulty'), false);
  const ticks = f.trace().filter((text) => text === 'faulty:tick').length;
  await new Promise((resolve) => setTimeout(resolve, 25));
  assert.equal(f.trace().filter((text) => text === 'faulty:tick').length, ticks);
  await f.host.services.emit('shared', 'ping', []);
  assert.equal(
    f.trace().includes('faulty:ping'),
    false,
    'handlers are removed after a failed start',
  );
});

test('server resourceStop runs first while the resource is usable, dependents first', async (t) => {
  const f = serverFixture(t);
  f.write(
    'library',
    `
    kcds.core.Exports.register('answer', 42);
    kcds.core.Events.on('resourceStop', (name) => {
      if (name === 'library') {
        kcds.core.Chat.sendToAll('library:stop:export=' + kcds.core.Exports.get('library', 'answer'));
      }
    });
    `,
    { exports: ['answer'] },
  );
  f.write(
    'consumer',
    `kcds.core.Events.on('resourceStop', (name) => kcds.core.Chat.sendToAll('consumer:sees-stop:' + name));`,
    { requires: ['library'] },
  );
  await f.host.startAll();
  await f.host.stop('library');

  assert.deepEqual(f.trace(), ['consumer:sees-stop:consumer', 'library:stop:export=42']);
  assert.equal(f.host.resources.has('consumer'), false);
  assert.equal(f.host.resources.has('library'), false);
});

test('server resourceStop rejection is logged and cleanup still happens', async (t) => {
  const f = serverFixture(t);
  f.write(
    'stubborn',
    `
    setInterval(() => kcds.core.Chat.sendToAll('stubborn:tick'), 5);
    kcds.core.Events.on('resourceStop', async () => {
      throw new Error('stop refused');
    });
    `,
  );
  await f.host.startAll();
  await f.host.stop('stubborn');
  assert.ok(f.logs.some(([owner, text]) => owner === 'stubborn' && text.includes('stop refused')));
  assert.equal(f.host.resources.has('stubborn'), false);
  const ticks = f.trace().filter((text) => text === 'stubborn:tick').length;
  await new Promise((resolve) => setTimeout(resolve, 25));
  assert.equal(f.trace().filter((text) => text === 'stubborn:tick').length, ticks);
});

test('client resourceStart runs after entry scripts, reaches every resource and gates dependents', async (t) => {
  const f = clientFixture(t);
  const send = `const trace = (value) => kcds.core.Events.toServer('trace', value);`;
  const alpha = f.resource(
    'alpha',
    `${send}
    trace('alpha:script');
    kcds.core.Events.on('resourceStart', (name) => trace('alpha:sees-start:' + name));`,
  );
  const beta = f.resource(
    'beta',
    `${send}
    trace('beta:script');
    kcds.core.Events.on('resourceStart', async (name) => {
      if (name !== 'beta') {
        return;
      }
      await new Promise((resolve) => setTimeout(resolve, 30));
      trace('beta:async-start-done');
    });`,
    { requires: ['alpha'] },
  );
  const gamma = f.resource('gamma', `${send} trace('gamma:script');`, {
    requires: ['beta'],
  });
  await f.runtime.apply({ resources: [alpha, beta, gamma] }, f.read);

  const trace = f.trace();
  assert.ok(trace.indexOf('alpha:script') < trace.indexOf('alpha:sees-start:alpha'));
  assert.ok(trace.indexOf('beta:script') < trace.indexOf('alpha:sees-start:beta'));
  assert.ok(trace.includes('alpha:sees-start:gamma'), 'resourceStart reaches every resource');
  assert.ok(
    trace.indexOf('beta:async-start-done') < trace.indexOf('gamma:script'),
    'a dependent starts only after the async resourceStart handler settled',
  );
});

test('client resourceStart rejection fails the start and cleans up the resource', async (t) => {
  const f = clientFixture(t);
  const faulty = f.resource(
    'faulty',
    `
    setInterval(() => kcds.core.Events.toServer('trace', 'faulty:tick'), 5);
    kcds.core.Events.on('ping', () => kcds.core.Events.toServer('trace', 'faulty:ping'));
    kcds.core.Events.on('resourceStart', async (name) => {
      if (name === 'faulty') {
        throw new Error('start refused');
      }
    });
    `,
  );
  await assert.rejects(f.runtime.apply({ resources: [faulty] }, f.read), (error) => {
    assert.match(String(error.errors?.[0]), /start refused/);
    return true;
  });
  assert.equal(f.runtime.resources.has('faulty'), false);
  const ticks = f.trace().filter((text) => text === 'faulty:tick').length;
  await new Promise((resolve) => setTimeout(resolve, 25));
  assert.equal(f.trace().filter((text) => text === 'faulty:tick').length, ticks);
  await f.runtime.services.emit('shared', 'ping', []);
  assert.equal(
    f.trace().includes('faulty:ping'),
    false,
    'handlers are removed after a failed start',
  );
});

test('client resourceStop runs first while the resource is usable, dependents first', async (t) => {
  const f = clientFixture(t);
  const library = f.resource(
    'library',
    `
    kcds.core.Exports.register('answer', 42);
    kcds.core.Events.on('resourceStop', (name) => {
      if (name === 'library') {
        kcds.core.Events.toServer('trace', 'library:stop:export=' + kcds.core.Exports.get('library', 'answer'));
      }
    });
    `,
    { exports: ['answer'] },
  );
  const consumer = f.resource(
    'consumer',
    `kcds.core.Events.on('resourceStop', (name) => kcds.core.Events.toServer('trace', 'consumer:sees-stop:' + name));`,
    { requires: ['library'] },
  );
  await f.runtime.apply({ resources: [library, consumer] }, f.read);
  await f.runtime.stop('library');

  assert.deepEqual(f.trace(), ['consumer:sees-stop:consumer', 'library:stop:export=42']);
  assert.equal(f.runtime.resources.has('consumer'), false);
  assert.equal(f.runtime.resources.has('library'), false);
});

test('client resourceStop rejection is logged and cleanup still happens', async (t) => {
  const f = clientFixture(t);
  const stubborn = f.resource(
    'stubborn',
    `
    setInterval(() => kcds.core.Events.toServer('trace', 'stubborn:tick'), 5);
    kcds.core.Events.on('resourceStop', async () => {
      throw new Error('stop refused');
    });
    `,
  );
  await f.runtime.apply({ resources: [stubborn] }, f.read);
  await f.runtime.stop('stubborn');
  assert.ok(f.logs.some(([owner, text]) => owner === 'stubborn' && text.includes('stop refused')));
  assert.equal(f.runtime.resources.has('stubborn'), false);
  const ticks = f.trace().filter((text) => text === 'stubborn:tick').length;
  await new Promise((resolve) => setTimeout(resolve, 25));
  assert.equal(f.trace().filter((text) => text === 'stubborn:tick').length, ticks);
});
