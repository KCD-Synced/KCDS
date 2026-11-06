'use strict';

// The world clock: the server's own time of day, which every player's game follows (World.day,
// World.hour, World.timeScale). Nothing ticks here. The clock holds
// the game time at one real moment and how fast it runs from there, and works out the time now
// from the real clock whenever it is asked, so it costs nothing while nobody looks.
//
// Game time is counted in seconds since midnight of day 0, the level's own midnight. The clock
// never runs backwards: a game cannot be wound back.

const SECONDS_PER_DAY = 86400;
const SECONDS_PER_HOUR = 3600;
// The game's own pace, in game seconds per real second.
const DEFAULT_SCALE = 15;
const MAX_SCALE = 200;
// The hour a new server starts at.
const START_HOUR = 9;
// The last day the clock counts. It stops at midnight of this day, so a long-running server keeps
// every time exact and well within what a client can hold.
const CEILING_DAYS = 36500;
const CEILING_SECONDS = CEILING_DAYS * SECONDS_PER_DAY;

function validHour(hour) {
  return typeof hour === 'number' && Number.isFinite(hour) && hour >= 0 && hour < 24;
}

class Clock {
  // `now` is the real time in milliseconds, from a clock that only goes forward.
  constructor(now) {
    this.now = now;
    this.scale = DEFAULT_SCALE;
    // The game time (seconds) at the real moment `at` (milliseconds)
    this.base = START_HOUR * SECONDS_PER_HOUR;
    this.at = now();
  }

  // The game time now, in seconds since midnight of day 0.
  seconds() {
    const elapsed = (this.now() - this.at) / 1000;
    return Math.min(CEILING_SECONDS, this.base + elapsed * this.scale);
  }

  // Whole days the clock has run.
  get day() {
    return Math.floor(this.seconds() / SECONDS_PER_DAY);
  }

  // Time of day as a decimal hour from 0 to below 24, with minutes in the fraction.
  get hour() {
    return (this.seconds() % SECONDS_PER_DAY) / SECONDS_PER_HOUR;
  }

  // Whether the clock has run out of days.
  atCeiling() {
    return this.seconds() >= CEILING_SECONDS;
  }

  // Game seconds from now to the next midnight.
  secondsToMidnight() {
    return SECONDS_PER_DAY - (this.seconds() % SECONDS_PER_DAY);
  }

  // Sets the speed in game seconds per real second, 0 to MAX_SCALE. False when it is out of range.
  setScale(scale) {
    if (typeof scale !== 'number' || !Number.isFinite(scale) || scale < 0 || scale > MAX_SCALE) {
      return false;
    }
    // Hold the game time as it is now, so that the new speed starts from here.
    this.windTo(this.seconds());
    this.scale = scale;
    return true;
  }

  // Winds the clock forward to a game time in seconds. False when that is already past or beyond
  // the last day (the present, when the clock stands at the ceiling, is always allowed).
  windTo(target) {
    const now = this.seconds();
    if (target < now || (target >= CEILING_SECONDS && target !== now)) {
      return false;
    }
    this.base = target;
    this.at = this.now();
    return true;
  }

  // Advances to a whole day number and an hour below 24. False when either
  // is out of range or that moment is already past.
  setTime(day, hour) {
    if (!Number.isInteger(day) || day < 0 || !validHour(hour)) {
      return false;
    }
    return this.windTo(day * SECONDS_PER_DAY + hour * SECONDS_PER_HOUR);
  }

  // Advances the clock to the next occurrence of this hour, tomorrow if today's has passed.
  setHour(hour) {
    if (!validHour(hour)) {
      return false;
    }
    const seconds = this.seconds();
    const today = Math.floor(seconds / SECONDS_PER_DAY) * SECONDS_PER_DAY;
    const target = today + hour * SECONDS_PER_HOUR;
    return this.windTo(target > seconds ? target : target + SECONDS_PER_DAY);
  }
}

module.exports = { Clock, DEFAULT_SCALE, MAX_SCALE, SECONDS_PER_DAY, SECONDS_PER_HOUR };
