'use strict';

// One owner's inventory as the server holds it: its rows, its revision, and the
// operations of the Inventory API. Every operation checks everything first and then happens in
// full, or not at all; one that happened returns the change for playerInventoryChanged.
//
// Each row counts identical units of an item class. Properties are kept normalized
// (`props`): quality, health, and the optional poison, poisonCharges, onEquipBuffs, custom and
// transient.
// What a script reads as `metadata` leaves the defaults out: quality 1 and full condition.

const { isClothing, isGear, lookupItemClass, lookupSlot, qualityBand } = require('./catalog.cjs');
const { equipmentConflicts } = require('./equipment_rules.cjs');

function isAmmo(itemClass) {
  return itemClass.category === 'ammo';
}

const MAX_ADD = 10000;
const MAX_ROWS = 2048;
const MAX_ROW_AMOUNT = 1000000;
const MAX_EQUIPPED_ROWS = 48;
const MAX_UNITS = 2048;
const ROW_ID = /^[A-Za-z0-9_:.-]{1,64}$/;
const GUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
// Health and condition given together may differ by rounding only.
const HEALTH_TOLERANCE = 1e-4;
const METADATA_KEYS = new Set([
  'quality',
  'health',
  'condition',
  'poison',
  'poisonCharges',
  'onEquipBuffs',
  'custom',
  'transient',
]);
const CUSTOM_NAME_BYTES = 128;
const CUSTOM_DESCRIPTION_BYTES = 1024;
const CUSTOM_DATA_BYTES = 16384;
// What a spent unit of a category is: the change's reason and playerItemUsed's kind.
const SPENT_AS = {
  food: { reason: 'use', kind: 'food' },
  ointment: { reason: 'use', kind: 'ointment' },
  poison: { reason: 'use', kind: 'potion' },
  ammo: { reason: 'shot', kind: 'shot' },
};

// `extra` adds fields to the result, such as the conflicts of invalidEquipment.
function refused(code, extra = {}) {
  return { result: { ok: false, code, revision: 0, items: [], ...extra }, change: null };
}

// A plain object of any realm: resources run in their own contexts, whose Object differs.
function isPlainObject(value) {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    return false;
  }
  const prototype = Object.getPrototypeOf(value);
  return prototype === null || Object.getPrototypeOf(prototype) === null;
}

function isWholeNumber(value, low, high) {
  return Number.isInteger(value) && value >= low && value <= high;
}

function hasControlCharacters(text) {
  return /[\x00-\x1f\x7f]/.test(text);
}

// The custom part of the properties: { name?, description?, data? }, or a code.
function customOf(value) {
  if (!isPlainObject(value)) {
    return { code: 'invalidMetadata' };
  }
  const custom = {};
  for (const [key, entry] of Object.entries(value)) {
    if (entry === undefined || entry === null) {
      continue;
    }
    if (key === 'name' || key === 'description') {
      const limit = key === 'name' ? CUSTOM_NAME_BYTES : CUSTOM_DESCRIPTION_BYTES;
      if (
        typeof entry !== 'string' ||
        Buffer.byteLength(entry) > limit ||
        hasControlCharacters(entry)
      ) {
        return { code: 'invalidMetadata' };
      }
      custom[key] = entry;
    } else if (key === 'data') {
      if (!isPlainObject(entry)) {
        return { code: 'invalidMetadata' };
      }
      let text;
      try {
        text = JSON.stringify(entry);
      } catch {
        return { code: 'invalidMetadata' };
      }
      if (Buffer.byteLength(text) > CUSTOM_DATA_BYTES) {
        return { code: 'invalidMetadata' };
      }
      custom.data = JSON.parse(text);
    } else {
      return { code: 'invalidMetadata' };
    }
  }
  return { custom };
}

// A row's normalized properties from a script's metadata, or a code.
function propsOf(itemClass, metadata) {
  if (metadata === undefined || metadata === null) {
    metadata = {};
  }
  if (!isPlainObject(metadata)) {
    return { code: 'invalidMetadata' };
  }
  for (const key of Object.keys(metadata)) {
    if (!METADATA_KEYS.has(key)) {
      return { code: 'invalidMetadata' };
    }
  }
  const quality = metadata.quality ?? 1;
  if (!isWholeNumber(quality, 1, itemClass.maximumQuality)) {
    return { code: 'invalidQuality' };
  }
  const band = qualityBand(itemClass, quality);
  const span = band.high - band.low;
  let health = band.high;
  const { health: givenHealth, condition } = metadata;
  if (condition !== undefined) {
    if (typeof condition !== 'number' || !(condition >= 0 && condition <= 1)) {
      return { code: 'invalidItemHealth' };
    }
    health = band.low + condition * span;
  }
  if (givenHealth !== undefined) {
    if (
      typeof givenHealth !== 'number' ||
      !(givenHealth >= band.low - HEALTH_TOLERANCE && givenHealth <= band.high + HEALTH_TOLERANCE)
    ) {
      return { code: 'invalidItemHealth' };
    }
    if (condition !== undefined && Math.abs(givenHealth - health) > HEALTH_TOLERANCE) {
      return { code: 'contradictoryItemHealth' };
    }
    health = Math.min(band.high, Math.max(band.low, givenHealth));
  }
  const props = { quality, health };
  if (metadata.poison !== undefined || metadata.poisonCharges !== undefined) {
    const poisonable = itemClass.category === 'ammo';
    if (
      !poisonable ||
      typeof metadata.poison !== 'string' ||
      !GUID.test(metadata.poison) ||
      !isWholeNumber(metadata.poisonCharges ?? 1, 1, 1000)
    ) {
      return { code: 'unsupportedPoisonProperties' };
    }
    props.poison = metadata.poison.toLowerCase();
    props.poisonCharges = metadata.poisonCharges ?? 1;
  }
  if (metadata.onEquipBuffs !== undefined) {
    const buffs = metadata.onEquipBuffs;
    if (
      !isGear(itemClass) ||
      !Array.isArray(buffs) ||
      buffs.length > 16 ||
      !buffs.every((buff) => typeof buff === 'string' && GUID.test(buff))
    ) {
      return { code: 'unsupportedOnEquipBuffs' };
    }
    if (buffs.length > 0) {
      props.onEquipBuffs = buffs.map((buff) => buff.toLowerCase());
    }
  }
  if (metadata.custom !== undefined) {
    const { custom, code } = customOf(metadata.custom);
    if (code) {
      return { code };
    }
    if (Object.keys(custom).length > 0) {
      props.custom = custom;
    }
  }
  // A row that is not kept: it leaves the inventory as its player disconnects,
  // before playerDisconnect, so a script that saves the inventory there never saves it.
  if (metadata.transient !== undefined) {
    if (typeof metadata.transient !== 'boolean') {
      return { code: 'invalidMetadata' };
    }
    if (metadata.transient) {
      props.transient = true;
    }
  }
  return { props };
}

