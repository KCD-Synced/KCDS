'use strict';

// Questions about the world asked of a player's game for the server (World.raycast, traceAll,
// snapToGround and entitiesAround). The server has no world of its
// own, so it asks one player's game to trace a segment with the game's physics, or to name the
// replicated bodies it shows around a point, and waits for the answer. The promise always
// settles with the reply, a five second timeout or the player's exit; check `answered`.
//
//   { op: 'worldRaycast', id: <player id>, request, query, start, end, mode, max }
//     query 0 = the first hit (max 1), 1 = every hit along the ray, nearest first (max 1 to 16).
//     start and end are the ray's ends (World.raycast's from and to; `to` on an op names a
//     replay's player).
//     mode 0 = cover (terrain, static geometry and rigid bodies; living bodies are not hit),
//     1 = anything (bodies too, the asked player's own skipped), 2 = ground (terrain and static
//     geometry only), 3 = terrain only.
//   { op: 'worldBodiesInRadius', id, request, center, radius }
//     radius 0 < r <= 512 m.
//   { op: 'worldRaycastReply', id, request, status: 'miss' | 'hit' | 'unavailable',
//     hits: [{ position, normal, distance, terrain, entity, surface, kind, model, guid, name, cls,
//       lo? }] }   lo: the level object { kind, name, guid, cls, at, rot, scale }
//     entity is the network id of the replicated body that was hit (a player, an NPC or a horse),
//     0 for anything else; surface the name of the surface type ('' when unknown).
//   { op: 'worldBodiesReply', id, request, status, bodies: [{ entity, position }] }
//   { op: 'worldLevelEntities', id, request, center, radius, max }
//     radius 0 < r <= 64 m, max 1 to 32; the reply is a worldRaycastReply whose hits are the
//     level's entities, each with its position, distance from the centre and level object.
//
// Results:
//   raycast        { answered, reason, hit } with hit null or { position, normal, distance,
//                  terrain, entity, surface } (entity: the handle of that body, or null)
//   traceAll     { answered, reason, hits } nearest first
//   snapToGround  { answered, reason, z, hit } z the ground's height under x / y, or null
//   entitiesAround { answered, reason, entities } of { entity, position, distance }, nearest
//                  first; bodies the server does not know are left out
//   levelEntitiesAround { answered, reason, entities } of { guid, name, class, position,
//                  distance, levelObject }, nearest first
//
// A hit also says what it is: kind, model (a brush's or vegetation instance's mesh),
// category, entityGuid / entityName / entityClass and levelObject, what LevelEdit.apply needs to
// find the object again. material is null for now.

const { Quaternion } = require('../math.cjs');
const { categoryOf, meshPathOf } = require('./mesh_categories.cjs');

const MODES = Object.freeze({ cover: 0, anything: 1, ground: 2, terrain: 3 });
const MAX_LENGTH = 4096;
const MAX_HITS = 16;
const MAX_RADIUS = 512;
// World.levelEntitiesAround's widest radius and most entities (world_ray.hpp kMaxEntityRadius,
// kMaxLevelEntities).
const MAX_ENTITY_RADIUS = 64;
const MAX_LEVEL_ENTITIES = 32;
const MAX_COORDINATE = 100000;
// snapToGround traces down from the given height, or from this one, over the longest ray.
const GROUND_TOP = 4000;
const TIMEOUT_MS = 5000;

function point(value, name, where = 'World.raycast') {
  const valid =
    value && Number.isFinite(value.x) && Number.isFinite(value.y) && Number.isFinite(value.z);
  if (!valid) {
    throw new TypeError(`${where}: ${name} must be a { x, y, z } of finite numbers`);
  }
  return { x: value.x, y: value.y, z: value.z };
}

function modeOf(options, fallback, where) {
  const name = options?.mode ?? fallback;
  if (typeof name !== 'string' || !Object.hasOwn(MODES, name)) {
    throw new TypeError(`${where}: mode is 'cover', 'anything', 'ground' or 'terrain'`);
  }
  return MODES[name];
}

function checkLength(start, end, where) {
  const length = Math.hypot(end.x - start.x, end.y - start.y, end.z - start.z);
  if (!(length > 0) || length > MAX_LENGTH) {
    throw new RangeError(`${where}: the ray must be longer than 0 and at most ${MAX_LENGTH} m`);
  }
}

class WorldTraces {
  constructor(host) {
    this.host = host;
    this.nextRequest = 1;
    this.pending = new Map();
  }

