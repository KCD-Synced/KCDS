'use strict';

// Buffs a resource defines itself (the server API's Buffs.register): a name, the
// status icon of one of the game's own buffs, a duration, stat changes and an optional tick.
//
// The game's buff tables cannot take a new entry, so these never reach the game's own buff
// manager. The server keeps them: a player carries the instance from addBuff until it ends, and
// that player's client shows its icon and timer in the status list (the `customBuff` ops below).
//
// Time: every instance has one due moment (its next tick or its end). All of them sit in one
// schedule sorted by that moment, and a single unref'd timer waits for the head of it, so the
// cost does not grow with the number of buffs or players.
//
// Ops sent to the player's client (bridge op `customBuff`):
//   { kind: 'show', id, buff, icon, iconId, label, duration, remaining, params }  show or restart
//        the entry: `icon` is the name of the game buff whose status icon it borrows, `iconId` that
//        buff's GUID text (the client clones its definition), `duration` the full run in seconds
//        (-1 none), `remaining` what is left (-1 none) for the timer display, `params` the game's
//        buff_params text of its native modifiers ('' for none).
//   { kind: 'hide', id, buff }                                    take the entry off the list.
//
// The id form: Buffs.register(id, definition) with a display `name`, a
// `description`, a `visual` (the game buff whose icon it borrows), `duration`, `tickInterval`,
// `stackable` with `maxStacks` and `refresh`, and `modifiers` (stat points held while an instance
// lasts and taken back when it ends). It returns a GUID that every player buff method takes; the
// same registration again returns it again, another one under the same id throws. Its instances
// raise playerCustomBuffApply, playerCustomBuffTick and playerCustomBuffRemove. A stackable buff
// keeps up to maxStacks instances, each with its own timer and modifiers; at the limit (1 when it
// does not stack) a refreshing one replaces its oldest instance, another one ignores the new one.
// A modifier adds points ('add', or a plain number), multiplies the stat ('multiply') or sets it
// ('set'): an instance works out its change from the player's reported value when it starts,
// holds that change and takes exactly it back when it ends. healthRegeneration, hungerDrain and energyDrain
// change health, hunger or exhaust every second while an instance lasts (one shared timer). The native names
// (maxStamina, carryCapacity, staminaRegeneration, charisma, strength, agility, vitality, speech)
// are the game's own buff modifiers: they travel in the show op as the game's buff_params text
// ("mst*1.2,cap+50") and the player's own game applies them while the buff is on its list, once
// however many stacks there are.

const crypto = require('node:crypto');
const { lookupBuff } = require('./catalog.cjs');
const { PlayerStat, statChangeRequest } = require('../stats/player_stats.cjs');

const NAME_PATTERN = /^[A-Za-z0-9:_.-]{1,64}$/;
const MAX_DEFINITIONS = 256;
const MAX_LABEL_BYTES = 128;
const MAX_DURATION = 7 * 24 * 3600;
const MIN_TICK_SECONDS = 1;
// setTimeout takes a 32-bit delay.
const MAX_TIMER_MS = 2 ** 31 - 1;
// The id form's limits.
const ID_PATTERN = /^[A-Za-z0-9:_.-]{1,128}$/;
const MAX_NAME_BYTES = 256;
const MAX_DESCRIPTION_BYTES = 4096;
const MIN_TICK_INTERVAL = 0.1;
const MAX_TICK_INTERVAL = 86400;
const MAX_STACKS = 16;
const DEFINITION_KEYS = new Set([
  'name',
  'description',
  'visual',
  'duration',
  'tickInterval',
  'stackable',
  'maxStacks',
  'refresh',
  'modifiers',
]);

function invalid(what) {
  return new TypeError(`Invalid custom buff: ${what}`);
}

// The stat changes of a definition, as { stat: delta }; each one a change addStat takes.
function statChangesOf(value, what) {
  if (value === undefined) {
    return [];
  }
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw invalid(`${what} must be an object of stat changes`);
  }
  return Object.entries(value).map(([stat, delta]) => {
    statChangeRequest(stat, delta);
    return [stat, delta];
  });
}