// Inventory.itemPrice: what the game prices one unit at, in money units (the units
// of the `money` item, tenths of a groschen; the tables' prices are in them too). The game works
// it out in single precision: the class's price times
// a tenth, times the health factor rounded to hundredths, times ten, cut to a whole number. The
// health factor is health * w + (1 - w), w being the game's RPG parameter (0.99,
// ItemHealthPriceStatusWeight). `pristine` is the price at the best health the item's quality
// allows, `current` at its own health. Null for a class the catalog does not carry or metadata
// Inventory.add would refuse.
const PRICE_HEALTH_WEIGHT = Math.fround(0.99);

function healthFactor(health) {
  const f = Math.fround;
  const weighted = f(f(f(f(health) * PRICE_HEALTH_WEIGHT) + f(1 - PRICE_HEALTH_WEIGHT)) * 100);
  const rounded = weighted < 0 ? Math.trunc(weighted - 0.5) : Math.trunc(weighted + 0.5);
  return f(f(rounded) * f(0.0099999998));
}

function unitPrice(price, health) {
  const f = Math.fround;
  const value = f(healthFactor(health) * f(f(price) * f(0.1)));
  return Math.abs(value) < 1e-38 ? 0 : Math.trunc(f(value * 10));
}

// The prices of an item at its quality and health; priceOf gives a class's base price on this
// server (the item config's), the class's own by default.
function itemPrice(item, metadata, priceOf = (itemClass) => itemClass.price) {
  const itemClass = lookupItemClass(item);
  if (!itemClass) {
    return null;
  }
  const { props, code } = propsOf(itemClass, metadata);
  if (code) {
    return null;
  }
  const price = priceOf(itemClass);
  return {
    pristine: unitPrice(price, qualityBand(itemClass, props.quality).high),
    current: unitPrice(price, props.health),
  };
}

// What a script reads as a row's metadata: every property but the defaults.
function metadataOf(itemClass, props) {
  const metadata = {};
  if (props.quality !== 1) {
    metadata.quality = props.quality;
  }
  const band = qualityBand(itemClass, props.quality);
  if (props.health < band.high) {
    metadata.health = props.health;
    metadata.condition = conditionOf(band, props.health);
  }
  if (props.poison !== undefined) {
    metadata.poison = props.poison;
    metadata.poisonCharges = props.poisonCharges;
  }
  if (props.onEquipBuffs !== undefined) {
    metadata.onEquipBuffs = [...props.onEquipBuffs];
  }
  if (props.custom !== undefined) {
    metadata.custom = structuredClone(props.custom);
  }
  if (props.transient) {
    metadata.transient = true;
  }
  return metadata;
}

function conditionOf(band, health) {
  const span = band.high - band.low;
  return span > 0 ? Math.min(1, Math.max(0, (health - band.low) / span)) : 1;
}

// The conflicts of worn pieces the rows `after` have that the rows `before` did not have. A
// body the game dressed oddly already does not stop a change that leaves that alone.
function newConflicts(before, after) {
  const known = new Set(equipmentConflicts(before).map((conflict) => JSON.stringify(conflict)));
  return equipmentConflicts(after).filter((conflict) => !known.has(JSON.stringify(conflict)));
}

// How many rows have units the body wears.
function wornRows(rows) {
  return rows.filter((row) => row.equipped > 0).length;
}

// Rows with equal keys hold the same class with equal properties.
function propsKey(itemClass, props) {
  const sorted = Object.keys(props)
    .sort()
    .map((key) => [key, props[key]]);
  return `${itemClass.id}|${JSON.stringify(sorted)}`;
}

class Inventory {
  // `maxRows`: how many rows it may hold (a container holds fewer than a player).
  // `harvestable`: the stock of an NPC, which also says whether its carcass was harvested (issue):
  // `harvested` is a boolean for it and undefined for every other owner.
  constructor({ maxRows = MAX_ROWS, harvestable = false } = {}) {
    this.maxRows = maxRows;
    this.harvested = harvestable ? false : undefined;
    this.revision = 0;
    // id -> { id, itemClass, amount, props, equipped }
    this.rows = new Map();
    // Called with each change right after it happened. An NPC's inventory watches the rows of its
    // outfit this way, so units that leave by any path are seen in one place.
    this.onChange = null;
    // The most units of a class it may hold (the server's item config): limitOf gives
    // a whole number, or null for no limit; onLimit hears each refusal (itemClass, max).
    this.limitOf = null;
    this.onLimit = null;
    this.quickslots = null;
    // Quickslots a set restored wait to go to the player's game with the next stock it shows.
    this.quickslotsRestore = false;
    this.nextId = 1;
  }

  // InventoryRow of one row.
  rowView(row) {
    const band = qualityBand(row.itemClass, row.props.quality);
    return {
      id: row.id,
      item: row.itemClass.id,
      name: row.itemClass.name,
      amount: row.amount,
      quality: row.props.quality,
      health: row.props.health,
      condition: conditionOf(band, row.props.health),
      equipped: row.equipped,
      metadata: metadataOf(row.itemClass, row.props),
    };
  }

  // InventoryState without `ready` and `harvested`, which the owner adds.
  snapshot() {
    const snapshot = {
      revision: this.revision,
      items: [...this.rows.values()].map((row) => this.rowView(row)),
      quickslots: this.quickslotsView(),
    };
    if (this.harvested !== undefined) {
      snapshot.harvested = this.harvested;
    }
    return snapshot;
  }

  // InventoryQuickslots as they stand: a reference to a row that went reads as an empty slot. A
  // weapon slot names its weapon whether or not the body wears it: the game counts only the
  // selected slot's weapon as worn, and the other slots keep theirs.
  quickslotsView() {
    if (this.quickslots === null) {
      return null;
    }
    const id = (value) => (value !== null && this.rows.has(value) ? value : null);
    return {
      weapons: this.quickslots.weapons.map((slot) => ({
        primary: id(slot.primary),
        secondary: id(slot.secondary),
      })),
      items: this.quickslots.items.map(id),
      activeWeapon: this.quickslots.activeWeapon,
      activeItem: this.quickslots.activeItem,
    };
  }

