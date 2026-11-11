'use strict';

// The players' inventories on the server: one Inventory per connected player, from
// just before playerConnect until just after playerDisconnect, the Inventory API over them, and
// playerInventoryChanged, which fires on the following tick, oldest change first.
//
// Players only for now; NPC stock comes with the NPC milestone.

const { Inventory, itemPrice, metadataOf, transfer } = require('./inventories.cjs');
const { WearRates } = require('./wear_rates.cjs');
const { equipRequestOf } = require('./equip_request.cjs');
const { rowLine } = require('./commit_guards.cjs');
const {
  classesNamed,
  isGear,
  itemClasses,
  lookupItemClass,
  lookupOutfit,
  outfitNames,
} = require('./catalog.cjs');

const ITEM_NAME_BYTES = 128;
const POT_GUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
// The most units one notice names (kMaxItemNoticeAmount).
const MAX_NOTICE_AMOUNT = 1000000;
// A use waits this long for its player's receipt; its unit is reserved meanwhile.
const CONSUMPTION_RECEIPT_MS = 10000;
// Uses one player may have waiting for receipts.
const MAX_CONSUMPTIONS = 32;
// The equipment slots of horse tack (horse_saddle, horse_head ...).
const HORSE_SLOT = /^horse_/;

// Whether an API request asks for the game's notice: `notify` true or left out. Undefined for a
// `notify` that is not a boolean.
function notifies(request) {
  const notify = request !== null && typeof request === 'object' ? request.notify : undefined;
  if (notify === undefined) {
    return true;
  }
  return typeof notify === 'boolean' ? notify : undefined;
}

function refusal(code) {
  return { ok: false, code, revision: 0, items: [] };
}

class PlayerInventories {
  // `host`: the resource host (players, emit); `project(player, inventory)`: shows a player's
  // inventory in their game after a change.
  constructor(host, project = () => {}) {
    this.host = host;
    this.project = project;
    this.inventories = new Map();
    this.pending = [];
    // How fast items wear (Inventory.setWearRates).
    this.wear = new WearRates();
    // What every player's game calls items (Inventory.renameItem): localization key -> name.
    this.itemNames = new Map();
    this.sendNames = () => {};
    // The poison added to a cooking pot: fireplace GUID (lower case) -> buff GUID. Nothing sets it
    // until the pots of 0.2.3 (CookPot.poison).
    this.potPoisons = new Map();
  }

  // A connecting player's empty inventory; a playerConnect handler restores a saved one.
  open(player) {
    if (!this.inventories.has(player.id)) {
      const inventory = new Inventory();
      // The server's limits per class, the player's own over them; the player
      // is told of each refusal.
      inventory.limitOf = (itemClass) => this.host.itemOverrides.maxOf(itemClass, player);
      inventory.onLimit = (itemClass, max) => {
        this.host.itemOverrides.limitReached(player, itemClass, max);
      };
      this.inventories.set(player.id, {
        inventory,
        shown: -1,
        readyRaised: false,
        adopted: false,
        // Uses waiting for the player's receipt: request -> { consumption, row, at }.
        consumptions: new Map(),
        // The names of the player's rows their game heard last: key -> text.
        rowNames: new Map(),
      });
    }
  }

  // A disconnecting player's queued changes are raised before playerDisconnect; the inventory
  // goes after it.
  beforeDisconnect(player) {
    this.flush((entry) => entry.player === player);
  }

  close(player) {
    this.inventories.delete(player.id);
  }

  // The player is leaving: the rows that are not kept leave first, before
  // playerDisconnect, so a script saving the inventory there does not save them. The player's game
  // is not told; it is going.
  dropTransient(player) {
    const entry = this.entryOf(player);
    return entry ? entry.inventory.dropTransient() : null;
  }

  entryOf(owner) {
    if (!owner || this.host.players.get(owner.id) !== owner) {
      return undefined;
    }
    return this.inventories.get(owner.id);
  }

