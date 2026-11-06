'use strict';

// Siege ladders: the server API's SiegeLadder, one of the game's ladders leaning on a
// wall. The server holds every ladder: its kind, place, pose and who pushed it. The climb is the
// game's own and needs nothing from the server.
//
// A ladder is `standing`, `pushed` (a defender's swing, then the fall), `down`, `raising` (the
// lift) or `broken`. A push and a raise take time the game plays out; the server notes when the
// step is due and ends it when a player's game reports it over, or, when no report comes, at one
// shared fallback timer that exists only while some ladder is mid-step.
//
// Bridge ops, each sent only when something changes:
//   server -> clients
//     { op: 'siegeLadderSpawn', id, kind, model, height, lean, position, rotation, pose, usable,
//       dimension }
//     { op: 'siegeLadderPose', id, pose, actor, usable }   actor: the pusher or raiser's id, 0 none
//     { op: 'siegeLadderDestroy', id }
//     { op: 'siegeLadderRefused', id }   (id: the player) their push or raise was not allowed
//   client -> server
//     { op: 'siegeLadderUse', id, ladder, action }   action: 'push' | 'raise'
//     { op: 'siegeLadderSettled', id, ladder, pose } pose: 'down' after a push, 'standing' after
//                                                    a raise
// `rotation` is a quaternion { x, y, z, w }.

const { resolveMesh } = require('../props/catalog.cjs');
const { OWNER, runningResource, removeOwned } = require('../lifecycle/owned_objects.cjs');
const { Quaternion, Vector3 } = require('../math.cjs');
const { distance, placementOf, playerOf, requireRunning, validWorld } = require('./placement.cjs');

// What each of the game's ladders is: the mesh, the height of its rungs in metres and its lean
// towards the wall in degrees.
const KINDS = new Map(
  [
    ['siege', 'ladder_siege_10m', 9.75, 12],
    ['tall', 'ladder_rustic_575', 5.5, 12],
    ['short', 'ladder_rustic_350', 3.25, 12],
    ['low', 'ladder_rustic_225', 2, 12],
  ].map(([kind, stem, height, lean]) => [kind, { kind, model: resolveMesh(stem), height, lean }]),
);

// How long the swing and the fall of a push last, and the lift of a raise, in milliseconds.
const PUSH_MS = 4500;
const RAISE_MS = 5700;
// A report may come this much before the step is due (the game's clock and ours differ), and the
// fallback timer waits this long after it for a report that does not come.
const EARLY_MS = 1500;
const GRACE_MS = 3000;
// How far from the foot of a ladder, beyond its height, a player may ask to push or raise it.
const REACH = 6;

// The step each mid-step pose ends in.
const ENDS = { pushed: 'down', raising: 'standing' };

const result = (accepted, reason = '', seconds = 0) => ({ accepted, reason, seconds });

class SiegeLadders {
  // `host`: the resource host (bridge, emit, players, allocateEntityId, the running resource).
  constructor(host, now = Date.now) {
    this.host = host;
    this.now = now;
    // id -> SiegeLadder
    this.ladders = new Map();
    // The one fallback timer, armed while a ladder is mid-step.
    this.timer = null;
  }