  // The quickslots a set restored, once, as the wire's list (quickslotsWire); undefined when none
  // wait.
  takeQuickslotsRestore() {
    if (!this.quickslotsRestore || this.quickslots === null) {
      return undefined;
    }
    this.quickslotsRestore = false;
    return quickslotsWire(this.quickslotsView());
  }

  // What the player's game says its quickslots hold now (the wire's list); a row the stock does
  // not have reads as an empty slot. False for a malformed list.
  reportQuickslots(list) {
    const reported = quickslotsFromWire(list, this.rows);
    if (!reported) {
      return false;
    }
    this.quickslots = reported;
    return true;
  }

  stale(revision) {
    return revision !== undefined && revision !== this.revision;
  }

  freshId() {
    let id;
    do {
      id = `i${this.nextId}`;
      this.nextId += 1;
    } while (this.rows.has(id));
    return id;
  }

  findMatching(itemClass, props) {
    const key = propsKey(itemClass, props);
    for (const row of this.rows.values()) {
      if (propsKey(row.itemClass, row.props) === key) {
        return row;
      }
    }
    return undefined;
  }

  // Applies a list of { id, before, after } row edits (after null removes the row) as one change.
  commit(reason, edits) {
    this.revision += 1;
    const items = [];
    for (const { id, before, next } of edits) {
      if (next === null) {
        this.rows.delete(id);
      } else {
        this.rows.set(id, next);
      }
      items.push({ id, before, after: next === null ? null : this.rowView(next) });
    }
    const change = { revision: this.revision, reason, items };
    if (this.onChange) {
      this.onChange(change);
    }
    return change;
  }

  // Inventory.add.
  // `worn`: ammunition goes on with the units of it the body wears (an arrow taken up from the
  // ground goes back into the quiver).
  add(request, { worn = false } = {}) {
    if (!isPlainObject(request)) {
      return refused('invalidRequest');
    }
    const itemClass = lookupItemClass(request.item);
    if (!itemClass) {
      return refused('invalidItem');
    }
    if (!isWholeNumber(request.amount, 1, MAX_ADD)) {
      return refused('invalidAmount');
    }
    if (itemClass.quest) {
      return refused('questItem');
    }
    const { props, code } = propsOf(itemClass, request.metadata);
    if (code) {
      return refused(code);
    }
    if (this.stale(request.revision)) {
      return refused('staleRevision');
    }
    return this.addUnits(itemClass, props, request.amount, 0, 'add', worn && isAmmo(itemClass));
  }

  // How many units of a class its rows hold.
  unitsOfClass(itemClass) {
    let units = 0;
    for (const row of this.rows.values()) {
      if (row.itemClass.id === itemClass.id) {
        units += row.amount;
      }
    }
    return units;
  }

  // The most units of a class it may hold, or null.
  limitFor(itemClass) {
    const max = this.limitOf ? this.limitOf(itemClass) : null;
    return Number.isInteger(max) ? max : null;
  }

  // How many of `amount` more units of a class fit under its limit, `held` held already; a
  // refusal of any of them is told.
  roomFor(itemClass, amount, held = this.unitsOfClass(itemClass)) {
    const max = this.limitFor(itemClass);
    if (max === null || held + amount <= max) {
      return amount;
    }
    if (this.onLimit) {
      this.onLimit(itemClass, max);
    }
    return Math.max(0, max - held);
  }

  // Adds units of one class with these properties: to the matching row or a new one. `worn`: the
  // units go on with a matching row's when the body wears some.
  addUnits(itemClass, props, amount, equipped, reason, worn = false) {
    if (this.roomFor(itemClass, amount) < amount) {
      return refused('itemLimit');
    }
    const match = this.findMatching(itemClass, props);
    if (match) {
      if (match.amount + amount > MAX_ROW_AMOUNT) {
        return refused('inventoryCapacity');
      }
      const next = { ...match, amount: match.amount + amount };
      if (worn && match.equipped > 0) {
        next.equipped = match.equipped + amount;
      }
      const change = this.commit(reason, [{ id: match.id, before: this.rowView(match), next }]);
      return this.applied(change, [{ id: match.id, amount }]);
    }
    if (this.rows.size >= this.maxRows) {
      return refused('inventoryCapacity');
    }
    const id = this.freshId();
    const next = { id, itemClass, amount, props, equipped };
    const change = this.commit(reason, [{ id, before: null, next }]);
    return this.applied(change, [{ id, amount }]);
  }

  applied(change, items) {
    return { result: { ok: true, code: '', revision: this.revision, items }, change };
  }

  // The rows and amounts of a units list, summed per row, or a code.
  takeOf(units) {
    if (!Array.isArray(units) || units.length === 0 || units.length > MAX_UNITS) {
      return { code: 'invalidRequest' };
    }
    const take = new Map();
    for (const unit of units) {
      if (!isPlainObject(unit) || typeof unit.id !== 'string') {
        return { code: 'invalidRequest' };
      }
      if (!isWholeNumber(unit.amount, 1, MAX_ROW_AMOUNT)) {
        return { code: 'invalidAmount' };
      }
      const row = this.rows.get(unit.id);
      if (!row) {
        return { code: 'unknownItem' };
      }
      take.set(row, (take.get(row) ?? 0) + unit.amount);
    }
    for (const [row, amount] of take) {
      if (amount > row.amount) {
        return { code: 'insufficientItems' };
      }
    }
    return { take };
  }

  // The edits that take these amounts out of their rows.
  removalEdits(take) {
    const edits = [];
    for (const [row, amount] of take) {
      const left = row.amount - amount;
      const next =
        left === 0 ? null : { ...row, amount: left, equipped: Math.min(row.equipped, left) };
      edits.push({ id: row.id, before: this.rowView(row), next });
    }
    return edits;
  }

