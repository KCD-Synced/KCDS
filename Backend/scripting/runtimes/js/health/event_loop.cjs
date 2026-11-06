'use strict';

// How long the server's script runtime is held: resources share one event loop, so a
// resource that computes or blocks for long delays every other resource and the server's events.
// Every everyMs the longest delay since the last look is measured; one over thresholdMs is logged
// with the resource whose code ran last, as a hint where to look.

const { monitorEventLoopDelay } = require('node:perf_hooks');

const kThresholdMs = 200;
const kEveryMs = 30000;

// log(text); lastResource() names the resource whose code ran last, or ''. Returns stop().
function watchEventLoop(
  log,
  { thresholdMs = kThresholdMs, everyMs = kEveryMs, lastResource } = {},
) {
  const histogram = monitorEventLoopDelay({ resolution: 10 });
  histogram.enable();
  const timer = setInterval(() => {
    const longestMs = histogram.max / 1e6;
    histogram.reset();
    if (longestMs > thresholdMs) {
      const who = lastResource ? lastResource() : '';
      log(
        `The script runtime was held for ${Math.round(longestMs)} ms` +
          (who ? ` (last resource code: ${who})` : '') +
          `; every resource and server event waited`,
      );
    }
  }, everyMs);
  timer.unref();
  return () => {
    clearInterval(timer);
    histogram.disable();
  };
}

module.exports = { watchEventLoop, kThresholdMs, kEveryMs };
