'use strict';

// Stopping and restarting the server with warnings: from KCDS-Admin, or every day
// at the times in KCDS_RESTART_TIMES ("06:00,18:00", the server's local time). The players are told
// in chat 15, 5 and 1 minute(s) and 10 seconds before; then the native server ends their sessions
// with the reason and exits. A restart exits with the code the start script starts the server again
// on (start-server.sh; a container's restart policy does the same).

const WARNINGS_SECONDS = [15 * 60, 5 * 60, 60, 10];
const MAX_MINUTES = 24 * 60;
const TIME = /^([01]\d|2[0-3]):([0-5]\d)$/;
const COLOR = 0xd9a441ff;

// "06:00,18:00" as [[6, 0], [18, 0]]; invalid entries are left out (and logged by the caller).
function parseTimes(text) {
  const times = [];
  const invalid = [];
  for (const part of String(text ?? '')
    .split(',')
    .map((entry) => entry.trim())
    .filter(Boolean)) {
    const match = TIME.exec(part);
    if (match) {
      times.push([Number(match[1]), Number(match[2])]);
    } else {
      invalid.push(part);
    }
  }
  return { times, invalid };
}

function inWords(seconds) {
  if (seconds >= 60) {
    const minutes = Math.round(seconds / 60);
    return minutes === 1 ? '1 minute' : `${minutes} minutes`;
  }
  return `${seconds} seconds`;
}

class ServerSchedule {
  // host: the resource host (its bridge sends chat and serverStop); options: { times, log, now,
  // timers: { setTimeout, clearTimeout } }.
  constructor(host, options = {}) {
    this.host = host;
    this.log = options.log ?? (() => {});
    this.now = options.now ?? (() => Date.now());
    this.timers = options.timers ?? { setTimeout, clearTimeout };
    const { times, invalid } = parseTimes(options.times);
    this.times = times;
    if (invalid.length > 0) {
      this.log(`KCDS_RESTART_TIMES: ignoring ${invalid.join(', ')} (HH:MM, 24 hours)`);
    }
    this.handles = [];
    this.plan = null;
  }

  // The daily restarts begin.
  start() {
    if (this.times.length > 0 && !this.plan) {
      this.planDaily();
    }
  }

  stop() {
    this.clear();
    this.plan = null;
  }

  // The next daily restart, at the earliest of the times still ahead.
  planDaily() {
    const now = new Date(this.now());
    let next = null;
    for (const [hour, minute] of this.times) {
      const at = new Date(now);
      at.setHours(hour, minute, 0, 0);
      if (at.getTime() <= now.getTime() + 60000) {
        at.setDate(at.getDate() + 1);
      }
      if (!next || at < next) {
        next = at;
      }
    }
    this.schedule({ restart: true, at: next.getTime(), by: 'the daily schedule', daily: true });
  }

  // Stops or restarts the server in `minutes` (0 at once), warning the players. Replaces a plan.
  request({ restart, minutes, by }) {
    if (!Number.isFinite(minutes) || minutes < 0 || minutes > MAX_MINUTES) {
      throw new RangeError(`In 0 to ${MAX_MINUTES} minutes`);
    }
    const now = this.now();
    return this.schedule({ restart: !!restart, at: now + Math.round(minutes * 60000), by, now });
  }

  schedule({ restart, at, by, daily = false, now = this.now() }) {
    this.clear();
    this.plan = { restart, at, by, daily };
    const what = restart ? 'restarts' : 'shuts down';
    // One clock reading: a warning exactly at the plan's start (1 minute for "in 1 minute") is sent.
    const start = Math.min(now, at);
    for (const seconds of WARNINGS_SECONDS) {
      const delay = at - seconds * 1000 - start;
      if (delay >= 0) {
        this.handles.push(
          this.timers.setTimeout(() => {
            this.say(`The server ${what} in ${inWords(seconds)}.`);
          }, delay),
        );
      }
    }
    this.handles.push(this.timers.setTimeout(() => this.fire(), Math.max(0, at - start)));
    this.log(`the server ${what} at ${new Date(at).toISOString()} (${by})`);
    return this.pending();
  }

  cancel(by) {
    if (!this.plan) {
      return false;
    }
    const restart = this.plan.restart;
    this.clear();
    this.plan = null;
    this.say(`The server ${restart ? 'restart' : 'shutdown'} was called off.`);
    this.log(`the planned ${restart ? 'restart' : 'shutdown'} was called off (${by})`);
    // The daily restarts go on.
    if (this.times.length > 0) {
      this.planDaily();
    }
    return true;
  }

  pending() {
    if (!this.plan) {
      return null;
    }
    const { restart, at, by, daily } = this.plan;
    return { restart, at, by, daily };
  }

  fire() {
    const restart = this.plan?.restart ?? false;
    this.handles = [];
    this.plan = null;
    const reason = restart
      ? 'The server is restarting. Join again in a minute.'
      : 'The server was shut down.';
    this.log(`the server ${restart ? 'restarts' : 'shuts down'} now`);
    // The data as it was before the stop; a failed backup does not keep it running.
    try {
      this.host.serverBackups?.create(`before the ${restart ? 'restart' : 'shutdown'}`);
    } catch (error) {
      this.log(`the backup before the stop failed: ${error.message}`);
    }
    // A downloaded server update goes in place now: the start script starts the new server.
    if (restart) {
      try {
        this.host.serverUpdates?.apply();
      } catch (error) {
        this.log(`the update was not applied: ${error.message}`);
      }
    }
    this.host.bridge.send({ op: 'serverStop', reason, restart });
  }

  say(text) {
    this.host.bridge.send({ op: 'chat', target: 0, text, author: 'Server', color: COLOR });
  }

  clear() {
    for (const handle of this.handles) {
      this.timers.clearTimeout(handle);
    }
    this.handles = [];
  }
}

module.exports = { ServerSchedule, parseTimes };
