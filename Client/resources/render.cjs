'use strict';

// Render (a KCDS addition): a picture of the game's world from any camera, taken on
// this machine without the player seeing another view. Render.view({ position, rotation |
// lookAt and roll, fov, width }) asks the game client ({ op: 'renderView', request, ... }); it
// films one frame from that pose and keeps showing the player the frame before meanwhile (a
// freeze of a few frames, never the other view), and answers with the frame as a PNG in base64
// pieces ({ op: 'renderViewChunk', request, data }: a runtime message holds 64 KiB), then { op:
// 'renderViewDone', request, chunks, width, height }, or { op: 'renderViewFailed', request,
// reason }. The picture is the game's rendered frame with its own HUD as the player has it
// (Hud.setHidden hides elements of it); KCDS's own overlays (chat, nameplates) are not in it.
//
// One render at a time; another waits for it. A pose is checked as Camera.startShot checks it
// (Backend/scripting/runtimes/js/camera/shots.cjs).

const { shotMessage } = require('../../Backend/scripting/runtimes/js/camera/shots.cjs');

// The longest a render may take before it is given up (a loading screen, the window minimised).
const TIMEOUT_MS = 15000;
const MIN_WIDTH = 16;
const MAX_WIDTH = 7680;

class ResourceRender {
  // timers: setTimeout and clearTimeout (tests give their own); the worker's own must be called
  // unbound, so they are wrapped.
  constructor({
    send,
    timers = {
      setTimeout: (callback, ms) => setTimeout(callback, ms),
      clearTimeout: (handle) => clearTimeout(handle),
    },
  }) {
    this.send = send;
    this.timers = timers;
    this.next = 1;
    // request -> { resolve, reject, timer, chunks: [] }
    this.pending = new Map();
  }

  view(options) {
    if (options === null || typeof options !== 'object') {
      throw new TypeError('Render.view takes { position, rotation | lookAt, fov, width }');
    }
    const width = options.width === undefined ? 0 : options.width;
    if (width !== 0 && (!Number.isInteger(width) || width < MIN_WIDTH || width > MAX_WIDTH)) {
      throw new RangeError(
        `Render.view width must be a whole number from ${MIN_WIDTH} to ${MAX_WIDTH}`,
      );
    }
    const shot = { ...options };
    delete shot.width;
    const message = shotMessage({ id: 0 }, shot);
    delete message.id;
    delete message.seconds;
    delete message.ease;
    const request = this.next;
    this.next += 1;
    message.op = 'renderView';
    message.request = request;
    message.width = width;
    return new Promise((resolve, reject) => {
      const timer = this.timers.setTimeout(() => {
        this.pending.delete(request);
        reject(new Error('Render.view timed out'));
      }, TIMEOUT_MS);
      this.pending.set(request, { resolve, reject, timer, chunks: [] });
      this.send(message);
    });
  }

  receiveChunk({ request, data }) {
    const waiting = this.pending.get(request);
    if (waiting && typeof data === 'string') {
      waiting.chunks.push(data);
    }
  }

  receiveDone({ request, chunks, width, height }) {
    const waiting = this.pending.get(request);
    if (!waiting) {
      return;
    }
    this.pending.delete(request);
    this.timers.clearTimeout(waiting.timer);
    if (waiting.chunks.length !== chunks) {
      waiting.reject(new Error('Render.view failed: the picture did not arrive whole'));
      return;
    }
    const png = waiting.chunks.join('');
    waiting.resolve(
      Object.freeze({
        png: `data:image/png;base64,${png}`,
        width: Number(width),
        height: Number(height),
      }),
    );
  }

  receiveFailed({ request, reason }) {
    const waiting = this.pending.get(request);
    if (!waiting) {
      return;
    }
    this.pending.delete(request);
    this.timers.clearTimeout(waiting.timer);
    waiting.reject(new Error(`Render.view failed: ${String(reason ?? 'unknown')}`));
  }

  facade() {
    const render = this;
    return Object.freeze({
      view(options) {
        return render.view(options);
      },
    });
  }
}

module.exports = { ResourceRender };
