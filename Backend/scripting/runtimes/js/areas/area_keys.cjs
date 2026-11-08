'use strict';

// An area's key: area.lockKey names an item class that works every door and chest of
// the area. A door's or chest's own key (door.setKey, stash.keyClass) takes precedence, so a supervisor's
// key can open what the others' cannot. player.keyBypass opens everything without one.
//
// The key works through the game's own lock: each client writes it into the lock's guidItemClassId,
// so the game itself offers Unlock to whoever carries it, and the server checks the key on every use.
// The game has no Lock action of its own for a player, so a holder gets a hint on an unlocked door or
// chest, through World.addInteraction.

const { lookupItemClass } = require('../inventory/catalog.cjs');
const { doorsOfArea } = require('./area_doors.cjs');

const LOCK_TEXT = 'Lock';
const LOCK_KEY = 'script_action';
const LOCK_RADIUS = 2.5;
const REFRESH_MS = 1000;
// How far from an area's middle its chests are looked for, past the area's own size (metres).
const CHEST_MARGIN = 5;

class AreaKeys {
  constructor(host) {
    this.host = host;
    // Area id -> { area, item (class id), hints: [{ interaction, target }] }
    this.keyed = new Map();
    this.timer = null;
  }

  api() {
    return this.host.api ?? this.host.makeAPI();
  }

  // The area's key class id, '' for none.
  keyOf(area) {
    return this.keyed.get(area.id)?.item ?? '';
  }

  // Sets the area's key, an item class by name or GUID; null takes it away.
  set(area, query) {
    const entry = this.keyed.get(area.id);
    if (entry) {
      this.removeHints(entry);
      this.keyed.delete(area.id);
    }
    let item = '';
    if (query !== null) {
      const itemClass = lookupItemClass(query);
      if (!itemClass) {
        throw new TypeError(`area.lockKey: '${String(query)}' is not an item class`);
      }
      item = itemClass.id;
      this.keyed.set(area.id, { area, item, hints: [] });
    }
    this.applyToLocks(area, item);
    if (item) {
      this.addHints(this.keyed.get(area.id));
      this.startTimer();
    }
  }

  // The doors of an area: those the level links to it, and those a player's game showed inside it.
  doorRows(area) {
    const rows = new Map();
    const level = this.host.areas.levelOfAreaId(area.id);
    for (const row of doorsOfArea(level, area.id)) {
      rows.set(row.guid, { guid: row.guid, name: row.name, position: row.position });
    }
    for (const door of this.api().Door.all()) {
      if (!rows.has(door.guid) && area.contains(door.position)) {
        rows.set(door.guid, { guid: door.guid, name: door.name, position: door.position });
      }
    }
    return [...rows.values()];
  }

  // The chests of an area, level chests made the server's so their key is the server's to set.
  chests(area) {
    const { Stash } = this.api();
    const found = new Map();
    for (const stash of Stash.all()) {
      if (!stash.isStorage && stash.position && area.contains(stash.position)) {
        found.set(stash.id, stash);
      }
    }
    const bounds = area.bounds;
    if (!bounds) {
      return [...found.values()];
    }
    const middle = {
      x: (bounds.min.x + bounds.max.x) / 2,
      y: (bounds.min.y + bounds.max.y) / 2,
      z: (bounds.min.z + bounds.max.z) / 2,
    };
    const across = Math.hypot(bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y);
    for (const row of Stash.levelChests(middle, across / 2 + CHEST_MARGIN)) {
      if (area.contains(row.position)) {
        const stash = Stash.adopt(row.placedGuid);
        if (stash) {
          found.set(stash.id, stash);
        }
      }
    }
    return [...found.values()];
  }

  // The key an area gives a door by its GUID and place, '' for none.
  keyOfDoor(guid, position) {
    for (const { area, item } of this.keyed.values()) {
      const level = this.host.areas.levelOfAreaId(area.id);
      const linked = doorsOfArea(level, area.id).some((row) => row.guid === guid);
      if (linked || (position && area.contains(position))) {
        return item;
      }
    }
    return '';
  }

  applyToLocks(area, item) {
    for (const stash of this.chests(area)) {
      this.host.stashes.setAreaKey(stash, item);
    }
    for (const door of this.api().Door.all()) {
      if (this.keyOfDoor(door.guid, door.position) === item) {
        this.host.doors.resend(door);
      }
    }
  }

  // Locks or unlocks every door and chest of the area, each for whoever carries its key.
  setLocked(area, locked) {
    if (typeof locked !== 'boolean') {
      throw new TypeError('area.setLocked takes true or false');
    }
    let count = 0;
    for (const row of this.doorRows(area)) {
      const door = this.api().Door.find(row.guid);
      if (door) {
        this.host.doors.change(door, { open: locked ? false : undefined, locked });
        count += 1;
      }
    }
    for (const stash of this.chests(area)) {
      this.host.stashes.setLock(stash, { locked, byServer: locked });
      count += 1;
    }
    return count;
  }

  addHints(entry) {
    const { World } = this.api();
    for (const row of this.doorRows(entry.area)) {
      const target = { guid: row.guid, position: row.position };
      const options = {
        entity: row.name,
        text: LOCK_TEXT,
        key: LOCK_KEY,
        radius: LOCK_RADIUS,
      };
      const interaction = World.addInteraction(options, (player) => {
        this.lockDoor(player, target);
      });
      interaction.setPlayers([]);
      entry.hints.push({ interaction, target });
    }
    for (const stash of this.chests(entry.area)) {
      const target = { stash };
      const options = {
        position: stash.position,
        text: LOCK_TEXT,
        key: LOCK_KEY,
        radius: LOCK_RADIUS,
      };
      const interaction = World.addInteraction(options, (player) => {
        this.lockStash(player, stash);
      });
      interaction.setPlayers([]);
      entry.hints.push({ interaction, target });
    }
  }

  removeHints(entry) {
    for (const { interaction } of entry.hints) {
      interaction.remove();
    }
    entry.hints = [];
  }

  lockDoor(player, target) {
    const door = this.api().Door.find(target.guid, player.dimension);
    if (door && !door.isLocked && this.host.doors.holdsKey(player, door)) {
      this.host.doors.change(door, { open: false, locked: true });
    }
  }

  lockStash(player, stash) {
    const { lock } = stash;
    if (!lock.locked && (player.keyBypass || this.host.stashes.carriesKey(player, lock.keyClass))) {
      this.host.stashes.setLock(stash, { locked: true, byServer: true });
    }
  }

  // The hint on an unlocked door or chest, for the players who may lock it.
  holdersOf(target) {
    const players = [...this.host.players.values()].filter((player) => player.ready);
    if (target.stash) {
      const { lock } = target.stash;
      if (lock.locked) {
        return [];
      }
      return players.filter(
        (player) => player.keyBypass || this.host.stashes.carriesKey(player, lock.keyClass),
      );
    }
    const door = this.api().Door.find(target.guid);
    if (!door || door.isLocked) {
      return [];
    }
    return players.filter((player) => this.host.doors.holdsKey(player, door));
  }

  refresh() {
    for (const entry of this.keyed.values()) {
      for (const hint of entry.hints) {
        hint.interaction.setPlayers(this.holdersOf(hint.target));
      }
    }
  }

  startTimer() {
    if (this.timer) {
      return;
    }
    this.timer = setInterval(() => this.refresh(), REFRESH_MS);
    this.timer.unref?.();
  }

  stop() {
    if (this.timer) {
      clearInterval(this.timer);
      this.timer = null;
    }
    this.keyed.clear();
  }
}

module.exports = { AreaKeys };
