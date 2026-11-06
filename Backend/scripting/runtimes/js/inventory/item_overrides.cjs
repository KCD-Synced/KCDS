'use strict';

// What the server changes of the game's own items: a class's weight and price, and
// the most units of it a player may hold, from the server's item config (item-config.json in the
// resources folder) and from resources at run time (Items.override). Every player's game gets the
// whole set when the player comes into the world and again on each change; prices the server
// computes (Inventory.itemPrice) use it.
//
// item-config.json: { "<item GUID or table name>": { "weight": kg, "price": price, "max": units,
// <stat>: value, ... }, ... }. The stats an item may take depend on its category (item_stats.cjs:
// armour defense, weapon attack, food health and so on). A field left out keeps the game's value
// (no limit for max). Each entry is checked on start: a wrong entry is named in the log and left
// out, the others apply.
//
// A limited class's limit shows in every player's inventory, in a max column after the amount
// (its tooltip the locale text items.maxTooltip in their language); a player who would take more
// than its limit gets none of the units over it and the notice items.limitReached.

const fs = require('node:fs');
const path = require('node:path');

const { foodOf, lookupItemClass } = require('./catalog.cjs');
const { statNames, statOf } = require('./item_stats.cjs');

const CONFIG_FILE = 'item-config.json';
// The limits of the wire (kcd2s/integration/item_overrides.hpp).
const MAX_WEIGHT = 100000;
const MAX_PRICE = 100000000;
const MAX_LIMIT = 1000000;
const MAX_OVERRIDES = 8192;
const FIELDS = new Set(['weight', 'price', 'max']);
// The locale texts of a limit (locales/locales.cjs).
const MAX_TOOLTIP = 'items.maxTooltip';
const LIMIT_REACHED = 'items.limitReached';
// The longest tooltip and notice a player's game takes (kcd2s/integration/item_overrides.hpp).
const MAX_TOOLTIP_BYTES = 128;
const MAX_NOTICE_BYTES = 256;
// How long a player is not told again of the same class's limit, in milliseconds.
const NOTICE_QUIET_MS = 2000;
// Inventory.overrideStats: the combat numbers each kind of class takes, by the names
// that call uses, and the stat each one is among the overrides' (item_stats.cjs).
const COMBAT_STATS = {
  meleeWeapon: [
    'attack',
    'attackModStab',
    'attackModSlash',
    'attackModSmash',
    'defense',
    'strReq',
    'agiReq',
  ],
  armor: ['defenseStab', 'defenseSlash', 'defenseSmash', 'noise', 'strReq'],
  helmet: ['defenseStab', 'defenseSlash', 'defenseSmash', 'noise', 'strReq'],
  hood: ['defenseStab', 'defenseSlash', 'defenseSmash', 'noise', 'strReq'],
  missileWeapon: ['power', 'defense', 'strReq', 'agiReq'],
  ammo: ['attackStab', 'attackSlash', 'attackSmash', 'powerMod'],
};
const STAT_NAMES = { strReq: 'strengthRequired', agiReq: 'agilityRequired' };

// The text cut to at most `bytes` bytes of UTF-8, whole characters only.
function fitted(text, bytes) {
  let fit = text;
  while (Buffer.byteLength(fit, 'utf8') > bytes) {
    fit = Array.from(fit).slice(0, -1).join('');
  }
  return fit;
}

class ItemOverrides {
  constructor(host) {
    this.host = host;
    // Class id -> { itemClass, weight, price, max, stats }; null (stats: none) where the game's
    // own stay. stats: { <name>: value }.
    this.overrides = new Map();
    // "<player id>:<class id>" -> when that player was last told of that class's limit.
    this.told = new Map();
    // A player's own limits: player id -> Map(class id -> units), over the server's.
    this.playerLimits = new Map();
    // How much of what players wear and hold equipped counts toward their load, in percent
    // the server's, and a player's own over it (player id -> percent).
    this.equippedPercent = 100;
    this.playerEquippedPercent = new Map();
  }

  // The equipped share that applies to a player.
  equippedPercentOf(player) {
    return this.playerEquippedPercent.get(player.id) ?? this.equippedPercent;
  }

  // Items.setEquippedWeightPercent: the server's share, 0 to 100 (100 the game's own).
  setEquippedPercent(percent) {
    if (!Number.isInteger(percent) || percent < 0 || percent > 100) {
      throw new TypeError('The equipped weight share is a whole percent 0 to 100');
    }
    this.equippedPercent = percent;
    this.send(0);
    return percent;
  }

