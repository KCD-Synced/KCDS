'use strict';

// Local resource contracts shared by the trusted server host and the browser
// client runtime. Each public facade captures its owner instead of accepting
// an owner/resource identity from script-supplied messages.

// A string payload as JSON text: itself when it already is JSON, else the string encoded.
function jsonTextOrString(text) {
  try {
    JSON.parse(text);
    return text;
  } catch {
    return JSON.stringify(text);
  }
}

class ResourceServices {
  constructor({
    side = 'server',
    send = () => {},
    log = () => {},
    defer = (fn) => setTimeout(fn, 0),
    run = (_owner, fn) => fn(),
    eventArgs = (_name, args, _owner) => args,
    // The KCDS version on this side, which the script API follows; '' when the build gave none.
    apiVersion = '',
    serverVersion = '',
    // The server's own folder (the one holding resources/ and server.json); null on a client.
    serverDir = null,
    // Internal safety guards for Messages.request. These are host/runtime
    // settings, never part of the public request(resource, type, payload)
    // signature. The documented contract is that a request waits forever and
    // rejects only when a resource stops, so the timeout defaults to disabled;
    // the pending caps default to generous finite limits that never affect
    // ordinary traffic but stop a runaway resource from growing the pending set
    // without bound. setTimer/clearTimer are injectable for fake-clock tests.
    requestTimeoutMs = 0,
    maxPendingRequests = 10000,
    maxPendingRequestsPerResource = 1000,
    setTimer = (fn, ms) => setTimeout(fn, ms),
    clearTimer = (timer) => clearTimeout(timer),
  } = {}) {
    if (!['client', 'server'].includes(side)) throw new TypeError('Invalid execution side');
    this.side = side;
    this.send = send;
    this.log = log;
    this.defer = defer;
    this.run = run;
    this.eventArgs = eventArgs;
    this.owners = new Map();
    this.handlers = new Map();
    this.pending = new Set();
    this.pendingPerSource = new Map();
    this.requestGuard = {
      timeoutMs: Number.isFinite(requestTimeoutMs) && requestTimeoutMs > 0 ? requestTimeoutMs : 0,
      maxPending: maxPendingRequests > 0 ? maxPendingRequests : Infinity,
      maxPerResource: maxPendingRequestsPerResource > 0 ? maxPendingRequestsPerResource : Infinity,
    };
    this.setTimer = setTimer;
    this.clearTimer = clearTimer;
    this.environment = Object.freeze({
      isClient: side === 'client',
      isServer: side === 'server',
      apiVersion,
      serverVersion,
      serverDir,
    });
  }

  add(name, manifest) {
    this.remove(name);
    const state = { name, manifest, messages: new Map(), exports: new Map() };
    this.owners.set(name, state);
    return this.facade(state);
  }

  requireOwner(state) {
    if (this.owners.get(state.name) !== state) throw new Error('Resource is stopped');
    return state;
  }

  // A network event's name and payload text, checked: at most 32 KB of JSON. A string that is
  // already JSON text goes as it is; any other string is sent as a JSON string, so the other side
  // gets the text instead of dropping it.
  networkEventOf(name, payload) {
    this.key('network', name);
    const text =
      typeof payload === 'string' ? jsonTextOrString(payload) : JSON.stringify(payload ?? null);
    if (new TextEncoder().encode(text).length > 32768) {
      throw new RangeError('Event payload too long');
    }
    return { name, payload: text };
  }

  // An event for one player's client only (player.emit).
  emitToPlayer(player, name, payload) {
    this.send({ op: 'event', ...this.networkEventOf(name, payload), player });
  }

  key(namespace, name, owner = '') {
    if (typeof name !== 'string') throw new TypeError('Event name must be a string');
    return JSON.stringify([namespace, owner, name]);
  }

  subscribe(state, namespace, name, fn, once = false) {
    this.requireOwner(state);
    if (typeof fn !== 'function') throw new TypeError('Event handler must be a function');
    const key = this.key(namespace, name, namespace === 'local' ? state.name : '');
    const entry = { state, fn, once, active: true };
    const list = this.handlers.get(key) ?? [];
    list.push(entry);
    this.handlers.set(key, list);
    return () => this.unsubscribeEntry(key, entry);
  }

  unsubscribeEntry(key, entry) {
    entry.active = false;
    const list = this.handlers.get(key)?.filter((handler) => handler !== entry);
    if (list?.length) this.handlers.set(key, list);
    else this.handlers.delete(key);
  }

  off(state, namespace, name, fn) {
    this.requireOwner(state);
    const key = this.key(namespace, name);
    for (const entry of [...(this.handlers.get(key) ?? [])]) {
      if (entry.state === state && entry.fn === fn) this.unsubscribeEntry(key, entry);
    }
  }