// Per-second rates while an instance lasts: the stat each one changes and the sign
// of its value (a drain takes the stat down by the value, an energy drain tires the player).
const RATES = Object.freeze({
  healthRegeneration: { stat: 'health', sign: 1 },
  hungerDrain: { stat: 'hunger', sign: -1 },
  energyDrain: { stat: 'exhaust', sign: 1 },
});
// The stats a custom buff's modifiers may name (Buffs.modifiers): the player's writable
// states, whose change an instance holds while it lasts, and the per-second rates.
// The native modifiers and the game's buff_params codes for them (its buff table: well_rested
// mst*1.2, test_huge_inventory cap+1000, closed_visor_debuff srg*0.95, attributes by name).
// The progression tracks are the game's own codes too (the buff table writes weapon_sword+3).
const TRACKS = [
  'stealth',
  'horse_riding',
  'fencing',
  'thievery',
  'alchemy',
  'craftsmanship',
  'drinking',
  'survival',
  'weapon_sword',
  'heavy_weapons',
  'marksmanship',
  'weapon_large',
  'weapon_unarmed',
  'scholarship',
  'houndmaster',
];
const NATIVE = Object.freeze({
  maxStamina: 'mst',
  carryCapacity: 'cap',
  staminaRegeneration: 'srg',
  charisma: 'charisma',
  strength: 'strength',
  agility: 'agility',
  vitality: 'vitality',
  speech: 'speech',
  // Run speed (the buff table: an injured leg is Run-1; live, Run+0.3 raised normalizedRunSpeed
  // 0.87 -> 1.03).
  runSpeed: 'Run',
  // Attack speed: the game's arm wound is asp%0.8, a leg wound asp-0.5; 'multiply'
  // 0.8 makes every swing take 80 % of its speed.
  attackSpeed: 'asp',
  // How loud, how visible and how conspicuous the player is to NPCs: the game's
  // crouch is noi*0.5, sprinting noi*1.5, its Slim fit perk noi*0.8 (the noise of worn armour).
  noise: 'noi',
  visibility: 'vib',
  conspicuousness: 'con',
  // How fast walking dirties the worn clothes: the game's
  // DerivStat_ClothDirtyingSpeedKm, buff code cds; its quest_noDirt buff stops it with cds=0.
  clothDirtying: 'cds',
  ...Object.fromEntries(TRACKS.map((track) => [track, track])),
});
// The game's own switches, without a value: `noSprint: true` keeps the player from
// sprinting while the buff lasts, as the game's leg wounds do (LimitSprint).
const FLAGS = Object.freeze({
  noSprint: 'LimitSprint',
});
const MODIFIERS = Object.freeze([
  ...Object.values(PlayerStat),
  ...Object.keys(RATES),
  ...Object.keys(NATIVE),
  ...Object.keys(FLAGS),
]);
const OPERATORS = Object.freeze({ add: '+', multiply: '*', set: '=' });
// Player.setGrimeRate's hidden buff: its name and the game buff it copies, and the highest rate.
const GRIME_BUFF = 'kcds.grimeRate';
const GRIME_ICON = 'quest_noDirt';
const GRIME_RATE_MAX = 100;
const OPERATIONS = new Set(['add', 'multiply', 'set']);
// How often the rates apply.
const REGENERATION_MS = 1000;

// The id form's modifiers as { stat, operation, value }: a number adds it, or
// { operation: 'add' | 'multiply' | 'set', value }.
function modifiersOf(value) {
  if (value === undefined) {
    return [];
  }
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw invalid('modifiers must be an object');
  }
  const modifiers = [];
  for (const [stat, modifier] of Object.entries(value)) {
    if (Object.hasOwn(FLAGS, stat)) {
      if (typeof modifier !== 'boolean') {
        throw invalid(`modifier ${stat} is true or false`);
      }
      if (modifier) {
        modifiers.push({ stat, operation: 'flag', value: 1 });
      }
      continue;
    }
    modifiers.push(numberModifierOf(stat, modifier));
  }
  return modifiers;
}

