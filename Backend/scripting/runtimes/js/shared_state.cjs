'use strict';

const LIMITS = Object.freeze({ keyBytes: 64, valueBytes: 4096, keys: 128 });
const SCOPES = new Set(['broadcast', 'owner', 'server']);
const encoder = new TextEncoder();
const bindings = new WeakMap();
const construct = Symbol('KCDS shared state binding');

function keyName(key) {
  if (typeof key !== 'string') throw new TypeError('State key must be a string');
  if (encoder.encode(key).length > LIMITS.keyBytes)
    throw new RangeError('State key exceeds 64 UTF-8 bytes');
  return key;
}

function encodeValue(value) {
  let kind = typeof value;
  if (kind === 'number' && !Number.isFinite(value))
    throw new TypeError('State number must be finite');
  if (!['boolean', 'number', 'string'].includes(kind)) {
    value = JSON.stringify(value);
    if (value === undefined) throw new TypeError('State value cannot be serialized as JSON');
    kind = 'json';
  }
  if (typeof value === 'string' && encoder.encode(value).length > LIMITS.valueBytes)
    throw new RangeError('State value exceeds 4096 bytes');
  return { kind, value };
}

function decodeValue(entry) {
  if (!entry) return undefined;
  return entry.kind === 'json' ? JSON.parse(entry.value) : entry.value;
}

function sameValue(left, right) {
  return left?.kind === right?.kind && left?.value === right?.value;
}

function bound(bag) {
  const binding = bindings.get(bag);
  if (!binding || binding.store.records.get(binding.record.id) !== binding.record)
    throw new Error('Entity state no longer exists');
  return binding;
}

class ReadSharedState {
  constructor(token, binding) {
    if (token !== construct) throw new TypeError('SharedState is obtained from entity.state');
    bindings.set(this, binding);
  }
  get(key) {
    return decodeValue(bound(this).record.values.get(keyName(key)));
  }
  has(key) {
    return bound(this).record.values.has(keyName(key));
  }
  keys() {
    return [...bound(this).record.values.keys()].sort();
  }
  toObject() {
    const { record } = bound(this);
    return Object.fromEntries(
      [...record.values]
        .sort(([left], [right]) => (left < right ? -1 : left > right ? 1 : 0))
        .map(([key, entry]) => [key, decodeValue(entry)]),
    );
  }
  onChange(key, handler) {
    const { store, record, owner } = bound(this);
    if (key !== null) keyName(key);
    if (typeof handler !== 'function')
      throw new TypeError('State change handler must be a function');
    if (!store.ownerAlive(owner)) throw new Error('Resource is stopped');
    const entry = { key, handler, owner, active: true };
    record.listeners.add(entry);
    return () => {
      entry.active = false;
      record.listeners.delete(entry);
    };
  }
}

class WriteSharedState extends ReadSharedState {
  set(key, value, options = {}) {
    const { store, record, owner } = bound(this);
    if (!store.ownerAlive(owner)) throw new Error('Resource is stopped');
    keyName(key);
    const scope = options?.scope ?? 'broadcast';
    if (!SCOPES.has(scope)) throw new TypeError('Unknown state scope');
    const next = { ...encodeValue(value), scope };
    const previous = record.values.get(key);
    if (sameValue(previous, next) && previous.scope === scope) return false;
    if (!previous && record.values.size >= LIMITS.keys)
      throw new RangeError('Shared state exceeds 128 keys');
    record.values.set(key, next);
    record.version = Symbol('state version');
    store.changeClock += 1;
    // Scope changes alter replication but do not change the locally read value.
    if (!sameValue(previous, next)) store.changed(record, key, next, previous);
    return true;
  }
  remove(key) {
    const { store, record, owner } = bound(this);
    if (!store.ownerAlive(owner)) throw new Error('Resource is stopped');
    keyName(key);
    const previous = record.values.get(key);
    if (!previous) return false;
    record.values.delete(key);
    record.version = Symbol('state version');
    store.changeClock += 1;
    store.changed(record, key, undefined, previous);
    return true;
  }
}