  // Whether a running resource handles a shared event.
  handles(name) {
    const key = this.key('shared', name);
    return (this.handlers.get(key) ?? []).some(
      (entry) => entry.active && this.owners.get(entry.state.name) === entry.state,
    );
  }

  emitSync(name, args) {
    let accepted = true;
    const key = this.key('shared', name);
    for (const entry of [...(this.handlers.get(key) ?? [])]) {
      if (!entry.active || this.owners.get(entry.state.name) !== entry.state) continue;
      if (entry.once) this.unsubscribeEntry(key, entry);
      try {
        const result = this.run(entry.state.name, () =>
          entry.fn(...this.eventArgs(name, args, entry.state.name)),
        );
        if (result === false) accepted = false;
        if (result?.then) result.catch((error) => this.log(entry.state.name, String(error)));
      } catch (error) {
        this.log(entry.state.name, String(error));
      }
    }
    return accepted;
  }

  async emit(namespace, name, args, target) {
    const key = this.key(namespace, name, namespace === 'local' ? target : '');
    const promises = [];
    for (const entry of [...(this.handlers.get(key) ?? [])]) {
      if (!entry.active || this.owners.get(entry.state.name) !== entry.state) continue;
      if (target !== undefined && entry.state.name !== target) continue;
      if (entry.once) this.unsubscribeEntry(key, entry);
      try {
        promises.push(
          Promise.resolve(
            this.run(entry.state.name, () =>
              entry.fn(...this.eventArgs(name, args, entry.state.name)),
            ),
          ),
        );
      } catch (error) {
        // A synchronous throw is logged with its stack and still rejects the emit.
        this.log(entry.state.name, error?.stack ?? String(error));
        promises.push(Promise.reject(error));
      }
    }
    const settled = await Promise.allSettled(promises);
    const errors = settled
      .filter((result) => result.status === 'rejected')
      .map((result) => result.reason);
    if (errors.length) throw new AggregateError(errors, `Event ${name}`);
  }

  requireTarget(name) {
    const state = this.owners.get(name);
    if (!state) throw new Error('Resource is not available');
    return state;
  }

  request(source, name, type, payload) {
    this.requireOwner(source);
    let target;
    let handler;
    try {
      target = this.requireTarget(name);
      if (!target.messages.size) throw new Error('Resource is not available');
      handler = target.messages.get(type);
      if (!handler) throw new Error('Resource does not handle this message type');
      this.guardPendingLimit(source);
    } catch (error) {
      return Promise.reject(error);
    }
    return new Promise((resolve, reject) => {
      const pending = { source, target, resolve, reject, replied: false, timer: null };
      this.trackPending(pending);
      // One cleanup path for every outcome (reply, handler error, timeout,
      // resource stop): drop the pending entry, release the per-source slot and
      // clear any armed deadline timer so nothing leaks.
      const finish = (error, value) => {
        if (!this.pending.has(pending)) return;
        this.untrackPending(pending);
        if (error) reject(error);
        else resolve(value);
      };
      const reply = (value) => {
        // Late replies (after a timeout/stop dropped the entry) and double
        // replies are ignored: the entry is gone or already marked replied.
        if (pending.replied || !this.pending.has(pending)) return;
        pending.replied = true;
        this.defer(() => finish(null, value));
      };
      if (this.requestGuard.timeoutMs > 0) {
        pending.timer = this.setTimer(() => {
          if (!pending.replied) finish(new Error('Message request timed out'));
        }, this.requestGuard.timeoutMs);
      }
      this.defer(() => {
        if (!this.pending.has(pending)) return;
        try {
          const result = this.run(target.name, () => handler(payload, reply));
          // The contract intentionally leaves requests pending when
          // an async handler rejects without replying. Stop and the optional
          // internal timeout are the only things that cancel it.
          if (result?.then) result.catch((error) => this.log(target.name, String(error)));
        } catch (error) {
          if (!pending.replied) finish(error);
          else this.log(target.name, String(error));
        }
      });
    });
  }

  guardPendingLimit(source) {
    if (this.pending.size >= this.requestGuard.maxPending)
      throw new Error('Pending request limit reached');
    if ((this.pendingPerSource.get(source) ?? 0) >= this.requestGuard.maxPerResource)
      throw new Error('Pending request limit reached for resource');
  }

  trackPending(pending) {
    this.pending.add(pending);
    this.pendingPerSource.set(pending.source, (this.pendingPerSource.get(pending.source) ?? 0) + 1);
  }