// One modifier with a number, as { stat, operation, value }.
function numberModifierOf(stat, modifier) {
  let operation = 'add';
  let points = modifier;
  if (modifier !== null && typeof modifier === 'object') {
    if (!OPERATIONS.has(modifier.operation)) {
      throw invalid(`modifier ${stat}: the operation is 'add', 'multiply' or 'set'`);
    }
    operation = modifier.operation;
    points = modifier.value;
  }
  if (typeof points !== 'number' || !Number.isFinite(points)) {
    throw invalid(`modifier ${stat} must be a number`);
  }
  if (Object.hasOwn(RATES, stat)) {
    if (operation !== 'add' || points < 0) {
      throw invalid(`${stat} takes a rate per second of 0 or more, with the add operation`);
    }
    statChangeRequest(RATES[stat].stat, points);
  } else if (Object.hasOwn(NATIVE, stat)) {
    if (operation !== 'add' && points < 0) {
      throw invalid(`modifier ${stat}: ${operation} takes a value of 0 or more`);
    }
  } else if (operation === 'add') {
    statChangeRequest(stat, points);
  } else {
    statChangeRequest(stat, 0);
    if (points < 0) {
      throw invalid(`modifier ${stat}: ${operation} takes a value of 0 or more`);
    }
  }
  return { stat, operation, value: points };
}

// The per-second changes of the rate modifiers as [stat, change], one per stat.
function ratesOf(modifiers) {
  const sums = new Map();
  for (const { stat, value } of modifiers) {
    if (Object.hasOwn(RATES, stat)) {
      const { stat: target, sign } = RATES[stat];
      sums.set(target, (sums.get(target) ?? 0) + sign * value);
    }
  }
  return [...sums];
}

// The game's buff_params text of the native modifiers, '' for none.
function nativeParamsOf(modifiers) {
  return modifiers
    .filter(
      (modifier) => Object.hasOwn(NATIVE, modifier.stat) || Object.hasOwn(FLAGS, modifier.stat),
    )
    .map(({ stat, operation, value }) => {
      if (operation === 'flag') {
        return FLAGS[stat];
      }
      const number = Number(value.toFixed(4));
      if (operation === 'add' && number < 0) {
        return `${NATIVE[stat]}-${-number}`;
      }
      return `${NATIVE[stat]}${OPERATORS[operation]}${number}`;
    })
    .join(',');
}