  // The client's report of the revision its game shows: `ready` and, the first time,
  // playerInventoryReady.
  // The census of what the player's game held as they came into the world (part 2): their stock
  // unless a resource set one already, which their game then shows instead.
  census(player, rows) {
    const entry = this.entryOf(player);
    if (!entry || entry.inventory.revision !== 0) {
      return;
    }
    entry.adopted = true;
    const { change } = entry.inventory.adopt(rows);
    this.record(player, change);
  }

  // What the player's game changed on its own: the changes, then playerItemUsed for what it spent
  // (raised after the playerInventoryChanged that removed it). While the player robs another
  // what their game gained is a claim on the victim's pockets instead.
  // `batch` ({ id, epoch, sequence, revision }) is the client's batch: it always gets its answer,
  // applied, or refused when the player has no stock, names a revision the server never sent, or
  // applying failed (the player's game is then shown the stock). Its changes carry `operation`.
  native(player, reports, batch) {
    let verdict = 'refused';
    try {
      verdict = this.applyBatch(player, reports, batch);
    } finally {
      const entry = this.entryOf(player);
      if (verdict !== 'applied' && entry) {
        this.project(player, entry.inventory);
      }
      this.answerBatch(batch, verdict, entry ? entry.inventory.revision : 0);
    }
  }

  applyBatch(player, reports, batch) {
    const entry = this.entryOf(player);
    if (!entry || (batch && !(batch.revision <= entry.inventory.revision))) {
      return 'refused';
    }
    const victim = this.host.pickpocketing?.victimOf(player);
    if (victim && Array.isArray(reports)) {
      const claims = reports.filter((report) => report?.kind === 'gained');
      if (claims.length > 0) {
        const lines = this.pickpocket(player, victim, claims);
        this.host.pickpocketing.settled(player, lines);
        reports = reports.filter((report) => report?.kind !== 'gained');
      }
    }
    const { changes, used, corrected } = entry.inventory.applyNative(reports, (itemClass) =>
      this.wear.of(itemClass),
    );
    for (const change of changes) {
      if (batch) {
        change.operation = `${batch.epoch}:${batch.sequence}`;
      }
      this.pending.push({ player, change });
    }
    for (const use of used) {
      this.pending.push({ player, used: use });
    }
    if (changes.length > 0 || corrected) {
      this.project(player, entry.inventory);
    }
    return 'applied';
  }

  // What a use would spend, for Inventory.beforeCommit: a unit of the player's row, or a serving of
  // class `item` from a cooking pot or barrel (`pot` the fireplace, '' when unknown).
  describeUse(player, { row, item, pot }, fromPot) {
    if (fromPot) {
      const itemClass = lookupItemClass(item);
      if (!itemClass) {
        return null;
      }
      return {
        kind: 'use',
        pot: typeof pot === 'string' ? pot : '',
        lines: [
          {
            item: itemClass.id,
            name: itemClass.name,
            amount: 1,
            metadata: {},
            from: 'pot',
            to: null,
          },
        ],
      };
    }
    const line = rowLine(this.entryOf(player)?.inventory, row, 1, 'player', null);
    return line ? { kind: 'use', lines: [line] } : null;
  }

  // The answer to a client's batch of changes, which its game waits for before it sends the next.
  answerBatch(batch, verdict, revision) {
    if (!batch) {
      return;
    }
    this.host.bridge?.send({
      op: 'inventoryChangesAck',
      id: batch.id,
      epoch: batch.epoch,
      sequence: batch.sequence,
      verdict,
      revision,
    });
  }

