'use strict';

// Who may sign in to the server's KCDS-Admin:
//
//   owner       the server owner: KCDS_ADMIN_PASSWORD (account name "owner"); everything
//   moderator   accounts the owner adds in the admin: players (kick, ban, message, teleport), the
//               log and the status; not the console, the resources or the accounts
//
// Moderators live in admin-accounts.json in the resources folder with a scrypt hash of their
// password (a random salt each), never the password. The owner's password is only held in memory
// as a hash too. Checks take the same time whether the name exists or not.

const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const { promisify } = require('node:util');

const scrypt = promisify(crypto.scrypt);

const FILE = 'admin-accounts.json';
const OWNER = 'owner';
const NAME = /^[A-Za-z0-9_.-]{2,32}$/;
const MIN_PASSWORD = 8;
const MAX_PASSWORD_BYTES = 128;
const MAX_ACCOUNTS = 64;
const KEY_BYTES = 32;
const SCRYPT = { N: 16384, r: 8, p: 1, maxmem: 64 * 1024 * 1024 };

function hashPassword(password, salt = crypto.randomBytes(16)) {
  const key = crypto.scryptSync(password, salt, KEY_BYTES, SCRYPT);
  return `scrypt$${salt.toString('hex')}$${key.toString('hex')}`;
}

// Off the event loop's thread: a sign-in never stalls the resource host.
async function checkPassword(password, stored) {
  const [kind, saltHex, keyHex] = String(stored).split('$');
  if (kind !== 'scrypt' || !/^[0-9a-f]{32}$/.test(saltHex) || !/^[0-9a-f]{64}$/.test(keyHex)) {
    return false;
  }
  const key = await scrypt(password, Buffer.from(saltHex, 'hex'), KEY_BYTES, SCRYPT);
  return crypto.timingSafeEqual(key, Buffer.from(keyHex, 'hex'));
}

function validPassword(password) {
  return (
    typeof password === 'string' &&
    password.length >= MIN_PASSWORD &&
    Buffer.byteLength(password) <= MAX_PASSWORD_BYTES
  );
}

class AdminAccounts {
  // ownerPassword: KCDS_ADMIN_PASSWORD; root: the resources folder.
  constructor(ownerPassword, root, log = () => {}) {
    this.file = root ? path.join(root, FILE) : '';
    this.log = log;
    this.owner = validPassword(ownerPassword) ? hashPassword(ownerPassword) : '';
    // A hash to check against for names that do not exist, so they take as long.
    this.decoy = hashPassword(crypto.randomBytes(16).toString('hex'));
    this.moderators = new Map();
    this.load();
  }

  // Whether the admin can be signed in to at all.
  enabled() {
    return this.owner !== '';
  }

  load() {
    if (!this.file || !fs.existsSync(this.file)) {
      return;
    }
    try {
      const data = JSON.parse(fs.readFileSync(this.file, 'utf8'));
      for (const account of Array.isArray(data?.accounts) ? data.accounts : []) {
        if (
          NAME.test(account?.name) &&
          account.name !== OWNER &&
          typeof account.hash === 'string'
        ) {
          this.moderators.set(account.name, account.hash);
        }
      }
    } catch (error) {
      this.log(`${FILE} could not be read: ${error.message}; only the owner can sign in`);
    }
  }

  save() {
    if (!this.file) {
      return;
    }
    const accounts = [...this.moderators].map(([name, hash]) => ({
      name,
      role: 'moderator',
      hash,
    }));
    const temporary = `${this.file}.${process.pid}.tmp`;
    fs.writeFileSync(temporary, JSON.stringify({ accounts }, null, 2) + '\n', {
      encoding: 'utf8',
      mode: 0o600,
    });
    fs.renameSync(temporary, this.file);
  }

  // The account's role ('owner' or 'moderator') when the password is right, else null.
  async verify(name, password) {
    if (typeof name !== 'string' || !validPassword(password) || !this.enabled()) {
      // Same work for a malformed attempt.
      await checkPassword(String(password ?? ''), this.decoy);
      return null;
    }
    if (name === OWNER) {
      return (await checkPassword(password, this.owner)) ? 'owner' : null;
    }
    const stored = this.moderators.get(name);
    const right = await checkPassword(password, stored ?? this.decoy);
    return stored && right ? 'moderator' : null;
  }

  list() {
    return [
      { name: OWNER, role: 'owner' },
      ...[...this.moderators.keys()].sort().map((name) => ({ name, role: 'moderator' })),
    ];
  }

  // Adds a moderator or sets a moderator's new password.
  setModerator(name, password) {
    if (!NAME.test(name ?? '') || name === OWNER) {
      throw new TypeError('A name of 2 to 32 letters, digits, _ . or -, not "owner"');
    }
    if (!validPassword(password)) {
      throw new TypeError(`A password of ${MIN_PASSWORD} to ${MAX_PASSWORD_BYTES} bytes`);
    }
    if (!this.moderators.has(name) && this.moderators.size >= MAX_ACCOUNTS) {
      throw new RangeError('Too many accounts');
    }
    this.moderators.set(name, hashPassword(password));
    this.save();
  }

  removeModerator(name) {
    if (!this.moderators.delete(name)) {
      return false;
    }
    this.save();
    return true;
  }
}

module.exports = { AdminAccounts, hashPassword, checkPassword, OWNER };
