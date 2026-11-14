'use strict';

// Crafting stations, who holds them and the players' sessions at them (first
// part): Crafting.stations, Crafting.occupancy and Crafting.session, and the events
// craftingStarting, craftingStarted and craftingEnded. stations.json is built from a game
// installation by Dev/tools/crafting/build_stations.py: every smithy and alchemy table placed in
// the game's levels.
//
// The player's game asks before it opens a station (CRAFT_REPORT Ask,
// kcd2s/integration/crafting.hpp): refused while another player holds that station in the same
// world, else craftingStarting decides, and the verdict goes back (CRAFT_VERDICT). A session runs
// from the minigame's start (CRAFT_REPORT Started, or the station the player's client publishes
// in the NetworkPlayer field "stationAction", which the native server passes in each player
// report) to its end (CRAFT_REPORT Ended, the field emptied, or the player leaving). Within it
// run alchemy batches (the first from the start, each next after a potion came) and smithing
// workpieces (from the moment the game took their materials): craftingStarted, then
// craftingCompleted and craftingEnded ('success') when the game gave a recipe's product, or
// craftingEnded ('cancelled') when the player left the station first.
//
// The materials: the player's game takes them and gives the product; the server
// follows the inventory changes that reach it (the game's own, `drop` and `loot`) and settles
// each batch or workpiece by them:
// - a recipe's product asks craftingCompleting first; granted, the server gives the product from
//   its own inventory in place of the unit the player's game made (reissue); a refusal takes the
//   product back out of the inventory (smithing gives back what a failed workpiece keeps: all but
//   half of each divisible material, a coin deciding a single unit);
// - a batch or workpiece that ends without a product waits a moment for the game's own return of
//   materials (the game's ReturnIngredientCoef share), then asks craftingRefunding: its refund()
//   gives back every unit taken and not yet returned, with its properties; without it the game's
//   own rules stand. craftingEnded names the rows that got materials back.

const catalog = require('./stations.json');
const recipes = require('../recipes/recipes.cjs');
const { SMITHING_FINISHING_XP } = recipes;
const { lookupItemClass } = require('../inventory/catalog.cjs');

const KINDS = ['alchemy', 'smithing'];
// The field's kinds.
const FIELD_KINDS = { smith: 'smithing', alchemy: 'alchemy' };
// The station kinds a player's game reports while it works at one (a smithy, an alchemy table, a
// grindstone); such a player cannot be fought.
const WORKING_KINDS = new Set(['smith', 'alchemy', 'grind']);
// How far a reported station may stand from the catalog's, in metres.
const STATION_MATCH = 1.5;
// A session's time before the station is taken back (the contract's 30 minutes a batch).
const SESSION_MS = 30 * 60 * 1000;
// A world left out is the global one, where every entity starts (4294967295 in KCDS).
const GLOBAL_WORLD = 0xffffffff;
// How long an unfinished batch or workpiece waits for the game's own return of materials before
// it settles, in milliseconds.
const SETTLE_MS = 2000;
// How long an alchemy table may show more of an ingredient than the brewer's inventory gave up
// (the inventory report and the table report travel apart) before the batch ends clientError.
const STEP_GRACE_MS = 2000;
// The categories whose units the inventory reports as used or shot, not dropped.
const UNCOUNTED_CATEGORIES = new Set(['food', 'ointment', 'poison', 'ammo']);
// The lockpicks a smith's perks draw alongside a product.
const LOCKPICK = '8d76f58e-a521-4205-a7e8-9ac077eee5f0';
// A whole alchemy recipe as a knowledge mask: its 29 authored steps.
const WHOLE_RECIPE = 536870911;
// The game's failed potion: what a brew that matched no recipe, or failed its recipe, gives
const FAILED_POTION = '006ec8d6-1ce1-4e90-8267-7c349812ddcd';

const STATIONS = catalog.map((station) => Object.freeze(station));
const BY_ID = new Map(STATIONS.map((station) => [station.id, station]));

// The game's recipes by kind, and each kind's products: item class -> its recipe.
const RECIPES = recipes.define();
const PRODUCTS = Object.fromEntries(
  KINDS.map((kind) => [
    kind,
    new Map(
      RECIPES.all(kind).flatMap((recipe) =>
        recipe.products.map((product) => [product.item, recipe]),
      ),
    ),
  ]),
);

// The smithing recipe whose ingredients the units lost cover, when only one does; else undefined
// (the product names it at the end).
function smithingRecipeOf(lost) {
  const matching = RECIPES.all('smithing').filter((recipe) =>
    recipe.ingredients.every((ingredient) => (lost.get(ingredient.item) ?? 0) >= ingredient.amount),
  );
  return matching.length === 1 ? matching[0].id : undefined;
}

