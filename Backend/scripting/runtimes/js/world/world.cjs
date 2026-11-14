'use strict';

// The server's world time and weather, which every player's game follows (the server API's World).
// A resource reads and sets the clock, the sky, the rain and the wind
// here; the server tells each player's game, which only applies what it hears.
//
// The ops sent to a player's client (every one is for a player with a ready world):
//   { op: 'worldTime', id: <player id>, day, hour, timeScale }
//     the game time at the moment of sending (day whole, hour 0 up to 24 with the minutes as the
//     fraction) and the clock's speed in game seconds per real second (0: frozen). The client sets its
//     clock to it and runs it at that speed.
//   { op: 'worldWeather', id: <player id>, preset, previous, seconds, remaining, rainStrength,
//     rainLevel, wind: { x, y, z }, snow: { amount, frost, freezing, flakes, flakeSize, storm },
//     fog }
//     the time-of-day preset the sky blends to, the one it blends from, the length of the blend
//     and how much of it is left (both in game seconds; `remaining` 0 means the sky is settled and
//     `preset` is shown at once), the rain (0 to 1 each), the wind in metres per second and the
//     snow (World.setSnow).
// Both are sent when the value they carry is changed by a resource and when a player's world is
// ready (again). worldTime is also sent to every player once in a while while the clock runs, to
// pull the players' own clocks back to the server's.
//
// There is one timer and it is not a tick: it sleeps until the next midnight or the next
// correction, whichever comes first, and does not exist while the clock is frozen.
//
// Events: worldDayChange (day) when the clock crosses midnight, walking or wound over, and
// worldWeatherChange (preset, previous, seconds) when a blend is started.
//
// Not covered: World.level, World.landmarks, the `/world weather` command, and wetness and
// puddles being known to the clients (they work these out from the rain themselves).

const { Clock } = require('./clock.cjs');
const { Weather } = require('./weather.cjs');

// The longest a running clock goes without being restated to the players.
const CORRECTION_MS = 30000;
// A timer is never set for sooner than this, so a wake-up that lands a hair early cannot spin.
const MIN_DELAY_MS = 250;
// A timer that fires this much before it was due still counts as due.
const EARLY_MS = 50;
// A player who becomes ready is sent the weather once more after this long, because their game
// may still be starting its weather system at the first send.
const ARRIVAL_WEATHER_RESEND_MS = 4000;

class WorldState {
  // `now` is the real time in milliseconds, from a clock that only goes forward.
  constructor(host, now = () => performance.now()) {
    this.host = host;
    this.now = now;
    this.clock = new Clock(now);
    this.weather = new Weather(() => this.clock.seconds());
    this.lastDay = this.clock.day;
    // When the players were last told the time
    this.lastSent = now();
    this.timer = null;
    // The one-time weather resends owed to players who just became ready.
    this.arrivalTimers = new Set();
  }

  sendTime(player) {
    this.host.bridge.send({
      op: 'worldTime',
      id: player.id,
      day: this.clock.day,
      hour: this.clock.hour,
      timeScale: this.clock.scale,
    });
  }

  sendWeather(player) {
    const weather = this.weather;
    this.host.bridge.send({
      op: 'worldWeather',
      id: player.id,
      preset: weather.target,
      previous: weather.previous,
      seconds: weather.blendSeconds,
      remaining: weather.remaining,
      rainStrength: weather.rainStrength,
      rainLevel: weather.rainLevel,
      wind: { ...weather.wind },
      snow: { ...weather.snow },
      fog: weather.fog,
    });
  }

  // Tells every player whose world is ready, if there is any.
  broadcast(send) {
    for (const player of this.host.players.values()) {
      if (player.ready) {
        send.call(this, player);
      }
    }
  }

  // A player's world is ready: they hear the time and the weather, and the weather once more a
  // few seconds later, in case their game was not ready for the first one.
  arrived(player) {
    this.sendTime(player);
    this.sendWeather(player);
    const timer = setTimeout(() => this.resendWeather(player, timer), ARRIVAL_WEATHER_RESEND_MS);
    timer.unref?.();
    this.arrivalTimers.add(timer);
  }

  // The one resend owed to a player who arrived: sent only while they are still in and ready.
  resendWeather(player, timer) {
    this.arrivalTimers.delete(timer);
    if (this.host.players.get(player.id) === player && player.ready) {
      this.sendWeather(player);
    }
  }

  // The time changed: everyone is told, the day is checked and the timer is set again.
  timeChanged() {
    this.broadcast(this.sendTime);
    this.lastSent = this.now();
    this.noteDay();
    this.schedule();
  }

  // Raises worldDayChange when the clock is on a later day than it was last seen on.
  noteDay() {
    const day = this.clock.day;
    if (day !== this.lastDay) {
      this.lastDay = day;
      this.host.emit('worldDayChange', day);
    }
  }

  // Starts the timer (the server calls this once its resources start).
  start() {
    this.schedule();
  }

  // Stops the timer. The clock goes on being right: it reads the real time when asked.
  stop() {
    clearTimeout(this.timer);
    this.timer = null;
    for (const timer of this.arrivalTimers) {
      clearTimeout(timer);
    }
    this.arrivalTimers.clear();
  }