  // Sends one question to a ready player's game; the promise settles with `settle(reply)` or with
  // `empty` and the reason it was not answered.
  ask(player, message, settle, empty) {
    const target = player && this.host.players.get(player.id);
    if (!target || target !== player || !target.ready) {
      return Promise.resolve({ answered: false, reason: 'player not ready', ...empty });
    }
    const request = this.nextRequest;
    this.nextRequest = this.nextRequest >= 0xffffffff ? 1 : this.nextRequest + 1;
    return new Promise((resolve) => {
      const unanswered = (reason) => resolve({ answered: false, reason, ...empty });
      const timer = setTimeout(() => {
        this.pending.delete(request);
        unanswered('timeout');
      }, TIMEOUT_MS);
      timer.unref?.();
      this.pending.set(request, { player, resolve, unanswered, settle, timer });
      this.host.bridge.send({ ...message, id: player.id, request });
    });
  }

  // The handle of the replicated body with this network id, or null.
  entityOf(id) {
    if (!(id > 0)) {
      return null;
    }
    return (
      this.host.players.get(id) ?? this.host.npcs.npcs.get(id) ?? this.host.horses.get(id) ?? null
    );
  }

  // The level object a hit names: every field a LevelEditDefinition needs to find it
  // again, or null.
  levelObjectOf(lo) {
    if (!lo || (lo.kind !== 'brush' && lo.kind !== 'entity')) {
      return null;
    }
    const turn = Array.isArray(lo.rot) && lo.rot.length === 4 ? lo.rot : [0, 0, 0, 1];
    const brush = lo.kind === 'brush';
    return Object.freeze({
      kind: lo.kind,
      name: brush ? meshPathOf(lo.name) ?? '' : String(lo.name ?? ''),
      guid: brush ? undefined : String(lo.guid ?? ''),
      class: brush ? undefined : String(lo.cls ?? ''),
      at: point(lo.at, 'at'),
      rotation: new Quaternion(turn[3], turn[0], turn[1], turn[2]),
      scale: point(lo.scale, 'scale'),
    });
  }

  hitOf(hit) {
    const kind = typeof hit.kind === 'string' ? hit.kind : 'static';
    const entity = kind === 'entity';
    const model = meshPathOf(hit.model);
    return {
      position: point(hit.position, 'position'),
      normal: point(hit.normal, 'normal'),
      distance: hit.distance,
      terrain: hit.terrain === true,
      entity: this.entityOf(hit.entity),
      surface: typeof hit.surface === 'string' ? hit.surface : '',
      // What the hit is, as client scripts read it.
      kind,
      model,
      category: categoryOf(model),
      material: null,
      entityGuid: entity && hit.guid ? hit.guid : null,
      entityName: entity ? String(hit.name ?? '') : null,
      entityClass: entity ? String(hit.cls ?? '') : null,
      levelObject: this.levelObjectOf(hit.lo),
    };
  }

  // { op: 'playerWeaponHit', id, hit }: what a player's swing met that is not a body (issue),
  // the player's own client's word (Backend/integration weapon_hits.hpp).
  weaponHit(player, raw) {
    if (!player || raw === null || typeof raw !== 'object') {
      return;
    }
    const hit = this.hitOf(raw);
    this.host.emit('playerWeaponHit', player, hit);
    // An axe blow on a tree or a felled trunk (Trees).
    this.host.trees?.weaponHit(player, hit);
  }

  hitsOf(message) {
    if (message.status !== 'hit' || !Array.isArray(message.hits)) {
      return [];
    }
    return message.hits.map((hit) => this.hitOf(hit));
  }

  rayMessage(from, to, mode, max, where) {
    const start = point(from, 'from', where);
    const end = point(to, 'to', where);
    checkLength(start, end, where);
    return { op: 'worldRaycast', query: max > 1 ? 1 : 0, start, end, mode, max };
  }

  raycast(player, from, to, options) {
    const where = 'World.raycast';
    const message = this.rayMessage(from, to, modeOf(options, 'cover', where), 1, where);
    return this.ask(player, message, (reply) => ({ hit: this.hitsOf(reply)[0] ?? null }), {
      hit: null,
    });
  }

  traceAll(player, from, to, options) {
    const where = 'World.traceAll';
    const max = options?.max ?? MAX_HITS;
    if (!Number.isInteger(max) || max < 1 || max > MAX_HITS) {
      throw new RangeError(`${where}: max is 1 to ${MAX_HITS}`);
    }
    const message = this.rayMessage(from, to, modeOf(options, 'cover', where), max, where);
    // query 1 even for one hit, so the answer is always a list.
    message.query = 1;
    return this.ask(player, message, (reply) => ({ hits: this.hitsOf(reply) }), { hits: [] });
  }