// Whether a product the player's game made can come from this run (the game, not the
// server, makes it, so a changed game could name any product): it is its recipe's product, the
// smith's chosen recipe when one was known, within the recipe's yield and the item's quality,
// and the run's taken materials cover every ingredient the inventory reports as taken (quest
// items are never taken; food, ointments, poisons and ammunition leave as used, not counted; an
// ingredient the catalog does not know counts).
function productFits(run, recipe, output) {
  if (run.recipe !== undefined && run.recipe !== recipe.id && recipe.kind === 'smithing') {
    return false;
  }
  const product = recipe.products.find((entry) => entry.item === output.row.item);
  const most = recipe.kind === 'smithing' ? 1 : product?.maxYield ?? 1;
  if (!product || !(output.amount >= 1 && output.amount <= most)) {
    return false;
  }
  const maximum = lookupItemClass(output.row.item)?.maximumQuality ?? 0;
  const quality = Number(output.row.quality) || 0;
  if (maximum > 0 && quality > maximum) {
    return false;
  }
  return recipe.ingredients.every((ingredient) => {
    const itemClass = lookupItemClass(ingredient.item);
    if (itemClass && (itemClass.quest || UNCOUNTED_CATEGORIES.has(itemClass.category))) {
      return true;
    }
    return (run.materials.get(ingredient.item) ?? 0) >= ingredient.amount;
  });
}

// What a failed workpiece keeps of the units it took: half of each divisible amount is spent,
// rounded down, with one unit left to chance. `random()` gives 0 to 1.
function failureKept(held, random) {
  const kept = [];
  for (const unit of held) {
    let spent = Math.floor(unit.amount / 2);
    if (unit.amount === 1) {
      spent = random() < 0.5 ? 1 : 0;
    }
    if (unit.amount - spent > 0) {
      kept.push({ ...unit, amount: unit.amount - spent });
    }
  }
  return kept;
}

// Units of a list less those of another, by item class (the first units of a class go first).
function unitsLess(units, less) {
  const left = new Map();
  for (const { item, amount } of less) {
    left.set(item, (left.get(item) ?? 0) + amount);
  }
  const rest = [];
  for (const unit of units) {
    const taken = Math.min(unit.amount, left.get(unit.item) ?? 0);
    left.set(unit.item, (left.get(unit.item) ?? 0) - taken);
    if (unit.amount - taken > 0) {
      rest.push({ ...unit, amount: unit.amount - taken });
    }
  }
  return rest;
}

// A recipe as Crafting.recipes gives it: its native identity (an alchemy recipe's number, a
// smithing recipe's id), what it takes (with the item's name and whether only a quest gives it)
// and what it can make (the alternatives by rank or minimum quality).
function recipeInfo(recipe) {
  const smithing = recipe.kind === 'smithing';
  const ingredients = recipe.ingredients.map(({ item, amount }) => {
    const itemClass = lookupItemClass(item);
    return Object.freeze({
      item,
      name: itemClass ? itemClass.name : '',
      amount,
      quest: Boolean(itemClass && itemClass.quest),
    });
  });
  const products = recipe.products.map((product) =>
    Object.freeze({
      item: product.item,
      threshold: smithing ? product.minQuality : product.quality,
      maxYield: smithing ? 1 : product.maxYield,
    }),
  );
  return Object.freeze({
    key: Object.freeze(
      smithing ? { kind: 'smithing', id: recipe.id } : { kind: 'alchemy', id: recipe.number },
    ),
    name: recipe.uiName,
    dlcId: recipe.dlcId,
    minSkill: smithing ? recipe.minSkill : 0,
    perk: smithing ? recipe.perks[0] : '',
    base: smithing ? '' : recipe.base,
    ingredients: Object.freeze(ingredients),
    products: Object.freeze(products),
    source: recipes.sourceOf(recipe.id) ?? 'game',
  });
}

function checkKind(kind, name) {
  if (kind !== undefined && !KINDS.includes(kind)) {
    throw new TypeError(`Crafting.${name} takes 'alchemy', 'smithing' or nothing`);
  }
}

function stationInfo(station) {
  return {
    id: station.id,
    kind: station.kind,
    level: station.level,
    layer: station.layer,
    position: { ...station.position },
    forward: { ...station.forward },
    // One of the level's own, not one a script placed.
    placed: false,
  };
}

// The kind and the place of the station words, or null.
function parseStation(words) {
  if (typeof words !== 'string' || words === '') {
    return null;
  }
  const [fieldKind, place] = words.split(' ');
  const kind = FIELD_KINDS[fieldKind];
  if (!kind || !place || place[0] !== 'S') {
    return null;
  }
  const [x, y, z] = place.slice(1).split(',').map(Number);
  if (![x, y, z].every(Number.isFinite)) {
    return null;
  }
  return { kind, position: { x, y, z } };
}

// The catalog's station of that kind standing at a place, or null; `placed` adds the ones scripts
// placed (SmartObject.spawn).
function stationAt(kind, position, placed = []) {
  let best = null;
  let bestDistance = STATION_MATCH;
  for (const station of [...STATIONS, ...placed]) {
    if (station.kind !== kind) {
      continue;
    }
    const at = station.position;
    const distance = Math.hypot(at.x - position.x, at.y - position.y, at.z - position.z);
    if (distance <= bestDistance) {
      best = station;
      bestDistance = distance;
    }
  }
  return best;
}

