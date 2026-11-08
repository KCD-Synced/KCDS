'use strict';

// Props the server puts in the world: the server API's Prop. A prop is a static mesh
// from the game's prop catalog (props/catalog.cjs), or one a resource streams
// (`objects/kcds/<resource>/<name>.cgf`), spawned as a replicated entity that every client in view
// builds; the server holds the model, pose, scale, collision and dimension.
//
// Display goes through four bridge ops, sent only when something changes (no timers):
//   { op: 'propSpawn', id, model, position, rotation, scale, physics, dimension[, body, joint] }
//   { op: 'propUpdate', id, physics, scale }   (the client rebuilds the prop with the new values)
//   { op: 'propMove', id, position, rotation }  (the client moves it as it is)
//   { op: 'propDestroy', id }
// A prop shown to some players only (showOnlyTo) has every op addressed to each of
// them (`to`); the others never build it.
//
// A rigid prop's body is simulated by one client: the nearest player's whose game
// shows it, however far, chosen again every ELECTION_MS. Two more ops, to that
// player only:
//   { op: 'propSimulate', id, simulate, to }   (its game simulates the body, or no longer does)
//   { op: 'propPush', id, push, linear, angular, point, hasPoint, to }
//                                              (push: impulse, velocity, wake or sleep)
// That client reports the body (op propReport: position, rotation, resting, hitSpeed); every
// other player is sent the pose as a propMove, a report of rest raises propRest and one of a hit
// raises propHit. A report farther than REST_RANGE from where the prop last rested
// (or was spawned) puts it back there instead.
// `rotation` is a quaternion { x, y, z, w } and `scale` a vector { x, y, z }.
//
// An area's collision walls (areas/area_walls.cjs) are props of their own: the game's
// 2 m primitive cube (its origin the centre of its bottom face) scaled to each piece, physics
// 'wall', a static body the game client does not draw. They are no Prop: not listed, held by no
// script; the area adds and removes them. A static Prop a script hides (visible = false, issue
// an invisible support or barrier) goes to the clients as 'wall' the same way.
//
// A prop on a body (Player.attachProp, cuffs, a sack on the head, a noose) hangs on a
// player's joint on every client: propSpawn carries the body's network id and the joint, its
// position and rotation are the offset from there, and it has no physics. It goes when its player
// leaves.

const { resolveMesh, collisionOf, resolvePrefab } = require('./catalog.cjs');
const { OWNER, removeOwned } = require('../lifecycle/owned_objects.cjs');
const { Quaternion, Vector3, rotation, vector } = require('../math.cjs');

const GLOBAL_WORLD = 0xffffffff;
const WALL_MESH = 'objects/special/primitive_cube.cgf';
const PHYSICS = new Set(['static', 'rigid', 'none']);
const SCALE_MIN = 0.01;
const SCALE_MAX = 100;
// A prop farther out than this lies outside any level.
const WORLD_LIMIT = 1e6;
// A rigid prop's simulation.
const REST_RANGE = 100;
const ELECTION_MS = 500;
const MASS_MAX = 100000;
const VELOCITY_MAX = 200;
const SPIN_MAX = 100;
const IMPULSE_MAX = 1e9;
// The game's hand slots: the joints it holds a hand's item on.
const HAND_SLOTS = Object.freeze({ left: 'LeftHandSlot', right: 'RightHandSlot' });
// A prop on a body: at most this far from its joint, a joint's name at most this
// long.
const HANG_OFFSET_MAX = 5;
const JOINT_MAX = 64;
// A mesh a resource streams is named by its full path and unknown to the catalog.
const STREAMED = /^objects\/kcds\/[a-z0-9_.-]+\/(?:[a-z0-9_.-]+\/)*[a-z0-9_.-]+\.cgf$/;
// KCDS's own models in the client's asset pak: the playing cards (TabletopDeck).
const BUILT_IN = /^objects\/kcds_cards\/[a-z0-9_]+\.cgf$/;
// Prop.glideTo: how long a glide may take (ms) and how high its arc may lift (metres).
const GLIDE_MS_MAX = 10000;
const GLIDE_ARC_MAX = 5;

function validWorld(world) {
  return Number.isSafeInteger(world) && world >= 0 && world <= GLOBAL_WORLD;
}

// A model naming one of the game's prefabs: "prefab:" and its path under Prefabs/ or
// its file stem.
const PREFAB = 'prefab:';

function isPrefab(model) {
  return model.startsWith(PREFAB);
}

