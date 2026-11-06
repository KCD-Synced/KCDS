'use strict';

// Particle effects the server puts in the world: the server API's Vfx. A placed effect
// is a replicated entity the clients in view build and keep running until it is destroyed; a burst
// is played once for the players near the point and keeps nothing. The server holds the name, pose,
// tuning and dimension.
//
// Display goes through these bridge ops, sent only when something changes (no timers at all: a
// burst's end is the client's own, from `durationMs`):
//   { op: 'vfxSpawn', id, effect, position, rotation, dimension, scale, countScale, speedScale,
//     timeScale, strength, pulsePeriod, prime, body, joint }
//     (body: the player or NPC it hangs on, 0 for none; position is then the offset
//     from it or from its joint)
//   { op: 'vfxUpdate', id, scale, countScale, speedScale, timeScale, strength, pulsePeriod }
//     (the client rebuilds the emitter with the new values, so the effect restarts)
//   { op: 'vfxDestroy', id }
//   { op: 'vfxBurst', effect, position, rotation, dimension, players, durationMs, prime, ...tuning }
//   { op: 'vfxBurstOn', effect, entity, offset, rotation, players, durationMs, prime, ...tuning }
// `rotation` is a quaternion { x, y, z, w }; `players` lists the IDs that should play a burst.

const catalog = require('./catalog.json');
const { OWNER, runningResource, removeOwned } = require('../lifecycle/owned_objects.cjs');
const { Quaternion, Vector3, rotation } = require('../math.cjs');

const {
  GLOBAL_WORLD,
  BURST_RANGE,
  BURST_MS_DEFAULT,
  BURST_MS_MAX,
  OFFSET_MAX,
  TUNING,
  jointOf,
  validWorld,
  effectOf,
  worldOf,
  tuned,
  tuning,
  pointOf,
} = require('./vfx_values.cjs');

const quat = (q) => ({ x: q.x, y: q.y, z: q.z, w: q.w });

// The body an effect hangs on: a Player or an Npc (its network id), 0 for none.
function bodyOf(entity) {
  if (entity === undefined || entity === null) {
    return 0;
  }
  const id = typeof entity === 'number' ? entity : entity.id;
  if (!Number.isInteger(id) || id < 1 || id > 0xffffffff) {
    throw new TypeError('Vfx entity must be a Player or an Npc');
  }
  return id;
}

class Vfxs {
  // `host`: the resource host (bridge, emit, allocateEntityId, the running resource).
  constructor(host) {
    this.host = host;
    // id -> Vfx
    this.effects = new Map();
  }

  checkOwner() {
    const owner = this.host.context.getStore();
    if (
      owner &&
      (this.host.resources.get(owner.name) !== owner || !owner.active || owner.failing)
    ) {
      throw new Error('Resource is stopped');
    }
  }