class CraftingSessions {
  // `events.emit(name, ...args)` raises a resources' event (false when a handler refused);
  // `events.sendVerdict(player, request, allowed)` answers a player's game;
  // `events.give(player, units)` adds units ({ item, amount, metadata }) to the player's inventory
  // and returns the rows that got them; `events.remove(player, units)` takes units ({ id, amount
  // }) out of it. `random()` decides a failed workpiece's single units.
  constructor(events = {}, random = Math.random) {
    this.events = {
      emit: () => true,
      sendVerdict: () => {},
      give: () => [],
      remove: () => false,
      ...events,
    };
    this.random = random;
    // player id -> the visit: { kind, entry, station, dimension, startedAt, run, lastRun }
    this.sessions = new Map();
    // player id -> { player, visit, run, reason, due }: unfinished runs waiting to settle
    this.settling = new Map();
    // The players whose station Crafting.cancel closed, until their game reports it closed.
    this.closing = new Set();
    // The players whose game reports them at a game station (catalogued or not), who cannot be
    // fought meanwhile.
    this.working = new Set();
    this.next = 1;
    // player id -> Map(recipe id -> mask): alchemy recipes known in part (grantKnowledge).
    this.partialKnowledge = new Map();
  }

  // The stations of a kind scripts placed ({ id, kind, position }), none without the host's.
  placedOf(kind) {
    return this.events.placedStations?.(kind) ?? [];
  }

  // The player's game asks to open a station (CRAFT_REPORT Ask): refused while another
  // player holds it in that world, else craftingStarting decides.
  ask(player, { request, kind, station: position }, dimension) {
    const station =
      KINDS.includes(kind) && position ? stationAt(kind, position, this.placedOf(kind)) : null;
    const stationKey = station ? station.id : '';
    let allowed = true;
    if (station) {
      const held = this.occupancyOf(stationKey, dimension);
      allowed = !held.occupied || held.occupant.playerId === player.id;
      // A station a level edit took out of the world takes no new session.
      if (allowed && this.events.stationRemoved?.(stationKey, dimension)) {
        allowed = false;
      }
    }
    // A smithy is asked about with the recipe chosen at it (chooseRecipe); an alchemy table as it
    // opens: a table the player kept follows its last batch.
    if (allowed && kind === 'alchemy') {
      const visit = this.sessions.get(player.id);
      const kept =
        visit &&
        visit.kind === 'alchemy' &&
        visit.station === stationKey &&
        visit.dimension === dimension;
      const proposal = Object.freeze({
        kind,
        station: stationKey,
        dimension,
        recipe: undefined,
        continuationOf: kept ? visit.lastRun : '',
      });
      allowed = this.events.emit('craftingStarting', player, proposal);
    }
    this.events.sendVerdict(player, request, allowed);
    return allowed;
  }

  // The player holds a station (CRAFT_REPORT Started, or the station field appearing). An alchemy
  // table's first batch starts at once; a smithy's workpiece when its recipe is chosen
  // (chooseRecipe) or, unreported, when it takes its materials.
  begin(player, kind, position, dimension, now = Date.now()) {
    const station = stationAt(kind, position, this.placedOf(kind));
    if (!station) {
      this.end(player, 'cancelled');
      return;
    }
    const current = this.sessions.get(player.id);
    if (current && current.station === station.id && current.dimension === dimension) {
      return;
    }
    if (current) {
      this.end(player, 'cancelled');
    }
    const visit = {
      player,
      kind: station.kind,
      entry: `${station.kind}-${this.next++}`,
      station: station.id,
      dimension,
      startedAt: now,
      run: null,
      lastRun: '',
    };
    this.sessions.set(player.id, visit);
    if (visit.kind === 'alchemy') {
      this.startRun(player, visit, undefined, visit.entry);
    }
  }

  // A batch or a workpiece starts: craftingStarted.
  startRun(player, visit, recipe, id = `${visit.kind}-${this.next++}`) {
    const continuationOf = visit.kind === 'alchemy' ? visit.lastRun : '';
    // materials: item class -> units taken; held: the units taken and not returned, with their
    // properties; returned: the rows the game gave materials back to.
    // An alchemy batch also keeps what lies on the table (resources, as its client reports it),
    // how many changes of it were seen (sequence), and the row each material was taken from.
    visit.run = {
      id,
      recipe,
      materials: new Map(),
      held: [],
      returned: [],
      continuationOf,
      resources: [],
      sequence: 0,
      nextResource: 1,
      rowOf: new Map(),
    };
    this.events.emit(
      'craftingStarted',
      player,
      Object.freeze({
        kind: visit.kind,
        session: id,
        station: visit.station,
        dimension: visit.dimension,
        recipe: visit.kind === 'smithing' ? recipe : undefined,
        continuationOf,
      }),
    );
  }

  // The visit's batch or workpiece is over: craftingEnded.
  endRun(player, visit, outcome, reason, refunded = []) {
    const run = visit.run;
    if (!run) {
      return;
    }
    visit.run = null;
    visit.lastRun = run.id;
    this.ended(player, visit, run, outcome, reason, refunded);
  }

  // craftingEnded, with the rows that got materials back.
  ended(player, visit, run, outcome, reason, refunded) {
    this.events.emit(
      'craftingEnded',
      player,
      Object.freeze({
        kind: visit.kind,
        session: run.id,
        station: visit.station,
        dimension: visit.dimension,
        outcome,
        reason,
        refunded: Object.freeze(refunded.map((row) => Object.freeze({ ...row }))),
      }),
    );
  }

