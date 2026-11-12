'use strict';

// The Master confirms a current game licence and signs a pass for one server handshake.
// This gate runs before the whitelist and playerConnecting handlers. Every server requires a
// legal game: listed, unlisted, LAN and localhost alike, and no setting turns it off.
// A listed server's passes name its listing; an unlisted server's name the address the player's
// game dialled ("addr:<ip>:<port>"), which must be this server's port and one of its own
// addresses: a network interface's, loopback, KCDS_SERVER_PUBLIC_ADDRESS's, or the one the Master
// saw this server ask for its keys from (a server behind NAT). A pass made for another server
// cannot be relayed here.
const crypto = require('node:crypto');
const dns = require('node:dns');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');

const CONFIG_FILE = 'licence.json';
const STORES = ['steam', 'gog', 'epic', 'xbox'];
const DEFAULT_MASTER = 'https://master.kcdsynced.com';
const MAX_PASS_BYTES = 1024;
const MAX_PASS_SECONDS = 300;
const CLOCK_SKEW_SECONDS = 30;
const NONCE_SLACK_SECONDS = 60;
const MAX_NONCES = 100000;
const KEY_REFRESH_MS = 10 * 60 * 1000;
const KEY_TIMEOUT_MS = 5000;
const MAX_KEY_BODY_BYTES = 64 * 1024;
const MAX_KEYS = 16;
// How long a resolved KCDS_SERVER_PUBLIC_ADDRESS host name is kept.
const PUBLIC_ADDRESS_REFRESH_MS = 10 * 60 * 1000;
// How long the network interfaces' addresses are kept: reading them can take a large part of a
// second on Windows, too long to repeat for every join.
const INTERFACE_REFRESH_MS = 30 * 1000;

const REASONS = {
  missing: 'This server only lets in a legally owned game: join through the KCDS launcher.',
  malformed: 'Your licence pass is not valid. Rejoin through the KCDS launcher.',
  unsigned: 'Your licence pass was not signed by the KCDS Master. Rejoin through the launcher.',
  expired: 'Your licence pass expired or has an invalid lifetime. Rejoin through the launcher.',
  wrongServer: 'Your licence pass is for another server. Rejoin through the KCDS launcher.',
  wrongBinding: 'Your licence pass is for another connection. Rejoin through the KCDS launcher.',
  reused: 'Your licence pass was already used. Rejoin through the KCDS launcher.',
  store: 'This server does not let in games from your store.',
  strength: 'Your store could not confirm a current game licence.',
  familySharing: 'This server does not let in family-shared copies.',
  sharingUnknown: 'This server cannot confirm unshared ownership for this store.',
  configuration: 'This server cannot validate game licences because its configuration is invalid.',
  capacity: 'This server cannot validate another licence pass yet. Please try again shortly.',
};

// Kingdom Come: Deliverance on Steam: a pass's apps say the Master checked the player owns it.
const KCD1_APP_ID = 379430;

// The other games a pass vouches for: a short list of the apps the Master checks, none twice.
function validPassApps(apps) {
  return (
    Array.isArray(apps) &&
    apps.length >= 1 &&
    apps.length <= 1 &&
    apps.every((app) => app === KCD1_APP_ID) &&
    new Set(apps).size === apps.length
  );
}

function decodeBase64Url(text) {
  if (typeof text !== 'string' || !/^[A-Za-z0-9_-]+$/.test(text)) {
    return null;
  }
  const decoded = Buffer.from(text, 'base64url');
  return decoded.toString('base64url') === text ? decoded : null;
}

function validAccount(value) {
  return typeof value === 'string' && /^[\x21-\x7e]{1,128}$/.test(value);
}

function validKeyID(value) {
  return typeof value === 'string' && /^[A-Za-z0-9_-]{1,32}$/.test(value);
}

function validBinding(value) {
  return typeof value === 'string' && /^[a-f0-9]{16}$/.test(value);
}