  // A player dropped units of a row: they go with reason 'drop'. The result carries the
  // row's class and properties as they were, for the stacks that lay them in the world.
  drop(id, amount) {
    const { take, code } = this.takeOf([{ id, amount }]);
    if (code) {
      return refused(code);
    }
    const [[row]] = [...take];
    const dropped = { itemClass: row.itemClass, props: { ...row.props } };
    const change = this.commit('drop', this.removalEdits(take));
    const applied = this.applied(change, [{ id, amount }]);
    return { ...applied, dropped };
  }

  // A vendor's deal: units out of rows (`take`, Map row -> amount, from takeOf), the
  // money balance (`money` units of `moneyClass`, negative when the player pays) and units of
  // classes in (`gains`: [{ itemClass, props, amount }]), all as one change with reason 'trade', or
  // nothing. Money is paid from the money rows in their order and received into the plain one.
  settle(take, moneyClass, money, gains) {
    const plan = this.planSettle(take, moneyClass, money, gains);
    if (plan.code) {
      return refused(plan.code);
    }
    return this.commitSettle(plan);
  }

  // A deal's edits checked against the inventory as it is, committing nothing: { edits, added }
  // or { code } (a trade between two players commits both sides only once both
  // plans hold).
  planSettle(take, moneyClass, money, gains) {
    // Row id -> the row as the deal leaves it (amount 0: gone); rows the deal adds keep `added`.
    const work = new Map();
    const current = (row) => work.get(row.id) ?? row;
    const change = (row, amount) => {
      const before = current(row);
      const left = before.amount + amount;
      work.set(row.id, { ...before, amount: left, equipped: Math.min(before.equipped, left) });
    };
    for (const [row, amount] of take) {
      change(row, -amount);
    }
    if (money < 0) {
      let owed = -money;
      for (const row of this.rows.values()) {
        if (owed === 0) {
          break;
        }
        const free = current(row).amount;
        if (row.itemClass !== moneyClass || free === 0) {
          continue;
        }
        const paid = Math.min(free, owed);
        change(row, -paid);
        owed -= paid;
      }
      if (owed > 0) {
        return { code: 'insufficientMoney' };
      }
    }
    const incoming = [...gains];
    if (money > 0) {
      incoming.push({ itemClass: moneyClass, props: propsOf(moneyClass).props, amount: money });
    }
    const added = [];
    for (const { itemClass, props, amount } of incoming) {
      const key = propsKey(itemClass, props);
      const fresh = added.find((row) => propsKey(row.itemClass, row.props) === key);
      if (fresh) {
        fresh.amount += amount;
        continue;
      }
      const match = [...this.rows.values()].find(
        (row) => current(row).amount > 0 && propsKey(row.itemClass, row.props) === key,
      );
      if (match) {
        change(match, amount);
        continue;
      }
      added.push({ id: '', itemClass, amount, props, equipped: 0 });
    }
    // The server's limits per class: what the deal brings may not take one over.
    for (const { itemClass } of gains) {
      const max = this.limitFor(itemClass);
      if (max === null) {
        continue;
      }
      let after = 0;
      for (const row of [...this.rows.values(), ...added]) {
        if (row.itemClass.id === itemClass.id) {
          after += row.id === '' ? row.amount : current(row).amount;
        }
      }
      if (after > max && after > this.unitsOfClass(itemClass)) {
        if (this.onLimit) {
          this.onLimit(itemClass, max);
        }
        return { code: 'itemLimit' };
      }
    }
    let rows = this.rows.size + added.length;
    for (const row of work.values()) {
      if (row.amount === 0) {
        rows -= 1;
      }
    }
    const tooMuch = [...work.values(), ...added].some((row) => row.amount > MAX_ROW_AMOUNT);
    if (rows > this.maxRows || tooMuch) {
      return { code: 'inventoryCapacity' };
    }
    const edits = [];
    for (const [id, next] of work) {
      edits.push({
        id,
        before: this.rowView(this.rows.get(id)),
        next: next.amount === 0 ? null : next,
      });
    }
    return { edits, added };
  }

  // Commits a plan from planSettle as one change with reason 'trade'.
  commitSettle({ edits, added }) {
    const taken = new Set(this.rows.keys());
    const items = [];
    for (const row of added) {
      row.id = this.freshIdAvoiding(taken);
      taken.add(row.id);
      edits.push({ id: row.id, before: null, next: row });
      items.push({ id: row.id, amount: row.amount });
    }
    return this.applied(this.commit('trade', edits), items);
  }

  // Inventory.remove.
  remove(request) {
    if (!isPlainObject(request)) {
      return refused('invalidRequest');
    }
    const { take, code } = this.takeOf(request.units);
    if (code) {
      return refused(code);
    }
    if (this.stale(request.revision)) {
      return refused('staleRevision');
    }
    const change = this.commit('remove', this.removalEdits(take));
    return this.applied(change, []);
  }

  // The rows that are not kept (metadata transient) leave, all as one change with
  // reason 'transient'; null when there are none.
  dropTransient() {
    const take = new Map();
    for (const row of this.rows.values()) {
      if (row.props.transient) {
        take.set(row, row.amount);
      }
    }
    return take.size > 0 ? this.commit('transient', this.removalEdits(take)) : null;
  }

  // Inventory.editRow: replaces a row's properties; it keeps its identity and amount.
  editRow(request) {
    if (!isPlainObject(request) || typeof request.id !== 'string') {
      return refused('invalidRequest');
    }
    const row = this.rows.get(request.id);
    if (!row) {
      return refused('unknownItem');
    }
    if (!isPlainObject(request.metadata)) {
      return refused('invalidMetadata');
    }
    const { props, code } = propsOf(row.itemClass, request.metadata);
    if (code) {
      return refused(code);
    }
    if (this.stale(request.revision)) {
      return refused('staleRevision');
    }
    const next = { ...row, props };
    const change = this.commit('properties', [{ id: row.id, before: this.rowView(row), next }]);
    return this.applied(change, [{ id: row.id, amount: row.amount }]);
  }

  // Inventory.editCustom: only the named custom fields; null resets one.
  editCustom(request) {
    if (!isPlainObject(request) || typeof request.id !== 'string') {
      return refused('invalidRequest');
    }
    const row = this.rows.get(request.id);
    if (!row) {
      return refused('unknownItem');
    }
    const custom = { ...(row.props.custom ?? {}) };
    for (const key of ['name', 'description', 'data']) {
      if (!(key in request)) {
        continue;
      }
      if (request[key] === null) {
        delete custom[key];
      } else {
        custom[key] = request[key];
      }
    }
    const checked = customOf(custom);
    if (checked.code) {
      return refused(checked.code);
    }
    if (this.stale(request.revision)) {
      return refused('staleRevision');
    }
    const props = { ...row.props };
    delete props.custom;
    if (Object.keys(checked.custom).length > 0) {
      props.custom = checked.custom;
    }
    const next = { ...row, props };
    const change = this.commit('properties', [{ id: row.id, before: this.rowView(row), next }]);
    return this.applied(change, [{ id: row.id, amount: row.amount }]);
  }

