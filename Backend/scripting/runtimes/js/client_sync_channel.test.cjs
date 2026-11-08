'use strict';

// The client worker's synchronous requests (Client/resources/sync_channel.cjs): a long run of
// answered calls keeps the worker's heartbeat going, so the runtime page does not end a worker
// that is working.
const test = require('node:test');
const assert = require('node:assert/strict');
const {
  SyncChannel,
  HEADER_BYTES,
  REPLY_BYTES,
} = require('../../../../Client/resources/sync_channel.cjs');

// A channel whose "game" answers every request at once with { v: true }.
function answering() {
  const sent = [];
  const buffer = new SharedArrayBuffer(HEADER_BYTES + REPLY_BYTES);
  const state = new Int32Array(buffer, 0, 2);
  const bytes = new Uint8Array(buffer, HEADER_BYTES, REPLY_BYTES);
  const channel = new SyncChannel((message) => {
    sent.push(message);
    if (message.op === 'syncRequest' && message.id > 0) {
      const reply = new TextEncoder().encode(JSON.stringify({ v: true }));
      bytes.set(reply);
      Atomics.store(state, 1, reply.length);
      Atomics.store(state, 0, message.id);
    }
  });
  channel.attach(buffer);
  return { channel, sent };
}

test('an answered call sends the heartbeat, at most once a second', () => {
  const { channel, sent } = answering();
  assert.equal(channel.call({ op: 'probe' }), true);
  assert.equal(channel.call({ op: 'probe' }), true);
  const beats = sent.filter((message) => message.op === 'heartbeat');
  assert.equal(beats.length, 1);
});

test('a call after a second sends the heartbeat again', () => {
  const { channel, sent } = answering();
  channel.call({ op: 'probe' });
  channel.lastHeartbeat -= 1500;
  channel.call({ op: 'probe' });
  const beats = sent.filter((message) => message.op === 'heartbeat');
  assert.equal(beats.length, 2);
});

test('no channel, no heartbeat', () => {
  const sent = [];
  const channel = new SyncChannel((message) => sent.push(message));
  assert.equal(channel.call({ op: 'probe' }), undefined);
  assert.equal(sent.length, 0);
});

// A channel whose "game" answers only when told to (a level loading runs no frames).
function busyGame() {
  const sent = [];
  const buffer = new SharedArrayBuffer(HEADER_BYTES + REPLY_BYTES);
  const state = new Int32Array(buffer, 0, 2);
  const bytes = new Uint8Array(buffer, HEADER_BYTES, REPLY_BYTES);
  const channel = new SyncChannel((message) => sent.push(message));
  channel.attach(buffer);
  const answer = (id) => {
    const reply = new TextEncoder().encode(JSON.stringify({ v: true }));
    bytes.set(reply);
    Atomics.store(state, 1, reply.length);
    Atomics.store(state, 0, id);
  };
  const requests = () => sent.filter((message) => message.op === 'syncRequest');
  return { channel, sent, answer, requests };
}

test('a call the busy game does not answer keeps the heartbeat and the next ones do not wait', () => {
  const { channel, sent, requests } = busyGame();
  assert.equal(channel.call({ op: 'probe' }, 20), undefined);
  assert.equal(requests().length, 1);
  assert.equal(sent.filter((message) => message.op === 'heartbeat').length, 1);
  // While the game is busy a call gives no answer at once and asks nothing more.
  const started = Date.now();
  for (let i = 0; i < 50; i += 1) {
    assert.equal(channel.call({ op: 'probe' }, 1000), undefined);
  }
  assert.ok(Date.now() - started < 500);
  assert.equal(requests().length, 1);
  // The heartbeat goes on while the script keeps asking.
  channel.lastHeartbeat -= 1500;
  channel.call({ op: 'probe' });
  assert.equal(sent.filter((message) => message.op === 'heartbeat').length, 2);
});

test('the late answer, or a while, lets calls through again', () => {
  const { channel, answer, requests } = busyGame();
  channel.call({ op: 'probe' }, 20);
  const first = requests()[0].id;
  // The game answers the old call once it runs frames again: the next call is sent.
  answer(first);
  assert.equal(channel.call({ op: 'probe' }, 20), undefined);
  assert.equal(requests().length, 2);
  // Unanswered again; after the retry time one call is sent again.
  channel.unansweredAt -= 2500;
  channel.call({ op: 'probe' }, 20);
  assert.equal(requests().length, 3);
});
