'use strict';

// Sealed client files: every file a resource sends to players is sealed as a KCDS
// asset container (Client/runtime/assets/asset_container.hpp), byte for byte as the game client
// seals and opens one:
//
//   0   8  "KCDSAST1"
//   8   4  format version, 1 (little endian)
//   12  12 the nonce, from the key and the file (seal)
//   24  16 the AES-GCM tag
//   40  2  length N of the resource's name (little endian)
//   42  N  the resource's name (UTF-8)
//   42+N   the file, encrypted with AES-256-GCM; the authenticated data is bytes 0..11 and the
//          name
//
// The key: SHA-256 of "kcds-asset-v1", 0, the resource's name, 0, the secret. The secret comes
// from the server's seal key, kept in its resources folder (stableSecretFor, serverSealKey), and
// goes to each player's game in the resource catalog over its session, so the files players keep
// in their download cache open only with a secret from a session to that server, and a copy of
// them is noise to the plain game and to other mods. What a player's game opens can still be read
// from its memory while it runs.

const fs = require('node:fs');
const path = require('node:path');
const {
  createCipheriv,
  createDecipheriv,
  createHash,
  createHmac,
  randomBytes,
} = require('node:crypto');

const MAGIC = Buffer.from('KCDSAST1', 'ascii');
const VERSION = 1;
const NONCE_AT = 12;
const TAG_AT = 24;
const NAME_AT = 42;
const NONCE_SIZE = 12;
const TAG_SIZE = 16;

// A new resource secret: 32 random bytes as text (the container's secret is text).
function secretFor() {
  return randomBytes(32).toString('base64url');
}

function keyFor(resource, secret) {
  return createHash('sha256')
    .update('kcds-asset-v1')
    .update(Buffer.from([0]))
    .update(Buffer.from(resource, 'utf8'))
    .update(Buffer.from([0]))
    .update(Buffer.from(secret, 'utf8'))
    .digest();
}

function authenticated(header, name) {
  return Buffer.concat([header.subarray(0, NONCE_AT), name]);
}

// `bytes` sealed for `resource` with `secret`.
function seal(bytes, resource, secret) {
  const name = Buffer.from(resource, 'utf8');
  if (name.length > 0xffff) {
    throw new RangeError('A resource name is at most 65535 bytes');
  }
  const header = Buffer.alloc(NAME_AT);
  MAGIC.copy(header, 0);
  header.writeUInt32LE(VERSION, 8);
  // The nonce comes from the key and the file itself: the same file seals to the same bytes (its
  // hash, the download cache's name for it, stays), and two different files never share a nonce
  // under one key.
  const key = keyFor(resource, secret);
  createHmac('sha256', key)
    .update('kcds-seal-nonce')
    .update(Buffer.from([0]))
    .update(bytes)
    .digest()
    .copy(header, NONCE_AT, 0, NONCE_SIZE);
  header.writeUInt16LE(name.length, 40);
  const cipher = createCipheriv('aes-256-gcm', key, header.subarray(NONCE_AT, TAG_AT));
  cipher.setAAD(authenticated(header, name));
  const body = Buffer.concat([cipher.update(bytes), cipher.final()]);
  cipher.getAuthTag().copy(header, TAG_AT);
  return Buffer.concat([header, name, body]);
}

// The file inside, when `sealed` is a container of `resource` sealed with `secret` and nothing in
// it was changed; null otherwise.
function open(sealed, resource, secret) {
  const bytes = Buffer.from(sealed);
  if (
    bytes.length < NAME_AT ||
    !bytes.subarray(0, 8).equals(MAGIC) ||
    bytes.readUInt32LE(8) !== VERSION
  ) {
    return null;
  }
  const length = bytes.readUInt16LE(40);
  const name = bytes.subarray(NAME_AT, NAME_AT + length);
  if (name.toString('utf8') !== resource) {
    return null;
  }
  try {
    const decipher = createDecipheriv(
      'aes-256-gcm',
      keyFor(resource, secret),
      bytes.subarray(NONCE_AT, NONCE_AT + NONCE_SIZE),
    );
    decipher.setAAD(authenticated(bytes, name));
    decipher.setAuthTag(bytes.subarray(TAG_AT, TAG_AT + TAG_SIZE));
    return Buffer.concat([decipher.update(bytes.subarray(NAME_AT + length)), decipher.final()]);
  } catch {
    return null;
  }
}

// How many bytes sealing adds to a file of `resource`.
function overhead(resource) {
  return NAME_AT + Buffer.byteLength(resource, 'utf8');
}

// The server's seal key, kept in the resources folder so a restarted server seals an unchanged
// file to the same bytes, and its players' download caches (keyed by the sealed file's hash)
// keep it instead of downloading every script again. Made on the first start; a key that cannot
// be read or written leaves this run with a key of its own (every file downloads once more).
const KEY_FILE = 'resource-seal.key';

function serverSealKey(root, log = () => {}) {
  const file = root ? path.join(root, KEY_FILE) : '';
  if (file) {
    try {
      const text = fs.readFileSync(file, 'utf8').trim();
      if (/^[0-9a-f]{64}$/.test(text)) {
        return Buffer.from(text, 'hex');
      }
      log(`${KEY_FILE} is not a 64-digit hex key; a new one replaces it`);
    } catch (error) {
      if (error.code !== 'ENOENT') {
        log(`${KEY_FILE} could not be read (${error.code}); this run seals with a key of its own`);
        return randomBytes(32);
      }
    }
  }
  const key = randomBytes(32);
  if (file) {
    try {
      fs.writeFileSync(file, key.toString('hex') + '\n', { encoding: 'utf8', mode: 0o600 });
    } catch (error) {
      log(`${KEY_FILE} could not be written (${error.code}); this run seals with a key of its own`);
    }
  }
  return key;
}

// The resource's secret from the server's seal key: the same on every run of this server.
function stableSecretFor(serverKey, resource) {
  return createHmac('sha256', serverKey)
    .update('kcds-resource-secret')
    .update(Buffer.from([0]))
    .update(Buffer.from(resource, 'utf8'))
    .digest('base64url');
}

module.exports = { seal, open, secretFor, stableSecretFor, serverSealKey, overhead };