  // What the player's game changed on its own (part 3): [{ kind: 'lost' | 'worn' | 'gained', id
  // (lost, worn), item (gained, GUID), amount, quality, health }]. Applied as one change per
  // reason, in the order the reasons first appear: units spent (`use` for food, potions and
  // ointments, `shot` for ammunition) or otherwise gone (`drop`); worn down (`wear`) or mended
  // (`repair`); picked up (`loot`). Returns { changes, used: [{ item, kind, amount }] }. A change
  // naming a row this inventory does not hold, or a class the catalog does not carry, is left out.
  // `wearRate(itemClass)` scales a drop in condition; `corrected` says the game's
  // condition of an item differs from the stock's now, which the player's game is to be shown.
  applyNative(reports, wearRate = () => 1) {
    this.wearRate = wearRate;
    this.corrected = false;
    const groups = new Map();
    const group = (reason) => {
      if (!groups.has(reason)) {
        groups.set(reason, []);
      }
      return groups.get(reason);
    };
    const used = [];
    let equipment = false;
    for (const report of Array.isArray(reports) ? reports : []) {
      if (isPlainObject(report) && report.kind === 'equipped') {
        equipment = this.wearing(report.id, report.amount) || equipment;
        continue;
      }
      if (!isPlainObject(report) || !isWholeNumber(report.amount, 1, MAX_ROW_AMOUNT)) {
        continue;
      }
      if (report.kind === 'gained') {
        const itemClass = lookupItemClass(report.item);
        if (itemClass) {
          group('loot').push({ kind: 'gained', itemClass, report });
        }
        continue;
      }
      const row = this.rows.get(report.id);
      if (!row) {
        continue;
      }
      if (report.kind === 'lost') {
        const spend = SPENT_AS[row.itemClass.category];
        group(spend ? spend.reason : 'drop').push({ kind: 'lost', row: row.id, report });
        if (spend) {
          used.push({ item: row.itemClass.id, kind: spend.kind, amount: report.amount });
        }
      } else if (report.kind === 'worn') {
        const reason = Number(report.health) < row.props.health ? 'wear' : 'repair';
        group(reason).push({ kind: 'worn', row: row.id, report });
      }
    }
    const changes = [];
    for (const [reason, entries] of groups) {
      const change = this.applyNativeGroup(reason, entries);
      if (change) {
        changes.push(change);
      }
    }
    // What the body wears changes no item: the revision stays, and Inventory.get has it.
    if (equipment) {
      changes.push({ revision: this.revision, reason: 'equipment', items: [] });
    }
    return { changes, used, corrected: this.corrected };
  }

  // Player.setOutfit: every piece of clothing the body wears comes off and stays, and
  // a new set of the outfit's clothes is added and worn; weapons and everything else stay as they
  // are. One change, reason `outfit`; refused with `inventoryCapacity` when the rows do not fit.
  dressIn(outfit) {
    const next = new Map();
    const current = (id) => (next.has(id) ? next.get(id) : this.rows.get(id));
    for (const row of this.rows.values()) {
      if (row.equipped > 0 && isClothing(row.itemClass)) {
        next.set(row.id, { ...row, equipped: 0 });
      }
    }
    let newRows = 0;
    for (const itemClass of outfit.items) {
      const props = { quality: 1, health: qualityBand(itemClass, 1).high };
      const key = propsKey(itemClass, props);
      const match = [...new Set([...this.rows.keys(), ...next.keys()])]
        .map(current)
        .find((row) => row && propsKey(row.itemClass, row.props) === key);
      if (match) {
        next.set(match.id, { ...match, amount: match.amount + 1, equipped: match.equipped + 1 });
        continue;
      }
      if (this.rows.size + newRows >= this.maxRows) {
        return refused('inventoryCapacity');
      }
      newRows += 1;
      const id = this.freshIdAvoiding(new Set(next.keys()));
      next.set(id, { id, itemClass, amount: 1, props, equipped: 1 });
    }
    if (next.size === 0) {
      return refused('invalidRequest');
    }
    const edits = [...next.entries()].map(([id, row]) => {
      const before = this.rows.get(id);
      return { id, before: before ? this.rowView(before) : null, next: row };
    });
    const change = this.commit('outfit', edits);
    return this.applied(
      change,
      outfit.items.map((itemClass) => ({ id: itemClass.id, amount: 1 })),
    );
  }

  // Player.equip: one new unit of a gear class with these properties is added and worn. What the
  // body wears in the same place comes off and stays in the inventory. One change, reason `equip`.
  // Refused with `alreadyWorn` when a worn row holds the same class with the same properties,
  // `invalidEquipment` when the piece cannot go on with what stays on (a piece it goes over is
  // missing, or one it cannot be worn with is on), and as add refuses a unit it has no room for.
  equip(itemClass, props) {
    const match = this.findMatching(itemClass, props);
    if (match && match.equipped > 0) {
      return refused('alreadyWorn');
    }
    if (this.roomFor(itemClass, 1) < 1) {
      return refused('itemLimit');
    }
    const next = new Map();
    const slot = lookupSlot(itemClass.slot);
    if (slot) {
      for (const row of this.rows.values()) {
        const own = row.equipped > 0 ? lookupSlot(row.itemClass.slot) : undefined;
        if (own && own.place === slot.place) {
          next.set(row.id, { ...row, equipped: 0 });
        }
      }
    }
    let id;
    if (match) {
      if (match.amount + 1 > MAX_ROW_AMOUNT) {
        return refused('inventoryCapacity');
      }
      id = match.id;
      next.set(id, { ...match, amount: match.amount + 1, equipped: 1 });
    } else {
      if (this.rows.size >= this.maxRows) {
        return refused('inventoryCapacity');
      }
      id = this.freshId();
      next.set(id, { id, itemClass, amount: 1, props, equipped: 1 });
    }
    const before = [...this.rows.values()];
    const after = before.map((row) => next.get(row.id) ?? row);
    if (!match) {
      after.push(next.get(id));
    }
    const conflicts = newConflicts(before, after);
    if (conflicts.length > 0) {
      return refused('invalidEquipment', { conflicts });
    }
    if (wornRows(after) > MAX_EQUIPPED_ROWS) {
      return refused('invalidEquipment');
    }
    const edits = [...next.entries()].map(([rowId, row]) => {
      const was = this.rows.get(rowId);
      return { id: rowId, before: was ? this.rowView(was) : null, next: row };
    });
    const change = this.commit('equip', edits);
    this.readyInSet(itemClass, id);
    return this.applied(change, [{ id, amount: 1 }]);
  }