// "prefab:<catalog path>" of the prefab `name` names; throws for one the catalog does not carry
// and for one the game cannot build while it runs.
function prefabOf(name) {
  const found = resolvePrefab(name);
  if (found === null) {
    throw new RangeError(`the prefab catalog does not carry ${name}`);
  }
  if (found.blocked) {
    throw new RangeError(
      `the game cannot build the prefab ${found.path} while it runs: it ${found.blocked}`,
    );
  }
  return `${PREFAB}${found.path}`;
}

// What a client is sent for a prop's model: a prefab by its template's GUID.
function wireModel(model) {
  return isPrefab(model) ? `${PREFAB}${resolvePrefab(model.slice(PREFAB.length)).guid}` : model;
}

// The catalog (or streamed) path of the mesh named by `model`, or "prefab:<path>" of a game
// prefab; throws for one neither carries.
function meshOf(model) {
  if (typeof model !== 'string' || model === '') {
    throw new TypeError('Prop model must be a mesh path or file stem');
  }
  if (model.toLowerCase().startsWith(PREFAB)) {
    return prefabOf(model.slice(PREFAB.length));
  }
  const name = model.replace(/\\/g, '/').toLowerCase();
  const mesh = STREAMED.test(name) || BUILT_IN.test(name) ? name : resolveMesh(model);
  if (mesh === null) {
    throw new RangeError(`the prop catalog does not carry ${model}`);
  }
  return mesh;
}

// A game prefab is built static, at its own size, in the world: the game lays its
// meshes as they are, solid.
function checkPrefab(collision, size, body) {
  if (collision !== 'static') {
    throw new TypeError('A game prefab is static');
  }
  if (!sameScale(size, { x: 1, y: 1, z: 1 })) {
    throw new TypeError('A game prefab keeps its own size (scale 1)');
  }
  if (body !== 0) {
    throw new TypeError('A game prefab cannot hang on a body');
  }
}

// The physics a client builds a prop with: a hidden static prop is drawn by no one ('wall').
function wirePhysics(prop) {
  return prop.hidden ? 'wall' : prop.collision;
}

function physicsOf(physics) {
  if (!PHYSICS.has(physics)) {
    throw new RangeError('Prop physics must be static, rigid or none');
  }
  return physics;
}

// One size or separate sizes, each clamped into the allowed range, as { x, y, z }.
function scaleOf(scale) {
  const sizes =
    typeof scale === 'number' ? [scale, scale, scale] : scale && [scale.x, scale.y, scale.z];
  if (!sizes || !sizes.every((n) => typeof n === 'number' && Number.isFinite(n))) {
    throw new TypeError('Prop scale must be a number or a Vector3');
  }
  const [x, y, z] = sizes.map((n) => Math.min(SCALE_MAX, Math.max(SCALE_MIN, n)));
  return { x, y, z };
}

const sameScale = (a, b) => a.x === b.x && a.y === b.y && a.z === b.z;

// { x, y, z } of a vector whose parts are finite numbers, else null.
function plainVector(value) {
  if (!value || typeof value !== 'object') {
    return null;
  }
  const { x, y, z } = value;
  return [x, y, z].every((n) => typeof n === 'number' && Number.isFinite(n)) ? { x, y, z } : null;
}

const lengthOf = (v) => Math.hypot(v.x, v.y, v.z);
const ZERO = Object.freeze({ x: 0, y: 0, z: 0 });

// A prop's position, checked: a point inside the levels.
function positionOf(position) {
  const at = vector(position ?? {});
  if (![at.x, at.y, at.z].every((n) => Number.isFinite(n) && Math.abs(n) <= WORLD_LIMIT)) {
    throw new RangeError('Invalid prop position');
  }
  return at;
}

// The audience of showOnlyTo (or the players of hideFrom): null for everyone (nobody), else the set
// of the players' ids.
function audienceOf(players, method = 'showOnlyTo') {
  if (players === null || players === undefined) {
    return null;
  }
  const list = Array.isArray(players) ? players : [players];
  const ids = new Set();
  for (const player of list) {
    if (!player || !Number.isInteger(player.id)) {
      throw new TypeError(`${method} takes a Player, an array of players or null`);
    }
    ids.add(player.id);
  }
  return ids;
}

class Props {
  // `host`: the resource host (bridge, emit, allocateEntityId, the running resource).
  constructor(host) {
    this.host = host;
    // id -> Prop
    this.props = new Map();
    // id -> the propSpawn op of an area's wall piece
    this.walls = new Map();
  }

