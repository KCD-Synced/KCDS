'use strict';

// The game's own items lying in its level: every client loads the same level and has
// its own copy of each, so the server decides who takes one up. A player's game asks before it
// takes one up (by its key: its class as scripts see it and where it lies, in whole centimetres,
// "<class>@<x>,<y>,<z>"); the first player who asks within reach gets it, with the game's own
// pick-up, and every other client takes it out of its world. A player who comes into the world
// later gets the list of items taken. Their game shows its notice of the item received (the game's
// pick-up, run for the server's verdict, shows none).
//
// The list lasts while the server runs: the level's items come back when it starts again.
// worldItemPickup(player, { item, position }) may refuse a pick-up (return false).
//
// A server can turn the level's items off: World.setLevelItemsEnabled(false), or
// { "enabled": false } in its level-items.json. Every client then takes every item of the level
// out of its world, now and as the level streams in, a player who comes later too, and no pick-up
// is granted. Turned on again, they come back.

const fs = require('node:fs');
const path = require('node:path');

const { lookupClassHex, lookupItemClass, classBytesHex } = require('../inventory/catalog.cjs');

// How far from an item a player's body may take it up, in metres.
const PICKUP_REACH = 6;
const KEY = /^([0-9a-f]{32})@(-?\d+),(-?\d+),(-?\d+)$/;
// Keys sent in one message (kMaxWorldTakenKeys).
const KEYS_PER_MESSAGE = 256;
const MAX_TAKEN = 100000;
const CONFIG_FILE = 'level-items.json';
// World.removeLevelItems: the default and largest radius (metres) and the most places
// (world_items.hpp kMaxRemovedItemRadius, kMaxRemovedItemAreas).
const REMOVE_RADIUS = 0.5;
const REMOVE_RADIUS_MAX = 100;
const REMOVE_AREAS_MAX = 1024;

// The class of a key's 32 hex digits, or undefined.
function classOfHex(hex) {
  return lookupClassHex(hex);
}

// { hex, position } of a well-formed key, else null.
function parseKey(key) {
  const match = typeof key === 'string' ? KEY.exec(key) : null;
  if (!match) {
    return null;
  }
  return {
    hex: match[1],
    position: { x: Number(match[2]) / 100, y: Number(match[3]) / 100, z: Number(match[4]) / 100 },
  };
}

class WorldPickables {
  constructor(host) {
    this.host = host;
    // key -> { by (player id), at (ms) }
    this.taken = new Map();
    // Whether the level's own items are on.
    this.enabled = true;
    // World.removeLevelItems: handle -> { center, radius, item hex or '' }.
    this.removed = new Map();
  }

  // World.removeLevelItems({ position, radius, item }): every game hides the level's own items
  // lying within `radius` metres (default 0.5, up to 100) of `position`, of one class if `item`
  // names one, now and for players who come later, until the handle's restore().
  removeArea(options) {
    if (options === null || typeof options !== 'object') {
      throw new TypeError('World.removeLevelItems takes { position, radius, item }');
    }
    const at = options.position;
    if (!at || ![at.x, at.y, at.z].every(Number.isFinite)) {
      throw new TypeError('World.removeLevelItems position must be { x, y, z }');
    }
    const radius = options.radius ?? REMOVE_RADIUS;
    if (typeof radius !== 'number' || !(radius > 0 && radius <= REMOVE_RADIUS_MAX)) {
      throw new RangeError(
        `World.removeLevelItems radius is above 0 and up to ${REMOVE_RADIUS_MAX}`,
      );
    }
    let item = '';
    if (options.item !== undefined) {
      const itemClass = lookupItemClass(options.item);
      if (!itemClass) {
        throw new TypeError('World.removeLevelItems item must be an item class name or GUID');
      }
      item = classBytesHex(itemClass);
    }
    if (this.removed.size >= REMOVE_AREAS_MAX) {
      throw new RangeError(`World.removeLevelItems keeps at most ${REMOVE_AREAS_MAX} places`);
    }
    const area = { center: { x: at.x, y: at.y, z: at.z }, radius, item };
    const handle = Object.freeze({
      position: area.center,
      radius,
      item: options.item ?? null,
      restore: () => {
        if (this.removed.delete(handle)) {
          this.sendRemoved(0);
        }
      },
    });
    this.removed.set(handle, area);
    this.sendRemoved(0);
    return handle;
  }

