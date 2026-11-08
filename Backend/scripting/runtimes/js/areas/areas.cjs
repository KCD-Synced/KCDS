'use strict';

// Script areas: the server API's Area. A resource builds an area
// (a box, a sphere, or a polygon with a height) with Area.create, and the server raises
// areaEnter(area, entity, matchingDimension) and areaExit(...) when a player, a horse or an
// NPC comes to be inside it or stops being.
//
// The server decides this itself from the positions it already holds, and it does so only when a
// body's position or world changed, and only against the areas registered near that position
// (areas/area_grid.cjs). There is no timer: a body that stands still costs nothing, and so does
// every body while no area exists. What a body is inside is kept per body, so an event is raised
// exactly when that set changes.
//
// A change of an area itself (create, setShape, enabled) is settled once, in a microtask after the
// calling code returns, against every body, and the
// events then arrive after the handle was returned. destroy raises the exits at once, while the
// handle still resolves.
//
// A body counts as inside by geometry alone, in any world. `matchingDimension` says whether the
// area is bound to the body's world (or to none), and the lookups that name a world or a body
// leave out the areas bound to another world.
//
// The level's own areas come from areas/level_areas.cjs once the server names its map
// (setLevel): every TriggerArea prism and SmartAreaShape of trosecko or kutnohorsko, kind 'level',
// in every world. Their shape, name and labels are the level's: only `enabled` can be changed, and
// setShape, destroy and toJSON refuse them. Lookups that list areas give the level's first.
//
// An AreaUnion of the level (a house of several rooms) is a level area of its own: a
// body is inside it while inside one of its members, and `members` lists them. Not covered: `doors`
// and `location` are empty; carts do not exist here yet.
//
// Collision: a script area with `collision` gets invisible walls on its outline that
// every player's game collides with (areas/area_walls.cjs builds the pieces, props/props.cjs shows
// them), and a floor and a ceiling plate when asked (`wallBottom`, `wallTop`; box and
// shape areas). They follow the area's shape, world and `enabled`, and go with it.

const { Vector3, Quaternion } = require('../math.cjs');
const { OWNER, runningResource, removeOwned } = require('../lifecycle/owned_objects.cjs');
const { validWorld } = require('../npcs/npc_values.cjs');
const { AreaGrid } = require('./area_grid.cjs');
const { buildShape, geometryOf, rotationOf } = require('./area_shapes.cjs');
const { doorsOfArea } = require('./area_doors.cjs');
const { AreaKeys } = require('./area_keys.cjs');
const { loadLevelAreas } = require('./level_areas.cjs');
const { onMap } = require('../maps/server_maps.cjs');
const { wallPieces, thicknessOf, DEFAULT_THICKNESS } = require('./area_walls.cjs');

const areaState = Symbol('kcds.areaState');
// How far a body must have moved from where it was last checked to be checked again.
const MOVE_EPSILON = 0.05;
const ID_MAX = 128;
const NAME_MAX = 128;
const LABELS_MAX = 64;
const LABEL_MAX = 64;
const METADATA_MAX = 65536;
// The world of the walls of an area bound to none.
const EVERY_WORLD = 0xffffffff;
// A level area's id: its EntityGuid as sixteen hex digits.
const LEVEL_ID = /^[0-9a-f]{16}$/i;

function labelsOf(value) {
  if (!Array.isArray(value) || value.length > LABELS_MAX) {
    throw new TypeError(`Area labels take up to ${LABELS_MAX} strings`);
  }
  for (const label of value) {
    if (typeof label !== 'string' || label.length === 0 || label.length > LABEL_MAX) {
      throw new TypeError(`An area label is a string of 1 to ${LABEL_MAX} characters`);
    }
  }
  return [...value];
}

// The JSON text of a metadata object, which is how the area keeps it.
function metadataOf(value) {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new TypeError('Area metadata takes an object that JSON can hold');
  }
  let text;
  try {
    text = JSON.stringify(value);
  } catch {
    throw new TypeError('Area metadata takes an object that JSON can hold');
  }
  if (text === undefined || text.length > METADATA_MAX) {
    throw new RangeError(`Area metadata takes up to ${METADATA_MAX} characters of JSON`);
  }
  return text;
}

function worldOrNull(value) {
  if (value === undefined || value === null) {
    return null;
  }
  if (!validWorld(value)) {
    throw new TypeError('Invalid dimension');
  }
  return value;
}