  // What the player's inventory changed while they hold a station, or while an unfinished batch
  // or workpiece of theirs waits to settle (after its playerInventoryChanged): units lost are its
  // materials, a recipe's product gained completes it, units of its materials gained are the
  // game giving them back. The player's game did all of it; the server follows.
  inventoryChanged(player, change) {
    // Only what the player's game changed on its own: units it gave up (`drop`) or gained
    // (`loot`).
    const native = change && (change.reason === 'drop' || change.reason === 'loot');
    if (!native || !Array.isArray(change.items)) {
      return;
    }
    const lost = [];
    const gained = [];
    for (const { id, before, after } of change.items) {
      const row = after ?? before;
      const delta = (after?.amount ?? 0) - (before?.amount ?? 0);
      if (!row || delta === 0) {
        continue;
      }
      if (delta < 0) {
        lost.push({ id, item: row.item, amount: -delta, metadata: (before ?? row).metadata ?? {} });
      } else {
        gained.push({ id, row, amount: delta });
      }
    }
    const visit = this.sessions.get(player.id);
    const waiting = this.settling.get(player.id);
    if (!visit) {
      if (waiting && !this.completeWaiting(player, waiting, gained)) {
        this.returned(waiting.run, gained);
      }
      return;
    }
    if (!visit.run && visit.kind === 'smithing' && lost.length > 0) {
      const units = new Map();
      for (const { item, amount } of lost) {
        units.set(item, (units.get(item) ?? 0) + amount);
      }
      this.startRun(player, visit, smithingRecipeOf(units));
    }
    if (!visit.run) {
      return;
    }
    if (lost.length > 0) {
      for (const unit of lost) {
        visit.run.materials.set(unit.item, (visit.run.materials.get(unit.item) ?? 0) + unit.amount);
        visit.run.held.push({ item: unit.item, amount: unit.amount, metadata: unit.metadata });
        visit.run.rowOf.set(unit.item, unit.id);
      }
    }
    const output = gained.find((unit) => PRODUCTS[visit.kind].has(unit.row.item));
    if (output) {
      this.complete(player, visit, PRODUCTS[visit.kind].get(output.row.item), output, gained);
      return;
    }
    const failed = gained.find((unit) => unit.row.item === FAILED_POTION);
    if (visit.kind === 'alchemy' && failed) {
      this.complete(player, visit, null, failed, gained);
      return;
    }
    this.returned(visit.run, gained);
  }

  // A finished workpiece's product that comes after the smithy closed (the game closes the station
  // as it hands the product over): the workpiece completes as it would at the station. False when
  // the waiting run is not a smith's unbroken workpiece or nothing gained is its product.
  completeWaiting(player, waiting, gained) {
    if (waiting.visit.kind !== 'smithing' || waiting.reason === 'craftingFailed') {
      return false;
    }
    const output = gained.find((unit) => PRODUCTS.smithing.has(unit.row.item));
    if (!output) {
      return false;
    }
    this.settling.delete(player.id);
    waiting.visit.run = waiting.run;
    this.complete(player, waiting.visit, PRODUCTS.smithing.get(output.row.item), output, gained);
    return true;
  }

  // Units of a run's materials the game gave back: no longer held, and their rows refunded.
  returned(run, gained) {
    const back = gained.filter((unit) => run.held.some((held) => held.item === unit.row.item));
    if (back.length === 0) {
      return;
    }
    run.held = unitsLess(
      run.held,
      back.map((unit) => ({ item: unit.row.item, amount: unit.amount })),
    );
    for (const unit of back) {
      run.returned.push({ id: unit.id, amount: unit.amount });
    }
  }

