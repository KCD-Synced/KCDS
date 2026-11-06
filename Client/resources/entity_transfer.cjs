'use strict';

// A shared state can exceed one KSTP message. Assemble bounded UTF-8 bytes before
// publishing; splitting by characters would break multibyte text and byte limits.
//
// A frame of up to CHUNK_BYTES comes as one entityFrame message; a larger one as entityBegin,
// entityChunk parts and entityCommit. Chunk data arrives as raw bytes: a Uint8Array, or the
// { __bytes: [...] } form the native runtime posts them in (base64 text is still taken).
//
// A directory frame is either a whole directory ({ localPlayerId, entities }) or a delta
// against the directory this client last committed ({ base: its revision, localPlayerId,
// upserts: [new entries], patches: [{ id, set: { changed fields }, unset: [fields] }],
// removed: [ids] }). A delta whose base is not that directory
// throws ResyncNeeded: the client acknowledges it with resync, and the sender sends the whole
// directory again. A delta frame is binary: DELTA_MARKER, the u32 length of its JSON part, the
// JSON part, then motion records for the entries whose only change is how they move (u32 id,
// u8 mask: 1 position, 2 rotation, 4 velocity, 8 look direction; then each named field's
// components as float32s: position x, y, z, rotation w, x, y, z, velocity x, y, z, look
// direction x, y, z).
const LIMITS = Object.freeze({ directory: 1024 * 1024, state: 4 * 1024 * 1024 });
const CHUNK_BYTES = 12288;
const MAX_DIRECTORY_ENTITIES = 4096;
const DELTA_MARKER = 1;
const MOTION_LAYOUT = [
  ['position', ['x', 'y', 'z']],
  ['rotation', ['w', 'x', 'y', 'z']],
  ['velocity', ['x', 'y', 'z']],
  ['gaze', ['x', 'y', 'z']],
];

class ResyncNeeded extends Error {
  constructor() {
    super('Entity directory delta does not match the committed directory');
  }
}

// The bytes of a chunk's data; throws for anything else.
function chunkBytes(data) {
  if (data instanceof Uint8Array) {
    return data;
  }
  if (data && typeof data === 'object' && Array.isArray(data.__bytes)) {
    if (data.__bytes.length > CHUNK_BYTES) {
      throw new TypeError('Invalid entity transfer chunk');
    }
    return Uint8Array.from(data.__bytes, (byte) => {
      if (!Number.isInteger(byte) || byte < 0 || byte > 255) {
        throw new TypeError('Invalid entity transfer chunk');
      }
      return byte;
    });
  }
  if (
    typeof data !== 'string' ||
    data.length > (CHUNK_BYTES * 4) / 3 ||
    !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(data)
  ) {
    throw new TypeError('Invalid entity transfer chunk');
  }
  return Uint8Array.from(atob(data), (character) => character.charCodeAt(0));
}

function validId(id) {
  return Number.isInteger(id) && id > 0 && id <= 0xffffffff;
}