  // The SiegeLadder class of the server API, an Entity.
  define(Entity) {
    const registry = this;
    class SiegeLadder {
      constructor(id) {
        const ladder = registry.ladders.get(id);
        if (!ladder) {
          throw new Error('Unknown siege ladder');
        }
        return ladder;
      }
      static spawn(kind, position, orientation, dimension) {
        return registry.spawn(SiegeLadder, kind, position, orientation, dimension);
      }
      static describe(kind) {
        const entry = KINDS.get(kind);
        return entry ? { ...entry } : null;
      }
      static all(dimension) {
        if (dimension !== undefined && !validWorld(dimension)) {
          throw new TypeError('Invalid dimension');
        }
        return [...registry.ladders.values()].filter(
          (ladder) => dimension === undefined || ladder.dimension === dimension,
        );
      }
      static getById(id) {
        return registry.ladders.get(id) ?? null;
      }
      static destroyAll(dimension) {
        const doomed = SiegeLadder.all(dimension);
        doomed.forEach((ladder) => registry.destroy(ladder));
        return doomed.length;
      }
      get position() {
        return new Vector3(this.at.x, this.at.y, this.at.z);
      }
      get rotation() {
        return new Quaternion(this.facing.w, this.facing.x, this.facing.y, this.facing.z);
      }
      get pose() {
        return this.data.pose;
      }
      get height() {
        return this.data.height;
      }
      get climbable() {
        return this.data.pose === 'standing';
      }
      get usable() {
        return this.data.usable;
      }
      set usable(value) {
        if (typeof value !== 'boolean') {
          throw new TypeError('SiegeLadder.usable must be a boolean');
        }
        registry.change(this, { usable: value });
      }
      get pusher() {
        return this.data.pusher;
      }
      push(player) {
        return registry.push(this, playerOf(registry.host, player));
      }
      raise(player) {
        return registry.raise(this, playerOf(registry.host, player));
      }
      break() {
        registry.change(this, { pose: 'broken', due: 0 });
      }
      repair() {
        if (this.data.pose !== 'standing') {
          registry.change(this, { pose: 'standing', due: 0 });
        }
      }
      destroy() {
        registry.destroy(this);
      }
      toString() {
        return `SiegeLadder(${this.id}, ${this.kind}, ${this.data.pose})`;
      }
    }
    Object.setPrototypeOf(SiegeLadder.prototype, Entity.prototype);
    return SiegeLadder;
  }

  spawn(SiegeLadder, kind, position = {}, orientation = {}, dimension) {
    requireRunning(this.host);
    const entry = KINDS.get(kind);
    if (!entry) {
      throw new RangeError('SiegeLadder kind must be siege, tall, short or low');
    }
    const place = placementOf('siege ladder', position, orientation, dimension);
    const ladder = Object.create(SiegeLadder.prototype);
    Object.defineProperty(ladder, 'id', { value: this.host.allocateEntityId(), enumerable: true });
    Object.defineProperty(ladder, 'kind', { value: kind, enumerable: true });
    Object.defineProperty(ladder, 'dimension', { value: place.world, enumerable: true });
    Object.defineProperty(ladder, 'at', { value: place.at });
    Object.defineProperty(ladder, 'facing', { value: place.facing });
    // The changeable state: the pose, the flag, the last pusher, the actor of the current step and
    // when it is due (0 outside a step).
    Object.defineProperty(ladder, 'data', {
      value: {
        pose: 'standing',
        usable: true,
        pusher: null,
        actor: null,
        due: 0,
        height: entry.height,
      },
    });
    ladder[OWNER] = runningResource(this.host);
    this.ladders.set(ladder.id, ladder);
    this.host.bridge.send(this.spawnMessage(ladder));
    this.host.emit('siegeLadderSpawn', ladder);
    return ladder;
  }

  // The op that makes a ladder appear, with the pose and flag it has now.
  spawnMessage(ladder) {
    const entry = KINDS.get(ladder.kind);
    return {
      op: 'siegeLadderSpawn',
      id: ladder.id,
      kind: ladder.kind,
      model: entry.model,
      height: entry.height,
      lean: entry.lean,
      position: ladder.at,
      rotation: ladder.facing,
      pose: ladder.data.pose,
      usable: ladder.data.usable,
      dimension: ladder.dimension,
    };
  }

  // A player's world became ready: their game lost the ladders, so it is told of each again. The
  // op names the player, so nobody else hears it.
  arrived(player) {
    for (const ladder of this.ladders.values()) {
      this.host.bridge.send({ ...this.spawnMessage(ladder), to: player.id });
    }
  }

  // Applies a change to a ladder's state and tells the clients, unless nothing they show changed.
  change(ladder, { pose, usable, pusher, actor, due }) {
    const data = ladder.data;
    if (this.ladders.get(ladder.id) !== ladder) {
      return;
    }
    const changed =
      (pose !== undefined && pose !== data.pose) ||
      (usable !== undefined && usable !== data.usable);
    if (pose !== undefined) data.pose = pose;
    if (usable !== undefined) data.usable = usable;
    if (pusher !== undefined) data.pusher = pusher;
    if (actor !== undefined) data.actor = actor;
    if (due !== undefined) data.due = due;
    if (due) {
      this.arm();
    }
    if (changed) {
      this.host.bridge.send({
        op: 'siegeLadderPose',
        id: ladder.id,
        pose: data.pose,
        actor: data.actor?.id ?? 0,
        usable: data.usable,
      });
    }
  }