  // The Prop class of the server API, an Entity.
  define(Entity) {
    const registry = this;
    class Prop {
      constructor(id) {
        const prop = registry.props.get(id);
        if (!prop) {
          throw new Error('Unknown prop');
        }
        return prop;
      }
      // `map`: one of World.maps, the first by default; only the players on it see the prop.
      static spawn(model, position, orientation, scale, physics, dimension, map) {
        return registry.spawn(Prop, {
          model,
          position,
          orientation,
          scale,
          physics,
          dimension,
          map,
        });
      }
      static all() {
        return [...registry.props.values()];
      }
      static getById(id) {
        return registry.props.get(id) ?? null;
      }
      // How a catalog mesh collides: 'mesh', 'proxy' or 'none'; null for one the
      // catalog does not carry (a mesh your resource ships).
      static collisionOf(model) {
        return collisionOf(model);
      }
      static destroyAll(dimension) {
        if (dimension !== undefined && !validWorld(dimension)) {
          throw new TypeError('Invalid dimension');
        }
        let removed = 0;
        for (const prop of [...registry.props.values()]) {
          if (dimension === undefined || prop.dimension === dimension) {
            registry.destroy(prop);
            removed += 1;
          }
        }
        return removed;
      }
      get meshName() {
        if (isPrefab(this.model)) {
          return this.model.slice(this.model.lastIndexOf('/') + 1);
        }
        return this.model.slice(this.model.lastIndexOf('/') + 1, -'.cgf'.length);
      }
      // The folder under objects/ the mesh sits in; `kcds/<resource>` for a streamed mesh;
      // `prefabs/<folder>` for a game prefab.
      get meshGroup() {
        if (isPrefab(this.model)) {
          return `prefabs/${this.model.slice(PREFAB.length, this.model.lastIndexOf('/'))}`;
        }
        const folder = this.model.slice('objects/'.length, this.model.lastIndexOf('/'));
        return folder.startsWith('kcds/') ? folder.split('/').slice(0, 2).join('/') : folder;
      }
      get position() {
        return new Vector3(this.at.x, this.at.y, this.at.z);
      }
      get rotation() {
        return new Quaternion(this.facing.w, this.facing.x, this.facing.y, this.facing.z);
      }
      set position(value) {
        registry.move(this, positionOf(value), this.facing);
      }
      set rotation(value) {
        registry.move(this, this.at, rotation(value));
      }
      // Moves it in one go: a position and, optionally, a rotation (Euler degrees or a
      // Quaternion); left out, it keeps its turn.
      setPose(position, orientation) {
        const facing = orientation === undefined ? this.facing : rotation(orientation);
        registry.move(this, positionOf(position), facing);
      }
      // Moves it smoothly: every player's game glides it there over `ms` (default 400, up to
      // 10000), lifted on an arc `arc` metres high at the middle (default 0, a straight line), as
      // a thrown card or a hopping piece. The server sends where to once, not every step; the
      // prop is at the end pose at once for the server and for players who come later.
      glideTo(position, orientation, options = {}) {
        if (options === null || typeof options !== 'object') {
          throw new TypeError('glideTo takes { ms, arc }');
        }
        const ms = options.ms === undefined ? 400 : Number(options.ms);
        const arc = options.arc === undefined ? 0 : Number(options.arc);
        if (!Number.isInteger(ms) || ms < 0 || ms > GLIDE_MS_MAX) {
          throw new RangeError(`glideTo: ms is a whole number from 0 to ${GLIDE_MS_MAX}`);
        }
        if (!Number.isFinite(arc) || Math.abs(arc) > GLIDE_ARC_MAX) {
          throw new RangeError(`glideTo: arc is from -${GLIDE_ARC_MAX} to ${GLIDE_ARC_MAX} metres`);
        }
        if (this.collision === 'rigid') {
          throw new TypeError('glideTo: a rigid prop moves by its physics');
        }
        const facing = orientation === undefined ? this.facing : rotation(orientation);
        registry.move(this, positionOf(position), facing, ms > 0 ? { ms, arc } : null);
      }
      // The players who see it, or null when everyone does.
      get visibleTo() {
        return this.audience === null ? null : [...this.audience].map((id) => registry.player(id));
      }
      // Shows it to these players only (a Player or an array of them), or to everyone again (null).
      showOnlyTo(players) {
        registry.setAudience(this, audienceOf(players));
      }
      // Hides it from these players (a Player or an array of them), whoever else sees it, players
      // who join later too; null or [] hides it from nobody again. A card in a player's hand shows
      // its back to everyone but them (Tabletop cards).
      hideFrom(players) {
        registry.setHiddenFrom(this, audienceOf(players, 'hideFrom') ?? new Set());
      }
      // The players it is hidden from (hideFrom), [] for nobody.
      get hiddenFrom() {
        return [...this.hiddenIds].map((id) => registry.player(id)).filter(Boolean);
      }
      get physics() {
        return this.collision;
      }
      set physics(value) {
        if (this.body !== 0) {
          throw new TypeError('A prop on a body has no physics');
        }
        registry.update(this, { physics: physicsOf(value) });
      }
      // The player it hangs on (Player.attachProp), null for a prop in the world.
      get attachedTo() {
        return this.body === 0 ? null : registry.player(this.body);
      }
      // The joint of the body it hangs on ('' for the body itself or a prop in the world).
      get joint() {
        return this.hangJoint;
      }
      // Whether the players see its model; a hidden static prop still collides.
      get visible() {
        return !this.hidden;
      }
      set visible(value) {
        registry.setVisible(this, value);
      }
      // A rigid prop's mass in kilograms; 0 takes the mass its mesh's collision gives it.
      get mass() {
        return this.weight;
      }
      set mass(value) {
        if (
          typeof value === 'number' &&
          Number.isFinite(value) &&
          value >= 0 &&
          value <= MASS_MAX
        ) {
          registry.update(this, { mass: value });
        }
      }
      applyImpulse(impulse, point, angularImpulse) {
        return registry.applyImpulse(this, impulse, point, angularImpulse);
      }
      setVelocity(velocity, angularVelocity) {
        return registry.setVelocity(this, velocity, angularVelocity);
      }
      wake() {
        return registry.pushTo(this, { push: 'wake' });
      }
      sleep() {
        return registry.pushTo(this, { push: 'sleep' });
      }
      resetPhysics() {
        return registry.resetPhysics(this);
      }
      // A number while the sizes agree, else the Vector3 of them.
      get scale() {
        const { x, y, z } = this.size;
        return x === y && y === z ? x : new Vector3(x, y, z);
      }
      set scale(value) {
        registry.update(this, { size: scaleOf(value) });
      }
      destroy() {
        registry.destroy(this);
      }
      toString() {
        return `Prop(${this.id}, ${this.model}, ${this.collision}, ${this.scale})`;
      }
    }
    Object.setPrototypeOf(Prop.prototype, Entity.prototype);
    // Kept for the placer, which turns a confirmed preview into one (props/previews.cjs).
    this.Prop = Prop;
    return Prop;
  }

