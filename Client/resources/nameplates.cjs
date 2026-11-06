'use strict';

// The client-only Nameplates global: what this player's own game draws of the nameplates
// above players, and the short lines resources hang above them. Nothing here is replicated; what
// the server says about a player's tag (Player.setNameplate*) comes with the player itself.
//
//   { op: 'nameplateView', visible, health, name, self }
//   { op: 'nameplateLabel', entity, text, durationMs, color }   text '' takes the label down
//   { op: 'nameplateLabelsClear' }
//
// The game client (Client/runtime/nameplates/nameplates.cpp) draws them. A label belongs to the
// resource that set it: a stopped resource takes its labels down.

const MAX_LABEL_BYTES = 256;
const DEFAULT_LABEL_MS = 6000;
const MAX_LABEL_MS = 3600000;
// What this player's game draws of every tag (setMode): the name and the health bar, one of them,
// or neither.
const MODES = {
  both: { name: true, health: true },
  name: { name: true, health: false },
  health: { name: false, health: true },
  none: { name: false, health: false },
};

const encoder = new TextEncoder();

function fail(message) {
  throw new TypeError(`Nameplates: ${message}`);
}

function flag(value, name) {
  if (typeof value !== 'boolean') {
    fail(`${name} takes a boolean`);
  }
  return value;
}

function entityOf(entityId) {
  if (!Number.isInteger(entityId) || entityId < 1 || entityId > 0xffffffff) {
    fail('an entity id is a positive whole number');
  }
  return entityId;
}

class ResourceNameplates {
  constructor({ send }) {
    this.send = send;
    this.view = { visible: true, health: true, name: true, self: false };
    // entity id -> the resource whose label it carries
    this.labels = new Map();
  }

  sendView() {
    const view = { ...this.view };
    if (this.suppressed) {
      view.visible = false;
    }
    this.send({ op: 'nameplateView', ...view });
  }

  // Hides every nameplate while the game's interface is put away (World Architect's editor),
  // without changing what the resources set.
  suppress(hidden) {
    this.suppressed = hidden;
    this.sendView();
  }

  setLabel(owner, entityId, text, durationMs = DEFAULT_LABEL_MS, color = 0) {
    const entity = entityOf(entityId);
    if (typeof text !== 'string') {
      fail('setLabel takes a text');
    }
    if (encoder.encode(text).length > MAX_LABEL_BYTES) {
      fail(`a label is at most ${MAX_LABEL_BYTES} UTF-8 bytes`);
    }
    if (typeof durationMs !== 'number' || !Number.isFinite(durationMs)) {
      fail('durationMs is a number of milliseconds');
    }
    if (!Number.isInteger(color) || color < 0 || color > 0xffffffff) {
      fail('a colour is packed 0xAARRGGBB');
    }
    const duration = durationMs <= 0 ? 0 : Math.min(Math.round(durationMs), MAX_LABEL_MS);
    if (text === '') {
      this.labels.delete(entity);
    } else {
      this.labels.set(entity, owner);
    }
    this.send({ op: 'nameplateLabel', entity, text, durationMs: duration, color: color >>> 0 });
  }

  clearLabel(entityId) {
    const entity = entityOf(entityId);
    this.labels.delete(entity);
    this.send({ op: 'nameplateLabel', entity, text: '', durationMs: 0, color: 0 });
  }

  clearLabels() {
    this.labels.clear();
    this.send({ op: 'nameplateLabelsClear' });
  }

  removeOwner(owner) {
    for (const [entity, labelOwner] of [...this.labels]) {
      if (labelOwner === owner) {
        this.clearLabel(entity);
      }
    }
  }

  facade(owner) {
    return Object.freeze({
      setVisible: (visible) => {
        this.view.visible = flag(visible, 'setVisible');
        this.sendView();
      },
      isVisible: () => this.view.visible,
      setHealthVisible: (visible) => {
        this.view.health = flag(visible, 'setHealthVisible');
        this.sendView();
      },
      isHealthVisible: () => this.view.health,
      // The names (a KCDS addition); with the health bars alone each bar takes its name's place.
      setNameVisible: (visible) => {
        this.view.name = flag(visible, 'setNameVisible');
        this.sendView();
      },
      isNameVisible: () => this.view.name,
      // 'both' (the default), 'name', 'health' or 'none' (a KCDS addition): the name and health
      // switches together.
      setMode: (mode) => {
        if (!Object.hasOwn(MODES, mode)) {
          fail(`setMode takes one of ${Object.keys(MODES).join(', ')}`);
        }
        this.view.name = MODES[mode].name;
        this.view.health = MODES[mode].health;
        this.sendView();
      },
      getMode: () =>
        Object.keys(MODES).find(
          (mode) => MODES[mode].name === this.view.name && MODES[mode].health === this.view.health,
        ),
      setSelfVisible: (visible) => {
        this.view.self = flag(visible, 'setSelfVisible');
        this.sendView();
      },
      isSelfVisible: () => this.view.self,
      setLabel: (entityId, text, durationMs, color) =>
        this.setLabel(owner, entityId, text, durationMs, color),
      clearLabel: (entityId) => this.clearLabel(entityId),
      clearLabels: () => this.clearLabels(),
    });
  }
}

module.exports = { ResourceNameplates, MAX_LABEL_BYTES, DEFAULT_LABEL_MS };