  // A recipe's product came (or, `recipe` null, the game's failed potion): craftingCompleting
  // decides. Granted: craftingCompleted and craftingEnded; refused: the product (and a smith's
  // lockpicks) go back out of the inventory and the batch or workpiece fails. At an alchemy table
  // the next batch follows either way.
  complete(player, visit, recipe, output, gained) {
    const run = visit.run;
    const smithing = visit.kind === 'smithing';
    const quality = Number(output.row.quality) || 0;
    const maximum = lookupItemClass(output.row.item)?.maximumQuality ?? 0;
    const picks = smithing ? gained.filter((unit) => unit.row.item === LOCKPICK) : [];
    const lockpicks = smithing ? picks.reduce((sum, unit) => sum + unit.amount, 0) : undefined;
    // The base craftsmanship XP: a smithing recipe's finishing XP, an alchemy recipe's own.
    const xp = !recipe ? 0 : smithing ? SMITHING_FINISHING_XP : recipe.xp ?? 0;
    const outcome = recipe ? 'success' : 'failed';
    const result = {
      kind: visit.kind,
      session: run.id,
      station: visit.station,
      outcome,
      recipe: recipe ? recipe.id : '',
      grade: quality,
      quality: maximum > 0 ? Math.min(1, quality / maximum) : 0,
      xp,
      lockpicks,
    };
    const proposal = Object.freeze({
      ...result,
      product: output.row.item,
      amount: output.amount,
    });
    // A product this run cannot have made is taken back, and nothing is refunded.
    if (recipe && !productFits(run, recipe, output)) {
      const made = [output, ...picks].map((unit) => ({ id: unit.id, amount: unit.amount }));
      this.events.remove(player, made);
      this.endRun(player, visit, 'failed', 'clientError', []);
      if (visit.kind === 'alchemy') {
        this.startRun(player, visit, undefined);
      }
      return;
    }
    if (!this.events.emit('craftingCompleting', player, proposal)) {
      const granted = [output, ...picks].map((unit) => ({ id: unit.id, amount: unit.amount }));
      this.events.remove(player, granted);
      const refunded = smithing ? this.events.give(player, failureKept(run.held, this.random)) : [];
      this.endRun(player, visit, 'failed', 'craftingCompletingRejected', refunded);
    } else {
      const outputs = [
        ...this.reissue(player, output),
        ...picks.map((unit) => ({ id: unit.id, amount: unit.amount })),
      ].map((row) => Object.freeze(row));
      this.events.emit(
        'craftingCompleted',
        player,
        Object.freeze({
          ...result,
          dimension: visit.dimension,
          outputs: Object.freeze(outputs),
          knowledge: visit.kind === 'alchemy' ? Object.freeze(this.knowledgeOf(player)) : undefined,
        }),
      );
      this.endRun(player, visit, outcome, '');
    }
    if (visit.kind === 'alchemy') {
      this.startRun(player, visit, undefined);
    }
  }

  // The product becomes the server's: the server gives the same item, amount and
  // properties from its own inventory and then takes out the unit the player's game made. When the
  // server cannot give it (a full inventory, an item limit) or take the game's unit, the game's
  // unit stays, so a product is never lost. The rows that hold the product.
  reissue(player, output) {
    const made = { id: output.id, amount: output.amount };
    const given = this.events.give(player, [
      { item: output.row.item, amount: output.amount, metadata: output.row.metadata ?? {} },
    ]);
    if (given.length === 0) {
      return [made];
    }
    if (!this.events.remove(player, [made])) {
      this.events.remove(
        player,
        given.map((row) => ({ id: row.id, amount: row.amount })),
      );
      return [made];
    }
    return given.map((row) => ({ id: row.id, amount: row.amount }));
  }

  // The player no longer holds the station (CRAFT_REPORT Ended, the station field gone, or the
  // player left): a batch or workpiece still running ends unfinished and settles once the game
  // had a moment to give materials back (at once when `now` is null).
  end(player, reason, now = Date.now()) {
    const visit = this.sessions.get(player.id);
    if (!visit) {
      return;
    }
    this.sessions.delete(player.id);
    const run = visit.run;
    if (!run) {
      return;
    }
    this.unfinished(player, visit, run, reason, now);
  }

  // The game broke the smith's workpiece (CRAFT_REPORT Failed): the workpiece ends
  // failed (craftingFailed) and settles as an unfinished one, with what the game gives back; the
  // smith keeps the station for the next one.
  fail(player, now = Date.now()) {
    const visit = this.sessions.get(player.id);
    if (!visit || visit.kind !== 'smithing' || !visit.run) {
      return;
    }
    this.unfinished(player, visit, visit.run, 'craftingFailed', now);
  }

  // A run that did not finish: it leaves the visit and settles once the game had a moment to
  // give materials back (at once when `now` is null).
  unfinished(player, visit, run, reason, now) {
    visit.run = null;
    visit.lastRun = run.id;
    // One run waits at a time: an earlier one settles first.
    const earlier = this.settling.get(player.id);
    if (earlier) {
      this.settle(earlier);
    }
    const waiting = { player, visit, run, reason, due: (now ?? 0) + SETTLE_MS };
    this.settling.set(player.id, waiting);
    if (now === null) {
      this.settle(waiting);
    }
  }

  // An unfinished batch or workpiece settles: craftingRefunding (its refund() gives back every
  // unit still held), then craftingEnded with the rows that got materials back.
  settle(waiting) {
    const { player, visit, run, reason } = waiting;
    if (this.settling.get(player.id) === waiting) {
      this.settling.delete(player.id);
    }
    let open = true;
    let asked = false;
    const refund = () => {
      if (open) {
        asked = true;
      }
    };
    this.events.emit(
      'craftingRefunding',
      player,
      Object.freeze({
        kind: visit.kind,
        session: run.id,
        station: visit.station,
        dimension: visit.dimension,
        outcome: reason === 'craftingFailed' ? 'failed' : 'cancelled',
        reason,
        materials: Object.freeze(run.held.map((unit) => Object.freeze({ ...unit }))),
      }),
      refund,
    );
    open = false;
    const refunded = [...run.returned];
    if ((asked || waiting.refundAll) && run.held.length > 0) {
      refunded.push(...this.events.give(player, run.held));
      run.held = [];
    }
    const outcome = reason === 'craftingFailed' ? 'failed' : 'cancelled';
    this.ended(player, visit, run, outcome, reason, refunded);
  }