  // Player.setEquippedWeightPercent: the player's own share over the server's; null clears it.
  // The share that applies to them now.
  setPlayerEquippedPercent(player, percent) {
    if (percent === null) {
      this.playerEquippedPercent.delete(player.id);
    } else if (!Number.isInteger(percent) || percent < 0 || percent > 100) {
      throw new TypeError('The equipped weight share is a whole percent 0 to 100, or null');
    } else {
      this.playerEquippedPercent.set(player.id, percent);
    }
    if (player.ready) {
      this.sendTo(player);
    }
    return this.equippedPercentOf(player);
  }

  // The override a request describes, or a TypeError naming what is wrong.
  check(item, fields) {
    const itemClass = lookupItemClass(item);
    if (!itemClass) {
      throw new TypeError(`unknown item ${JSON.stringify(item)}`);
    }
    if (itemClass.custom) {
      throw new TypeError(`${itemClass.name} is a registered type; Items.register sets its values`);
    }
    if (!fields || typeof fields !== 'object' || Array.isArray(fields)) {
      throw new TypeError(`${itemClass.name}: the override is an object { weight, price, max }`);
    }
    const stats = {};
    for (const [field, value] of Object.entries(fields)) {
      if (FIELDS.has(field) || value === null || value === undefined) {
        continue;
      }
      const stat = statOf(field, itemClass.category);
      if (!stat) {
        const allowed = ['weight', 'price', 'max', ...statNames(itemClass.category)].join(', ');
        throw new TypeError(`${itemClass.name}: ${field} cannot be overridden (${allowed})`);
      }
      const valid = stat.whole ? Number.isInteger(value) : Number.isFinite(value);
      if (typeof value !== 'number' || !valid || value < stat.min || value > stat.max) {
        throw new TypeError(
          `${itemClass.name}: ${field} is a ${stat.whole ? 'whole ' : ''}number ${stat.min} to ${
            stat.max
          }`,
        );
      }
      stats[field] = value;
    }
    const weight = fields.weight ?? null;
    const price = fields.price ?? null;
    const max = fields.max ?? null;
    if (weight === null && price === null && max === null && Object.keys(stats).length === 0) {
      throw new TypeError(`${itemClass.name}: the override changes nothing`);
    }
    if (weight !== null) {
      if (typeof weight !== 'number' || !Number.isFinite(weight)) {
        throw new TypeError(`${itemClass.name}: weight is a number`);
      }
      if (weight < 0 || weight > MAX_WEIGHT) {
        throw new TypeError(`${itemClass.name}: weight is 0 to ${MAX_WEIGHT}`);
      }
    }
    if (price !== null) {
      if (!Number.isInteger(price) || price < 0 || price > MAX_PRICE) {
        throw new TypeError(`${itemClass.name}: price is a whole number 0 to ${MAX_PRICE}`);
      }
    }
    if (max !== null) {
      if (!Number.isInteger(max) || max < 1 || max > MAX_LIMIT) {
        throw new TypeError(`${itemClass.name}: max is a whole number 1 to ${MAX_LIMIT}`);
      }
    }
    return { itemClass, weight, price, max, stats };
  }

  // Reads the server's item config, when there is one. How many entries apply.
  load(root) {
    const file = path.join(root, CONFIG_FILE);
    if (!fs.existsSync(file)) {
      return 0;
    }
    let config;
    try {
      config = JSON.parse(fs.readFileSync(file, 'utf8'));
    } catch (error) {
      this.host.bridge.log('host', `${CONFIG_FILE}: ${error.message}`);
      return 0;
    }
    if (!config || typeof config !== 'object' || Array.isArray(config)) {
      this.host.bridge.log('host', `${CONFIG_FILE}: an object of item -> { weight, price, max }`);
      return 0;
    }
    for (const [item, fields] of Object.entries(config)) {
      try {
        this.put(this.check(item, fields));
      } catch (error) {
        this.host.bridge.log('host', `${CONFIG_FILE}: ${error.message}`);
      }
    }
    this.host.bridge.log('host', `${CONFIG_FILE}: ${this.overrides.size} items overridden`);
    return this.overrides.size;
  }

  put(entry) {
    if (!this.overrides.has(entry.itemClass.id) && this.overrides.size >= MAX_OVERRIDES) {
      throw new RangeError(`at most ${MAX_OVERRIDES} items are overridden`);
    }
    this.overrides.set(entry.itemClass.id, entry);
  }

  // The weight, price and limit (null: none) a class has on this server, and the stats the server
  // overrides ({} for none: the game's own stay, which the server does not read).
  effectiveOf(itemClass) {
    const entry = this.overrides.get(itemClass.id);
    return {
      weight: entry?.weight ?? itemClass.weight,
      price: entry?.price ?? itemClass.price,
      max: entry?.max ?? null,
      stats: Object.freeze({ ...(entry?.stats ?? {}) }),
    };
  }