function positionOf(value, name) {
  const finite = (c) => typeof c === 'number' && Number.isFinite(c);
  if (
    value === null ||
    typeof value !== 'object' ||
    !finite(value.x) ||
    !finite(value.y) ||
    !finite(value.z)
  ) {
    throw new TypeError(`${name} takes a position with finite x, y and z`);
  }
  return value;
}

class Areas {
  constructor(host) {
    this.host = host;
    this.grid = new AreaGrid();
    // id -> the area's state, in creation order: the script areas, then the level's own
    this.byId = new Map();
    this.levelById = new Map();
    // The first map's level, once the server named it, and every map's by its index.
    this.level = null;
    this.levels = new Map();
    this.keys = new AreaKeys(host);
    this.byHandle = new Map();
    this.nextHandle = 1;
    // network id -> { id, kind, body, x, y, z, world, placed, areas: Set of states }
    this.bodies = new Map();
    // The areas that changed and wait for the settling pass.
    this.dirty = new Set();
    this.settling = false;
  }

  // The Area class of the server API.
  define() {
    const registry = this;
    class Area {
      constructor(handle) {
        const state = registry.byHandle.get(handle);
        if (!state) {
          throw new Error('Unknown area');
        }
        return state.area;
      }
      get exists() {
        return this[areaState].alive;
      }
      get id() {
        return registry.readable(this) ? this[areaState].id : '';
      }
      get kind() {
        return this[areaState].kind;
      }
      get type() {
        const state = this[areaState];
        return state.kind === 'level' ? state.levelType : state.geometry.type;
      }
      get name() {
        return registry.readable(this) ? this[areaState].name : '';
      }
      set name(value) {
        registry.requireScript(this, 'name');
        if (typeof value !== 'string' || value.length > NAME_MAX) {
          throw new TypeError(`Area name takes a string of up to ${NAME_MAX} characters`);
        }
        this[areaState].name = value;
      }
      get labels() {
        return registry.readable(this) ? [...this[areaState].labels] : [];
      }
      set labels(value) {
        registry.requireScript(this, 'labels');
        this[areaState].labels = labelsOf(value);
      }
      get metadata() {
        return registry.readable(this) ? JSON.parse(this[areaState].metadata) : {};
      }
      set metadata(value) {
        registry.requireScript(this, 'metadata');
        this[areaState].metadata = metadataOf(value);
      }
      get collision() {
        return registry.readable(this) && this[areaState].collision === true;
      }
      set collision(value) {
        registry.requireScript(this, 'collision');
        registry.setCollision(this[areaState], { collision: registry.flagOf(value, 'collision') });
      }
      get wallBottom() {
        return registry.readable(this) && this[areaState].wallBottom === true;
      }
      set wallBottom(value) {
        registry.requireScript(this, 'wallBottom');
        registry.setCollision(this[areaState], { floor: registry.flagOf(value, 'wallBottom') });
      }
      get wallTop() {
        return registry.readable(this) && this[areaState].wallTop === true;
      }
      set wallTop(value) {
        registry.requireScript(this, 'wallTop');
        registry.setCollision(this[areaState], {
          ceiling: registry.flagOf(value, 'wallTop'),
        });
      }
      get wallThickness() {
        const state = this[areaState];
        return state.wallThickness ?? DEFAULT_THICKNESS;
      }
      set wallThickness(value) {
        registry.requireScript(this, 'wallThickness');
        registry.setCollision(this[areaState], { thickness: thicknessOf(value) });
      }
      get enabled() {
        return registry.readable(this) && this[areaState].enabled;
      }
      set enabled(value) {
        registry.requireLive(this);
        if (typeof value !== 'boolean') {
          throw new TypeError('Area enabled takes true or false');
        }
        const state = this[areaState];
        if (state.enabled !== value) {
          state.enabled = value;
          registry.changed(state);
          registry.buildWalls(state);
        }
      }
      get dimension() {
        return registry.readable(this) ? this[areaState].dimension : null;
      }
      set dimension(value) {
        registry.requireScript(this, 'dimension');
        this[areaState].dimension = worldOrNull(value);
        registry.buildWalls(this[areaState]);
      }
      // A level area's is the centre of its bounds.
      get position() {
        if (!registry.readable(this)) {
          return new Vector3(0, 0, 0);
        }
        const state = this[areaState];
        if (state.kind === 'level') {
          const { min, max } = state.shape.bounds;
          return new Vector3((min.x + max.x) / 2, (min.y + max.y) / 2, (min.z + max.z) / 2);
        }
        return state.geometry.position.clone();
      }
      set position(value) {
        registry.requireScript(this, 'position');
        positionOf(value, 'Area.position');
        registry.reshape(this[areaState], { position: value });
      }
      get rotation() {
        return registry.readable(this)
          ? this[areaState].geometry.rotation.clone()
          : new Quaternion(1, 0, 0, 0);
      }
      set rotation(value) {
        registry.requireScript(this, 'rotation');
        rotationOf(value);
        registry.reshape(this[areaState], { rotation: value });
      }
      get height() {
        if (!registry.readable(this)) {
          return 0;
        }
        const { geometry } = this[areaState];
        if (geometry.type === 'shape') {
          return geometry.height;
        }
        return geometry.type === 'box' ? geometry.max.z - geometry.min.z : 0;
      }
      get location() {
        return '';
      }
      get bounds() {
        const empty = new Vector3(0, 0, 0);
        if (!registry.readable(this)) {
          return { min: empty, max: empty.clone() };
        }
        const { bounds } = this[areaState].shape;
        return { min: bounds.min.clone(), max: bounds.max.clone() };
      }
      // A union's member areas; empty for any other area.
      get members() {
        const { unionOf } = this[areaState];
        return unionOf ? unionOf.map((member) => member.area) : [];
      }
      // The key that works this area's doors and chests: an item class id, '' for none.
      // Assign an item class by name or GUID, or null to take it away.
      get lockKey() {
        return registry.keys.keyOf(this);
      }
      set lockKey(value) {
        registry.keys.set(this, value);
      }
      // Locks or unlocks every door and chest of the area; how many it reached.
      setLocked(locked) {
        return registry.keys.setLocked(this, locked);
      }
      // The doors the level links to this area, with how: empty for an area a script made.
      get doors() {
        return doorsOfArea(registry.levelOf(this[areaState]), this.id);
      }
      get players() {
        return registry.occupantsOf(this, 'player');
      }
      get occupants() {
        return registry.occupantsOf(this);
      }
      toString() {
        const state = this[areaState];
        return `Area(${state.id}, ${state.kind}, ${this.type}, ${state.name})`;
      }
      // Whether a position is inside, enabled or not and in whatever world.
      contains(position) {
        positionOf(position, 'Area.contains');
        if (!registry.readable(this)) {
          return false;
        }
        return this[areaState].shape.contains(position.x, position.y, position.z);
      }
      // Whether a player, horse or NPC stands inside right now, in the area's world.
      holdsEntity(body) {
        const there = registry.bodyPlace(body, 'Area.holdsEntity');
        if (!registry.readable(this)) {
          return false;
        }
        const state = this[areaState];
        const { x, y, z } = there.position;
        const sameMap = (state.mapIndex ?? 0) === there.mapIndex;
        return sameMap && registry.inWorld(state, there.world) && state.shape.contains(x, y, z);
      }
      // The level's own herbs inside the area that are there to pick in a world: the
      // area's world by default, the global one for an area in every world. Empty for a destroyed
      // area or another world than the area's.
      herbsWithin(dimension) {
        const world = registry.herbWorld(this, dimension, 'Area.herbsWithin');
        if (world === null) {
          return [];
        }
        const gathering = registry.host.gathering;
        return gathering.available(world, registry.herbsInside(this[areaState]));
      }
      // Takes the level's own herbs inside the area away in a world, picked ones too, until a
      // respawn; no items, experience or harvest events. How many were not removed already.
      clearHerbs(dimension) {
        const world = registry.herbWorld(this, dimension, 'Area.clearHerbs');
        if (world === null) {
          return 0;
        }
        const gathering = registry.host.gathering;
        return gathering.clearHerbs(world, registry.herbsInside(this[areaState]));
      }
      // Brings back the picked and removed plants inside the area in a world at once. How many
      // picks and removals ended.
      regrowHerbs(dimension) {
        const world = registry.herbWorld(this, dimension, 'Area.regrowHerbs');
        if (world === null) {
          return 0;
        }
        const state = this[areaState];
        return registry.host.gathering.grow(world, (pick) =>
          state.shape.contains(pick.position.x, pick.position.y, pick.position.z),
        );
      }
      // Moves, turns or reshapes the area; the fields left out keep their value. False for a level
      // area, which the level shapes.
      setShape(definition) {
        if (definition === null || typeof definition !== 'object') {
          throw new TypeError('Area.setShape takes an object of shape fields');
        }
        if (!registry.readable(this) || this[areaState].kind === 'level') {
          return false;
        }
        return registry.reshape(this[areaState], definition);
      }
      // Raises the exits of everyone inside, then removes the area. False when it is gone already
      // or is a level area.
      destroy() {
        if (!registry.readable(this) || this[areaState].kind === 'level') {
          return false;
        }
        registry.remove(this[areaState]);
        return true;
      }
      // An Area.create definition that recreates this area; null for a level area, which
      // the level always builds.
      toJSON() {
        if (!registry.readable(this) || this[areaState].kind === 'level') {
          return null;
        }
        return registry.definitionOf(this[areaState]);
      }
      static create(definition) {
        return registry.create(definition);
      }
      // A script area's id, or a level area's EntityGuid as sixteen hex digits.
      static getById(id) {
        if (typeof id !== 'string') {
          return null;
        }
        const state = registry.byId.get(id) ?? registry.levelById.get(id.toLowerCase());
        return state?.area ?? null;
      }
      // Every enabled area containing a position; with a world, those bound to another one are
      // left out.
      static getAt(position, dimension) {
        positionOf(position, 'Area.getAt');
        const world = dimension === undefined ? undefined : worldOrNull(dimension);
        return registry.at(position, world);
      }
      // The enabled areas currently containing the body, in its dimension.
      static aroundEntity(body) {
        const there = registry.bodyPlace(body, 'Area.aroundEntity');
        return registry.at(there.position, there.world, there.mapIndex);
      }
      // Areas with bounds inside the radius of a position, closest first, enabled or not.
      static nearby(position, radius, dimension) {
        positionOf(position, 'Area.nearby');
        if (typeof radius !== 'number' || Number.isNaN(radius) || radius < 0) {
          throw new RangeError('Area.nearby takes a radius of 0 or more');
        }
        const world = dimension === undefined ? undefined : worldOrNull(dimension);
        return registry.near(position, radius, world);
      }
      // Every area carrying a label, compared without regard to case.
      static taggedWith(label) {
        if (typeof label !== 'string') {
          throw new TypeError('Area.taggedWith takes a label string');
        }
        const wanted = label.toLowerCase();
        const found = [];
        for (const state of registry.states()) {
          if (state.labels.some((own) => own.toLowerCase() === wanted)) {
            found.push(state.area);
          }
        }
        return found;
      }
      // Every area the server knows, the level's first.
      static all(kind) {
        if (kind !== undefined && kind !== 'level' && kind !== 'script') {
          throw new TypeError("Area.all takes 'level', 'script' or nothing");
        }
        const found = [];
        if (kind !== 'script') {
          for (const state of registry.levelById.values()) {
            found.push(state.area);
          }
        }
        if (kind !== 'level') {
          for (const state of registry.byId.values()) {
            found.push(state.area);
          }
        }
        return found;
      }
    }
    this.Area = Area;
    return Area;
  }

