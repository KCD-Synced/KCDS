'use strict';

// How long each client resource holds the client scripts with synchronous calls to the game
// (sync_channel.cjs). Every client resource runs in one worker: while one script waits for the
// game's answer, no other script's timers, events or web view messages run. A resource whose calls
// waited more than thresholdMs within one second is named in the client log, with its calls by
// API, at most once every quietMs, so a server team finds the resource that slows the others.

const THRESHOLD_MS = 200;
const WINDOW_MS = 1000;
const QUIET_MS = 30000;

// The API names of the calls scripts make most; any other is named by its request.
const API_NAMES = {
  localWorldCamera: 'Camera.getPose',
  localWorldRay: 'World.raycast',
  localWorldEntities: 'World.entitiesAround',
  localWorldPose: 'Player.jointPosition',
  localGroundHeights: 'World.groundHeight',
  weaponsState: 'Weapons.state',
  weaponsSelect: 'Weapons.select',
};

class SyncWaits {
  // log(owner, text); now() in milliseconds.
  constructor({
    log,
    now = () => Date.now(),
    thresholdMs = THRESHOLD_MS,
    windowMs = WINDOW_MS,
    quietMs = QUIET_MS,
  }) {
    this.log = log;
    this.now = now;
    this.thresholdMs = thresholdMs;
    this.windowMs = windowMs;
    this.quietMs = quietMs;
    this.windowStart = now();
    // owner -> { ms, calls: Map(api -> count) } within the window.
    this.byOwner = new Map();
    // owner -> when it was last named.
    this.named = new Map();
  }

  // A call of `owner` ('' for the runtime itself) that waited `ms`.
  record(owner, op, ms) {
    const time = this.now();
    if (time - this.windowStart >= this.windowMs) {
      this.report(time);
    }
    if (!owner || !(ms >= 0)) {
      return;
    }
    let entry = this.byOwner.get(owner);
    if (!entry) {
      entry = { ms: 0, calls: new Map() };
      this.byOwner.set(owner, entry);
    }
    entry.ms += ms;
    const api = API_NAMES[op] ?? (typeof op === 'string' && op ? op : 'a game query');
    entry.calls.set(api, (entry.calls.get(api) ?? 0) + 1);
  }

  // The window closes at the first call after it: it may have run longer than windowMs (no calls
  // for a while), so the waits are measured against the time it really ran.
  report(time) {
    const seconds = Math.max(1, Math.round((time - this.windowStart) / 1000));
    for (const [owner, entry] of this.byOwner) {
      if (entry.ms / seconds <= this.thresholdMs) {
        continue;
      }
      const last = this.named.get(owner);
      if (last !== undefined && time - last < this.quietMs) {
        continue;
      }
      this.named.set(owner, time);
      const calls = [...entry.calls]
        .sort((a, b) => b[1] - a[1])
        .map(([api, count]) => `${api} ×${count}`)
        .join(', ');
      const span = seconds === 1 ? 'the last second' : `the last ${seconds} seconds`;
      this.log(
        owner,
        `${owner} held the client scripts for ${Math.round(entry.ms)} ms in ${span} waiting ` +
          `for the game (${calls}); every client resource waited. Prefer data the game streams ` +
          `(Camera.getPose, Hud.worldToScreen) and fewer queries from timers`,
      );
    }
    this.byOwner.clear();
    this.windowStart = time;
  }
}

module.exports = { SyncWaits, THRESHOLD_MS, WINDOW_MS, QUIET_MS };