  // Every tick: unfinished batches and workpieces whose wait is over settle.
  tick(now = Date.now()) {
    for (const visit of [...this.sessions.values()]) {
      if (visit.run?.excessSince !== undefined) {
        this.stepCheck(visit.player, now);
      }
    }
    for (const waiting of [...this.settling.values()]) {
      if (waiting.due <= now) {
        this.settle(waiting);
      }
    }
  }

  // The smithing recipe the player chose at the smithy they hold (CRAFT_REPORT Recipe):
  // craftingStarting decides with the recipe named; a refusal closes the station (as
  // Crafting.cancel does), else the workpiece starts (craftingStarted with its recipe).
  chooseRecipe(player, recipe, now = Date.now()) {
    const visit = this.sessions.get(player.id);
    const known = RECIPES.get(recipe);
    if (!visit || visit.kind !== 'smithing' || !known || known.kind !== 'smithing' || visit.run) {
      return;
    }
    const proposal = Object.freeze({
      kind: 'smithing',
      station: visit.station,
      dimension: visit.dimension,
      recipe,
      continuationOf: '',
    });
    // The server checks the player's progression: a recipe whose perk the player does
    // not own, or whose minSkill is over their craftsmanship, is refused before any resource is
    // asked, as the game's own screen would not offer it.
    // The server's inventory must hold the recipe's materials (a quest item is
    // required but never taken, so it is not counted).
    const materials = recipeInfo(known)
      .ingredients.filter((ingredient) => !ingredient.quest)
      .map(({ item, amount }) => ({ item, amount }));
    const holds = !this.events.holds || this.events.holds(player, materials);
    const allowed = holds && (!this.events.mayForge || this.events.mayForge(player, known));
    if (!allowed || !this.events.emit('craftingStarting', player, proposal)) {
      this.events.sendVerdict(player, 0, false);
      this.closing.add(player.id);
      this.sessions.delete(player.id);
      return;
    }
    this.startRun(player, visit, recipe);
  }

  // A report of the player's game (CRAFT_REPORT): 'ask', 'started', 'recipe' or 'ended'.
  report(player, message, dimension) {
    if (!player || !KINDS.includes(message.kind)) {
      return;
    }
    if (message.report === 'ask') {
      this.closing.delete(player.id);
      this.ask(player, message, dimension);
    } else if (message.report === 'started' && message.station) {
      if (!this.closing.has(player.id)) {
        this.begin(player, message.kind, message.station, dimension);
      }
    } else if (message.report === 'recipe' && message.kind === 'smithing') {
      this.chooseRecipe(player, message.reason);
    } else if (message.report === 'failed' && message.kind === 'smithing') {
      this.fail(player);
    } else if (message.report === 'ended') {
      this.end(
        player,
        typeof message.reason === 'string' && message.reason ? message.reason : 'cancelled',
      );
    }
  }

  // A player report: the station words the player's client publishes now, in their world.
  observe(player, words, dimension, now = Date.now()) {
    const fieldKind = typeof words === 'string' ? words.split(' ')[0] : '';
    if (WORKING_KINDS.has(fieldKind)) {
      this.working.add(player.id);
    } else {
      this.working.delete(player.id);
    }
    const reported = parseStation(words);
    if (reported && this.closing.has(player.id)) {
      // A station Crafting.cancel closed, until the player's game has closed it.
      return;
    }
    if (reported) {
      this.begin(player, reported.kind, reported.position, dimension, now);
    } else {
      this.closing.delete(player.id);
      this.end(player, 'cancelled');
    }
  }

  // Crafting.cancel at a game station: the player's game is told to close it (a verdict for
  // request 0, which no ask carries) and the session ends as leaving it would.
  // With refund, what ran settles at once with every material it still held given back.
  cancel(player, now = Date.now(), { refund = false } = {}) {
    if (!this.sessions.has(player.id)) {
      return false;
    }
    this.events.sendVerdict(player, 0, false);
    this.closing.add(player.id);
    this.end(player, 'cancelled', now);
    const waiting = this.settling.get(player.id);
    if (refund && waiting) {
      waiting.refundAll = true;
      this.settle(waiting);
    }
    return true;
  }

  // Crafting.knowledge (alchemy): recipe id -> mask of the steps the player knows. The
  // game knows a recipe whole, through its perks (Player.knowsRecipe): such a recipe is
  // WHOLE_RECIPE; a part of one set by grantKnowledge is kept here for this session, and the game's
  // book does not show it.
  knowledgeOf(player) {
    const knowledge = {};
    const partial = this.partialKnowledge.get(player.id);
    for (const recipe of RECIPES.all('alchemy')) {
      if (typeof player.knowsRecipe === 'function' && player.knowsRecipe(recipe.id)) {
        knowledge[recipe.id] = WHOLE_RECIPE;
      } else if (partial?.has(recipe.id)) {
        knowledge[recipe.id] = partial.get(recipe.id);
      }
    }
    return knowledge;
  }