  // A thief's game took items from a victim's pockets: each class it gained comes out
  // of the victim's own rows, at most what they carry and do not wear (reason `pickpocket`); what
  // the victim lacks is not given, and the thief's game is shown what the server holds. Returns
  // the lines that moved: [{ item, name, amount }].
  pickpocket(thief, victim, claims) {
    const from = this.entryOf(victim);
    const to = this.entryOf(thief);
    const lines = [];
    if (!from || !to) {
      return lines;
    }
    for (const claim of claims) {
      const itemClass = lookupItemClass(claim.item);
      if (!itemClass || !Number.isInteger(claim.amount) || claim.amount < 1) {
        continue;
      }
      let wanted = claim.amount;
      const units = [];
      for (const row of from.inventory.rows.values()) {
        if (wanted <= 0) {
          break;
        }
        const free = row.amount - (row.equipped ?? 0);
        if (row.itemClass.id !== itemClass.id || free < 1) {
          continue;
        }
        const take = Math.min(free, wanted);
        units.push({ id: row.id, amount: take });
        wanted -= take;
      }
      if (units.length === 0) {
        continue;
      }
      const moved = transfer(
        from.inventory,
        to.inventory,
        { units },
        {
          source: 'pickpocket',
          target: 'pickpocket',
        },
      );
      if (!moved.result.ok) {
        continue;
      }
      this.record(victim, moved.change);
      this.record(thief, moved.targetChange);
      lines.push({ item: itemClass.id, name: itemClass.name, amount: claim.amount - wanted });
    }
    this.project(thief, to.inventory);
    return lines;
  }

  // The player ate or drank an item of a row in their inventory screen, and their game
  // waits for the verdict: 'refused', 'native' (as the game does it) or 'plain' (used up without
  // the game's own effects). A food or potion raises playerConsuming, which may refuse it, then
  // playerConsumptionEffects, which may take the game's own effects away; its unit is reserved
  // until the receipt. Anything else the screen uses (an ointment) goes as the game does it.
  consume(player, { request, row }) {
    const entry = this.entryOf(player);
    if (!entry || !Number.isInteger(request) || request < 0) {
      return;
    }
    const reply = (verdict) => this.host.sendConsumeVerdict(player, request, verdict);
    const now = Date.now();
    for (const [id, pending] of entry.consumptions) {
      if (now - pending.at > CONSUMPTION_RECEIPT_MS) {
        entry.consumptions.delete(id);
        // A pot meal the game never confirmed goes back into the pot.
        this.host.cookPots?.settle(player, pending.reservation, false);
      }
    }
    const stock = entry.inventory.rows.get(row);
    let reserved = 0;
    for (const pending of entry.consumptions.values()) {
      if (pending.row === row) {
        reserved += 1;
      }
    }
    if (
      !stock ||
      stock.amount - reserved < 1 ||
      entry.consumptions.has(request) ||
      entry.consumptions.size >= MAX_CONSUMPTIONS
    ) {
      reply('refused');
      return;
    }
    const food = this.host.itemOverrides.foodOf(stock.itemClass);
    if (!food) {
      reply('native');
      return;
    }
    const snapshot = (nativeEffects) =>
      Object.freeze({
        source: 'inventory',
        item: stock.itemClass.id,
        kind: food.kind,
        baseEffects: food.baseEffects,
        rowId: row,
        potGuid: null,
        metadata: metadataOf(stock.itemClass, stock.props),
        poison: null,
        nativeEffects,
      });
    if (!this.host.emit('playerConsuming', player, snapshot(true))) {
      reply('refused');
      return;
    }
    const nativeEffects = this.host.emit('playerConsumptionEffects', player, snapshot(true));
    entry.consumptions.set(request, { consumption: snapshot(nativeEffects), row, at: now });
    reply(nativeEffects ? 'native' : 'plain');
  }

