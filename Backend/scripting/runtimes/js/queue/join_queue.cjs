'use strict';

// A full server's join queue and the whitelist (Queue).
//
// When every place is taken, a joining player waits in line instead of being refused; the native
// host keeps the line and tells each waiting player their place, how long they waited and the
// resources' status (WAIT_STATUS, kcd2s/transport/handshake.hpp), and lets the first in line in
// when a place opens. First come, first served, with no setup; resources reorder it:
//
//   Queue.list()                         -> [{ id, nickname, steamId, ip, waitedSeconds, priority }]
//                                           in the queue's order (the first is next)
//   Queue.setPriority(id, priority)      a higher priority goes first; equal ones by arrival
//   Queue.setStatus(id, { line, fields }) a line and up to 3 { label, value } the player's queue
//                                           popup shows (plain text; none: their place and time)
//   Queue.reject(id, reason?)            turns them away with the reason
//
//   events: playerQueued(entry), playerUnqueued(entry, 'admitted' | 'left' | 'rejected')
//
// The whitelist: whitelist.json in the server's resources folder,
//   { "enabled": true, "nicknames": ["Henry"], "steamIds": ["7656..."], "reason": "..." }
// lets in only who is listed (a nickname matches ignoring case); others are refused with the
// reason, in the queue and at playerConnecting alike. The identity is what the player's client
// reports, unverified.
//
//   from the host: { op: 'joinQueued', id, ip, nickname, steamId }, { op: 'joinUnqueued', id, reason }
//   to the host:   { op: 'joinQueueSet', id, priority?, line?, fields?: [[label, value]], reject?,
//                    reason? }

const fs = require('node:fs');
const path = require('node:path');

const WHITELIST_FILE = 'whitelist.json';
const MAX_FIELDS = 3;
const MAX_LABEL_BYTES = 32;
const MAX_VALUE_BYTES = 96;
const MAX_LINE_BYTES = 256;
const MAX_REASON_BYTES = 256;
const UNQUEUE_REASONS = new Set(['admitted', 'left', 'rejected']);

function isObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function text(value, what, max) {
  if (typeof value !== 'string') {
    throw new TypeError(`${what} is a text`);
  }
  if (Buffer.byteLength(value, 'utf8') > max) {
    throw new RangeError(`${what} is at most ${max} bytes`);
  }
  return value;
}

function stringList(value, what, log) {
  if (value === undefined) {
    return [];
  }
  if (!Array.isArray(value) || value.some((entry) => typeof entry !== 'string')) {
    log(`${WHITELIST_FILE}: ${what} must be a list of texts; it is left out`);
    return [];
  }
  return value;
}

class JoinQueue {
  constructor(host) {
    this.host = host;
    // id -> { id, nickname, steamId, ip, since, priority }
    this.entries = new Map();
    this.whitelist = { enabled: false, nicknames: new Set(), steamIds: new Set(), reason: '' };
  }

  loadWhitelist(root) {
    const file = path.join(root, WHITELIST_FILE);
    if (!fs.existsSync(file)) {
      return;
    }
    const log = (line) => this.host.bridge.log('host', line);
    let config;
    try {
      config = JSON.parse(fs.readFileSync(file, 'utf8'));
    } catch (error) {
      log(`${WHITELIST_FILE}: ${error.message}; no whitelist`);
      return;
    }
    if (!isObject(config)) {
      log(`${WHITELIST_FILE} must be an object; no whitelist`);
      return;
    }
    this.whitelist = {
      enabled: config.enabled === true,
      nicknames: new Set(
        stringList(config.nicknames, 'nicknames', log).map((name) => name.toLowerCase()),
      ),
      steamIds: new Set(stringList(config.steamIds, 'steamIds', log)),
      reason:
        typeof config.reason === 'string'
          ? config.reason
          : 'This server only admits listed players.',
    };
  }

