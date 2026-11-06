'use strict';

// Custom recipes and custom crafting stations (a KCDS addition): a resource defines
// its own recipes (Crafting.addRecipe: its materials and amounts, its products, how long it
// takes) and puts stations on any prop or place (Crafting.addStation: the game's "press [key] to
// ..." hint through World.addInteraction, the animation the crafting player plays, whether
// progress runs by itself). The server rules them wholly: it checks and takes the materials from
// the player's inventory, runs the progress (by time, or only through Crafting.addProgress and
// setProgress when `autoProgress` is false), grants the products and refunds the materials of a
// craft that ends unfinished. The events are the crafting ones, with kind 'custom':
// craftingStarting (refusable), craftingStarted, craftingCompleting (refusable), craftingCompleted,
// craftingRefunding and craftingEnded. One player at a station at a time; a player who goes
// farther than the station's reach (three times its radius) cancels.
//
// A recipe with `learn: true` (or a `sketch`) is crafted only by the players who know it: the
// SDK teaches it (Crafting.learnRecipe, forgetRecipe, knowsRecipe, learnedRecipes), and a recipe's
// sketch, a usable item type (Items.register, usable), teaches it to the player who uses it in
// their inventory. Each time a player learns one, craftingRecipeLearned(player, { recipe, source })
// with source 'sdk' or 'sketch'. What a player knows lasts while they are on the server; a
// resource keeps it across visits with learnedRecipes and learnRecipe.

const { lookupItemClass } = require('../inventory/catalog.cjs');

const ID_MAX = 64;
const NAME_MAX_BYTES = 128;
const MATERIALS_MAX = 16;
const AMOUNT_MAX = 10000;
const DURATION_MAX = 3600;
const RECIPES_MAX = 1024;
const STATIONS_MAX = 256;
const REACH_FACTOR = 3;
const STATION_OPTIONS = new Set([
  'entity',
  'position',
  'radius',
  'text',
  'key',
  'recipes',
  'animation',
  'autoProgress',
  'dimension',
]);
const RECIPE_OPTIONS = new Set([
  'id',
  'name',
  'ingredients',
  'products',
  'duration',
  'learn',
  'sketch',
]);
const encoder = new TextEncoder();

function idOf(value, what) {
  if (typeof value !== 'string' || !/^[A-Za-z0-9_.:-]{1,64}$/.test(value)) {
    throw new TypeError(`${what} must be 1 to ${ID_MAX} letters, digits or _ . : -`);
  }
  return value;
}

// [{ item, amount }] of item classes the game has.
function unitsOf(list, what) {
  if (!Array.isArray(list) || list.length === 0 || list.length > MATERIALS_MAX) {
    throw new TypeError(`${what} must be a list of 1 to ${MATERIALS_MAX} { item, amount }`);
  }
  return list.map((unit) => {
    const itemClass = unit && typeof unit === 'object' ? lookupItemClass(unit.item) : undefined;
    if (!itemClass) {
      throw new TypeError(`${what}: unknown item ${String(unit?.item)}`);
    }
    const amount = unit.amount ?? 1;
    if (!Number.isInteger(amount) || amount < 1 || amount > AMOUNT_MAX) {
      throw new RangeError(`${what}: amount must be a whole number from 1 to ${AMOUNT_MAX}`);
    }
    return Object.freeze({ item: itemClass.id, amount });
  });
}

