'use strict';

// Sealed client files: every file a resource sends to players is sealed as a KCDS
// asset container (Client/runtime/assets/asset_container.hpp), byte for byte as the game client
// seals and opens one:
//
//   0   8  "KCDSAST1"
//   8   4  format version, 1 (little endian)
//   12  12 the nonce, new for every seal
//   24  16 the AES-GCM tag
//   40  2  length N of the resource's name (little endian)
//   42  N  the resource's name (UTF-8)
//   42+N   the file, encrypted with AES-256-GCM; the authenticated data is bytes 0..11 and the
//          name
//
// The key: SHA-256 of "kcds-asset-v1", 0, the resource's name, 0, the secret. The secret is made
// when the resource starts (secretFor) and goes to each player's game in the resource catalog
// over its session, so the files players keep in their download cache open only with a secret
// from a live session to that server, and a copy of them is noise to the plain game and to other
// mods. What a player's game opens can still be read from its memory while it runs.

const { createCipheriv, createDecipheriv, createHash, randomBytes } = require('node:crypto');

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
  randomBytes(NONCE_SIZE).copy(header, NONCE_AT);
  header.writeUInt16LE(name.length, 40);
  const cipher = createCipheriv(
    'aes-256-gcm',
    keyFor(resource, secret),
    header.subarray(NONCE_AT, TAG_AT),
  );
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

module.exports = { seal, open, secretFor, overhead };