  // Whether the handle's area still exists.
  readable(area) {
    return area[areaState].alive;
  }

  requireLive(area) {
    if (!area[areaState].alive) {
      throw new Error('The area was destroyed');
    }
  }

  // What only a script area lets a script change.
  requireScript(area, field) {
    this.requireLive(area);
    if (area[areaState].kind === 'level') {
      throw new TypeError(`A level area's ${field} is the level's and cannot be changed`);
    }
  }

  // Every area, the level's first.
  *states() {
    yield* this.levelById.values();
    yield* this.byId.values();
  }

  // A map of the server (its hostSettings; the first by default): the level areas of that map
  // become that level's. A body inside one that goes away hears its exit; one inside a new one
  // hears its enter after this returns. A server of several maps calls it once for each.
  setLevel(level, entries, mapIndex = 0) {
    if (typeof level !== 'string' || level === this.levels.get(mapIndex)) {
      return;
    }
    for (const state of [...this.levelById.values()]) {
      if (state.mapIndex === mapIndex) {
        this.remove(state);
      }
    }
    this.levels.set(mapIndex, level);
    if (mapIndex === 0) {
      this.level = level;
    }
    const loaded = entries ?? loadLevelAreas(level);
    for (const entry of loaded) {
      if (entry.type !== 'AreaUnion') {
        this.addLevelArea(entry, mapIndex, level);
      }
    }
    // Unions last: their members are areas of the same level.
    for (const entry of loaded) {
      if (entry.type === 'AreaUnion') {
        this.addLevelUnion(entry, mapIndex, level);
      }
    }
  }