function validListing(value) {
  return typeof value === 'string' && /^[A-Za-z0-9]{1,128}$/.test(value);
}

// An IP address as compared here: no IPv6 zone, IPv4-mapped IPv6 as IPv4; '' for no address.
function plainAddress(text) {
  if (typeof text !== 'string') {
    return '';
  }
  let address = text
    .trim()
    .replace(/^\[|\]$/g, '')
    .replace(/%.*$/, '');
  if (/^::ffff:[0-9.]+$/i.test(address)) {
    address = address.slice(7);
  }
  return net.isIP(address) ? address.toLowerCase() : '';
}

// The address and port an unlisted server's pass names ("addr:<ip>:<port>", IPv6 in brackets),
// or null.
function addressAudience(server) {
  if (typeof server !== 'string' || server.length > 128) {
    return null;
  }
  const parts = /^addr:(.+):([0-9]{1,5})$/.exec(server);
  if (!parts) {
    return null;
  }
  const address = plainAddress(parts[1]);
  const port = Number(parts[2]);
  if (!address || port < 1 || port > 65535) {
    return null;
  }
  return { address, port };
}

// The host of a "host:port" (or bare host) public address, '' for none.
function publicHost(text) {
  if (typeof text !== 'string' || text.trim() === '' || text.length > 255) {
    return '';
  }
  const value = text.trim();
  const bracketed = /^\[([^\]]+)\](?::[0-9]{1,5})?$/.exec(value);
  if (bracketed) {
    return bracketed[1];
  }
  if (net.isIP(value)) {
    return value;
  }
  return value.replace(/:[0-9]{1,5}$/, '');
}

function masterURL(value, allowLoopbackHTTP) {
  if (typeof value !== 'string' || value.length > 2048) {
    return null;
  }
  try {
    const url = new URL(value);
    const loopback = ['localhost', '127.0.0.1', '[::1]'].includes(url.hostname);
    if (
      (url.protocol !== 'https:' && !(allowLoopbackHTTP && loopback && url.protocol === 'http:')) ||
      url.username ||
      url.password ||
      url.search ||
      url.hash
    ) {
      return null;
    }
    return url.href.replace(/\/+$/, '');
  } catch {
    return null;
  }
}

async function readKeys(answer) {
  if (!answer.ok || !answer.body || typeof answer.body.getReader !== 'function') {
    throw new Error('key response unavailable');
  }
  const length = answer.headers?.get('content-length');
  if (length && (!/^\d+$/.test(length) || Number(length) > MAX_KEY_BODY_BYTES)) {
    throw new Error('key response too large');
  }
  const reader = answer.body.getReader();
  const chunks = [];
  let bytes = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) {
        break;
      }
      bytes += value.byteLength;
      if (bytes > MAX_KEY_BODY_BYTES) {
        void reader.cancel().catch(() => {});
        throw new Error('key response too large');
      }
      chunks.push(Buffer.from(value));
    }
  } finally {
    reader.releaseLock();
  }
  const data = JSON.parse(Buffer.concat(chunks, bytes).toString('utf8'));
  const address = plainAddress(data?.address);
  if (!Array.isArray(data?.keys) || data.keys.length === 0 || data.keys.length > MAX_KEYS) {
    throw new Error('invalid key response');
  }
  const fresh = new Map();
  for (const entry of data.keys) {
    if (!validKeyID(entry?.kid) || typeof entry.ed25519 !== 'string') {
      throw new Error('invalid key response');
    }
    const raw = Buffer.from(entry.ed25519, 'base64');
    if (raw.length !== 32 || raw.toString('base64') !== entry.ed25519 || fresh.has(entry.kid)) {
      throw new Error('invalid key response');
    }
    fresh.set(
      entry.kid,
      crypto.createPublicKey({
        key: { kty: 'OKP', crv: 'Ed25519', x: raw.toString('base64url') },
        format: 'jwk',
      }),
    );
  }
  return { keys: fresh, address };
}

