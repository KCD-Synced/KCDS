'use strict';

// Stacks of items lying in the world: the server API's GroundItem. The server holds
// every stack; each is a NetworkGroundItem entity that every client in view lays as the game's own
// pickable stack (Client/runtime/ground). A stack is one entity however many units it holds.
//
// A player takes a stack up with the game's own pick-up: their game leaves it lying and asks the
// server, which adds its units to their inventory (as Inventory.add, reason 'add') and, when they
// fit, raises groundItemPickup and destroys the stack for everyone. Their game shows its own notice
// of the units received.
//
// A player drops units in their inventory screen: their game keeps them and asks the server, which
// takes them out of their inventory (reason 'drop') and lays them at the player's feet, dropped by
// them: one stack, or one per unit of a class that does not stack.
//
// A stack is laid where the server puts it and the game's physics lets it fall and roll from there
// Until a game reports where it came to rest, it is not `resting`: the first
// plausible report (rest) is where it lies from then on, on every client, and raises
// groundItemResting. A landed projectile lies where the shooter's game saw it stop: at rest.
//
// A carryable stack (`carryable`) is held in the arms instead of going to the
// inventory, and must be a single item with a carry animation in the game. Taking it up with the game's
// pick-up is a carry (carry/player_carry.cjs), never an inventory; a carry's put-down lays one.
//
// A shot's projectile that lands lies as a stack of one where the shooter's game says
// it landed, dropped by the shooter: every client takes its own copy out of its world, so the
// server's is the only one and one player takes it up. Ammunition taken up goes back into the
// quiver. At most LANDED_LIMIT of them lie at once: the oldest goes when another lands.

// How far from a stack a player's body may take it up, in metres.
const PICKUP_REACH = 6;
// How far a stack may fall or roll from where it was laid, and rise above it, before a report of
// its rest is not believed (metres).
const REST_REACH = 25;
const REST_RISE = 0.5;
// Where a stack lies and whether it came to rest: { at, facing, resting }, which rest changes.
const POSE = Symbol('kcds.groundItemPose');
// The one player who sees the stack (showOnlyTo): { playerId }, 0 for everyone.
// The server sends the stack's entity to that player's game only, and only they may take it up.
const AUDIENCE = Symbol('kcds.groundItemAudience');
// Landed projectiles lying at once.
const LANDED_LIMIT = 200;

const {
  classBytesHex,
  invisibleOnGround,
  lookupItemClass,
  qualityBand,
} = require('../inventory/catalog.cjs');
const { OWNER, removeOwned } = require('../lifecycle/owned_objects.cjs');
const { rowLine } = require('../inventory/commit_guards.cjs');
const { onMap } = require('../maps/server_maps.cjs');
const { carryClassOf } = require('../carry/player_carry.cjs');
const { Quaternion, Vector3, rotation, vector } = require('../math.cjs');

const GLOBAL_WORLD = 0xffffffff;
const MAX_AMOUNT = 10000;
// The longest name a script gives a stack.
const NAME_MAX = 128;
// A stack farther out than this lies outside any level.
const WORLD_LIMIT = 1e6;

function validWorld(world) {
  return Number.isSafeInteger(world) && world >= 0 && world <= GLOBAL_WORLD;
}

function finiteVector(value) {
  return [value.x, value.y, value.z].every((n) => Number.isFinite(n) && Math.abs(n) <= WORLD_LIMIT);
}

// { quality, health, condition } a stack is made with: 0 and -1 leave them to the class; ammo
// is made at quality 1 and full health unless told otherwise, as a client would build it.
function propertiesOf(itemClass, properties) {
  if (properties === null || typeof properties !== 'object' || Array.isArray(properties)) {
    throw new TypeError('GroundItem properties must be an object');
  }
  const ammo = itemClass.category === 'ammo';
  let quality = properties.quality ?? (ammo ? 1 : 0);
  if (!Number.isInteger(quality) || quality < 0 || quality > itemClass.maximumQuality) {
    throw new RangeError(`quality must be 1 to ${itemClass.maximumQuality}`);
  }
  let health = properties.health ?? (ammo ? 1 : -1);
  if (health !== -1 && !(typeof health === 'number' && health >= 0 && health <= 1)) {
    throw new RangeError('health must be 0 to 1');
  }
  let condition = -1;
  if (properties.condition !== undefined) {
    condition = properties.condition;
    if (!(typeof condition === 'number' && condition >= 0 && condition <= 1)) {
      throw new RangeError('condition must be 0 to 1');
    }
    const band = qualityBand(itemClass, quality || 1);
    const fromCondition = band.low + condition * (band.high - band.low);
    if (properties.health !== undefined && Math.abs(fromCondition - health) > 1e-4) {
      throw new RangeError('health and condition disagree');
    }
    health = fromCondition;
    quality = quality || 1;
  } else if (health !== -1 && quality > 0) {
    const band = qualityBand(itemClass, quality);
    condition = band.high > band.low ? (health - band.low) / (band.high - band.low) : 1;
    condition = Math.min(1, Math.max(0, condition));
  }
  return { quality, health, condition };
}