  // The level an area belongs to: its own for a level area, the first map's for a script's.
  levelOf(state) {
    return state.level ?? this.level;
  }

  // The level of an area by its id (a level area's GUID, or a script area's id).
  levelOfAreaId(id) {
    const state = this.levelById.get(String(id).toLowerCase()) ?? this.byId.get(id);
    return state ? this.levelOf(state) : this.level;
  }

  // A union of the level's areas: inside while inside one of its members, whether
  // that member is enabled or not; its bounds hold all of theirs.
  addLevelUnion(entry, mapIndex = 0, level = this.level) {
    if (this.levelById.has(entry.id)) {
      return;
    }
    const unionOf = entry.members
      .map((id) => this.levelById.get(id))
      .filter((member) => member && !member.unionOf);
    if (!unionOf.length) {
      return;
    }
    const min = unionOf[0].shape.bounds.min.clone();
    const max = unionOf[0].shape.bounds.max.clone();
    for (const member of unionOf) {
      const bounds = member.shape.bounds;
      min.x = Math.min(min.x, bounds.min.x);
      min.y = Math.min(min.y, bounds.min.y);
      min.z = Math.min(min.z, bounds.min.z);
      max.x = Math.max(max.x, bounds.max.x);
      max.y = Math.max(max.y, bounds.max.y);
      max.z = Math.max(max.z, bounds.max.z);
    }
    const state = {
      handle: this.nextHandle,
      id: entry.id,
      kind: 'level',
      levelType: entry.type,
      name: entry.name,
      labels: [...entry.labels],
      metadata: '{}',
      enabled: true,
      dimension: null,
      geometry: { type: 'union' },
      shape: {
        bounds: { min, max },
        contains: (x, y, z) => unionOf.some((member) => member.shape.contains(x, y, z)),
      },
      unionOf,
      mapIndex,
      level,
      occupants: new Set(),
      alive: true,
      area: null,
    };
    this.nextHandle += 1;
    this.register(state);
  }