  // The Vfx class of the server API, an Entity.
  define(Entity) {
    const registry = this;
    registry.Entity = Entity;
    class Vfx {
      constructor(id) {
        const vfx = registry.effects.get(id);
        if (!vfx) {
          throw new Error('Unknown effect');
        }
        return vfx;
      }
      static spawn(effect, position, options, dimension) {
        return registry.spawn(Vfx, { effect, position, options: options ?? {}, dimension });
      }
      static burst(effect, position, options, dimension) {
        return registry.burst(effect, position, options ?? {}, dimension);
      }
      static burstFrom(effect, entity, options) {
        return registry.burstFrom(effect, entity, options ?? {});
      }
      static all(dimension) {
        if (dimension !== undefined && !validWorld(dimension)) {
          throw new TypeError('Invalid dimension');
        }
        return [...registry.effects.values()].filter(
          (vfx) => dimension === undefined || vfx.dimension === dimension,
        );
      }
      static getById(id) {
        return registry.effects.get(id) ?? null;
      }
      static destroyAll(dimension) {
        let removed = 0;
        for (const vfx of Vfx.all(dimension)) {
          registry.destroy(vfx);
          removed += 1;
        }
        return removed;
      }
      // Every effect name the catalog carries, optionally only those starting with `prefix`.
      static list(prefix) {
        if (prefix !== undefined && typeof prefix !== 'string') {
          throw new TypeError('Vfx.list prefix must be a string');
        }
        return catalog.effects.filter((name) => prefix === undefined || name.startsWith(prefix));
      }
      get library() {
        return this.effect.split('.')[0];
      }
      get group() {
        return this.effect.split('.')[1];
      }
      get position() {
        return new Vector3(this.at.x, this.at.y, this.at.z);
      }
      get rotation() {
        return new Quaternion(this.facing.w, this.facing.x, this.facing.y, this.facing.z);
      }
      destroy() {
        registry.destroy(this);
      }
      toString() {
        return `Vfx(${this.id}, ${this.effect}, ${this.scale})`;
      }
    }
    // Each tuning value is a getter and a setter that restarts the effect when it changes.
    for (const name of Object.keys(TUNING)) {
      Object.defineProperty(Vfx.prototype, name, {
        get() {
          return this.tune[name];
        },
        set(value) {
          registry.update(this, name, tuned(name, value));
        },
      });
    }
    Object.setPrototypeOf(Vfx.prototype, Entity.prototype);
    return Vfx;
  }

  spawn(Vfx, { effect, position = {}, options, dimension }) {
    this.checkOwner();
    const name = effectOf(effect);
    const world = worldOf(dimension);
    const at = pointOf(position, 'effect position');
    // On a player's or NPC's body: the position is the offset from it or its joint.
    const body = bodyOf(options.entity);
    const joint = body === 0 ? '' : jointOf(options.joint);
    if (body === 0 && options.joint !== undefined) {
      throw new TypeError('Vfx joint needs an entity');
    }
    if (body !== 0 && Math.hypot(at.x, at.y, at.z) > OFFSET_MAX) {
      throw new RangeError(`Vfx offset must be within ${OFFSET_MAX} m of the entity`);
    }
    const facing = rotation(options.rotation);
    const tune = tuning(options);
    const vfx = Object.create(Vfx.prototype);
    Object.defineProperty(vfx, 'id', { value: this.host.allocateEntityId(), enumerable: true });
    Object.defineProperty(vfx, 'effect', { value: name, enumerable: true });
    Object.defineProperty(vfx, 'dimension', { value: world, enumerable: true });
    Object.defineProperty(vfx, 'at', { value: at });
    Object.defineProperty(vfx, 'facing', { value: quat(facing) });
    // The changeable state: assigned through the tuning setters.
    Object.defineProperty(vfx, 'tune', { value: tune });
    Object.defineProperty(vfx, 'primed', { value: options.prime === true });
    Object.defineProperty(vfx, 'body', { value: body });
    Object.defineProperty(vfx, 'joint', { value: joint, enumerable: true });
    vfx[OWNER] = runningResource(this.host);
    this.effects.set(vfx.id, vfx);
    this.host.bridge.send(this.spawnMessage(vfx));
    this.host.emit('vfxSpawn', vfx);
    return vfx;
  }

  // The vfxSpawn op for an effect as it stands now.
  spawnMessage(vfx) {
    return {
      op: 'vfxSpawn',
      id: vfx.id,
      effect: vfx.effect,
      position: vfx.at,
      rotation: vfx.facing,
      dimension: vfx.dimension,
      ...vfx.tune,
      prime: vfx.primed,
      body: vfx.body,
      joint: vfx.joint,
    };
  }

  // The effects hung on a player who left go with them.
  leaving(id) {
    for (const vfx of [...this.effects.values()]) {
      if (vfx.body === id) {
        this.destroy(vfx);
      }
    }
  }

  // A player's world became ready: their game lost the effects, so it is told of each again. The
  // op names the player, so nobody else hears it.
  arrived(player) {
    for (const vfx of this.effects.values()) {
      this.host.bridge.send({ ...this.spawnMessage(vfx), to: player.id });
    }
  }