// A dropped weapon lies on its side: a weapon model stands on its point at the identity rotation
// (a knocked-out player's sword stood upright in the grass), so it is turned a
// quarter about the x axis. Other items keep the identity.
const LYING_WEAPON = Object.freeze({ x: Math.SQRT1_2, y: 0, z: 0, w: Math.SQRT1_2 });

function dropOrientation(itemClass) {
  if (itemClass.category === 'meleeWeapon' || itemClass.category === 'missileWeapon') {
    return LYING_WEAPON;
  }
  return {};
}

class GroundItems {
  // `host`: the resource host (bridge, emit, players, allocateEntityId, the running resource).
  constructor(host) {
    this.host = host;
    // id -> GroundItem
    this.stacks = new Map();
    // The ids of the landed projectiles' stacks, oldest first.
    this.landedStacks = [];
  }

  // The GroundItem class of the server API, an Entity.
  define(Entity) {
    const registry = this;
    const host = this.host;
    class GroundItem {
      constructor(id) {
        const stack = registry.stacks.get(id);
        if (!stack) {
          throw new Error('Unknown ground item');
        }
        return stack;
      }
      static spawn(
        item,
        position = {},
        orientation = {},
        amount = 1,
        dimension = GLOBAL_WORLD,
        properties = {},
        map = undefined,
      ) {
        // `map`: one of World.maps, the first by default; only the players on it see the stack.
        return registry.spawn(GroundItem, {
          item,
          position,
          orientation,
          amount,
          dimension,
          properties,
          mapIndex: host.mapIndexOf(map, 'GroundItem.spawn'),
        });
      }
      static all() {
        return [...registry.stacks.values()];
      }
      static getById(id) {
        return registry.stacks.get(id) ?? null;
      }
      static destroyAll(dimension) {
        if (dimension !== undefined && !validWorld(dimension)) {
          throw new TypeError('Invalid dimension');
        }
        let removed = 0;
        for (const stack of [...registry.stacks.values()]) {
          if (dimension === undefined || stack.dimension === dimension) {
            registry.destroy(stack);
            removed += 1;
          }
        }
        return removed;
      }
      get position() {
        return new Vector3(this.at.x, this.at.y, this.at.z);
      }
      get rotation() {
        return new Quaternion(this.facing.w, this.facing.x, this.facing.y, this.facing.z);
      }
      // Whether the stack came to rest where a game's physics laid it.
      get resting() {
        return this[POSE].resting;
      }
      get leftBy() {
        return host.players.get(this.droppedById) ?? null;
      }
      // The one player who sees it, or null when everyone does.
      get visibleTo() {
        const { playerId } = this[AUDIENCE];
        return playerId === 0 ? null : host.players.get(playerId) ?? null;
      }
      // Shows it to this player only (their game alone lays it, and only they take it up), or to
      // everyone again with null.
      showOnlyTo(player) {
        registry.setAudience(this, registry.audienceOf(player));
      }
      destroy() {
        registry.destroy(this);
      }
      toString() {
        const state = this.resting ? 'resting' : 'settling';
        return `GroundItem(${this.id}, ${this.itemClass} x${this.amount}, ${state})`;
      }
    }
    Object.setPrototypeOf(GroundItem.prototype, Entity.prototype);
    // The stacks players drop are made with the first API's class.
    this.GroundItem ??= GroundItem;
    return GroundItem;
  }

