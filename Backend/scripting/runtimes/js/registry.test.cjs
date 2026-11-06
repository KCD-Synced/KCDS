'use strict';

// Acceptance coverage for the cross-resource Imports/Exports registry.
// The behaviour asserted here is implemented by the shared
// ResourceServices facade that both the trusted server host and the browser
// client runtime bind:
//
//   - Exports.register(name, value) stores a value only when the name is listed
//     in the manifest exports allow-list. An undeclared name logs a warning,
//     stores nothing and still returns true; the first get for it then throws.
//   - Exports.get(resource, name) returns one live value (the real reference,
//     not a copy) and throws when the resource is not running or the name was
//     never stored.
//   - Imports.get(resource) returns a fresh object of every current export, an
//     empty object when there are none, and throws when the resource is gone.
//   - A read of a resource that is not a declared dependency warns once per
//     read; declared dependencies and self-reads stay silent.
//   - Stopping a resource clears its exports and makes its facade inert, so a
//     reload never hands a consumer a previous generation's value.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');
const { ResourceServices } = require('./services.cjs');
const { normalizeManifest } = require('./manifest.cjs');
const { ResourceHost } = require('./host.cjs');
const { BrowserResourceRuntime } = require('../../../../Client/resources/runtime.cjs');

// A bare services core, exercising the registry facade directly without the
// surrounding native host. This is the unit the client runtime reuses.
function services() {
  const logs = [];
  const core = new ResourceServices({
    log: (owner, message) => logs.push([owner, message]),
  });
  const add = (name, kcds = {}) => core.add(name, normalizeManifest({ name, kcds }));
  const dependencyWarnings = () =>
    logs.filter(([, message]) => /Undeclared resource dependency/.test(message));
  return { core, add, logs, dependencyWarnings };
}

test('Imports.get returns every current export keyed by name and Exports.get reads one live value', () => {
  const { add } = services();
  const library = add('library', { exports: ['config', 'pay'] });
  const shop = add('shop', { requires: ['library'] });
  const config = { currency: 'groschen' };
  const pay = (amount) => amount - 1;

  assert.equal(library.Exports.register('config', config), true);
  assert.equal(library.Exports.register('pay', pay), true);

  // A single export is the real reference, not a copy, and a function stays
  // callable across the resource boundary.
  assert.equal(shop.Exports.get('library', 'config'), config);
  assert.equal(shop.Exports.get('library', 'pay')(10), 9);

  // The bulk import contains exactly the registered names.
  const imported = shop.Imports.get('library');
  assert.deepEqual(Object.keys(imported).sort(), ['config', 'pay']);
  assert.equal(imported.config, config);
  assert.equal(imported.pay, pay);
});

test('Imports.get returns a fresh container each call while exported objects stay live and mutable', () => {
  const { add } = services();
  const library = add('library', { exports: ['state'] });
  const shop = add('shop', { requires: ['library'] });
  const state = { balance: 0 };
  library.Exports.register('state', state);

  const first = shop.Imports.get('library');
  const second = shop.Imports.get('library');
  assert.notEqual(first, second); // a new object is built per call
  assert.equal(first.state, second.state); // pointing at the same live value

  // The export is shared, not copied: whoever holds it can change it. This is
  // why the guide tells libraries to export functions, not mutable state.
  first.state.balance = 500;
  assert.equal(shop.Exports.get('library', 'state').balance, 500);
});

test('a running resource with no exports imports as an empty object and a missing export throws', () => {
  const { add } = services();
  add('library', { exports: ['declared'] }); // declared in the manifest but never registered
  const shop = add('shop', { requires: ['library'] });

  assert.deepEqual(shop.Imports.get('library'), {});
  assert.throws(() => shop.Exports.get('library', 'declared'), /Export not found/);
  assert.throws(() => shop.Exports.get('library', 'never-declared'), /Export not found/);
});

test('registering a name outside the manifest allow-list warns, stores nothing and still returns true', () => {
  const { add, logs } = services();
  const library = add('library', { exports: ['allowed'] });
  const shop = add('shop', { requires: ['library'] });

  assert.equal(library.Exports.register('allowed', 1), true);
  // Accepted at the API surface (returns true) but dropped from storage.
  assert.equal(library.Exports.register('forbidden', 2), true);

  assert.equal(shop.Exports.get('library', 'allowed'), 1);
  assert.throws(() => shop.Exports.get('library', 'forbidden'), /Export not found/);
  assert.deepEqual(shop.Imports.get('library'), { allowed: 1 });
  assert.ok(
    logs.some(
      ([owner, message]) =>
        owner === 'library' && /Undeclared export ignored: forbidden/.test(message),
    ),
  );
});

test('re-registering a declared export replaces the stored value', () => {
  const { add } = services();
  const library = add('library', { exports: ['version'] });
  const shop = add('shop', { requires: ['library'] });

  library.Exports.register('version', 1);
  assert.equal(shop.Exports.get('library', 'version'), 1);

  library.Exports.register('version', 2);
  assert.equal(shop.Exports.get('library', 'version'), 2);
  assert.deepEqual(shop.Imports.get('library'), { version: 2 });
});

