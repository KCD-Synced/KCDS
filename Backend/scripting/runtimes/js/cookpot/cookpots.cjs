'use strict';

// Server-owned stew pots: CookPot. pots.json is built from a game installation by
// Dev/tools/cookpot/build_pots.py: every pot placed in the game's levels (by its fireplace's level
// GUID, the GUID the player's client reports a pot meal by), the game's three pot meals and the
// poisons a pot takes.
//
// A pot's stock lives per dimension, from its first use or write until the server stops: the
// meal it serves, the meals left, how many the last refill put in (the halfway point of its look),
// whether eating is allowed and the poison in it. A pot nobody touched serves the level's meal
// there, four meals of it, or none when the level starts it empty. KCDS's global world lists every
// catalogued pot from the start; another world lists the pots it used. A meal is reserved as the
// player's game asks to eat (playerInventories.consumePot) and spent once the game says it ate; a
// meal not eaten goes back. A pot whose meal differs from the game's answers "plain": the game's
// effects are left out and the server's meal's nutrition is added to the player's hunger instead.
//
// Every player's game shows the pots of their world as the server keeps them (op potLooks): each
// pot's cauldron in the game's prefab phase for its meal and fill, all of them as the player
// arrives in a world and each one as it changes.

const catalog = require('./pots.json');
const { lookupItemClass } = require('../inventory/catalog.cjs');

const FOODS = Object.freeze(Object.keys(catalog.foods).sort());
const FOOD_OF_ITEM = new Map(
  Object.entries(catalog.foods).map(([food, info]) => [info.item, food]),
);
const POTS = new Map(catalog.pots.map((pot) => [pot.guid, Object.freeze(pot)]));
const POISONS = new Map(Object.entries(catalog.poisons));
// Meals a pot is stocked with when nothing says otherwise, and the most a refill may put in.
const DEFAULT_PORTIONS = 4;
const PORTIONS_MAX = 1000;
// How near a player stands to a pot to refill or poison it, in metres.
const REACH = 4;
// KCDS's global world, where every entity starts; it is a stock world like any other.
const GLOBAL_WORLD = 0xffffffff;
// The cauldron's prefab phases (so_fireplace.xml): 1 empty, then each meal half full and full.
const EMPTY_PHASE = 1;
const MEAL_PHASES = Object.freeze({ goulash: [2, 3], lentil: [4, 5], soup: [6, 7] });
// Pot looks sent in one message.
const LOOKS_PER_MESSAGE = 512;
const GUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

function checkWorld(world, name) {
  if (!Number.isInteger(world) || world < 0 || world > GLOBAL_WORLD) {
    throw new TypeError(`CookPot.${name} takes a dimension, 0 to 4294967295`);
  }
}

function checkFood(food, name) {
  if (!FOODS.includes(food)) {
    throw new TypeError(`CookPot.${name} takes a meal: ${FOODS.join(', ')}`);
  }
}

// A pot nobody used yet in a world: the level's meal, four meals of it or none.
function untouched(guid, world) {
  const pot = POTS.get(guid);
  return {
    guid,
    world,
    food: pot.meal ?? null,
    portions: pot.full === false ? 0 : DEFAULT_PORTIONS,
    capacity: DEFAULT_PORTIONS,
    enabled: true,
    poison: null,
    // Whether the fire burns for every player of the world (CookPot.setFire);
    // null while each player's own game lights and puts it out.
    fireLit: null,
  };
}

// The look of a stock: empty, half (at most half of the last refill) or full.
function stateOf(stock) {
  if (stock.portions <= 0) {
    return 'empty';
  }
  return stock.portions * 2 <= stock.capacity ? 'half' : 'full';
}

// The cauldron's prefab phase for a stock: empty, or its meal half full or full.
function phaseOf(stock) {
  const state = stateOf(stock);
  if (state === 'empty') {
    return EMPTY_PHASE;
  }
  const [half, full] = MEAL_PHASES[stock.food ?? 'goulash'];
  return state === 'half' ? half : full;
}

// The fire a player's game is told: 0 its own, 1 lit, 2 put out.
function fireOf(stock) {
  if (stock.fireLit === null || stock.fireLit === undefined) {
    return 0;
  }
  return stock.fireLit ? 1 : 2;
}