  // A carryable prop a player's carry let go of, where it lies.
  layCarryable(item, position, player) {
    if (!this.GroundItem) {
      this.host.makeAPI();
    }
    return this.spawn(this.GroundItem, {
      item,
      position,
      orientation: {},
      amount: 1,
      dimension: player.dimension,
      properties: { carryable: true },
      droppedById: player.id,
      mapIndex: this.host.playerMapIndex(player),
    });
  }

  // A stack a player dropped, with the row's own properties.
  lay(request, { dimension, droppedById, props, mapIndex = 0 }) {
    if (!this.GroundItem) {
      this.host.makeAPI();
    }
    const itemClass = lookupItemClass(request.item);
    const properties = {};
    if (itemClass.qualityBands || props.quality > 1) {
      properties.quality = props.quality;
      properties.health = props.health;
    }
    return this.spawn(this.GroundItem, {
      ...request,
      dimension,
      properties,
      droppedById,
      mapIndex,
    });
  }

  spawn(
    GroundItem,
    {
      item,
      position,
      orientation,
      amount,
      dimension,
      properties,
      droppedById,
      resting = false,
      pinned = false,
      mapIndex = 0,
    },
  ) {
    const owner = this.host.context.getStore();
    if (
      owner &&
      (this.host.resources.get(owner.name) !== owner || !owner.active || owner.failing)
    ) {
      throw new Error('Resource is stopped');
    }
    const carryable = !!properties?.carryable;
    const itemClass = carryable ? carryClassOf(item) : lookupItemClass(item);
    if (!itemClass) {
      const kind = carryable ? 'carryable item' : 'item class';
      throw new TypeError(`Unknown ${kind}: ${String(item)}`);
    }
    if (!carryable && invisibleOnGround(itemClass)) {
      throw new TypeError(
        `${itemClass.name} has no model to lie on the ground (a worn class whose clothing gives none)`,
      );
    }
    if (carryable && amount !== 1) {
      throw new RangeError('A carryable ground item is a single item');
    }
    if (!Number.isInteger(amount) || amount < 1 || amount > MAX_AMOUNT) {
      throw new RangeError(`amount must be 1 to ${MAX_AMOUNT}`);
    }
    if (amount > 1 && !itemClass.stackable) {
      throw new RangeError(`${itemClass.name} does not stack: lay its units one by one`);
    }
    if (!validWorld(dimension)) {
      throw new TypeError('Invalid dimension');
    }
    const at = vector(position);
    if (!finiteVector(at)) {
      throw new RangeError('Invalid ground item position');
    }
    const facing = rotation(orientation);
    const { quality, health, condition } = propertiesOf(itemClass, properties ?? {});
    // A name the script gives the stack to find it again; empty for none.
    const name = properties?.name ?? '';
    // Shown to one player only.
    const audience = this.audienceOf(properties?.visibleTo ?? null);
    if (typeof name !== 'string' || name.length > NAME_MAX) {
      throw new TypeError(`A ground item's name is a string of up to ${NAME_MAX} characters`);
    }
    const stack = Object.create(GroundItem.prototype);
    Object.defineProperty(stack, 'id', { value: this.host.allocateEntityId(), enumerable: true });
    Object.assign(stack, {
      itemClass: classBytesHex(itemClass),
      amount,
      quality,
      health,
      condition,
      carryable,
      droppedById: droppedById ?? 0,
      dimension,
      name,
    });
    const pose = {
      at: { x: at.x, y: at.y, z: at.z },
      facing: { x: facing.x, y: facing.y, z: facing.z, w: facing.w },
      resting,
    };
    Object.defineProperty(stack, POSE, { value: pose });
    Object.defineProperty(stack, AUDIENCE, { value: { playerId: audience } });
    Object.defineProperty(stack, 'at', {
      get() {
        return pose.at;
      },
    });
    Object.defineProperty(stack, 'klass', { value: itemClass });
    // The map it lies on (an index of World.maps), and its name.
    Object.defineProperty(stack, 'mapIndex', { value: mapIndex });
    const host = this.host;
    Object.defineProperty(stack, 'map', {
      enumerable: true,
      get() {
        return host.mapName(mapIndex);
      },
    });
    Object.defineProperty(stack, 'facing', {
      get() {
        return pose.facing;
      },
    });
    // A stack a player dropped is the player's, not the running resource's.
    Object.defineProperty(stack, OWNER, {
      value: droppedById === undefined ? owner?.name ?? null : null,
    });
    Object.freeze(stack);
    this.stacks.set(stack.id, stack);
    this.host.bridge.send({
      op: 'groundItemSpawn',
      id: stack.id,
      itemClass: stack.itemClass,
      amount,
      quality,
      health,
      position: stack.at,
      rotation: stack.facing,
      // The native world carries the map, so the server shows it only on its map.
      dimension: onMap(mapIndex, dimension),
      leftBy: stack.droppedById,
      resting,
      // Held where it lies without a body: an arrow stuck in a target.
      ...(pinned ? { pinned: true } : {}),
    });
    if (audience !== 0) {
      this.host.bridge.send({ op: 'entityVisibleTo', id: stack.id, playerId: audience });
    }
    this.host.emit('groundItemSpawn', stack);
    return stack;
  }