test('reads warn only for undeclared dependencies, never for declared ones or self-reads', () => {
  const { add, dependencyWarnings } = services();
  const library = add('library', { exports: ['api'] });
  const shop = add('shop', { requires: ['library'] });
  const snoop = add('snoop'); // declares no dependencies
  library.Exports.register('api', 42);

  // A declared dependency reads silently.
  shop.Exports.get('library', 'api');
  shop.Imports.get('library');
  assert.equal(dependencyWarnings().length, 0);

  // Reading your own exports never warns.
  library.Exports.get('library', 'api');
  library.Imports.get('library');
  assert.equal(dependencyWarnings().length, 0);

  // An undeclared dependency warns once for that resource, not on every read.
  snoop.Exports.get('library', 'api');
  snoop.Imports.get('library');
  snoop.Imports.get('library');
  assert.equal(dependencyWarnings().length, 1);
});

test('both Exports.get and Imports.get throw for a resource that is not running', () => {
  const { add } = services();
  const shop = add('shop');

  assert.throws(() => shop.Exports.get('ghost', 'value'), /Resource is not available/);
  assert.throws(() => shop.Imports.get('ghost'), /Resource is not available/);
});

test('stopping a resource clears its exports, disables its facade and reloads start empty', () => {
  const { core, add } = services();
  const library = add('library', { exports: ['value'] });
  const shop = add('shop', { requires: ['library'] });
  library.Exports.register('value', 'first');
  assert.equal(shop.Exports.get('library', 'value'), 'first');

  core.remove('library'); // unload / hot-reload cleanup

  // Consumers can no longer read the stopped resource.
  assert.throws(() => shop.Exports.get('library', 'value'), /Resource is not available/);
  assert.throws(() => shop.Imports.get('library'), /Resource is not available/);

  // The stopped resource's own facade is inert: it can neither register nor read.
  assert.throws(() => library.Exports.register('value', 'stale'), /Resource is stopped/);
  assert.throws(() => library.Exports.get('library', 'value'), /Resource is stopped/);

  // A fresh generation starts with no exports; the old value never leaks back.
  const reloaded = add('library', { exports: ['value'] });
  assert.deepEqual(shop.Imports.get('library'), {});
  assert.throws(() => shop.Exports.get('library', 'value'), /Export not found/);
  reloaded.Exports.register('value', 'second');
  assert.equal(shop.Exports.get('library', 'value'), 'second');
});

// A real ResourceHost over on-disk resource folders, exercising the registry
// end to end through script loading, dependency ordering and hot reload.
function host(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-registry-'));
  const logs = [];
  const instance = new ResourceHost(root, { send() {}, log: (...entry) => logs.push(entry) });
  const write = (name, code, kcds = {}) => {
    const folder = path.join(root, name);
    fs.mkdirSync(folder, { recursive: true });
    fs.writeFileSync(
      path.join(folder, 'package.json'),
      JSON.stringify({ name, kcds: { serverScripts: ['main.js'], ...kcds } }),
    );
    fs.writeFileSync(path.join(folder, 'main.js'), code);
  };
  t.after(async () => {
    await instance.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  });
  return { host: instance, write, logs };
}

test('real host: a dependent reads a library function export and ensure() reloads a fresh generation', async (t) => {
  const { host: instance, write, logs } = host(t);
  write(
    'library',
    [
      "kcds.core.Exports.register('greeting', 'v1');",
      "kcds.core.Exports.register('shout', (text) => text.toUpperCase());",
    ].join('\n'),
    { exports: ['greeting', 'shout'] },
  );
  write(
    'shop',
    [
      "kcds.core.Events.on('resourceStart', (name) => {",
      "  if (name !== 'shop') return;",
      "  const shout = kcds.core.Exports.get('library', 'shout');",
      "  console.log(kcds.core.Exports.get('library', 'greeting') + ':' + shout('hi'));",
      '});',
    ].join('\n'),
    { requires: ['library'] },
  );
  // Reads the library without declaring the dependency, so it must warn: once, however often it
  // reads it. One that lists it as optional never warns.
  write(
    'snoop',
    "kcds.core.Imports.get('library'); kcds.core.Imports.get('library'); kcds.core.Imports.get('library');",
    {},
  );
  write('optional', "kcds.core.Imports.get('library');", {
    requires: [{ name: 'library', optional: true }],
  });

  await instance.startAll();
  assert.ok(logs.some(([name, value]) => name === 'shop' && value === 'v1:HI'));
  assert.equal(
    logs.filter(
      ([name, message]) => name === 'snoop' && /Undeclared resource dependency/.test(message),
    ).length,
    1,
  );
  assert.ok(!logs.some(([name, message]) => name === 'optional' && /Undeclared/.test(message)));

  // Hot reload: rewrite the library, then ensure() restarts it together with
  // its dependents, which must observe the new generation's exports.
  write(
    'library',
    [
      "kcds.core.Exports.register('greeting', 'v2');",
      "kcds.core.Exports.register('shout', (text) => '!' + text.toUpperCase());",
    ].join('\n'),
    { exports: ['greeting', 'shout'] },
  );
  await instance.ensure('library');
  assert.ok(logs.some(([name, value]) => name === 'shop' && value === 'v2:!HI'));
});