  // Whether the whitelist lets this identity in (always without a whitelist).
  admits(identity) {
    const list = this.whitelist;
    if (!list.enabled) {
      return true;
    }
    return (
      (identity.steamId !== '' && list.steamIds.has(identity.steamId)) ||
      (identity.nickname !== '' && list.nicknames.has(identity.nickname.toLowerCase()))
    );
  }

  view(entry) {
    return Object.freeze({
      id: entry.id,
      nickname: entry.nickname,
      steamId: entry.steamId,
      ip: entry.ip,
      waitedSeconds: Math.max(0, Math.floor((Date.now() - entry.since) / 1000)),
      priority: entry.priority,
    });
  }

  ordered() {
    return [...this.entries.values()].sort((a, b) => {
      if (a.priority !== b.priority) {
        return b.priority - a.priority;
      }
      return a.since - b.since || a.id - b.id;
    });
  }

  queued(message) {
    if (!Number.isInteger(message.id)) {
      return;
    }
    const entry = {
      id: message.id,
      nickname: typeof message.nickname === 'string' ? message.nickname : '',
      steamId: typeof message.steamId === 'string' ? message.steamId : '',
      ip: typeof message.ip === 'string' ? message.ip : '',
      since: Date.now(),
      priority: 0,
    };
    this.entries.set(entry.id, entry);
    if (!this.admits(entry)) {
      this.reject(entry.id, this.whitelist.reason);
      return;
    }
    this.host.emit('playerQueued', this.view(entry));
  }

  unqueued(message) {
    const entry = this.entries.get(message.id);
    if (!entry) {
      return;
    }
    this.entries.delete(entry.id);
    const reason = UNQUEUE_REASONS.has(message.reason) ? message.reason : 'left';
    this.host.emit('playerUnqueued', this.view(entry), entry.rejected ? 'rejected' : reason);
  }

  entry(id, what) {
    const entry = this.entries.get(id);
    if (!entry) {
      throw new RangeError(`${what}: no player waits in the queue with id ${String(id)}`);
    }
    return entry;
  }

  setPriority(id, priority) {
    const entry = this.entry(id, 'Queue.setPriority');
    if (!Number.isSafeInteger(priority)) {
      throw new TypeError('Queue.setPriority takes a whole number');
    }
    entry.priority = priority;
    this.host.bridge.send({ op: 'joinQueueSet', id, priority });
  }

  setStatus(id, status) {
    this.entry(id, 'Queue.setStatus');
    if (!isObject(status)) {
      throw new TypeError('Queue.setStatus takes { line?, fields? }');
    }
    const message = { op: 'joinQueueSet', id };
    if (status.line !== undefined) {
      message.line = text(status.line, 'Queue.setStatus line', MAX_LINE_BYTES);
    }
    if (status.fields !== undefined) {
      if (!Array.isArray(status.fields) || status.fields.length > MAX_FIELDS) {
        throw new TypeError(`Queue.setStatus fields are at most ${MAX_FIELDS} { label, value }`);
      }
      message.fields = status.fields.map((field) => {
        if (!isObject(field) || typeof field.label !== 'string' || field.label === '') {
          throw new TypeError('Queue.setStatus fields are { label, value } with a label');
        }
        return [
          text(field.label, 'a field label', MAX_LABEL_BYTES),
          text(String(field.value ?? ''), 'a field value', MAX_VALUE_BYTES),
        ];
      });
    }
    this.host.bridge.send(message);
  }

  reject(id, reason = '') {
    const entry = this.entry(id, 'Queue.reject');
    entry.rejected = true;
    this.host.bridge.send({
      op: 'joinQueueSet',
      id,
      reject: true,
      reason: text(reason, 'Queue.reject reason', MAX_REASON_BYTES),
    });
  }

  api() {
    const self = this;
    return Object.freeze({
      list: () => self.ordered().map((entry) => self.view(entry)),
      setPriority: (id, priority) => self.setPriority(id, priority),
      setStatus: (id, status) => self.setStatus(id, status),
      reject: (id, reason) => self.reject(id, reason),
    });
  }
}

module.exports = { JoinQueue };