  // Crafting.grantKnowledge: the whole recipe teaches it at once, 0 forgets it, a part is kept for
  // the session (the recipe stays unknown to the game). False for an unknown recipe, a mask out
  // of range or a player who is not connected.
  grantKnowledge(player, recipeId, mask) {
    const recipe = RECIPES.all('alchemy').find((each) =>
      typeof recipeId === 'number' ? each.number === recipeId : each.id === recipeId,
    );
    if (!recipe || !Number.isInteger(mask) || mask < 0 || mask > WHOLE_RECIPE) {
      return false;
    }
    if (!this.events.connected || !this.events.connected(player)) {
      return false;
    }
    const partial = this.partialKnowledge.get(player.id) ?? new Map();
    partial.delete(recipe.id);
    if (mask === WHOLE_RECIPE) {
      player.learnRecipe(recipe.id);
    } else {
      player.forgetRecipe(recipe.id);
      if (mask !== 0) {
        partial.set(recipe.id, mask);
      }
    }
    this.partialKnowledge.set(player.id, partial);
    return true;
  }

  // The player left the server: what they had running settles now, while their inventory is
  // still there.
  left(player) {
    this.partialKnowledge.delete(player.id);
    this.working.delete(player.id);
    this.closing.delete(player.id);
    this.end(player, 'disconnected', null);
    const waiting = this.settling.get(player.id);
    if (waiting) {
      this.settle(waiting);
    }
  }

  // CRAFT_TABLE: what lies on the alchemy table the player brews at, [position, item class,
  // base, milled, distilled] each. A thing still lying where it lay keeps its id; every thing that
  // came, went or changed counts as one step of the batch's sequence. Each step is checked against
  // the ingredients the server holds: a table that shows more units of an ingredient than the
  // brewer's inventory gave up for the batch (one unit per place; base liquids are the table's
  // own) is a client that brews from nothing, and once that lasts STEP_GRACE_MS the batch ends
  // clientError and the table closes (stepCheck).
  table(player, items, now = Date.now()) {
    const visit = this.sessions.get(player.id);
    const run = visit?.run;
    if (!run || visit.kind !== 'alchemy' || !Array.isArray(items)) {
      return;
    }
    const before = [...run.resources];
    const next = [];
    let changes = 0;
    for (const entry of items) {
      if (!Array.isArray(entry) || entry.length !== 5) {
        continue;
      }
      const [position, item, base, milled, distilled] = entry;
      const index = before.findIndex((kept) => kept.position === position && kept.item === item);
      const kept = index >= 0 ? before.splice(index, 1)[0] : null;
      if (!kept || kept.milled !== milled || kept.distilled !== distilled) {
        changes += 1;
      }
      next.push(
        Object.freeze({
          id: kept ? kept.id : run.nextResource++,
          item,
          itemId: base ? '' : run.rowOf.get(item) ?? '',
          position,
          base: Boolean(base),
          milled: Boolean(milled),
          distilled: Boolean(distilled),
        }),
      );
    }
    run.resources = next;
    run.sequence += changes + before.length;
    if (this.excessOf(run) && run.excessSince === undefined) {
      run.excessSince = now;
    }
    this.stepCheck(player, now);
  }

  // Whether the table shows more of an ingredient than the inventory gave up for the run. Only
  // ingredients the inventory reports given up as a drop are counted; food, ointments, poisons and
  // ammunition leave it as used or shot (inventories.cjs SPENT_AS) and are not checked.
  excessOf(run) {
    const shown = new Map();
    for (const resource of run.resources) {
      const category = lookupItemClass(resource.item)?.category;
      if (!resource.base && !UNCOUNTED_CATEGORIES.has(category)) {
        shown.set(resource.item, (shown.get(resource.item) ?? 0) + 1);
      }
    }
    for (const [item, units] of shown) {
      if (units > (run.materials.get(item) ?? 0)) {
        return true;
      }
    }
    return false;
  }

  // A batch whose table showed more than the server holds for longer than the grace: it ends
  // clientError and the player's table closes, as Crafting.cancel closes it.
  stepCheck(player, now) {
    const visit = this.sessions.get(player.id);
    const run = visit?.run;
    if (!run || run.excessSince === undefined) {
      return;
    }
    if (!this.excessOf(run)) {
      run.excessSince = undefined;
      return;
    }
    if (now - run.excessSince < STEP_GRACE_MS) {
      return;
    }
    this.events.sendVerdict(player, 0, false);
    this.closing.add(player.id);
    this.end(player, 'clientError', now);
  }

  activeSession(playerId, now = Date.now()) {
    const visit = this.sessions.get(playerId);
    if (!visit) {
      return null;
    }
    const run = visit.run;
    const smithing = visit.kind === 'smithing';
    return {
      kind: visit.kind,
      id: run ? run.id : visit.lastRun || visit.entry,
      station: visit.station,
      dimension: visit.dimension,
      phase: smithing ? 'working' : run ? 'brewing' : 'betweenBatches',
      sequence: !smithing && run ? run.sequence : undefined,
      resources: !smithing && run ? Object.freeze([...run.resources]) : undefined,
      recipe: smithing ? run?.recipe : undefined,
      materials:
        smithing && run
          ? [...run.materials].map(([item, amount]) => ({ item, amount }))
          : undefined,
      expiresInMs: Math.max(0, visit.startedAt + SESSION_MS - now),
    };
  }

