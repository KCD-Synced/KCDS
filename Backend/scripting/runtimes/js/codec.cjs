'use strict';
// Existing sdk::Value tagged marshal format; IPC uses hex lines, no engine pointers.
function encode(value) {
  const out = [];
  const byte = (n) => out.push(Number(n) & 255);
  const uint = (n) => {
    n = BigInt(n);
    do {
      const b = n & 127n;
      n >>= 7n;
      byte(b | (n ? 128n : 0n));
    } while (n);
  };
  const str = (s) => {
    const b = Buffer.from(s, 'utf8');
    uint(b.length);
    out.push(...b);
  };
  function put(v, depth = 0) {
    if (depth > 64) throw new RangeError('Marshal nesting');
    if (v == null) byte(0);
    else if (typeof v === 'boolean') {
      byte(1);
      byte(v ? 1 : 0);
    } else if (typeof v === 'number') {
      if (!Number.isFinite(v)) throw new TypeError('Non-finite');
      byte(3);
      const b = Buffer.alloc(8);
      b.writeDoubleLE(v);
      out.push(...b);
    } else if (typeof v === 'string') {
      byte(4);
      str(v);
    } else if (v instanceof Uint8Array) {
      // Raw bytes (a Buffer): the Bytes tag, so chunk data needs no text encoding.
      byte(5);
      uint(v.length);
      for (const b of v) out.push(b);
    } else if (Array.isArray(v)) {
      byte(6);
      uint(v.length);
      for (const e of v) put(e, depth + 1);
    } else if (typeof v === 'object') {
      const entries = Object.entries(v);
      byte(7);
      uint(entries.length);
      for (const [k, x] of entries) {
        str(k);
        put(x, depth + 1);
      }
    } else throw new TypeError('Unsupported marshal value');
  }
  put(value);
  return Buffer.from(out);
}
function decode(buffer) {
  let p = 0;
  const take = (n) => {
    if (n < 0 || p + n > buffer.length) throw new RangeError('Truncated marshal');
    const b = buffer.subarray(p, p + n);
    p += n;
    return b;
  };
  const byte = () => take(1)[0];
  const uint = () => {
    let n = 0n;
    for (let s = 0; s < 70; s += 7) {
      const b = byte();
      n |= BigInt(b & 127) << BigInt(s);
      if (!(b & 128)) {
        if (n > BigInt(Number.MAX_SAFE_INTEGER)) throw new RangeError('Marshal integer overflow');
        return Number(n);
      }
    }
    throw new RangeError('Marshal varint');
  };
  const str = () => take(uint()).toString('utf8');
  function get(depth = 0) {
    if (depth > 64) throw new RangeError('Marshal nesting');
    switch (byte()) {
      case 0:
        return null;
      case 1:
        return byte() !== 0;
      case 2: {
        const z = uint();
        return z % 2 ? -(z + 1) / 2 : z / 2;
      }
      case 3: {
        const n = take(8).readDoubleLE();
        if (!Number.isFinite(n)) throw new TypeError('Non-finite');
        return n;
      }
      case 4:
        return str();
      case 5:
        return take(uint());
      case 6: {
        const n = uint();
        if (n > buffer.length - p) throw new RangeError('Array size');
        return Array.from({ length: n }, () => get(depth + 1));
      }
      case 7: {
        const n = uint();
        if (n > buffer.length - p) throw new RangeError('Map size');
        const m = Object.create(null);
        for (let i = 0; i < n; i++) {
          const k = str();
          m[k] = get(depth + 1);
        }
        return m;
      }
      case 8:
        return { entityId: uint() };
      default:
        throw new TypeError('Marshal tag');
    }
  }
  const result = get();
  if (p !== buffer.length) throw new RangeError('Trailing marshal');
  return result;
}
module.exports = { encode, decode };
