'use strict';

// The sky, the rain, the wind and the snow the server dictates (World.weather, World.setRain,
// World.setWind, World.setSnow).
// Like the clock it holds the state and works the time-dependent
// parts out when asked. Blends are measured in game seconds, so they follow the world clock: a
// frozen clock holds a blend where it is, and winding the clock past its end settles it.

const { Vector3 } = require('../math.cjs');

const DEFAULT_PRESET = 'cloudless_sunny';
const PRESET_PATTERN = /^[\w.-]{1,64}$/;
const MAX_BLEND_SECONDS = 21600;
const MAX_WIND = 50;
// Game seconds for a full downpour to wet the ground by 1, and for dry weather to take 1 away.
// Ground dries far slower than it soaks.
const SOAK_SECONDS = 1800;
const DRY_SECONDS = 14400;
const MAX_WETNESS = 3;
// Snow on surfaces goes up to 10 (deep), the falling flakes around each player up to 10000
// (kcd2s/integration/world_weather.hpp kMaxSnowAmount, kMaxSnowFlakes).
const MAX_SNOW_AMOUNT = 10;
const MAX_SNOW_FLAKES = 10000;
const NO_SNOW = Object.freeze({ amount: 0, frost: 0, freezing: 0, flakes: 0 });

function unit(value) {
  return typeof value === 'number' && Number.isFinite(value) && value >= 0 && value <= 1;
}

class Weather {
  // `seconds` gives the game time now.
  constructor(seconds) {
    this.seconds = seconds;
    this.target = DEFAULT_PRESET;
    this.from = DEFAULT_PRESET;
    // The length of the running blend and the game time it ends at
    this.blendSeconds = 0;
    this.blendEnd = 0;
    this.rainStrength = 0;
    this.rainLevel = 1;
    // The ground's wetness at the game time `wetAt`
    this.wet = 0;
    this.wetAt = seconds();
    this.wind = { x: 0, y: 0, z: 0 };
    this.snow = NO_SNOW;
  }

  // Sets the snow: { amount (0 to 10 on surfaces), frost (0 to 1), freezing (0 to 1), flakes
  // (0 to 10000 falling around each player) }, a part left out being 0. False for a part out of
  // range; nothing changes then.
  setSnow(snow) {
    if (snow === null || typeof snow !== 'object' || Array.isArray(snow)) {
      return false;
    }
    const amount = snow.amount ?? 0;
    const frost = snow.frost ?? 0;
    const freezing = snow.freezing ?? 0;
    const flakes = snow.flakes ?? 0;
    const amountFits =
      typeof amount === 'number' &&
      Number.isFinite(amount) &&
      amount >= 0 &&
      amount <= MAX_SNOW_AMOUNT;
    const flakesFit = Number.isInteger(flakes) && flakes >= 0 && flakes <= MAX_SNOW_FLAKES;
    if (!amountFits || !unit(frost) || !unit(freezing) || !flakesFit) {
      return false;
    }
    this.snow = Object.freeze({ amount, frost, freezing, flakes });
    return true;
  }

  // Game seconds left of the running blend, 0 once the sky has settled.
  get remaining() {
    return Math.max(0, this.blendEnd - this.seconds());
  }

  get blending() {
    return this.remaining > 0;
  }

  // The preset the sky blends from; the target itself once it has settled.
  get previous() {
    return this.blending ? this.from : this.target;
  }

  // Starts blending to a preset over `seconds` game seconds (0 for at once). False while an
  // earlier blend runs or when the name or length is out of range.
  blendTo(preset, seconds = 0) {
    if (
      typeof preset !== 'string' ||
      !PRESET_PATTERN.test(preset) ||
      typeof seconds !== 'number' ||
      !Number.isFinite(seconds) ||
      seconds < 0 ||
      seconds > MAX_BLEND_SECONDS ||
      this.blending
    ) {
      return false;
    }
    this.from = this.target;
    this.target = preset;
    this.blendSeconds = seconds;
    this.blendEnd = this.seconds() + seconds;
    return true;
  }

  // Sets the rain; both values run 0 to 1. False when one is out of range.
  setRain(intensity, amount = 1) {
    if (!unit(intensity) || !unit(amount)) {
      return false;
    }
    // The ground keeps the wetness it had and soaks or dries from here.
    this.wet = this.wetness;
    this.wetAt = this.seconds();
    this.rainStrength = intensity;
    this.rainLevel = amount;
    return true;
  }

  // How wet the ground is, 0 to 3: it soaks while it rains and dries slowly after.
  get wetness() {
    const elapsed = Math.max(0, this.seconds() - this.wetAt);
    if (this.rainStrength > 0) {
      return Math.min(MAX_WETNESS, this.wet + (this.rainStrength * elapsed) / SOAK_SECONDS);
    }
    return Math.max(0, this.wet - elapsed / DRY_SECONDS);
  }

  // Puddle coverage, 0 to 1: they only start to fill once the ground is thoroughly wet.
  get puddles() {
    return Math.min(1, Math.max(0, (this.wetness - 1) / 2));
  }

  // Sets the wind in metres per second, a component left out being 0. False when one is not
  // finite or the wind blows harder than MAX_WIND.
  setWind(wind) {
    if (wind === null || typeof wind !== 'object') {
      return false;
    }
    const x = wind.x ?? 0;
    const y = wind.y ?? 0;
    const z = wind.z ?? 0;
    if (![x, y, z].every((part) => typeof part === 'number' && Number.isFinite(part))) {
      return false;
    }
    if (Math.hypot(x, y, z) > MAX_WIND) {
      return false;
    }
    this.wind = { x, y, z };
    return true;
  }

  get windSpeed() {
    return Math.hypot(this.wind.x, this.wind.y, this.wind.z);
  }

  windVector() {
    return new Vector3(this.wind.x, this.wind.y, this.wind.z);
  }
}

module.exports = { Weather, DEFAULT_PRESET, MAX_SNOW_AMOUNT, MAX_SNOW_FLAKES };