  // The network id of the one player a stack is shown to, 0 for everyone (null).
  audienceOf(player) {
    if (player === null || player === undefined) {
      return 0;
    }
    if (!player || this.host.players.get(player.id) !== player) {
      throw new TypeError('A ground item is shown to a connected Player, or to everyone with null');
    }
    return player.id;
  }

  setAudience(stack, playerId) {
    if (this.stacks.get(stack.id) !== stack) {
      throw new Error('Unknown ground item');
    }
    if (stack[AUDIENCE].playerId === playerId) {
      return;
    }
    stack[AUDIENCE].playerId = playerId;
    this.host.bridge.send({ op: 'entityVisibleTo', id: stack.id, playerId });
  }

  // A player's game says where a stack it laid came to rest: the first plausible
  // report is where the stack lies from then on, on every client. Nothing happens for a stack at
  // rest already or of another world, or a place too far from where it was laid or above it.
  rest(player, report) {
    const stack = this.stacks.get(report?.stack);
    if (!stack || stack.resting || !player || this.host.players.get(player.id) !== player) {
      return false;
    }
    if (stack.dimension !== GLOBAL_WORLD && stack.dimension !== player.dimension) {
      return false;
    }
    if (stack.mapIndex !== this.host.playerMapIndex(player)) {
      return false;
    }
    let at;
    try {
      at = vector(report.position ?? {});
    } catch {
      return false;
    }
    const turn = report.rotation ?? {};
    const parts = [turn.x, turn.y, turn.z, turn.w];
    if (!finiteVector(at) || !parts.every(Number.isFinite)) {
      return false;
    }
    const length = Math.hypot(...parts);
    const pose = stack[POSE];
    const moved = Math.hypot(at.x - pose.at.x, at.y - pose.at.y, at.z - pose.at.z);
    if (length < 0.5 || length > 1.5 || moved > REST_REACH || at.z > pose.at.z + REST_RISE) {
      return false;
    }
    pose.at = { x: at.x, y: at.y, z: at.z };
    pose.facing = {
      x: turn.x / length,
      y: turn.y / length,
      z: turn.z / length,
      w: turn.w / length,
    };
    pose.resting = true;
    this.host.bridge.send({
      op: 'groundItemRest',
      id: stack.id,
      position: pose.at,
      rotation: pose.facing,
    });
    this.host.emit('groundItemResting', stack);
    return true;
  }

  // A projectile of a player's shot landed (the server matched it to one of their shots in
  // flight): it lies there as a stack of one, theirs. One stuck in what it hit (a target, a wall)
  // stays there on every client, pinned without a body. The stack, or null for
  // anything but ammunition or a place outside the world.
  landed(player, { item, position, rotation: facing, stuck = false }) {
    if (!player || this.host.players.get(player.id) !== player) {
      return null;
    }
    const itemClass = lookupItemClass(item);
    if (!itemClass || itemClass.category !== 'ammo') {
      return null;
    }
    if (!this.GroundItem) {
      this.host.makeAPI();
    }
    let stack;
    try {
      stack = this.spawn(this.GroundItem, {
        item: itemClass.id,
        position,
        orientation: facing ?? {},
        amount: 1,
        dimension: player.dimension,
        properties: {},
        droppedById: player.id,
        mapIndex: this.host.playerMapIndex(player),
        // The shooter's game saw it stop there.
        resting: true,
        pinned: stuck === true,
      });
    } catch {
      return null;
    }
    this.landedStacks.push(stack.id);
    while (this.landedStacks.length > LANDED_LIMIT) {
      const oldest = this.stacks.get(this.landedStacks.shift());
      if (oldest) {
        this.destroy(oldest);
      }
    }
    return stack;
  }