  // The player ate or drank a serving of a class from a cooking pot or a barrel: no row
  // is spent. playerConsuming may refuse it and playerConsumptionEffects take the game's effects
  // away, as for the inventory. `pot`: the GUID of the fireplace the player's game ate from (null
  // when it found none). A catalogued pot's stock is the server's (CookPot): a disabled
  // or empty pot refuses, a meal is reserved until the game says it ate, the pot's poison goes
  // with it, and a pot serving another meal than the game's gives that meal's nutrition instead
  // of the game's effects. A pot the catalog does not know takes its poison from potPoisons.
  consumePot(player, { request, item, pot }) {
    const entry = this.entryOf(player);
    if (!entry || !Number.isInteger(request) || request < 0) {
      return;
    }
    const potGuid = typeof pot === 'string' && POT_GUID.test(pot) ? pot.toLowerCase() : null;
    const reply = (verdict) => this.host.sendConsumeVerdict(player, request, verdict);
    const itemClass = lookupItemClass(item);
    const food = itemClass ? this.host.itemOverrides.foodOf(itemClass) : undefined;
    if (!food || entry.consumptions.has(request) || entry.consumptions.size >= MAX_CONSUMPTIONS) {
      reply(food ? 'refused' : 'native');
      return;
    }
    const world = Number.isInteger(player.dimension) ? player.dimension : 0xffffffff;
    const reservation =
      potGuid && this.host.cookPots
        ? this.host.cookPots.reserve(potGuid, world, itemClass.id)
        : null;
    if (reservation && !reservation.ok) {
      reply('refused');
      return;
    }
    const poison = reservation
      ? reservation.poison
      : potGuid
      ? this.potPoisons.get(potGuid) ?? null
      : null;
    const snapshot = (nativeEffects) =>
      Object.freeze({
        source: 'cookPot',
        item: itemClass.id,
        kind: food.kind,
        baseEffects: food.baseEffects,
        rowId: null,
        potGuid,
        metadata: Object.freeze({}),
        poison,
        nativeEffects,
      });
    if (!this.host.emit('playerConsuming', player, snapshot(true))) {
      this.host.cookPots?.settle(player, reservation, false);
      reply('refused');
      return;
    }
    // The game's effects stay unless a handler takes them away or the pot serves another meal than
    // the game's; the pot's poison stays unless a handler takes the effects away.
    const gameMeal = !reservation || reservation.native;
    const allowed = this.host.emit('playerConsumptionEffects', player, snapshot(gameMeal));
    const nativeEffects = allowed && gameMeal;
    entry.consumptions.set(request, {
      consumption: snapshot(nativeEffects),
      row: null,
      at: Date.now(),
      reservation,
      poisonAllowed: allowed,
    });
    reply(nativeEffects ? 'native' : 'plain');
  }

  // The player's game says whether it used what the server let it: playerConsumed
  // follows on the next tick, after the change that spent the unit when it came first. A pot meal
  // with added poison and the game's own effects raises playerPoisonAbsorbing first; unless a
  // handler refuses it, the poison's buff goes on the player.
  consumed(player, { request, used }) {
    const entry = this.entryOf(player);
    const pending = entry?.consumptions.get(request);
    if (!pending) {
      return;
    }
    entry.consumptions.delete(request);
    this.host.cookPots?.settle(player, pending.reservation, used === true);
    if (used !== true) {
      return;
    }
    const { consumption } = pending;
    if (
      consumption.poison &&
      (pending.poisonAllowed ?? consumption.nativeEffects) &&
      this.host.emit('playerPoisonAbsorbing', player, consumption, consumption.poison)
    ) {
      player.addBuff(consumption.poison);
    }
    this.pending.push({ player, consumed: consumption });
  }

  // What the player's game says the current outfit's quickslots hold (the wire's list).
  // It changes no revision and raises no event: `quickslots` of their state reads it.
  quickslots(player, list) {
    const entry = this.entryOf(player);
    if (entry) {
      entry.inventory.reportQuickslots(list);
    }
  }

  // Whether the player's stock replaces what their game holds: a resource set it rather than it
  // growing from their census.
  replaces(player) {
    const entry = this.entryOf(player);
    return !!entry && !entry.adopted;
  }

  // A player whose game is ready shows the stock a resource set; an untouched one waits for its
  // census.
  arrived(player) {
    const entry = this.entryOf(player);
    if (entry && entry.inventory.revision > 0) {
      this.project(player, entry.inventory);
    }
  }