  untrackPending(pending) {
    if (!this.pending.delete(pending)) return;
    if (pending.timer !== null) {
      this.clearTimer(pending.timer);
      pending.timer = null;
    }
    const remaining = (this.pendingPerSource.get(pending.source) ?? 0) - 1;
    if (remaining > 0) this.pendingPerSource.set(pending.source, remaining);
    else this.pendingPerSource.delete(pending.source);
  }

  // A resource that reaches another without listing it among its requires (an
  // optional one included: { name, optional: true }) is told once per resource it reaches, not on
  // every call.
  warnDependency(source, target) {
    if (source.name === target.name) return;
    if (source.manifest.kcds.requires.some((dep) => dep.name === target.name)) return;
    source.undeclaredWarned ??= new Set();
    if (source.undeclaredWarned.has(target.name)) return;
    source.undeclaredWarned.add(target.name);
    this.log(source.name, `Undeclared resource dependency: ${target.name}`);
  }

  facade(state) {
    const ensure =
      (fn) =>
      (...args) => {
        this.requireOwner(state);
        return fn(...args);
      };
    const Events = {
      on: (name, fn) => this.subscribe(state, 'shared', name, fn),
      once: (name, fn) => {
        this.subscribe(state, 'shared', name, fn, true);
      },
      off: (name, fn) => this.off(state, 'shared', name, fn),
      emit: ensure((name, ...args) => this.emit('shared', name, args)),
      toResource: ensure((target, name, ...args) => this.emit('shared', name, args, target)),
      listen: (name, fn) => {
        this.subscribe(state, 'local', name, fn);
      },
      fire: ensure((name, ...args) => this.emit('local', name, args, state.name)),
      listenerCount: ensure((name) => (this.handlers.get(this.key('shared', name)) ?? []).length),
    };
    const networkEvent = (name, payload) => {
      this.send({ op: 'event', ...this.networkEventOf(name, payload) });
    };
    if (this.side === 'client') Events.toServer = ensure(networkEvent);
    else {
      Events.broadcast = ensure(networkEvent);
      Events.fromClient = (name, fn) => this.subscribe(state, 'client', name, fn);
      Events.onceFromClient = (name, fn) => {
        this.subscribe(state, 'client', name, fn, true);
      };
      Events.offFromClient = (name, fn) => this.off(state, 'client', name, fn);
    }
    const Messages = {
      handle: ensure((type, handler) => {
        if (typeof type !== 'string' || typeof handler !== 'function')
          throw new TypeError('Invalid message handler');
        state.messages.set(type, handler);
      }),
      request: (name, type, payload) => this.request(state, name, type, payload),
      send: ensure((name, type, payload) => {
        const target = this.owners.get(name);
        const handler = target?.messages.get(type);
        if (!handler) return;
        this.defer(() => {
          if (this.owners.get(state.name) !== state || this.owners.get(name) !== target) return;
          try {
            const result = this.run(name, () => handler(payload, () => {}));
            if (result?.then) result.catch((error) => this.log(name, String(error)));
          } catch (error) {
            this.log(name, String(error));
          }
        });
      }),
    };
    const Exports = {
      register: ensure((name, value) => {
        if (typeof name !== 'string') throw new TypeError('Invalid export name');
        if (state.manifest.kcds.exports.includes(name)) state.exports.set(name, value);
        else this.log(state.name, `Undeclared export ignored: ${name}`);
        return true;
      }),
      get: ensure((name, exported) => {
        const target = this.requireTarget(name);
        this.warnDependency(state, target);
        if (!target.exports.has(exported)) throw new Error('Export not found');
        return target.exports.get(exported);
      }),
    };
    const Imports = {
      get: ensure((name) => {
        const target = this.requireTarget(name);
        this.warnDependency(state, target);
        return Object.fromEntries(target.exports);
      }),
    };
    return { Events, Messages, Exports, Imports, Runtime: this.environment };
  }

  remove(name) {
    const state = this.owners.get(name);
    if (!state) return;
    for (const [key, list] of [...this.handlers]) {
      for (const entry of [...list]) {
        if (entry.state === state) this.unsubscribeEntry(key, entry);
      }
    }
    for (const request of [...this.pending]) {
      if (request.source === state || request.target === state) {
        this.untrackPending(request);
        request.reject(new Error('Resource stopped before message reply'));
      }
    }
    state.messages.clear();
    state.exports.clear();
    this.owners.delete(name);
  }
}

// Readable log text for a handler failure. An AggregateError from an event emit carries the
// individual handler errors, which are listed so the actual cause is not lost.
function describeError(error) {
  if (error instanceof AggregateError && Array.isArray(error.errors) && error.errors.length) {
    const causes = error.errors.map((cause) => describeError(cause));
    return `${error.message}: ${causes.join('; ')}`;
  }
  return error?.stack ?? String(error);
}

module.exports = { ResourceServices, describeError };