  // `audience` (a Set of player ids, or null) and `hiddenIds` (a Set) are who sees it from its
  // first message on: a card's face reaches no game but its holder's, not even for a moment.
  spawn(
    Prop,
    {
      model,
      position = {},
      orientation = {},
      scale = 1,
      physics = 'static',
      dimension,
      map,
      body = 0,
      joint = '',
      audience = null,
      hiddenIds = new Set(),
    },
  ) {
    const owner = this.host.context.getStore();
    if (
      owner &&
      (this.host.resources.get(owner.name) !== owner || !owner.active || owner.failing)
    ) {
      throw new Error('Resource is stopped');
    }
    const mesh = meshOf(model);
    const world = dimension ?? GLOBAL_WORLD;
    if (!validWorld(world)) {
      throw new TypeError('Invalid dimension');
    }
    // The map it stands on (an index of World.maps): a prop on a player's body is on theirs.
    const bodyPlayer = body !== 0 ? this.host.players.get(body) : null;
    const mapIndex = bodyPlayer
      ? this.host.playerMapIndex(bodyPlayer)
      : this.host.mapIndexOf(map, 'Prop.spawn');
    const at = positionOf(position);
    const facing = rotation(orientation);
    const size = scaleOf(scale);
    const collision = physicsOf(physics);
    if (isPrefab(mesh)) {
      checkPrefab(collision, size, body);
    }
    if (body !== 0 && Math.hypot(at.x, at.y, at.z) > HANG_OFFSET_MAX) {
      throw new RangeError(`A prop on a body is at most ${HANG_OFFSET_MAX} m from its joint`);
    }
    const prop = Object.create(Prop.prototype);
    Object.defineProperty(prop, 'id', { value: this.host.allocateEntityId(), enumerable: true });
    Object.defineProperty(prop, 'model', { value: mesh, enumerable: true });
    Object.defineProperty(prop, 'dimension', { value: world, enumerable: true });
    Object.defineProperty(prop, 'mapIndex', { value: mapIndex });
    const host = this.host;
    Object.defineProperty(prop, 'map', {
      enumerable: true,
      get() {
        return host.mapName(mapIndex);
      },
    });
    // The body it hangs on and the joint of it.
    Object.defineProperty(prop, 'body', { value: body });
    Object.defineProperty(prop, 'hangJoint', { value: joint });
    // The changeable state: assigned through the setters.
    prop.at = { x: at.x, y: at.y, z: at.z };
    prop.facing = { x: facing.x, y: facing.y, z: facing.z, w: facing.w };
    prop.collision = collision;
    prop.hidden = false;
    prop.size = size;
    prop.weight = 0;
    // Where it was spawned (resetPhysics) and where it last rested (the reports' check).
    prop.spawnAt = { x: at.x, y: at.y, z: at.z };
    prop.spawnFacing = { x: facing.x, y: facing.y, z: facing.z, w: facing.w };
    prop.restAt = { x: at.x, y: at.y, z: at.z };
    // The player whose game simulates the rigid body; 0 for none.
    prop.simulator = 0;
    // null: every player sees it; else the ids of the players who do.
    prop.audience = audience === null ? null : new Set(audience);
    // The ids of the players it is hidden from (hideFrom), whoever else sees it.
    prop.hiddenIds = new Set(hiddenIds);
    prop[OWNER] = owner?.name ?? null;
    this.props.set(prop.id, prop);
    this.send(prop, this.spawnMessage(prop));
    this.host.emit('propSpawn', prop);
    return prop;
  }

