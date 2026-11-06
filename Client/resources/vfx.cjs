'use strict';

// The client-side Vfx: particle effects a client script plays in this game only, as
// the server's Vfx does for players near them (Backend/scripting/runtimes/js/vfx/vfx.cjs, the same
// checks). The game client applies them as it applies the server's (Client/runtime/vfx/
// local_effects.hpp):
//   { op: 'vfxLocal', kind: 'spawn', id, effect, position, rotation, prime, ...tuning }
//   { op: 'vfxLocal', kind: 'destroy', id }
//   { op: 'vfxLocal', kind: 'burst', effect, position, rotation, durationMs, prime, ...tuning }
//   { op: 'vfxLocal', kind: 'burstFrom', effect, entity, offset, rotation, durationMs, prime,
//     ...tuning }
// A placed effect keeps running until destroyed or its resource stops; its id comes from
// FIRST_ID up, a range the server never gives.

const {
  effectOf,
  tuning,
  pointOf,
  BURST_MS_DEFAULT,
  BURST_MS_MAX,
  OFFSET_MAX,
  jointOf,
} = require('../../Backend/scripting/runtimes/js/vfx/vfx_values.cjs');
const { rotation } = require('../../Backend/scripting/runtimes/js/math.cjs');
// The game's particle effects, the server's catalog.
const catalog = require('../../Backend/scripting/runtimes/js/vfx/catalog.json');

const FIRST_ID = 0xf0000000;
const LAST_ID = 0xffffffff;

const quat = (q) => ({ x: q.x, y: q.y, z: q.z, w: q.w });

function burstMs(options) {
  const ms = options.durationMs ?? BURST_MS_DEFAULT;
  if (typeof ms !== 'number' || !Number.isFinite(ms)) {
    throw new TypeError('Vfx durationMs must be a finite number');
  }
  return Math.min(BURST_MS_MAX, Math.max(0, ms));
}

function optionsOf(options) {
  if (options === undefined || options === null) {
    return {};
  }
  if (typeof options !== 'object') {
    throw new TypeError('Vfx options must be an object');
  }
  return options;
}

class ResourceVfx {
  constructor({ send = () => {} } = {}) {
    this.send = send;
    this.nextId = FIRST_ID;
    // id -> owner
    this.effects = new Map();
  }

  allocate() {
    const id = this.nextId;
    this.nextId = this.nextId === LAST_ID ? FIRST_ID : this.nextId + 1;
    return id;
  }