function recipeOf(options) {
  if (!options || typeof options !== 'object') {
    throw new TypeError('Crafting.addRecipe takes an options object');
  }
  for (const key of Object.keys(options)) {
    if (!RECIPE_OPTIONS.has(key)) {
      throw new TypeError(`unknown Crafting.addRecipe option ${key}`);
    }
  }
  const name = options.name ?? options.id;
  if (typeof name !== 'string' || encoder.encode(name).length > NAME_MAX_BYTES) {
    throw new TypeError(`Crafting.addRecipe name must be text of at most ${NAME_MAX_BYTES} bytes`);
  }
  const duration = options.duration ?? 5;
  if (typeof duration !== 'number' || !(duration > 0) || duration > DURATION_MAX) {
    throw new RangeError(`Crafting.addRecipe duration must be seconds from 0 to ${DURATION_MAX}`);
  }
  if (options.learn !== undefined && typeof options.learn !== 'boolean') {
    throw new TypeError('Crafting.addRecipe learn must be true or false');
  }
  let sketch = null;
  if (options.sketch !== undefined) {
    const sketchClass = lookupItemClass(options.sketch);
    if (!sketchClass || !sketchClass.custom || !sketchClass.usable) {
      throw new TypeError(
        'Crafting.addRecipe sketch must be a usable item type (Items.register with usable: true)',
      );
    }
    sketch = sketchClass.id;
  }
  return Object.freeze({
    id: idOf(options.id, 'Crafting.addRecipe id'),
    name,
    ingredients: Object.freeze(unitsOf(options.ingredients, 'Crafting.addRecipe ingredients')),
    products: Object.freeze(unitsOf(options.products, 'Crafting.addRecipe products')),
    duration,
    learn: options.learn === true || sketch !== null,
    sketch,
  });
}

function recipeInfo(recipe) {
  return {
    id: recipe.id,
    name: recipe.name,
    ingredients: recipe.ingredients.map((unit) => ({ ...unit })),
    products: recipe.products.map((unit) => ({ ...unit })),
    duration: recipe.duration,
    learn: recipe.learn,
    sketch: recipe.sketch,
  };
}

class CustomCrafting {
  // `host` gives emit, worldInteractions, playerInventories, playerAnimations and players;
  // `owner()` names the calling resource.
  constructor(host) {
    this.host = host;
    this.recipes = new Map(); // id -> { recipe, resource }
    this.stations = new Map(); // id -> station
    this.sessions = new Map(); // player id -> session
    this.learned = new Map(); // player id -> Set of recipe ids
    this.nextStation = 1;
    this.nextSession = 1;
  }

  owner() {
    return this.host.context.getStore()?.name;
  }

  addRecipe(options) {
    const recipe = recipeOf(options);
    if (!this.recipes.has(recipe.id) && this.recipes.size >= RECIPES_MAX) {
      throw new RangeError(`at most ${RECIPES_MAX} custom recipes`);
    }
    this.recipes.set(recipe.id, { recipe, resource: this.owner() });
    return recipeInfo(recipe);
  }

  removeRecipe(id) {
    for (const known of this.learned.values()) {
      known.delete(id);
    }
    return this.recipes.delete(id);
  }

  // Crafting.learnRecipe: false for a recipe the server does not have; a recipe already known is
  // not announced again.
  learnRecipe(player, id, source = 'sdk') {
    if (!this.recipes.has(id)) {
      return false;
    }
    const known = this.learned.get(player.id) ?? new Set();
    if (known.has(id)) {
      return true;
    }
    known.add(id);
    this.learned.set(player.id, known);
    this.host.emit('craftingRecipeLearned', player, Object.freeze({ recipe: id, source }));
    return true;
  }

  forgetRecipe(player, id) {
    return this.learned.get(player.id)?.delete(id) ?? false;
  }

  // Whether the player may craft the recipe: any player for one without learning.
  knowsRecipe(player, id) {
    const known = this.recipes.get(id);
    if (!known) {
      return false;
    }
    return !known.recipe.learn || (this.learned.get(player.id)?.has(id) ?? false);
  }

  learnedRecipes(player) {
    return Object.freeze([...(this.learned.get(player.id) ?? [])].sort());
  }

  // A player used an item in their inventory (playerCustomItemUse): the recipes it is the sketch
  // of are learned. Whether it taught one.
  itemUsed(player, row) {
    let taught = false;
    for (const { recipe } of this.recipes.values()) {
      if (recipe.sketch !== null && recipe.sketch === row?.item) {
        this.learnRecipe(player, recipe.id, 'sketch');
        taught = true;
      }
    }
    return taught;
  }

  recipe(id) {
    const known = this.recipes.get(id);
    return known ? recipeInfo(known.recipe) : null;
  }

