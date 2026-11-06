'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { EntitySender } = require('./entity_sender.cjs');
const { EntityStateStore } = require('./shared_state.cjs');
const {
  EntityTransfer,
  ResyncNeeded,
  CHUNK_BYTES,
} = require('../../../../Client/resources/entity_transfer.cjs');
const { ClientEntities } = require('../../../../Client/resources/entities.cjs');
const SESSION = '0123456789abcdef0123456789abcdef';

function view(owner = true, x = 1) {
  return {
    localPlayerId: 1,
    entities: [
      {
        id: 1,
        nativeId: 1,
        kind: 'player',
        isOwner: owner,
        dimension: 0xffffffff,
        position: { x, y: 2, z: 3 },
        rotation: { w: 1, x: 0, y: 0, z: 0 },
      },
    ],
  };
}

function fixture() {
  const store = new EntityStateStore();
  store.attach(1);
  const messages = [];
  const entities = new ClientEntities();
  const receiver = new EntityTransfer((kind, value) => {
    if (kind === 'directory') entities.updateDirectory(value);
    else entities.applyState(value);
  });
  const sender = new EntitySender({
    store,
    session: () => SESSION,
    send: (peer, message) => messages.push({ peer, message }),
  });
  let time = 0;
  function step(ack = true) {
    sender.pump(time);
    time += 0.03;
    while (messages.length) {
      const { peer, message } = messages.shift();
      assert.ok(Buffer.byteLength(JSON.stringify(message)) < 65536);
      if (message.op === 'entitySession') receiver.reset(message.session);
      else receiver.receive(message);
      if (ack) sender.acknowledge(peer, { session: message.session, delivery: message.delivery });
    }
  }
  function drain(count = 1000) {
    for (let index = 0; index < count; ++index) step();
  }
  return { store, sender, entities, receiver, messages, step, drain };
}

test('end-to-end sender projects scopes and owner transfer removes private keys', () => {
  const f = fixture();
  const bag = f.store.bag(1);
  bag.set('public', 'čau');
  bag.set('owner', 42, { scope: 'owner' });
  bag.set('secret', 'hidden', { scope: 'server' });
  f.sender.updateView(1, view());
  f.drain(20);
  const state = f.entities.facade().LocalPlayer.state;
  assert.deepEqual(state.keys(), ['owner', 'public']);
  f.sender.updateView(1, view(false));
  f.drain(20);
  assert.deepEqual(state.keys(), ['public']);
  assert.equal(state.get('public'), 'čau');
  bag.remove('public');
  f.drain(20);
  assert.deepEqual(state.keys(), []);
});

// a client whose resource runtime started anew holds nothing; once the new runtime asks
// for the catalog the peer gets a new session, its whole directory and every state again.
test('a restarted client runtime gets the directory and state again', () => {
  const f = fixture();
  f.store.bag(1).set('public', 'one');
  assert.equal(f.sender.restartPeer(1), false);
  f.sender.updateView(1, view());
  f.drain(20);
  assert.equal(f.entities.facade().LocalPlayer.state.get('public'), 'one');
  // The new runtime's own receiver: it drops what belongs to a session it does not hold.
  const entities = new ClientEntities();
  const receiver = new EntityTransfer((kind, value) => {
    if (kind === 'directory') entities.updateDirectory(value);
    else entities.applyState(value);
  });
  f.receiver.reset = (session) => receiver.reset(session);
  f.receiver.receive = (message) => receiver.receive(message);
  assert.equal(f.sender.restartPeer(1), true);
  f.drain(20);
  assert.equal(entities.facade().LocalPlayer.state.get('public'), 'one');
  assert.equal(f.sender.restartPeer(2), false);
});

test('a maximum escaped state crosses hundreds of frames with bounded outstanding work', () => {
  const f = fixture();
  const bag = f.store.bag(1);
  for (let index = 0; index < 128; ++index) bag.set('key' + index, '\u0000'.repeat(4096));
  f.sender.updateView(1, view());
  f.drain();
  assert.equal(f.entities.facade().LocalPlayer.state.keys().length, 128);
  assert.equal(f.entities.facade().LocalPlayer.state.get('key127').length, 4096);
  assert.equal(f.sender.peers.get(1).pending, null);
  assert.equal(f.sender.peers.get(1).active, null);
});