  // The Vfx global of one resource.
  facade(owner) {
    const registry = this;
    return Object.freeze({
      // Every effect name the game's particle libraries carry, the server's Vfx.list too (issue),
      // optionally only those starting with `prefix`.
      list(prefix) {
        if (prefix !== undefined && typeof prefix !== 'string') {
          throw new TypeError('Vfx.list prefix must be a string');
        }
        return catalog.effects.filter((name) => prefix === undefined || name.startsWith(prefix));
      },
      // Places an effect in this game; it runs until destroy() or the resource stops.
      spawn(effect, position, options) {
        const given = optionsOf(options);
        const id = registry.allocate();
        registry.send({
          op: 'vfxLocal',
          kind: 'spawn',
          id,
          effect: effectOf(effect),
          position: pointOf(position, 'effect position'),
          rotation: quat(rotation(given.rotation)),
          prime: given.prime === true,
          ...tuning(given),
        });
        registry.effects.set(id, owner);
        return Object.freeze({
          id,
          effect,
          destroy() {
            return registry.destroy(id);
          },
        });
      },
      // Hangs an effect on a player or NPC (a handle or its network id), where it follows them until
      // stop, durationMs or the resource stops.
      attach(effect, entity, options) {
        const given = optionsOf(options);
        const body = typeof entity === 'number' ? entity : entity?.id;
        if (!Number.isInteger(body) || body < 1 || body >= FIRST_ID) {
          throw new TypeError('Vfx.attach takes an entity or a network id of an entity');
        }
        const offset = pointOf(given.offset ?? { x: 0, y: 0, z: 0 }, 'effect offset');
        if (Math.hypot(offset.x, offset.y, offset.z) > OFFSET_MAX) {
          throw new RangeError(`Vfx offset must be within ${OFFSET_MAX} m of the entity`);
        }
        const { durationMs } = given;
        if (durationMs !== undefined && !(Number.isFinite(durationMs) && durationMs > 0)) {
          throw new RangeError('Vfx.attach durationMs must be above 0');
        }
        const id = registry.allocate();
        registry.send({
          op: 'vfxLocal',
          kind: 'spawn',
          id,
          effect: effectOf(effect),
          entity: body,
          joint: jointOf(given.joint),
          position: offset,
          rotation: quat(rotation(given.rotation)),
          prime: given.prime === true,
          ...tuning(given),
        });
        registry.effects.set(id, owner);
        if (durationMs !== undefined) {
          setTimeout(() => registry.destroy(id), durationMs);
        }
        return Object.freeze({
          id,
          effect,
          destroy() {
            return registry.destroy(id);
          },
        });
      },
      // Stops an effect of this resource by its handle or id; false when it is not playing.
      stop(effect) {
        const id = typeof effect === 'number' ? effect : effect?.id;
        return registry.effects.get(id) === owner && registry.destroy(id);
      },
      // Stops every effect this resource placed; how many it stopped.
      stopAll() {
        return registry.removeOwner(owner);
      },
      // Whether this resource's effect is still playing.
      isPlaying(effect) {
        const id = typeof effect === 'number' ? effect : effect?.id;
        return registry.effects.get(id) === owner;
      },
      // Whether the game has an effect of this name.
      has(effect) {
        return typeof effect === 'string' && catalog.effects.includes(effect);
      },
      // How many effects this resource has placed and not stopped.
      count() {
        return [...registry.effects.values()].filter((held) => held === owner).length;
      },
      // Plays an effect once at a point, in this game only.
      burst(effect, position, options) {
        const given = optionsOf(options);
        registry.send({
          op: 'vfxLocal',
          kind: 'burst',
          effect: effectOf(effect),
          position: pointOf(position, 'effect position'),
          rotation: quat(rotation(given.rotation)),
          durationMs: burstMs(given),
          prime: given.prime === true,
          ...tuning(given),
        });
        return true;
      },
      // Plays an effect once on a player or NPC (a handle or its network id), following it.
      burstFrom(effect, entity, options) {
        const given = optionsOf(options);
        const id = typeof entity === 'number' ? entity : entity?.id;
        if (!Number.isInteger(id) || id < 1 || id >= FIRST_ID) {
          throw new TypeError('Vfx.burstFrom takes an entity or a network id of an entity');
        }
        const offset = pointOf(given.offset ?? { x: 0, y: 0, z: 0 }, 'effect offset');
        if (Math.hypot(offset.x, offset.y, offset.z) > OFFSET_MAX) {
          throw new RangeError(`Vfx offset must be within ${OFFSET_MAX} m of the entity`);
        }
        const joint = jointOf(given.joint);
        registry.send({
          op: 'vfxLocal',
          kind: 'burstFrom',
          effect: effectOf(effect),
          entity: id,
          offset,
          joint,
          rotation: quat(rotation(given.rotation)),
          durationMs: burstMs(given),
          prime: given.prime === true,
          ...tuning(given),
        });
        return true;
      },
    });
  }

  destroy(id) {
    if (!this.effects.has(id)) {
      return false;
    }
    this.effects.delete(id);
    this.send({ op: 'vfxLocal', kind: 'destroy', id });
    return true;
  }

  // The resource stopped: its placed effects go with it.
  removeOwner(owner) {
    let removed = 0;
    for (const [id, effectOwner] of [...this.effects]) {
      if (effectOwner === owner) {
        this.destroy(id);
        removed += 1;
      }
    }
    return removed;
  }
}

module.exports = { ResourceVfx, FIRST_ID };
