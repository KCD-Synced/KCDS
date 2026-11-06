'use strict';

// A sealed client file opened in the resource runtime: the server seals every file
// a resource sends as a KCDS asset container (Backend/scripting/runtimes/js/resource_seal.cjs,
// Client/runtime/assets/asset_container.hpp) with the resource's secret, which comes in the
// catalog over this player's session. The download cache keeps the sealed bytes; the plain ones
// live in this runtime's memory only. WebCrypto (crypto.subtle) does the AES-256-GCM.

const MAGIC = 'KCDSAST1';
const VERSION = 1;
const NONCE_AT = 12;
const TAG_AT = 24;
const NAME_AT = 42;
const TAG_SIZE = 16;

const encoder = new TextEncoder();
const decoder = new TextDecoder();

async function keyFor(resource, secret) {
  const material = encoder.encode(`kcds-asset-v1\0${resource}\0${secret}`);
  const digest = await crypto.subtle.digest('SHA-256', material);
  return crypto.subtle.importKey('raw', digest, 'AES-GCM', false, ['decrypt']);
}

// The file inside `sealed` (a Uint8Array), when it is a container of `resource` sealed with
// `secret` and nothing in it was changed; null otherwise.
async function openSealed(sealed, resource, secret) {
  if (!(sealed instanceof Uint8Array) || sealed.length < NAME_AT) {
    return null;
  }
  if (decoder.decode(sealed.subarray(0, 8)) !== MAGIC) {
    return null;
  }
  const view = new DataView(sealed.buffer, sealed.byteOffset, sealed.byteLength);
  if (view.getUint32(8, true) !== VERSION) {
    return null;
  }
  const length = view.getUint16(40, true);
  const name = sealed.subarray(NAME_AT, NAME_AT + length);
  if (decoder.decode(name) !== resource) {
    return null;
  }
  const authenticated = new Uint8Array(NONCE_AT + length);
  authenticated.set(sealed.subarray(0, NONCE_AT), 0);
  authenticated.set(name, NONCE_AT);
  // WebCrypto takes the tag after the encrypted bytes.
  const body = sealed.subarray(NAME_AT + length);
  const withTag = new Uint8Array(body.length + TAG_SIZE);
  withTag.set(body, 0);
  withTag.set(sealed.subarray(TAG_AT, TAG_AT + TAG_SIZE), body.length);
  try {
    const plain = await crypto.subtle.decrypt(
      {
        name: 'AES-GCM',
        iv: sealed.subarray(NONCE_AT, TAG_AT),
        additionalData: authenticated,
        tagLength: TAG_SIZE * 8,
      },
      await keyFor(resource, secret),
      withTag,
    );
    return new Uint8Array(plain);
  } catch {
    return null;
  }
}

module.exports = { openSealed };