test('lost acknowledgments retry the identical frame; forged and stale credits cannot advance', () => {
  const f = fixture();
  f.sender.updateView(1, view());
  f.sender.pump(0);
  const first = f.messages.shift().message;
  assert.equal(f.sender.acknowledge(1, { session: SESSION, delivery: 999 }), false);
  assert.equal(f.sender.acknowledge(2, { session: SESSION, delivery: first.delivery }), false);
  f.sender.pump(0.5);
  assert.equal(f.messages.length, 0);
  f.sender.pump(1);
  assert.deepEqual(f.messages.shift().message, first);
  assert.equal(f.sender.acknowledge(1, { session: SESSION, delivery: first.delivery }), true);
  assert.equal(f.sender.acknowledge(1, { session: SESSION, delivery: first.delivery }), false);
  f.sender.disconnect(1);
  assert.equal(f.sender.peers.size, 0);
  assert.equal(f.sender.acknowledge(1, { session: SESSION, delivery: first.delivery }), false);
});

test('view narrowing cancels unfinished owner state before commit and invalidates stale handles', () => {
  const f = fixture();
  // Larger than one chunk, so the state frame is still in flight when the view narrows.
  for (let key = 0; key < 4; ++key) {
    f.store.bag(1).set(`private${key}`, 'v'.repeat(4000), { scope: 'owner' });
  }
  f.sender.updateView(1, view());
  for (let count = 0; count < 2; ++count) f.step();
  const player = f.entities.facade().LocalPlayer;
  f.step();
  assert.equal(f.sender.peers.get(1).active.kind, 'state');
  f.sender.updateView(1, { localPlayerId: 0, entities: [] });
  f.drain(20);
  assert.equal(f.entities.facade().LocalPlayer, null);
  assert.throws(() => player.state, /no longer/);
});

test('moving directory updates cannot starve state publication and unchanged snapshots send nothing', () => {
  const f = fixture();
  f.store.bag(1).set('score', 1);
  f.sender.updateView(1, view());
  for (let index = 0; index < 20; ++index) {
    f.sender.updateView(1, view(true, index));
    f.step();
  }
  assert.equal(f.entities.facade().LocalPlayer.state.get('score'), 1);
  f.drain(20);
  const deliveries = f.sender.peers.get(1).delivery;
  f.drain(20);
  assert.equal(f.sender.peers.get(1).delivery, deliveries);
});

test('malformed native views do not replace previous authoritative views', () => {
  const f = fixture();
  f.sender.updateView(1, view());
  assert.throws(
    () =>
      f.sender.updateView(1, { ...view(), entities: [{ ...view().entities[0], isOwner: null }] }),
    /ownership/,
  );
  assert.throws(() => f.sender.updateView(1, { ...view(), localPlayerId: 2 }), /Local player/);
  assert.equal(f.sender.peers.get(1).owners.get(1), true);
  f.drain(20);
  assert.equal(f.entities.facade().LocalPlayer.position.x, 1);
});

test('cached unchanged state skips encoding but scope-only mutations invalidate projection', () => {
  const f = fixture();
  const bag = f.store.bag(1);
  bag.set('value', 10);
  f.sender.updateView(1, view(false));
  f.drain(20);
  let projections = 0;
  const project = f.store.project.bind(f.store);
  f.store.project = (...args) => {
    projections++;
    return project(...args);
  };
  f.drain(20);
  assert.equal(projections, 0);
  bag.set('value', 10, { scope: 'owner' });
  f.drain(20);
  assert.equal(projections, 1);
  assert.equal(f.entities.facade().LocalPlayer.state.has('value'), false);
});

test('native multipart view publishes atomically and bad offsets preserve the previous view', () => {
  const f = fixture();
  const first = view();
  const second = { ...first.entities[0], id: 2, nativeId: 2, isOwner: false };
  const identity = { peer: 1, revision: 1, total: 2, localPlayerId: 1 };
  assert.equal(
    f.sender.receiveViewPart({ ...identity, offset: 0, entities: first.entities }),
    false,
  );
  assert.equal(f.sender.peers.size, 0);
  assert.equal(f.sender.receiveViewPart({ ...identity, offset: 1, entities: [second] }), true);
  assert.equal(f.sender.peers.get(1).directory.entities.length, 2);
  f.sender.receiveViewPart({ ...identity, revision: 2, offset: 0, entities: first.entities });
  assert.throws(
    () => f.sender.receiveViewPart({ ...identity, revision: 2, offset: 2, entities: [] }),
    /sequence/,
  );
  assert.equal(f.sender.peers.get(1).directory.entities.length, 2);
  f.sender.disconnect(1);
  assert.equal(f.sender.views.size, 0);
});

