'use strict';

// The server's own bans (KCDS-Admin's Ban): kept in bans.json in the resources
// folder, checked when a player connects, after the licence and the password and before the
// whitelist and every playerConnecting handler. A ban names identifiers as "kind:id" (the kinds of
// player.identifiers: steam, epic, xbox, gog, discord, hardware, ip); a player matching any of them
// is turned away with the ban's reason and its end. These are this server's bans only; the KCDS
// central ban list is the Master's.

const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');

const FILE = 'bans.json';
const MAX_BANS = 10000;
const MAX_REASON_BYTES = 300;
const MAX_NAME_BYTES = 64;
const MAX_IDENTIFIERS = 8;
const IDENTIFIER = /^(steam|epic|xbox|gog|discord|hardware|ip):[A-Za-z0-9._:-]{1,128}$/;

function printable(text, limit) {
  if (typeof text !== 'string' || Buffer.byteLength(text) > limit) {
    return false;
  }
  for (const character of text) {
    const code = character.codePointAt(0);
    if (code < 32 || code === 127) {
      return false;
    }
  }
  return true;
}

class ServerBans {
  // root: the resources folder; now(): milliseconds.
  constructor(root, log = () => {}, now = () => Date.now()) {
    this.file = root ? path.join(root, FILE) : '';
    this.log = log;
    this.now = now;
    this.bans = [];
    this.load();
  }

  load() {
    if (!this.file || !fs.existsSync(this.file)) {
      return;
    }
    try {
      const data = JSON.parse(fs.readFileSync(this.file, 'utf8'));
      const bans = Array.isArray(data?.bans) ? data.bans : [];
      this.bans = bans.filter((ban) => this.valid(ban)).slice(0, MAX_BANS);
    } catch (error) {
      this.log(`${FILE} could not be read: ${error.message}; no server bans are active`);
    }
  }

  valid(ban) {
    return (
      ban &&
      typeof ban.id === 'string' &&
      Array.isArray(ban.identifiers) &&
      ban.identifiers.length > 0 &&
      ban.identifiers.length <= MAX_IDENTIFIERS &&
      ban.identifiers.every((identifier) => IDENTIFIER.test(identifier)) &&
      printable(ban.reason, MAX_REASON_BYTES) &&
      printable(ban.by, MAX_NAME_BYTES) &&
      Number.isSafeInteger(ban.at) &&
      Number.isSafeInteger(ban.until) &&
      ban.until >= 0
    );
  }

  save() {
    if (!this.file) {
      return;
    }
    const temporary = `${this.file}.${process.pid}.tmp`;
    fs.writeFileSync(temporary, JSON.stringify({ bans: this.bans }, null, 2) + '\n', 'utf8');
    fs.renameSync(temporary, this.file);
  }

  active(ban) {
    return ban.until === 0 || this.now() < ban.until;
  }

  // Every ban, the running ones first.
  list() {
    return this.bans
      .map((ban) => ({ ...ban, identifiers: [...ban.identifiers], active: this.active(ban) }))
      .sort((a, b) => Number(b.active) - Number(a.active) || b.at - a.at);
  }

  // A new ban: { identifiers: ['steam:7656...'], reason, by, minutes (0 for ever) }.
  add({ identifiers, reason, by, minutes = 0 }) {
    const unique = [...new Set(Array.isArray(identifiers) ? identifiers : [])];
    const ban = {
      id: crypto.randomBytes(8).toString('hex'),
      identifiers: unique,
      reason: typeof reason === 'string' ? reason.trim() : '',
      by,
      at: this.now(),
      until: Number.isSafeInteger(minutes) && minutes > 0 ? this.now() + minutes * 60000 : 0,
    };
    if (ban.reason === '') {
      ban.reason = 'Banned from this server.';
    }
    if (!this.valid(ban) || !Number.isSafeInteger(minutes) || minutes < 0) {
      throw new TypeError('A ban needs 1 to 8 identifiers, a reason and its length in minutes');
    }
    if (this.bans.length >= MAX_BANS) {
      throw new RangeError('The ban list is full');
    }
    this.bans.push(ban);
    this.save();
    return { ...ban };
  }

  remove(id) {
    const before = this.bans.length;
    this.bans = this.bans.filter((ban) => ban.id !== id);
    if (this.bans.length === before) {
      return false;
    }
    this.save();
    return true;
  }

  // The running ban matching any of these identifiers ([{ kind, id }]), or null.
  match(identifiers) {
    const keys = new Set(identifiers.map(({ kind, id }) => `${kind}:${id}`));
    return (
      this.bans.find((ban) => this.active(ban) && ban.identifiers.some((key) => keys.has(key))) ??
      null
    );
  }

  // What the banned player is told.
  refusal(ban) {
    const reason = /[.!?]$/.test(ban.reason) ? ban.reason : `${ban.reason}.`;
    let text = `You are banned from this server: ${reason}`;
    if (ban.until > 0) {
      text += ` Until ${new Date(ban.until).toISOString().slice(0, 16).replace('T', ' ')} UTC.`;
    }
    return text;
  }
}

module.exports = { ServerBans, IDENTIFIER };