  // What a drop or a pickup would move, for Inventory.beforeCommit; null when it cannot happen.
  describeDrop(player, row, amount) {
    const line = rowLine(
      this.host.playerInventories.entryOf(player)?.inventory,
      row,
      amount,
      'player',
      'ground',
    );
    return line ? { kind: 'drop', lines: [line] } : null;
  }

  describePickup(player, id) {
    const stack = this.stacks.get(id);
    if (!stack || stack.carryable) {
      return null;
    }
    const metadata = {};
    if (stack.quality > 0) {
      metadata.quality = stack.quality;
    }
    return {
      kind: 'pickup',
      groundItem: stack,
      lines: [
        {
          item: stack.klass.id,
          name: stack.klass.name,
          amount: stack.amount,
          metadata,
          from: 'ground',
          to: 'player',
        },
      ],
    };
  }

  // A player's game dropped units of a row: they leave the inventory and lie at the player's feet.
  drop(player, row, amount) {
    if (!player || this.host.players.get(player.id) !== player || !Number.isInteger(amount)) {
      return [];
    }
    const result = this.host.playerInventories.operate(player, (inventory) => {
      const { result: outcome, change, dropped } = inventory.drop(row, amount);
      return { result: { ...outcome, dropped }, change };
    });
    if (!result.ok) {
      return [];
    }
    const { itemClass, props } = result.dropped;
    const feet = player.position;
    const stacks = itemClass.stackable ? [amount] : Array.from({ length: amount }, () => 1);
    const laid = [];
    stacks.forEach((units, index) => {
      const at = { x: feet.x + 0.15 * index, y: feet.y, z: feet.z + 0.05 };
      laid.push(
        this.lay(
          {
            item: itemClass.id,
            position: at,
            orientation: dropOrientation(itemClass),
            amount: units,
          },
          {
            dimension: player.dimension,
            droppedById: player.id,
            props,
            mapIndex: this.host.playerMapIndex(player),
          },
        ),
      );
    });
    return laid;
  }

  // A player's game took a stack up (by network id): its units go into their inventory and the
  // stack goes. Nothing happens for a stack that is gone, out of reach or another world's, or
  // whose units do not fit.
  pickup(player, id) {
    const stack = this.stacks.get(id);
    if (!stack || !player || this.host.players.get(player.id) !== player) {
      return false;
    }
    // A stack shown to one player is theirs alone to take up.
    const { playerId } = stack[AUDIENCE];
    if (playerId !== 0 && playerId !== player.id) {
      return false;
    }
    if (stack.carryable) {
      return this.host.carries.takeUp(player, stack, { ask: true });
    }
    const where = player.position;
    const reach = Math.hypot(where.x - stack.at.x, where.y - stack.at.y, where.z - stack.at.z);
    const world = player.dimension;
    const otherMap = stack.mapIndex !== this.host.playerMapIndex(player);
    if (
      reach > PICKUP_REACH ||
      otherMap ||
      (stack.dimension !== 0xffffffff && stack.dimension !== world)
    ) {
      return false;
    }
    const metadata = {};
    if (stack.quality > 0) {
      metadata.quality = stack.quality;
    }
    if (stack.health >= 0 && stack.quality > 0) {
      metadata.health = stack.health;
    }
    const result = this.host.playerInventories.operate(
      player,
      (inventory) =>
        inventory.add({ item: stack.klass.id, amount: stack.amount, metadata }, { worn: true }),
      { notify: true },
    );
    if (!result.ok) {
      return false;
    }
    this.host.emit('groundItemPickup', stack, player);
    this.destroy(stack);
    return true;
  }

  // The ground items a stopping resource made go with it.
  resourceStopped(name) {
    removeOwned(this.stacks.values(), name, (stack) => this.destroy(stack));
  }

  destroy(stack) {
    if (this.stacks.get(stack.id) !== stack) {
      return;
    }
    this.host.emit('groundItemDestroy', stack);
    this.stacks.delete(stack.id);
    const landed = this.landedStacks.indexOf(stack.id);
    if (landed >= 0) {
      this.landedStacks.splice(landed, 1);
    }
    this.host.carries.stackGone(stack.id);
    this.host.bridge.send({ op: 'groundItemDestroy', id: stack.id });
  }
}

module.exports = { GroundItems };