class LicenceGate {
  constructor(host, options = {}) {
    this.host = host;
    this.fetch = options.fetch ?? globalThis.fetch;
    this.now = options.now ?? (() => Date.now());
    // Tests may inject a transport for a local HTTP fixture; production has no HTTP fallback.
    this.allowLoopbackHTTP = typeof options.fetch === 'function';
    // This server's network interfaces, its public address and the resolver for a host name in it
    // (tests inject them).
    this.interfaces = options.interfaces ?? (() => os.networkInterfaces());
    this.publicAddress = options.publicAddress ?? process.env.KCDS_SERVER_PUBLIC_ADDRESS ?? '';
    this.lookup = options.lookup ?? ((host) => dns.promises.lookup(host, { all: true }));
    this.keyTimeoutMs = Math.min(
      KEY_TIMEOUT_MS,
      Math.max(1, options.keyTimeoutMs ?? KEY_TIMEOUT_MS),
    );
    this.config = {
      stores: [...STORES],
      minimumStrength: 'licence',
      familySharing: true,
      master: DEFAULT_MASTER,
    };
    this.configInvalid = false;
    this.listing = '';
    // The UDP port players dial (the native server's hostSettings), 0 until told.
    this.port = 0;
    // The address the Master last saw this server ask for its keys from.
    this.masterAddress = '';
    this.publicResolved = { at: 0, addresses: [] };
    this.interfacesRead = { at: 0, addresses: null };
    this.keys = new Map();
    this.keysFetchedAt = 0;
    this.keysRequest = null;
    this.nonces = new Map();
  }

  load(root) {
    const file = path.join(root, CONFIG_FILE);
    if (!fs.existsSync(file)) {
      return;
    }
    try {
      const raw = JSON.parse(fs.readFileSync(file, 'utf8'));
      if (
        raw === null ||
        typeof raw !== 'object' ||
        Array.isArray(raw) ||
        ('required' in raw && typeof raw.required !== 'boolean') ||
        ('familySharing' in raw && typeof raw.familySharing !== 'boolean') ||
        ('master' in raw && typeof raw.master !== 'string') ||
        ('minimumStrength' in raw && !['account', 'licence'].includes(raw.minimumStrength)) ||
        ('stores' in raw &&
          (!Array.isArray(raw.stores) || raw.stores.some((store) => !STORES.includes(store))))
      ) {
        throw new Error('invalid configuration');
      }
      const master = masterURL(raw.master ?? this.config.master, this.allowLoopbackHTTP);
      if (!master) {
        throw new Error('invalid master URL');
      }
      if ('required' in raw) {
        this.host.bridge.log(
          'host',
          `${CONFIG_FILE}: "required" is not read: every KCDS server requires a legal game`,
        );
      }
      this.config = {
        stores: raw.stores ? [...raw.stores] : this.config.stores,
        minimumStrength: 'licence',
        familySharing: raw.familySharing ?? this.config.familySharing,
        master,
      };
      this.configInvalid = false;
    } catch {
      this.configInvalid = true;
      this.host.bridge.log('host', `${CONFIG_FILE}: invalid configuration; joins are refused`);
    }
  }

  setListing(id) {
    if (validListing(id)) {
      this.listing = id;
    }
  }

  setPort(port) {
    if (Number.isInteger(port) && port >= 1 && port <= 65535) {
      this.port = port;
    }
  }

  // This server's own addresses: loopback, its network interfaces', the Master's view of it and
  // its configured public address (a host name resolved at most every PUBLIC_ADDRESS_REFRESH_MS).
  async ownAddresses() {
    const own = new Set(['127.0.0.1', '::1']);
    for (const address of this.interfaceAddresses()) {
      own.add(address);
    }
    if (this.masterAddress) {
      own.add(this.masterAddress);
    }
    const host = publicHost(this.publicAddress);
    if (host && plainAddress(host)) {
      own.add(plainAddress(host));
    } else if (host) {
      for (const address of await this.resolvePublic(host)) {
        own.add(address);
      }
    }
    return own;
  }

