'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const { normalizeManifest } = require('./manifest.cjs');
const {
  BrowserResourceRuntime,
  resolveModule,
} = require('../../../../Client/resources/runtime.cjs');

function fixture(t) {
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
  return { runtime, resource, read, files, messages };
}

test('compiled client/shared code starts in dependency order and uses local services', async (t) => {
  const f = fixture(t);
  const library = f.resource(
    'library',
    'kcds.core.Exports.register("answer", () => new kcds.math.Vector3(1,2,3).length);',
    { exports: ['answer'] },
  );
  const mode = f.resource(
    'mode',
    `
    if (!kcds.core.Runtime.isClient || kcds.core.Runtime.isServer) throw Error('wrong side');
    if (typeof process !== 'undefined') throw Error('Node leaked');
    kcds.core.Events.toServer('answer', kcds.core.Exports.get('library','answer')());
    kcds.core.Events.on('resourceStop', name => { if(name==='mode') kcds.core.Events.toServer('stopped'); });
  `,
    { requires: ['library'] },
  );
  await f.runtime.apply({ resources: [mode, library] }, f.read);
  assert.deepEqual([...f.runtime.resources.keys()], ['library', 'mode']);
  assert.equal(f.messages[0].name, 'answer');
  assert.ok(Math.abs(Number(f.messages[0].payload) - Math.sqrt(14)) < 1e-10);
  await f.runtime.disconnect();
  assert.equal(f.messages.at(-1).name, 'stopped');
  assert.equal(f.runtime.resources.size, 0);
});

test('the animation clips a resource ships go to the game client once', async (t) => {
  const f = fixture(t);
  const mode = f.resource('mode', '');
  mode.files.push({ name: 'clips/wave.caf' }, { name: 'clips/notes.txt' });
  f.files.set('mode/clips/wave.caf', new Uint8Array([1, 2, 3]));
  f.files.set('mode/clips/notes.txt', new Uint8Array([4]));
  await f.runtime.apply({ resources: [mode] }, f.read);
  const shipped = f.messages.filter((m) => m.op === 'nativeFile');
  assert.deepEqual(
    shipped.map(({ resource, path, offset, total }) => ({ resource, path, offset, total })),
    [{ resource: 'mode', path: 'clips/wave.caf', offset: 0, total: 3 }],
  );
  // The same content is not sent again when the catalog comes again.
  await f.runtime.apply({ resources: [mode] }, f.read);
  assert.equal(f.messages.filter((m) => m.op === 'nativeFile').length, 1);
});

test('relative CommonJS resolves extensions and never escapes the delivered resource', async (t) => {
  const f = fixture(t);
  const mode = f.resource(
    'mode',
    'kcds.core.Events.toServer("helper", require("./helper").answer);',
  );
  mode.files.push({ name: 'helper.js' });
  f.files.set('mode/helper.js', new TextEncoder().encode('exports.answer=42;'));
  await f.runtime.apply({ resources: [mode] }, f.read);
  assert.equal(f.messages[0].payload, '42');
  assert.throws(() => resolveModule('../secret', 'main.js'), /escapes/);
  assert.throws(() => resolveModule('node:fs', 'main.js'), /relative/);
  assert.throws(() => resolveModule('./C:/secret', 'main.js'), /Invalid/);
});

test('changed dependency restarts unchanged consumers, unchanged catalog does not duplicate handlers', async (t) => {
  const f = fixture(t);
  const library = f.resource('library', 'kcds.core.Exports.register("answer", 1);', {
    exports: ['answer'],
  });
  const mode = f.resource(
    'mode',
    'kcds.core.Events.on("count",()=> kcds.core.Events.toServer("value", kcds.core.Exports.get("library","answer")));',
    { requires: ['library'] },
  );
  await f.runtime.apply({ resources: [library, mode] }, f.read);
  await f.runtime.apply({ resources: [library, mode] }, f.read);
  assert.equal(
    f.runtime.services.facade(f.runtime.services.owners.get('mode')).Events.listenerCount('count'),
    1,
  );
  const replacement = f.resource(
    'library',
    'kcds.core.Exports.register("answer", 2);',
    { exports: ['answer'] },
    'library-v2',
  );
  await f.runtime.apply({ resources: [replacement, mode] }, f.read);
  await f.runtime.services.emit('shared', 'count', []);
  assert.deepEqual(
    f.messages.map((message) => message.payload),
    ['2'],
  );
});

