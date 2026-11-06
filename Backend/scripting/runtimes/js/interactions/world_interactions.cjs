'use strict';

// Interactions on things in the world (a KCDS addition): World.addInteraction puts the
// game's own "press [key] to ..." hint on a level entity (by its name, e.g. a pillory's holder), on
// a player's or NPC's body (the target follows the body; its own player does not see
// it) or on a position. Several interactions on one target show their keys together; a player who aims at it within `radius` metres sees the hint, and using it runs
// the resource's handler(player, interaction) here. What is aimed at is the entity's drawn box,
// or for a target that draws none an upright cylinder of `targetRadius` and `targetHeight`
// standing on it; nothing solid may be in between. See kcd2s/integration/interactions.hpp.
//
// Each player's client gets its whole list ({ op: 'interactions', id, list }) when an interaction
// it sees is added, changed or removed, and again when its world gets ready. The client reports a
// use ({ op: 'interactionFired', id, interaction }); the handler runs only when that player still
// sees the interaction. A resource's interactions go when it stops.

const { positionOf } = require('../npcs/npc_values.cjs');

// The game controls a hint can be on (kInteractionKeys); the index travels.
const KEYS = Object.freeze([
  'primary_action',
  'attack_special',
  'torch',
  'companion_interaction',
  'distract',
  'jump',
  'script_action',
]);
const NAME_MAX = 255;
const TEXT_MAX_BYTES = 256;
const RADIUS_MIN = 0.5;
const RADIUS_MAX = 10;
const HOLD_MIN = 0.2;
const HOLD_MAX = 10;
// A player's client gets at most this many: the nearest to them of those they see. With more than
// that on the server the lists are picked again every NEAREST_MS as players move. 255 is all the
// INTERACTIONS message's u8 count holds (kMaxInteractions in kcd2s/integration/interactions.hpp).
const MAX_PER_PLAYER = 255;
const NEAREST_MS = 1000;
// What the player aims at when the target draws no box (a helper entity, a position): an upright
// cylinder standing on it.
const TARGET_MIN = 0.1;
const TARGET_RADIUS_MAX = 5;
const TARGET_HEIGHT_MAX = 10;
const OPTIONS = new Set([
  'entity',
  'position',
  'radius',
  'key',
  'text',
  'mode',
  'holdDuration',
  'targetRadius',
  'targetHeight',
]);

const encoder = new TextEncoder();
const state = Symbol('kcds.interaction');

function numberIn(value, min, max, what) {
  if (typeof value !== 'number' || !Number.isFinite(value) || value < min || value > max) {
    throw new RangeError(`${what} must be a number from ${min} to ${max}`);
  }
  return value;
}

function textOf(value) {
  if (typeof value !== 'string') {
    throw new TypeError('addInteraction text must be a string');
  }
  if (encoder.encode(value).length > TEXT_MAX_BYTES || /[\x00-\x1f\x7f]/.test(value)) {
    throw new RangeError(`addInteraction text must be one line of at most ${TEXT_MAX_BYTES} bytes`);
  }
  return value;
}