  addStation(options) {
    if (!options || typeof options !== 'object') {
      throw new TypeError('Crafting.addStation takes an options object');
    }
    for (const key of Object.keys(options)) {
      if (!STATION_OPTIONS.has(key)) {
        throw new TypeError(`unknown Crafting.addStation option ${key}`);
      }
    }
    if (this.stations.size >= STATIONS_MAX) {
      throw new RangeError(`at most ${STATIONS_MAX} custom stations`);
    }
    const recipes = options.recipes ?? [];
    if (!Array.isArray(recipes) || recipes.some((id) => typeof id !== 'string')) {
      throw new TypeError('Crafting.addStation recipes must be a list of recipe ids');
    }
    if (options.autoProgress !== undefined && typeof options.autoProgress !== 'boolean') {
      throw new TypeError('Crafting.addStation autoProgress must be true or false');
    }
    const animation = options.animation;
    if (
      animation !== undefined &&
      typeof animation !== 'string' &&
      (!animation || typeof animation !== 'object' || typeof animation.fragment !== 'string')
    ) {
      throw new TypeError('Crafting.addStation animation must be a fragment name or { fragment }');
    }
    const id = `custom/${this.nextStation++}`;
    const radius = options.radius ?? 2;
    const interaction = this.host.worldInteractions.add(
      {
        ...(options.entity !== undefined ? { entity: options.entity } : {}),
        ...(options.position !== undefined ? { position: options.position } : {}),
        radius,
        key: options.key ?? 'primary_action',
        text: options.text ?? 'Craft',
      },
      (player) => this.used(player, id),
    );
    const station = {
      id,
      recipes: [...recipes],
      animation,
      autoProgress: options.autoProgress ?? true,
      dimension: options.dimension,
      position: options.position ? { ...options.position } : null,
      entity: options.entity ?? '',
      reach: radius * REACH_FACTOR,
      interaction,
      resource: this.owner(),
    };
    this.stations.set(id, station);
    return id;
  }

  removeStation(id) {
    const station = this.stations.get(id);
    if (!station) {
      return false;
    }
    for (const [playerId, session] of this.sessions) {
      if (session.station === id) {
        this.cancelFor(this.host.players.get(playerId), 'cancelled');
      }
    }
    station.interaction.remove();
    this.stations.delete(id);
    return true;
  }

  stationInfos() {
    return [...this.stations.values()].map((station) => ({
      id: station.id,
      kind: 'custom',
      recipes: [...station.recipes],
      position: station.position ? { ...station.position } : null,
      entity: station.entity,
      autoProgress: station.autoProgress,
    }));
  }

  // A player used a station's hint: its only recipe starts, else the resource chooses
  // (craftingStationUsed, then Crafting.craft).
  used(player, stationKey) {
    const station = this.stations.get(stationKey);
    if (!station) {
      return;
    }
    if (station.recipes.length === 1) {
      this.craft(player, stationKey, station.recipes[0]);
      return;
    }
    this.host.emit('craftingStationUsed', player, Object.freeze({ station: stationKey }));
  }

  occupantOf(stationKey) {
    for (const [playerId, session] of this.sessions) {
      if (session.station === stationKey) {
        return playerId;
      }
    }
    return null;
  }

  // Starts a craft: { ok, reason }.
  craft(player, stationKey, recipeId) {
    const station = this.stations.get(stationKey);
    const known = this.recipes.get(recipeId);
    if (!station) {
      return { ok: false, reason: 'unknownStation' };
    }
    if (!known || !station.recipes.includes(recipeId)) {
      return { ok: false, reason: 'unknownRecipe' };
    }
    if (!this.knowsRecipe(player, recipeId)) {
      return { ok: false, reason: 'notLearned' };
    }
    if (this.sessions.has(player.id)) {
      return { ok: false, reason: 'busy' };
    }
    const occupant = this.occupantOf(stationKey);
    if (occupant !== null) {
      return { ok: false, reason: 'occupied' };
    }
    const recipe = known.recipe;
    const dimension = player.dimension;
    const proposal = Object.freeze({
      kind: 'custom',
      station: stationKey,
      dimension,
      recipe: recipe.id,
      continuationOf: '',
    });
    if (!this.host.emit('craftingStarting', player, proposal)) {
      return { ok: false, reason: 'refused' };
    }
    const taken = this.take(player, recipe.ingredients);
    if (!taken.ok) {
      return { ok: false, reason: taken.reason };
    }
    const session = {
      id: `custom-${this.nextSession++}`,
      station: stationKey,
      recipe,
      dimension,
      progress: 0,
      startedAt: Date.now(),
      lastTick: Date.now(),
      materials: taken.materials,
    };
    this.sessions.set(player.id, session);
    if (station.animation !== undefined) {
      const { fragment, ...rest } =
        typeof station.animation === 'string' ? { fragment: station.animation } : station.animation;
      this.host.playerAnimations.play(player, fragment, rest);
    }
    this.host.emit(
      'craftingStarted',
      player,
      Object.freeze({
        kind: 'custom',
        session: session.id,
        station: stationKey,
        dimension,
        recipe: recipe.id,
        continuationOf: '',
      }),
    );
    return { ok: true, reason: '' };
  }

