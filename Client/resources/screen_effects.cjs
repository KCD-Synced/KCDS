'use strict';

// Hud screen effects: full-screen post effects on this player's own
// picture, owned by the resource that adds them.
//
// Every effect, its fades and its timer live here. Effects combine into one value per engine
// parameter (brightness, contrast and saturation multiply; the colour cast and the hue add;
// blur, ghosting and blood take the strongest), and only a changed set goes to the game client
// ({ op: 'screenFx', values }), which hands each value to the engine's own post effects. The
// game's own grade uses its own parameters, so these sit on top of it. Effects go when their
// resource stops, when the session ends (the runtime goes with it) and when the level changes.

const MAX_EFFECTS = 64;
const MAX_FADE_MS = 60000;
// Fades are sampled at about 30 Hz.
const TICK_MS = 33;

function param(name, description, min, max, defaultValue, neutral) {
  return { name, description, min, max, default: defaultValue, neutral };
}

const EFFECTS = [
  {
    name: 'colorGrade',
    description:
      'Re-grades the whole picture. Saturation, brightness and contrast multiply across effects; ' +
      'the colour cast and the hue shift add.',
    params: [
      param('saturation', 'Colour saturation; 0 is grey.', 0, 2, 1, 1),
      param('brightness', 'Overall brightness.', 0, 2, 1, 1),
      param('contrast', 'Contrast.', 0, 2, 1, 1),
      param('cyan', 'Cyan cast.', 0, 1, 0, 0),
      param('magenta', 'Magenta cast.', 0, 1, 0, 0),
      param('yellow', 'Yellow cast.', 0, 1, 0, 0),
      param('darkness', 'Darkens the picture towards black.', 0, 1, 0, 0),
      param('hue', 'Hue shift, times 180 degrees.', -1, 1, 0, 0),
    ],
  },
  {
    name: 'blur',
    description:
      'Blends the picture towards a blurred copy of itself. The strongest blur asked for is drawn.',
    params: [param('amount', 'How blurred.', 0, 1, 0.5, 0)],
  },
  {
    name: 'ghosting',
    description:
      'Earlier frames linger over the current one. The strongest ghosting asked for is drawn.',
    params: [param('amount', 'How long the trails linger.', 0, 1, 0.5, 0)],
  },
  {
    name: 'screenBlood',
    description:
      'The picture goes the red of blood and a little darker. The strongest blood asked for is ' +
      'drawn.',
    params: [param('amount', 'How much blood.', 0, 1, 0.5, 0)],
  },
];
const BY_NAME = new Map(EFFECTS.map((effect) => [effect.name, effect]));

function target(name, rule, scale = (value) => value) {
  return { name, rule, scale };
}

// The engine parameters each effect parameter drives, how they combine and the value each gets.
// The engine's own screen blood (ScreenBlood_Amount) draws nothing in this game build (every
// blood parameter tried), so screenBlood is a red cast (magenta and yellow) that also darkens.
const BLOOD_CAST = 0.35;
const BLOOD_DARKENING = 0.15;
const ENGINE = {
  'colorGrade.saturation': [target('Global_User_Saturation', 'multiply')],
  'colorGrade.brightness': [target('Global_User_Brightness', 'multiply')],
  'colorGrade.contrast': [target('Global_User_Contrast', 'multiply')],
  'colorGrade.cyan': [target('Global_User_ColorC', 'add')],
  'colorGrade.magenta': [target('Global_User_ColorM', 'add')],
  'colorGrade.yellow': [target('Global_User_ColorY', 'add')],
  'colorGrade.darkness': [target('Global_User_ColorK', 'add')],
  'colorGrade.hue': [target('Global_User_ColorHue', 'add')],
  'blur.amount': [target('FilterBlurring_Amount', 'max')],
  'ghosting.amount': [target('ImageGhosting_Amount', 'max')],
  'screenBlood.amount': [
    target('Global_User_ColorM', 'add', (value) => value * BLOOD_CAST),
    target('Global_User_ColorY', 'add', (value) => value * BLOOD_CAST),
    target('Global_User_Brightness', 'multiply', (value) => 1 - value * BLOOD_DARKENING),
  ],
};
// The range each engine parameter is clamped to once combined, and its neutral value.
const ENGINE_RANGE = {
  Global_User_Saturation: [0, 4, 1],
  Global_User_Brightness: [0, 4, 1],
  Global_User_Contrast: [0, 4, 1],
  Global_User_ColorC: [0, 1, 0],
  Global_User_ColorM: [0, 1, 0],
  Global_User_ColorY: [0, 1, 0],
  Global_User_ColorK: [0, 1, 0],
  Global_User_ColorHue: [-1, 1, 0],
  FilterBlurring_Amount: [0, 1, 0],
  ImageGhosting_Amount: [0, 1, 0],
};

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function milliseconds(options, key, fallback, limit) {
  const value = options[key];
  if (value === undefined) {
    return fallback;
  }
  if (typeof value !== 'number' || !Number.isFinite(value) || value < 0) {
    throw new TypeError(`Hud: ${key} must be a number of milliseconds, 0 or more`);
  }
  return limit === undefined ? value : Math.min(value, limit);
}