test('failed starts clean subscriptions and timers; invalid dependencies start nothing', async (t) => {
  const f = fixture(t);
  const bad = f.resource(
    'bad',
    'kcds.core.Events.on("leak",()=>{}); setInterval(()=>{},1); throw Error("bad start");',
  );
  await assert.rejects(f.runtime.apply({ resources: [bad] }, f.read), /bad start/);
  assert.equal(f.runtime.resources.size, 0);
  assert.equal(f.runtime.services.handlers.size, 0);
  const missing = f.resource('missing', 'throw Error("must not execute");', {
    requires: ['not-installed'],
  });
  await assert.rejects(
    f.runtime.apply({ resources: [missing] }, f.read),
    /dependency|resource missing/,
  );
  assert.equal(f.runtime.resources.size, 0);
});

test('production module evaluation preserves live getters and CommonJS this', async (t) => {
  let local = null;
  const messages = [];
  const runtime = new BrowserResourceRuntime({
    send: (message) => messages.push(message),
    nativeAPI: () =>
      Object.defineProperty({}, 'LocalPlayer', {
        enumerable: true,
        get: () => local,
      }),
  });
  t.after(() => runtime.disconnect());
  const definition = {
    hash: 'live-scope',
    manifest: normalizeManifest({ name: 'scope', kcds: { clientScripts: ['main.js'] } }),
    files: [{ name: 'main.js' }],
  };
  const source = new TextEncoder().encode(`
    if(this!==module.exports) throw Error('CommonJS this is wrong');
    kcds.core.Events.on('sample',()=> kcds.core.Events.toServer('current',kcds.players.LocalPlayer?.id ?? null));
  `);
  await runtime.apply({ resources: [definition] }, () => source);
  await runtime.services.emit('shared', 'sample', []);
  local = { id: 42 };
  await runtime.services.emit('shared', 'sample', []);
  local = null;
  await runtime.services.emit('shared', 'sample', []);
  assert.deepEqual(
    messages.map((message) => message.payload),
    ['null', '42', 'null'],
  );
});

test('assigning the local player transform asks the server; other entities stay read-only', async (t) => {
  const f = fixture(t);
  const pose = { x: 1, y: 2, z: 3 };
  const identity = { w: 1, x: 0, y: 0, z: 0 };
  f.runtime.entities.updateDirectory({
    localPlayerId: 7,
    entities: [
      { id: 7, nativeId: 70, kind: 'player', dimension: 0, position: pose, rotation: identity },
      { id: 8, nativeId: 80, kind: 'player', dimension: 0, position: pose, rotation: identity },
    ],
  });
  const mode = f.resource(
    'mode',
    `
    kcds.players.LocalPlayer.position = new kcds.math.Vector3(5, 6, 7);
    kcds.players.LocalPlayer.rotation = new kcds.math.Quaternion(0, 0, 0, 2);
    try {
      new kcds.players.Entity(8).position = new kcds.math.Vector3(0, 0, 0);
    } catch (error) {
      kcds.core.Events.toServer('blocked', error.message);
    }
  `,
  );
  await f.runtime.apply({ resources: [mode] }, f.read);
  const transforms = f.messages.filter((message) => message.op === 'entityTransform');
  assert.deepEqual(transforms, [
    { op: 'entityTransform', id: 7, field: 'position', x: 5, y: 6, z: 7 },
    { op: 'entityTransform', id: 7, field: 'rotation', x: 0, y: 0, z: 2, w: 0 },
  ]);
  const blocked = f.messages.find((message) => message.name === 'blocked');
  assert.match(blocked.payload, /only move the local player/);
});
