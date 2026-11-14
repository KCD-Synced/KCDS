'use strict';

// The client-only ActionHint global: "Press [key] to ..." hints
// drawn by the game's own HUD, on one of the game's controls.
//
// Hints live here, per resource. Several hints on one control stack: only the newest is shown
// and receives input; removing it reveals the previous one. A press hint and a hold hint
// stack apart: the newest of each is shown, and then a short press fires the press hint and a
// full hold the hold hint, as the game's own prompts do. The game client gets the
// hints to show, one per control ({ op: 'actionHints', hints: [...] }), draws them while the
// player controls their character, and reports a press, or a filled hold ring
// ({ op: 'actionHintFired', id }); the handler of a hint that is still there and enabled runs
// with its id.
//
// The game client also tells what the HUD draws, every hint whoever put it up (the game's own,
// interaction prompts, every resource's), when that changes ({ op: 'actionHintsShown', hints:
// [{ key, map, text, mode, enabled, progress, target }] }): ActionHint.shown, shownKeys and
// isKeyInUse read it, and actionHintsChanged is raised.

// Hint targets, in a set order: the game's controls, script_action, KCDS's own
// free key for scripts (U), and mouse_wheel, which only shows the game's mouse-wheel icon and
// never fires. An enabled hint takes its key's press: the game's own action on that key does not
// run meanwhile.
const KEYS = Object.freeze([
  'primary_action',
  'attack_special',
  'torch',
  'companion_interaction',
  'distract',
  'jump',
  'script_action',
  'mouse_wheel',
]);
const MAX_TEXT_BYTES = 256;
const MIN_HOLD = 0.2;
const MAX_HOLD = 10;

function byteLength(text) {
  return new TextEncoder().encode(text).length;
}

function hintText(text) {
  if (typeof text !== 'string') {
    throw new TypeError('ActionHint: text must be a string');
  }
  if (byteLength(text) > MAX_TEXT_BYTES) {
    throw new RangeError(`ActionHint: text is longer than ${MAX_TEXT_BYTES} bytes`);
  }
  return text;
}

function readOptions(options) {
  if (options === null || typeof options !== 'object') {
    throw new TypeError('ActionHint: options must be an object');
  }
  const { key, text, mode = 'press', holdDuration = 1, enabled = true } = options;
  if (!KEYS.includes(key)) {
    throw new RangeError(`ActionHint: key must be one of ${KEYS.join(', ')}`);
  }
  if (mode !== 'press' && mode !== 'hold') {
    throw new RangeError('ActionHint: mode must be "press" or "hold"');
  }
  if (typeof holdDuration !== 'number' || !Number.isFinite(holdDuration)) {
    throw new TypeError('ActionHint: holdDuration must be a number of seconds');
  }
  if (holdDuration < MIN_HOLD || holdDuration > MAX_HOLD) {
    throw new RangeError(`ActionHint: holdDuration must be ${MIN_HOLD} to ${MAX_HOLD} seconds`);
  }
  if (typeof enabled !== 'boolean') {
    throw new TypeError('ActionHint: enabled must be a boolean');
  }
  return { key, text: hintText(text), mode, holdDuration, enabled };
}

class ResourceActionHints {
  constructor({ send, log, emit = () => {} }) {
    this.send = send;
    this.log = log;
    this.emit = emit;
    // Every hint the HUD draws, as the game client last told it (frozen entries).
    this.drawn = Object.freeze([]);
    // id -> { owner, key, text, mode, holdDuration, enabled, handler }, in creation order.
    this.hints = new Map();
    this.nextId = 1;
    // Nothing is shown until a hint is created.
    this.published = '[]';
  }

  // The newest press hint and the newest hold hint on each control, as the game client draws
  // them.
  shown() {
    const top = new Map();
    for (const [id, hint] of this.hints) {
      top.set(`${hint.key}/${hint.mode}`, { id, hint });
    }
    const out = [];
    for (const key of KEYS) {
      for (const mode of ['press', 'hold']) {
        const entry = top.get(`${key}/${mode}`);
        if (!entry) {
          continue;
        }
        const { id, hint } = entry;
        out.push({
          id,
          key,
          text: hint.text,
          mode: hint.mode,
          holdDuration: hint.holdDuration,
          enabled: hint.enabled,
        });
      }
    }
    return out;
  }

  publish() {
    const hints = this.shown();
    const text = JSON.stringify(hints);
    if (text !== this.published) {
      this.published = text;
      this.send({ op: 'actionHints', hints });
    }
  }

  // The game client reports a press or a filled hold of a shown hint.
  receiveFired({ id }) {
    const hint = this.hints.get(id);
    if (!hint || !hint.enabled) {
      return;
    }
    try {
      const result = hint.handler(id);
      if (result && typeof result.then === 'function') {
        result.then(undefined, (error) => this.log(hint.owner, String(error)));
      }
    } catch (error) {
      this.log(hint.owner, String(error));
    }
  }

  // What the HUD draws now: { hints: [{ key, map, text, mode, enabled, progress, target }] }.
  receiveShown({ hints }) {
    const entries = Array.isArray(hints) ? hints : [];
    this.drawn = Object.freeze(
      entries.map((hint) =>
        Object.freeze({
          key: String(hint.key ?? ''),
          map: String(hint.map ?? ''),
          text: String(hint.text ?? ''),
          mode: hint.mode === 'hold' ? 'hold' : 'press',
          enabled: hint.enabled !== false,
          progress: typeof hint.progress === 'number' ? hint.progress : 0,
          target: String(hint.target ?? ''),
        }),
      ),
    );
    this.emit('actionHintsChanged', []);
  }

  // The resource stopped: its hints go.
  removeOwner(owner) {
    for (const [id, hint] of this.hints) {
      if (hint.owner === owner) {
        this.hints.delete(id);
      }
    }
    this.publish();
  }

  facade(owner) {
    const hints = this;
    const own = (id) => {
      const hint = hints.hints.get(id);
      return hint && hint.owner === owner ? hint : null;
    };
    return Object.freeze({
      create(options, handler) {
        const hint = readOptions(options);
        if (typeof handler !== 'function') {
          throw new TypeError('ActionHint: handler must be a function');
        }
        const id = hints.nextId++;
        hints.hints.set(id, { owner, handler, ...hint });
        hints.publish();
        return id;
      },
      setText(id, text) {
        const hint = own(id);
        if (!hint || typeof text !== 'string' || byteLength(text) > MAX_TEXT_BYTES) {
          return false;
        }
        hint.text = text;
        hints.publish();
        return true;
      },
      setEnabled(id, enabled) {
        const hint = own(id);
        if (!hint) {
          return false;
        }
        hint.enabled = Boolean(enabled);
        hints.publish();
        return true;
      },
      remove(id) {
        if (!own(id)) {
          return false;
        }
        hints.hints.delete(id);
        hints.publish();
        return true;
      },
      keys() {
        return [...KEYS];
      },
      // Every hint the HUD draws now, the game's own and interaction prompts included, in the
      // HUD's order: { key, map, text, mode, enabled, progress, target } each.
      shown() {
        return hints.drawn.map((hint) => ({ ...hint }));
      },
      // Every control a drawn hint is on now, each once.
      shownKeys() {
        return [...new Set(hints.drawn.map((hint) => hint.key))];
      },
      // True while one of the game's controls has a visible hint.
      isKeyInUse(key) {
        if (typeof key !== 'string') {
          throw new TypeError('ActionHint.isKeyInUse takes a control name');
        }
        return hints.drawn.some((hint) => hint.key === key);
      },
    });
  }
}

module.exports = { ResourceActionHints, KEYS };