  // The propSpawn op for a prop as it stands now.
  spawnMessage(prop) {
    return {
      op: 'propSpawn',
      id: prop.id,
      model: wireModel(prop.model),
      position: prop.at,
      rotation: prop.facing,
      scale: { ...prop.size },
      physics: wirePhysics(prop),
      dimension: prop.dimension,
      mass: prop.weight,
      ...(prop.body !== 0 ? { body: prop.body, joint: prop.hangJoint } : {}),
    };
  }

  // player.attachProp: a prop on a joint of the player's body, without physics.
  // `hand` hangs it in the game's own slot of that hand, where the game holds an item
  // such as its rope cuffs: the slot joints LeftHandSlot / RightHandSlot that animations move with
  // what the hand holds, and that the game's item meshes are made for.
  // `viewers` (internal, the cards in a hand): { audience, hiddenIds } as spawn takes them.
  attach(player, options, viewers = {}) {
    if (options === null || typeof options !== 'object') {
      throw new TypeError('attachProp takes { model, joint, hand, position, rotation, scale }');
    }
    const { model, hand, position = {}, rotation: turn = {}, scale = 1 } = options;
    let joint = options.joint ?? '';
    if (hand !== undefined) {
      if (!Object.hasOwn(HAND_SLOTS, hand)) {
        throw new TypeError("attachProp hand is 'left' or 'right'");
      }
      if (options.joint !== undefined) {
        throw new TypeError('attachProp takes a joint or a hand, not both');
      }
      joint = HAND_SLOTS[hand];
    }
    if (typeof joint !== 'string' || joint.length > JOINT_MAX || !/^[\x20-\x7e]*$/.test(joint)) {
      throw new TypeError(`attachProp joint is a joint's name of up to ${JOINT_MAX} characters`);
    }
    if (!this.Prop) {
      this.host.makeAPI();
    }
    return this.spawn(this.Prop, {
      model,
      position,
      orientation: turn,
      scale,
      physics: 'none',
      dimension: player.dimension,
      body: player.id,
      joint,
      ...viewers,
    });
  }

  // player.addMark: a mark on the skin, a flat mesh hung on a joint as attachProp
  // hangs a prop, under an id of its own. Throws without a joint: a mark sits on one.
  addMark(player, options) {
    if (options === null || typeof options !== 'object') {
      throw new TypeError('addMark takes { model, joint, position, rotation, scale }');
    }
    if (typeof options.joint !== 'string' || options.joint === '') {
      throw new TypeError("addMark needs the joint it sits on ('Head', 'Spine3', 'RightArm')");
    }
    const mark = this.attach(player, options);
    mark.mark = true;
    return mark.id;
  }

  // player.removeMark: true when the id was one of this player's marks.
  removeMark(player, id) {
    const prop = this.props.get(id);
    if (!prop || prop.body !== player.id || prop.mark !== true) {
      return false;
    }
    this.destroy(prop);
    return true;
  }

  // player.marks: the ids of the player's marks, oldest first.
  marksOf(player) {
    const ids = [];
    for (const prop of this.props.values()) {
      if (prop.body === player.id && prop.mark === true) {
        ids.push(prop.id);
      }
    }
    return ids;
  }

  // player.detachProp: a prop attachProp hung on this player; true when it was.
  detach(player, which) {
    const id = typeof which === 'object' && which !== null ? which.id : which;
    const prop = this.props.get(id);
    if (!prop || prop.body !== player.id) {
      return false;
    }
    this.destroy(prop);
    return true;
  }

  // The props hung on a player who left go with them.
  leaving(id) {
    for (const prop of [...this.props.values()]) {
      if (prop.body === id) {
        this.destroy(prop);
      }
    }
  }