// The interaction's settings from addInteraction's options; throws for one it cannot use.
function settingsOf(options) {
  if (options === null || typeof options !== 'object') {
    throw new TypeError('addInteraction takes an options object');
  }
  for (const key of Object.keys(options)) {
    if (!OPTIONS.has(key)) {
      throw new TypeError(`unknown addInteraction option ${key}`);
    }
  }
  const { entity, position, radius = 2, key = 'primary_action', mode = 'press' } = options;
  const settings = { entity: '', body: 0, position: { x: 0, y: 0, z: 0 } };
  if ((entity === undefined) === (position === undefined)) {
    throw new TypeError('addInteraction takes an entity name or a position, one of them');
  }
  if (entity !== null && typeof entity === 'object') {
    // A player's, an NPC's or a horse's body (a horse).
    if (!Number.isInteger(entity.id) || entity.id < 1 || entity.id > 0xffffffff) {
      throw new TypeError(
        'addInteraction entity must be a name, a Player, an Npc, a Horse or a Prop',
      );
    }
    settings.body = entity.id;
  } else if (entity !== undefined) {
    const valid =
      typeof entity === 'string' &&
      entity.length > 0 &&
      entity.length <= NAME_MAX &&
      /^[\x20-\x7e]*$/.test(entity);
    if (!valid) {
      throw new TypeError(`addInteraction entity must be a name of 1 to ${NAME_MAX} characters`);
    }
    settings.entity = entity;
  } else {
    const at = positionOf(position, 'addInteraction');
    settings.position = { x: at.x, y: at.y, z: at.z };
  }
  settings.radius = numberIn(radius, RADIUS_MIN, RADIUS_MAX, 'addInteraction radius');
  if (!KEYS.includes(key)) {
    throw new RangeError(`addInteraction key must be one of ${KEYS.join(', ')}`);
  }
  settings.key = KEYS.indexOf(key);
  if (mode !== 'press' && mode !== 'hold') {
    throw new RangeError('addInteraction mode must be "press" or "hold"');
  }
  settings.hold = mode === 'hold';
  settings.holdSeconds = numberIn(
    options.holdDuration ?? 1,
    HOLD_MIN,
    HOLD_MAX,
    'addInteraction holdDuration',
  );
  settings.targetRadius = numberIn(
    options.targetRadius ?? 0.6,
    TARGET_MIN,
    TARGET_RADIUS_MAX,
    'addInteraction targetRadius',
  );
  settings.targetHeight = numberIn(
    options.targetHeight ?? 1.8,
    TARGET_MIN,
    TARGET_HEIGHT_MAX,
    'addInteraction targetHeight',
  );
  settings.text = textOf(options.text);
  return settings;
}

class WorldInteractions {
  constructor(host) {
    this.host = host;
    this.nextId = 1;
    this.interactions = new Map();
    // The list each player's client last got (as JSON), so an unchanged list is not sent again.
    this.sentLists = new WeakMap();
    // Picks the nearest again while there are more than a player can get.
    this.nearestTimer = null;
  }

  // Keeps the nearest pick running while more interactions exist than a player gets.
  watchNearest() {
    const needed = this.interactions.size > MAX_PER_PLAYER;
    if (needed && this.nearestTimer === null) {
      this.nearestTimer = setInterval(() => this.sync(), NEAREST_MS);
      this.nearestTimer.unref?.();
    } else if (!needed && this.nearestTimer !== null) {
      clearInterval(this.nearestTimer);
      this.nearestTimer = null;
    }
  }

  // Where an interaction is: its position, or the body it is on; null for a level entity, whose
  // place the server does not know.
  placeOf(handle) {
    const { body, entity, position } = handle[state];
    if (body) {
      const host = this.host;
      const owner = host.players.get(body) ?? host.horses.get(body) ?? host.npcs?.npcs?.get(body);
      if (owner) {
        return owner.position ?? null;
      }
      // A prop; one on a player's body is where that player is.
      const prop = host.props?.props?.get(body);
      if (prop && prop.body !== 0) {
        return host.players.get(prop.body)?.position ?? null;
      }
      if (prop) {
        return prop.position;
      }
      return null;
    }
    return entity ? null : position;
  }

  // World.addInteraction(options, handler): the handle of a new interaction.
  add(options, handler) {
    const owner = this.host.context.getStore();
    if (
      owner &&
      (this.host.resources.get(owner.name) !== owner || !owner.active || owner.failing)
    ) {
      throw new Error('Resource is stopped');
    }
    const settings = settingsOf(options);
    if (typeof handler !== 'function') {
      throw new TypeError('addInteraction takes a handler function');
    }
    const registry = this;
    const handle = Object.freeze({
      get id() {
        return this[state].id;
      },
      get exists() {
        return registry.interactions.get(this[state].id) === this;
      },
      get enabled() {
        return this[state].enabled;
      },
      // Shows or hides the hint for everyone who sees the interaction.
      setEnabled(enabled) {
        return registry.update(this, { enabled });
      },
      // Another text for the hint.
      setText(text) {
        return registry.update(this, { text: textOf(text) });
      },
      // Only these players see it (an array of players), or everyone again with null.
      setPlayers(players) {
        return registry.update(this, { players: registry.playersOf(players) });
      },
      remove() {
        registry.remove(this);
      },
      [state]: {
        id: this.nextId++,
        ...settings,
        enabled: true,
        players: null,
        handler,
        owner,
        resource: owner?.name,
      },
    });
    this.interactions.set(handle[state].id, handle);
    this.watchNearest();
    this.sync();
    return handle;
  }

