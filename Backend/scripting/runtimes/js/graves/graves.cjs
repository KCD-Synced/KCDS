'use strict';

// The level's graves, the same for every player (the server API's Grave). A grave is
// a spot of the level where a player may dig with a shovel (the game's entity class "Hole"); the
// digging is the game's own minigame on that player's game. The players' games report the graves
// their level has (Client/runtime/graves/), so the server knows where they are, and tell the
// server when their player dug one out. The server owns each grave's state and every player's game
// shows it: a grave one player dug out is open for everyone, a player who joins later sees it so,
// and a script may dig one out or fill it again.
//
// Client -> host ops (Backend/integration/local/runtime.hpp, GRAVE in graves.hpp):
//   { op: 'graveReport', id: <player id>, grave, position, dug }  a grave the player's level has
//   { op: 'graveDug', id: <player id>, grave, position }          the player dug it out
// Host -> client op, for one ready player (`to`) or every one (0):
//   { op: 'graveSet', to, grave, dug }
// `grave` is the grave's id, its position in whole centimetres ("x,y,z"). Nothing is saved: a
// server start finds every grave full again.
//
// Events: graveDug (player, grave) when a player dug a grave out.

// The most graves one level may have (the client's kMaxGraves).
const MAX_GRAVES = 1024;
const GRAVE_ID = /^-?\d{1,8},-?\d{1,8},-?\d{1,8}$/;

function position(value) {
  if (!value || typeof value !== 'object') return null;
  const x = Number(value.x);
  const y = Number(value.y);
  const z = Number(value.z);
  return Number.isFinite(x) && Number.isFinite(y) && Number.isFinite(z) ? { x, y, z } : null;
}

function distance(a, b) {
  return Math.hypot(a.x - b.x, a.y - b.y, a.z - b.z);
}

class Graves {
  constructor(host) {
    this.host = host;
    // id -> { position, dug }
    this.graves = new Map();
    // id -> the Grave handle scripts hold
    this.handles = new Map();
  }

  // The Grave class of the server API; every resource gets the same one.
  define() {
    this.Grave ??= this.makeClass();
    return this.Grave;
  }

  makeClass() {
    const registry = this;
    class Grave {
      #id;
      constructor(id) {
        if (!registry.graves.has(id)) {
          throw new Error('Unknown grave');
        }
        const known = registry.handles.get(id);
        if (known) return known;
        this.#id = id;
        registry.handles.set(id, this);
      }
      // Its id: where it lies in whole centimetres, "x,y,z".
      get id() {
        return this.#id;
      }
      // Where the grave lies, in metres.
      get position() {
        return { ...registry.graves.get(this.id).position };
      }
      // Whether it is dug out.
      get dug() {
        return registry.graves.get(this.id).dug;
      }
      // How far it lies from a position, in metres.
      distanceTo(at) {
        const from = position(at);
        if (!from) throw new Error('distanceTo needs a position { x, y, z }');
        return distance(registry.graves.get(this.id).position, from);
      }
      // Digs it out for every player; false when it is dug out already.
      dig() {
        return registry.set(this.id, true);
      }
      // Fills it again for every player, so it can be dug once more; false when it is full.
      reset() {
        return registry.set(this.id, false);
      }
      toString() {
        return `Grave(${this.id}, ${this.dug ? 'dug out' : 'full'})`;
      }
      // Every grave the players' games found in the server's level, nearest first when `near` is
      // given.
      static all(near) {
        const graves = [...registry.graves.keys()].map((id) => new Grave(id));
        const from = near === undefined ? null : position(near);
        if (near !== undefined && !from) throw new Error('Grave.all needs a position { x, y, z }');
        if (from) graves.sort((a, b) => a.distanceTo(from) - b.distanceTo(from));
        return graves;
      }
      // The grave of this id, or null.
      static get(id) {
        return registry.graves.has(id) ? new Grave(id) : null;
      }
      // The grave nearest to a position within `radius` metres (any distance without one), or
      // null.
      static nearest(at, radius) {
        const from = position(at);
        if (!from) throw new Error('Grave.nearest needs a position { x, y, z }');
        const found = Grave.all(from)[0] ?? null;
        if (!found) return null;
        return radius === undefined || found.distanceTo(from) <= radius ? found : null;
      }
      // Fills every dug-out grave again; the number filled.
      static resetAll() {
        let filled = 0;
        for (const id of registry.graves.keys()) {
          if (registry.set(id, false)) filled += 1;
        }
        return filled;
      }
    }
    return Grave;
  }

  set(id, dug) {
    const grave = this.graves.get(id);
    if (!grave || grave.dug === dug) return false;
    grave.dug = dug;
    this.tell(0, id);
    return true;
  }

  tell(to, id) {
    this.host.bridge.send({ op: 'graveSet', to, grave: id, dug: this.graves.get(id).dug });
  }

  // A grave a player's level has: the first report of a grave the server did not know makes it
  // known with the state that game shows; a game showing another state than the server's is set
  // right.
  report(player, id, at, dug) {
    if (!player || typeof id !== 'string' || !GRAVE_ID.test(id)) return;
    const known = this.graves.get(id);
    if (!known) {
      const where = position(at);
      if (!where || this.graves.size >= MAX_GRAVES) return;
      this.graves.set(id, { position: where, dug: dug === true });
      return;
    }
    if (known.dug !== (dug === true)) {
      this.tell(player.id, id);
    }
  }

  // A player dug a grave out (the server checked they stand at it).
  dug(player, id, at) {
    if (!player || typeof id !== 'string' || !GRAVE_ID.test(id)) return;
    if (!this.graves.has(id)) {
      const where = position(at);
      if (!where || this.graves.size >= MAX_GRAVES) return;
      this.graves.set(id, { position: where, dug: false });
    }
    if (!this.set(id, true)) return;
    const Grave = this.define();
    this.host.emit('graveDug', player, new Grave(id));
  }

  // A player's world is ready: their game shows every grave full, so it hears the dug-out ones.
  arrived(player) {
    for (const [id, grave] of this.graves) {
      if (grave.dug) {
        this.tell(player.id, id);
      }
    }
  }
}

module.exports = { Graves };