  // Takes all of the ingredients from the player's inventory or none: { ok, reason, materials }
  // where materials are what was taken with its properties, for a refund.
  take(player, ingredients) {
    const materials = [];
    const result = this.host.playerInventories.operate(
      player,
      (inventory) => {
        const units = [];
        for (const { item, amount } of ingredients) {
          let wanted = amount;
          const rows = [...inventory.rows.values()]
            .filter((row) => row.itemClass.id === item)
            .sort((a, b) => a.equipped - b.equipped);
          for (const row of rows) {
            const free = row.amount - row.equipped;
            const used = Math.min(free, wanted);
            if (used > 0) {
              units.push({ id: row.id, amount: used });
              materials.push({ item, amount: used, metadata: inventory.rowView(row).metadata });
              wanted -= used;
            }
          }
          if (wanted > 0) {
            return { result: { ok: false, code: 'insufficientItems' }, change: null };
          }
        }
        return inventory.remove({ units });
      },
      { notify: true },
    );
    if (!result?.ok) {
      return { ok: false, reason: result?.code ?? 'inventoryUnavailable' };
    }
    return { ok: true, materials };
  }

  activeSession(playerId, now = Date.now()) {
    const session = this.sessions.get(playerId);
    if (!session) {
      return null;
    }
    return {
      kind: 'custom',
      id: session.id,
      station: session.station,
      dimension: session.dimension,
      phase: 'working',
      sequence: undefined,
      resources: undefined,
      recipe: session.recipe.id,
      materials: session.materials.map(({ item, amount }) => ({ item, amount })),
      progress: session.progress,
      expiresInMs: Math.max(0, (1 - session.progress) * session.recipe.duration * 1000),
    };
  }

  occupancyOf(stationKey, dimension) {
    if (!this.stations.has(stationKey)) {
      return null;
    }
    const playerId = this.occupantOf(stationKey);
    if (playerId === null) {
      return { station: stationKey, dimension, occupied: false, occupant: null };
    }
    const info = this.activeSession(playerId);
    return {
      station: stationKey,
      dimension,
      occupied: true,
      occupant: {
        playerId,
        entryId: info.id,
        sessionId: info.id,
        phase: 'working',
        expiresInMs: info.expiresInMs,
      },
    };
  }

  // Crafting.setProgress / addProgress: false when the player crafts at no custom station.
  setProgress(player, value) {
    const session = this.sessions.get(player.id);
    if (!session) {
      return false;
    }
    if (typeof value !== 'number' || !Number.isFinite(value)) {
      throw new TypeError('Crafting progress must be a number');
    }
    session.progress = Math.min(1, Math.max(0, value));
    if (session.progress >= 1) {
      this.complete(player, session);
    }
    return true;
  }

  addProgress(player, amount) {
    const session = this.sessions.get(player.id);
    if (!session) {
      return false;
    }
    if (typeof amount !== 'number' || !Number.isFinite(amount)) {
      throw new TypeError('Crafting progress must be a number');
    }
    return this.setProgress(player, session.progress + amount);
  }

