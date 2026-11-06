'use strict';

// The client-only Follow global: the local player follows another player or a server
// NPC with a body here with the game's own follow action (Client/runtime/follow/follow.hpp). Only one scripted follow is queued
// or running; Follow.stop(), a movement press, the target going, the game's own end and the
// resource stopping end it.
//
//   Follow.targets()        -> [{ id, kind: 'player' | 'npc', name, position }] within 20 m
//   Follow.canStart(target) -> the game's own checks and the start distance
//   Follow.start(target, { hint, playerCanStop, pace, distance }?) -> queued; followStarted(target)
//                             once the game's action runs (hint false hides the game's
//                             "Stop following" prompt, playerCanStop false takes its key and the
//                             movement keys, so only a script ends it: distance, 0.5
//                             to 10 m, is how close the follower keeps, 0 the game's own)
//   Follow.getTarget(), Follow.isPending(), Follow.stop()
//
//   events: followStarted(target), followStopped(target, reason) with reason 'script', 'input',
//   'mountChanged', 'teleported', 'worldChanged', 'unavailable', 'interrupted', 'targetLost',
//   'failed', 'resourceStopped' or 'sessionEnded'
//
// The game client answers { op: 'syncRequest', request: { op: 'followCheck' | 'followStart' |
// 'followStop', entity } } and tells { op: 'followEvent', event, reason }.

const TARGET_RANGE = 20;
// The global world: what is in it is in every dimension.
const GLOBAL_WORLD = 0xffffffff;

function sharesWorld(one, other) {
  return one === other || one === GLOBAL_WORLD || other === GLOBAL_WORLD;
}

class ResourceFollow {
  // entities: the ClientEntities; sync: the SyncChannel; emit(name, args) dispatches an event.
  constructor({ entities, sync, emit, Vector3 }) {
    this.entities = entities;
    this.sync = sync;
    this.emit = emit;
    this.Vector3 = Vector3;
    // { owner, target, pending }
    this.current = null;
  }

  localRecord() {
    return this.entities.records.get(this.entities.localId) ?? null;
  }

  describe(id) {
    const npc = this.entities.npcBodies?.get(id);
    if (npc) {
      const { x, y, z } = npc.position;
      return Object.freeze({
        id,
        kind: 'npc',
        name: npc.name,
        position: new this.Vector3(x, y, z),
      });
    }
    const record = this.entities.records.get(id);
    if (!record || record.kind !== 'player' || id === this.entities.localId) {
      return null;
    }
    const { x, y, z } = record.position;
    return Object.freeze({
      id,
      kind: 'player',
      name: record.nickname ?? '',
      position: new this.Vector3(x, y, z),
    });
  }

  // The engine entity of a target's body on this machine, or 0.
  engineOf(id) {
    const npc = this.entities.npcBodies?.get(id);
    if (npc) {
      return npc.engineId;
    }
    return this.entities.engineIds.get(id) ?? 0;
  }

  targets() {
    const local = this.localRecord();
    if (!local) {
      return [];
    }
    // An NPC is in both lists (the directory's records and the bodies here): once.
    const ids = new Set([
      ...this.entities.records.keys(),
      ...(this.entities.npcBodies?.keys() ?? []),
    ]);
    const found = [];
    for (const id of ids) {
      const target = this.describe(id);
      if (!target || !this.engineOf(id)) {
        continue;
      }
      const dx = target.position.x - local.position.x;
      const dy = target.position.y - local.position.y;
      const dz = target.position.z - local.position.z;
      if (Math.hypot(dx, dy, dz) <= TARGET_RANGE) {
        found.push(target);
      }
    }
    return found;
  }

  // The network id a Player handle or a FollowTarget names.
  idOf(target) {
    const id = target?.id;
    if (!Number.isInteger(id) || id <= 0) {
      throw new TypeError('Follow takes a Player or a target from Follow.targets()');
    }
    return id;
  }

  canStart(target) {
    const id = this.idOf(target);
    const entity = this.engineOf(id);
    if (!this.describe(id) || !entity) {
      return false;
    }
    return this.sync.call({ op: 'followCheck', entity }) === true;
  }

