'use strict';

// Lua server resources: a resource whose server scripts are .lua runs in its own Lua
// state over the same SDK a JavaScript resource gets.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');
const { normalizeManifest } = require('./manifest.cjs');
const { moduleFile } = require('./languages/lua_host.cjs');

function luaResource(root, name, files, kcds = { server: 'main.lua' }) {
  const directory = path.join(root, name);
  fs.mkdirSync(directory, { recursive: true });
  fs.writeFileSync(path.join(directory, 'package.json'), JSON.stringify({ name, kcds }));
  for (const [file, code] of Object.entries(files)) {
    fs.mkdirSync(path.dirname(path.join(directory, file)), { recursive: true });
    fs.writeFileSync(path.join(directory, file), code);
  }
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

test('the manifest infers each side language from its scripts', () => {
  const lua = normalizeManifest({ name: 'a', kcds: { server: 'main.lua', client: 'c.js' } });
  assert.deepEqual(lua.kcds.language, { server: 'lua', client: 'javascript' });
  const none = normalizeManifest({ name: 'b' });
  assert.deepEqual(none.kcds.language, { server: 'javascript', client: 'javascript' });
  const named = normalizeManifest({ name: 'c', kcds: { language: 'lua' } });
  assert.deepEqual(named.kcds.language, { server: 'lua', client: 'lua' });
  assert.throws(
    () => normalizeManifest({ name: 'd', kcds: { serverScripts: ['a.lua', 'b.js'] } }),
    /mix languages/,
  );
  assert.throws(
    () => normalizeManifest({ name: 'e', kcds: { language: 'lua', server: 'main.js' } }),
    /main.js is javascript, but the server side is lua/,
  );
  assert.throws(
    () => normalizeManifest({ name: 'f', kcds: { language: 'cobol' } }),
    /'cobol' is not one of javascript, lua, csharp/,
  );
});

test('a module name is a file of the resource and nothing else', () => {
  assert.equal(moduleFile('util'), 'util.lua');
  assert.equal(moduleFile('shared.math'), 'shared/math.lua');
  assert.equal(moduleFile('../secret'), null);
  assert.equal(moduleFile('a/b'), null);
  assert.equal(moduleFile(''), null);
});

test('a Lua resource uses the SDK: classes, statics, events, require and timers', async (t) => {
  const f = serverFixture(t);
  luaResource(f.root, 'greeter', {
    'main.lua': `
      local util = require("shared.util")
      local Vector3 = kcds.math.Vector3
      local v = Vector3.new(1, 2, 2)
      print("length", v.length, util.double(21), v:distance(Vector3.zero()))
      print("same module", require("shared.util") == util)
      print("players", #kcds.players.Player.all(), kcds.world.Gathering.regrowTime == nil)
      kcds.core.Events.on("playerConnect", function(player)
        print("joined", player.id, player.nickname)
        kcds.core.Chat.sendToPlayer(player, "welcome", { color = 0xff0000ff })
      end)
      kcds.core.Events.on("luaPing", function(a, b)
        print("ping", a + b)
      end)
      setTimeout(function() print("timer") end, 5)
      local ok, problem = pcall(kcds.core.Chat.sendToAll, 5)
      print("pcall", ok, tostring(problem))
    `,
    'shared/util.lua': `
      local M = {}
      function M.double(x) return x * 2 end
      return M
    `,
  });
  await f.host.startAll();
  const lines = () => f.linesOf('greeter');
  await waitFor(() => lines().includes('timer'), 'the Lua timer');
  assert.ok(lines().includes('length\t3\t42\t3'), lines().join('\n'));
  assert.ok(lines().includes('same module\ttrue'));
  assert.ok(lines().includes('players\t0\ttrue'), lines().join('\n'));
  assert.ok(lines().includes('pcall\tfalse\tTypeError: Chat text must be string'));
  f.report(7);
  await waitFor(() => lines().some((line) => line.startsWith('joined')), 'the join handler');
  assert.ok(lines().includes('joined\t7\tPlayer 7'), lines().join('\n'));
  const welcome = f.sent.find((m) => m.op === 'chat' && m.target === 7);
  assert.equal(welcome?.text, 'welcome');
  const api = f.host.makeAPI();
  api.Events.emit('luaPing', 2, 3);
  await waitFor(() => lines().includes('ping\t5'), 'the local event');
});

test('stopping a Lua resource drops its handlers and timers', async (t) => {
  const f = serverFixture(t);
  luaResource(f.root, 'ticker', {
    'main.lua': `
      kcds.core.Events.on("luaTick", function() print("tick") end)
      setInterval(function() print("interval") end, 5)
    `,
  });
  await f.host.startAll();
  const lines = () => f.linesOf('ticker');
  await waitFor(() => lines().includes('interval'), 'the interval');
  await f.host.stop('ticker');
  const before = lines().length;
  const api = f.host.makeAPI();
  api.Events.emit('luaTick');
  await new Promise((resolve) => setTimeout(resolve, 40));
  assert.equal(lines().length, before);
});

test('Lua reaches no files, processes, environment, bytecode or debug library', async (t) => {
  const f = serverFixture(t);
  luaResource(f.root, 'boxed', {
    'main.lua': `
      print("libs", type(io), type(os.exit), type(os.execute), type(os.getenv), type(debug))
      print("loaders", type(dofile), type(loadfile), type(package))
      print("kept", type(os.time()), type(string.format), type(table.concat), type(math.floor))
      local chunk = load(string.dump(function() return 1 end))
      print("bytecode", chunk == nil)
    `,
  });
  await f.host.startAll();
  const lines = f.linesOf('boxed');
  assert.ok(lines.includes('libs\tnil\tnil\tnil\tnil\tnil'), lines.join('\n'));
  assert.ok(lines.includes('loaders\tnil\tnil\tnil'));
  assert.ok(lines.includes('kept\tnumber\tfunction\tfunction\tfunction'));
  assert.ok(lines.includes('bytecode\ttrue'));
});

test('a file may wait for a promise at its top level, and an endless loop is stopped', async (t) => {
  const f = serverFixture(t);
  luaResource(f.root, 'waiter', {
    'main.lua': `
      print("before")
      local value = __test_wait():await()
      print("after", value)
      kcds.core.Events.on("luaSpin", function()
        while true do end
      end)
    `,
  });
  luaResource(f.root, 'spinner', { 'main.lua': 'while true do end' });
  const { LuaResource } = require('./languages/lua_host.cjs');
  const start = LuaResource.prototype.start;
  t.after(() => {
    LuaResource.prototype.start = start;
  });
  LuaResource.prototype.start = async function startWithWait() {
    await start.call(this);
    this.engine.global.set('__test_wait', () => {
      return new Promise((resolve) => setTimeout(() => resolve(7), 30));
    });
    return this;
  };
  await f.host.startAll();
  await waitFor(() => f.linesOf('waiter').includes('after\t7'), 'the awaited value');
  assert.equal(f.host.resources.has('spinner'), false);
  assert.ok(
    f.linesOf('spinner').some((line) => /timeout/.test(line)),
    f.linesOf('spinner').join('\n'),
  );
  const api = f.host.makeAPI();
  const began = Date.now();
  await assert.rejects(api.Events.emit('luaSpin'), (error) => {
    return error instanceof AggregateError && /timeout/.test(String(error.errors[0]));
  });
  assert.ok(Date.now() - began < 8000);
});

// The members a value has as a script reads them: its own and its prototypes', without the
// language's own (Object.prototype, Function.prototype, a class's length/name/prototype).
function membersOf(value) {
  const names = new Set();
  const skip = new Set(['constructor', 'length', 'name', 'prototype', 'caller', 'arguments']);
  let object = value;
  while (object && object !== Object.prototype && object !== Function.prototype) {
    for (const key of Object.getOwnPropertyNames(object)) {
      if (!skip.has(key) && !key.startsWith('_')) {
        names.add(key);
      }
    }
    object = Object.getPrototypeOf(object);
  }
  return [...names].sort();
}

// What Lua's type() answers for a JavaScript value as the binding hands it over.
function luaTypeOf(value) {
  if (value === undefined || value === null) {
    return 'nil';
  }
  if (typeof value === 'function') {
    return /^class\b/.test(Function.prototype.toString.call(value)) ? 'userdata' : 'function';
  }
  if (typeof value === 'object') {
    return 'userdata';
  }
  return typeof value === 'bigint' ? 'number' : typeof value;
}

function readPath(root, keys) {
  try {
    let value = root;
    for (const key of keys) {
      value = value[key];
    }
    return luaTypeOf(value);
  } catch {
    return 'error';
  }
}

test('parity: Lua reads every SDK member a JavaScript server resource has', async (t) => {
  const f = serverFixture(t);
  luaResource(f.root, 'parity', { 'main.lua': '' });
  await f.host.startAll();
  const state = f.host.resources.get('parity');
  const kcds = state.vm.kcds;
  const paths = [];
  for (const domain of Object.keys(kcds)) {
    for (const name of Object.keys(kcds[domain])) {
      paths.push([domain, name]);
      const value = kcds[domain][name];
      if (value && (typeof value === 'object' || typeof value === 'function')) {
        const statics = typeof value === 'function' ? Object.getOwnPropertyNames(value) : null;
        for (const member of statics
          ? statics.filter((key) => !['length', 'name', 'prototype'].includes(key))
          : membersOf(value)) {
          paths.push([domain, name, member]);
        }
      }
    }
  }
  assert.ok(paths.length > 500, `only ${paths.length} members`);
  state.lua.engine.global.set('__paths', paths);
  const seen = await state.lua.engine.doString(`
    local out = {}
    for i = 1, #__paths do
      local path = __paths[i]
      local ok, value = pcall(function()
        local value = kcds
        for j = 1, #path do
          value = value[path[j]]
        end
        return value
      end)
      out[i] = ok and type(value) or "error"
    end
    return out
  `);
  const differences = [];
  paths.forEach((keys, index) => {
    const expected = readPath(kcds, keys);
    if (seen[index] !== expected) {
      differences.push(`${keys.join('.')}: JavaScript ${expected}, Lua ${seen[index]}`);
    }
  });
  assert.deepEqual(differences, []);
});

test('a Lua error names its file and line and stops the resource', async (t) => {
  const f = serverFixture(t);
  luaResource(f.root, 'broken', { 'main.lua': 'local x = nil\nprint(x.y)\n' });
  await f.host.startAll();
  assert.equal(f.host.resources.has('broken'), false);
  const lines = f.linesOf('broken');
  assert.ok(
    lines.some((line) => /main\.lua:2: attempt to index a nil value/.test(line)),
    lines.join('\n'),
  );
});

test('exports and events cross between JavaScript and Lua resources', async (t) => {
  const f = serverFixture(t);
  const js = path.join(f.root, 'bank');
  fs.mkdirSync(js);
  fs.writeFileSync(
    path.join(js, 'package.json'),
    JSON.stringify({ name: 'bank', kcds: { server: 'main.js', exports: ['config', 'pay'] } }),
  );
  fs.writeFileSync(
    path.join(js, 'main.js'),
    `
    kcds.core.Exports.register('config', { currency: 'groschen', coins: [1, 5, 10] });
    kcds.core.Exports.register('pay', (amount) => amount - 1);
    kcds.core.Events.on('shopSold', (item, price) => console.log('sold', item, price));
    kcds.core.Events.on('bankAsk', () => {
      const offer = kcds.core.Exports.get('shop', 'offer');
      console.log('offer', offer('sword'), JSON.stringify(kcds.core.Exports.get('shop', 'stock')));
    });
    `,
  );
  luaResource(
    f.root,
    'shop',
    {
      'main.lua': `
        local config = kcds.core.Exports.get("bank", "config")
        local pay = kcds.core.Exports.get("bank", "pay")
        print("config", config.currency, #config.coins, config.coins[3], pay(100))
        kcds.core.Exports.register("offer", function(item) return item .. " for 30" end)
        kcds.core.Exports.register("stock", { swords = 2, names = { "a", "b" } })
        kcds.core.Events.emit("shopSold", "sword", 30)
      `,
    },
    { server: 'main.lua', requires: ['bank'], exports: ['offer', 'stock'] },
  );
  await f.host.startAll();
  assert.ok(
    f.linesOf('shop').includes('config\tgroschen\t3\t10\t99'),
    f.linesOf('shop').join('\n'),
  );
  assert.ok(f.linesOf('bank').includes('sold sword 30'), f.linesOf('bank').join('\n'));
  await f.host.makeAPI().Events.emit('bankAsk');
  // A Lua table has no key order, so the stock is compared as a value.
  const offer = f.linesOf('bank').find((line) => line.startsWith('offer sword for 30 '));
  assert.ok(offer, f.linesOf('bank').join('\n'));
  assert.deepEqual(JSON.parse(offer.slice('offer sword for 30 '.length)), {
    swords: 2,
    names: ['a', 'b'],
  });
});

// A server runs for days: many calls into a Lua resource and back must not wear its state out
// (the engine's stack once overflowed after about 60 runs of code).
test('a Lua resource keeps working over many calls both ways', async (t) => {
  const f = serverFixture(t);
  luaResource(
    f.root,
    'busy',
    {
      'main.lua': `
      local count = 0
      kcds.core.Events.on("busyTick", function(n)
        count = count + 1
        local v = kcds.math.Vector3.new(n, 0, 0)
        local players = kcds.players.Player.all()
        return { n = n, length = v.length, players = #players, text = json.encode({ n = n }) }
      end)
      kcds.core.Exports.register("count", function() return count end)
    `,
    },
    { server: 'main.lua', exports: ['count'] },
  );
  await f.host.startAll();
  const api = f.host.makeAPI();
  for (let n = 0; n < 20000; n += 1) {
    await api.Events.emit('busyTick', n);
  }
  assert.equal(f.host.services.owners.get('busy').exports.get('count')(), 20000);
  const state = f.host.resources.get('busy');
  for (let n = 0; n < 2000; n += 1) {
    assert.equal(await state.lua.engine.doString(`return ${n} + 1`), n + 1);
  }
});