  // Every server tick: time-run progress, and a player who went away cancels.
  tick(now = Date.now()) {
    for (const [playerId, session] of [...this.sessions]) {
      const player = this.host.players.get(playerId);
      const station = this.stations.get(session.station);
      if (!player || !station) {
        continue;
      }
      if (station.position && player.position) {
        const at = player.position;
        const there = station.position;
        if (Math.hypot(at.x - there.x, at.y - there.y, at.z - there.z) > station.reach) {
          this.cancelFor(player, 'cancelled');
          continue;
        }
      }
      if (station.autoProgress) {
        const seconds = Math.max(0, (now - session.lastTick) / 1000);
        session.lastTick = now;
        this.setProgress(player, session.progress + seconds / session.recipe.duration);
      }
    }
  }

  // The craft is done: craftingCompleting decides, then the products come.
  complete(player, session) {
    this.sessions.delete(player.id);
    this.stopAnimation(player, session);
    const recipe = session.recipe;
    const first = recipe.products[0];
    const proposal = Object.freeze({
      kind: 'custom',
      session: session.id,
      station: session.station,
      outcome: 'success',
      recipe: recipe.id,
      grade: 0,
      product: first.item,
      amount: first.amount,
      quality: 1,
      xp: 0,
      lockpicks: undefined,
    });
    if (!this.host.emit('craftingCompleting', player, proposal)) {
      this.ended(player, session, 'failed', 'craftingCompletingRejected', []);
      return;
    }
    const outputs = this.give(player, recipe.products);
    this.host.emit(
      'craftingCompleted',
      player,
      Object.freeze({
        kind: 'custom',
        session: session.id,
        station: session.station,
        dimension: session.dimension,
        outcome: 'success',
        recipe: recipe.id,
        grade: 0,
        quality: 1,
        xp: 0,
        lockpicks: undefined,
        outputs: Object.freeze(outputs),
        knowledge: undefined,
      }),
    );
    this.ended(player, session, 'success', '', []);
  }

  // Crafting.cancel, a player who went away or left: craftingRefunding is told what the craft
  // holds (refund() changes nothing: an unfinished custom craft always gives its materials back,
  // with their properties).
  cancelFor(player, reason) {
    const session = player ? this.sessions.get(player.id) : undefined;
    if (!session) {
      return false;
    }
    this.sessions.delete(player.id);
    this.stopAnimation(player, session);
    this.host.emit(
      'craftingRefunding',
      player,
      Object.freeze({
        kind: 'custom',
        session: session.id,
        station: session.station,
        dimension: session.dimension,
        outcome: 'cancelled',
        reason,
        materials: Object.freeze(session.materials.map((unit) => Object.freeze({ ...unit }))),
      }),
      () => {},
    );
    const refunded = this.give(player, session.materials);
    this.ended(player, session, 'cancelled', reason, refunded);
    return true;
  }

  // Adds units to the player's inventory (with their properties, when given); the rows and units
  // that received them.
  give(player, units) {
    const rows = [];
    for (const { item, amount, metadata } of units) {
      const result = this.host.playerInventories.operate(
        player,
        (inventory) => inventory.add(metadata ? { item, amount, metadata } : { item, amount }),
        { notify: true },
      );
      for (const row of result?.items ?? []) {
        rows.push(Object.freeze({ id: row.id, amount: row.amount }));
      }
    }
    return rows;
  }

  ended(player, session, outcome, reason, refunded) {
    this.host.emit(
      'craftingEnded',
      player,
      Object.freeze({
        kind: 'custom',
        session: session.id,
        station: session.station,
        dimension: session.dimension,
        outcome,
        reason,
        refunded: Object.freeze(refunded),
      }),
    );
  }

  stopAnimation(player, session) {
    const station = this.stations.get(session.station);
    if (station?.animation !== undefined && this.host.players.get(player.id) === player) {
      this.host.playerAnimations.stop(player, {});
    }
  }

  left(player) {
    this.cancelFor(player, 'disconnected');
    this.learned.delete(player.id);
  }

  resourceStopped(name) {
    for (const [id, station] of [...this.stations]) {
      if (station.resource === name) {
        this.removeStation(id);
      }
    }
    for (const [id, known] of [...this.recipes]) {
      if (known.resource === name) {
        this.recipes.delete(id);
      }
    }
  }
}

module.exports = { CustomCrafting };