  // The network interfaces' addresses, read again at most every INTERFACE_REFRESH_MS.
  interfaceAddresses() {
    const age = this.now() - this.interfacesRead.at;
    if (this.interfacesRead.addresses && age >= 0 && age < INTERFACE_REFRESH_MS) {
      return this.interfacesRead.addresses;
    }
    let interfaces = {};
    try {
      interfaces = this.interfaces() ?? {};
    } catch {
      interfaces = {};
    }
    const addresses = [];
    for (const entries of Object.values(interfaces)) {
      for (const entry of entries ?? []) {
        const address = plainAddress(entry?.address);
        if (address) {
          addresses.push(address);
        }
      }
    }
    this.interfacesRead = { at: this.now(), addresses };
    return addresses;
  }

  // The addresses a public host name resolves to, looked up again after
  // PUBLIC_ADDRESS_REFRESH_MS; none when it does not resolve.
  async resolvePublic(host) {
    const age = this.now() - this.publicResolved.at;
    if (age >= 0 && age < PUBLIC_ADDRESS_REFRESH_MS) {
      return this.publicResolved.addresses;
    }
    let addresses = [];
    try {
      const found = await this.lookup(host);
      addresses = (Array.isArray(found) ? found : [found])
        .map((entry) => plainAddress(entry?.address))
        .filter((address) => address !== '');
    } catch {
      addresses = [];
    }
    this.publicResolved = { at: this.now(), addresses };
    return addresses;
  }

  // Whether a pass's server names this one: its listing, or this server's port and one of its
  // own addresses.
  async isThisServer(server) {
    if (this.listing && server === this.listing) {
      return true;
    }
    const audience = addressAudience(server);
    if (!audience || !this.port || audience.port !== this.port) {
      return false;
    }
    return (await this.ownAddresses()).has(audience.address);
  }

  async refreshKeys() {
    const controller = new AbortController();
    let timeout;
    try {
      const master = masterURL(this.config.master, this.allowLoopbackHTTP);
      if (!master) {
        return false;
      }
      const fetchKeys = async () => {
        const answer = await this.fetch(`${master}/v1/licence/keys`, {
          signal: controller.signal,
          redirect: 'error',
        });
        return readKeys(answer);
      };
      const expired = new Promise((_, reject) => {
        timeout = setTimeout(() => {
          controller.abort();
          reject(new Error('key request timed out'));
        }, this.keyTimeoutMs);
      });
      const answer = await Promise.race([fetchKeys(), expired]);
      this.keys = answer.keys;
      if (answer.address) {
        this.masterAddress = answer.address;
      }
      this.keysFetchedAt = this.now();
      return true;
    } catch {
      this.host.bridge.log(
        'host',
        'licence keys unavailable; joins needing fresh keys are refused',
      );
      return false;
    } finally {
      clearTimeout(timeout);
      controller.abort();
    }
  }

  async keyFor(kid) {
    const age = this.now() - this.keysFetchedAt;
    if (this.keys.has(kid) && age >= 0 && age < KEY_REFRESH_MS) {
      return this.keys.get(kid);
    }
    if (!this.keysRequest) {
      this.keysRequest = this.refreshKeys().finally(() => {
        this.keysRequest = null;
      });
    }
    if (!(await this.keysRequest)) {
      return null;
    }
    return this.keys.get(kid) ?? null;
  }