// The browser client runtime binds the very same ResourceServices facade the
// server host does, so the registry contract must hold identically through the
// client's module loader, dependency ordering and hot-reload (apply) path. This
// fixture mirrors client_runtime.test.cjs: resources are delivered as a catalog
// of hashed in-memory files and evaluated in a vm context, never touching disk.
function clientRuntime(t) {
  const messages = [];
  const logs = [];
  const files = new Map();
  const runtime = new BrowserResourceRuntime({
    send: (value) => messages.push(value),
    log: (name, message) => logs.push([name, message]),
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
  const resource = (name, source, kcds = {}, hash = name) => {
    files.set(name + '/main.js', new TextEncoder().encode(source));
    return {
      hash,
      manifest: normalizeManifest({ name, kcds: { clientScripts: ['main.js'], ...kcds } }),
      files: [{ name: 'main.js' }],
    };
  };
  const read = (name, file) => {
    const bytes = files.get(name + '/' + file);
    if (!bytes) throw new Error('Unknown file');
    return bytes;
  };
  const dependencyWarnings = () =>
    logs.filter(([, message]) => /Undeclared resource dependency/.test(message));
  t.after(() => runtime.disconnect());
  return { runtime, resource, read, messages, logs, dependencyWarnings };
}

test('client runtime: declared consumer reads live exports across the resource boundary, undeclared read warns', async (t) => {
  const f = clientRuntime(t);
  const library = f.resource(
    'library',
    [
      "kcds.core.Exports.register('greeting', 'v1');",
      "kcds.core.Exports.register('shout', (text) => text.toUpperCase());",
    ].join('\n'),
    { exports: ['greeting', 'shout'] },
  );
  // A declared dependency reads one value and one function, plus the bulk import.
  const shop = f.resource(
    'shop',
    [
      "const shout = kcds.core.Exports.get('library', 'shout');",
      "kcds.core.Events.toServer('read', kcds.core.Exports.get('library', 'greeting') + ':' + shout('hi'));",
      "kcds.core.Events.toServer('keys', Object.keys(kcds.core.Imports.get('library')).sort().join(','));",
    ].join('\n'),
    { requires: ['library'] },
  );
  // Reads the library without declaring it as a dependency: must warn once.
  const snoop = f.resource('snoop', "kcds.core.Imports.get('library');", {});

  await f.runtime.apply({ resources: [library, shop, snoop] }, f.read);

  // A plain string travels as a JSON string.
  assert.equal(JSON.parse(f.messages.find((m) => m.name === 'read').payload), 'v1:HI');
  assert.equal(JSON.parse(f.messages.find((m) => m.name === 'keys').payload), 'greeting,shout');
  assert.equal(f.dependencyWarnings().length, 1);
  assert.equal(f.dependencyWarnings()[0][0], 'snoop');
});

test('client runtime: hot reload restarts consumers on a fresh generation and never leaks a stale export', async (t) => {
  const f = clientRuntime(t);
  const library = f.resource('library', "kcds.core.Exports.register('version', 'v1');", {
    exports: ['version'],
  });
  // The consumer re-reads on every 'poll' so we can observe the value both
  // before and after the library is swapped underneath it.
  const shop = f.resource(
    'shop',
    "kcds.core.Events.on('poll', () => kcds.core.Events.toServer('value', kcds.core.Exports.get('library', 'version')));",
    { requires: ['library'] },
  );
  await f.runtime.apply({ resources: [library, shop] }, f.read);
  await f.runtime.services.emit('shared', 'poll', []);

  // Swap the library for a new generation (new hash => reload). Its dependent is
  // restarted even though shop's own file is unchanged, and must bind the new
  // value; the previous generation's export must not survive the reload.
  const reloaded = f.resource(
    'library',
    "kcds.core.Exports.register('version', 'v2');",
    {
      exports: ['version'],
    },
    'library-v2',
  );
  await f.runtime.apply({ resources: [reloaded, shop] }, f.read);
  await f.runtime.services.emit('shared', 'poll', []);

  assert.deepEqual(
    f.messages.filter((m) => m.name === 'value').map((m) => JSON.parse(m.payload)),
    ['v1', 'v2'],
  );

  // Dropping the library from the catalog unloads it together with its
  // dependent; a later read of the gone resource throws, never a cached value.
  await f.runtime.apply({ resources: [] }, f.read);
  assert.equal(f.runtime.resources.size, 0);
  const orphan = f.resource('orphan', '', {});
  await f.runtime.apply({ resources: [orphan] }, f.read);
  const facade = f.runtime.services.facade(f.runtime.services.owners.get('orphan'));
  assert.throws(() => facade.Exports.get('library', 'version'), /Resource is not available/);
  assert.throws(() => facade.Imports.get('library'), /Resource is not available/);
});