  // One area of the level catalog (areas/level_areas.cjs): world points, so no position or turn.
  addLevelArea(entry, mapIndex = 0, level = this.level) {
    if (this.levelById.has(entry.id)) {
      return;
    }
    const geometry = {
      type: 'shape',
      position: new Vector3(0, 0, 0),
      rotation: new Quaternion(1, 0, 0, 0),
      points: entry.points,
      height: entry.height,
    };
    const state = {
      handle: this.nextHandle,
      id: entry.id,
      kind: 'level',
      levelType: entry.type,
      name: entry.name,
      labels: [...entry.labels],
      metadata: '{}',
      enabled: true,
      dimension: null,
      geometry,
      shape: buildShape(geometry),
      mapIndex,
      level,
      occupants: new Set(),
      alive: true,
      area: null,
    };
    this.nextHandle += 1;
    this.register(state);
  }

  // A collision flag: true or false (false when left out).
  flagOf(value, field) {
    if (value === undefined) {
      return false;
    }
    if (typeof value !== 'boolean') {
      throw new TypeError(`Area ${field} takes true or false`);
    }
    return value;
  }

  // The collision fields of a definition, checked against its geometry when collision is on.
  collisionOf(definition, geometry) {
    const options = {
      collision: this.flagOf(definition.collision, 'collision'),
      floor: this.flagOf(definition.wallBottom, 'wallBottom'),
      ceiling: this.flagOf(definition.wallTop, 'wallTop'),
      thickness: thicknessOf(definition.wallThickness),
    };
    if (options.collision) {
      wallPieces(geometry, options);
    }
    return options;
  }

  // The walls' options of an area.
  wallOptions(state) {
    return {
      floor: state.wallBottom,
      ceiling: state.wallTop,
      thickness: state.wallThickness,
    };
  }

  // Changes collision fields; throws (and keeps the old ones) when the shape cannot take them.
  setCollision(state, change) {
    const next = {
      collision: change.collision ?? state.collision,
      floor: change.floor ?? state.wallBottom,
      ceiling: change.ceiling ?? state.wallTop,
      thickness: change.thickness ?? state.wallThickness,
    };
    if (next.collision) {
      wallPieces(state.geometry, next);
    }
    state.collision = next.collision;
    state.wallBottom = next.floor;
    state.wallTop = next.ceiling;
    state.wallThickness = next.thickness;
    this.buildWalls(state);
  }

