'use strict';

// Bytes for the model code shared by the MCP server (Node) and World Architect in the game
// (open_core.cjs, cgf_writer.cjs): Node's Buffer where there is one, else a Uint8Array with the
// few Buffer methods that code uses (little-endian numbers, latin1 and utf8 text), so the same
// lines run in both.

const LATIN1 = 'latin1';

function latin1Of(bytes, start, end) {
  let text = '';
  const piece = 0x2000;
  for (let at = start; at < end; at += piece) {
    text += String.fromCharCode(...bytes.subarray(at, Math.min(end, at + piece)));
  }
  return text;
}

function utf8Bytes(text) {
  const encoded = unescape(encodeURIComponent(text));
  const out = new Uint8Array(encoded.length);
  for (let at = 0; at < encoded.length; at += 1) {
    out[at] = encoded.charCodeAt(at);
  }
  return out;
}

class Bytes extends Uint8Array {
  static alloc(size) {
    return new Bytes(size);
  }

  static from(value, encoding) {
    if (typeof value === 'string') {
      if (encoding === LATIN1) {
        const out = new Bytes(value.length);
        for (let at = 0; at < value.length; at += 1) {
          out[at] = value.charCodeAt(at) & 0xff;
        }
        return out;
      }
      const encoded = utf8Bytes(value);
      const out = new Bytes(encoded.length);
      out.set(encoded);
      return out;
    }
    const out = new Bytes(value.length);
    out.set(value);
    return out;
  }

  static concat(list) {
    const size = list.reduce((sum, part) => sum + part.length, 0);
    const out = new Bytes(size);
    let at = 0;
    for (const part of list) {
      out.set(part, at);
      at += part.length;
    }
    return out;
  }

  view() {
    return new DataView(this.buffer, this.byteOffset, this.byteLength);
  }

  readUInt32LE(at) {
    return this.view().getUint32(at, true);
  }

  readInt32LE(at) {
    return this.view().getInt32(at, true);
  }

  readUInt16LE(at) {
    return this.view().getUint16(at, true);
  }

  readFloatLE(at) {
    return this.view().getFloat32(at, true);
  }

  writeUInt32LE(value, at) {
    this.view().setUint32(at, value >>> 0, true);
    return at + 4;
  }

  writeInt32LE(value, at) {
    this.view().setInt32(at, value, true);
    return at + 4;
  }

  writeUInt16LE(value, at) {
    this.view().setUint16(at, value, true);
    return at + 2;
  }

  writeInt16LE(value, at) {
    this.view().setInt16(at, value, true);
    return at + 2;
  }

  writeFloatLE(value, at) {
    this.view().setFloat32(at, value, true);
    return at + 4;
  }

  // Text written at `offset` (latin1: a byte a character), cut at the end of the bytes.
  write(text, offset = 0, encoding = 'utf8') {
    const encoded = encoding === LATIN1 ? Bytes.from(text, LATIN1) : utf8Bytes(text);
    const count = Math.min(encoded.length, this.length - offset);
    this.set(encoded.subarray(0, count), offset);
    return count;
  }

  toString(encoding = 'utf8', start = 0, end = this.length) {
    const text = latin1Of(this, start, Math.min(end, this.length));
    if (encoding === LATIN1) {
      return text;
    }
    return decodeURIComponent(escape(text));
  }

  copy(target, targetStart = 0, start = 0, end = this.length) {
    const part = this.subarray(start, end);
    target.set(part, targetStart);
    return part.length;
  }
}

// Node's Buffer when it is there (the MCP server), the stand-in otherwise (the game's editor).
const NodeBuffer = typeof Buffer === 'function' ? Buffer : null;

module.exports = { Bytes: NodeBuffer ?? Bytes, ShimBytes: Bytes };
