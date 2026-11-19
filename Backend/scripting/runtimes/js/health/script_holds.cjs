'use strict';

// Which resource holds the server's script runtime. Resources share one event loop: while one
// resource's code runs, every other resource and every server event waits. The host enters
// resource code through run(owner, kind, fn) (an event handler, a timer, an export, an HTTP route,
// a script being loaded); a run that holds the loop longer than the threshold is logged as it
// ends, naming the resource and what ran. Each run counts only its own time: a resource's handler
// that calls another resource's export does not take the blame for the export's time.
//
// Code a resource runs after an await is not entered through the host. When the event loop
// monitor sees a hold that no run explains, traceAsync() times every async callback as well for a
// while, with the resource it belongs to. That costs every promise a little, so it is not always
// on.
//
// The time each resource took in the last minute and its slowest runs are kept for KCDS-Admin.

const { createHook } = require('node:async_hooks');

const kThresholdMs = 200;
// One line per resource at most this often; the holds in between are counted in the next line.
const kLogEveryMs = 10000;
const kTraceMs = 10 * 60 * 1000;
// The admin view: the time taken over the last minute (per second), the slowest runs of the last
// ten minutes.
const kSeconds = 60;
const kSlowestKept = 5;
const kSlowestForMs = 10 * 60 * 1000;
const kSlowestFromMs = 5;
// A hold also named a resource that took at least this share of it.
const kAlsoShare = 0.1;
const kAlsoMost = 2;

const AFTER_AWAIT = 'code after an await';
const HOST = 'host';

function rounded(ms) {
  return Math.round(ms);
}

// "shop (setInterval callback, every 3000 ms)", "the host's own code".
function describe(part) {
  if (part.owner === null) {
    return "the host's own code";
  }
  return `${part.owner} (${part.kind})`;
}

class ScriptHolds {
  // log(source, text); owner() names the resource whose code runs now, or null (the host's).
  // now() is a monotonic clock in ms, clock() the wall clock in ms.
  constructor({
    log,
    owner = () => null,
    thresholdMs = kThresholdMs,
    now = () => performance.now(),
    clock = () => Date.now(),
  }) {
    this.log = log;
    this.owner = owner;
    this.thresholdMs = thresholdMs;
    this.now = now;
    this.clock = clock;
    this.stack = [];
    this.usage = new Map();
    this.lastLogged = new Map();
    this.longestNamed = 0;
    this.hook = null;
    this.hookFrames = [];
    this.tracingUntil = 0;
  }

  // Runs fn as resource code: owner is the resource's name (null for the host's own code), kind
  // what runs ("playerJoin handler").
  run(owner, kind, fn) {
    const frame = this.enter(owner, kind);
    try {
      return fn();
    } finally {
      this.leave(frame);
    }
  }

  enter(owner, kind) {
    const frame = { owner, kind, start: this.now(), inner: 0, parts: null };
    this.stack.push(frame);
    return frame;
  }

  leave(frame) {
    const at = this.stack.lastIndexOf(frame);
    if (at < 0) {
      return;
    }
    // Frames above it whose end was never seen (an async callback that threw) are dropped.
    this.stack.length = at;
    const total = this.now() - frame.start;
    const self = Math.max(0, total - frame.inner);
    const part = { owner: frame.owner, kind: frame.kind, ms: self };
    if (self > 0) {
      this.spend(part);
    }
    const parent = this.stack.at(-1);
    if (parent) {
      parent.inner += total;
      parent.parts ??= [];
      parent.parts.push(part);
      if (frame.parts) {
        parent.parts.push(...frame.parts);
      }
      return;
    }
    if (total >= this.thresholdMs) {
      this.held(total, frame.parts ? [part, ...frame.parts] : [part]);
    }
  }