  start(owner, target, options = {}) {
    if (options === null || typeof options !== 'object') {
      throw new TypeError('Follow.start options must be an object');
    }
    const { hint = true, playerCanStop = true, pace = 'walk', distance = 0 } = options;
    if (typeof hint !== 'boolean' || typeof playerCanStop !== 'boolean') {
      throw new TypeError('Follow.start hint and playerCanStop must be true or false');
    }
    // 'run' lets the follower keep up with a leader who runs; the game's follow walks.
    if (pace !== 'walk' && pace !== 'run') {
      throw new TypeError("Follow.start pace must be 'walk' or 'run'");
    }
    // How close the follower keeps, in metres; 0, the default, is the game's own.
    if (
      typeof distance !== 'number' ||
      !Number.isFinite(distance) ||
      (distance !== 0 && (distance < 0.5 || distance > 10))
    ) {
      throw new RangeError("Follow.start distance must be 0 (the game's) or 0.5 to 10 metres");
    }
    const id = this.idOf(target);
    const described = this.describe(id);
    const entity = this.engineOf(id);
    if (!described || !entity) {
      return false;
    }
    if (this.current) {
      this.end('script');
    }
    const run = pace === 'run';
    if (
      this.sync.call({ op: 'followStart', entity, hint, playerCanStop, run, distance }) !== true
    ) {
      return false;
    }
    this.current = {
      owner,
      target: described,
      pending: true,
      world: this.localRecord()?.dimension,
    };
    return true;
  }

  // Why the follow can no longer go on as the directory now has it: 'worldChanged' (the local
  // player went to another dimension), 'unavailable' (the target is in a world the player is
  // not in, dead, or an NPC whose body here is gone), or null.
  endReason() {
    const { target, world } = this.current;
    const local = this.localRecord();
    if (local && world !== undefined && local.dimension !== world) {
      return 'worldChanged';
    }
    if (target.kind === 'npc') {
      return this.entities.npcBodies?.has(target.id) ? null : 'unavailable';
    }
    const record = this.entities.records.get(target.id);
    if (!record) {
      // The game client tells of a target that is gone (targetLost).
      return null;
    }
    if (local && !sharesWorld(local.dimension, record.dimension)) {
      return 'unavailable';
    }
    const health = record.statSnapshot?.vitals?.health;
    if (typeof health === 'number' && health <= 0) {
      return 'unavailable';
    }
    return null;
  }

  // The directory or the NPC bodies changed: ends a follow that can no longer go on.
  check() {
    if (!this.current) {
      return;
    }
    const reason = this.endReason();
    if (reason) {
      this.sync.call({ op: 'followStop' });
      this.end(reason);
    }
  }

  stop(owner) {
    if (!this.current || this.current.owner !== owner) {
      return;
    }
    this.sync.call({ op: 'followStop' });
  }

  end(reason) {
    const ended = this.current;
    this.current = null;
    if (ended) {
      this.emit('followStopped', [ended.target, reason]);
    }
  }

  receiveEvent(message) {
    if (!this.current) {
      return;
    }
    if (message.event === 'started') {
      this.current.pending = false;
      this.emit('followStarted', [this.current.target]);
    } else if (message.event === 'stopped') {
      this.end(
        typeof message.reason === 'string' && message.reason ? message.reason : 'interrupted',
      );
    }
  }

  removeOwner(owner) {
    if (this.current?.owner === owner) {
      this.sync.call({ op: 'followStop' });
      this.end('resourceStopped');
    }
  }

  facade(owner) {
    return Object.freeze({
      targets: () => this.targets(),
      canStart: (target) => this.canStart(target),
      start: (target, options) => this.start(owner, target, options),
      getTarget: () => (this.current && !this.current.pending ? this.current.target : null),
      isPending: () => Boolean(this.current?.pending),
      stop: () => this.stop(owner),
    });
  }
}

module.exports = { ResourceFollow, TARGET_RANGE };