  push(ladder, player) {
    requireRunning(this.host);
    const { pose } = ladder.data;
    if (pose !== 'standing') {
      return result(false, pose === 'broken' ? 'is broken' : 'is not standing');
    }
    this.change(ladder, {
      pose: 'pushed',
      pusher: player,
      actor: player,
      due: this.now() + PUSH_MS,
    });
    return result(true, '', PUSH_MS / 1000);
  }

  raise(ladder, player) {
    requireRunning(this.host);
    const { pose } = ladder.data;
    if (pose !== 'down') {
      return result(false, pose === 'broken' ? 'is broken' : 'is not down');
    }
    this.change(ladder, { pose: 'raising', actor: player, due: this.now() + RAISE_MS });
    return result(true, '', RAISE_MS / 1000);
  }

  // A pushed ladder has hit the ground, or a raised one leans on its wall again.
  settle(ladder) {
    const end = ENDS[ladder.data.pose];
    if (!end) {
      return;
    }
    const pushed = ladder.data.pose === 'pushed';
    this.change(ladder, { pose: end, due: 0 });
    if (pushed) {
      this.host.emit('siegeLadderFall', ladder, ladder.data.pusher);
    } else {
      this.host.emit('siegeLadderRaise', ladder);
    }
  }

  // The fallback timer: one for all ladders, set for the earliest step and only while one exists.
  arm() {
    if (this.timer) {
      return;
    }
    const due = Math.min(...[...this.ladders.values()].map((l) => l.data.due).filter((d) => d > 0));
    if (!Number.isFinite(due)) {
      return;
    }
    this.timer = setTimeout(() => {
      this.timer = null;
      this.sweep(this.now());
    }, Math.max(0, due + GRACE_MS - this.now()));
    this.timer.unref?.();
  }

  // Ends every step whose report is overdue, then waits for the next one.
  sweep(now) {
    for (const ladder of [...this.ladders.values()]) {
      if (ladder.data.due > 0 && ladder.data.due + GRACE_MS <= now) {
        this.settle(ladder);
      }
    }
    this.arm();
  }

  // The siege ladders a stopping resource made go with it.
  resourceStopped(name) {
    removeOwned(this.ladders.values(), name, (ladder) => this.destroy(ladder));
  }

  destroy(ladder) {
    if (this.ladders.get(ladder.id) !== ladder) {
      return;
    }
    this.host.emit('siegeLadderDestroy', ladder);
    this.ladders.delete(ladder.id);
    this.host.bridge.send({ op: 'siegeLadderDestroy', id: ladder.id });
    if (this.ladders.size === 0 && this.timer) {
      clearTimeout(this.timer);
      this.timer = null;
    }
  }

  // siegeLadderUse: a player's game asks to push a ladder from its top or raise a fallen one.
  use(player, id, action) {
    const ladder = this.ladders.get(id);
    if (!player?.ready || !ladder || (action !== 'push' && action !== 'raise')) {
      return;
    }
    const near =
      player.dimension === ladder.dimension &&
      distance(player.position, ladder.at) <= ladder.data.height + REACH;
    const allowed =
      near && ladder.data.usable && this.host.emit('siegeLadderUse', player, ladder, action);
    const done =
      allowed && (action === 'push' ? this.push(ladder, player) : this.raise(ladder, player));
    if (!done?.accepted) {
      this.host.bridge.send({ op: 'siegeLadderRefused', id: player.id });
    }
  }

  // siegeLadderSettled: a player's game saw the fall or the lift end.
  settled(player, id, pose) {
    const ladder = this.ladders.get(id);
    if (
      !player?.ready ||
      !ladder ||
      ENDS[ladder.data.pose] !== pose ||
      player.dimension !== ladder.dimension
    ) {
      return;
    }
    if (this.now() >= ladder.data.due - EARLY_MS) {
      this.settle(ladder);
    }
  }

  // The player is leaving: a step they started goes on without them.
  left(player) {
    for (const ladder of this.ladders.values()) {
      if (ladder.data.actor === player) ladder.data.actor = null;
      if (ladder.data.pusher === player) ladder.data.pusher = null;
    }
  }
}

module.exports = { SiegeLadders };
