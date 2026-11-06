'use strict';

// C# resources: a resource's scripts written in C#, built by its author against the
// KCDS SDK (Backend/scripting/runtimes/csharp/Kcds.Sdk) into .dll files, and run on .NET compiled to
// WebAssembly inside this host. The server's Node host
// and the client's resource worker both use this file.
//
// One .NET runtime per side serves every C# resource; each resource's assemblies load into its own
// collectible AssemblyLoadContext, so one resource's types and static state are its own. As for
// Lua, the binding is the SDK itself: the C# SDK reaches the resource's own scope (the `kcds` a
// JavaScript resource of this side gets, its timers, its console) through the few calls below,
// with the resource's number; every member, event, error and lifecycle rule is the one
// implementation.
//
// Values between the languages (Kcds.Sdk/Runtime/Values.cs): numbers, text, true/false and null
// as themselves; JavaScript objects and functions as references; a C# delegate as a function the
// host may call later (one function per delegate); an argument the C# side leaves out as
// undefined, so JavaScript defaults apply. Errors cross both ways.

// No Node modules at load time: the client's runtime bundle carries this file.

// Stands for undefined in an argument list from C# (Sdk.Undefined); replaced before the call.
const UNDEFINED = Object.freeze({ kcdsUndefined: true });

// The scopes of the running C# resources, by the number the C# side knows them by.
const scopes = new Map();
let nextResource = 1;
let runtimePromise = null;

function scopeOf(resource) {
  const scope = scopes.get(resource);
  if (!scope) {
    throw new Error(`C# resource ${resource} is not running`);
  }
  return scope;
}

// A JavaScript function (a class too), error or array reaches C# in a box, one box per value:
// the runtime takes plain objects as references, but makes functions unusable, errors into .NET
// exceptions and arrays into copies (losing the error's own fields and the array's identity). The
// bridge takes the value out again.
const boxes = new WeakMap();

function boxed(value) {
  let box = boxes.get(value);
  if (!box) {
    box = Object.freeze({ kcdsBoxed: value });
    boxes.set(value, box);
  }
  return box;
}

// A value from C# as JavaScript uses it: the undefined marker is undefined, a box its value.
function fromAny(value) {
  if (value === UNDEFINED) {
    return undefined;
  }
  if (value !== null && typeof value === 'object' && Object.hasOwn(value, 'kcdsBoxed')) {
    return value.kcdsBoxed;
  }
  return value;
}

// Arguments from C#: as fromAny, and trailing undefined ones are not passed.
function argumentsOf(values) {
  const args = Array.from(values ?? [], fromAny);
  while (args.length > 0 && args[args.length - 1] === undefined) {
    args.pop();
  }
  return args;
}

// A promise as the C# side gets it: one that never rejects, settled as { kcdsValue } or
// { kcdsError }. The .NET runtime makes a task of a promise without handling its rejection, so a
// rejection would otherwise be unhandled even when C# awaits it; C# unwraps the outcome (a value,
// or a JsError), and a task nobody awaits keeps its error, as in .NET.
function settled(promise) {
  return Promise.resolve(promise).then(
    (value) => ({ kcdsValue: toAny(value) }),
    (error) => ({ kcdsError: toAny(error) }),
  );
}

// A JavaScript value as the C# side takes it (no undefined: null; a function in its box; a
// promise settled).
function toAny(value) {
  if (value === undefined) {
    return null;
  }
  if (typeof value === 'bigint') {
    return Number(value);
  }
  if (typeof value === 'function' || value instanceof Error || Array.isArray(value)) {
    return boxed(value);
  }
  if (value !== null && typeof value === 'object' && typeof value.then === 'function') {
    return settled(value);
  }
  return value;
}

// What the last bridge call that failed threw, for the C# side's JsError (read at once, as the
// side is single-threaded).
let lastError = null;

function guarded(call) {
  return (...args) => {
    try {
      return call(...args);
    } catch (error) {
      lastError = toAny(error);
      throw error;
    }
  };
}