  shown(player, revision) {
    const entry = this.entryOf(player);
    if (!entry || !Number.isInteger(revision) || revision > entry.inventory.revision) {
      return;
    }
    entry.shown = Math.max(entry.shown, revision);
    if (!entry.readyRaised && entry.shown === entry.inventory.revision) {
      entry.readyRaised = true;
      this.host.emit('playerInventoryReady', player);
    }
  }

  record(player, change) {
    if (!change) {
      return;
    }
    // A change a guarded action made (Inventory.beforeCommit) names its transaction.
    const transaction = this.host.commitGuards?.runningTransaction();
    if (transaction) {
      change = { ...change, transaction: transaction.id };
    }
    this.pending.push({ player, change });
    this.project(player, this.inventories.get(player.id).inventory);
    // A trade showing this player's inventory shows it anew.
    this.host.vendors?.trades.inventoryChanged(player);
  }

  // A guarded transaction settled (commit_guards.cjs): playerInventoryTransaction is raised after
  // the changes it made.
  settled(player, outcome) {
    this.pending.push({ player, settled: Object.freeze(outcome) });
  }

  // Raises the queued playerInventoryChanged events (all, or those `which` picks), in order.
  flush(which = () => true) {
    const due = this.pending.filter(which);
    this.pending = this.pending.filter((entry) => !which(entry));
    for (const { player, change, used, consumed, settled } of due) {
      if (settled) {
        this.host.emit('playerInventoryTransaction', player, settled);
      } else if (consumed) {
        this.host.emit('playerConsumed', player, consumed);
      } else if (used) {
        this.host.emit('playerItemUsed', player, used.item, used.kind, used.amount);
      } else {
        this.host.emit('playerInventoryChanged', player, change);
        // What a crafting station took or gave, raised after the change.
        this.host.crafting?.inventoryChanged(player, change);
        // What a food preparing converted (playerPrepared).
        this.host.preparing?.inventoryChanged(player, change);
      }
    }
  }

  // InventoryState of a player, or null.
  stateOf(owner) {
    const entry = this.entryOf(owner);
    if (!entry) {
      return null;
    }
    const snapshot = entry.inventory.snapshot();
    return {
      ready: entry.shown === entry.inventory.revision,
      revision: snapshot.revision,
      items: snapshot.items,
      harvested: undefined,
      quickslots: snapshot.quickslots,
    };
  }

  // Runs one operation on an owner's inventory and records the change it made. `notify`: a player's
  // game shows its own notice of what the change gave or took.
  operate(owner, operation, { notify = false } = {}) {
    const entry = this.entryOf(owner);
    if (!entry) {
      return refusal('inventoryUnavailable');
    }
    const { result, change } = operation(entry.inventory);
    this.record(owner, change);
    if (notify) {
      this.announce(owner, change);
    }
    return result;
  }

  // The game's own notice ("You received", and its notice of a loss) in a player's game: units
  // each class gained or lost in a change, one notice per class.
  announce(owner, change) {
    if (!change || !owner || this.host.players.get(owner.id) !== owner) {
      return;
    }
    const net = new Map();
    for (const { before, after } of change.items) {
      const item = (after ?? before).item;
      net.set(item, (net.get(item) ?? 0) + (after?.amount ?? 0) - (before?.amount ?? 0));
    }
    for (const [item, amount] of net) {
      if (amount !== 0) {
        this.host.bridge.send({
          op: 'itemNotice',
          id: owner.id,
          item,
          amount: Math.abs(amount),
          gained: amount > 0,
        });
      }
    }
  }