  // Takes an area's walls down and puts up those its state asks for now.
  buildWalls(state) {
    const props = this.host.props;
    for (const id of state.walls ?? []) {
      props.destroyWall(id);
    }
    state.walls = [];
    if (!state.alive || !state.enabled || !state.collision || state.kind === 'level') {
      return;
    }
    const world = state.dimension ?? EVERY_WORLD;
    for (const piece of wallPieces(state.geometry, this.wallOptions(state))) {
      state.walls.push(props.spawnWall(piece, world));
    }
  }

  create(definition) {
    if (definition === null || typeof definition !== 'object') {
      throw new TypeError('Area.create takes a definition object');
    }
    const { id } = definition;
    if (typeof id !== 'string' || id.length === 0 || id.length > ID_MAX) {
      throw new TypeError(`Area id takes a string of 1 to ${ID_MAX} characters`);
    }
    if (LEVEL_ID.test(id)) {
      throw new RangeError(
        'Area id may not be sixteen hex digits, which is how a level area reads',
      );
    }
    if (this.byId.has(id)) {
      throw new Error(`Area id ${id} is already taken`);
    }
    const name = definition.name === undefined ? id : definition.name;
    if (typeof name !== 'string' || name.length > NAME_MAX) {
      throw new TypeError(`Area name takes a string of up to ${NAME_MAX} characters`);
    }
    const geometry = geometryOf(definition);
    const collision = this.collisionOf(definition, geometry);
    if (definition.enabled !== undefined && typeof definition.enabled !== 'boolean') {
      throw new TypeError('Area enabled takes true or false');
    }
    const state = {
      handle: this.nextHandle,
      id,
      kind: 'script',
      [OWNER]: runningResource(this.host),
      name,
      labels: definition.labels === undefined ? [] : labelsOf(definition.labels),
      metadata: definition.metadata === undefined ? '{}' : metadataOf(definition.metadata),
      enabled: definition.enabled !== false,
      dimension: worldOrNull(definition.dimension),
      geometry,
      shape: buildShape(geometry),
      occupants: new Set(),
      alive: true,
      area: null,
      collision: collision.collision,
      wallBottom: collision.floor,
      wallTop: collision.ceiling,
      wallThickness: collision.thickness,
      walls: [],
    };
    this.nextHandle += 1;
    const area = this.register(state);
    this.buildWalls(state);
    return area;
  }

  // Gives a new area's state its handle object and enters it in the lookups and the grid.
  register(state) {
    const area = Object.create(this.Area.prototype);
    Object.defineProperty(area, 'handle', { value: state.handle });
    area[areaState] = state;
    state.area = area;
    if (state.kind === 'level') {
      this.levelById.set(state.id, state);
    } else {
      this.byId.set(state.id, state);
    }
    this.byHandle.set(state.handle, state);
    this.grid.add(state, state.shape.bounds);
    this.changed(state);
    return area;
  }

  // Moves, turns or reshapes an area. False for a shape that cannot be built.
  reshape(state, fields) {
    let geometry;
    try {
      geometry = geometryOf(fields, state.geometry);
    } catch (error) {
      if (error instanceof TypeError || error instanceof RangeError) {
        return false;
      }
      throw error;
    }
    if (state.collision) {
      try {
        wallPieces(geometry, this.wallOptions(state));
      } catch (error) {
        if (error instanceof RangeError) {
          return false;
        }
        throw error;
      }
    }
    state.geometry = geometry;
    state.shape = buildShape(geometry);
    this.grid.add(state, state.shape.bounds);
    this.changed(state);
    this.buildWalls(state);
    return true;
  }

  // The definition that builds the area again.
  definitionOf(state) {
    const { geometry } = state;
    const definition = {
      id: state.id,
      name: state.name,
      type: geometry.type,
      position: geometry.position.clone(),
      rotation: geometry.rotation.clone(),
    };
    if (geometry.type === 'box') {
      definition.min = geometry.min.clone();
      definition.max = geometry.max.clone();
    } else if (geometry.type === 'sphere') {
      definition.radius = geometry.radius;
    } else {
      definition.points = geometry.points.map((point) => point.clone());
      definition.height = geometry.height;
    }
    definition.labels = [...state.labels];
    definition.metadata = JSON.parse(state.metadata);
    if (state.dimension !== null) {
      definition.dimension = state.dimension;
    }
    if (state.collision) {
      definition.collision = true;
      definition.wallBottom = state.wallBottom;
      definition.wallTop = state.wallTop;
      definition.wallThickness = state.wallThickness;
    }
    definition.enabled = state.enabled;
    return definition;
  }