  // catalog.foodOf with the server's food stats: what eating or drinking the class does by itself.
  foodOf(itemClass) {
    const food = foodOf(itemClass);
    const stats = this.overrides.get(itemClass.id)?.stats;
    if (!food || !stats) {
      return food;
    }
    return Object.freeze({
      kind: food.kind,
      baseEffects: Object.freeze({
        ...food.baseEffects,
        health: stats.health ?? food.baseEffects.health,
        energy: stats.refresh ?? food.baseEffects.energy,
        nourishment: stats.nutrition ?? food.baseEffects.nourishment,
        alcohol: stats.alcohol ?? food.baseEffects.alcohol,
      }),
    });
  }

  // The most units of a class a player may hold (their own limit before the server's), or null.
  maxOf(itemClass, player) {
    const own = player ? this.playerLimits.get(player.id)?.get(itemClass.id) : undefined;
    if (own !== undefined) {
      return own;
    }
    return this.overrides.get(itemClass.id)?.max ?? null;
  }

  // Player.setItemLimit: the most units of an item this player may hold, over the server's
  // limit; null clears the player's own (the server's stands again). The limit that applies now.
  setPlayerLimit(player, item, max) {
    const itemClass = lookupItemClass(item);
    if (!itemClass) {
      throw new TypeError(`Unknown item: ${String(item)}`);
    }
    if (max !== null && (!Number.isInteger(max) || max < 1 || max > MAX_LIMIT)) {
      throw new TypeError(
        `${itemClass.name}: a limit is a whole number 1 to ${MAX_LIMIT}, or null`,
      );
    }
    let limits = this.playerLimits.get(player.id);
    if (max === null) {
      limits?.delete(itemClass.id);
    } else {
      if (!limits) {
        limits = new Map();
        this.playerLimits.set(player.id, limits);
      }
      limits.set(itemClass.id, max);
    }
    if (player.ready) {
      this.sendTo(player);
    }
    return this.maxOf(itemClass, player);
  }

  // Player.itemLimit: the limit that applies to this player for an item, or null.
  playerLimitOf(player, item) {
    const itemClass = lookupItemClass(item);
    if (!itemClass) {
      throw new TypeError(`Unknown item: ${String(item)}`);
    }
    return this.maxOf(itemClass, player);
  }

  // A player who left: their own limits and share go with them.
  left(player) {
    this.playerLimits.delete(player.id);
    this.playerEquippedPercent.delete(player.id);
  }

  // A player would take more of a class than its limit: the notice, once in a while per class.
  limitReached(player, itemClass, max) {
    const key = `${player.id}:${itemClass.id}`;
    const now = Date.now();
    if (now - (this.told.get(key) ?? -Infinity) < NOTICE_QUIET_MS) {
      return;
    }
    if (this.told.size >= 4096) {
      this.told.clear();
    }
    this.told.set(key, now);
    const text = this.host.locales.text(player, LIMIT_REACHED, { max, item: itemClass.name });
    this.notice(player, text);
  }

  // A line among that player's game notifications, or `center` in the middle of their screen.
  notice(player, text, { center = false } = {}) {
    const order = { op: 'itemLimitNotice', id: player.id, text: fitted(text, MAX_NOTICE_BYTES) };
    if (center) {
      order.center = true;
    }
    this.host.bridge.send(order);
  }

  // Items.effective: { id, name, weight, price, max, stats, overridden } of an item, or null.
  effective(item) {
    const itemClass = lookupItemClass(item);
    if (!itemClass) {
      return null;
    }
    return Object.freeze({
      id: itemClass.id,
      name: itemClass.name,
      ...this.effectiveOf(itemClass),
      overridden: this.overrides.has(itemClass.id),
    });
  }

  // Items.override: changes an item for every player; its effective values.
  override(item, fields) {
    this.put(this.check(item, fields));
    this.send(0);
    return this.effective(item);
  }

