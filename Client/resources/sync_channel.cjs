'use strict';

// Synchronous requests from the client resource worker to the game client.
// Some client APIs answer at once with a value only the game has, such as a
// variable of one of the game's Scaleform movies. The runtime page shares a buffer with the
// worker (it is cross-origin isolated for that); the worker posts { op: 'syncRequest', id,
// request } and waits on the buffer, the page forwards the request to the game client like any
// other message, and the game client answers on its next frame with { op: 'syncReply', id,
// value }, which the page writes into the buffer before it wakes the worker. The page never
// blocks; only the calling script waits, for about one game frame.
//
// Buffer layout: Int32 [0] the id of the reply in it, Int32 [1] its byte length, then the reply
// as UTF-8 JSON { v: value }.

const HEADER_BYTES = 8;
const REPLY_BYTES = 262144;
const DEFAULT_TIMEOUT_MS = 1000;
// The runtime page ends a worker that sent no heartbeat for 10 s. A long run of answered calls
// (World Architect leaving a map of thousands of objects, each taken away with a call) is work,
// not a stall: every answer sends the heartbeat again, at most once a second.
const HEARTBEAT_MS = 1000;
// After a call the game did not answer in time (it is busy, such as loading a level, and runs no
// frames), the next calls give no answer at once instead of each waiting its time out; one is
// sent again this long after, or as soon as the late answer arrives.
const BUSY_RETRY_MS = 2000;

class SyncChannel {
  constructor(send) {
    this.send = send;
    this.state = null;
    this.bytes = null;
    this.nextId = 1;
    this.decoder = new TextDecoder('utf-8');
    this.lastHeartbeat = 0;
    // The call the game has not answered yet (0 when none), and when it timed out.
    this.unanswered = 0;
    this.unansweredAt = 0;
  }

  // True while the game is still busy with a call it did not answer in time: calls give no
  // answer without waiting.
  busy() {
    if (this.unanswered === 0) {
      return false;
    }
    if (Atomics.load(this.state, 0) === this.unanswered) {
      // The late answer came: the game runs frames again.
      this.unanswered = 0;
      return false;
    }
    return Date.now() - this.unansweredAt < BUSY_RETRY_MS;
  }

  // The worker's heartbeat, sent from inside a run of calls.
  beat() {
    const now = Date.now();
    if (now - this.lastHeartbeat < HEARTBEAT_MS) {
      return;
    }
    this.lastHeartbeat = now;
    this.send({ op: 'heartbeat' });
  }

  // The page's shared buffer, sent once when the worker starts.
  attach(buffer) {
    if (typeof SharedArrayBuffer !== 'function' || !(buffer instanceof SharedArrayBuffer)) {
      return;
    }
    if (buffer.byteLength < HEADER_BYTES + REPLY_BYTES) {
      return;
    }
    this.state = new Int32Array(buffer, 0, 2);
    this.bytes = new Uint8Array(buffer, HEADER_BYTES, REPLY_BYTES);
  }

  get available() {
    return this.state !== null;
  }

  // Sends request without waiting: the game client runs it in order with the other requests
  // and answers nobody (a change whose result the caller does not need).
  post(request) {
    if (this.state) {
      this.send({ op: 'syncRequest', id: 0, posted: true, request });
    }
  }

  // Sends request and waits for the game client's answer; undefined when there is no answer
  // (no channel, or no reply in time).
  call(request, timeoutMs = DEFAULT_TIMEOUT_MS) {
    if (!this.state) {
      return undefined;
    }
    // A script asking while the game is busy is working, not stalled: the heartbeat goes on, so
    // the runtime page does not end the worker while a level loads.
    if (this.busy()) {
      this.beat();
      return undefined;
    }
    const id = this.nextId;
    this.nextId = this.nextId >= 0x7fffffff ? 1 : this.nextId + 1;
    Atomics.store(this.state, 0, 0);
    this.send({ op: 'syncRequest', id, request });
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      const current = Atomics.load(this.state, 0);
      if (current === id) {
        break;
      }
      const left = deadline - Date.now();
      if (left <= 0) {
        this.unanswered = id;
        this.unansweredAt = Date.now();
        this.beat();
        return undefined;
      }
      Atomics.wait(this.state, 0, current, left);
    }
    this.unanswered = 0;
    this.beat();
    const length = Atomics.load(this.state, 1);
    if (length <= 0 || length > REPLY_BYTES) {
      return undefined;
    }
    try {
      const reply = JSON.parse(this.decoder.decode(this.bytes.slice(0, length)));
      return reply && typeof reply === 'object' ? reply.v : undefined;
    } catch {
      return undefined;
    }
  }
}

module.exports = { SyncChannel, HEADER_BYTES, REPLY_BYTES };