// What a player's game is told of one pot: [cauldron, camp, phase, fireplace, fire]; the
// fireplace's kettle behaviour is told the same fill and the server's fire.
function lookEntry(pot, stock) {
  return [pot.cauldron, pot.camp === true, phaseOf(stock), pot.fireplace ?? '', fireOf(stock)];
}

class CookPots {
  constructor(host) {
    this.host = host;
    // `${world}/${guid}` -> { guid, world, food, portions, capacity, enabled, poison }
    this.stocks = new Map();
    // `${world}/${guid}` -> the phase the world's players were last shown
    this.shown = new Map();
    // player id -> the world whose pots their game shows
    this.shownWorld = new Map();
  }

  playersIn(world) {
    return [...this.host.players.values()].filter(
      (player) => player.ready && this.worldOf(player) === world,
    );
  }

  sendLooks(player, looks) {
    for (let start = 0; start < looks.length; start += LOOKS_PER_MESSAGE) {
      this.host.bridge.send({
        op: 'potLooks',
        id: player.id,
        looks: looks.slice(start, start + LOOKS_PER_MESSAGE),
      });
    }
  }

  // A stock changed: the players in its world see its pot's new look. `again` sends it even when
  // the look stayed: after a meal, so each game's kettle hears the server's fill again (its own
  // count of player meals would otherwise empty it, so_fireplace.xml onPlayerEating).
  showLook(stock, again = false) {
    const pot = POTS.get(stock.guid);
    if (!pot || !pot.cauldron) {
      return;
    }
    const key = `${stock.world}/${stock.guid}`;
    const look = `${phaseOf(stock)}/${fireOf(stock)}`;
    if (!again && this.shown.get(key) === look) {
      return;
    }
    this.shown.set(key, look);
    for (const player of this.playersIn(stock.world)) {
      this.sendLooks(player, [lookEntry(pot, stock)]);
    }
  }

  // Every pot of the player's world, to their game, whose world is ready (again).
  arrived(player) {
    const world = this.worldOf(player);
    this.shownWorld.set(player.id, world);
    const looks = [];
    for (const pot of POTS.values()) {
      if (pot.cauldron) {
        const stock = this.stocks.get(`${world}/${pot.guid}`) ?? untouched(pot.guid, world);
        looks.push(lookEntry(pot, stock));
      }
    }
    this.sendLooks(player, looks);
  }

  // A player who moved to another dimension sees its pots.
  worldChanged(player) {
    if (player.ready && this.shownWorld.get(player.id) !== this.worldOf(player)) {
      this.arrived(player);
    }
  }

  left(player) {
    this.shownWorld.delete(player.id);
  }

  // The stock of a catalogued pot in a world, made when asked to.
  stock(guid, world) {
    const key = `${world}/${guid}`;
    let stock = this.stocks.get(key);
    if (!stock) {
      stock = untouched(guid, world);
      this.stocks.set(key, stock);
    }
    return stock;
  }

  snapshot(stock) {
    const pot = POTS.get(stock.guid);
    return Object.freeze({
      food: stock.food,
      nutrition: stock.food === null ? null : catalog.foods[stock.food].nutrition,
      poison: stock.poison,
      guid: stock.guid,
      position: Object.freeze({ ...pot.position }),
      portions: stock.portions,
      capacity: stock.capacity,
      enabled: stock.enabled,
      state: stateOf(stock),
      fireLit: stock.fireLit ?? null,
      dimension: stock.world,
    });
  }

  // A player's game asks to eat from a pot (its fireplace GUID) in their world, serving the item
  // class `served`: null for a pot the server does not know (the game's own rules then), else
  // { ok: false } when it is disabled or empty, or { ok: true, stock, food, poison, native }
  // with a meal reserved. `native` says the game's meal is the pot's.
  reserve(guid, world, served) {
    if (!POTS.has(guid)) {
      return null;
    }
    const stock = this.stock(guid, world);
    if (!stock.enabled || stock.portions <= 0) {
      return { ok: false };
    }
    stock.portions -= 1;
    this.showLook(stock);
    return {
      ok: true,
      stock,
      food: stock.food,
      poison: stock.poison,
      // A meal neither the game nor the server names is the game's own.
      native: stock.food === null || FOOD_OF_ITEM.get(served) === stock.food,
    };
  }