  // Inventory.overrideStats: the combat numbers of a class for every player, a new
  // set replacing the previous one whole; null (or {}) gives the authored numbers back. The other
  // fields of the class's override (weight, price, max, durability, ...) stay. Throws for an
  // unknown class, a class with no combat numbers, a name its kind lacks or a value out of range.
  overrideStats(item, stats) {
    const itemClass = lookupItemClass(item);
    if (!itemClass) {
      throw new TypeError(`unknown item ${JSON.stringify(item)}`);
    }
    const names = COMBAT_STATS[itemClass.category];
    if (!names) {
      throw new TypeError(`${itemClass.name} carries no combat numbers`);
    }
    if (stats !== null && (typeof stats !== 'object' || Array.isArray(stats))) {
      throw new TypeError(`${itemClass.name}: the stats are an object of numbers, or null`);
    }
    const fields = {};
    for (const [name, value] of Object.entries(stats ?? {})) {
      if (name === 'limbResistance') {
        throw new TypeError(`${itemClass.name}: limbResistance is not supported yet`);
      }
      if (!names.includes(name)) {
        throw new TypeError(`${itemClass.name}: ${name} is not one of ${names.join(', ')}`);
      }
      fields[STAT_NAMES[name] ?? name] = value;
    }
    const entry = this.overrides.get(itemClass.id);
    const combat = new Set(names.map((name) => STAT_NAMES[name] ?? name));
    const kept = Object.fromEntries(
      Object.entries(entry?.stats ?? {}).filter(([name]) => !combat.has(name)),
    );
    const others = {
      weight: entry?.weight ?? undefined,
      price: entry?.price ?? undefined,
      max: entry?.max ?? undefined,
      ...kept,
    };
    const next = { ...others, ...fields };
    const changes = Object.values(next).some((value) => value !== undefined && value !== null);
    if (!changes) {
      this.overrides.delete(itemClass.id);
    } else {
      this.put(this.check(itemClass.id, next));
    }
    this.send(0);
    return true;
  }

  // Inventory.itemStats: the combat numbers the server set on a class, by overrideStats's
  // names, or null if no script changed it. An unknown class throws.
  itemStats(item) {
    const itemClass = lookupItemClass(item);
    if (!itemClass) {
      throw new TypeError(`unknown item ${JSON.stringify(item)}`);
    }
    const stats = this.overrides.get(itemClass.id)?.stats ?? {};
    const found = {};
    for (const name of COMBAT_STATS[itemClass.category] ?? []) {
      const value = stats[STAT_NAMES[name] ?? name];
      if (value !== undefined) {
        found[name] = value;
      }
    }
    return Object.keys(found).length > 0 ? found : null;
  }

  // Items.clearOverride: the game's own values again. Whether there was an override.
  clearOverride(item) {
    const itemClass = lookupItemClass(item);
    if (!itemClass || !this.overrides.delete(itemClass.id)) {
      return false;
    }
    this.send(0);
    return true;
  }

  // The whole set to one player, or everyone in the world (0): each in their language.
  send(to) {
    if (to !== 0) {
      const player = this.host.players.get(to);
      if (player) {
        this.sendTo(player);
      }
      return;
    }
    for (const player of this.host.players.values()) {
      if (player.ready) {
        this.sendTo(player);
      }
    }
  }

  sendTo(player) {
    const tooltip = this.host.locales.text(player, MAX_TOOLTIP);
    this.host.bridge.send({
      op: 'itemOverrides',
      id: player.id,
      items: this.entriesFor(player).map(({ itemClass, weight, price, max, stats }) => [
        itemClass.id,
        weight,
        price,
        max,
        Object.entries(stats).map(([name, value]) => [statOf(name, itemClass.category).id, value]),
      ]),
      maxTooltip: fitted(tooltip, MAX_TOOLTIP_BYTES),
      equippedWeightPercent: this.equippedPercentOf(player),
    });
  }

  // The overrides one player's game gets: the server's, with the player's own limits over them.
  entriesFor(player) {
    const entries = new Map(this.overrides);
    for (const [id, max] of this.playerLimits.get(player.id) ?? []) {
      const entry = entries.get(id);
      entries.set(
        id,
        entry
          ? { ...entry, max }
          : { itemClass: lookupItemClass(id), weight: null, price: null, max, stats: {} },
      );
    }
    return [...entries.values()];
  }

  // A player who came into the world; their game left every class as its own when they left.
  arrived(player) {
    if (
      this.overrides.size > 0 ||
      this.playerLimits.has(player.id) ||
      this.equippedPercentOf(player) !== 100
    ) {
      this.sendTo(player);
    }
  }

  api() {
    return {
      override: (item, fields) => this.override(item, fields),
      clearOverride: (item) => this.clearOverride(item),
      effective: (item) => this.effective(item),
      overrides: () => [...this.overrides.keys()].map((id) => this.effective(id)),
      // how much of what players wear counts toward their load, in percent.
      setEquippedWeightPercent: (percent) => this.setEquippedPercent(percent),
      equippedWeightPercent: () => this.equippedPercent,
    };
  }
}

module.exports = { ItemOverrides };