  // The areas a stopping resource made go with it.
  resourceStopped(name) {
    removeOwned(this.byId.values(), name, (state) => this.remove(state));
  }

  // Raises the exits of everyone inside while the handle still resolves, then removes the area.
  remove(state) {
    const leaving = [...state.occupants];
    for (const record of leaving) {
      this.unlink(state, record);
    }
    for (const record of leaving) {
      this.raise('areaExit', state, record);
    }
    state.alive = false;
    this.buildWalls(state);
    this.grid.remove(state);
    if (state.kind === 'level') {
      this.levelById.delete(state.id);
    } else {
      this.byId.delete(state.id);
    }
    this.byHandle.delete(state.handle);
    this.dirty.delete(state);
  }

  // An area changed: every body is checked against it once, after the calling code returns.
  changed(state) {
    this.dirty.add(state);
    if (!this.settling) {
      this.settling = true;
      queueMicrotask(() => this.settle());
    }
  }

  // Checks every body against the areas that changed since the last pass.
  settle() {
    this.settling = false;
    const states = [...this.dirty];
    this.dirty.clear();
    for (const state of states) {
      if (state.alive) {
        this.refresh(state);
      }
    }
  }

  // Makes one area's occupants those bodies that are inside it now.
  refresh(state) {
    const inside = new Set();
    if (state.enabled) {
      for (const record of this.bodies.values()) {
        const sameMap = record.mapIndex === (state.mapIndex ?? 0);
        if (record.placed && sameMap && state.shape.contains(record.x, record.y, record.z)) {
          inside.add(record);
        }
      }
    }
    const exits = [...state.occupants].filter((record) => !inside.has(record));
    const enters = [...inside].filter((record) => !state.occupants.has(record));
    for (const record of exits) {
      this.unlink(state, record);
    }
    for (const record of enters) {
      this.link(state, record);
    }
    for (const record of exits) {
      this.raise('areaExit', state, record);
    }
    for (const record of enters) {
      this.raise('areaEnter', state, record);
    }
  }

  link(state, record) {
    state.occupants.add(record);
    record.areas.add(state);
  }

  unlink(state, record) {
    state.occupants.delete(record);
    record.areas.delete(state);
  }

  inWorld(state, world) {
    return state.dimension === null || state.dimension === world;
  }

  // The world an area's herbs are asked about: the one given, else the area's own,
  // else the global one (an area in every world). Null for a destroyed area or a world the area is
  // not in; an invalid world throws.
  herbWorld(area, dimension, where) {
    if (dimension !== undefined && !validWorld(dimension)) {
      throw new TypeError(`${where}: dimension must be a whole number from 0 to 4294967295`);
    }
    if (!this.readable(area)) {
      return null;
    }
    const state = area[areaState];
    const world = dimension ?? state.dimension ?? EVERY_WORLD;
    // On the area's map: the herbs' world there is the native value of it.
    return this.inWorld(state, world) ? onMap(state.mapIndex ?? 0, world) : null;
  }

  // The level's own plants (gathering/authored_herbs.cjs) whose places lie inside the area now.
  herbsInside(state) {
    const herbs = this.host.gathering.herbs(state.mapIndex ?? 0);
    const { min, max } = state.shape.bounds;
    return herbs.inBox(min.x, min.y, max.x, max.y).filter((index) => {
      const at = herbs.positionOf(index);
      return state.shape.contains(at.x, at.y, at.z);
    });
  }

  raise(name, state, record) {
    if (!state.alive && name === 'areaEnter') {
      return;
    }
    this.host.emit(name, state.area, record.body, this.inWorld(state, record.world));
  }

