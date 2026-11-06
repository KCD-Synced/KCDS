'use strict';

// Harvesting a dead animal and using an NPC.
//
// Harvest: a player opens a dead animal's carcass to loot it (stash/stashes.cjs, the corpse
// container), or a client reports that its butchering of a carcass finished (npcHarvest). The
// server decides whether it counts, and what it yields comes from the server's own table
// (Npc.setHarvestTable), never from the client: the report names only the carcass and the
// player. The yield is added to the carcass's inventory, where a looter takes it, with the change
// reason `harvest`. A carcass is harvested once per life; revive() starts a new life.
//
// Events: npcHarvest(player, npc) asks first and a handler returning false refuses it (who may
// harvest is the gamemode's); a handler returning { items: [{ item, amount }] } gives that yield
// instead of the table's, for this player's harvest (a skilled hunter gets more).
// npcHarvested(npc, player) follows an accepted one.
//
// Use: a player's client reports a press of the use key on an NPC (NPC_USE: a trace
// from its view camera met the NPC's body). The server confirms the player stands near the NPC and
// then raises npcInteract(npc, player) for NPCs whose `interactable` is on.
//
// Nothing here runs on a timer: a report is handled when it arrives.

const { ANIMAL_CLASSES, USE_RANGE } = require('./npc_values.cjs');
const { lookupItemClass } = require('../inventory/catalog.cjs');
const { propsOf } = require('../inventory/inventories.cjs');

// The most units one table row yields, and the most rows one animal class has.
const YIELD_AMOUNT_MAX = 100;
const YIELD_ROWS_MAX = 16;

// The change reason of the yield's rows.
const HARVEST_REASON = 'harvest';

// One row of a harvest table: { item, amount }, with a class the catalog has and a whole amount.
function yieldRowOf(given, what) {
  if (given === null || typeof given !== 'object' || Array.isArray(given)) {
    throw new TypeError(`${what} must be { item, amount }`);
  }
  const itemClass = lookupItemClass(given.item);
  if (!itemClass) {
    throw new RangeError(`${what} names no item class: ${String(given.item)}`);
  }
  if (itemClass.quest) {
    throw new RangeError(`${what} names a quest item, which cannot be given as a yield`);
  }
  const amount = given.amount;
  if (!Number.isInteger(amount) || amount < 1 || amount > YIELD_AMOUNT_MAX) {
    throw new RangeError(`${what} amount must be a whole number from 1 to ${YIELD_AMOUNT_MAX}`);
  }
  return { item: itemClass.id, amount };
}

// A whole harvest table: { [animal class]: [{ item, amount }] }. Throws for anything else.
function tableOf(given) {
  if (given === null || typeof given !== 'object' || Array.isArray(given)) {
    throw new TypeError('Npc.setHarvestTable takes an object of animal classes');
  }
  const table = new Map();
  for (const [actorClass, rows] of Object.entries(given)) {
    if (!ANIMAL_CLASSES.includes(actorClass)) {
      throw new RangeError(`Npc.setHarvestTable: ${actorClass} is no animal class`);
    }
    if (!Array.isArray(rows) || rows.length > YIELD_ROWS_MAX) {
      throw new RangeError(
        `Npc.setHarvestTable ${actorClass} must be a list of at most ${YIELD_ROWS_MAX} rows`,
      );
    }
    const what = `Npc.setHarvestTable ${actorClass} row`;
    table.set(
      actorClass,
      rows.map((row) => yieldRowOf(row, what)),
    );
  }
  return table;
}

// A handler's own yield: a list of at most YIELD_ROWS_MAX rows, as a table's. Throws for anything
// else.
function yieldOf(items) {
  if (!Array.isArray(items) || items.length > YIELD_ROWS_MAX) {
    throw new RangeError(`items must be a list of at most ${YIELD_ROWS_MAX} rows`);
  }
  return items.map((row, index) => yieldRowOf(row, `npcHarvest yield row ${index}`));
}

class NpcHarvests {
  // `npcs`: the NPC registry; `npcState`: the symbol its NPCs keep their state under.
  constructor(npcs, npcState) {
    this.npcs = npcs;
    this.npcState = npcState;
    // animal class -> [{ item, amount }]
    this.table = new Map();
  }

  // Npc.setHarvestTable: replaces the whole table. A class left out yields nothing, so its
  // carcasses cannot be harvested. Returns the new table as a plain copy.
  configure(given) {
    this.table = tableOf(given);
    for (const npc of this.npcs.npcs.values()) {
      this.npcs.sendCarcassClosed(npc);
    }
    return this.tableCopy();
  }

  // The table as a plain object of copies.
  tableCopy() {
    const copy = {};
    for (const [actorClass, rows] of this.table) {
      copy[actorClass] = rows.map((row) => ({ ...row }));
    }
    return copy;
  }

  // Whether the NPC is a carcass that can still be harvested: dead, of a class in the table, and
  // not harvested in this life.
  harvestable(npc) {
    const state = npc[this.npcState];
    return !state.alive && this.table.has(state.actorClass) && state.inventory.harvested === false;
  }

  // The client's report that its butchering of a carcass is done: { id, playerId }.
  harvest(report) {
    const npc = this.npcs.npcs.get(report.id);
    const player = this.npcs.host.players.get(report.playerId);
    if (!npc || !player || !this.harvestable(npc) || !this.npcs.reaches(npc, player, USE_RANGE)) {
      return false;
    }
    // The last handler's own yield, as the table's rows (a wrong one is logged and the table's
    // given instead).
    let chosen = null;
    const accepted = this.npcs.host.emitCollecting(
      'npcHarvest',
      (result, resource) => {
        if (result === null || typeof result !== 'object' || !('items' in result)) {
          return;
        }
        try {
          chosen = yieldOf(result.items);
        } catch (error) {
          this.npcs.host.bridge.log(resource, `npcHarvest yield refused: ${error.message}`);
        }
      },
      player,
      npc,
    );
    if (!accepted) {
      return false;
    }
    // A handler may have removed, revived or harvested the carcass itself.
    if (!this.npcs.live(npc) || !this.harvestable(npc)) {
      return false;
    }
    this.give(npc, chosen);
    this.npcs.host.emit('npcHarvested', npc, player);
    return true;
  }

  // Marks the carcass harvested and adds the yield to its inventory: a handler's own, else the
  // table's. Rows that do not fit (a full inventory) are left out; the carcass counts as harvested
  // all the same.
  give(npc, chosen = null) {
    const state = npc[this.npcState];
    state.inventory.harvested = true;
    this.npcs.sendCarcassClosed(npc);
    for (const row of chosen ?? this.table.get(state.actorClass)) {
      const itemClass = lookupItemClass(row.item);
      const { props } = propsOf(itemClass, undefined);
      state.inventory.addUnits(itemClass, props, row.amount, 0, HARVEST_REASON);
    }
  }

  // The client's report of a press of the use key on an NPC: { id, playerId }. The player must
  // stand near where the NPC was last reported.
  interact(report) {
    const npc = this.npcs.npcs.get(report.id);
    const player = this.npcs.host.players.get(report.playerId);
    if (!npc || !player || !npc[this.npcState].interactable) {
      return false;
    }
    if (!this.npcs.reaches(npc, player, USE_RANGE)) {
      return false;
    }
    this.npcs.host.emit('npcInteract', npc, player);
    return true;
  }
}

module.exports = { NpcHarvests, HARVEST_REASON };