  // The player's game ate the reserved meal or did not (it goes back then). A meal of the
  // server's own, not the game's, adds its nutrition.
  settle(player, reservation, used) {
    if (!reservation || !reservation.ok) {
      return;
    }
    if (!used) {
      reservation.stock.portions += 1;
      this.showLook(reservation.stock);
      return;
    }
    if (!reservation.native && player && typeof player.addStat === 'function') {
      player.addStat('hunger', catalog.foods[reservation.food].nutrition);
    }
    this.showLook(reservation.stock, true);
  }

  // Whether a player may work a pot now: connected, alive and able to act, in the pot's reach.
  // The code that refuses, or '' when they may.
  reachCode(player, pot) {
    if (!player || this.host.players.get(player.id) !== player) {
      return 'playerUnavailable';
    }
    if (player.ableToAct === false) {
      return 'cannotAct';
    }
    const at = player.position;
    if (
      !at ||
      Math.hypot(at.x - pot.position.x, at.y - pot.position.y, at.z - pot.position.z) > REACH
    ) {
      return 'tooFar';
    }
    return '';
  }

  // The world a player's pot is in.
  worldOf(player) {
    const world = player.dimension;
    return Number.isInteger(world) ? world : GLOBAL_WORLD;
  }

  define(playerOf) {
    const known = (guid, name) => {
      if (typeof guid !== 'string' || !GUID.test(guid)) {
        throw new TypeError(`CookPot.${name} takes a fireplace GUID`);
      }
      return POTS.get(guid.toLowerCase()) ?? null;
    };
    const result = (code) => Object.freeze({ ok: code === '', code });
    return Object.freeze({
      // The global world: every catalogued pot; another world: every pot used or written in it
      // (empty for a world nobody used yet).
      all: (dimension = GLOBAL_WORLD) => {
        checkWorld(dimension, 'all');
        if (dimension === GLOBAL_WORLD) {
          return Object.freeze(
            [...POTS.keys()].map((guid) =>
              this.snapshot(this.stocks.get(`${dimension}/${guid}`) ?? untouched(guid, dimension)),
            ),
          );
        }
        return Object.freeze(
          [...this.stocks.values()]
            .filter((stock) => stock.world === dimension)
            .map((stock) => this.snapshot(stock)),
        );
      },
      // A catalogued pot's stock in a world (four meals of a meal not known yet when nothing
      // used it), or null for a GUID the catalog does not have.
      get: (guid, dimension = GLOBAL_WORLD) => {
        checkWorld(dimension, 'get');
        const pot = known(guid, 'get');
        return pot ? this.snapshot(this.stock(pot.guid, dimension)) : null;
      },
      // Puts `portions` meals in (the new halfway point too) and clears the poison.
      refill: (guid, portions = DEFAULT_PORTIONS, dimension = GLOBAL_WORLD) => {
        checkWorld(dimension, 'refill');
        if (!Number.isInteger(portions) || portions < 0 || portions > PORTIONS_MAX) {
          throw new TypeError('CookPot.refill takes 0 to 1000 meals');
        }
        const pot = known(guid, 'refill');
        if (!pot) {
          return false;
        }
        const stock = this.stock(pot.guid, dimension);
        stock.portions = portions;
        stock.capacity = portions;
        stock.poison = null;
        this.showLook(stock);
        return true;
      },
      // Lights the pot's fire (true) or puts it out (false) for every player of the world, and
      // holds it so; null hands it back to each player's own game.
      setFire: (guid, lit, dimension = GLOBAL_WORLD) => {
        checkWorld(dimension, 'setFire');
        if (lit !== null && typeof lit !== 'boolean') {
          throw new TypeError('CookPot.setFire takes true, false or null');
        }
        const pot = known(guid, 'setFire');
        if (!pot) {
          return false;
        }
        const stock = this.stock(pot.guid, dimension);
        stock.fireLit = lit;
        this.showLook(stock);
        return true;
      },
      setEnabled: (guid, enabled, dimension = GLOBAL_WORLD) => {
        checkWorld(dimension, 'setEnabled');
        const pot = known(guid, 'setEnabled');
        if (!pot) {
          return false;
        }
        this.stock(pot.guid, dimension).enabled = Boolean(enabled);
        return true;
      },
      // Empty, two meals or four; the halfway point goes back to four.
      setState: (guid, state, dimension = GLOBAL_WORLD) => {
        checkWorld(dimension, 'setState');
        const portions = { empty: 0, half: 2, full: 4 }[state];
        if (portions === undefined) {
          throw new TypeError("CookPot.setState takes 'empty', 'half' or 'full'");
        }
        const pot = known(guid, 'setState');
        if (!pot) {
          return false;
        }
        const stock = this.stock(pot.guid, dimension);
        stock.portions = portions;
        stock.capacity = DEFAULT_PORTIONS;
        this.showLook(stock);
        return true;
      },
      setFood: (guid, food, dimension = GLOBAL_WORLD) => {
        checkWorld(dimension, 'setFood');
        checkFood(food, 'setFood');
        const pot = known(guid, 'setFood');
        if (!pot) {
          return false;
        }
        const stock = this.stock(pot.guid, dimension);
        stock.food = food;
        this.showLook(stock);
        return true;
      },
      poisons: () => Object.freeze([...POISONS.keys()].sort()),
      // Fills an empty, enabled pot from the player's inventory: the recipe's ingredients are
      // taken all or none, the meal and the meals it makes put in, an old poison cleared.
      topUpFrom: (player, guid, recipe) => {
        const checked = playerOf(player);
        const pot = known(guid, 'topUpFrom');
        if (!pot) {
          return result('unknownPot');
        }
        if (!recipe || typeof recipe !== 'object') {
          throw new TypeError('CookPot.topUpFrom takes a recipe');
        }
        checkFood(recipe.food, 'topUpFrom');
        if (
          !Number.isInteger(recipe.portions) ||
          recipe.portions < 1 ||
          recipe.portions > PORTIONS_MAX ||
          !Array.isArray(recipe.ingredients) ||
          recipe.ingredients.length === 0 ||
          !recipe.ingredients.every(
            (unit) =>
              unit &&
              lookupItemClass(unit.item) &&
              Number.isInteger(unit.amount) &&
              unit.amount >= 1,
          )
        ) {
          return result('invalidRecipe');
        }
        const reach = this.reachCode(checked, pot);
        if (reach) {
          return result(reach);
        }
        const stock = this.stock(pot.guid, this.worldOf(checked));
        if (!stock.enabled) {
          return result('disabled');
        }
        if (stock.portions > 0) {
          return result('notEmpty');
        }
        const ingredients = recipe.ingredients.map((unit) => ({
          item: lookupItemClass(unit.item).id,
          amount: unit.amount,
        }));
        const taken = this.host.customCrafting.take(checked, ingredients);
        if (!taken.ok) {
          return result(taken.reason === 'insufficientItems' ? 'insufficientItems' : taken.reason);
        }
        stock.food = recipe.food;
        stock.portions = recipe.portions;
        stock.capacity = recipe.portions;
        stock.poison = null;
        this.showLook(stock);
        return result('');
      },
      // Poisons a stocked, enabled pot with one unit of a poison row the player holds: future
      // meals give the poison's ingested buff. A second poison waits for a refill.
      poison: (player, guid, rowId) => {
        const checked = playerOf(player);
        const pot = known(guid, 'poison');
        if (!pot) {
          return result('unknownPot');
        }
        const reach = this.reachCode(checked, pot);
        if (reach) {
          return result(reach);
        }
        const stock = this.stock(pot.guid, this.worldOf(checked));
        if (!stock.enabled) {
          return result('disabled');
        }
        if (stock.portions <= 0) {
          return result('empty');
        }
        if (stock.poison) {
          return result('alreadyPoisoned');
        }
        let buff = null;
        const spent = this.host.playerInventories.operate(
          checked,
          (inventory) => {
            const row = inventory.rows.get(rowId);
            buff = row ? POISONS.get(row.itemClass.id) ?? null : null;
            if (!row || !buff || row.amount - row.equipped < 1) {
              return { result: { ok: false, code: 'invalidRow' }, change: null };
            }
            return inventory.remove({ units: [{ id: row.id, amount: 1 }] });
          },
          { notify: true },
        );
        if (!spent?.ok) {
          return result(spent?.code === 'invalidRow' ? 'invalidRow' : spent?.code ?? 'invalidRow');
        }
        stock.poison = buff;
        return result('');
      },
    });
  }
}

module.exports = { CookPots, FOODS, POTS };