  // Changes one tuning value and has every client rebuild the emitter; nothing is sent when the
  // value stays.
  update(vfx, name, value) {
    if (this.effects.get(vfx.id) !== vfx || vfx.tune[name] === value) {
      return;
    }
    vfx.tune[name] = value;
    this.host.bridge.send({ op: 'vfxUpdate', id: vfx.id, ...vfx.tune });
  }

  // The effects a stopping resource made go with it.
  resourceStopped(name) {
    removeOwned(this.effects.values(), name, (vfx) => this.destroy(vfx));
  }

  destroy(vfx) {
    if (this.effects.get(vfx.id) !== vfx) {
      return;
    }
    this.host.emit('vfxDestroy', vfx);
    this.effects.delete(vfx.id);
    this.host.bridge.send({ op: 'vfxDestroy', id: vfx.id });
  }

  // IDs of the players in `world` within the draw range of `at`.
  audience(at, world) {
    const ids = [];
    for (const player of this.host.players.values()) {
      if (player.dimension !== world) continue;
      const p = player.position;
      if (Math.hypot(p.x - at.x, p.y - at.y, p.z - at.z) <= BURST_RANGE) ids.push(player.id);
    }
    return ids;
  }

  burstMs(options) {
    const ms = options.durationMs ?? BURST_MS_DEFAULT;
    if (typeof ms !== 'number' || !Number.isFinite(ms)) {
      throw new TypeError('Vfx durationMs must be a finite number');
    }
    return Math.min(BURST_MS_MAX, Math.max(0, ms));
  }

  // Plays an effect once at a point for the players who can see it; returns how many were told.
  burst(effect, position, options, dimension) {
    this.checkOwner();
    const name = effectOf(effect);
    const world = worldOf(dimension);
    const at = pointOf(position, 'effect position');
    const message = {
      op: 'vfxBurst',
      effect: name,
      position: at,
      rotation: quat(rotation(options.rotation)),
      dimension: world,
      players: this.audience(at, world),
      durationMs: this.burstMs(options),
      prime: options.prime === true,
      ...tuning(options),
    };
    if (message.players.length > 0) this.host.bridge.send(message);
    return message.players.length;
  }

  // A single burst that moves with a replicated entity; gives the number of players told.
  burstFrom(effect, entity, options) {
    this.checkOwner();
    const name = effectOf(effect);
    const id = typeof entity === 'number' ? entity : entity?.id;
    const target = new this.Entity(id);
    const offset = pointOf(options.offset, 'effect offset');
    if (Math.hypot(offset.x, offset.y, offset.z) > OFFSET_MAX) {
      throw new RangeError(`Vfx offset must be within ${OFFSET_MAX} m of the entity`);
    }
    const joint = jointOf(options.joint);
    const players = this.audience(target.position, target.dimension);
    if (players.length > 0) {
      this.host.bridge.send({
        op: 'vfxBurstOn',
        effect: name,
        entity: id,
        offset,
        joint,
        rotation: quat(rotation(options.rotation)),
        players,
        durationMs: this.burstMs(options),
        prime: options.prime === true,
        ...tuning(options),
      });
    }
    return players.length;
  }

  // A burst on a player's body at a joint for the players who can see it, as burstFrom plays one,
  // for the server's own timers (a studio animation's events): no resource owns it.
  // `effect` was checked when the animation was defined.
  burstOnBody(effect, player, joint, scale = 1) {
    const players = this.audience(player.position, player.dimension);
    if (players.length > 0) {
      this.host.bridge.send({
        op: 'vfxBurstOn',
        effect,
        entity: player.id,
        offset: { x: 0, y: 0, z: 0 },
        joint,
        rotation: quat(rotation(undefined)),
        players,
        durationMs: BURST_MS_DEFAULT,
        prime: false,
        ...tuning({ scale }),
      });
    }
    return players.length;
  }
}

module.exports = { Vfxs };
