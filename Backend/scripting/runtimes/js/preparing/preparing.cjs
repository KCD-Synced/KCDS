'use strict';

// Food preparing at the game's own fires, smokehouses and drying racks: before the
// player's game cooks, smokes or dries what the player chose, it asks the server, and
// playerPreparing lets a game mode allow or refuse it.
//
//   Events.on('playerPreparing', (player, preparing) => preparing.kind !== 'smoking');
//
// preparing: { kind: 'cooking' | 'smoking' | 'drying', position: Vector3 (the fire, smokehouse
// or rack), dimension, station (the level's drying rack or smokehouse there, its
// SmartObject.stations id; null at a fire) }. A handler returning false refuses: the player's game
// does not start the preparing and nothing is taken. Without a refusal the game prepares as it
// does alone: the items convert in the player's inventory (the server's inventory follows), and
// the game's survival XP comes through the server's XP rules.
//
// The game picks the items after the ask (the player's tree asks the station for them, then
// converts one row at a time), so the rows come with playerPrepared(player, preparation): the same
// preparation with `items`, [{ id, item, amount, product }], what the player's inventory lost
// (`drop` or `use`) and gained (`loot`) once the preparing was allowed, each lost stack paired with the
// product gained in the same amount. It is raised when the inventory has been still for a few
// seconds, and not at all when nothing converted.
//
// The player's game asks with CRAFT_REPORT `Prepare` (request numbers of their own) and waits
// for the CRAFT_VERDICT of that request; a game that hears nothing in a few seconds prepares.

const KINDS = Object.freeze(['cooking', 'smoking', 'drying']);
// How long after the last conversion a preparing is over, and how long one may take to start.
const QUIET_MS = 3000;
const START_MS = 30000;

class FoodPreparing {
  // host: emit(name, player, payload) -> bool, send(event), vector(xyz), and
  // stationRemoved(kind, position, dimension) -> bool: a drying rack or smokehouse a level edit
  // took out of the player's world (SmartObject.remove).
  // stationAt(kind, position) -> the station's id or null.
  constructor({ emit, send, vector, stationRemoved = () => false, stationAt = () => null }) {
    this.emit = emit;
    this.send = send;
    this.vector = vector;
    this.stationRemoved = stationRemoved;
    this.stationAt = stationAt;
    // player id -> { player, preparation, lost: [], gained: [], timer }
    this.open = new Map();
  }

  // A player's game asks to prepare: { request, reason (the kind), station (where) }.
  ask(player, { request, reason, station }) {
    if (!player || !Number.isInteger(request) || request <= 0) {
      return;
    }
    let allowed = false;
    const food = { drying: 'drying', smoking: 'smoking' }[reason];
    const position = this.vector(station);
    if (food && this.stationRemoved(food, position, player.dimension)) {
      this.send({ op: 'craftVerdict', id: player.id, request, allowed: false });
      return;
    }
    if (KINDS.includes(reason)) {
      const preparing = Object.freeze({
        kind: reason,
        position: this.vector(station),
        dimension: player.dimension,
        station: food ? this.stationAt(food, position) : null,
      });
      allowed = this.emit('playerPreparing', player, preparing) !== false;
      if (allowed) {
        this.watch(player, preparing);
      }
    }
    this.send({ op: 'craftVerdict', id: player.id, request, allowed });
  }

  // The allowed preparing's conversions are watched in the player's inventory.
  watch(player, preparation) {
    this.finish(player.id);
    const entry = { player, preparation, lost: [], gained: [], timer: null };
    this.open.set(player.id, entry);
    this.arm(entry, START_MS);
  }

  arm(entry, ms) {
    clearTimeout(entry.timer);
    entry.timer = setTimeout(() => this.finish(entry.player.id), ms);
    entry.timer.unref?.();
  }

  // What the player's game changed on its own while a preparing is watched (after the
  // playerInventoryChanged that carried it).
  inventoryChanged(player, change) {
    const entry = this.open.get(player?.id);
    // The game reports food it takes as 'use' and other things it takes as 'drop'.
    const native = change && ['drop', 'use', 'loot'].includes(change.reason);
    if (!entry || !native || !Array.isArray(change.items)) {
      return;
    }
    for (const { id, before, after } of change.items) {
      const row = after ?? before;
      const delta = (after?.amount ?? 0) - (before?.amount ?? 0);
      if (!row || delta === 0) {
        continue;
      }
      if (delta < 0) {
        entry.lost.push({ id, item: row.item, amount: -delta });
      } else {
        entry.gained.push({ item: row.item, amount: delta });
      }
    }
    this.arm(entry, QUIET_MS);
  }

  // The preparing is over: playerPrepared with the rows, when anything converted.
  finish(playerId) {
    const entry = this.open.get(playerId);
    if (!entry) {
      return;
    }
    clearTimeout(entry.timer);
    this.open.delete(playerId);
    const gained = [...entry.gained];
    const items = [];
    for (const lost of entry.lost) {
      const at = gained.findIndex((row) => row.amount === lost.amount);
      const product = at === -1 ? null : gained.splice(at, 1)[0].item;
      items.push(Object.freeze({ id: lost.id, item: lost.item, amount: lost.amount, product }));
    }
    if (items.length === 0) {
      return;
    }
    const prepared = Object.freeze({ ...entry.preparation, items: Object.freeze(items) });
    this.emit('playerPrepared', entry.player, prepared);
  }

  // A player who leaves ends what was watched for them.
  left(playerId) {
    const entry = this.open.get(playerId);
    if (entry) {
      clearTimeout(entry.timer);
      this.open.delete(playerId);
    }
  }
}

module.exports = { FoodPreparing, KINDS };
