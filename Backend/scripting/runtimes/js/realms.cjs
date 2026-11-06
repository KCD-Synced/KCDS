'use strict';

// `instanceof` across the server's resource contexts.
//
// Each server resource gets a vm context of its own, so it has its own built-ins: one resource
// cannot overwrite another's globals or patch Array.prototype for everyone, and stopping it
// leaves nothing behind. Values the host hands a resource (a rejected Events.emit's
// AggregateError, a TypeError from an SDK call, promises, arrays, plain objects) are created in
// the host's realm, and values another resource emits in that resource's realm, so a plain
// `instanceof` against the resource's own built-ins would be false.
//
// Each context's built-in constructors get a Symbol.hasInstance that also accepts instances of
// the same built-in from the host realm and from every other linked context. Prototypes, globals
// and the built-ins themselves stay per context.

const vm = require('node:vm');

const BUILTINS = [
  'Object',
  'Function',
  'Array',
  'Error',
  'EvalError',
  'RangeError',
  'ReferenceError',
  'SyntaxError',
  'TypeError',
  'URIError',
  'AggregateError',
  'Promise',
  'Map',
  'Set',
  'WeakMap',
  'WeakSet',
  'Date',
  'RegExp',
  'ArrayBuffer',
  'DataView',
  'Int8Array',
  'Uint8Array',
  'Uint8ClampedArray',
  'Int16Array',
  'Uint16Array',
  'Int32Array',
  'Uint32Array',
  'Float32Array',
  'Float64Array',
  'BigInt64Array',
  'BigUint64Array',
];

const ordinaryHasInstance = Function.prototype[Symbol.hasInstance];

function hostIntrinsics() {
  return Object.fromEntries(BUILTINS.map((name) => [name, globalThis[name]]));
}

class RealmLinks {
  constructor() {
    this.realms = new Set([hostIntrinsics()]);
  }

  // Links a vm context; returns the function that unlinks it when its resource stops.
  link(context) {
    const intrinsics = vm.runInContext(`({ ${BUILTINS.join(', ')} })`, context);
    const realms = this.realms;
    for (const name of BUILTINS) {
      const own = intrinsics[name];
      Object.defineProperty(own, Symbol.hasInstance, {
        configurable: true,
        value(candidate) {
          if (ordinaryHasInstance.call(this, candidate)) {
            return true;
          }
          // A subclass inherits this static method; only the built-in itself widens the check.
          if (this !== own) {
            return false;
          }
          for (const realm of realms) {
            const other = realm[name];
            if (other !== own && ordinaryHasInstance.call(other, candidate)) {
              return true;
            }
          }
          return false;
        },
      });
    }
    this.realms.add(intrinsics);
    return () => this.realms.delete(intrinsics);
  }
}

module.exports = { RealmLinks, BUILTINS };
