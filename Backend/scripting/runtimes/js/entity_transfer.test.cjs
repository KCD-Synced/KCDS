'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  EntityTransfer,
  LIMITS,
  CHUNK_BYTES,
} = require('../../../../Client/resources/entity_transfer.cjs');
const { ClientEntities } = require('../../../../Client/resources/entities.cjs');
const SESSION = '0123456789abcdef0123456789abcdef';
const NEXT = 'abcdef0123456789abcdef0123456789';

function frames(kind, value, revision = 1, session = SESSION) {
  const bytes = Buffer.from(JSON.stringify(value), 'utf8');
  const identity = { kind, revision, session };
  const result = [{ op: 'entityBegin', ...identity, size: bytes.length }];
  for (let offset = 0; offset < bytes.length; offset += CHUNK_BYTES) {
    result.push({
      op: 'entityChunk',
      ...identity,
      offset,
      data: bytes.subarray(offset, offset + CHUNK_BYTES).toString('base64'),
    });
  }
  result.push({ op: 'entityCommit', ...identity });
  return result;
}

test('large escaped and multibyte state publishes exactly once after complete transfer', () => {
  const applied = [];
  const receiver = new EntityTransfer((kind, value) => applied.push({ kind, value }));
  receiver.reset(SESSION);
  // Every key is valid at 4096 bytes; JSON escaping makes the wire payload >3MB.
  const value = {
    id: 1,
    nativeId: 1,
    entries: Array.from({ length: 128 }, (_, index) => ({
      key: 'key' + index,
      kind: 'string',
      value: '\u0000'.repeat(4096),
    })),
  };
  value.entries[0].value = 'šťastný hráč'.repeat(200);
  const messages = frames('state', value);
  assert.ok(messages[0].size > 3 * 1000 * 1000);
  for (const message of messages.slice(0, -1)) {
    assert.equal(receiver.receive(message), true);
    assert.equal(applied.length, 0);
    assert.ok(Buffer.byteLength(JSON.stringify(message)) < 65536);
  }
  assert.equal(receiver.receive(messages.at(-1)), true);
  assert.deepEqual(applied, [{ kind: 'state', value }]);
  for (const message of messages) assert.equal(receiver.receive(message), false);
  assert.equal(applied.length, 1);
});

test('session reset rejects old messages and clears partial transfers and revision fences', () => {
  const applied = [];
  const receiver = new EntityTransfer((kind, value) => applied.push(value));
  const old = frames('directory', { value: 'old' }, 50);
  assert.equal(receiver.receive(old[0]), false);
  receiver.reset(SESSION);
  receiver.receive(old[0]);
  receiver.receive(old[1]);
  receiver.reset(NEXT);
  assert.equal(receiver.pending.size, 0);
  assert.equal(receiver.receive(old.at(-1)), false);
  for (const message of frames('directory', { value: 'new' }, 1, NEXT)) receiver.receive(message);
  assert.deepEqual(applied, [{ value: 'new' }]);
  receiver.reset(null);
  assert.equal(receiver.receive(old[0]), false);
});

test('limits, gaps, invalid UTF-8 and incomplete commits never publish partial data', () => {
  const receiver = new EntityTransfer(() => assert.fail('Unexpected publication'));
  receiver.reset(SESSION);
  const messages = frames('state', { value: 'ok' });
  assert.throws(() => receiver.receive({ ...messages[0], size: LIMITS.state + 1 }), /size/);
  assert.equal(receiver.pending.size, 0);
  receiver.receive(messages[0]);
  assert.throws(() => receiver.receive({ ...messages[1], offset: 1 }), /chunk/);
  assert.equal(receiver.pending.size, 0);
  receiver.receive(messages[0]);
  assert.throws(() => receiver.receive(messages.at(-1)), /Incomplete/);
  receiver.receive({ ...messages[0], size: 1 });
  receiver.receive({ ...messages[1], data: '/w==' });
  assert.throws(() => receiver.receive(messages.at(-1)), /encoded data/);
  assert.equal(receiver.pending.size, 0);
  assert.throws(() => receiver.receive({ ...messages[0], kind: '__proto__' }), /identity/);
});

test('failed entity validation leaves live directory intact and a corrected revision can retry', () => {
  const entities = new ClientEntities();
  const receiver = new EntityTransfer((kind, value) => {
    if (kind === 'directory') entities.updateDirectory(value);
    else entities.applyState(value);
  });
  receiver.reset(SESSION);
  const directory = {
    localPlayerId: 1,
    entities: [
      {
        id: 1,
        nativeId: 1,
        kind: 'player',
        dimension: 0xffffffff,
        position: { x: 1, y: 2, z: 3 },
        rotation: { w: 1, x: 0, y: 0, z: 0 },
      },
    ],
  };
  for (const message of frames('directory', directory)) receiver.receive(message);
  const handle = entities.facade().LocalPlayer;
  const invalid = frames(
    'directory',
    { ...directory, entities: [{ ...directory.entities[0], position: {} }] },
    2,
  );
  for (const message of invalid.slice(0, -1)) receiver.receive(message);
  assert.throws(() => receiver.receive(invalid.at(-1)), /transform/);
  assert.equal(handle.position.x, 1);
  for (const message of frames('directory', directory, 2)) receiver.receive(message);
  for (const message of frames('state', {
    id: 1,
    nativeId: 1,
    entries: [{ key: 'text', kind: 'string', value: 'čau' }],
  }))
    receiver.receive(message);
  assert.equal(handle.state.get('text'), 'čau');
});