  // The Inventory global of the server API.
  api() {
    const inventories = this;
    return Object.freeze({
      classes: () => itemClasses(),
      setWearRates: (table) => inventories.wear.set(table),
      getWearRates: () => inventories.wear.get(),
      itemPrice: (item, metadata) =>
        itemPrice(item, metadata, (itemClass) => {
          return inventories.host.itemOverrides.effectiveOf(itemClass).price;
        }),
      renameItem: (item, name) => inventories.renameItem(item, name),
      // The combat numbers of a class for every player.
      overrideStats: (item, stats) => inventories.host.itemOverrides.overrideStats(item, stats),
      itemStats: (item) => inventories.host.itemOverrides.itemStats(item),
      get: (owner) => inventories.stateOf(owner),
      add: (owner, request) => {
        const notify = notifies(request);
        if (notify === undefined) {
          return refusal('invalidRequest');
        }
        return inventories.operate(owner, (inventory) => inventory.add(request), { notify });
      },
      remove: (owner, request) => {
        const notify = notifies(request);
        if (notify === undefined) {
          return refusal('invalidRequest');
        }
        return inventories.operate(owner, (inventory) => inventory.remove(request), { notify });
      },
      editCustom: (owner, request) =>
        inventories.operate(owner, (inventory) => inventory.editCustom(request)),
      editRow: (owner, request) =>
        inventories.operate(owner, (inventory) => inventory.editRow(request)),
      set: (owner, request) => inventories.operate(owner, (inventory) => inventory.set(request)),
      transfer(source, target, request) {
        const from = inventories.entryOf(source);
        const to = inventories.entryOf(target);
        if (!from || !to) {
          return refusal('inventoryUnavailable');
        }
        const notify = notifies(request);
        if (notify === undefined) {
          return refusal('invalidRequest');
        }
        const moved = transfer(from.inventory, to.inventory, request);
        inventories.record(source, moved.change);
        inventories.record(target, moved.targetChange);
        if (notify) {
          inventories.announce(source, moved.change);
          inventories.announce(target, moved.targetChange);
        }
        return moved.result;
      },
    });
  }

  // Player.setOutfit: one of the game's outfits for this player's gender; false for an outfit the
  // catalog does not carry or the other gender's, a player with no inventory, or no room.
  setOutfit(player, preset, gender) {
    const outfit = lookupOutfit(preset);
    if (!outfit || (outfit.gender !== 'any' && outfit.gender !== gender)) {
      return false;
    }
    return this.operate(player, (inventory) => inventory.dressIn(outfit)).ok;
  }

  // Player.equip: one new unit of a piece (inventory/equip_request.cjs) added and worn, what shares
  // its place coming off into the inventory. Throws a TypeError for a request it does not take or a
  // class the body cannot wear; false for a player with no inventory, the same piece worn already,
  // a piece that cannot go on with the rest, or no room.
  equip(player, request) {
    const { itemClass, props } = equipRequestOf(request, 'Player.equip');
    // Horse tack is armour in the tables too, in slots of its own that no person has.
    if (!isGear(itemClass) || HORSE_SLOT.test(itemClass.slot ?? '')) {
      throw new TypeError(`Player.equip: ${itemClass.name} is not worn or held by a person`);
    }
    return this.operate(player, (inventory) => inventory.equip(itemClass, props));
  }

  // Player.outfitSets.
  static outfitSets(gender) {
    if (gender !== undefined && gender !== 'male' && gender !== 'female') {
      throw new TypeError('outfitSets takes male, female or nothing');
    }
    return outfitNames(gender);
  }

  // Inventory.renameItem: what every player's game calls a class and every class that shares its
  // name, until the server stops; null restores the original. Returns their GUIDs.
  renameItem(item, name) {
    if (name !== null) {
      if (
        typeof name !== 'string' ||
        name.length === 0 ||
        Buffer.byteLength(name) > ITEM_NAME_BYTES ||
        /[\x00-\x1f\x7f]/.test(name)
      ) {
        throw new TypeError('An item name is 1 to 128 bytes without control characters, or null');
      }
    }
    const itemClass = lookupItemClass(item);
    if (!itemClass || !itemClass.nameKey) {
      return [];
    }
    if (name === null) {
      this.itemNames.delete(itemClass.nameKey);
    } else {
      this.itemNames.set(itemClass.nameKey, name);
    }
    this.sendNames(0, false, [[itemClass.nameKey, name ?? '']]);
    return classesNamed(itemClass.nameKey).map((entry) => entry.id);
  }