  // The whole list of removed places, to one player or everyone (0).
  sendRemoved(id) {
    this.host.bridge.send({ op: 'worldItemsRemoved', id, areas: [...this.removed.values()] });
  }

  // level-items.json in the server's folder: { "enabled": false } turns the level's items off.
  load(root) {
    const file = path.join(root, CONFIG_FILE);
    if (!fs.existsSync(file)) {
      return;
    }
    try {
      const raw = JSON.parse(fs.readFileSync(file, 'utf8'));
      if (raw === null || typeof raw !== 'object' || typeof (raw.enabled ?? true) !== 'boolean') {
        throw new Error('enabled must be true or false');
      }
      this.enabled = raw.enabled ?? true;
    } catch (error) {
      this.host.bridge.log('host', `${CONFIG_FILE}: ${error.message}; the level's items stay on`);
    }
  }

  // World.setLevelItemsEnabled: every player's game takes the level's items out of its world, or
  // brings them back with its next level load.
  setEnabled(enabled) {
    if (typeof enabled !== 'boolean') {
      throw new TypeError('World.setLevelItemsEnabled takes true or false');
    }
    if (enabled !== this.enabled) {
      this.enabled = enabled;
      this.host.bridge.send({ op: 'worldItemsEnabled', id: 0, enabled });
    }
    return true;
  }

  // A player's game would take up the level's item named by `key`: the verdict to their client,
  // and the item out of every other world when they get it.
  pickup(player, key) {
    if (!player || this.host.players.get(player.id) !== player) {
      return false;
    }
    const verdict = (granted) => {
      this.host.bridge.send({ op: 'worldPickupVerdict', id: player.id, key, granted });
      return granted;
    };
    const parsed = parseKey(key);
    if (parsed && !this.enabled) {
      return verdict(false);
    }
    if (!parsed || this.taken.has(key) || this.taken.size >= MAX_TAKEN) {
      return parsed ? verdict(false) : false;
    }
    const where = player.position;
    const reach = Math.hypot(
      where.x - parsed.position.x,
      where.y - parsed.position.y,
      where.z - parsed.position.z,
    );
    if (reach > PICKUP_REACH) {
      return verdict(false);
    }
    const itemClass = classOfHex(parsed.hex);
    const item = Object.freeze({
      item: itemClass ? itemClass.id : null,
      name: itemClass ? itemClass.name : null,
      position: Object.freeze(parsed.position),
    });
    if (!this.host.emit('worldItemPickup', player, item)) {
      return verdict(false);
    }
    // A class the player holds the server's most of stays where it lies.
    const entry = itemClass ? this.host.playerInventories.entryOf(player) : null;
    if (entry && entry.inventory.roomFor(itemClass, 1) < 1) {
      return verdict(false);
    }
    this.taken.set(key, { by: player.id, at: Date.now() });
    verdict(true);
    if (itemClass) {
      this.host.bridge.send({
        op: 'itemNotice',
        id: player.id,
        item: itemClass.id,
        amount: 1,
        gained: true,
      });
    }
    this.host.bridge.send({ op: 'worldTaken', id: 0, full: false, keys: [key] });
    return true;
  }

  // The whole list to a player whose world is ready (their client starts each world with none).
  arrived(player) {
    if (this.removed.size > 0) {
      this.sendRemoved(player.id);
    }
    if (!this.enabled) {
      this.host.bridge.send({ op: 'worldItemsEnabled', id: player.id, enabled: false });
    }
    const keys = [...this.taken.keys()];
    for (let start = 0; start < keys.length; start += KEYS_PER_MESSAGE) {
      this.host.bridge.send({
        op: 'worldTaken',
        id: player.id,
        full: start === 0,
        keys: keys.slice(start, start + KEYS_PER_MESSAGE),
      });
    }
  }
}

module.exports = { WorldPickables, parseKey };
