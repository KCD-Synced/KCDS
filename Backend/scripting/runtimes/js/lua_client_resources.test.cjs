'use strict';

// Lua client resources: a resource whose client scripts are .lua runs in the player's
// resource worker in its own Lua state over the same client SDK a JavaScript resource gets.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');
const { execFileSync } = require('node:child_process');
const { normalizeManifest } = require('./manifest.cjs');
const { BrowserResourceRuntime } = require('../../../../Client/resources/runtime.cjs');

const ROOT = path.resolve(__dirname, '../../../..');

// A client runtime over an in-memory catalog, as the guide scenarios drive it.
function clientRuntime(t, Runtime = BrowserResourceRuntime) {
  const sent = [];
  const logs = [];
  const files = new Map();
  const runtime = new Runtime({
    send: (message) => sent.push(message),
    log: (resource, text) => logs.push({ resource, text: String(text) }),
  });
  const resource = (name, sources, kcds = { client: 'main.lua' }) => {
    for (const [file, code] of Object.entries(sources)) {
      files.set(`${name}/${file}`, new TextEncoder().encode(code));
    }
    return {
      hash: name,
      manifest: normalizeManifest({ name, kcds }),
      files: Object.keys(sources).map((file) => ({ name: file })),
    };
  };
  const read = (name, file) => {
    const bytes = files.get(`${name}/${file}`);
    if (!bytes) {
      throw new Error('Unknown file');
    }
    return bytes;
  };
  const linesOf = (name) => logs.filter((entry) => entry.resource === name).map((e) => e.text);
  t.after(() => runtime.disconnect());
  return { runtime, resource, read, sent, linesOf };
}

function waitFor(predicate, what) {
  const end = Date.now() + 3000;
  return new Promise((resolve, reject) => {
    const check = () => {
      if (predicate()) {
        resolve();
      } else if (Date.now() > end) {
        reject(new Error(`Timed out waiting for ${what}`));
      } else {
        setTimeout(check, 5);
      }
    };
    check();
  });
}

const GREETER = {
  'main.lua': `
    local util = require("util")
    local v = kcds.math.Vector3.new(3, 4, 0)
    print("length", v.length, util.answer)
    kcds.core.Events.on("luaPing", function(value)
      kcds.core.Events.toServer("luaPong", value * 2)
    end)
    setTimeout(function() print("timer") end, 5)
  `,
  'util.lua': 'return { answer = 42 }',
};

test('a Lua client resource uses the client SDK: classes, events to the server, timers', async (t) => {
  const f = clientRuntime(t);
  await f.runtime.apply({ resources: [f.resource('greeter', GREETER)] }, f.read);
  await waitFor(() => f.linesOf('greeter').includes('timer'), 'the Lua timer');
  assert.ok(f.linesOf('greeter').includes('length\t5\t42'), f.linesOf('greeter').join('\n'));
  await f.runtime.services.emit('shared', 'luaPing', [21]);
  const pong = f.sent.find((message) => message.op === 'event' && message.name === 'luaPong');
  // The payload travels as JSON text, as a JavaScript resource's does.
  assert.equal(pong?.payload, '42');
  await f.runtime.stop('greeter');
  assert.ok(f.linesOf('greeter').includes('client resource stopped'));
});

test('a client Lua require reaches only files the server sent', async (t) => {
  const f = clientRuntime(t);
  const resource = f.resource('lonely', { 'main.lua': 'require("missing")' });
  await assert.rejects(f.runtime.apply({ resources: [resource] }, f.read), /missing.lua/);
});

test('parity: Lua reads every SDK member a JavaScript client resource has', async (t) => {
  const f = clientRuntime(t);
  await f.runtime.apply({ resources: [f.resource('parity', { 'main.lua': '' })] }, f.read);
  const state = f.runtime.resources.get('parity');
  const engine = state.lua.engine;
  const kcds = engine.global.get('kcds');
  assert.ok(kcds, 'the Lua state has kcds');
  // The JavaScript tree this resource got is the scope the Lua binding was made over.
  const scope = state.lua.scope.kcds;
  const paths = [];
  for (const domain of Object.keys(scope)) {
    for (const name of Object.keys(scope[domain])) {
      paths.push([domain, name]);
    }
  }
  assert.ok(paths.length > 40, `only ${paths.length} members`);
  engine.global.set('__paths', paths);
  const seen = await engine.doString(`
    local out = {}
    for i = 1, #__paths do
      local path = __paths[i]
      local ok, value = pcall(function() return kcds[path[1]][path[2]] end)
      out[i] = ok and value ~= nil
    end
    return out
  `);
  const missing = paths.filter((keys, index) => {
    const value = scope[keys[0]][keys[1]];
    return (value !== undefined && value !== null) !== seen[index];
  });
  assert.deepEqual(missing, []);
});