test('newer begin supersedes partial transfer and old chunks cannot complete it', () => {
  const applied = [];
  const receiver = new EntityTransfer((kind, value) => applied.push(value));
  receiver.reset(SESSION);
  const old = frames('state', { value: 1 });
  const fresh = frames('state', { value: 2 }, 2);
  receiver.receive(old[0]);
  receiver.receive(fresh[0]);
  assert.equal(receiver.receive(old[1]), false);
  assert.equal(receiver.receive(old.at(-1)), false);
  receiver.receive(fresh[1]);
  receiver.receive(fresh.at(-1));
  assert.deepEqual(applied, [{ value: 2 }]);
});

test('disconnect inside publication cannot revive old revision state', () => {
  const receiver = new EntityTransfer(() => receiver.reset(NEXT));
  receiver.reset(SESSION);
  for (const message of frames('state', {})) receiver.receive(message);
  assert.equal(receiver.session, NEXT);
  assert.equal(receiver.revisions.size, 0);
  assert.equal(receiver.pending.size, 0);
});

test('a resent chunk already held is ignored and the transfer still completes', () => {
  const published = [];
  const transfer = new EntityTransfer((kind, value) => published.push([kind, value]));
  transfer.reset(SESSION);
  const value = { text: 'x'.repeat(CHUNK_BYTES * 2 + 10) };
  const [begin, first, second, third, commit] = frames('state', value);
  assert.equal(transfer.receive(begin), true);
  assert.equal(transfer.receive(first), true);
  assert.equal(transfer.receive(first), false);
  assert.equal(transfer.receive(second), true);
  assert.equal(transfer.receive(first), false);
  assert.equal(transfer.receive(second), false);
  assert.throws(() => transfer.receive({ ...third, offset: third.offset + 1 }), TypeError);
  assert.equal(transfer.receive(commit), false);
  const again = frames('state', value, 2);
  for (const message of again) {
    assert.equal(transfer.receive(message), true);
  }
  assert.deepEqual(published, [['state', value]]);
});

test('one-message frames take raw bytes, the native { __bytes } form and base64', () => {
  const applied = [];
  const receiver = new EntityTransfer((kind, value) => applied.push(value));
  receiver.reset(SESSION);
  const directory = { localPlayerId: 0, entities: [] };
  const bytes = Buffer.from(JSON.stringify(directory));
  const frame = (revision, data) => ({
    op: 'entityFrame',
    kind: 'directory',
    revision,
    session: SESSION,
    data,
  });
  assert.equal(receiver.receive(frame(1, bytes)), true);
  assert.equal(receiver.receive(frame(2, { __bytes: [...bytes] })), true);
  assert.equal(receiver.receive(frame(3, bytes.toString('base64'))), true);
  assert.equal(receiver.receive(frame(3, bytes)), false);
  assert.throws(() => receiver.receive(frame(4, { __bytes: [256] })), TypeError);
  assert.throws(() => receiver.receive(frame(4, Buffer.alloc(CHUNK_BYTES + 1, 32))), RangeError);
  assert.equal(applied.length, 3);
});

test('a malformed directory delta is refused and leaves the directory as it was', () => {
  const applied = [];
  const receiver = new EntityTransfer((kind, value) => applied.push(value));
  receiver.reset(SESSION);
  // A whole directory is JSON; a delta is the binary frame (marker, JSON length, JSON).
  const send = (revision, value) => {
    let data = Buffer.from(JSON.stringify(value));
    if (value.base !== undefined) {
      const header = Buffer.alloc(5);
      header[0] = 1;
      header.writeUInt32LE(data.length, 1);
      data = Buffer.concat([header, data]);
    }
    return receiver.receive({
      op: 'entityFrame',
      kind: 'directory',
      revision,
      session: SESSION,
      data,
    });
  };
  send(1, { localPlayerId: 0, entities: [{ id: 5 }] });
  assert.throws(
    () => send(2, { base: 1, localPlayerId: 0, upserts: [{}], removed: [] }),
    TypeError,
  );
  assert.throws(
    () => send(2, { base: 1, localPlayerId: 0, upserts: [], removed: [-1] }),
    TypeError,
  );
  assert.equal(receiver.directory.entities.length, 1);
  send(2, { base: 1, localPlayerId: 0, upserts: [{ id: 3 }], removed: [] });
  assert.deepEqual(
    applied.at(-1).entities.map((entry) => entry.id),
    [3, 5],
  );
});

test('the codec carries raw bytes as Bytes', () => {
  const { encode, decode } = require('./codec.cjs');
  const data = Buffer.from([0, 1, 254, 255]);
  const value = decode(encode({ op: 'entityFrame', data }));
  assert.ok(Buffer.isBuffer(value.data));
  assert.deepEqual([...value.data], [0, 1, 254, 255]);
});