  playersOf(players) {
    if (players === null) {
      return null;
    }
    if (!Array.isArray(players)) {
      throw new TypeError('setPlayers takes an array of players or null');
    }
    return new Set(players.map((player) => player.id));
  }

  update(handle, changes) {
    if (!this.interactions.has(handle[state].id)) {
      return false;
    }
    if (changes.enabled !== undefined && typeof changes.enabled !== 'boolean') {
      throw new TypeError('setEnabled takes true or false');
    }
    Object.assign(handle[state], changes);
    this.sync();
    return true;
  }

  remove(handle) {
    if (this.interactions.delete(handle[state].id)) {
      this.watchNearest();
      this.sync();
    }
  }

  visibleTo(handle, player) {
    const { enabled, players, hiddenFrom, body } = handle[state];
    // A body's own player never sees the hints on it.
    return (
      enabled &&
      body !== player.id &&
      (players === null || players.has(player.id)) &&
      !hiddenFrom?.has(player.id)
    );
  }

  // Hides the interaction from these players (their ids), whoever else sees it; for the server's
  // own hints (carts/carts.cjs: a cart's hints are not for those who ride it).
  hideFrom(handle, ids) {
    if (!this.interactions.has(handle[state].id)) {
      return false;
    }
    handle[state].hiddenFrom = new Set(ids);
    this.sync();
    return true;
  }

  // The interactions a player's client gets: all they see, or the MAX_PER_PLAYER nearest to them
  // when they see more (a level entity's, whose place the server does not know, after those it
  // does), in the order they were made.
  listFor(player) {
    let seen = [...this.interactions.values()].filter((handle) => this.visibleTo(handle, player));
    if (seen.length > MAX_PER_PLAYER) {
      const at = player.position;
      const distanceOf = (handle) => {
        const place = this.placeOf(handle);
        if (!place || !at) {
          return Infinity;
        }
        return Math.hypot(place.x - at.x, place.y - at.y, place.z - at.z);
      };
      const nearest = seen
        .map((handle) => ({ handle, distance: distanceOf(handle) }))
        .sort((a, b) => a.distance - b.distance || a.handle[state].id - b.handle[state].id)
        .slice(0, MAX_PER_PLAYER)
        .map(({ handle }) => handle);
      const kept = new Set(nearest);
      seen = seen.filter((handle) => kept.has(handle));
    }
    return seen.map((handle) => {
      const { id, entity, body, position, radius, key, hold, holdSeconds, text } = handle[state];
      const { targetRadius, targetHeight } = handle[state];
      return {
        id,
        entity,
        body,
        position,
        radius,
        key,
        hold,
        holdSeconds,
        targetRadius,
        targetHeight,
        text,
      };
    });
  }

  sendTo(player) {
    if (player.ready) {
      this.host.bridge.send({ op: 'interactions', id: player.id, list: this.listFor(player) });
    }
  }

  // Every ready player gets their list again; an unchanged list is not sent again.
  sync() {
    for (const player of this.host.players.values()) {
      if (!player.ready) {
        continue;
      }
      const text = JSON.stringify(this.listFor(player));
      if (this.sentLists.get(player) !== text) {
        this.sentLists.set(player, text);
        this.sendTo(player);
      }
    }
  }

  // The player's world is ready (again): their list goes to them.
  arrived(player) {
    this.sentLists.set(player, JSON.stringify(this.listFor(player)));
    if (this.interactions.size > 0) {
      this.sendTo(player);
    }
  }

  // interactionFired from the player's client.
  fired(player, interactionId) {
    const handle = this.interactions.get(interactionId);
    if (!player || !handle || !this.visibleTo(handle, player)) {
      return;
    }
    const { handler, owner, resource } = handle[state];
    try {
      const result = this.host.context.run(owner, () => handler(player, handle));
      if (result?.then) {
        result.catch((error) => this.host.bridge.log(resource, error.stack ?? String(error)));
      }
    } catch (error) {
      this.host.bridge.log(resource, error.stack ?? String(error));
    }
  }

  resourceStopped(name) {
    let removed = false;
    for (const [id, handle] of [...this.interactions]) {
      if (handle[state].resource === name) {
        this.interactions.delete(id);
        removed = true;
      }
    }
    if (removed) {
      this.watchNearest();
      this.sync();
    }
  }
}

module.exports = { WorldInteractions, KEYS };
