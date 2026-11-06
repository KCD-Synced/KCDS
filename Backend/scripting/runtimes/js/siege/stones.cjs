'use strict';

// Stone piles on a battlement: the server API's StonePile. The prompt, the lift and
// the heave over the wall are the game's own stone throwing, played by the thrower's game; the
// server holds how many stones a pile has left, whether it is offered, and decides what a stone
// that comes down does.
//
// A throw: the thrower's game takes a stone and reports it (stonePileTake). The server counts it
// off the pile, raises stonePileThrow and notes one stone in flight for that player. The stone's
// first contact is reported by the thrower's game (stoneImpact); the server accepts it only for a
// stone in flight and only within IMPACT_RANGE of the pile it was taken from, then hurts everyone
// within the pile's damage radius: the full damage at the point, a quarter of it at the edge.
// Players take it as a health stat change in their own game, NPCs through Npc.damage.
//
// Bridge ops, each sent only when something changes:
//   server -> clients
//     { op: 'stonePileSpawn', id, position, rotation, stones, usable, dimension }
//     { op: 'stonePileUpdate', id, stones, usable }
//     { op: 'stonePileDestroy', id }
//   client -> server
//     { op: 'stonePileTake', id, pile }                  the player's game lifted a stone
//     { op: 'stoneImpact', id, pile, position }          where it first met something
// `rotation` is a quaternion { x, y, z, w }. A pile that runs dry or is made unusable is sent as
// an update; a take it cannot honour is answered with the pile's current update to that
// player's game alone (the update carries no player, so it goes to everyone, and only changes
// state to what the server already holds).

const { Quaternion, Vector3, vector } = require('../math.cjs');
const { OWNER, runningResource, removeOwned } = require('../lifecycle/owned_objects.cjs');
const { distance, placementOf, requireRunning, validWorld } = require('./placement.cjs');

// The bounds of the server API.
const DAMAGE_DEFAULT = 60;
const DAMAGE_MAX = 1000;
const RADIUS_DEFAULT = 1.5;
const RADIUS_MAX = 10;
// A stone's first contact must lie this near its pile (metres).
const IMPACT_RANGE = 30;
// How far from a pile a player may stand and take a stone (metres).
const TAKE_REACH = 6;
// Stones in flight one player may have at once.
const IN_FLIGHT_LIMIT = 8;
// The share of the damage that reaches the edge of the radius.
const EDGE_SHARE = 0.25;

function numberIn(value, max, what) {
  if (typeof value !== 'number' || !Number.isFinite(value) || value < 0 || value > max) {
    throw new RangeError(`StonePile.${what} must be a number from 0 to ${max}`);
  }
  return value;
}

// The health a point `from` the centre of a stone's fall takes, or 0 outside the radius.
function damageAt(damage, radius, from) {
  if (from > radius) {
    return 0;
  }
  const reach = radius > 0 ? from / radius : 0;
  return damage * (1 - (1 - EDGE_SHARE) * reach);
}

class StonePiles {
  // `host`: the resource host (bridge, emit, players, npcs, allocateEntityId, the running resource).
  constructor(host) {
    this.host = host;
    // id -> StonePile
    this.piles = new Map();
    // player id -> the piles their stones in flight were taken from, oldest first.
    this.inFlight = new Map();
  }

  // The StonePile class of the server API, an Entity.
  define(Entity) {
    const registry = this;
    class StonePile {
      constructor(id) {
        const pile = registry.piles.get(id);
        if (!pile) {
          throw new Error('Unknown stone pile');
        }
        return pile;
      }
      static spawn(position, orientation, dimension) {
        return registry.spawn(StonePile, position, orientation, dimension);
      }
      static all(dimension) {
        if (dimension !== undefined && !validWorld(dimension)) {
          throw new TypeError('Invalid dimension');
        }
        return [...registry.piles.values()].filter(
          (pile) => dimension === undefined || pile.dimension === dimension,
        );
      }
      static getById(id) {
        return registry.piles.get(id) ?? null;
      }
      static destroyAll(dimension) {
        const doomed = StonePile.all(dimension);
        doomed.forEach((pile) => registry.destroy(pile));
        return doomed.length;
      }
      get position() {
        return new Vector3(this.at.x, this.at.y, this.at.z);
      }
      get rotation() {
        return new Quaternion(this.facing.w, this.facing.x, this.facing.y, this.facing.z);
      }
      // Stones left; any negative value is -1, a pile that never runs out.
      get stones() {
        return this.data.stones;
      }
      set stones(value) {
        if (!Number.isFinite(value)) {
          throw new TypeError('StonePile.stones must be a number');
        }
        registry.change(this, { stones: value < 0 ? -1 : Math.floor(value) });
      }
      get usable() {
        return this.data.usable;
      }
      set usable(value) {
        if (typeof value !== 'boolean') {
          throw new TypeError('StonePile.usable must be a boolean');
        }
        registry.change(this, { usable: value });
      }
      get damage() {
        return this.data.damage;
      }
      set damage(value) {
        this.data.damage = numberIn(value, DAMAGE_MAX, 'damage');
      }
      get blastRadius() {
        return this.data.blastRadius;
      }
      set blastRadius(value) {
        this.data.blastRadius = numberIn(value, RADIUS_MAX, 'blastRadius');
      }
      destroy() {
        registry.destroy(this);
      }
      toString() {
        return `StonePile(${this.id}, ${this.data.stones})`;
      }
    }
    Object.setPrototypeOf(StonePile.prototype, Entity.prototype);
    return StonePile;
  }