function optionsBag(value) {
  if (value === undefined) {
    return {};
  }
  if (value === null || typeof value !== 'object') {
    throw new TypeError('Hud: options must be an object');
  }
  return value;
}

// The effect's parameters from a params bag, clamped; base fills the ones left out.
function readParams(effect, params, base) {
  if (params !== undefined && (params === null || typeof params !== 'object')) {
    throw new TypeError('Hud: params must be an object');
  }
  const bag = params ?? {};
  for (const key of Object.keys(bag)) {
    if (!effect.params.some((entry) => entry.name === key)) {
      throw new RangeError(`Hud: ${effect.name} has no parameter ${key}`);
    }
  }
  const values = {};
  for (const entry of effect.params) {
    const value = bag[entry.name];
    if (value === undefined) {
      values[entry.name] = base[entry.name];
    } else if (typeof value !== 'number' || !Number.isFinite(value)) {
      throw new TypeError(`Hud: ${entry.name} must be a finite number`);
    } else {
      values[entry.name] = clamp(value, entry.min, entry.max);
    }
  }
  return values;
}

function neutralParams(effect) {
  return Object.fromEntries(effect.params.map((entry) => [entry.name, entry.neutral]));
}

function defaultParams(effect) {
  return Object.fromEntries(effect.params.map((entry) => [entry.name, entry.default]));
}

class ScreenEffects {
  constructor({ send, now = () => Date.now(), timers = globalThis }) {
    this.send = send;
    this.now = now;
    this.timers = timers;
    this.effects = new Map();
    this.nextHandle = 1;
    this.sent = null;
    this.timer = null;
  }

  // The parameters an effect shows at time.
  current(state, time) {
    const { fade } = state;
    if (!fade) {
      return state.values;
    }
    const progress = fade.ms === 0 ? 1 : clamp((time - fade.start) / fade.ms, 0, 1);
    const values = {};
    for (const key of Object.keys(fade.to)) {
      values[key] = fade.from[key] + (fade.to[key] - fade.from[key]) * progress;
    }
    return values;
  }

  startFade(state, to, ms, time) {
    state.fade = {
      from: this.current(state, time),
      to,
      start: time,
      ms: Math.min(ms, MAX_FADE_MS),
    };
    state.values = to;
  }

  // Ends finished fades and timers, drops faded-out effects, then sends the combined values.
  update() {
    const time = this.now();
    for (const [handle, state] of this.effects) {
      if (!state.leaving && state.endAt !== 0 && time >= state.endAt) {
        this.leave(state, state.fadeOutMs, time);
      }
      if (state.fade && time >= state.fade.start + state.fade.ms) {
        state.fade = null;
        if (state.leaving) {
          this.effects.delete(handle);
        }
      }
    }
    this.publish(time);
    this.schedule();
  }

  schedule() {
    const busy = [...this.effects.values()].some(
      (state) => state.fade || (!state.leaving && state.endAt !== 0),
    );
    if (busy && this.timer === null) {
      this.timer = this.timers.setTimeout(() => {
        this.timer = null;
        this.update();
      }, TICK_MS);
    }
  }

