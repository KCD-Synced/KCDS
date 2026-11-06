'use strict';

// Lua resources: a resource's scripts written in Lua 5.4, run by our own runtime (Lua
// compiled to WebAssembly, vendor/wasmoon), never by the game's Lua. The server's Node host and the
// client's resource worker both use this file, so a Lua resource behaves the same on both sides.
//
// The binding is the SDK itself: each Lua resource gets its own Lua state whose `kcds` is the very
// object tree a JavaScript resource of the same side gets (the same scope the host builds for it),
// so every member, event, error and lifecycle rule is the one implementation, and nothing can be
// missing in Lua that JavaScript has (the parity test walks the contract to prove it). What Lua
// adds on top, for the language only:
//
//   Class.new(...)          a class's constructor (`kcds.math.Vector3.new(1, 2, 3)`), as Lua has no
//                           `new`; static members are reached as before (`Player.all()`)
//   print(...)              the resource's console log
//   json.encode/decode      JSON text, as JavaScript's JSON.stringify and JSON.parse
//   setTimeout & co.        the host's timers, which the host clears when the resource stops, and
//                           the scope's other globals (fetch, WebSocket.new, URL.new, ...)
//   require("name")         another Lua file of this resource ("name" or "folder.name", .lua)
//
// A promise (what an async member answers) has :next(fn), :catch(fn) and :finally(fn) in Lua;
// :await() waits for it at the top of a file, not inside a handler the host calls (Lua cannot
// yield across the host's call), where :next is the way.
//
// A JavaScript value reaches Lua as itself: objects and arrays as userdata read with `.` and
// indexed from 1, methods called with `:` or `.`; a Lua table reaches JavaScript as an object (or
// an array when its keys are 1..n); a Lua function as a JavaScript function the host may call
// later. Errors cross both ways: a JavaScript throw is a Lua error (`pcall` sees it), a Lua error
// in a handler is the handler's exception, as it is for JavaScript.

// Plain relative requires only: the client's runtime bundle (Dev/tools/build_resource_runtime.py)
// carries this file and the vendored engine.
const wasmoonModule = require('./vendor/wasmoon/index.js');

// The longest a Lua call may run without returning, as for a JavaScript resource's script; past
// it the call fails with a timeout error, so an endless loop cannot hang the host.
const RUN_LIMIT_MS = 5000;

// lua_resume's answer when the code waits (LUA_YIELD).
const LUA_YIELD = 1;

let factory = null;

// The one Lua module (the compiled WebAssembly), shared by every resource's own state.
function luaFactory() {
  if (!factory) {
    factory = new wasmoonModule.LuaFactory(compiledLua());
  }
  return factory;
}

// The compiled module: on the server the vendored file's path, in the client's worker (which may
// read no files) the bytes its bundle carries.
function compiledLua() {
  if (typeof __dirname === 'string') {
    return require('node:path').join(__dirname, 'vendor', 'wasmoon', 'glue.wasm');
  }
  return require('./vendor/wasmoon/glue.wasm');
}

function isClass(value) {
  if (typeof value !== 'function' || !value.prototype) {
    return false;
  }
  return /^class\b/.test(Function.prototype.toString.call(value));
}

// A class as Lua sees it: the class itself, with `new` for its constructor. It goes to Lua as an
// object (userdata), as Lua cannot index a function: its statics and `new` are read with `.`.
function luaClass(target) {
  const view = new Proxy(target, {
    get(object, key, receiver) {
      if (key === 'new' && !Object.hasOwn(object, 'new')) {
        return (...args) => new object(...args);
      }
      return Reflect.get(object, key, receiver);
    },
  });
  return wasmoonModule.decorateProxy(view, { proxy: true });
}

// The SDK tree as Lua sees it: each domain's members read through (live values stay live), each
// class with `new`.
function luaTree(kcds) {
  const classes = new Map();
  const wrap = (value) => {
    if (!isClass(value)) {
      return value;
    }
    if (!classes.has(value)) {
      classes.set(value, luaClass(value));
    }
    return classes.get(value);
  };
  const tree = {};
  for (const domain of Object.keys(kcds)) {
    const members = kcds[domain];
    const view = {};
    for (const name of Object.keys(members)) {
      Object.defineProperty(view, name, { get: () => wrap(members[name]), enumerable: true });
    }
    tree[domain] = Object.freeze(view);
  }
  return tree;
}

// The scope's globals a JavaScript script of this side has besides `kcds` (timers, and on the
// server fetch, WebSocket, URL and the web platform's other classes), as Lua globals; a
// constructor (a name in capitals) with `new`. Left out: what Lua has its own way for (console:
// print; require; the global object) and Node's own objects (Buffer, process).
const NOT_FOR_LUA = new Set([
  'kcds',
  'console',
  'require',
  'module',
  'exports',
  'global',
  'globalThis',
  'Buffer',
  'process',
]);

function scopeGlobals(scope) {
  const globals = [];
  for (const name of Object.keys(scope)) {
    if (NOT_FOR_LUA.has(name)) {
      continue;
    }
    const value = scope[name];
    if (value === undefined || value === null) {
      continue;
    }
    const constructor = typeof value === 'function' && /^[A-Z]/.test(name) && value.prototype;
    globals.push([name, constructor ? luaClass(value) : value]);
  }
  return globals;
}