test('malformed acknowledgment never throws or creates credit on an idle peer', () => {
  const f = fixture();
  f.sender.updateView(1, view());
  assert.equal(f.sender.acknowledge(1, { session: SESSION }), false);
  assert.equal(f.sender.acknowledge(1, { session: SESSION, delivery: 0 }), false);
  f.sender.pump(0);
  assert.equal(f.sender.acknowledge(1, { session: SESSION }), false);
  assert.equal(f.sender.peers.get(1).ready, false);
});

test('aggregate budget bounds bursts, counts retries and fairly services late peers', () => {
  const store = new EntityStateStore();
  store.attach(1);
  const sent = [];
  const sender = new EntitySender({
    store,
    session: () => SESSION,
    send: (peer, message) => sent.push({ peer, message }),
    bytesPerSecond: 1000,
    burstBytes: CHUNK_BYTES * 2,
    framesPerPump: 1,
  });
  for (let peer = 1; peer <= 3; ++peer) sender.updateView(peer, view());
  sender.pump(0);
  sender.pump(0);
  sender.pump(0);
  assert.deepEqual(
    sent.map((entry) => entry.peer),
    [1, 2, 3],
  );
  const initial = sent.reduce(
    (sum, entry) => sum + Buffer.byteLength(JSON.stringify(entry.message)),
    0,
  );
  assert.equal(sender.credit, sender.burstBytes - initial);
  sender.credit = 0;
  sender.pump(1);
  assert.equal(sent.length, 4);
  assert.deepEqual(sent[3], sent[0]);
  assert.equal(sender.credit, 1000 - Buffer.byteLength(JSON.stringify(sent[3].message)));
  sender.credit = 0;
  sender.pump(1);
  assert.equal(sent.length, 4);
  assert.throws(() => sender.pump(0.9), /backwards/);
});

test('no aggregate credit leaves delivery unchanged until refill', () => {
  const f = fixture();
  f.sender.updateView(1, view());
  f.sender.credit = 0;
  f.sender.pump(0);
  assert.equal(f.sender.peers.get(1).delivery, 0);
  assert.equal(f.sender.peers.get(1).pending, null);
  assert.equal(f.messages.length, 0);
  f.sender.pump(0.1);
  assert.equal(f.sender.peers.get(1).delivery, 1);
  assert.equal(f.messages.length, 1);
  assert.throws(() => new EntitySender({ store: f.store, send() {}, burstBytes: 1 }), /budget/);
});

test('a larger frame reserves its turn instead of starving behind smaller frames', () => {
  const f = fixture();
  f.sender.updateView(1, view());
  f.sender.updateView(2, view());
  f.sender.peers.get(1).pending = {
    message: { op: 'retry-test', data: 'x'.repeat(500) },
    sentAt: -2,
  };
  f.sender.credit = 200;
  f.sender.pump(0);
  assert.equal(f.messages.length, 0);
  assert.equal(f.sender.peerCursor, 0);
  f.sender.pump(0.001);
  assert.deepEqual(
    f.messages.map((entry) => entry.peer),
    [1, 2],
  );
});