  async check(text, identity = {}) {
    const refuse = (why) => ({ allowed: false, reason: REASONS[why], why, pass: null });
    if (this.configInvalid) {
      return refuse('configuration');
    }
    if (typeof text !== 'string' || text === '') {
      return refuse('missing');
    }
    if (text.length > MAX_PASS_BYTES) {
      return refuse('malformed');
    }
    const [encoded, signed, extra] = text.split('.');
    const body = decodeBase64Url(encoded);
    const signature = decodeBase64Url(signed);
    if (!body || !signature || signature.length !== 64 || extra !== undefined) {
      return refuse('malformed');
    }
    let pass;
    try {
      pass = JSON.parse(body.toString('utf8'));
    } catch {
      return refuse('malformed');
    }
    if (
      pass === null ||
      typeof pass !== 'object' ||
      Array.isArray(pass) ||
      pass.v !== 1 ||
      !validKeyID(pass.kid) ||
      !STORES.includes(pass.store) ||
      !validAccount(pass.account) ||
      (pass.owner !== undefined && !validAccount(pass.owner)) ||
      (pass.discord !== undefined &&
        (typeof pass.discord !== 'string' || !/^[0-9]{1,20}$/.test(pass.discord))) ||
      (pass.apps !== undefined && !validPassApps(pass.apps)) ||
      (pass.store === 'steam' && !/^[0-9]{1,20}$/.test(pass.account)) ||
      typeof pass.nonce !== 'string' ||
      !/^[A-Za-z0-9_-]{16,128}$/.test(pass.nonce) ||
      (!validListing(pass.server) && !addressAudience(pass.server))
    ) {
      return refuse('malformed');
    }
    const key = await this.keyFor(pass.kid);
    if (!key || !crypto.verify(null, Buffer.from(encoded), key, signature)) {
      return refuse('unsigned');
    }
    const seconds = Math.floor(this.now() / 1000);
    if (
      !Number.isSafeInteger(pass.iat) ||
      !Number.isSafeInteger(pass.exp) ||
      pass.iat <= 0 ||
      pass.exp <= pass.iat ||
      pass.exp - pass.iat > MAX_PASS_SECONDS ||
      pass.iat > seconds + CLOCK_SKEW_SECONDS ||
      seconds >= pass.exp
    ) {
      return refuse('expired');
    }
    if (!(await this.isThisServer(pass.server))) {
      return refuse('wrongServer');
    }
    if (
      !validBinding(pass.binding) ||
      !validBinding(identity.licenceBinding) ||
      pass.binding !== identity.licenceBinding
    ) {
      return refuse('wrongBinding');
    }
    if (this.nonces.has(pass.nonce)) {
      return refuse('reused');
    }
    if (!this.config.stores.includes(pass.store)) {
      return refuse('store');
    }
    if (pass.strength !== 'licence') {
      return refuse('strength');
    }
    // Microsoft licence tokens identify the licence owner but cannot establish whether
    // the current player uses a shared licence. Their isShared field is not authoritative.
    if (pass.store === 'xbox' && !this.config.familySharing) {
      return refuse('sharingUnknown');
    }
    if (pass.owner && !this.config.familySharing) {
      return refuse('familySharing');
    }
    if (pass.store === 'steam' && identity.steamId && identity.steamId !== pass.account) {
      return refuse('unsigned');
    }
    // There are no awaits between the reuse check and consumption: concurrent joins cannot
    // consume the same pass twice, including when their key requests shared an in-flight fetch.
    if (!this.remember(pass.nonce, pass.exp, seconds)) {
      return refuse('capacity');
    }
    return { allowed: true, reason: '', pass };
  }

  remember(nonce, expires, seconds) {
    if (this.nonces.size >= MAX_NONCES) {
      for (const [old, until] of this.nonces) {
        if (until <= seconds) {
          this.nonces.delete(old);
        }
      }
      if (this.nonces.size >= MAX_NONCES) {
        return false;
      }
    }
    this.nonces.set(nonce, expires + NONCE_SLACK_SECONDS);
    return true;
  }
}

module.exports = { LicenceGate, LICENCE_REASONS: REASONS, KCD1_APP_ID };