// The "kcds" module the C# SDK imports (Kcds.Sdk/Runtime/Bridge.cs).
function bridge(exportsOf) {
  return {
    root: (resource) => scopeOf(resource).kcds,
    global: (resource, name) => {
      if (name === '__kcds_undefined') {
        return UNDEFINED;
      }
      return toAny(scopeOf(resource)[name]);
    },
    get: guarded((target, name) => toAny(fromAny(target)[name])),
    set: guarded((target, name, value) => {
      fromAny(target)[name] = fromAny(value);
    }),
    invoke: guarded((target, name, values) => {
      const self = fromAny(target);
      const member = self[name];
      if (typeof member !== 'function') {
        throw new TypeError(`${name} is not a function`);
      }
      return toAny(member.apply(self, argumentsOf(values)));
    }),
    call: guarded((fn, values) => toAny(fromAny(fn)(...argumentsOf(values)))),
    construct: guarded((type, values) => toAny(new (fromAny(type))(...argumentsOf(values)))),
    lastError: () => lastError,
    // A C# delegate as a function. What it answers comes back as JavaScript uses it (a box
    // unwrapped, the undefined marker undefined), for a Task once it completes.
    callback: (resource, id, returnsTask) => {
      const callbacks = exportsOf().Kcds.Runtime.Callbacks;
      return (...args) => {
        const values = args.map(toAny);
        if (returnsTask) {
          return callbacks.DispatchAsync(id, values).then(fromAny);
        }
        return fromAny(callbacks.Dispatch(id, values));
      };
    },
    newObject: () => ({}),
    newArray: (items) => Array.from(items ?? [], fromAny),
    keys: (target) => Object.keys(fromAny(target)),
    isArray: (value) => Array.isArray(fromAny(value)),
    isPromise: (value) => value !== null && typeof fromAny(value)?.then === 'function',
    length: (target) => fromAny(target).length ?? 0,
    at: (target, index) => toAny(fromAny(target)[index]),
    typeOf: (value) => (value === null ? 'object' : typeof fromAny(value)),
    className: (value) => {
      const constructor = Object(fromAny(value)).constructor;
      return typeof constructor === 'function' ? constructor.name : '';
    },
    text: (value) => String(fromAny(value)),
    same: (a, b) => Object.is(fromAny(a), fromAny(b)),
    jsonParse: (text) => JSON.parse(text),
    jsonStringify: (value) => JSON.stringify(fromAny(value) ?? null),
    log: (resource, level, text) => {
      const scope = scopes.get(resource);
      const write = scope?.console?.[level] ?? scope?.console?.log;
      if (write) {
        write(text);
      }
    },
  };
}

// Where the runtime's files are: built by Dev/tools/build_csharp_host.py next to this file on the
// server; the client bundle gives its own loader (setRuntimeLoader).
let loadRuntime = async () => {
  const path = require('node:path');
  const { pathToFileURL } = require('node:url');
  const entry = path.join(__dirname, 'dotnet', '_framework', 'dotnet.js');
  const { dotnet } = await import(pathToFileURL(entry).href);
  return dotnet.create();
};

function setRuntimeLoader(loader) {
  loadRuntime = loader;
  runtimePromise = null;
}

// The side's one .NET runtime, started on first use.
function runtime() {
  if (!runtimePromise) {
    runtimePromise = (async () => {
      const api = await loadRuntime();
      let sdkExports = null;
      api.setModuleImports(
        'kcds',
        bridge(() => sdkExports),
      );
      sdkExports = await api.getAssemblyExports('Kcds.Sdk.dll');
      const host = await api.getAssemblyExports(api.getConfig().mainAssemblyName);
      await api.runMain();
      return { api, entry: host.Kcds.Host.Entry };
    })();
  }
  return runtimePromise;
}

// What a C# resource needs of its host: its name, the scope a JavaScript resource of this side
// would get, and how to read one of its own files as bytes (a path relative to the resource).
class CSharpResource {
  constructor({ name, scope, readBytes }) {
    this.name = name;
    this.scope = scope;
    this.readBytes = readBytes;
    this.id = null;
    this.entry = null;
  }

  // Loads the resource's .dll files in their order and starts the scripts in them.
  async start(files) {
    const { entry } = await runtime();
    this.entry = entry;
    this.id = nextResource;
    nextResource += 1;
    scopes.set(this.id, this.scope);
    for (const file of files) {
      if (!file.toLowerCase().endsWith('.dll')) {
        throw new TypeError(
          `${file}: a C# resource lists the .dll files it was built into, not its sources`,
        );
      }
      entry.Load(this.id, Array.from(this.readBytes(file)));
    }
    return entry.Start(this.id).split('\n').filter(Boolean);
  }

  stop() {
    if (this.id === null) {
      return;
    }
    try {
      this.entry.Stop(this.id);
    } finally {
      scopes.delete(this.id);
      this.id = null;
    }
  }
}

module.exports = { CSharpResource, setRuntimeLoader, argumentsOf, UNDEFINED };