  // A player's world became ready: their game lost the props, so it is told of each again. The
  // op names the player, so nobody else hears it.
  arrived(player) {
    for (const prop of this.props.values()) {
      // Only the props of the map they stand on: their game has that map's level now.
      if (this.shownTo(prop, player.id) && this.onPropMap(player, prop)) {
        this.host.bridge.send({ ...this.spawnMessage(prop), to: player.id });
      }
      // Their game builds it anew, following: the next election may hand it back.
      if (prop.simulator === player.id) {
        prop.simulator = 0;
      }
    }
    for (const message of this.walls.values()) {
      this.host.bridge.send({ ...message, to: player.id });
    }
  }

  // Shows a piece of an area's walls ({ center, half, rotation: { w, x, y, z } }) in a virtual
  // world; returns its id.
  spawnWall(piece, dimension) {
    const { center, half, rotation: q } = piece;
    // The cube stands on its bottom face: its origin is the centre less the turned half height.
    const up = {
      x: 2 * (q.x * q.z + q.w * q.y),
      y: 2 * (q.y * q.z - q.w * q.x),
      z: 1 - 2 * (q.x * q.x + q.y * q.y),
    };
    const id = this.host.allocateEntityId();
    const message = {
      op: 'propSpawn',
      id,
      model: WALL_MESH,
      position: {
        x: center.x - up.x * half.z,
        y: center.y - up.y * half.z,
        z: center.z - up.z * half.z,
      },
      rotation: { x: q.x, y: q.y, z: q.z, w: q.w },
      scale: { x: half.x, y: half.y, z: half.z },
      physics: 'wall',
      dimension,
    };
    this.walls.set(id, message);
    this.host.bridge.send(message);
    return id;
  }

  destroyWall(id) {
    if (this.walls.delete(id)) {
      this.host.bridge.send({ op: 'propDestroy', id });
    }
  }

  // Whether the prop is shown to the player of this id: in its audience (or everyone's) and not
  // hidden from them.
  shownTo(prop, id) {
    return (prop.audience === null || prop.audience.has(id)) && !prop.hiddenIds.has(id);
  }

  // Whether the player stands on the prop's map (always on a server of one map).
  onPropMap(player, prop) {
    return this.host.playerMapIndex(player) === prop.mapIndex;
  }

  // An op about a prop to the players who see it: one op for everyone on a server of one map,
  // else one per player (its audience, and on a server of several maps those on its map).
  send(prop, message) {
    const oneMap = this.host.maps.length <= 1;
    if (oneMap && prop.audience === null && prop.hiddenIds.size === 0) {
      this.host.bridge.send(message);
      return;
    }
    const ids = prop.audience === null ? [...this.host.players.keys()] : [...prop.audience];
    for (const id of ids) {
      const player = this.host.players.get(id);
      if (prop.hiddenIds.has(id) || (!oneMap && (!player || !this.onPropMap(player, prop)))) {
        continue;
      }
      this.host.bridge.send({ ...message, to: id });
    }
  }

  // The live player of an id, or null.
  player(id) {
    return this.host.players.get(id) ?? null;
  }

  // Moves a prop; nothing is sent when it stays where it is.
  // `glide` ({ ms, arc }): the players' games move it there smoothly over ms on an arc arc metres
  // high (Prop.glideTo); the server holds the end pose at once, so a player who comes later sees
  // it where it ends.
  move(prop, at, facing, glide = null) {
    if (this.props.get(prop.id) !== prop) {
      return;
    }
    // On a body the position is the offset from its joint.
    if (prop.body !== 0 && Math.hypot(at.x, at.y, at.z) > HANG_OFFSET_MAX) {
      throw new RangeError(`A prop on a body is at most ${HANG_OFFSET_MAX} m from its joint`);
    }
    const same =
      at.x === prop.at.x &&
      at.y === prop.at.y &&
      at.z === prop.at.z &&
      facing.x === prop.facing.x &&
      facing.y === prop.facing.y &&
      facing.z === prop.facing.z &&
      facing.w === prop.facing.w;
    if (same) {
      return;
    }
    prop.at = { x: at.x, y: at.y, z: at.z };
    prop.facing = { x: facing.x, y: facing.y, z: facing.z, w: facing.w };
    prop.restAt = { x: at.x, y: at.y, z: at.z };
    const message = { op: 'propMove', id: prop.id, position: prop.at, rotation: prop.facing };
    if (glide !== null) {
      message.ms = glide.ms;
      message.arc = glide.arc;
    }
    this.send(prop, message);
  }

  // Who sees a prop from now on: the players who lose it take it down, those who gain it build it.
  setAudience(prop, audience) {
    this.changeViewers(prop, () => {
      prop.audience = audience;
    });
  }