  publish(time) {
    const values = {};
    for (const [name, [, , neutral]] of Object.entries(ENGINE_RANGE)) {
      values[name] = neutral;
    }
    for (const state of this.effects.values()) {
      const shown = this.current(state, time);
      for (const [key, value] of Object.entries(shown)) {
        for (const { name, rule, scale } of ENGINE[`${state.effect.name}.${key}`]) {
          const scaled = scale(value);
          if (rule === 'multiply') {
            values[name] *= scaled;
          } else if (rule === 'add') {
            values[name] += scaled;
          } else {
            values[name] = Math.max(values[name], scaled);
          }
        }
      }
    }
    for (const [name, [min, max]] of Object.entries(ENGINE_RANGE)) {
      values[name] = Math.round(clamp(values[name], min, max) * 10000) / 10000;
    }
    const changed =
      this.sent === null || Object.keys(values).some((name) => values[name] !== this.sent[name]);
    if (changed) {
      this.sent = values;
      this.send({ op: 'screenFx', values });
    }
  }

  leave(state, fadeOutMs, time) {
    state.leaving = true;
    this.startFade(state, neutralParams(state.effect), fadeOutMs, time);
  }

  owned(owner, handle) {
    const state = this.effects.get(handle);
    return state && state.owner === owner ? state : null;
  }

  add(owner, name, params, options) {
    const effect = BY_NAME.get(name);
    if (!effect) {
      throw new RangeError(`Hud: unknown screen effect ${name}`);
    }
    const values = readParams(effect, params, defaultParams(effect));
    const bag = optionsBag(options);
    const fadeInMs = milliseconds(bag, 'fadeInMs', 0, MAX_FADE_MS);
    const fadeOutMs = milliseconds(bag, 'fadeOutMs', 0, MAX_FADE_MS);
    const durationMs = milliseconds(bag, 'durationMs', 0);
    if (this.effects.size >= MAX_EFFECTS) {
      return 0;
    }
    const time = this.now();
    const handle = this.nextHandle++;
    const state = {
      owner,
      effect,
      values: neutralParams(effect),
      fade: null,
      fadeOutMs,
      endAt: durationMs > 0 ? time + durationMs : 0,
      leaving: false,
    };
    this.startFade(state, values, fadeInMs, time);
    this.effects.set(handle, state);
    this.update();
    return handle;
  }

  change(owner, handle, params, options) {
    const state = this.owned(owner, handle);
    if (!state) {
      return false;
    }
    const time = this.now();
    const fadeMs = milliseconds(optionsBag(options), 'fadeMs', 0, MAX_FADE_MS);
    // Unchanged parameters keep the value they are heading to.
    const values = readParams(state.effect, params, state.values);
    if (!state.leaving) {
      this.startFade(state, values, fadeMs, time);
      this.update();
    }
    return true;
  }

  remove(owner, handle, options) {
    const state = this.owned(owner, handle);
    if (!state) {
      return false;
    }
    const bag = optionsBag(options);
    // A running fade-out continues from its current value.
    if (!state.leaving || bag.fadeOutMs !== undefined) {
      this.leave(state, milliseconds(bag, 'fadeOutMs', state.fadeOutMs, MAX_FADE_MS), this.now());
    }
    this.update();
    return true;
  }

  removeAll(owner) {
    let removed = 0;
    for (const [handle, state] of this.effects) {
      if (state.owner === owner) {
        this.effects.delete(handle);
        removed += 1;
      }
    }
    if (removed > 0) {
      this.update();
    }
    return removed;
  }

  isActive(handle) {
    return this.effects.has(handle);
  }

  // The level changed or the resource stopped: effects go at once.
  clear() {
    if (this.effects.size === 0) {
      return;
    }
    this.effects.clear();
    this.update();
  }

  list() {
    return EFFECTS.map((effect) => ({
      name: effect.name,
      description: effect.description,
      params: effect.params.map((entry) => ({ ...entry })),
    }));
  }
}

module.exports = { ScreenEffects, EFFECTS, ENGINE, MAX_EFFECTS };