  spawn(StonePile, position = {}, orientation = {}, dimension) {
    requireRunning(this.host);
    const place = placementOf('stone pile', position, orientation, dimension);
    const pile = Object.create(StonePile.prototype);
    Object.defineProperty(pile, 'id', { value: this.host.allocateEntityId(), enumerable: true });
    Object.defineProperty(pile, 'dimension', { value: place.world, enumerable: true });
    Object.defineProperty(pile, 'at', { value: place.at });
    Object.defineProperty(pile, 'facing', { value: place.facing });
    Object.defineProperty(pile, 'data', {
      value: {
        stones: -1,
        usable: true,
        damage: DAMAGE_DEFAULT,
        blastRadius: RADIUS_DEFAULT,
      },
    });
    pile[OWNER] = runningResource(this.host);
    this.piles.set(pile.id, pile);
    this.host.bridge.send(this.spawnMessage(pile));
    this.host.emit('stonePileSpawn', pile);
    return pile;
  }

  // The op that makes a pile appear, with the stones and flag it has now.
  spawnMessage(pile) {
    return {
      op: 'stonePileSpawn',
      id: pile.id,
      position: pile.at,
      rotation: pile.facing,
      stones: pile.data.stones,
      usable: pile.data.usable,
      dimension: pile.dimension,
    };
  }

  // A player's world became ready: their game lost the piles, so it is told of each again. The
  // op names the player, so nobody else hears it.
  arrived(player) {
    for (const pile of this.piles.values()) {
      this.host.bridge.send({ ...this.spawnMessage(pile), to: player.id });
    }
  }

  // Changes how many stones a pile has or whether it is offered, and tells the clients, unless
  // nothing changed.
  change(pile, { stones, usable }) {
    const data = pile.data;
    if (this.piles.get(pile.id) !== pile) {
      return;
    }
    const changed =
      (stones !== undefined && stones !== data.stones) ||
      (usable !== undefined && usable !== data.usable);
    if (stones !== undefined) data.stones = stones;
    if (usable !== undefined) data.usable = usable;
    if (changed) {
      this.sendUpdate(pile);
    }
  }

  sendUpdate(pile) {
    this.host.bridge.send({
      op: 'stonePileUpdate',
      id: pile.id,
      stones: pile.data.stones,
      usable: pile.data.usable,
    });
  }

  // The stone piles a stopping resource made go with it.
  resourceStopped(name) {
    removeOwned(this.piles.values(), name, (pile) => this.destroy(pile));
  }

  destroy(pile) {
    if (this.piles.get(pile.id) !== pile) {
      return;
    }
    this.host.emit('stonePileDestroy', pile);
    this.piles.delete(pile.id);
    this.host.bridge.send({ op: 'stonePileDestroy', id: pile.id });
  }

  // stonePileTake: a player's game lifted a stone off a pile and heaves it at once, so it cannot
  // be refused; a take the pile could not give is corrected by sending its state again.
  take(player, id) {
    const pile = this.piles.get(id);
    if (!player?.ready || !pile) {
      return;
    }
    const data = pile.data;
    const honoured =
      data.usable &&
      data.stones !== 0 &&
      player.dimension === pile.dimension &&
      distance(player.position, pile.at) <= TAKE_REACH;
    if (!honoured) {
      this.sendUpdate(pile);
      return;
    }
    if (data.stones > 0) {
      this.change(pile, { stones: data.stones - 1 });
    }
    const flying = this.inFlight.get(player.id) ?? [];
    flying.push(pile);
    if (flying.length > IN_FLIGHT_LIMIT) {
      flying.shift();
    }
    this.inFlight.set(player.id, flying);
    this.host.emit('stonePileThrow', player, pile);
  }

  // stoneImpact: where a stone the player threw first met something.
  impact(player, id, position) {
    const flying = this.inFlight.get(player?.id);
    if (!flying || flying.length === 0) {
      return;
    }
    // The stone is the oldest in flight; the pile the player's game names must be its own.
    const pile = flying[0];
    if (pile.id !== id) {
      return;
    }
    const at = vector(position);
    if (![at.x, at.y, at.z].every(Number.isFinite) || distance(at, pile.at) > IMPACT_RANGE) {
      return;
    }
    flying.shift();
    if (flying.length === 0) {
      this.inFlight.delete(player.id);
    }
    this.hurtAround(pile, at, player);
    // The pile can be gone by the time the stone lands, and then it is null.
    const live = this.piles.get(pile.id) === pile ? pile : null;
    this.host.emit('stoneImpact', live, new Vector3(at.x, at.y, at.z), player);
  }

  // Hurts every player and NPC of the pile's world within its radius of the point.
  hurtAround(pile, at, attacker) {
    const { damage, blastRadius } = pile.data;
    if (damage <= 0) {
      return;
    }
    for (const player of this.host.players.values()) {
      if (!player.ready || player.dimension !== pile.dimension) {
        continue;
      }
      const share = damageAt(damage, blastRadius, distance(player.position, at));
      if (share > 0) {
        player.addStat('health', -share);
      }
    }
    for (const npc of this.host.npcs.npcs.values()) {
      if (npc.dimension !== pile.dimension) {
        continue;
      }
      const share = damageAt(damage, blastRadius, distance(npc.position, at));
      if (share > 0) {
        npc.damage(share, attacker);
      }
    }
  }

  // The player is leaving: the stones they threw come down unseen.
  left(player) {
    this.inFlight.delete(player.id);
  }
}

module.exports = { StonePiles, damageAt };