// A GUID that stands for a registered id, the same on every start.
function guidOf(id) {
  const hex = crypto.createHash('sha1').update(`kcds-custom-buff:${id}`).digest('hex');
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(
    16,
    20,
  )}-${hex.slice(20, 32)}`;
}

function textOf(value, field, maxBytes, optional) {
  if (value === undefined && optional) {
    return null;
  }
  if (typeof value !== 'string' || (!optional && !value) || Buffer.byteLength(value) > maxBytes) {
    throw invalid(`${field} must be a text of up to ${maxBytes} bytes`);
  }
  return value;
}

class CustomBuffs {
  constructor(host) {
    this.host = host;
    // name -> definition (frozen handle plus the owner and the rules)
    this.definitions = new Map();
    // GUID (lower case) -> definition
    this.byGuid = new Map();
    // handle -> definition
    this.byHandle = new WeakMap();
    // player id -> (name -> its instances, oldest first)
    this.active = new Map();
    // The one schedule: instances sorted by `due`, earliest first.
    this.schedule = [];
    this.timer = null;
    // player id -> the rate Player.setGrimeRate gave them, when not 1
    this.grimeRates = new Map();
  }

  api() {
    return (idOrDefinition, definition = undefined) =>
      definition === undefined
        ? this.register(idOrDefinition)
        : this.registerById(idOrDefinition, definition);
  }

  // The id form: Buffs.register(id, definition) -> the buff's GUID.
  registerById(id, definition) {
    if (typeof id !== 'string' || !ID_PATTERN.test(id)) {
      throw invalid('id must be 1 to 128 letters, digits, : _ . -');
    }
    if (lookupBuff(id)) {
      throw invalid(`${id} is the name of a game buff`);
    }
    if (definition === null || typeof definition !== 'object' || Array.isArray(definition)) {
      throw invalid('the definition must be an object');
    }
    for (const key of Object.keys(definition)) {
      if (!DEFINITION_KEYS.has(key)) {
        throw invalid(`unknown key ${key}`);
      }
    }
    const displayName = textOf(definition.name, 'name', MAX_NAME_BYTES, false);
    const description = textOf(definition.description, 'description', MAX_DESCRIPTION_BYTES, true);
    const visual =
      typeof definition.visual === 'string' ? lookupBuff(definition.visual) : undefined;
    if (!visual) {
      throw invalid('visual must be the name or GUID of a game buff');
    }
    const duration = definition.duration ?? -1;
    if (
      duration !== -1 &&
      !(Number.isFinite(duration) && duration > 0 && duration <= MAX_DURATION)
    ) {
      throw invalid('duration must be -1 or seconds above 0, up to seven days');
    }
    const tickInterval = definition.tickInterval ?? 0;
    if (
      tickInterval !== 0 &&
      !(
        Number.isFinite(tickInterval) &&
        tickInterval >= MIN_TICK_INTERVAL &&
        tickInterval <= MAX_TICK_INTERVAL
      )
    ) {
      throw invalid(`tickInterval must be 0 or ${MIN_TICK_INTERVAL} to ${MAX_TICK_INTERVAL}`);
    }
    for (const flag of ['stackable', 'refresh']) {
      if (definition[flag] !== undefined && typeof definition[flag] !== 'boolean') {
        throw invalid(`${flag} must be true or false`);
      }
    }
    const maxStacks = definition.maxStacks ?? 1;
    if (!Number.isInteger(maxStacks) || maxStacks < 1 || maxStacks > MAX_STACKS) {
      throw invalid(`maxStacks must be a whole number from 1 to ${MAX_STACKS}`);
    }
    const modifiers = modifiersOf(definition.modifiers);
    const canonical = JSON.stringify([
      displayName,
      description,
      visual.id,
      duration,
      tickInterval,
      definition.stackable === true,
      maxStacks,
      definition.refresh !== false,
      modifiers,
    ]);
    const known = this.definitions.get(id);
    if (known) {
      if (known.canonical === canonical) {
        return known.guid;
      }
      throw new Error(`Custom buff ${id} is already registered with another definition`);
    }
    if (this.definitions.size >= MAX_DEFINITIONS) {
      throw invalid('too many custom buffs');
    }
    const owner = this.host.context.getStore();
    const guid = guidOf(id);
    const handle = Object.freeze({
      name: id,
      icon: visual.name,
      label: displayName,
      duration,
      tickSeconds: tickInterval === 0 ? null : tickInterval,
    });
    const def = {
      handle,
      owner,
      resource: owner?.name,
      iconBuff: visual,
      stats: [],
      modifiers,
      params: nativeParamsOf(modifiers),
      rates: ratesOf(modifiers),
      tickStats: [],
      onTick: undefined,
      reference: true,
      held: true,
      guid,
      canonical,
      description,
      stackable: definition.stackable === true,
      limit: definition.stackable === true ? maxStacks : 1,
      refresh: definition.refresh !== false,
    };
    this.definitions.set(id, def);
    this.byGuid.set(guid, def);
    this.byHandle.set(handle, def);
    return guid;
  }

  // BuffInfo of a definition.
  infoOf(def) {
    return {
      name: def.handle.name,
      class: 'custom',
      aiTag: null,
      duration: def.handle.duration,
      custom: true,
      displayName: def.handle.label,
      description: def.description ?? null,
    };
  }

  // Buffs.find of a custom buff by its id or GUID, or null.
  find(query) {
    const def = typeof query === 'string' ? this.resolve(query) : undefined;
    return def ? this.infoOf(def) : null;
  }

  register(definition) {
    if (definition === null || typeof definition !== 'object') {
      throw invalid('the definition must be an object');
    }
    const { name, icon, label, duration, effects, tickSeconds, onTick } = definition;
    if (typeof name !== 'string' || !NAME_PATTERN.test(name)) {
      throw invalid('name must be 1 to 64 letters, digits, : _ . -');
    }
    if (lookupBuff(name)) {
      throw invalid(`${name} is the name of a game buff`);
    }
    if (this.definitions.has(name)) {
      throw invalid(`${name} is already registered`);
    }
    if (this.definitions.size >= MAX_DEFINITIONS) {
      throw invalid('too many custom buffs');
    }
    // The status icon is the one of a game buff, named as Buffs.find spells it.
    const iconBuff = typeof icon === 'string' ? lookupBuff(icon) : undefined;
    if (!iconBuff) {
      throw invalid('icon must be the name of a game buff');
    }
    if (
      label !== undefined &&
      (typeof label !== 'string' || !label || Buffer.byteLength(label) > MAX_LABEL_BYTES)
    ) {
      throw invalid('label must be a short text');
    }
    const endless = duration === -1;
    if (!endless && !(Number.isFinite(duration) && duration > 0 && duration <= MAX_DURATION)) {
      throw invalid('duration must be -1 or seconds above 0');
    }
    const fx = effects ?? {};
    if (typeof fx !== 'object') {
      throw invalid('effects must be an object');
    }
    const stats = statChangesOf(fx.stats, 'effects.stats');
    const tickStats = statChangesOf(fx.tickStats, 'effects.tickStats');
    const ticks = tickSeconds !== undefined;
    if (ticks && !(Number.isFinite(tickSeconds) && tickSeconds >= MIN_TICK_SECONDS)) {
      throw invalid(`tickSeconds must be ${MIN_TICK_SECONDS} or more`);
    }
    if (onTick !== undefined && typeof onTick !== 'function') {
      throw invalid('onTick must be a function');
    }
    if (ticks && !onTick && !tickStats.length) {
      throw invalid('tickSeconds needs an onTick or effects.tickStats');
    }
    if (!ticks && (onTick || tickStats.length)) {
      throw invalid('onTick and effects.tickStats need tickSeconds');
    }
    const owner = this.host.context.getStore();
    const handle = Object.freeze({
      name,
      icon,
      label: label ?? name,
      duration,
      tickSeconds: ticks ? tickSeconds : null,
    });
    this.definitions.set(name, {
      handle,
      owner,
      resource: owner?.name,
      iconBuff,
      stats,
      tickStats,
      onTick,
    });
    this.byHandle.set(handle, this.definitions.get(name));
    return handle;
  }

  // The definition a handle or a name stands for, or undefined (a game buff, an unknown name).
  resolve(buff) {
    if (typeof buff === 'string') {
      return this.definitions.get(buff) ?? this.byGuid.get(buff.toLowerCase());
    }
    return buff !== null && typeof buff === 'object' ? this.byHandle.get(buff) : undefined;
  }

  // Puts the buff on the player: the entry shows on their client, the effects apply, the
  // timer starts. On a player that has it, the timer restarts and the effects apply again (no
  // events). False for a player the host cannot reach.
  add(player, def) {
    if (!this.host.canOrderPlayer(player)) {
      return false;
    }
    const now = this.host.buffClock();
    const mine = this.active.get(player.id) ?? new Map();
    this.active.set(player.id, mine);
    const list = mine.get(def.handle.name) ?? [];
    const again = list.length > 0;
    if (def.reference) {
      // At the limit: the oldest makes room, or the new one is ignored.
      if (list.length >= def.limit) {
        if (!def.refresh) {
          return false;
        }
        const oldest = list[0];
        this.cancel(oldest);
        if (def.reference) {
          this.host.emit('playerCustomBuffRemove', player, this.infoOf(def), 'refreshed');
        }
      }
    } else if (again) {
      this.cancel(list[0]);
    }
    const { duration, tickSeconds } = def.handle;
    const instance = {
      def,
      player,
      since: now,
      endsAt: duration === -1 ? Infinity : now + duration,
      nextTick: tickSeconds === null ? Infinity : now + tickSeconds,
      due: 0,
      dead: false,
    };
    instance.due = Math.min(instance.endsAt, instance.nextTick);
    const kept = mine.get(def.handle.name) ?? [];
    kept.push(instance);
    mine.set(def.handle.name, kept);
    if (instance.due !== Infinity) {
      this.insert(instance);
    }
    const changes = def.modifiers ? this.changesOf(player, def.modifiers) : def.stats;
    instance.held = def.held ? changes : [];
    this.applyStats(player, changes);
    if (def.rates?.length) {
      this.regenerate(instance);
    }
    this.bridge().send({
      op: 'customBuff',
      kind: 'show',
      id: player.id,
      buff: def.handle.name,
      icon: def.iconBuff.name,
      iconId: def.iconBuff.id,
      label: def.handle.label,
      duration,
      remaining: duration,
      params: def.params ?? '',
    });
    if (!again) {
      this.host.emit('playerBuffAdded', player, def.handle.name, 'server');
    }
    if (def.reference) {
      this.host.emit('playerCustomBuffApply', player, this.infoOf(def));
    }
    return true;
  }

  // Takes every instance of the buff off the player. `reason`: 'server' (removeBuff). Returns
  // whether the player carried it.
  end(player, name, reason) {
    const list = this.active.get(player.id)?.get(name);
    if (!list || !list.length) {
      return false;
    }
    for (const instance of [...list]) {
      this.cancel(instance);
      if (instance.def.reference) {
        this.host.emit('playerCustomBuffRemove', player, this.infoOf(instance.def), reason);
      }
    }
    this.bridge().send({ op: 'customBuff', kind: 'hide', id: player.id, buff: name });
    this.host.emit('playerBuffRemoved', player, name, reason);
    return true;
  }

  // One instance ran out: the entry goes when it was the last one.
  expire(instance) {
    const { player, def } = instance;
    const name = def.handle.name;
    this.cancel(instance);
    if (def.reference) {
      this.host.emit('playerCustomBuffRemove', player, this.infoOf(def), 'expired');
    }
    if (this.active.get(player.id)?.get(name)?.length) {
      return;
    }
    this.bridge().send({ op: 'customBuff', kind: 'hide', id: player.id, buff: name });
    this.host.emit('playerBuffRemoved', player, name, 'expired');
  }

  // Player.setGrimeRate: scales how fast walking dirties the player's worn
  // clothes, 1 the game's own rate. It is a copy of the game's quest_noDirt buff, which has no
  // icon and no status entry, holding cds*rate; rate 1 takes it off.
  setGrimeRate(player, rate) {
    if (typeof rate !== 'number' || !Number.isFinite(rate) || rate < 0 || rate > GRIME_RATE_MAX) {
      throw new RangeError(`A grime rate is a number from 0 to ${GRIME_RATE_MAX}`);
    }
    if (!this.host.canOrderPlayer(player)) {
      return false;
    }
    const before = this.grimeRates.get(player.id) ?? 1;
    if (rate === before) {
      return true;
    }
    if (rate === 1) {
      this.grimeRates.delete(player.id);
      this.bridge().send({ op: 'customBuff', kind: 'hide', id: player.id, buff: GRIME_BUFF });
      return true;
    }
    this.grimeRates.set(player.id, rate);
    const hidden = lookupBuff(GRIME_ICON);
    this.bridge().send({
      op: 'customBuff',
      kind: 'show',
      id: player.id,
      buff: GRIME_BUFF,
      icon: hidden.name,
      iconId: hidden.id,
      label: GRIME_BUFF,
      duration: -1,
      remaining: -1,
      params: `cds*${Number(rate.toFixed(4))}`,
    });
    return true;
  }

  // The rate setGrimeRate gave the player, 1 by default.
  grimeRateOf(player) {
    return this.grimeRates.get(player.id) ?? 1;
  }

  has(player, name) {
    return !!this.active.get(player.id)?.get(name)?.length;
  }

  // Player.buffs entries for the custom buffs the player carries, one for each instance.
  statesOf(player, now) {
    const states = [];
    for (const list of this.active.get(player.id)?.values() ?? []) {
      for (const instance of list) {
        states.push({
          ...this.infoOf(instance.def),
          since: Math.max(0, Math.round((now - instance.since) * 1000) / 1000),
          source: 'server',
        });
      }
    }
    return states;
  }

  // The player's body is gone: its custom buffs go with it, silently as the game's do; the
  // reference form's instances raise their removal ('disconnect').
  playerLeft(player) {
    for (const list of this.active.get(player.id)?.values() ?? []) {
      for (const instance of [...list]) {
        this.cancel(instance);
        if (instance.def.reference) {
          this.host.emit('playerCustomBuffRemove', player, this.infoOf(instance.def), 'disconnect');
        }
      }
    }
    this.active.delete(player.id);
    this.grimeRates.delete(player.id);
  }

  // A resource stopped: its definitions go, and so does every instance of them.
  resourceStopped(resource) {
    for (const [name, def] of [...this.definitions]) {
      if (def.resource !== resource) {
        continue;
      }
      this.definitions.delete(name);
      if (def.guid) {
        this.byGuid.delete(def.guid);
      }
      for (const mine of this.active.values()) {
        const list = mine.get(name);
        if (list && list.length) {
          const playerId = list[0].player.id;
          for (const instance of [...list]) {
            this.cancel(instance);
          }
          this.bridge().send({ op: 'customBuff', kind: 'hide', id: playerId, buff: name });
        }
      }
    }
  }

  bridge() {
    return this.host.bridge;
  }

  applyStats(player, changes) {
    for (const [stat, delta] of changes) {
      this.host.addPlayerStat(player, stat, delta);
    }
  }

  // An instance's changes from its modifiers, worked out from the player's reported values now.
  changesOf(player, modifiers) {
    const changes = [];
    for (const { stat, operation, value } of modifiers) {
      if (Object.hasOwn(RATES, stat) || Object.hasOwn(NATIVE, stat) || Object.hasOwn(FLAGS, stat)) {
        continue;
      }
      let delta = value;
      if (operation !== 'add') {
        const current = Number(player.getStat(stat)) || 0;
        delta = operation === 'multiply' ? current * (value - 1) : value - current;
      }
      if (delta !== 0 && Math.fround(delta) !== 0) {
        changes.push([stat, delta]);
      }
    }
    return changes;
  }

  // The rates: the instance changes its stats every second until it ends.
  regenerate(instance) {
    this.regenerating ??= new Set();
    this.regenerating.add(instance);
    if (this.regenerationTimer) {
      return;
    }
    this.regenerationTimer = setInterval(() => {
      for (const each of this.regenerating) {
        for (const [stat, perSecond] of each.def.rates) {
          this.host.addPlayerStat(each.player, stat, perSecond);
        }
      }
    }, REGENERATION_MS);
    this.regenerationTimer.unref?.();
  }

  // The instance leaves the schedule and the player's list; held modifiers are taken back.
  cancel(instance) {
    if (instance.dead) {
      return;
    }
    instance.dead = true;
    const at = this.schedule.indexOf(instance);
    if (at !== -1) {
      this.schedule.splice(at, 1);
    }
    const name = instance.def.handle.name;
    const mine = this.active.get(instance.player.id);
    const list = mine?.get(name);
    if (list) {
      const index = list.indexOf(instance);
      if (index !== -1) {
        list.splice(index, 1);
      }
      if (!list.length) {
        mine.delete(name);
      }
    }
    this.applyStats(
      instance.player,
      (instance.held ?? []).map(([stat, points]) => [stat, -points]),
    );
    if (this.regenerating?.delete(instance) && this.regenerating.size === 0) {
      clearInterval(this.regenerationTimer);
      this.regenerationTimer = null;
    }
    this.arm();
  }

  // Into the schedule at its sorted place (after those due at the same moment).
  insert(instance) {
    let low = 0;
    let high = this.schedule.length;
    while (low < high) {
      const middle = (low + high) >> 1;
      if (this.schedule[middle].due <= instance.due) {
        low = middle + 1;
      } else {
        high = middle;
      }
    }
    this.schedule.splice(low, 0, instance);
    this.arm();
  }

  // The one timer: waits for the head of the schedule, and never keeps the process alive.
  arm() {
    clearTimeout(this.timer);
    this.timer = null;
    const head = this.schedule[0];
    if (!head) {
      return;
    }
    const delay = Math.min(MAX_TIMER_MS, Math.max(0, (head.due - this.host.buffClock()) * 1000));
    this.timer = setTimeout(() => this.process(), Math.ceil(delay));
    this.timer.unref();
  }

  // Ends and ticks everything that is due.
  process() {
    this.timer = null;
    const now = this.host.buffClock();
    while (this.schedule.length && this.schedule[0].due <= now) {
      const instance = this.schedule.shift();
      if (instance.dead) {
        continue;
      }
      if (instance.endsAt <= now) {
        this.expire(instance);
        continue;
      }
      this.tick(instance, now);
    }
    this.arm();
  }

  tick(instance, now) {
    const { def, player } = instance;
    // A tick missed while the host was busy is not made up for.
    const next = instance.nextTick + def.handle.tickSeconds;
    instance.nextTick = next > now ? next : now + def.handle.tickSeconds;
    instance.due = Math.min(instance.endsAt, instance.nextTick);
    this.insert(instance);
    this.applyStats(player, def.tickStats);
    if (def.reference) {
      this.host.emit('playerCustomBuffTick', player, this.infoOf(def));
    }
    if (!def.onTick) {
      return;
    }
    try {
      const result = this.host.context.run(def.owner, () => def.onTick(player, def.handle));
      if (result?.then) {
        result.catch((error) => this.host.bridge.log(def.resource, error.stack ?? String(error)));
      }
    } catch (error) {
      this.host.bridge.log(def.resource, error.stack ?? String(error));
    }
  }
}

module.exports = { CustomBuffs, MODIFIERS };