// A Lua module name ("name", "folder.name") as a file of the resource, or null when it names none.
function moduleFile(name) {
  if (typeof name !== 'string' || !/^[A-Za-z0-9_]+(\.[A-Za-z0-9_]+)*$/.test(name)) {
    return null;
  }
  return `${name.replaceAll('.', '/')}.lua`;
}

// What a Lua resource needs of its host: its name, the scope a JavaScript resource of this side
// would get (its `kcds`, timers, console), and how to read one of its own files (a path relative to
// the resource, '/' separated; throws when it is not there).
class LuaResource {
  constructor({ name, scope, readFile }) {
    this.name = name;
    this.scope = scope;
    this.readFile = readFile;
    this.engine = null;
  }

  async start() {
    const engine = await luaFactory().createEngine({
      // The `null` value (JavaScript's null, for a member that tells it from not given) and
      // Promise.create/all/resolve. A null the host gives is still nil (the vendored patch).
      injectObjects: true,
      enableProxy: true,
      traceAllocations: false,
      functionTimeout: RUN_LIMIT_MS,
    });
    this.engine = engine;
    const { scope } = this;
    const kcds = luaTree(scope.kcds);
    engine.global.set('kcds', kcds);
    const log = scope.console?.log ?? (() => {});
    engine.global.set('print', (...args) => log(args.map(luaText).join('\t')));
    for (const [name, value] of scopeGlobals(scope)) {
      engine.global.set(name, value);
    }
    // JSON, which JavaScript has built in: a Lua table as JSON text and back.
    engine.global.set('json', {
      encode: (value) => JSON.stringify(value ?? null),
      decode: (text) => JSON.parse(text),
    });
    engine.global.set('__kcds_read', (name) => {
      const file = moduleFile(name);
      if (!file) {
        throw new TypeError(`require: '${name}' is not a module name (letters, digits, _ and .)`);
      }
      return { file, code: this.readFile(file) };
    });
    await this.runCode(PRELUDE);
    return this;
  }

  // Runs one of the resource's Lua files (a path relative to the resource).
  async run(file) {
    const code = this.readFile(file);
    await this.runCode(chunk(code, file));
  }

  // Runs Lua code on its own thread of the state. Each stretch of it between two waits (a
  // promise's :await()) may run RUN_LIMIT_MS; the waits themselves take as long as they take.
  async runCode(code) {
    const global = this.engine.global;
    const thread = global.newThread();
    const index = global.getTop();
    try {
      thread.loadString(code);
      const step = (argCount) => {
        thread.setTimeout(Date.now() + RUN_LIMIT_MS);
        try {
          return thread.resume(argCount);
        } finally {
          thread.setTimeout(undefined);
        }
      };
      let result = step(0);
      while (result.result === LUA_YIELD) {
        const waited = result.resultCount > 0 ? thread.getValue(-1) : undefined;
        if (result.resultCount > 0) {
          thread.pop(result.resultCount);
        }
        if (waited && typeof waited.then === 'function') {
          await waited;
        } else {
          await new Promise((resolve) => setImmediate(resolve));
        }
        result = step(0);
      }
      thread.assertOk(result.result);
    } finally {
      global.remove(index);
    }
  }

  close() {
    if (this.engine) {
      this.engine.global.close();
      this.engine = null;
    }
  }
}

// A value as print shows it.
function luaText(value) {
  if (value === undefined || value === null) {
    return 'nil';
  }
  if (value instanceof Error) {
    return String(value);
  }
  if (typeof value === 'object') {
    try {
      return JSON.stringify(value);
    } catch {
      return String(value);
    }
  }
  return String(value);
}

// Lua source run under its file's name, so errors say where they are.
function chunk(code, file) {
  const name = JSON.stringify(`@${file}`);
  return `local chunk, problem = load(${JSON.stringify(code)}, ${name}, "t")
if not chunk then error(problem, 0) end
return chunk()`;
}

// The Lua side of the binding: the standard library without what reaches outside the resource
// (files, processes, the environment, bytecode, the debug library), and require from the
// resource's own files.
const PRELUDE = `
os = {
  time = os.time,
  clock = os.clock,
  date = os.date,
  difftime = os.difftime,
}
io = nil
dofile = nil
loadfile = nil
package = nil
debug = nil
local rawload = load
function load(chunk, name, mode, ...)
  if select("#", ...) == 0 then
    return rawload(chunk, name, "t")
  end
  return rawload(chunk, name, "t", ...)
end

local loaded = {}
local read = __kcds_read
__kcds_read = nil
function require(name)
  if loaded[name] ~= nil then
    return loaded[name]
  end
  local found = read(name)
  local chunk, problem = load(found.code, "@" .. found.file, "t")
  if not chunk then
    error(problem, 2)
  end
  local result = chunk(name)
  if result == nil then
    result = true
  end
  loaded[name] = result
  return result
end
`;

module.exports = { LuaResource, luaTree, isClass, moduleFile };