  // Player.equip readies a weapon or ammunition the way the game's own Equip does: a weapon
  // becomes the selected weapon set's weapon (the one there stays in the inventory), a bow or
  // crossbow with the ammunition the body wears as its second item, and ammunition becomes the
  // second item of the set holding a missile weapon, the selected set first. Only with the
  // quickslots known; the changed sets go to the player's game with the next stock it shows.
  readyInSet(itemClass, id) {
    if (this.quickslots === null) {
      return;
    }
    const sets = this.quickslots.weapons;
    const active = this.quickslots.activeWeapon;
    const categoryOf = (rowId) => this.rows.get(rowId)?.itemClass.category;
    if (itemClass.category === 'meleeWeapon' || itemClass.category === 'missileWeapon') {
      let secondary = null;
      if (itemClass.category === 'missileWeapon') {
        const ammo = [...this.rows.values()].find((row) => isAmmo(row.itemClass) && row.equipped > 0);
        secondary = ammo ? ammo.id : null;
      } else if (categoryOf(sets[active].secondary) !== 'ammo') {
        secondary = sets[active].secondary;
      }
      sets[active] = { primary: id, secondary };
    } else if (isAmmo(itemClass)) {
      const order = [active, 0, 1, 2, 3].filter((slot, index, all) => all.indexOf(slot) === index);
      const slot = order.find((each) => categoryOf(sets[each].primary) === 'missileWeapon');
      if (slot === undefined) {
        return;
      }
      sets[slot] = { primary: sets[slot].primary, secondary: id };
    } else {
      return;
    }
    this.quickslotsRestore = true;
  }

  // An NPC's outfit: `leaving` ([{ id, amount }]) are units that come out of the rows
  // they name, worn ones first, and `pieces` ([{ itemClass, props, amount }]) go in as worn units,
  // joining the row of the same class and properties when there is one. One change, reason
  // `outfit`; nothing changes and the code is `inventoryCapacity` when the rows do not fit. The
  // result's items name the row each piece went to.
  dressWorn(leaving, pieces) {
    const next = new Map();
    const current = (id) => (next.has(id) ? next.get(id) : this.rows.get(id));
    for (const { id, amount } of leaving) {
      const row = current(id);
      if (!row) {
        continue;
      }
      const taken = Math.min(amount, row.amount);
      const left = row.amount - taken;
      const worn = Math.max(0, row.equipped - taken);
      next.set(id, left === 0 ? null : { ...row, amount: left, equipped: worn });
    }
    const placed = [];
    for (const { itemClass, props, amount } of pieces) {
      const key = propsKey(itemClass, props);
      const match = [...new Set([...this.rows.keys(), ...next.keys()])]
        .map(current)
        .find((row) => row && propsKey(row.itemClass, row.props) === key);
      if (match) {
        if (match.amount + amount > MAX_ROW_AMOUNT) {
          return refused('inventoryCapacity');
        }
        const joined = { ...match, amount: match.amount + amount };
        next.set(match.id, { ...joined, equipped: match.equipped + amount });
        placed.push({ id: match.id, amount });
        continue;
      }
      const id = this.freshIdAvoiding(new Set([...this.rows.keys(), ...next.keys()]));
      next.set(id, { id, itemClass, amount, props, equipped: amount });
      placed.push({ id, amount });
    }
    if (next.size === 0) {
      return this.applied(null, placed);
    }
    const ids = new Set([...this.rows.keys(), ...next.keys()]);
    const kept = [...ids].filter((id) => current(id)).length;
    if (kept > this.maxRows) {
      return refused('inventoryCapacity');
    }
    const edits = [...next.entries()].map(([id, row]) => {
      const before = this.rows.get(id);
      return { id, before: before ? this.rowView(before) : null, next: row };
    });
    const change = this.commit('outfit', edits);
    return this.applied(change, placed);
  }

  // The body wears `units` of a row now; false when that changes nothing.
  wearing(id, units) {
    const row = this.rows.get(id);
    if (!row || !Number.isInteger(units) || units < 0) {
      return false;
    }
    const equipped = isGear(row.itemClass) ? Math.min(units, row.amount) : 0;
    if (equipped === row.equipped) {
      return false;
    }
    this.rows.set(id, { ...row, equipped });
    return true;
  }