// Entity lifecycle, owner selection and visibility are supplied by the native
// directory. No constructor here spawns an entity or accepts client writes.
class EntityStateStore {
  constructor({
    side = 'server',
    ownerAlive = () => true,
    run = (_owner, fn) => fn(),
    onChange = () => {},
    onError = () => {},
  } = {}) {
    if (!['server', 'client'].includes(side)) throw new TypeError('Invalid state side');
    this.side = side;
    this.ownerAlive = ownerAlive;
    this.run = run;
    this.onChange = onChange;
    this.onError = onError;
    this.SharedState = side === 'server' ? WriteSharedState : ReadSharedState;
    this.records = new Map();
    // Advances on every attach, destroy and value or scope change, so a reader can skip scanning
    // bags while nothing changed.
    this.changeClock = 0;
    this.notifications = [];
    this.notifying = false;
  }
  attach(id) {
    if (!Number.isInteger(id) || id < 1 || id > 0xffffffff)
      throw new TypeError('Invalid entity ID');
    if (!this.records.has(id)) {
      this.records.set(id, {
        id,
        version: Symbol('state version'),
        values: new Map(),
        listeners: new Set(),
      });
      this.changeClock += 1;
    }
  }
  bag(id, owner) {
    const record = this.records.get(id);
    if (!record) throw new Error('Unknown entity');
    return new this.SharedState(construct, { store: this, record, owner });
  }
  forOwner(bag, owner) {
    const binding = bound(bag);
    if (binding.store !== this) throw new Error('Shared state belongs to another store');
    return new this.SharedState(construct, { ...binding, owner });
  }
  destroy(id) {
    const record = this.records.get(id);
    if (!record) return;
    for (const listener of record.listeners) listener.active = false;
    record.listeners.clear();
    this.records.delete(id);
    this.changeClock += 1;
  }
  disconnect() {
    for (const id of this.records.keys()) this.destroy(id);
  }
  removeResource(owner) {
    for (const record of this.records.values()) {
      for (const listener of record.listeners) {
        if (listener.owner === owner) {
          listener.active = false;
          record.listeners.delete(listener);
        }
      }
    }
  }
  report(error, owner) {
    try {
      this.onError(error, owner);
    } catch {
      /* Error reporting cannot abort state delivery. */
    }
  }
  invoke(owner, fn) {
    try {
      const result = this.run(owner, fn);
      if (result?.then) result.catch((error) => this.report(error, owner));
    } catch (error) {
      this.report(error, owner);
    }
  }
  changed(record, key, value, previous) {
    this.notifications.push({ record, key, value, previous, listeners: [...record.listeners] });
    if (this.notifying) return;
    this.notifying = true;
    try {
      while (this.notifications.length) {
        const change = this.notifications.shift();
        if (this.records.get(change.record.id) !== change.record) continue;
        for (const listener of change.listeners) {
          if (
            !listener.active ||
            !this.ownerAlive(listener.owner) ||
            (listener.key !== null && listener.key !== change.key)
          )
            continue;
          this.invoke(listener.owner, () =>
            listener.handler(change.key, decodeValue(change.value), decodeValue(change.previous)),
          );
        }
        // EventMap.entityStateChange is (entity, key, value, previous).
        this.invoke(undefined, () =>
          this.onChange(
            change.record.id,
            change.key,
            decodeValue(change.value),
            decodeValue(change.previous),
          ),
        );
      }
    } finally {
      this.notifying = false;
    }
  }
  project(id, isOwner = false) {
    if (this.side !== 'server') throw new Error('Client cannot project authoritative state');
    const record = this.records.get(id);
    if (!record) throw new Error('Unknown entity');
    return [...record.values]
      .filter(([, value]) => value.scope === 'broadcast' || (isOwner && value.scope === 'owner'))
      .map(([key, { kind, value }]) => ({ key, kind, value }));
  }
  replace(id, entries) {
    if (this.side !== 'client') throw new Error('Server cannot accept client state snapshots');
    const record = this.records.get(id);
    if (!record) throw new Error('Unknown entity');
    if (!Array.isArray(entries) || entries.length > LIMITS.keys)
      throw new RangeError('Invalid state snapshot');
    const next = new Map();
    for (const entry of entries) {
      const key = keyName(entry?.key);
      if (next.has(key)) throw new TypeError('Duplicate state key');
      if (
        !['boolean', 'number', 'string', 'json'].includes(entry.kind) ||
        (entry.kind === 'json'
          ? typeof entry.value !== 'string'
          : typeof entry.value !== entry.kind)
      )
        throw new TypeError('Invalid state value');
      const normalized =
        entry.kind === 'json' ? encodeValue(JSON.parse(entry.value)) : encodeValue(entry.value);
      if (entry.kind === 'json' && encoder.encode(entry.value).length > LIMITS.valueBytes)
        throw new RangeError('State value exceeds 4096 bytes');
      next.set(key, normalized);
    }
    const previous = record.values;
    record.values = next;
    for (const key of new Set([...previous.keys(), ...next.keys()])) {
      if (!sameValue(previous.get(key), next.get(key)))
        this.changed(record, key, next.get(key), previous.get(key));
    }
  }
}

module.exports = { EntityStateStore, LIMITS };