// [the delta's JSON value, its motion records] of a binary delta frame.
function decodeDelta(bytes) {
  const invalid = () => new TypeError('Invalid entity directory delta');
  if (bytes.length < 5) {
    throw invalid();
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const jsonLength = view.getUint32(1, true);
  if (5 + jsonLength > bytes.length) {
    throw invalid();
  }
  const text = new TextDecoder('utf-8', { fatal: true }).decode(bytes.subarray(5, 5 + jsonLength));
  const value = JSON.parse(text);
  const moves = [];
  let at = 5 + jsonLength;
  const float = () => {
    if (at + 4 > bytes.length) {
      throw invalid();
    }
    const number = view.getFloat32(at, true);
    at += 4;
    if (!Number.isFinite(number)) {
      throw invalid();
    }
    return number;
  };
  while (at < bytes.length) {
    if (at + 5 > bytes.length || moves.length >= MAX_DIRECTORY_ENTITIES) {
      throw invalid();
    }
    const id = view.getUint32(at, true);
    const mask = bytes[at + 4];
    at += 5;
    if (!validId(id) || mask < 1 || mask >= 1 << MOTION_LAYOUT.length) {
      throw invalid();
    }
    const fields = {};
    MOTION_LAYOUT.forEach(([field, components], bit) => {
      if (!(mask & (1 << bit))) return;
      fields[field] = Object.fromEntries(components.map((key) => [key, float()]));
    });
    moves.push({ id, fields });
  }
  return [value, moves];
}

function validPatch(patch) {
  return (
    patch !== null &&
    typeof patch === 'object' &&
    validId(patch.id) &&
    patch.set !== null &&
    typeof patch.set === 'object' &&
    !Array.isArray(patch.set) &&
    (patch.unset === undefined ||
      (Array.isArray(patch.unset) && patch.unset.every((key) => typeof key === 'string')))
  );
}

class EntityTransfer {
  constructor(apply) {
    this.apply = apply;
    this.reset(null);
  }
  reset(session) {
    if (session !== null && (typeof session !== 'string' || !/^[a-f0-9]{32}$/.test(session))) {
      throw new TypeError('Invalid entity session');
    }
    this.session = session;
    this.pending = new Map();
    this.revisions = new Map();
    // The last committed whole directory, which a delta applies to.
    this.directory = null;
  }
  receive(message) {
    if (!this.session || message.session !== this.session) return false;
    const { kind, revision } = message;
    if (!Object.hasOwn(LIMITS, kind) || !Number.isSafeInteger(revision) || revision < 1) {
      throw new TypeError('Invalid entity transfer identity');
    }
    if (revision <= (this.revisions.get(kind) ?? 0)) return false;
    if (message.op === 'entityFrame') {
      const bytes = chunkBytes(message.data);
      if (!bytes.length || bytes.length > CHUNK_BYTES) {
        throw new RangeError('Invalid entity frame size');
      }
      this.pending.delete(kind);
      return this.commit(kind, revision, bytes);
    }
    if (message.op === 'entityBegin') {
      if (!Number.isSafeInteger(message.size) || message.size < 1 || message.size > LIMITS[kind]) {
        throw new RangeError('Invalid entity transfer size');
      }
      const previous = this.pending.get(kind);
      if (previous && revision <= previous.revision) return false;
      this.pending.set(kind, { revision, bytes: new Uint8Array(message.size), offset: 0 });
      return true;
    }
    const transfer = this.pending.get(kind);
    if (!transfer || transfer.revision !== revision) return false;
    try {
      if (message.op === 'entityChunk') {
        if (
          Number.isSafeInteger(message.offset) &&
          message.offset >= 0 &&
          message.offset < transfer.offset
        ) {
          // The sender resends a frame whose acknowledgment was late: already held.
          return false;
        }
        if (message.offset !== transfer.offset) {
          throw new TypeError('Invalid entity transfer chunk');
        }
        const bytes = chunkBytes(message.data);
        if (!bytes.length || transfer.offset + bytes.length > transfer.bytes.length) {
          throw new RangeError('Entity transfer chunk exceeds declared size');
        }
        transfer.bytes.set(bytes, transfer.offset);
        transfer.offset += bytes.length;
        return true;
      }
      if (message.op !== 'entityCommit' || transfer.offset !== transfer.bytes.length) {
        throw new TypeError('Incomplete entity transfer');
      }
      this.pending.delete(kind);
      return this.commit(kind, revision, transfer.bytes);
    } catch (error) {
      this.pending.delete(kind);
      throw error;
    }
  }
  // Decodes a whole frame and applies it; a directory delta becomes the whole directory first.
  commit(kind, revision, bytes) {
    let value;
    if (kind === 'directory' && bytes[0] === DELTA_MARKER) {
      value = this.wholeDirectory(...decodeDelta(bytes));
    } else {
      value = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes));
    }
    const session = this.session;
    this.apply(kind, value);
    if (this.session !== session) return false;
    this.revisions.set(kind, revision);
    if (kind === 'directory') {
      this.directory = value;
    }
    return true;
  }
  wholeDirectory(value, moves) {
    if (!value || typeof value !== 'object' || !Number.isSafeInteger(value.base)) {
      throw new TypeError('Invalid entity directory delta');
    }
    if (!this.directory || value.base !== (this.revisions.get('directory') ?? 0)) {
      throw new ResyncNeeded();
    }
    const { localPlayerId, upserts, patches = [], removed } = value;
    if (
      !Array.isArray(upserts) ||
      !Array.isArray(patches) ||
      !Array.isArray(removed) ||
      upserts.length > MAX_DIRECTORY_ENTITIES ||
      patches.length > MAX_DIRECTORY_ENTITIES ||
      removed.length > MAX_DIRECTORY_ENTITIES ||
      !removed.every(validId) ||
      !upserts.every((entry) => entry && typeof entry === 'object' && validId(entry.id)) ||
      !patches.every(validPatch)
    ) {
      throw new TypeError('Invalid entity directory delta');
    }
    const entities = new Map(this.directory.entities.map((entry) => [entry.id, entry]));
    for (const id of removed) {
      entities.delete(id);
    }
    for (const entry of upserts) {
      entities.set(entry.id, entry);
    }
    for (const { id, set, unset = [] } of patches) {
      const entry = entities.get(id);
      if (!entry) {
        throw new ResyncNeeded();
      }
      const patched = { ...entry, ...set };
      for (const key of unset) {
        delete patched[key];
      }
      patched.id = id;
      entities.set(id, patched);
    }
    for (const { id, fields } of moves) {
      const entry = entities.get(id);
      if (!entry) {
        throw new ResyncNeeded();
      }
      entities.set(id, { ...entry, ...fields });
    }
    return {
      localPlayerId,
      entities: [...entities.values()].sort((left, right) => left.id - right.id),
    };
  }
}

module.exports = { EntityTransfer, ResyncNeeded, LIMITS, CHUNK_BYTES };