  // One reason's native changes as one commit, or null when nothing was left to change.
  applyNativeGroup(reason, entries) {
    // Rows as they will be: id -> row (null: emptied), and the rows the group touched.
    const next = new Map();
    const touched = [];
    const current = (id) => (next.has(id) ? next.get(id) : this.rows.get(id));
    const touch = (id, row) => {
      if (!touched.includes(id)) {
        touched.push(id);
      }
      next.set(id, row);
    };
    // `worn`: ammunition the player picked up goes on with the units of it the body wears
    // (back into the quiver).
    const land = (itemClass, props, amount, worn = false) => {
      const key = propsKey(itemClass, props);
      for (const id of [...this.rows.keys(), ...next.keys()]) {
        const row = current(id);
        if (row && propsKey(row.itemClass, row.props) === key) {
          const total = Math.min(MAX_ROW_AMOUNT, row.amount + amount);
          const equipped =
            worn && row.equipped > 0 ? row.equipped + (total - row.amount) : row.equipped;
          touch(id, { ...row, amount: total, equipped });
          return;
        }
      }
      if (this.rows.size + touched.length >= this.maxRows) {
        return;
      }
      const id = this.freshId();
      touch(id, { id, itemClass, amount, props, equipped: 0 });
    };
    // Units of a class the rows will hold.
    const heldOf = (itemClass) => {
      let units = 0;
      for (const id of new Set([...this.rows.keys(), ...next.keys()])) {
        const row = current(id);
        if (row && row.itemClass.id === itemClass.id) {
          units += row.amount;
        }
      }
      return units;
    };
    for (const entry of entries) {
      if (entry.kind === 'gained') {
        // Units over the class's limit are not taken; the game is shown the stock without them.
        const room = this.roomFor(entry.itemClass, entry.report.amount, heldOf(entry.itemClass));
        if (room < entry.report.amount) {
          this.corrected = true;
        }
        if (room === 0) {
          continue;
        }
        const quality = Math.min(
          entry.itemClass.maximumQuality,
          Math.max(1, Math.round(entry.report.quality || 1)),
        );
        const band = qualityBand(entry.itemClass, quality);
        const health = Math.min(band.high, Math.max(band.low, Number(entry.report.health) || 0));
        land(entry.itemClass, { quality, health }, room, isAmmo(entry.itemClass));
        continue;
      }
      const row = current(entry.row);
      if (!row) {
        continue;
      }
      // A drop in condition is the game's times the class's wear rate.
      let reported = Number(entry.report.health) || 0;
      if (entry.kind === 'worn' && reason === 'wear') {
        const rate = this.wearRate ? this.wearRate(row.itemClass) : 1;
        if (rate !== 1) {
          reported = row.props.health - (row.props.health - reported) * rate;
          this.corrected = true;
        }
        if (Math.abs(reported - row.props.health) < 1e-9) {
          continue;
        }
      }
      const amount = Math.min(row.amount, entry.report.amount);
      const left = row.amount - amount;
      touch(
        row.id,
        left === 0 ? null : { ...row, amount: left, equipped: Math.min(row.equipped, left) },
      );
      if (entry.kind === 'worn') {
        const quality = Math.min(
          row.itemClass.maximumQuality,
          Math.max(1, Math.round(entry.report.quality || row.props.quality)),
        );
        const band = qualityBand(row.itemClass, quality);
        const health = Math.min(band.high, Math.max(band.low, reported));
        land(row.itemClass, { ...row.props, quality, health }, amount);
      }
    }
    if (touched.length === 0) {
      return null;
    }
    const edits = touched.map((id) => {
      const before = this.rows.get(id);
      return { id, before: before ? this.rowView(before) : null, next: next.get(id) };
    });
    return this.commit(reason, edits);
  }

  // The census of what the player's game held when they came into the world, taken as their
  // stock while nothing else set one: [{ item (GUID), amount, quality, health, equipped }].
  // Classes the catalog does not carry are left out; quality and health are kept inside what the
  // class allows. Units with the same properties share a row.
  adopt(census) {
    if (!Array.isArray(census)) {
      return refused('invalidRequest');
    }
    const edits = [];
    const rows = new Map();
    for (const entry of census) {
      const itemClass = isPlainObject(entry) ? lookupItemClass(entry.item) : undefined;
      if (!itemClass || !isWholeNumber(entry.amount, 1, MAX_ROW_AMOUNT)) {
        continue;
      }
      const quality = Math.min(
        itemClass.maximumQuality,
        Math.max(1, Math.round(entry.quality || 1)),
      );
      const band = qualityBand(itemClass, quality);
      const health = Math.min(band.high, Math.max(band.low, Number(entry.health) || 0));
      const props = { quality, health };
      const key = propsKey(itemClass, props);
      const row = rows.get(key);
      const equipped = isGear(itemClass) && Number.isInteger(entry.equipped) ? entry.equipped : 0;
      if (row) {
        row.next.amount += entry.amount;
        row.next.equipped = Math.min(row.next.amount, row.next.equipped + equipped);
        continue;
      }
      if (rows.size >= this.maxRows) {
        break;
      }
      const id = this.freshId();
      const edit = {
        id,
        before: null,
        next: {
          id,
          itemClass,
          amount: entry.amount,
          props,
          equipped: Math.min(entry.amount, equipped),
        },
      };
      rows.set(key, edit);
      edits.push(edit);
    }
    for (const row of this.rows.values()) {
      edits.unshift({ id: row.id, before: this.rowView(row), next: null });
    }
    this.rows.clear();
    const change = this.commit('census', edits);
    return this.applied(
      change,
      edits.filter((edit) => edit.next).map((edit) => ({ id: edit.id, amount: edit.next.amount })),
    );
  }

  // Inventory.set: replaces everything; rows keep the ids they are given. The pieces it leaves
  // equipped must go on a body together (equipment_rules.cjs), else invalidEquipment. Rows that
  // are not equipped, such as the stock of a chest, are not checked.
  set(request) {
    if (
      !isPlainObject(request) ||
      !Array.isArray(request.items) ||
      request.items.length > this.maxRows
    ) {
      return refused('invalidRequest');
    }
    const rows = [];
    const ids = new Set();
    let equippedRows = 0;
    for (const entry of request.items) {
      if (!isPlainObject(entry)) {
        return refused('invalidItems');
      }
      const itemClass = lookupItemClass(entry.item);
      if (!itemClass) {
        return refused('invalidItem');
      }
      if (!isWholeNumber(entry.amount, 1, MAX_ROW_AMOUNT)) {
        return refused('invalidAmount');
      }
      if (entry.id !== undefined) {
        if (typeof entry.id !== 'string' || !ROW_ID.test(entry.id) || ids.has(entry.id)) {
          return refused('invalidItems');
        }
        ids.add(entry.id);
      }
      const equipped = entry.equipped ?? 0;
      if (!isWholeNumber(equipped, 0, entry.amount) || (equipped > 0 && !isGear(itemClass))) {
        return refused('invalidEquipment');
      }
      if (equipped > 0) {
        equippedRows += 1;
      }
      const { props, code } = propsOf(itemClass, entry.metadata);
      if (code) {
        return refused(code);
      }
      rows.push({ id: entry.id, itemClass, amount: entry.amount, props, equipped });
    }
    if (equippedRows > MAX_EQUIPPED_ROWS) {
      return refused('invalidEquipment');
    }
    // Pieces the body cannot wear together, named in the result.
    const conflicts = equipmentConflicts(rows);
    if (conflicts.length > 0) {
      return refused('invalidEquipment', { conflicts });
    }
    const quickslots = request.quickslots ?? null;
    if (quickslots !== null && !quickslotsValid(quickslots, ids)) {
      return refused('invalidQuickslots');
    }
    // `harvested` restores a carcass's flag; absent keeps it. Only an NPC's stock has one.
    if (
      request.harvested !== undefined &&
      (typeof request.harvested !== 'boolean' || this.harvested === undefined)
    ) {
      return refused('invalidRequest');
    }
    if (this.stale(request.revision)) {
      return refused('staleRevision');
    }
    if (request.harvested !== undefined) {
      this.harvested = request.harvested;
    }
    const edits = [];
    for (const row of this.rows.values()) {
      edits.push({ id: row.id, before: this.rowView(row), next: null });
    }
    const before = new Map(edits.map((edit) => [edit.id, edit]));
    this.rows.clear();
    const written = [];
    for (const row of rows) {
      const id = row.id ?? this.freshIdAvoiding(ids);
      ids.add(id);
      const next = { ...row, id };
      const old = before.get(id);
      if (old) {
        old.next = next;
      } else {
        edits.push({ id, before: null, next });
      }
      written.push({ id, amount: row.amount });
    }
    this.quickslots = quickslots === null ? null : structuredClone(quickslots);
    this.quickslotsRestore = quickslots !== null;
    const change = this.commit('set', edits);
    return this.applied(change, written);
  }