  occupancyOf(stationKey, dimension, now = Date.now()) {
    if (!BY_ID.has(stationKey)) {
      return null;
    }
    for (const [playerId, session] of this.sessions) {
      if (session.station !== stationKey || session.dimension !== dimension) {
        continue;
      }
      const info = this.activeSession(playerId, now);
      return {
        station: stationKey,
        dimension,
        occupied: true,
        occupant: {
          playerId,
          entryId: session.entry,
          sessionId: info.id,
          phase: info.phase,
          expiresInMs: info.expiresInMs,
        },
      };
    }
    return { station: stationKey, dimension, occupied: false, occupant: null };
  }

  // The resources' Crafting; `playerOf` checks a Player handle (throws otherwise); `custom` holds
  // the custom recipes and stations (crafting/custom.cjs).
  define(playerOf, custom) {
    const sessions = this;
    const checkAnyKind = (kind, name) => {
      if (kind !== 'custom') {
        checkKind(kind, name);
      }
    };
    return Object.freeze({
      // The server does not rule alchemy or smithing yet: the players' games do. Custom crafting
      // is the server's own.
      isAvailable(kind) {
        checkAnyKind(kind, 'isAvailable');
        return kind === 'custom';
      },
      stations(kind) {
        checkAnyKind(kind, 'stations');
        // A station a level edit took out of every world is listed nowhere.
        const native = STATIONS.filter(
          (station) =>
            (kind === undefined || station.kind === kind) &&
            !sessions.events.stationRemoved?.(station.id, GLOBAL_WORLD),
        );
        const own = kind === undefined || kind === 'custom' ? custom.stationInfos() : [];
        // The ones scripts placed (SmartObject.spawn).
        const placed = (kind === undefined ? KINDS : KINDS.filter((k) => k === kind)).flatMap((k) =>
          sessions.placedOf(k).map((station) => ({
            id: station.id,
            kind: station.kind,
            level: '',
            layer: '',
            position: { ...station.position },
            forward: { x: 0, y: 1 },
            placed: true,
          })),
        );
        return Object.freeze([...native.map(stationInfo), ...placed, ...own]);
      },
      // The game's recipes of a kind (both when left out). The server has no DLC policy: every
      // recipe the game ships is listed. Listing one says nothing of the player's knowledge.
      recipes: (player, kind) => {
        playerOf(player);
        checkKind(kind, 'recipes');
        return Object.freeze(RECIPES.all(kind).map(recipeInfo));
      },
      // Registers an alchemy or smithing recipe until the server stops: every
      // client, one that joins later too, loads it into the game's own recipe table.
      defineRecipe: (definition) => {
        const table = recipes.defineRecipe(sessions.events.owner?.() ?? null, definition);
        if (table) {
          sessions.events.sendRecipeTable?.(table);
        }
      },
      session: (player) => {
        const id = playerOf(player).id;
        return custom.activeSession(id) ?? this.activeSession(id);
      },
      occupancy: (stationKey, dimension = GLOBAL_WORLD) => {
        if (typeof stationKey !== 'string') {
          throw new TypeError('Crafting.occupancy takes a station id');
        }
        if (!Number.isInteger(dimension) || dimension < 0 || dimension > GLOBAL_WORLD) {
          throw new TypeError('Crafting.occupancy takes a dimension, 0 to 4294967295');
        }
        return custom.occupancyOf(stationKey, dimension) ?? this.occupancyOf(stationKey, dimension);
      },
      // Ends the player's custom craft (its materials come back), or their session at a game
      // station as leaving it would: their game closes the station and what ran settles as any
      // unfinished batch or workpiece. False when there was nothing to end.
      // { refund: true } gives a game station's run every material it still held back (a custom
      // craft's materials always come back).
      cancel: (player, options = {}) => {
        const checked = playerOf(player);
        if (options === null || typeof options !== 'object') {
          throw new TypeError('Crafting.cancel takes a player and { refund }');
        }
        if (custom.cancelFor(checked, 'cancelled')) {
          return true;
        }
        return this.cancel(checked, Date.now(), { refund: options.refund === true });
      },
      // Alchemy recipe knowledge: recipe id -> mask of its steps.
      knowledge: (player) => Object.freeze(this.knowledgeOf(playerOf(player))),
      grantKnowledge: (player, recipeId, mask) =>
        this.grantKnowledge(playerOf(player), recipeId, mask),
      // Custom recipes and stations (KCDS additions).
      addRecipe: (options) => custom.addRecipe(options),
      removeRecipe: (id) => custom.removeRecipe(id),
      recipe: (id) => custom.recipe(id),
      addStation: (options) => custom.addStation(options),
      removeStation: (id) => custom.removeStation(id),
      craft: (player, stationKey, recipeId) => custom.craft(playerOf(player), stationKey, recipeId),
      setProgress: (player, value) => custom.setProgress(playerOf(player), value),
      addProgress: (player, amount) => custom.addProgress(playerOf(player), amount),
      learnRecipe: (player, id) => custom.learnRecipe(playerOf(player), id),
      forgetRecipe: (player, id) => custom.forgetRecipe(playerOf(player), id),
      knowsRecipe: (player, id) => custom.knowsRecipe(playerOf(player), id),
      learnedRecipes: (player) => custom.learnedRecipes(playerOf(player)),
    });
  }
}

module.exports = { CraftingSessions, parseStation, stationAt, STATIONS };