  // A player, horse or NPC is at `position` in `world` on a map (an index of World.maps): the host
  // calls this whenever it learns a new position, world or map of a body, and the areas raise what
  // changed. `kind` is 'player', 'horse' or 'npc'.
  touch(kind, body, position, world, mapIndex = 0) {
    let record = this.bodies.get(body.id);
    if (!record) {
      record = {
        id: body.id,
        kind,
        body,
        x: 0,
        y: 0,
        z: 0,
        world,
        mapIndex,
        placed: false,
        areas: new Set(),
      };
      this.bodies.set(body.id, record);
    }
    const dx = position.x - record.x;
    const dy = position.y - record.y;
    const dz = position.z - record.z;
    const moved = dx * dx + dy * dy + dz * dz > MOVE_EPSILON * MOVE_EPSILON;
    const changedWorld = world !== record.world || mapIndex !== record.mapIndex;
    if (record.placed && !moved && !changedWorld) {
      return;
    }
    record.x = position.x;
    record.y = position.y;
    record.z = position.z;
    record.world = world;
    record.mapIndex = mapIndex;
    record.placed = true;
    // With no area anywhere there is nothing to compare.
    if (this.byId.size === 0 && this.levelById.size === 0 && record.areas.size === 0) {
      return;
    }
    this.place(record);
  }

  // Compares where a body is with what it was inside, and raises the exits and then the enters.
  place(record) {
    const inside = this.containing(record.x, record.y, record.z, record.mapIndex);
    const exits = [];
    for (const state of record.areas) {
      if (!inside.includes(state)) {
        exits.push(state);
      }
    }
    const enters = inside.filter((state) => !record.areas.has(state));
    for (const state of exits) {
      this.unlink(state, record);
    }
    for (const state of enters) {
      this.link(state, record);
    }
    for (const state of exits) {
      this.raise('areaExit', state, record);
    }
    for (const state of enters) {
      this.raise('areaEnter', state, record);
    }
  }

  // The enabled areas that contain a point, whatever their world; on one map when `mapIndex` is
  // given (the areas of the other maps lie at the same coordinates of another level).
  containing(x, y, z, mapIndex) {
    const found = [];
    const onMap = (state) => mapIndex === undefined || (state.mapIndex ?? 0) === mapIndex;
    for (const state of this.grid.near(x, y)) {
      if (state.enabled && onMap(state) && state.shape.contains(x, y, z)) {
        found.push(state);
      }
    }
    for (const state of this.grid.wide) {
      if (state.enabled && onMap(state) && state.shape.contains(x, y, z)) {
        found.push(state);
      }
    }
    return found;
  }

  // A body that left the server (a disconnect, a despawn): its areas lose it and raise nothing.
  forget(id) {
    const record = this.bodies.get(id);
    if (!record) {
      return;
    }
    for (const state of [...record.areas]) {
      this.unlink(state, record);
    }
    this.bodies.delete(id);
  }

  // Area.getAt and Area.aroundEntity: the enabled areas at a point, those bound to another world
  // left out when a world is given.
  at(position, world, mapIndex) {
    const found = this.containing(position.x, position.y, position.z, mapIndex);
    return found
      .filter((state) => world === undefined || world === null || this.inWorld(state, world))
      .map((state) => state.area);
  }

  near(position, radius, world) {
    const found = [];
    for (const state of this.states()) {
      if (world !== undefined && world !== null && !this.inWorld(state, world)) {
        continue;
      }
      const { min, max } = state.shape.bounds;
      const gapX = Math.max(min.x - position.x, 0, position.x - max.x);
      const gapY = Math.max(min.y - position.y, 0, position.y - max.y);
      const gapZ = Math.max(min.z - position.z, 0, position.z - max.z);
      const distance = Math.hypot(gapX, gapY, gapZ);
      if (distance <= radius) {
        found.push({ area: state.area, distance });
      }
    }
    found.sort((a, b) => a.distance - b.distance);
    return found.map((entry) => entry.area);
  }

  // The position and world of a player, horse or NPC the server holds.
  bodyPlace(body, name) {
    const known =
      body !== null &&
      typeof body === 'object' &&
      (this.host.players.get(body.id) === body ||
        this.host.horses.get(body.id) === body ||
        this.host.npcs.npcs.get(body.id) === body);
    if (!known) {
      throw new TypeError(`${name} takes a Player, Horse or Npc the server holds`);
    }
    // Its map, as the host last told the areas (the first map for a body they have not met).
    const mapIndex = this.bodies.get(body.id)?.mapIndex ?? 0;
    return { position: body.position, world: body.dimension, mapIndex };
  }

  occupantsOf(area, kind) {
    if (!this.readable(area)) {
      return [];
    }
    const found = [];
    for (const record of area[areaState].occupants) {
      if (kind === undefined || record.kind === kind) {
        found.push(record.body);
      }
    }
    return found;
  }
}

module.exports = { Areas };