  freshIdAvoiding(taken) {
    let id;
    do {
      id = `i${this.nextId}`;
      this.nextId += 1;
    } while (taken.has(id));
    return id;
  }
}

// Four weapon slots of { primary, secondary }, four item slots, and the two selections; every row
// id among them must be one of `ids`.
function quickslotsValid(quickslots, ids) {
  if (!isPlainObject(quickslots)) {
    return false;
  }
  const { weapons, items, activeWeapon, activeItem } = quickslots;
  const known = (id) => id === null || (typeof id === 'string' && ids.has(id));
  return (
    Array.isArray(weapons) &&
    weapons.length === 4 &&
    weapons.every((slot) => isPlainObject(slot) && known(slot.primary) && known(slot.secondary)) &&
    Array.isArray(items) &&
    items.length === 4 &&
    items.every(known) &&
    isWholeNumber(activeWeapon, 0, 3) &&
    isWholeNumber(activeItem, 0, 3)
  );
}

// Quickslots on the wire (kcd2s/integration/inventory.hpp): twelve row ids, "" for an empty slot
// (each weapon slot's primary and secondary in turn, then the item slots), then the selected
// weapon slot and item slot.
function quickslotsWire(quickslots) {
  const id = (value) => value ?? '';
  return [
    ...quickslots.weapons.flatMap((slot) => [id(slot.primary), id(slot.secondary)]),
    ...quickslots.items.map(id),
    quickslots.activeWeapon,
    quickslots.activeItem,
  ];
}

// InventoryQuickslots of a wire list, with only rows of `rows`; null for a malformed list.
function quickslotsFromWire(list, rows) {
  if (!Array.isArray(list) || list.length !== 14) {
    return null;
  }
  const [activeWeapon, activeItem] = list.slice(12);
  if (!isWholeNumber(activeWeapon, 0, 3) || !isWholeNumber(activeItem, 0, 3)) {
    return null;
  }
  const id = (value) => (typeof value === 'string' && rows.has(value) ? value : null);
  const weapons = [0, 1, 2, 3].map((slot) => ({
    primary: id(list[slot * 2]),
    secondary: id(list[slot * 2 + 1]),
  }));
  return { weapons, items: list.slice(8, 12).map(id), activeWeapon, activeItem };
}

// Inventory.transfer: exact units with their properties, all or nothing. The result names the
// target's rows; its revision is the source's. `reasons`: the change reason on each side.
function transfer(source, target, request, reasons = { source: 'transfer', target: 'transfer' }) {
  if (!isPlainObject(request)) {
    return { ...refused('invalidRequest'), targetChange: null };
  }
  if (source === target) {
    return { ...refused('sameInventory'), targetChange: null };
  }
  const { take, code } = source.takeOf(request.units);
  if (code) {
    return { ...refused(code), targetChange: null };
  }
  if (source.stale(request.sourceRevision) || target.stale(request.targetRevision)) {
    return { ...refused('staleRevision'), targetChange: null };
  }
  // Where each moved row lands in the target, checked before anything changes.
  const landings = [];
  let newRows = 0;
  for (const [row, amount] of take) {
    const match = target.findMatching(row.itemClass, row.props);
    if (match && match.amount + amount > MAX_ROW_AMOUNT) {
      return { ...refused('inventoryCapacity'), targetChange: null };
    }
    if (!match) {
      newRows += 1;
    }
    landings.push({ row, amount });
  }
  if (target.rows.size + newRows > target.maxRows) {
    return { ...refused('inventoryCapacity'), targetChange: null };
  }
  // The target's limits per class.
  const arriving = new Map();
  for (const [row, amount] of take) {
    const known = arriving.get(row.itemClass.id);
    arriving.set(row.itemClass.id, {
      itemClass: row.itemClass,
      amount: (known?.amount ?? 0) + amount,
    });
  }
  for (const { itemClass, amount } of arriving.values()) {
    if (target.roomFor(itemClass, amount) < amount) {
      return { ...refused('itemLimit'), targetChange: null };
    }
  }
  const sourceChange = source.commit(reasons.source, source.removalEdits(take));
  const edits = [];
  const received = [];
  const pending = new Map();
  for (const { row, amount } of landings) {
    const key = propsKey(row.itemClass, row.props);
    const queued = pending.get(key);
    if (queued) {
      queued.next = { ...queued.next, amount: queued.next.amount + amount };
      received.push({ id: queued.id, amount });
      continue;
    }
    const match = target.findMatching(row.itemClass, row.props);
    const edit = match
      ? {
          id: match.id,
          before: target.rowView(match),
          next: { ...match, amount: match.amount + amount },
        }
      : {
          id: target.freshId(),
          before: null,
          next: { itemClass: row.itemClass, amount, props: row.props, equipped: 0 },
        };
    edit.next.id = edit.id;
    pending.set(key, edit);
    edits.push(edit);
    received.push({ id: edit.id, amount });
  }
  const targetChange = target.commit(reasons.target, edits);
  return {
    result: { ok: true, code: '', revision: source.revision, items: received },
    change: sourceChange,
    targetChange,
  };
}

module.exports = { Inventory, itemPrice, transfer, propsOf, metadataOf };