  snapToGround(player, position, options) {
    const where = 'World.snapToGround';
    const valid = position && Number.isFinite(position.x) && Number.isFinite(position.y);
    if (!valid || (position.z !== undefined && !Number.isFinite(position.z))) {
      throw new TypeError(`${where}: position must be a { x, y } or { x, y, z } of finite numbers`);
    }
    const mode = modeOf(options, 'ground', where);
    if (mode !== MODES.ground && mode !== MODES.terrain) {
      throw new TypeError(`${where}: mode is 'ground' or 'terrain'`);
    }
    const top = Math.min(position.z ?? GROUND_TOP, MAX_COORDINATE);
    const bottom = Math.max(top - MAX_LENGTH, -MAX_COORDINATE);
    const message = this.rayMessage(
      { x: position.x, y: position.y, z: top },
      { x: position.x, y: position.y, z: bottom },
      mode,
      1,
      where,
    );
    return this.ask(
      player,
      message,
      (reply) => {
        const hit = this.hitsOf(reply)[0] ?? null;
        return { z: hit ? hit.position.z : null, hit };
      },
      { z: null, hit: null },
    );
  }

  entitiesAround(player, center, radius) {
    const where = 'World.entitiesAround';
    const centre = point(center, 'center', where);
    if (!Number.isFinite(radius) || radius <= 0 || radius > MAX_RADIUS) {
      throw new RangeError(`${where}: radius is more than 0 and at most ${MAX_RADIUS} m`);
    }
    const message = { op: 'worldBodiesInRadius', center: centre, radius };
    const settle = (reply) => {
      const bodies = reply.status === 'hit' && Array.isArray(reply.bodies) ? reply.bodies : [];
      const entities = [];
      for (const body of bodies) {
        const entity = this.entityOf(body.entity);
        if (!entity) {
          continue;
        }
        const position = point(body.position, 'position', where);
        const distance = Math.hypot(
          position.x - centre.x,
          position.y - centre.y,
          position.z - centre.z,
        );
        entities.push({ entity, position, distance });
      }
      entities.sort((a, b) => a.distance - b.distance);
      return { entities };
    };
    return this.ask(player, message, settle, { entities: [] });
  }

  // The entities the level names around a point in a player's game, such as
  // a drying rack's sausages, a door or a lever: what LevelEdit.apply and addInteraction take.
  levelEntitiesAround(player, center, radius, options) {
    const where = 'World.levelEntitiesAround';
    const centre = point(center, 'center', where);
    if (!Number.isFinite(radius) || radius <= 0 || radius > MAX_ENTITY_RADIUS) {
      throw new RangeError(`${where}: radius is more than 0 and at most ${MAX_ENTITY_RADIUS} m`);
    }
    const max = options?.max ?? MAX_LEVEL_ENTITIES;
    if (!Number.isInteger(max) || max < 1 || max > MAX_LEVEL_ENTITIES) {
      throw new RangeError(`${where}: max is an integer from 1 to ${MAX_LEVEL_ENTITIES}`);
    }
    const message = { op: 'worldLevelEntities', center: centre, radius, max };
    const settle = (reply) => {
      const entities = this.hitsOf(reply)
        .filter((hit) => hit.entityGuid !== null)
        .map((hit) => ({
          guid: hit.entityGuid,
          name: hit.entityName,
          class: hit.entityClass,
          position: hit.position,
          distance: hit.distance,
          levelObject: hit.levelObject,
        }));
      return { entities };
    };
    return this.ask(player, message, settle, { entities: [] });
  }

  // worldRaycastReply and worldBodiesReply: what the player's game found.
  reply(player, message) {
    const query = this.pending.get(message.request);
    if (!query || query.player !== player) {
      return;
    }
    this.pending.delete(message.request);
    clearTimeout(query.timer);
    if (message.status === 'unavailable') {
      query.unanswered('unavailable');
      return;
    }
    let result;
    try {
      result = query.settle(message);
    } catch {
      query.unanswered('bad answer');
      return;
    }
    query.resolve({ answered: true, reason: '', ...result });
  }

  // A player left: their open questions end.
  left(player) {
    for (const [request, query] of this.pending) {
      if (query.player === player) {
        this.pending.delete(request);
        clearTimeout(query.timer);
        query.unanswered('player left');
      }
    }
  }

  api() {
    return {
      raycast: (player, from, to, options) => this.raycast(player, from, to, options),
      traceAll: (player, from, to, options) => this.traceAll(player, from, to, options),
      snapToGround: (player, position, options) => this.snapToGround(player, position, options),
      entitiesAround: (player, center, radius) => this.entitiesAround(player, center, radius),
      levelEntitiesAround: (player, center, radius, options) =>
        this.levelEntitiesAround(player, center, radius, options),
    };
  }
}

module.exports = { WorldTraces };
