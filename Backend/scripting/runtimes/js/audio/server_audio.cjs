'use strict';

// The server API's Audio: one of the game's sound triggers played for every player near a point
// or a body, and the catalog of the game's trigger names.
//
// A sound is sent to each player in the dimension within `range` metres as the built-in client
// event 'kcds:audio' (scripts never see it), whose client runtime plays it as Audio.playAt or
// Audio.playFrom do on that machine:
//   { trigger, at: { x, y, z } }             a free sound at the point
//   { trigger, entity, at: { x, y, z } }     a sound from the body of that network entity, or at
//                                            the point where its game has no such body
// Nothing is kept: a sound is told once, and a player who comes near later does not hear it.

const catalog = require('./catalog.json');
const { GLOBAL_WORLD, pointOf, worldOf } = require('../vfx/vfx_values.cjs');

const AUDIO_EVENT = 'kcds:audio';
const RANGE_DEFAULT = 30;
const RANGE_MAX = 200;
const TRIGGER_NAME = /^[A-Za-z0-9_]{1,128}$/;

function triggerOf(trigger) {
  if (typeof trigger !== 'string' || !TRIGGER_NAME.test(trigger)) {
    throw new TypeError('Audio trigger must be a sound trigger name (letters, digits and _)');
  }
  return trigger;
}

function rangeOf(options) {
  const range = options?.range ?? RANGE_DEFAULT;
  if (typeof range !== 'number' || !Number.isFinite(range) || range <= 0 || range > RANGE_MAX) {
    throw new RangeError(`Audio range must be more than 0 and at most ${RANGE_MAX} m`);
  }
  return range;
}

function optionsOf(options) {
  if (options !== undefined && (options === null || typeof options !== 'object')) {
    throw new TypeError('Audio options must be an object');
  }
  return options ?? {};
}

class ServerAudio {
  // `host`: the resource host (its players, the clients whose scripts run, its services' events to
  // a player's client, and its API for entities).
  constructor(host) {
    this.host = host;
  }

  // The players in `world` within `range` of `at` whose client scripts run (a sound is not held
  // for a client that starts later: it would play long after).
  listeners(at, world, range) {
    const ids = [];
    for (const player of this.host.players.values()) {
      if (player.dimension !== world || !this.host.clientResourcesRunning.has(player.id)) {
        continue;
      }
      const p = player.position;
      if (Math.hypot(p.x - at.x, p.y - at.y, p.z - at.z) <= range) {
        ids.push(player.id);
      }
    }
    return ids;
  }

  // Whether any player was told (a boolean, as the client's Audio.playAt gives, so one C#
  // signature serves both sides).
  tell(ids, payload) {
    for (const id of ids) {
      this.host.services.emitToPlayer(id, AUDIO_EVENT, payload);
    }
    return ids.length > 0;
  }

  // A free sound at a point for the players near it; whether any was told.
  playAt(trigger, position, options) {
    const name = triggerOf(trigger);
    const settings = optionsOf(options);
    const at = pointOf(position, 'sound position');
    const world = worldOf(settings.dimension);
    return this.tell(this.listeners(at, world, rangeOf(settings)), { trigger: name, at });
  }

  // A sound from an entity's body (a Player, an Npc, a Horse or its network id) for the players
  // near it, in its dimension; whether any was told.
  playFrom(trigger, entity, options) {
    const name = triggerOf(trigger);
    const settings = optionsOf(options);
    const id = typeof entity === 'number' ? entity : entity?.id;
    if (!Number.isInteger(id) || id < 1) {
      throw new TypeError('Audio entity must be a Player, an Npc, a Horse or a network id');
    }
    const api = this.host.api ?? this.host.makeAPI();
    const target = new api.Entity(id);
    const at = pointOf(target.position, 'entity position');
    return this.tell(this.listeners(at, target.dimension, rangeOf(settings)), {
      trigger: name,
      entity: id,
      at,
    });
  }

  // The game's sound trigger names, optionally only those starting with `prefix`, sorted.
  list(prefix) {
    if (prefix !== undefined && typeof prefix !== 'string') {
      throw new TypeError('Audio.list prefix must be a string');
    }
    return catalog.triggers.filter((name) => prefix === undefined || name.startsWith(prefix));
  }

  api() {
    return Object.freeze({
      playAt: (trigger, position, options) => this.playAt(trigger, position, options),
      playFrom: (trigger, entity, options) => this.playFrom(trigger, entity, options),
      list: (prefix) => this.list(prefix),
    });
  }
}

module.exports = { ServerAudio, AUDIO_EVENT, RANGE_DEFAULT, RANGE_MAX, GLOBAL_WORLD };