  // Who it is hidden from from now on (hideFrom): a Set of ids, empty for nobody.
  setHiddenFrom(prop, hidden) {
    this.changeViewers(prop, () => {
      prop.hiddenIds = hidden;
    });
  }

  // Applies a change of who sees the prop: the players who lose it take it down, those who gain it
  // build it.
  changeViewers(prop, change) {
    if (this.props.get(prop.id) !== prop) {
      return;
    }
    const everyone = [...this.host.players.keys()];
    const before = new Set(everyone.filter((id) => this.shownTo(prop, id)));
    change();
    for (const id of everyone) {
      if (before.has(id) && !this.shownTo(prop, id)) {
        this.host.bridge.send({ op: 'propDestroy', id: prop.id, to: id });
      }
    }
    for (const id of everyone) {
      if (!before.has(id) && this.shownTo(prop, id)) {
        this.host.bridge.send({ ...this.spawnMessage(prop), to: id });
      }
    }
  }

  // Changes a prop's collision and/or size and has every client rebuild it; nothing is sent when
  // the values stay.
  update(prop, { physics, size, mass }) {
    if (this.props.get(prop.id) !== prop) {
      return;
    }
    const physicsChanged = physics !== undefined && physics !== prop.collision;
    const sizeChanged = size !== undefined && !sameScale(size, prop.size);
    const massChanged = mass !== undefined && mass !== prop.weight;
    if (!physicsChanged && !sizeChanged && !massChanged) {
      return;
    }
    if (isPrefab(prop.model) && (physicsChanged || sizeChanged || massChanged)) {
      throw new TypeError('A game prefab stays static, at its own size');
    }
    if (physicsChanged && prop.hidden && physics !== 'static') {
      throw new TypeError('A hidden prop stays static; show it (visible = true) first');
    }
    if (physicsChanged) prop.collision = physics;
    if (sizeChanged) prop.size = size;
    if (massChanged) prop.weight = mass;
    // Every game builds it anew and follows: the next election names the one to simulate it.
    prop.simulator = 0;
    this.send(prop, {
      op: 'propUpdate',
      id: prop.id,
      physics: wirePhysics(prop),
      scale: { ...prop.size },
      mass: prop.weight,
    });
  }

  // Hides a static prop's model (its collision stays) or shows it again; every
  // client rebuilds it.
  setVisible(prop, value) {
    if (typeof value !== 'boolean') {
      throw new TypeError('Prop visible must be a boolean');
    }
    if (this.props.get(prop.id) !== prop || prop.hidden === !value) {
      return;
    }
    if (isPrefab(prop.model)) {
      throw new TypeError('A game prefab cannot be hidden');
    }
    if (!value && prop.collision !== 'static') {
      throw new TypeError('Only a static prop can be hidden');
    }
    prop.hidden = !value;
    this.send(prop, {
      op: 'propUpdate',
      id: prop.id,
      physics: wirePhysics(prop),
      scale: { ...prop.size },
      mass: prop.weight,
    });
  }

  // Whether the player's game shows the prop.
  sees(player, prop) {
    return (
      player.ready === true &&
      this.shownTo(prop, player.id) &&
      this.onPropMap(player, prop) &&
      (prop.dimension === GLOBAL_WORLD || player.dimension === prop.dimension)
    );
  }

  // The nearest player whose game shows the prop, however far (a barrel nobody stood
  // within 60 m of stayed put); null for none.
  nearestSimulator(prop) {
    let best = null;
    let bestDistance = Infinity;
    for (const player of this.host.players.values()) {
      const at = player.position;
      if (!at || !this.sees(player, prop)) {
        continue;
      }
      const distance = Math.hypot(at.x - prop.at.x, at.y - prop.at.y, at.z - prop.at.z);
      if (distance < bestDistance) {
        best = player;
        bestDistance = distance;
      }
    }
    return best;
  }

  // Names the game that simulates each rigid prop: the nearest player's.
  tick(now = Date.now()) {
    if (this.electedAt !== undefined && now - this.electedAt < ELECTION_MS) {
      return;
    }
    this.electedAt = now;
    for (const prop of this.props.values()) {
      this.elect(prop);
    }
  }

  elect(prop) {
    const nearest = prop.collision === 'rigid' ? this.nearestSimulator(prop) : null;
    const next = nearest?.id ?? 0;
    if (next === prop.simulator) {
      return;
    }
    if (prop.simulator !== 0 && this.host.players.has(prop.simulator)) {
      this.host.bridge.send({
        op: 'propSimulate',
        id: prop.id,
        simulate: false,
        to: prop.simulator,
      });
    }
    prop.simulator = next;
    if (next !== 0) {
      this.host.bridge.send({ op: 'propSimulate', id: prop.id, simulate: true, to: next });
    }
  }