  // A hold that ended: logged under the resource that took most of it.
  held(total, parts) {
    const merged = new Map();
    for (const part of parts) {
      const key = `${part.owner}\u0000${part.kind}`;
      const known = merged.get(key);
      if (known) {
        known.ms += part.ms;
      } else {
        merged.set(key, { ...part });
      }
    }
    const ranked = [...merged.values()].sort((a, b) => b.ms - a.ms);
    const main = ranked[0];
    this.longestNamed = Math.max(this.longestNamed, total);
    const source = main.owner ?? HOST;
    const last = this.lastLogged.get(source);
    const time = this.clock();
    if (last && time - last.at < kLogEveryMs) {
      last.skipped += 1;
      last.skippedLongest = Math.max(last.skippedLongest, total);
      return;
    }
    let text = `The script runtime was held for ${rounded(total)} ms by ${describe(main)}`;
    const also = ranked
      .slice(1)
      .filter((part) => part.owner !== main.owner && part.ms >= total * kAlsoShare)
      .slice(0, kAlsoMost);
    if (also.length > 0) {
      const named = also.map((part) => `${rounded(part.ms)} ms in ${describe(part)}`);
      text += `, ${named.join(' and ')}`;
    }
    text += '; every resource and server event waited';
    if (last && last.skipped > 0) {
      text +=
        ` (and ${last.skipped} more such holds in the last ${Math.round(
          (time - last.at) / 1000,
        )} s,` + ` the longest ${rounded(last.skippedLongest)} ms)`;
    }
    this.lastLogged.set(source, { at: time, skipped: 0, skippedLongest: 0 });
    this.log(source, text);
  }

  spend(part) {
    const name = part.owner ?? HOST;
    let usage = this.usage.get(name);
    if (!usage) {
      usage = { seconds: [], slowest: [] };
      for (let index = 0; index < kSeconds; index += 1) {
        usage.seconds.push({ second: -1, ms: 0 });
      }
      this.usage.set(name, usage);
    }
    const time = this.clock();
    const second = Math.floor(time / 1000);
    const bucket = usage.seconds[second % kSeconds];
    if (bucket.second !== second) {
      bucket.second = second;
      bucket.ms = 0;
    }
    bucket.ms += part.ms;
    if (part.ms < kSlowestFromMs) {
      return;
    }
    const slowest = usage.slowest.filter((run) => time - run.at < kSlowestForMs);
    if (slowest.length < kSlowestKept || part.ms > slowest.at(-1).ms) {
      slowest.push({ kind: part.kind, ms: part.ms, at: time });
      slowest.sort((a, b) => b.ms - a.ms);
      slowest.length = Math.min(slowest.length, kSlowestKept);
    }
    usage.slowest = slowest;
  }

  // The time a resource's code took over the last minute and its slowest runs of the last ten:
  // { cpuMs, slowest: [{ kind, ms, at (ISO time) }] }.
  usageOf(name) {
    const usage = this.usage.get(name);
    const time = this.clock();
    if (!usage) {
      return { cpuMs: 0, slowest: [] };
    }
    const second = Math.floor(time / 1000);
    let cpuMs = 0;
    for (const bucket of usage.seconds) {
      if (bucket.second > second - kSeconds) {
        cpuMs += bucket.ms;
      }
    }
    const slowest = usage.slowest
      .filter((run) => time - run.at < kSlowestForMs)
      .map((run) => ({ kind: run.kind, ms: rounded(run.ms), at: new Date(run.at).toISOString() }));
    return { cpuMs: rounded(cpuMs), slowest };
  }

  // A stopped resource's numbers go.
  forget(name) {
    this.usage.delete(name);
    this.lastLogged.delete(name);
  }

  // The longest hold logged (or counted) since the last call: what the event loop monitor does
  // not have to report again.
  takeLongestNamed() {
    const longest = this.longestNamed;
    this.longestNamed = 0;
    return longest;
  }

  get tracing() {
    return this.hook !== null;
  }

  // Times code run after an await as well, for ms from now.
  traceAsync(ms = kTraceMs) {
    this.tracingUntil = this.clock() + ms;
    if (this.hook) {
      return;
    }
    this.hook = createHook({
      before: () => {
        this.hookFrames.push(this.enter(this.owner(), AFTER_AWAIT));
      },
      after: () => {
        const frame = this.hookFrames.pop();
        if (frame) {
          this.leave(frame);
        }
        if (this.hookFrames.length === 0 && this.clock() >= this.tracingUntil) {
          this.stopTracing();
        }
      },
    });
    this.hook.enable();
  }

  stopTracing() {
    if (!this.hook) {
      return;
    }
    this.hook.disable();
    this.hook = null;
    // The callbacks still open will not say they ended.
    for (const frame of this.hookFrames) {
      const at = this.stack.indexOf(frame);
      if (at >= 0) {
        this.stack.splice(at, 1);
      }
    }
    this.hookFrames = [];
  }
}

module.exports = { ScriptHolds, kThresholdMs, kTraceMs, AFTER_AWAIT };