test('an entity that leaves and re-enters the view receives its state again', () => {
  const f = fixture();
  f.store.attach(2);
  f.store.bag(2).set('public', 'returning');
  f.store.bag(2).set('owner', 'never', { scope: 'owner' });
  const other = {
    id: 2,
    nativeId: 2,
    kind: 'player',
    isOwner: false,
    dimension: 0xffffffff,
    position: { x: 5, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  };
  const both = () => ({ ...view(), entities: [...view().entities, other] });
  f.sender.updateView(1, both());
  f.drain(40);
  const { Entity } = f.entities.facade();
  assert.equal(new Entity(2).state.get('public'), 'returning');

  // Leaving the view removes the record and its state on the client.
  f.sender.updateView(1, view());
  f.drain(40);
  assert.throws(() => new Entity(2), /Unknown entity/);

  // Re-entering must deliver the directory first and then the state again.
  f.sender.updateView(1, both());
  f.drain(40);
  assert.equal(new Entity(2).state.get('public'), 'returning');
  assert.equal(new Entity(2).state.has('owner'), false);
});

test('state for an entity is never sent before the client holds it in its directory', () => {
  const f = fixture();
  f.store.attach(2);
  f.store.bag(2).set('public', 'late');
  f.sender.updateView(1, view());
  f.drain(40);
  const sent = [];
  const original = f.sender.send;
  f.sender.send = (peer, message) => {
    sent.push(message);
    original(peer, message);
  };
  f.sender.updateView(1, {
    ...view(),
    entities: [
      ...view().entities,
      {
        id: 2,
        nativeId: 2,
        kind: 'entity',
        isOwner: false,
        dimension: 0xffffffff,
        position: { x: 5, y: 2, z: 3 },
        rotation: { w: 1, x: 0, y: 0, z: 0 },
      },
    ],
  });
  f.drain(40);
  const begins = sent.filter(
    (message) => message.op === 'entityBegin' || message.op === 'entityFrame',
  );
  assert.deepEqual(
    begins.map((message) => message.kind),
    ['directory', 'state'],
  );
  assert.equal(new (f.entities.facade().Entity)(2).state.get('public'), 'late');
});

// Native changed-state deltas: shared entity records plus per-peer view changes.
function record(nativeId, x = 1, kind = 'player') {
  return {
    id: nativeId,
    nativeId,
    kind,
    dimension: 0xffffffff,
    position: { x, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  };
}

function delta(peer, added, removed = [], owned = [], localPlayerId = peer) {
  return { peer, localPlayerId, added, removed, owned, final: true };
}

test('delta views publish the directory and state like whole views', () => {
  const f = fixture();
  f.store.bag(1).set('public', 'delta');
  f.store.bag(1).set('owner', 7, { scope: 'owner' });
  f.sender.applyRecords({ entities: [record(1)] });
  assert.equal(f.sender.applyViewDelta(delta(1, [1], [], [1], 1)), true);
  f.drain(20);
  const state = f.entities.facade().LocalPlayer.state;
  assert.deepEqual(state.keys(), ['owner', 'public']);
  // Ownership moves without a membership change: the owner key is withdrawn.
  f.sender.applyViewDelta(delta(1, [], [], [], 1));
  f.drain(20);
  assert.deepEqual(state.keys(), ['public']);
});

test('moving records refresh the directory lazily and an idle sender sends nothing', () => {
  const f = fixture();
  f.sender.applyRecords({ entities: [record(1)] });
  f.sender.applyViewDelta(delta(1, [1], [], [1], 1));
  f.drain(20);
  const deliveries = f.sender.peers.get(1).delivery;
  f.drain(20);
  assert.equal(f.sender.peers.get(1).delivery, deliveries);
  f.sender.applyRecords({ entities: [record(1, 42)] });
  f.drain(20);
  assert.equal(f.entities.facade().LocalPlayer.position.x, 42);
});

test('a player record carries its stats into the client directory', () => {
  const f = fixture();
  const vitals = {
    health: 70,
    stamina: 50,
    exhaust: 60,
    hunger: 40,
    bleeding: 0,
    sleeping: 0,
    consciousness: 1,
    drunkenness: 0,
    poisoning: 0,
  };
  f.sender.applyRecords({ entities: [{ ...record(1), vitals, scaledStats: null }] });
  f.sender.applyViewDelta(delta(1, [1], [], [1], 1));
  f.drain(20);
  const local = f.entities.facade().LocalPlayer;
  assert.equal(local.getStat('hunger'), 40);
  assert.deepEqual(local.scaledStats, { strength: 0, agility: 0, vitality: 0 });
  f.sender.applyRecords({ entities: [{ ...record(1), vitals: { ...vitals, hunger: 39.5 } }] });
  f.drain(20);
  assert.equal(local.getStat('hunger'), 39.5);
});

test('an idle peer does not rescan shared state while nothing changed', () => {
  const f = fixture();
  f.store.attach(2);
  f.sender.applyRecords({ entities: [record(1), record(2, 5, 'entity')] });
  f.sender.applyViewDelta(delta(1, [1, 2], [], [1], 1));
  f.drain(40);
  const cursor = f.sender.peers.get(1).stateCursor;
  f.drain(40);
  assert.equal(f.sender.peers.get(1).stateCursor, cursor);
  f.store.bag(2).set('late', true);
  f.drain(40);
  assert.equal(new (f.entities.facade().Entity)(2).state.get('late'), true);
});

test('views stay scoped per peer and departures release shared records', () => {
  const f = fixture();
  f.store.attach(2);
  f.store.bag(2).set('private', 'peer-one-only');
  f.sender.applyRecords({ entities: [record(1), record(2), record(3, 9, 'entity')] });
  f.sender.applyViewDelta(delta(1, [1, 3], [], [1], 1));
  f.sender.applyViewDelta(delta(2, [2], [], [2], 2));
  assert.deepEqual(
    f.sender.peers.get(1).directory.entities.map((entity) => entity.id),
    [1, 3],
  );
  assert.deepEqual(
    f.sender.peers.get(2).directory.entities.map((entity) => entity.id),
    [2],
  );
  assert.throws(() => f.sender.removeRecords({ ids: [3] }), /still in a view/);
  f.sender.applyViewDelta(delta(1, [], [3], [1], 1));
  f.sender.removeRecords({ ids: [3] });
  assert.equal(f.sender.records.has(3), false);
  f.sender.disconnect(2);
  assert.equal(f.sender.records.get(2).viewers, 0);
  f.sender.removeRecords({ ids: [2] });
  assert.equal(f.sender.records.size, 1);
});

test('malformed deltas are rejected before they change the view', () => {
  const f = fixture();
  f.sender.applyRecords({ entities: [record(1), record(2, 5, 'entity')] });
  f.sender.applyViewDelta(delta(1, [1], [], [1], 1));
  const before = f.sender.peers.get(1).directoryBytes;
  assert.throws(() => f.sender.applyViewDelta(delta(1, [9])), /unknown entity/);
  assert.throws(() => f.sender.applyViewDelta(delta(1, [], [2])), /unknown member/);
  assert.throws(() => f.sender.applyViewDelta(delta(1, [1])), /twice/);
  assert.throws(() => f.sender.applyViewDelta(delta(1, [], [], [2], 1)), /non-member/);
  assert.throws(() => f.sender.applyViewDelta(delta(1, [2], [1], [], 1)), /Local player/);
  assert.throws(() => f.sender.applyViewDelta(delta(1, [], [], [], 2)), /Local player/);
  assert.throws(
    () => f.sender.applyRecords({ entities: [{ ...record(3), kind: 'npc' }] }),
    /entry/,
  );
  assert.throws(
    () => f.sender.applyRecords({ entities: [{ ...record(4), id: 1 }] }),
    /already in use/,
  );
  assert.equal(f.sender.peers.get(1).directoryBytes, before);
  assert.deepEqual([...f.sender.peers.get(1).members], [[1, true]]);
  assert.equal(f.sender.records.get(2).viewers, 0);
});

test('a delta split into parts publishes only on its final part', () => {
  const f = fixture();
  f.sender.applyRecords({ entities: [record(1), record(2, 5, 'entity')] });
  assert.equal(
    f.sender.applyViewDelta({ peer: 1, localPlayerId: 1, added: [1], removed: [], final: false }),
    false,
  );
  assert.equal(f.sender.peers.get(1).directoryBytes, null);
  assert.equal(f.sender.applyViewDelta(delta(1, [2], [], [1], 1)), true);
  assert.deepEqual(
    f.sender.peers.get(1).directory.entities.map((entity) => entity.id),
    [1, 2],
  );
  assert.equal(f.sender.peers.get(1).owners.get(1), true);
  assert.equal(f.sender.peers.get(1).owners.get(2), false);
});

test('the peer cap follows the native connection limit with bounded budgets', () => {
  const store = new EntityStateStore();
  const sender = new EntitySender({ store, send() {} });
  assert.equal(sender.maxPeers, 32);
  assert.equal(sender.bytesPerSecond, 1024 * 1024);
  assert.equal(sender.framesPerPump, 64);
  for (let peer = 1; peer <= 32; ++peer) {
    store.attach(peer);
    sender.applyRecords({ entities: [record(peer)] });
    sender.applyViewDelta(delta(peer, [peer], [], [peer]));
  }
  store.attach(33);
  sender.applyRecords({ entities: [record(33)] });
  assert.throws(() => sender.applyViewDelta(delta(33, [33], [], [33])), /peer limit/);
  assert.equal(sender.peers.size, 32);

  sender.configure({ maxPeers: 1000 });
  assert.equal(sender.maxPeers, 1000);
  // The aggregate budget scales with the cap but stays bounded.
  assert.equal(sender.bytesPerSecond, 8 * 1024 * 1024);
  assert.equal(sender.framesPerPump, 192);
  sender.applyViewDelta(delta(33, [33], [], [33]));
  assert.equal(sender.peers.size, 33);
  assert.throws(() => sender.configure({ maxPeers: 1025 }), /peer limit/);
  assert.throws(() => sender.configure({ maxPeers: 0 }), /peer limit/);

  const explicit = new EntitySender({
    store,
    send() {},
    bytesPerSecond: 5000,
    framesPerPump: 3,
    maxPeers: 500,
  });
  assert.equal(explicit.bytesPerSecond, 5000);
  assert.equal(explicit.framesPerPump, 3);
  explicit.configure({ maxPeers: 1000 });
  assert.equal(explicit.bytesPerSecond, 5000);
  assert.equal(explicit.framesPerPump, 3);
});

test('a thousand peers in one dense view stay inside the aggregate budgets', () => {
  const store = new EntityStateStore();
  let frames = 0;
  let bytes = 0;
  const sender = new EntitySender({
    store,
    session: () => SESSION,
    send(_peer, message) {
      frames += 1;
      bytes += Buffer.byteLength(JSON.stringify(message));
    },
    maxPeers: 1000,
  });
  const records = [];
  for (let id = 1; id <= 1000; ++id) {
    store.attach(id);
    records.push(record(id, id));
  }
  for (let offset = 0; offset < records.length; offset += 250) {
    sender.applyRecords({ entities: records.slice(offset, offset + 250) });
  }
  const everyone = records.map((entry) => entry.nativeId);
  for (let peer = 1; peer <= 1000; ++peer) {
    sender.applyViewDelta(delta(peer, everyone, [], [peer]));
  }
  for (let index = 0; index <= 60; ++index) sender.pump(index / 60);
  assert.ok(frames <= sender.framesPerPump * 61);
  // One second of pumping: at most the burst plus one second of refill.
  assert.ok(bytes <= sender.burstBytes + sender.bytesPerSecond);
});

// Directory deltas, one-message frames and raw bytes.
// A directory frame's payload: a whole directory (JSON) or a delta (marker 1, u32 JSON length,
// JSON, then motion records: u32 id, u8 mask, float32s).
function framePayload(data) {
  const bytes = Buffer.from(data);
  if (bytes[0] !== 1) return JSON.parse(bytes);
  const length = bytes.readUInt32LE(1);
  const payload = JSON.parse(bytes.subarray(5, 5 + length));
  payload.moves = [];
  const layout = [
    ['position', 3],
    ['rotation', 4],
    ['velocity', 3],
    ['gaze', 3],
  ];
  for (let at = 5 + length; at < bytes.length; ) {
    const move = { id: bytes.readUInt32LE(at) };
    const mask = bytes[at + 4];
    at += 5;
    layout.forEach(([field, count], bit) => {
      if (!(mask & (1 << bit))) return;
      move[field] = Array.from({ length: count }, (_, i) => bytes.readFloatLE(at + i * 4));
      at += count * 4;
    });
    payload.moves.push(move);
  }
  return payload;
}
test('after the first directory only the changed entries and the departed ids go out', () => {
  const f = fixture();
  const sent = [];
  const original = f.sender.send;
  f.sender.send = (peer, message) => {
    sent.push(message);
    original(peer, message);
  };
  f.sender.applyRecords({ entities: [record(1), record(2), record(3)] });
  f.sender.applyViewDelta(delta(1, [1, 2, 3], [], [1]));
  f.drain(20);
  const first = sent.filter((message) => message.kind === 'directory');
  assert.equal(first.length, 1);
  assert.equal(first[0].op, 'entityFrame');
  assert.ok(first[0].data instanceof Uint8Array);
  assert.equal(framePayload(first[0].data).entities.length, 3);
  sent.length = 0;
  // Player 2 moves, player 3 leaves.
  f.sender.applyRecords({ entities: [record(2, 9)] });
  f.sender.applyViewDelta(delta(1, [], [3], [1]));
  f.drain(20);
  const frames = sent.filter((message) => message.kind === 'directory');
  assert.equal(frames.length, 1);
  const payload = framePayload(frames[0].data);
  assert.equal(payload.base, first[0].revision);
  // Only what moved, as a motion record: player 2's position.
  assert.deepEqual(payload.upserts, []);
  assert.deepEqual(payload.patches, []);
  assert.deepEqual(payload.moves, [{ id: 2, position: [9, 2, 3] }]);
  assert.deepEqual(payload.removed, [3]);
  const api = f.entities.facade();
  assert.equal(new api.Entity(2).position.x, 9);
  assert.throws(() => new api.Entity(3), /Unknown entity/);
  assert.equal(api.LocalPlayer.id, 1);
});

test('a delta the client cannot apply is acknowledged with resync and the whole directory follows', () => {
  const f = fixture();
  f.sender.applyRecords({ entities: [record(1), record(2)] });
  f.sender.applyViewDelta(delta(1, [1, 2], [], [1]));
  f.drain(20);
  // The client lost its directory (its transfer started over in the same session).
  f.receiver.directory = null;
  const sent = [];
  const original = f.sender.send;
  f.sender.send = (peer, message) => {
    sent.push(message);
    original(peer, message);
  };
  f.sender.applyRecords({ entities: [record(2, 7)] });
  f.sender.applyViewDelta(delta(1, [], [], [1]));
  for (let step = 0; step < 20; ++step) {
    f.sender.pump(100 + step * 0.03);
    while (f.messages.length) {
      const { peer, message } = f.messages.shift();
      let resync = false;
      try {
        f.receiver.receive(message);
      } catch (error) {
        assert.ok(error instanceof ResyncNeeded);
        resync = true;
      }
      f.sender.acknowledge(peer, { session: message.session, delivery: message.delivery, resync });
    }
  }
  const frames = sent
    .filter((message) => message.kind === 'directory')
    .map((message) => framePayload(message.data));
  assert.equal(frames.length, 2);
  assert.ok(frames[0].base !== undefined);
  assert.equal(frames[1].entities.length, 2);
  assert.equal(new (f.entities.facade().Entity)(2).position.x, 7);
});

test('a large directory still goes in chunks of raw bytes', () => {
  const f = fixture();
  const sent = [];
  const original = f.sender.send;
  f.sender.send = (peer, message) => {
    sent.push(message);
    original(peer, message);
  };
  const many = Array.from({ length: 200 }, (_, index) => record(index + 1, index));
  f.sender.applyRecords({ entities: many });
  f.sender.applyViewDelta(
    delta(
      1,
      many.map((entry) => entry.id),
      [],
      [1],
    ),
  );
  f.drain(50);
  const chunks = sent.filter((message) => message.op === 'entityChunk');
  assert.ok(chunks.length > 1);
  assert.ok(chunks.every((message) => message.data instanceof Uint8Array));
  assert.equal(f.entities.records.size, 200);
});

// Motion readings: velocities, look direction and the ridden horse in directories.
test('directories carry the motion readings; the own entry has the animation velocity', () => {
  const f = fixture();
  const moving = (x, look) => ({
    ...record(1),
    velocity: { x, y: 0, z: 0 },
    motionVelocity: { x: x + 0.5, y: 0, z: 0 },
    gaze: look,
    horseId: 77,
  });
  const other = {
    ...record(2),
    velocity: { x: 2, y: 0, z: 0 },
    motionVelocity: { x: 9, y: 0, z: 0 },
  };
  f.sender.applyRecords({ entities: [moving(1, { x: 0, y: 1, z: 0 }), other] });
  f.sender.applyViewDelta(delta(1, [1, 2], [], [1]));
  f.drain(20);
  const api = f.entities.facade();
  const local = api.LocalPlayer;
  assert.equal(local.local, true);
  assert.deepEqual({ ...local.velocity }, { x: 1.5, y: 0, z: 0 });
  assert.deepEqual({ ...local.gaze }, { x: 0, y: 1, z: 0 });
  assert.equal(local.horseId, 77);
  const remote = new api.Player(2);
  assert.equal(remote.local, false);
  assert.deepEqual({ ...remote.velocity }, { x: 2, y: 0, z: 0 });
  assert.deepEqual({ ...remote.gaze }, { x: 0, y: 0, z: 0 });
  assert.equal(remote.horseId, 0);
  assert.equal(remote.entityId, 0);
  f.entities.updateEngineIds({
    ids: [
      [1, 5001],
      [2, 5002],
    ],
  });
  assert.equal(remote.entityId, 5002);
  f.entities.updateEngineIds({ ids: [[2, 'x']] });
  assert.equal(remote.entityId, 5002);
  // Looking and moving on are motion records.
  const sent = [];
  const original = f.sender.send;
  f.sender.send = (peer, message) => {
    sent.push(message);
    original(peer, message);
  };
  f.sender.applyRecords({ entities: [moving(3, { x: 1, y: 0, z: 0 })] });
  f.drain(20);
  const payload = framePayload(sent.find((message) => message.kind === 'directory').data);
  assert.deepEqual(payload.patches, []);
  assert.equal(payload.moves.length, 1);
  assert.deepEqual({ ...local.velocity }, { x: 3.5, y: 0, z: 0 });
  assert.deepEqual({ ...local.gaze }, { x: 1, y: 0, z: 0 });
});

// Posture: off the ground, crouched and the physics profile, one number a player.
test('directories carry the posture; a change goes as a patch', () => {
  const f = fixture();
  const standing = (extra) => ({ ...record(1), physicsProfile: 1, ...extra });
  f.sender.applyRecords({ entities: [standing({}), record(2)] });
  f.sender.applyViewDelta(delta(1, [1, 2], [], [1]));
  f.drain(20);
  const api = f.entities.facade();
  assert.equal(api.LocalPlayer.physicsProfile, 1);
  assert.equal(api.LocalPlayer.inAir, false);
  assert.equal(new api.Player(2).physicsProfile, 255);
  const sent = [];
  const original = f.sender.send;
  f.sender.send = (peer, message) => {
    sent.push(message);
    original(peer, message);
  };
  f.sender.applyRecords({ entities: [standing({ inAir: true, crouched: true })] });
  f.drain(20);
  const payload = framePayload(sent.find((message) => message.kind === 'directory').data);
  assert.equal(payload.patches.length, 1);
  assert.equal(api.LocalPlayer.inAir, true);
  assert.equal(api.LocalPlayer.crouched, true);
});

test('directories carry what a body wears and holds; a bare body carries neither', () => {
  const f = fixture();
  const sword = '46db8ef200cca9e319099351ec86bf8c';
  const shirt = '1a2b3c4d5e6f708192a3b4c5d6e7f801';
  const dressed = (extra) => ({ ...record(1), ...extra });
  f.sender.applyRecords({
    entities: [dressed({ equipment: [shirt], hands: [sword, ''] }), record(2)],
  });
  f.sender.applyViewDelta(delta(1, [1, 2], [], [1]));
  f.drain(20);
  const api = f.entities.facade();
  assert.deepEqual(api.LocalPlayer.equipment, [shirt]);
  assert.equal(api.LocalPlayer.rightHandItem, sword);
  assert.equal(api.LocalPlayer.leftHandItem, '');
  const bare = new api.Player(2);
  assert.deepEqual(bare.equipment, []);
  assert.equal(bare.rightHandItem, '');
  const sent = [];
  const original = f.sender.send;
  f.sender.send = (peer, message) => {
    sent.push(message);
    original(peer, message);
  };
  // Undressing goes as a patch that unsets both; a malformed report reads as a bare body.
  f.sender.applyRecords({ entities: [dressed({ equipment: ['not a class'], hands: [] })] });
  f.drain(20);
  const payload = framePayload(sent.find((message) => message.kind === 'directory').data);
  assert.equal(payload.patches.length, 1);
  assert.deepEqual(payload.patches[0].unset.sort(), ['equipment', 'hands']);
  assert.deepEqual(api.LocalPlayer.equipment, []);
  assert.equal(api.LocalPlayer.rightHandItem, '');
});

// A hidden player (Player.setVisible) reads visible false on every client.
test('a player record carries whether its body is drawn into the client directory', () => {
  const f = fixture();
  f.sender.applyRecords({ entities: [{ ...record(1), visible: false }] });
  f.sender.applyViewDelta(delta(1, [1], [], [1], 1));
  f.drain(20);
  const local = f.entities.facade().LocalPlayer;
  assert.equal(local.visible, false);
  f.sender.applyRecords({ entities: [{ ...record(1), visible: true }] });
  f.drain(20);
  assert.equal(local.visible, true);
});

// Player.visorDown on the client: a closed visor reaches every client's directory.
test('a player record carries whether its visor is down into the client directory', () => {
  const f = fixture();
  f.sender.applyRecords({ entities: [{ ...record(1), visorDown: true }] });
  f.sender.applyViewDelta(delta(1, [1], [], [1], 1));
  f.drain(20);
  const local = f.entities.facade().LocalPlayer;
  assert.equal(local.visorDown, true);
  f.sender.applyRecords({ entities: [{ ...record(1), visorDown: false }] });
  f.drain(20);
  assert.equal(local.visorDown, false);
});