// The worker bundle the game really runs (Dev/tools/build_resource_runtime.py): its modules come
// from the bundle, and it has no Node: no require, __dirname, process or files.
function bundledModules(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-lua-bundle-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const header = path.join(directory, 'runtime.hpp');
  const python = process.platform === 'win32' ? 'python' : 'python3';
  execFileSync(python, [path.join(ROOT, 'Dev/tools/build_resource_runtime.py'), header]);
  const pieces = fs.readFileSync(header, 'utf8').matchAll(/R"KCDS\(([\s\S]*?)\)KCDS"/g);
  const html = [...pieces]
    .map((piece) => piece[1])
    .join('')
    .replaceAll('<\\/script', '</script');
  const start = html.indexOf('new Blob([') + 'new Blob(['.length;
  const end = html.indexOf("], { type: 'text/javascript' })", start);
  const bootstrap = JSON.parse(html.slice(start, end));
  const entry = "load('Client/resources/worker.cjs');";
  assert.ok(bootstrap.includes(entry));
  const context = vm.createContext({
    atob,
    console,
    setTimeout,
    clearTimeout,
    setInterval,
    clearInterval,
    queueMicrotask,
    TextEncoder,
    TextDecoder,
    URL,
    // A dedicated worker started from a blob, as the runtime page starts it.
    location: { href: 'blob:https://kcds.invalid/worker' },
    importScripts() {
      throw new Error('The resource worker loads no scripts');
    },
    fetch() {
      throw new Error('The resource worker has no network');
    },
  });
  context.self = context;
  vm.runInContext(bootstrap.replace(entry, 'globalThis.loadModule = load;'), context);
  return (name) => context.loadModule(name);
}

test('the worker bundle runs a Lua client resource with the engine it carries', async (t) => {
  const load = bundledModules(t);
  assert.ok(
    load('Backend/scripting/runtimes/js/languages/vendor/wasmoon/glue.wasm').length > 100000,
  );
  const { BrowserResourceRuntime: Bundled } = load('Client/resources/runtime.cjs');
  const f = clientRuntime(t, Bundled);
  await f.runtime.apply({ resources: [f.resource('greeter', GREETER)] }, f.read);
  await waitFor(() => f.linesOf('greeter').includes('timer'), 'the Lua timer');
  assert.ok(f.linesOf('greeter').includes('length\t5\t42'), f.linesOf('greeter').join('\n'));
});

// From the resource folder to the player's runtime as a server sends it: the catalog players get
// (its manifest, its files) and the files' bytes from the store.
test('a Lua client resource goes through the server catalog and runs in the player runtime', async (t) => {
  const { ResourceBundleStore } = require('./bundles.cjs');
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-lua-catalog-'));
  t.after(() => fs.rmSync(folder, { recursive: true, force: true }));
  fs.mkdirSync(path.join(folder, 'client'));
  fs.writeFileSync(path.join(folder, 'server.lua'), 'print("server")');
  fs.writeFileSync(
    path.join(folder, 'client/main.lua'),
    'print("client says", require("client.util").answer)',
  );
  fs.writeFileSync(path.join(folder, 'client/util.lua'), 'return { answer = 42 }');
  const source = { name: 'shipped', kcds: { server: 'server.lua', client: 'client/main.lua' } };
  const store = new ResourceBundleStore();
  store.report = (text) => assert.fail(text);
  store.publish([{ folder, manifest: normalizeManifest(source) }]);
  // The player's game checks and downloads the catalog (Client/resources/bundle_cache.cjs).
  const { BundleCache } = require('../../../../Client/resources/bundle_cache.cjs');
  const { digest } = require('./bundles.cjs');
  const cache = new BundleCache(async (bytes) => digest(bytes));
  const download = async ({ revision, hash, offset, length }) => {
    const chunk = store.chunk(revision, hash, offset, length);
    return { ...chunk, bytes: Uint8Array.from(Buffer.from(chunk.data, 'base64')) };
  };
  const catalog = store.manifest();
  await cache.acquire(catalog, download);
  const f = clientRuntime(t);
  await f.runtime.apply({ resources: catalog.resources }, (name, file) => cache.read(name, file));
  assert.ok(f.linesOf('shipped').includes('client says\t42'), f.linesOf('shipped').join('\n'));
});