  // A player who came into the world hears every name the server set, and the names of their own
  // rows.
  sendAllNames(player) {
    const entry = this.entryOf(player);
    const rows = entry ? this.rowNamesOf(entry.inventory) : new Map();
    if (entry) {
      entry.rowNames = rows;
    }
    const names = [...this.itemNames.entries(), ...rows.entries()];
    if (names.length > 0) {
      this.sendNames(player.id, true, names);
    }
  }

  // The names a player's rows show in their game alone (custom_items rowNamesOf): key -> text.
  rowNamesOf(inventory) {
    return this.host.customItems?.rowNamesOf(inventory) ?? new Map();
  }

  // Before a player's game shows their stock: the names of their rows that changed since their
  // game last heard them, and '' for the names of rows that lost theirs.
  sendRowNames(player, inventory) {
    const entry = this.entryOf(player);
    if (!entry) {
      return;
    }
    const wanted = this.rowNamesOf(inventory);
    const sent = entry.rowNames ?? new Map();
    const changes = [];
    for (const [key, text] of wanted) {
      if (sent.get(key) !== text) {
        changes.push([key, text]);
      }
    }
    for (const key of sent.keys()) {
      if (!wanted.has(key)) {
        changes.push([key, '']);
      }
    }
    entry.rowNames = wanted;
    if (changes.length > 0) {
      this.sendNames(player.id, false, changes);
    }
  }

  // Player.showItemNotice: the game's notice alone; the inventory stays as it is.
  showNotice(player, item, amount = 1, received = true) {
    const itemClass = lookupItemClass(item);
    if (
      !player ||
      this.host.players.get(player.id) !== player ||
      !itemClass ||
      !Number.isInteger(amount) ||
      amount < 1 ||
      amount > MAX_NOTICE_AMOUNT ||
      typeof received !== 'boolean'
    ) {
      return false;
    }
    this.host.bridge.send({
      op: 'itemNotice',
      id: player.id,
      item: itemClass.id,
      amount,
      gained: received,
    });
    return true;
  }

  // Player.giveItem: their game announces the units.
  giveItem(player, item, amount = 1) {
    return this.operate(player, (inventory) => inventory.add({ item, amount }), { notify: true })
      .ok;
  }

  // Player.takeItem: units of a class across its rows, unworn ones first, all of them or none; their
  // game announces the loss.
  takeItem(player, item, amount = 1) {
    const answer = (result) =>
      Promise.resolve({
        removed: result.ok ? amount : 0,
        requested: amount,
        ok: result.ok,
        reason: result.code,
      });
    const entry = this.entryOf(player);
    if (!entry) {
      return answer(refusal('inventoryUnavailable'));
    }
    const itemClass = lookupItemClass(item);
    if (!itemClass) {
      return answer(refusal('invalidItem'));
    }
    if (!Number.isInteger(amount) || amount < 1 || amount > 10000) {
      return answer(refusal('invalidAmount'));
    }
    const rows = [...entry.inventory.rows.values()]
      .filter((row) => row.itemClass === itemClass)
      .sort((a, b) => a.equipped - b.equipped);
    const units = [];
    let left = amount;
    for (const row of rows) {
      if (left === 0) {
        break;
      }
      const take = Math.min(left, row.amount);
      units.push({ id: row.id, amount: take });
      left -= take;
    }
    if (left > 0) {
      return answer(refusal('insufficientItems'));
    }
    return answer(
      this.operate(player, (inventory) => inventory.remove({ units }), { notify: true }),
    );
  }
}

module.exports = { PlayerInventories };