  // The client simulating a rigid prop, now: elected at once when it has none yet. 0 for none.
  simulatorOf(prop) {
    if (this.props.get(prop.id) !== prop || prop.collision !== 'rigid') {
      return 0;
    }
    if (prop.simulator === 0 || !this.host.players.has(prop.simulator)) {
      this.elect(prop);
    }
    return prop.simulator;
  }

  // Sends a push to the prop's simulating client, if it has one; otherwise false.
  pushTo(prop, push) {
    const to = this.simulatorOf(prop);
    if (to === 0) {
      return false;
    }
    this.host.bridge.send({
      op: 'propPush',
      id: prop.id,
      linear: ZERO,
      angular: ZERO,
      point: ZERO,
      hasPoint: false,
      ...push,
      to,
    });
    return true;
  }

  applyImpulse(prop, impulse, point, angularImpulse) {
    const linear = plainVector(impulse);
    const angular = angularImpulse === undefined ? ZERO : plainVector(angularImpulse);
    const at = point === undefined ? null : plainVector(point);
    if (
      !linear ||
      !angular ||
      (point !== undefined && !at) ||
      lengthOf(linear) > IMPULSE_MAX ||
      lengthOf(angular) > IMPULSE_MAX ||
      (at && ![at.x, at.y, at.z].every((n) => Math.abs(n) <= WORLD_LIMIT))
    ) {
      return false;
    }
    return this.pushTo(prop, {
      push: 'impulse',
      linear,
      angular,
      ...(at ? { point: at, hasPoint: true } : {}),
    });
  }

  setVelocity(prop, velocity, angularVelocity) {
    const linear = plainVector(velocity);
    const angular = angularVelocity === undefined ? ZERO : plainVector(angularVelocity);
    if (!linear || !angular || lengthOf(linear) > VELOCITY_MAX || lengthOf(angular) > SPIN_MAX) {
      return false;
    }
    return this.pushTo(prop, { push: 'velocity', linear, angular });
  }

  // Puts a rigid prop back where it was spawned, at rest, for every player.
  resetPhysics(prop) {
    if (this.props.get(prop.id) !== prop || prop.collision !== 'rigid') {
      return false;
    }
    prop.at = { ...prop.spawnAt };
    prop.facing = { ...prop.spawnFacing };
    prop.restAt = { ...prop.spawnAt };
    this.send(prop, { op: 'propMove', id: prop.id, position: prop.at, rotation: prop.facing });
    return true;
  }

  // Where the body of a rigid prop a player's game simulates is (op propReport). Taken from the
  // game the prop's simulator only; every other player is shown the pose.
  report(player, { prop: id, position, rotation: facing, resting, hitSpeed }) {
    const prop = this.props.get(id);
    if (!player || !prop || prop.collision !== 'rigid' || prop.simulator !== player.id) {
      return false;
    }
    const at = plainVector(position);
    if (!at || !facing) {
      return false;
    }
    const away = Math.hypot(at.x - prop.restAt.x, at.y - prop.restAt.y, at.z - prop.restAt.z);
    if (away > REST_RANGE) {
      // Put back where it last rested, in every game.
      prop.at = { ...prop.restAt };
      this.send(prop, { op: 'propMove', id: prop.id, position: prop.at, rotation: prop.facing });
      return false;
    }
    prop.at = at;
    prop.facing = { x: facing.x, y: facing.y, z: facing.z, w: facing.w };
    const move = { op: 'propMove', id: prop.id, position: prop.at, rotation: prop.facing };
    for (const other of this.host.players.values()) {
      if (other.id !== player.id && this.sees(other, prop)) {
        this.host.bridge.send({ ...move, to: other.id });
      }
    }
    // It struck something: a barrel breaks when it hits, not after a set time.
    if (typeof hitSpeed === 'number' && Number.isFinite(hitSpeed) && hitSpeed > 0) {
      this.host.emit('propHit', prop, hitSpeed, prop.position);
    }
    if (resting === true) {
      prop.restAt = { ...at };
      this.host.emit('propRest', prop, prop.position, prop.rotation);
    }
    return true;
  }

  // The props a stopping resource made go with it.
  resourceStopped(name) {
    removeOwned(this.props.values(), name, (prop) => this.destroy(prop));
  }

  destroy(prop) {
    if (this.props.get(prop.id) !== prop) {
      return;
    }
    this.host.emit('propDestroy', prop);
    this.props.delete(prop.id);
    this.send(prop, { op: 'propDestroy', id: prop.id });
  }
}

module.exports = { Props, meshOf, physicsOf, isPrefab, WORLD_LIMIT };
