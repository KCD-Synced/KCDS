'use strict';

// The server's password: KCDS_SERVER_PASSWORD, 1 to 64 bytes of printable text. A
// joining game never sends the password itself: it sends SHA-256, in lowercase hex, of
// "<licence binding>:<password>", where the binding is the server's nonce for that one handshake
// (transport/handshake.hpp, JOIN_IDENTITY). The proof is checked in constant time and is good for
// that connection only. The password is never logged, reported or given to resources.

const crypto = require('node:crypto');

const kMaxPasswordBytes = 64;
const kProofPattern = /^[0-9a-f]{64}$/;
const kBindingPattern = /^[0-9a-f]{16}$/;

function validPassword(value) {
  if (typeof value !== 'string' || value === '' || Buffer.byteLength(value) > kMaxPasswordBytes) {
    return false;
  }
  for (const character of value) {
    const code = character.codePointAt(0);
    if (code < 32 || code === 127) {
      return false;
    }
  }
  return true;
}

// What a game sends for this password and binding.
function passwordProof(binding, password) {
  return crypto.createHash('sha256').update(`${binding}:${password}`, 'utf8').digest('hex');
}

class ServerPassword {
  #password;
  #invalid;

  // value: KCDS_SERVER_PASSWORD. An invalid one lets nobody in (and says so in the log) rather
  // than leaving a server open its owner meant to close.
  constructor(value, log = () => {}) {
    this.#password = '';
    this.#invalid = false;
    if (value === undefined || value === '') {
      return;
    }
    if (!validPassword(value)) {
      this.#invalid = true;
      log('KCDS_SERVER_PASSWORD must be 1 to 64 bytes of printable text: nobody can join');
      return;
    }
    this.#password = value;
    log('joining asks for the server password');
  }

  required() {
    return this.#password !== '' || this.#invalid;
  }

  // Whether a join's proof matches: always with no password set.
  accepts(binding, proof) {
    if (!this.required()) {
      return true;
    }
    if (this.#invalid) {
      return false;
    }
    if (typeof binding !== 'string' || !kBindingPattern.test(binding)) {
      return false;
    }
    if (typeof proof !== 'string' || !kProofPattern.test(proof)) {
      return false;
    }
    const expected = Buffer.from(passwordProof(binding, this.#password), 'hex');
    return crypto.timingSafeEqual(expected, Buffer.from(proof, 'hex'));
  }
}

module.exports = { ServerPassword, passwordProof, validPassword, kMaxPasswordBytes };
