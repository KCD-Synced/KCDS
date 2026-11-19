'use strict';

// How long the server's script runtime is held: resources share one event loop, so a
// resource that computes or blocks for long delays every other resource and the server's events.
// Holds by resource code the host entered are logged as they end, with the resource
// (script_holds.cjs). This monitor is the net under that: every everyMs it takes the longest delay
// since the last look, and one over thresholdMs that no logged hold explains is reported, and
// unnamed(ms) may add what is done to name it next time.

const { monitorEventLoopDelay } = require('node:perf_hooks');

const kThresholdMs = 200;
const kEveryMs = 30000;
// The monitor samples every 10 ms: a hold it measures may be that much longer than the one logged.
const kResolutionMs = 10;
const kSlackMs = 50;

// log(text); lastResource() names the resource whose code ran last, or ''; named() the longest
// hold logged since the last look (ms); unnamed(ms) a sentence to add to the report, or ''.
// Returns stop().
function watchEventLoop(
  log,
  { thresholdMs = kThresholdMs, everyMs = kEveryMs, lastResource, named, unnamed } = {},
) {
  const histogram = monitorEventLoopDelay({ resolution: kResolutionMs });
  histogram.enable();
  const timer = setInterval(() => {
    const longestMs = histogram.max / 1e6;
    histogram.reset();
    const explained = named ? named() : 0;
    if (longestMs <= thresholdMs || longestMs <= explained + kSlackMs) {
      return;
    }
    const who = lastResource ? lastResource() : '';
    const seconds = Math.max(1, Math.round(everyMs / 1000));
    log(
      `The script runtime was held for ${Math.round(longestMs)} ms in the last ${seconds} s` +
        (who ? ` (last resource code: ${who})` : '') +
        '; every resource and server event waited' +
        (unnamed ? unnamed(longestMs) : ''),
    );
  }, everyMs);
  timer.unref();
  return () => {
    clearInterval(timer);
    histogram.disable();
  };
}

module.exports = { watchEventLoop, kThresholdMs, kEveryMs };