  // Sets the one timer for whichever comes first, the next midnight or the next correction. A
  // frozen clock needs neither.
  schedule() {
    clearTimeout(this.timer);
    this.timer = null;
    const scale = this.clock.scale;
    if (!(scale > 0)) {
      return;
    }
    const untilMidnight = (this.clock.secondsToMidnight() / scale) * 1000;
    const untilCorrection = this.lastSent + CORRECTION_MS - this.now();
    const delay = Math.max(MIN_DELAY_MS, Math.min(untilMidnight, untilCorrection));
    this.timer = setTimeout(() => this.run(), delay);
    this.timer.unref?.();
  }

  // The timer, outside any resource and never throwing: an error in a timer would take the whole
  // server down.
  run() {
    this.timer = null;
    this.host.context.exit(() => {
      try {
        this.wake();
      } catch (error) {
        this.host.bridge.log('host', `world clock: ${error?.stack ?? error}`);
      }
    });
  }

  // What the timer does: sees to the day, restates the time when it is due (or at once if the
  // clock ran out of days, which stops it), and sets the timer again.
  wake() {
    const ranOut = this.clock.atCeiling() && this.clock.scale > 0;
    if (ranOut) {
      this.clock.setScale(0);
    }
    if (ranOut || this.now() - this.lastSent >= CORRECTION_MS - EARLY_MS) {
      this.broadcast(this.sendTime);
      this.lastSent = this.now();
    }
    this.noteDay();
    this.schedule();
  }

  // The value-taking setters of the server's World. They answer false for a value out of range.
  setTime(day, hour) {
    const done = this.clock.setTime(day, hour);
    if (done) {
      this.timeChanged();
    }
    return done;
  }

  setHour(hour) {
    const done = this.clock.setHour(hour);
    if (done) {
      this.timeChanged();
    }
    return done;
  }

  setTimeScale(scale) {
    const done = this.clock.setScale(scale);
    if (done) {
      this.timeChanged();
    }
    return done;
  }

  setWeather(preset, seconds) {
    const previous = this.weather.target;
    if (!this.weather.blendTo(preset, seconds)) {
      return false;
    }
    this.broadcast(this.sendWeather);
    this.host.emit('worldWeatherChange', preset, previous, seconds ?? 0);
    return true;
  }

  setRain(intensity, amount) {
    const done = this.weather.setRain(intensity, amount);
    if (done) {
      this.broadcast(this.sendWeather);
    }
    return done;
  }

  setWind(wind) {
    const done = this.weather.setWind(wind);
    if (done) {
      this.broadcast(this.sendWeather);
    }
    return done;
  }

  // Every player's game shows the snow from now on; a snow equal to the one shown sends nothing.
  setSnow(snow) {
    const before = this.weather.snow;
    const done = this.weather.setSnow(snow);
    const after = this.weather.snow;
    const changed = Object.keys(after).some((key) => after[key] !== before[key]);
    if (done && changed) {
      this.broadcast(this.sendWeather);
    }
    return done;
  }

  // Every player's game shows the fog from now on; the same fog again sends nothing.
  setFog(strength) {
    const before = this.weather.fog;
    const done = this.weather.setFog(strength);
    if (done && this.weather.fog !== before) {
      this.broadcast(this.sendWeather);
    }
    return done;
  }

  // The World constant of the server API: `base` is what the host already puts on it, and the
  // reads here stay live reads.
  // `base`: further members; `extraReads`: further read-only properties, name -> reader.
  api(base, extraReads = {}) {
    const self = this;
    const clock = this.clock;
    const weather = this.weather;
    const reads = {
      day: () => clock.day,
      hour: () => clock.hour,
      timeScale: () => clock.scale,
      weather: () => weather.target,
      lastWeather: () => weather.previous,
      weatherFade: () => weather.blending,
      weatherLeft: () => weather.remaining,
      rainStrength: () => weather.rainStrength,
      rainLevel: () => weather.rainLevel,
      wind: () => weather.windVector(),
      windSpeed: () => weather.windSpeed,
      wetness: () => weather.wetness,
      puddles: () => weather.puddles,
      snow: () => weather.snow,
      fog: () => weather.fog,
    };
    const world = { ...base };
    for (const [name, read] of Object.entries({ ...reads, ...extraReads })) {
      Object.defineProperty(world, name, { get: read, enumerable: true });
    }
    world.setTime = (day, hour) => self.setTime(day, hour);
    world.setHour = (hour) => self.setHour(hour);
    world.setTimeScale = (scale) => self.setTimeScale(scale);
    world.setWeather = (preset, seconds) => self.setWeather(preset, seconds);
    world.setRain = (intensity, amount) => self.setRain(intensity, amount);
    world.setWind = (wind) => self.setWind(wind);
    world.setSnow = (snow) => self.setSnow(snow);
    world.setFog = (strength) => self.setFog(strength);
    return Object.freeze(world);
  }
}

module.exports = { WorldState };
