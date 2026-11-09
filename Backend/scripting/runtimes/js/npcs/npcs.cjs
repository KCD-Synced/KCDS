'use strict';

// Synced NPCs: the server API's Npc (phase 1). An NPC is a
// NetworkNpc entity the native server holds (Backend/integration/include/kcd2s/integration/npc.hpp).
// The server spawns it at Npc.create; the client of the nearest player
// within 150 m simulates its body and every other client draws what that one reports. A change of
// simulator comes here as npcSimulator and is raised as npcSimulatorChange(npc, player, previous).
//
// This host keeps each NPC's last reported position, rotation, velocity and dimension, so
// reads stay local, and what the resources set on it. A change goes to the native server at once
// and shows here before the server's next report confirms it.
//
// Phase 1: create, look up, rename, teleport, freeze (hold), change world, restrict visibility and
// remove. added roles (npcs/roles.cjs), the nameplate, locomotion, faction and pin.
//
// Orders: hold, moveTo, patrol, follow, flee, lookAt and attack. An order
// goes to the native server as the NPC's `intent` text (npc.hpp NpcIntent), which every client
// receives; the game's own movement and pathfinding on the simulating client carry it out, which
// comes with the native client side. Until a client reports progress, `status` reads `running`
// for an order that ends and `idle` otherwise. The server plans no routes here. A patrol is
// stepped by the server, one move order a leg, on the simulator's reports (npcs/patrols.cjs).
//
//
// Health is this host's: every body of the NPC shows the server's `health` and takes
// none of its own game's. A blow a player's game prices on an NPC (or a fall its simulator sees)
// comes here as npcHit; this host takes the health off, raises npcDamage and, at 0, npcDeath, and
// tells the server, whose `alive` makes every body die the game's own death.
//
// Inventory and loot are this host's too: each NPC holds an Inventory
// (getInventory) for as long as it lives. A dead NPC that is `lootable` (true by default) is a
// container at its body that players search (stash/stashes.cjs); the server moves what they take.
// What a resource dressed the NPC in (setOutfit) is part of that inventory: its pieces are rows
// worn whole, so a looter can take them, and units that leave by any path (a loot take, a script's
// remove or set) leave the outfit too and every body takes the piece off. An NPC whose resource
// never called setOutfit has no such rows: the clothes of its soul are the game's and not lootable.
// A dead animal is harvested once per life (npcs/harvest.cjs): a player's client reports the
// finished butchering, the server adds its own table's yield to the inventory, and npcHarvested
// follows. A press of the use key on an NPC whose `interactable` is on reaches the same module as
// npcInteract.

const { Quaternion, Vector3 } = require('../math.cjs');
const {
  classBytesHex,
  isGear,
  lookupItemClass,
  lookupOutfit,
  qualityBand,
} = require('../inventory/catalog.cjs');
const { Inventory, propsOf } = require('../inventory/inventories.cjs');
const { MAX_STASH_ROWS } = require('../stash/stashes.cjs');
const { damageOf } = require('../combat/player_damage.cjs');
const { AnimalPopulation } = require('../animals/population.cjs');
const {
  GLOBAL_WORLD,
  validWorld,
  soulOf,
  actorClassOf,
  nameOf,
  worldOf,
  positionProblem,
  positionOf,
  facingOrNull,
  facingOf,
  booleanOf,
  soulAppearance,
  npcAppearanceOf,
  animationRequestOf,
  speechOf,
  ATTACK_RANGE,
  moveOptionsOf,
  patrolOverridesOf,
  followOptionsOf,
  fleeOptionsOf,
  attackOptionsOf,
  placeOf,
  decimal,
  ANIMAL_CLASSES,
  KINEMATIC_CLASSES,
} = require('./npc_values.cjs');
const { NpcPatrols } = require('./patrols.cjs');
const { NpcHarvests } = require('./harvest.cjs');
const { roleOf, roleNames } = require('./roles.cjs');
const { cleanText } = require('../nameplates/player_nameplates.cjs');
const { onMap, worldOf: worldOnMap } = require('../maps/server_maps.cjs');

// Health an NPC starts with (kNpcMaxHealth).
const MAX_HEALTH = 100;
// The animals that turn on a player who locks on to them; the others flee.
const FIGHTING_ANIMALS = ['Dog', 'WildDog', 'Wolf', 'Boar'];

// A striker this recent (ms) is still in the fight when the NPC's target goes down.
const STRIKER_FIGHTING_MS = 15000;
// How long a bark's bubble shows: a base and a share per character, at most the cap.
const BARK_BASE_MS = 2500;
const BARK_MS_PER_CHARACTER = 60;
const BARK_MAX_MS = 8000;

// What npcIntentDone can say of an order that ended.
const FINISHED_STATUSES = ['reached', 'blocked', 'failed'];

// The appearance catalog's gender of an NPC class.
function genderOf(actorClass) {
  return actorClass === 'NPC_Female' ? 'female' : 'male';
}

// Npc.setOutfit: at most this many pieces (kNpcOutfitPiecesMax).
const OUTFIT_PIECES_MAX = 32;

// One piece of setOutfit: { item (the class id), quality, health }. Throws for an item that is not
// a class of the catalog worn or held, or a quality or health it cannot have.
function outfitPieceOf(given) {
  const request = typeof given === 'string' ? { item: given } : given;
  if (request === null || typeof request !== 'object') {
    throw new TypeError('setOutfit takes item names or ids, or { item, quality, health }');
  }
  const itemClass = lookupItemClass(request.item);
  if (!itemClass) {
    throw new RangeError(`setOutfit: no item class ${String(request.item)}`);
  }
  if (!isGear(itemClass)) {
    throw new RangeError(`setOutfit: ${itemClass.name} is not worn or held`);
  }
  const { quality = itemClass.maximumQuality, health = 1 } = request;
  const validQuality =
    Number.isInteger(quality) && quality >= 1 && quality <= itemClass.maximumQuality;
  if (!validQuality) {
    throw new RangeError(
      `setOutfit: ${itemClass.name} has a quality of 1 to ${itemClass.maximumQuality}`,
    );
  }
  if (typeof health !== 'number' || !(health >= 0 && health <= 1)) {
    throw new RangeError('setOutfit: health must be a number from 0 to 1');
  }
  return { item: itemClass.id, quality, health };
}

// The inventory rows of an outfit: its pieces of one class, quality and health together, as
// { itemClass, props, pieces }. A row's health is the piece's, kept within what its quality allows.
function outfitGroupsOf(outfit) {
  const groups = new Map();
  for (const piece of outfit) {
    const itemClass = lookupItemClass(piece.item);
    const band = qualityBand(itemClass, piece.quality);
    const health = Math.min(band.high, Math.max(band.low, piece.health));
    const key = `${piece.item}|${piece.quality}|${health}`;
    if (!groups.has(key)) {
      const { props } = propsOf(itemClass, { quality: piece.quality, health });
      groups.set(key, { itemClass, props, pieces: [] });
    }
    groups.get(key).pieces.push(piece);
  }
  return [...groups.values()];
}

// A health a resource gives: a number from 0 (above 0 when `positive`) to MAX_HEALTH.
function healthOf(value, what, positive = false) {
  const least = positive ? value > 0 : value >= 0;
  if (typeof value !== 'number' || !(least && value <= MAX_HEALTH)) {
    const from = positive ? 'above 0' : 'from 0';
    throw new RangeError(`${what} must be a number ${from} to ${MAX_HEALTH}`);
  }
  return value;
}

// The health of the ledger (the `health` setter and Npc.create's health): a number above the
// maximum is clamped to it.
function ledgerHealthOf(value, what, positive = false) {
  if (typeof value === 'number' && value > MAX_HEALTH) {
    return MAX_HEALTH;
  }
  return healthOf(value, what, positive);
}

// What setOutfit does for a game outfit preset: its pieces, as the player's outfit has them
// (quality 1, the health that quality allows); null for a preset the catalog does not carry, the
// other gender's, or one with more pieces than an outfit holds.
function presetPiecesOf(preset, gender) {
  const outfit = lookupOutfit(preset);
  if (!outfit || (outfit.gender !== 'any' && outfit.gender !== gender)) {
    return null;
  }
  if (outfit.items.length > OUTFIT_PIECES_MAX) {
    return null;
  }
  return outfit.items.map((itemClass) => ({
    item: itemClass.id,
    quality: 1,
    health: qualityBand(itemClass, 1).high,
  }));
}

// What wear does for a list of item classes: { pieces } for classes that are worn or held, or
// { refused } with every entry that is not one (a name or GUID the catalog does not have, a class
// that is no gear, anything but a string). Throws for what is no list or holds too many.
function wearPiecesOf(itemClasses, what) {
  if (!Array.isArray(itemClasses)) {
    throw new TypeError(`${what} takes an array of item class GUIDs`);
  }
  if (itemClasses.length > OUTFIT_PIECES_MAX) {
    throw new RangeError(`${what} takes at most ${OUTFIT_PIECES_MAX} item classes`);
  }
  const pieces = [];
  const refused = [];
  for (const given of itemClasses) {
    const itemClass = typeof given === 'string' ? lookupItemClass(given) : undefined;
    if (itemClass && isGear(itemClass)) {
      pieces.push(outfitPieceOf(itemClass.id));
    } else {
      refused.push(String(given));
    }
  }
  return { pieces, refused };
}

// How the simulating client moves the body: the game's own movement and walk
// animations, or the body put along the way without them.
const LOCOMOTIONS = ['native', 'kinematic'];
// The largest faction row a resource may name (the game's faction table is far smaller).
const FACTION_MAX = 65535;
const NAMEPLATE_WHITE = 0xffffffff;
// What this host holds of an NPC.
const npcState = Symbol('kcds.npcState');

function locomotionOf(value, what) {
  if (!LOCOMOTIONS.includes(value)) {
    throw new TypeError(`${what} must be 'native' or 'kinematic'`);
  }
  return value;
}

function factionOf(value, what) {
  if (!Number.isInteger(value) || value < 0 || value > FACTION_MAX) {
    throw new RangeError(`${what} must be a whole number from 0 to ${FACTION_MAX}`);
  }
  return value;
}

function copyVector(v) {
  return new Vector3(v.x, v.y, v.z);
}

// A place's coordinates as the plain decimal texts of an intent.
function coordinatesOf(place) {
  return [decimal(place.x), decimal(place.y), decimal(place.z)];
}

// Whether lookAt's target names an entity (a handle or a network id) rather than a position.
function namesEntity(target) {
  if (typeof target === 'number') {
    return true;
  }
  return target !== null && typeof target === 'object' && 'id' in target;
}

function copyQuaternion(q) {
  return new Quaternion(q.w, q.x, q.y, q.z);
}

const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');

// The game's loot of an animal's soul, as Npc.lootOf lists it: tools/animals build_animal_loot.py.
const ANIMAL_LOOT = require('./animal_loot.json');
const FOOD_CATEGORY = 'food';
// The game's trophies and special parts, which only a skilled hunter gets.
const HARVESTER_PREFIX = 'special_';

function animalLootOf(soul) {
  const entry = typeof soul === 'string' ? ANIMAL_LOOT[soul.toLowerCase()] : undefined;
  if (!entry) {
    return null;
  }
  return entry.items.map((row) => {
    const itemClass = lookupItemClass(row.name);
    return {
      item: itemClass ? itemClass.id : row.name,
      name: row.name,
      amount: row.amount,
      fraction: row.fraction,
      variation: row.variation,
      food: Boolean(itemClass) && itemClass.category === FOOD_CATEGORY,
      harvester: row.name.startsWith(HARVESTER_PREFIX),
    };
  });
}

// Npc.placements: Dev/tools/npcs/build_placements.py. Read when it is first asked for.
let placementCatalog = null;

function placementsOf(level) {
  if (placementCatalog === null) {
    const file = path.join(__dirname, 'placements.json.gz');
    placementCatalog = JSON.parse(zlib.gunzipSync(fs.readFileSync(file)).toString('utf8'));
  }
  const rows = placementCatalog[level] ?? [];
  return rows.map((row) => ({
    ...row,
    level,
    position: { ...row.position },
    rotation: { ...row.rotation },
  }));
}

// Npc.setSpeaking's styles, a player's too (animation/player_animation.cjs).
const TALK_STYLES = ['neutral', 'happy', 'drunk', 'chew'];
const FACE_TEXT_MAX = 64;

class Npcs {
  // `host`: the resource host (bridge, emit, context, resources, players, allocateEntityId,
  // bindEntityState, stateStore).
  constructor(host) {
    this.host = host;
    // id -> Npc
    this.npcs = new Map();
    // The animals the server spawns around the players (animals/population.cjs); it
    // hears of deaths, revivals and removals here, and the host starts and stops its tick.
    this.population = new AnimalPopulation(this);
    // The patrol routes and the stepping of the patrols NPCs walk.
    this.patrols = new NpcPatrols(this, npcState);
    // The harvest table and the client reports of harvests and use presses.
    this.harvests = new NpcHarvests(this, npcState);
  }

  // The Npc class of the server API, an Entity.
  define(Entity) {
    const registry = this;
    class Npc {
      constructor(id) {
        const npc = registry.npcs.get(id);
        if (!npc) {
          throw new Error('Unknown NPC');
        }
        return npc;
      }
      // Spawns an NPC: { soul?, class?, name?, position, rotation?, dimension?, frozen?,
      // appearance?, health?, maxHealth? (100 only), invulnerable?, lootable?, outfit? (a preset
      // as setOutfit takes it), wearing? (item classes as wear takes them) }. A new NPC is
      // vulnerable, lootable, at full health and wears its soul's own clothes unless told
      // otherwise. `soul` is a role from Npc.roles() or a soul GUID; `nameplate`, `locomotion`
      // and `faction` as the members of those names.
      static create(options) {
        return registry.create(Npc, options);
      }
      // The named kinds of NPC (guard, bandit, townswoman ...), each one real soul of the game's
      // tables.
      // The level's own people as it places them, by name, soul and place. Their first
      // places, not where a schedule has them; it spawns nothing.
      static placements() {
        return placementsOf(registry.host.areas?.level);
      }
      // One of the level's people for a script: the same one again gives the same NPC;
      // null if that GUID is not in the level.
      static adopt(placedGuid, options) {
        return registry.host.people.adopt(placedGuid, options);
      }
      // The level's people around the players: enabled, activationRadius, despawnRadius, maxPeople.
      static setPopulation(settings) {
        registry.host.people.set(settings);
      }
      static roles() {
        return roleNames();
      }
      static all(dimension) {
        const npcs = [...registry.npcs.values()];
        if (dimension === undefined) {
          return npcs;
        }
        const world = worldOf(dimension);
        return npcs.filter((npc) => npc[npcState].dimension === world);
      }
      static getById(id) {
        return registry.npcs.get(id) ?? null;
      }
      static removeAll(dimension) {
        registry.requireRunning();
        const world = dimension === undefined ? undefined : worldOf(dimension);
        let removed = 0;
        for (const npc of [...registry.npcs.values()]) {
          if (world === undefined || npc[npcState].dimension === world) {
            registry.destroy(npc);
            removed += 1;
          }
        }
        return removed;
      }
      // The server's animal population (a KCDS addition): the animals it spawns around
      // the players from the game's spawn points. Settings: { enabled, level, activationRadius,
      // despawnRadius, maxAnimals, kinds: { [kind]: { enabled, percent, respawnMinutes } } }, all
      // optional; a call changes only what it names. Returns the new settings (frozen).
      static setAnimalPopulation(config) {
        registry.requireRunning();
        return registry.population.configure(config);
      }
      // Switches the animal population on or off (the same as setAnimalPopulation({ enabled })).
      static setAnimalPopulationEnabled(enabled) {
        registry.requireRunning();
        return registry.population.setEnabled(enabled);
      }
      // Changes one animal kind of the population: a percent, or { enabled, percent,
      // respawnMinutes } of which only what is named changes. Returns the new settings.
      static setAnimalSpecies(kind, settings) {
        registry.requireRunning();
        return registry.population.setSpecies(kind, settings);
      }
      // The population's settings, a frozen copy.
      static get animalPopulation() {
        return registry.population.settings;
      }
      // The game's animal spawn points (copies of the entries of animals/spawn_points.json), those
      // of `level` ("trosecko" or "kutnohorsko") when one is given.
      static animalSpots(level) {
        return AnimalPopulation.spawnpoints(level);
      }
      // What a harvest of a dead animal yields (a KCDS addition): { [animal class]:
      // [{ item, amount }] } with the classes of Npc.animals() and items of Inventory.classes,
      // at most 16 rows of 1 to 100 units per class. It replaces the whole table; an animal whose
      // class is not in it cannot be harvested. Returns the new table. Empty until set.
      static setHarvestTable(table) {
        registry.requireRunning();
        return registry.harvests.configure(table);
      }
      // The harvest table, a plain copy.
      static get harvestTable() {
        return registry.harvests.tableCopy();
      }
      // The NPCs the population spawned, living and dead. They belong to no resource and stay when
      // a resource stops.
      static animals() {
        return registry.population.animals();
      }
      // The character components a resource chose: { body, head, hair, beard }, "" the soul's own.
      get appearance() {
        return Object.freeze({ ...this[npcState].appearance });
      }
      // Gives the NPC other character components (names of Appearances.options, of the NPC's
      // gender): a part left out keeps what it has, "" gives the soul's own back. Every player near
      // it sees the new look. False for a name the catalog does not have or a beard the face cannot
      // wear.
      setAppearance(request) {
        return registry.setAppearance(this, request);
      }
      // How well the NPC fights: { defense } is the share (0 to 1) of the attacks coming at it
      // that it parries (the game's perfect block): 1 every one, 0.5 about half, 0 none. null,
      // the default, leaves every defence to the game's own combat brain (its block, parry or
      // dodge as the NPC's skill gives). { damage } (a KCDS addition) is the share of their damage
      // its blows on players take, 0 to 10: 0.5 half, 2 double; null, the default, is the game's
      // own damage. A value left out keeps what it has. Throws for a value outside its range.
      setCombatSkill(skill) {
        return registry.setCombatSkill(this, skill);
      }
      // The NPC's combat skill: { defense, damage }.
      get combatSkill() {
        return { defense: this[npcState].defense, damage: this[npcState].damage };
      }
      // What a resource dressed and armed the NPC in (setOutfit): [{ item, quality, health }], or
      // null while the NPC wears its soul's own clothes. A piece that leaves the NPC's inventory
      // (a looter took it, a script removed it) is no longer in it.
      get outfit() {
        const outfit = this[npcState].outfit;
        return outfit ? outfit.map((piece) => ({ ...piece })) : null;
      }
      // The item classes the NPC wears, as the GUIDs of Inventory.classes, one per piece (wear,
      // setOutfit); empty while it wears its soul's own clothes or nothing.
      get wearing() {
        const outfit = this[npcState].outfit ?? [];
        return outfit.map((piece) => piece.item);
      }
      // Dresses the NPC in exactly these item classes (GUIDs, or names, of Inventory.classes that
      // are worn or held), at their best quality, in place of what an earlier wear or setOutfit
      // dressed it in; every other inventory row stays. True when every entry names such a class;
      // otherwise nothing changes, each refused entry is logged and false is returned. Throws for
      // what is no list or holds more than 32 entries, or when the inventory has no room.
      wear(itemClasses) {
        return registry.wear(this, itemClasses);
      }
      // Dresses the NPC in one of the game's own outfits (a name or GUID of Player.outfitSets):
      // true when the NPC's gender has one so named. A KCDS addition on top of it: a list of item
      // classes, each by name or id or { item, quality, health }, dresses and arms the NPC with
      // exactly these (clothing, armour, weapons, shields, ammunition); every body of it wears them
      // and shows its weapons where the game keeps them (a bow on the back, a sword at the belt).
      // The pieces are also rows of getInventory(), worn whole, in place of the rows an earlier
      // call put there; every other row stays. True once sent; throws for an item
      // it cannot use or when the inventory has no room for the rows.
      setOutfit(outfit) {
        return registry.setOutfit(this, outfit);
      }
      get soul() {
        return this[npcState].soul;
      }
      get actorClass() {
        return this[npcState].actorClass;
      }
      // A spawned NPC adopted no level body.
      get placedGuid() {
        return 0;
      }
      get name() {
        return this[npcState].name;
      }
      set name(value) {
        registry.rename(this, value);
      }
      get health() {
        return this[npcState].health;
      }
      // 0 kills the NPC (npcDeath without a killer); more than maxHealth is clamped to it. A dead
      // NPC comes back only with revive().
      set health(value) {
        registry.setHealth(this, ledgerHealthOf(value, 'Npc.health'));
      }
      get maxHealth() {
        return MAX_HEALTH;
      }
      get alive() {
        return this[npcState].alive;
      }
      // Blows, shots, falls and damage() take no health while it is set; kill() and setting
      // `health` still do.
      get invulnerable() {
        return this[npcState].invulnerable;
      }
      set invulnerable(value) {
        this[npcState].invulnerable = booleanOf(value, 'Npc.invulnerable');
      }
      // Whether a blow by a player or an NPC makes it attack the striker, as the game's own people
      // fight back (true by default for people, false for animals). A KCDS addition.
      get fightsBack() {
        return this[npcState].brain && this[npcState].fightsBack;
      }
      set fightsBack(value) {
        this[npcState].fightsBack = booleanOf(value, 'Npc.fightsBack');
      }
      // Takes `amount` health off the NPC as a scripted hit by `attacker` (a Player or a player's
      // id, where an id nobody has is no attacker; a KCDS addition: an Npc; or null):
      // npcDamage, and npcDeath when nothing is left. Returns false for a dead, invulnerable or gone NPC and true otherwise.
      damage(amount, attacker = null) {
        return registry.scriptedDamage(this, amount, attacker);
      }
      // Kills the NPC: every body of it dies the game's own death and lies (npcDeath with
      // `killer`, who is named as the attacker of damage is).
      // Returns false for a dead or gone NPC.
      kill(killer = null) {
        return registry.kill(this, killer);
      }
      // Revives a dead NPC with full or the given `health`: every body of it
      // is made anew where it lies (npcRevive). Returns
      // false for a living or gone NPC.
      revive(health = MAX_HEALTH) {
        return registry.revive(this, healthOf(health, 'Npc.revive health', true));
      }
      // What the NPC carries: an Inventory resources fill with its usual methods. It lives as long
      // as the NPC; a dead, lootable NPC's body shows it to players (a KCDS addition).
      getInventory() {
        return this[npcState].inventory;
      }
      // Whether players can search the NPC's body once it is dead (true by default). Setting it
      // while the NPC is dead puts the container there or takes it away.
      get lootable() {
        return this[npcState].lootable;
      }
      set lootable(value) {
        registry.setLootable(this, booleanOf(value, 'Npc.lootable'));
      }
      // Whether the carcass was harvested in this life (npcHarvested). Restore it with
      // getInventory().set({ items, harvested }); a revive clears it.
      get harvested() {
        return this[npcState].inventory.harvested;
      }
      // Whether a harvest report for this NPC would count now: it is a dead animal of a class in
      // the harvest table and was not harvested yet. A KCDS addition.
      get harvestable() {
        return registry.harvests.harvestable(this);
      }
      // Whether the game's own brain runs the NPC (true by default; a KCDS addition).
      // Off, its simulator's body does not think, perceive or fight on its own: only what the
      // server orders moves it, a blow takes no revenge (fightsBack reads false) and attack()
      // returns false. Animals have a brain too.
      get brain() {
        return this[npcState].brain;
      }
      set brain(value) {
        registry.setBrain(this, booleanOf(value, 'Npc.brain'));
      }
      // Whether a player can lock on to the NPC (true by default; a KCDS addition).
      // Off, the game's lock-on never offers its body, on every client. Blows still land unless
      // `invulnerable` is on.
      get targetable() {
        return this[npcState].targetable;
      }
      set targetable(value) {
        registry.setLockable(this, booleanOf(value, 'Npc.targetable'));
      }
      // False: players cannot lock on to it, and their blows, shots and horses' hits do nothing, so
      // neither the NPC nor anyone around reacts. It still talks, walks and trades.
      get attackable() {
        return this[npcState].attackable;
      }
      set attackable(value) {
        registry.setAttackable(this, booleanOf(value, 'Npc.attackable'));
      }
      // Whether clients report a press of the use key on the body as npcInteract (true by default).
      get interactable() {
        return this[npcState].interactable;
      }
      set interactable(value) {
        registry.setInteractable(this, booleanOf(value, 'Npc.interactable'));
      }
      get frozen() {
        return this[npcState].frozen;
      }
      set frozen(value) {
        registry.freeze(this, booleanOf(value, 'Npc.frozen'));
      }
      // Whether it shows a nameplate like a player (off by default).
      get nameplate() {
        return this[npcState].nameplate.visible;
      }
      set nameplate(value) {
        registry.changeNameplate(this, { visible: booleanOf(value, 'Npc.nameplate') });
      }
      isNameplateVisible() {
        return this[npcState].nameplate.visible;
      }
      setNameplateVisible(visible) {
        registry.changeNameplate(this, { visible: booleanOf(visible, 'Npc.setNameplateVisible') });
      }
      isNameplateHealthVisible() {
        return this[npcState].nameplate.health;
      }
      setNameplateHealthVisible(visible) {
        const health = booleanOf(visible, 'Npc.setNameplateHealthVisible');
        registry.changeNameplate(this, { health });
      }
      // The text drawn instead of its name, '' while the name is drawn.
      getNameplateText() {
        return this[npcState].nameplate.text;
      }
      setNameplateText(text = '') {
        if (typeof text !== 'string') {
          throw new TypeError('Npc.setNameplateText takes a string');
        }
        registry.changeNameplate(this, { text: cleanText(text) });
      }
      // The colour of its nameplate's text as 0xAARRGGBB.
      getNameplateColor() {
        return this[npcState].nameplate.color;
      }
      setNameplateColor(color) {
        if (!Number.isInteger(color) || color < 0 || color > 0xffffffff) {
          throw new RangeError('Npc.setNameplateColor takes a 0xAARRGGBB number');
        }
        registry.changeNameplate(this, { color });
      }
      // How the simulating client moves the body: 'native' (the game's movement and walk
      // animations, the default) or 'kinematic' (put along the way without walking).
      get locomotion() {
        return this[npcState].locomotion;
      }
      set locomotion(value) {
        registry.setLocomotion(this, locomotionOf(value, 'Npc.locomotion'));
      }
      // The mouth moves as if talking on every client until setSpeaking(false): a loop of
      // the game's own face clip, not lip-sync. True once sent; false for a dead NPC.
      setSpeaking(talking, style) {
        return registry.setSpeaking(this, talking, style);
      }
      // The game's loot candidates for this animal; it rolls nothing and fills no stock. Null for
      // anything without a preset of the game's.
      lootOf() {
        return animalLootOf(this.soul);
      }
      // A fragment on the face's own scope (FE_Default, FE_DialogueIdle, FE_DialogueSpeaking) with
      // `tags` (happy, angry, sad, nervous, pensive, arogant, drunk), on every client beside what the
      // body plays; '' gives the face back to the game. True once sent; false for a
      // dead NPC.
      setExpression(fragment, options) {
        return registry.setFace(this, fragment, options);
      }
      // The faction row the resource gave it, 0 for none. The body keeps its soul's
      // own faction in the game for now: this is the server's record.
      get faction() {
        return this[npcState].faction;
      }
      set faction(value) {
        registry.requireLive(this);
        this[npcState].faction = factionOf(value, 'Npc.faction');
      }
      // Lets one player's client run the NPC whatever the distances say, or the election again
      // with null. A pinned NPC goes dormant rather than to another player when that
      // player is out of range, and is unpinned when they leave.
      pin(player) {
        registry.pin(this, player);
      }
      // The player it is pinned to, or null.
      get pinnedTo() {
        return registry.host.players.get(this[npcState].pinnedId) ?? null;
      }
      // The player whose client simulates the NPC, or null while nobody near does.
      get simulator() {
        return registry.host.players.get(this[npcState].simulatorId) ?? null;
      }
      // How fast its body moves, as its simulator last reported (a KCDS addition).
      get velocity() {
        return copyVector(this[npcState].velocity);
      }
      get dimension() {
        return this[npcState].dimension;
      }
      // The map it stands on (World.maps): players on another map never see it.
      get map() {
        return registry.host.mapName(this[npcState].map ?? 0);
      }
      // Entity.position and rotation: assigning one teleports the NPC.
      get position() {
        return copyVector(this[npcState].position);
      }
      set position(value) {
        registry.place(this, value, undefined);
      }
      get rotation() {
        return copyQuaternion(this[npcState].rotation);
      }
      set rotation(value) {
        registry.place(this, undefined, value);
      }
      setDimension(world) {
        registry.moveToWorld(this, world);
      }
      showOnlyTo(player) {
        registry.host.setPlayerVisibleTo(this, player);
      }
      // Despawns it everywhere, after npcDestroy.
      remove() {
        registry.requireRunning();
        registry.destroy(this);
      }
      // Drops its order: the intent field empties and the body holds its pose (frozen) until
      // `frozen` is set false.
      hold() {
        registry.hold(this);
      }
      // What it has been told to do: `hold` (the default), `moveTo`, `follow`, `flee`, `lookAt` or
      // `attack`; a patrol reads `moveTo`, its legs being moves, and routeState tells it apart.
      // playAnim and talk are not reported yet.
      get intent() {
        return this[npcState].intent;
      }
      // The patrol it is on ({ routeId, waypointIndex, lap, direction, phase }, a copy), or null
      // while it is not patrolling. It shows the first waypoint until the simulator reports the
      // first leg done; updating a route does not change it.
      get routeState() {
        const patrol = this[npcState].patrol;
        return patrol ? registry.patrols.snapshot(patrol) : null;
      }
      // How the order stands: `idle` (nothing ordered, or an order that has no end),
      // `running` from the moment a move, patrol, follow, flee or attack is given, and the outcome
      // npcIntentDone announced (`reached`, `blocked`, `failed`) after that. The NPC's simulator
      // reports the outcome (NPC_INTENT_DONE), so it stays `running` until that arrives.
      get status() {
        return this[npcState].status;
      }
      // Walks to `position` (a component left out keeps the NPC's current one); options.speed
      // `walk` (default), `jog` or `run`, options.radius the arrival distance in metres (1.5),
      // options.pathfinding `auto` (default), `game` or `server`. The route is the simulating
      // client's to find; `server` is passed on and cannot be checked here. True when the order
      // went out; false for a gone or dead NPC or a destination it cannot use. Throws for an
      // option it cannot use.
      moveTo(position, options) {
        return registry.moveTo(this, position, options);
      }
      // Patrols an array of 1 to 256 positions, a PatrolRoute, or a route's id or name. The route
      // is copied, so editing it later does not change this patrol. options: speed, radius and
      // pathfinding as moveTo takes them, mode `once`, `loop` (default) or `pingPong`, or `loop`
      // as a boolean (not both), waitSeconds (0 to 3600) at each waypoint; they override a
      // route's own defaults, and a waypoint's own values override them. patrolWaypoint is raised
      // for each leg and patrolFinished when it ends. True once sent; false for an
      // empty or long route, a route that is gone or unknown, a waypoint it cannot use, a
      // waitSeconds that is not a number, or a gone or dead NPC. Throws for an option it cannot
      // use.
      patrol(route, options) {
        return registry.patrol(this, route, options);
      }
      // Has the NPC trail a Player, an Npc or a network id: options.speed (walk by default) and
      // options.radius (3 m by default). True once sent; false for a target that is
      // gone, the NPC itself, or a gone or dead NPC. Throws for a target of another type.
      follow(target, options) {
        return registry.follow(this, target, options);
      }
      // Moves away from a place: options.speed (run by default) and options.radius, the distance
      // to put between them (30 m by default). True when the order went out.
      flee(from, options) {
        return registry.flee(this, from, options);
      }
      // Turns towards a Player, an Npc, a network id or a position without moving. True when the
      // order went out.
      lookAt(target) {
        return registry.lookAt(this, target);
      }
      // Sits on a seat or lies on a bed of a placed SmartObject as the game's own people do:
      // walks to it and plays the game's own fragments, then npcObjectUseStarted; npcObjectUseEnded
      // when it stands up or something else ends it. False when no seat is free, the NPC is dead,
      // in another dimension or uses something already.
      useObject(object, options) {
        registry.requireLive(this);
        return registry.host.objectUses.use(this, object, options);
      }
      // Stands up from what it uses (or stops walking there). False when it uses nothing.
      stopUsingObject() {
        return registry.host.objectUses.stop(this);
      }
      // What it uses now: { object, place }, or null.
      get usingObject() {
        return registry.host.objectUses.current(this);
      }
      // Attacks a living Player or Npc that shares its visible world and stands within 128 m:
      // options.weapon `melee` (default) or `bow` (whether it holds a bow is the client's to find
      // out). True when accepted; false, leaving the previous order, for a target that is gone,
      // dead, out of reach or in another world. Throws for a target of another type or an option
      // it cannot use.
      attack(target, options) {
        return registry.attack(this, target, options);
      }
      // Puts it at `position` (missing components keep the current ones), facing `rotation` (Euler
      // degrees or a Quaternion; left out, it keeps its facing). False for a gone
      // NPC or a position or rotation it cannot use.
      teleport(position, rotation) {
        return registry.teleport(this, position, rotation);
      }
      // Plays a clip of the game's own animations on the NPC (`options.clip`, by its name or
      // path), or else the Mannequin fragment by its name with `options.tags`, looped with
      // `options.loop`; every player near it sees it. True when the request went out.
      playAnimation(fragment, options = {}) {
        return registry.animate(this, fragment, options);
      }
      // Ends what playAnimation started. True when the request went out.
      stopAnimation() {
        return registry.stopAnimation(this);
      }
      // Shows the text in the game's own speech bubble over the NPC's head on every client near it,
      // for `options.duration` milliseconds (by default longer for a longer text); "" takes the
      // bubble away. A KCDS addition. True when the request went out.
      say(text, options = {}) {
        return registry.say(this, text, options);
      }
      toString() {
        const name = this[npcState].name;
        return name ? `Npc(${this.id}, ${name})` : `Npc(${this.id})`;
      }
    }
    Object.setPrototypeOf(Npc.prototype, Entity.prototype);
    this.Npc = Npc;
    return Npc;
  }

  // A call from a stopped resource never reaches native.
  requireRunning() {
    const owner = this.host.context.getStore();
    if (
      owner &&
      (this.host.resources.get(owner.name) !== owner || !owner.active || owner.failing)
    ) {
      throw new Error('Resource is stopped');
    }
  }

  live(npc) {
    return Boolean(npc) && this.npcs.get(npc.id) === npc;
  }

  requireLive(npc) {
    this.requireRunning();
    if (!this.live(npc)) {
      throw new Error('Unknown NPC');
    }
  }

  create(Npc, options) {
    this.requireRunning();
    if (options === null || typeof options !== 'object') {
      throw new TypeError('Npc.create takes an options object');
    }
    if (options.maxHealth !== undefined && options.maxHealth !== MAX_HEALTH) {
      throw new Error(
        `Npc.create option "maxHealth" comes in a later KCDS phase (it is ${MAX_HEALTH} for now)`,
      );
    }
    // A role names its soul and the body it has, unless `class` says otherwise.
    const role = roleOf(options.soul);
    const soul = role ? role.soul : soulOf(options.soul);
    const actorClass =
      role && options.class === undefined ? role.actorClass : actorClassOf(options.class);
    let nameplateVisible = false;
    if (options.nameplate !== undefined) {
      nameplateVisible = booleanOf(options.nameplate, 'Npc.create nameplate');
    }
    let locomotion = KINEMATIC_CLASSES.includes(actorClass) ? 'kinematic' : 'native';
    if (options.locomotion !== undefined) {
      locomotion = locomotionOf(options.locomotion, 'Npc.create locomotion');
    }
    const faction =
      options.faction === undefined ? 0 : factionOf(options.faction, 'Npc.create faction');
    const name = nameOf(options.name);
    const position = positionOf(options.position, 'Npc.create');
    let facing = new Quaternion();
    if (options.rotation !== undefined) {
      facing = facingOf(options.rotation, 'Npc.create');
    }
    const dimension = worldOf(options.dimension);
    // The map it stands on (World.maps), the first by default.
    const map = this.host.mapIndexOf(options.map, 'Npc.create');
    let frozen = false;
    if (options.frozen !== undefined) {
      frozen = booleanOf(options.frozen, 'Npc.create frozen');
    }
    let appearance = soulAppearance();
    if (options.appearance !== undefined) {
      appearance = npcAppearanceOf(appearance, options.appearance, genderOf(actorClass));
      if (appearance === null) {
        throw new RangeError('Npc.create appearance names a part the catalog does not have');
      }
    }
    let health = MAX_HEALTH;
    if (options.health !== undefined) {
      health = ledgerHealthOf(options.health, 'Npc.create health', true);
    }
    let invulnerable = false;
    if (options.invulnerable !== undefined) {
      invulnerable = booleanOf(options.invulnerable, 'Npc.create invulnerable');
    }
    let lootable = true;
    if (options.lootable !== undefined) {
      lootable = booleanOf(options.lootable, 'Npc.create lootable');
    }
    let fightsBack = !ANIMAL_CLASSES.includes(actorClass);
    if (options.fightsBack !== undefined) {
      fightsBack = booleanOf(options.fightsBack, 'Npc.create fightsBack');
    }
    let brain = true;
    if (options.brain !== undefined) {
      brain = booleanOf(options.brain, 'Npc.create brain');
    }
    let attackable = true;
    if (options.attackable !== undefined) {
      attackable = booleanOf(options.attackable, 'Npc.create attackable');
    }
    let targetable = true;
    if (options.targetable !== undefined) {
      targetable = booleanOf(options.targetable, 'Npc.create targetable');
    }
    let interactable = true;
    if (options.interactable !== undefined) {
      interactable = booleanOf(options.interactable, 'Npc.create interactable');
    }
    const outfit = this.createOutfitOf(options, genderOf(actorClass));
    const npc = Object.create(Npc.prototype);
    Object.defineProperty(npc, 'id', { value: this.host.allocateEntityId(), enumerable: true });
    npc[npcState] = {
      soul,
      actorClass,
      name,
      position,
      rotation: facing,
      velocity: new Vector3(0, 0, 0),
      dimension,
      map,
      simulatorId: 0,
      // The simulator's handle as it was at its last change: npcSimulatorChange's `previous`.
      simulatorHandle: null,
      frozen,
      // The share of attacks it parries (setCombatSkill), null for the game's own defences.
      defense: null,
      // The share of their damage its blows on players take (setCombatSkill), null for the game's.
      damage: null,
      health,
      alive: true,
      invulnerable,
      lootable,
      interactable,
      brain,
      targetable,
      attackable,
      fightsBack,
      nameplate: {
        visible: nameplateVisible,
        health: true,
        text: '',
        color: NAMEPLATE_WHITE,
      },
      locomotion,
      faction,
      // The player whose client runs it whatever the distances (pin), 0 for the election.
      pinnedId: 0,
      // The last order (see the intent, status and routeState getters) and its sequence number
      // in the intent text; `patrol` is the run npcs/patrols.cjs steps, null when not patrolling.
      intent: 'hold',
      status: 'idle',
      patrol: null,
      intentSequence: 0,
      inventory: new Inventory({ maxRows: MAX_STASH_ROWS, harvestable: true }),
      // The inventory rows setOutfit put there: row id -> the outfit pieces they still hold.
      outfitRows: new Map(),
      appearance,
      resource: this.host.context.getStore()?.name,
    };
    npc[npcState].inventory.onChange = (change) => {
      this.outfitRowsChanged(npc);
      // The stock of an NPC that is not (yet) spawned has no listeners to tell.
      if (this.live(npc)) {
        this.host.emit('npcInventoryChanged', npc, change);
      }
    };
    if (outfit !== null) {
      if (!this.dress(npc, outfit)) {
        throw new RangeError("Npc.create: the NPC's inventory has no room for the outfit");
      }
      npc[npcState].outfit = outfit;
    }
    this.npcs.set(npc.id, npc);
    this.host.bindEntityState(npc);
    this.host.areas.touch('npc', npc, position, dimension, map);
    this.host.bridge.send({
      op: 'npcSpawn',
      id: npc.id,
      position: { x: position.x, y: position.y, z: position.z },
      rotation: { x: facing.x, y: facing.y, z: facing.z, w: facing.w },
      soul,
      actorClass,
      name,
      dimension: onMap(map, dimension),
      frozen,
    });
    if (options.appearance !== undefined) {
      this.sendAppearance(npc);
    }
    if (!interactable) {
      this.sendInteractable(npc);
    }
    if (!brain) {
      this.sendBrain(npc);
    }
    if (!targetable) {
      this.sendTargetable(npc);
    }
    if (!attackable) {
      this.sendAttackable(npc);
      this.sendTargetable(npc);
    }
    if (nameplateVisible) {
      this.sendNameplate(npc);
    }
    if (locomotion !== 'native') {
      this.sendLocomotion(npc);
    }
    if (health !== MAX_HEALTH) {
      this.sendHealth(npc);
    }
    if (outfit !== null) {
      this.sendOutfit(npc);
    }
    this.host.emit('npcSpawn', npc);
    return npc;
  }

  // The pieces Npc.create's `outfit` (a preset) and `wearing` (item classes) dress the new NPC in,
  // `wearing` after `outfit` and so in place of it; null when neither is given. Throws, before
  // anything spawns, for what the NPC could not wear.
  createOutfitOf(options, gender) {
    let pieces = null;
    if (options.outfit !== undefined) {
      if (typeof options.outfit !== 'string') {
        throw new TypeError('Npc.create outfit must be an outfit preset name or GUID');
      }
      pieces = presetPiecesOf(options.outfit, gender);
      if (pieces === null) {
        throw new RangeError(`Npc.create outfit ${options.outfit} is no outfit for this NPC`);
      }
    }
    if (options.wearing !== undefined) {
      const wearing = wearPiecesOf(options.wearing, 'Npc.create wearing');
      if (wearing.refused.length > 0) {
        const names = wearing.refused.join(', ');
        throw new RangeError(`Npc.create wearing names no item class worn or held: ${names}`);
      }
      pieces = wearing.pieces;
    }
    return pieces;
  }

  sendHealth(npc) {
    const { health, alive } = npc[npcState];
    this.host.bridge.send({ op: 'npcHealth', id: npc.id, health, alive });
  }

  // A Player or an Npc that dealt a blow, or null; a number is a player's id, and an id no player
  // has is no attacker. Throws for anything else.
  attackerOf(given, what) {
    if (given === null || given === undefined) {
      return null;
    }
    if (typeof given === 'number') {
      return this.host.players.get(given) ?? null;
    }
    const player = this.host.players.get(given?.id);
    if (player === given || this.live(given)) {
      return given;
    }
    throw new TypeError(`${what} must be a Player, a player id, an Npc or null`);
  }

  // Npc.health: 0 kills; a dead NPC keeps lying.
  setHealth(npc, health) {
    this.requireLive(npc);
    const state = npc[npcState];
    if (!state.alive) {
      return;
    }
    if (health === 0) {
      this.die(npc, null);
      return;
    }
    state.health = health;
    this.sendHealth(npc);
  }

  scriptedDamage(npc, amount, attacker) {
    this.requireRunning();
    if (typeof amount !== 'number' || !(amount > 0) || !Number.isFinite(amount)) {
      throw new RangeError('Npc.damage amount must be a number above 0');
    }
    const by = this.attackerOf(attacker, 'Npc.damage attacker');
    if (!this.live(npc) || npc[npcState].invulnerable) {
      return false;
    }
    return this.takeHealth(npc, by, amount, null, 'scripted');
  }

  // npcEngage: a player locked on to the NPC. The game's lock-on takes only a body
  // that fights, and an animal that is not hunting never fights a player: a dog stood untouched,
  // its blows went over it. An animal of a kind that fights (FIGHTING_ANIMALS) with a brain attacks
  // the player, unless it is the player's own dog or attacks someone already.
  engaged(report) {
    const npc = this.npcs.get(report.id);
    const player = this.host.players.get(report.playerId);
    if (!npc || !player) {
      return false;
    }
    const state = npc[npcState];
    if (!state.alive || !state.brain || !FIGHTING_ANIMALS.includes(npc.actorClass)) {
      return false;
    }
    if (this.host.dogs?.dogs.get(npc.id)?.ownerId === player.id || state.intent === 'attack') {
      return false;
    }
    return this.host.context.exit(() => this.attack(npc, player));
  }

  // npcHit: a blow, shot or fall a client's game would have taken off the NPC's body. The
  // resources' npcHit event sees it first: false refuses it, and hit.damage sets the health it
  // takes (0 takes none).
  hit(report) {
    const npc = this.npcs.get(report.id);
    const damage = damageOf(report);
    if (!npc || !damage || npc[npcState].invulnerable) {
      return;
    }
    const attacker = this.host.players.get(report.attackerId) ?? this.npcs.get(report.attackerId);
    const hit = { damage: damage.amount, bodyPart: damage.bodyPart, reason: damage.reason };
    if (!this.host.emit('npcHit', npc, attacker ?? null, hit) || !this.live(npc)) {
      return;
    }
    const amount = Number.isFinite(hit.damage) && hit.damage >= 0 ? hit.damage : damage.amount;
    if (amount === 0) {
      return;
    }
    this.takeHealth(npc, attacker ?? null, amount, damage.bodyPart, damage.reason);
  }

  // npcDamage(npc, attacker, taken, bodyPart, reason), as playerDamage, then npcDeath with the
  // attacker as killer when no health is left. `taken` is what the ledger lost, which is less than
  // the blow when the NPC had less health. A handler may change the NPC's health first.
  takeHealth(npc, attacker, amount, bodyPart, reason) {
    const state = npc[npcState];
    if (!state.alive) {
      return false;
    }
    const taken = Math.min(amount, state.health);
    state.health -= taken;
    const sequence = state.intentSequence;
    // Who struck it in a fight, and when: they are still fighting it when its target goes down.
    if (reason === 'combat' && attacker !== null && attacker !== npc) {
      if (!state.strikers) {
        state.strikers = new Map();
      }
      state.strikers.set(attacker, Date.now());
    }
    this.host.emit('npcDamage', npc, attacker, taken, bodyPart, reason);
    if (!this.live(npc) || !state.alive) {
      return true;
    }
    if (state.health === 0) {
      this.die(npc, attacker);
    } else {
      this.sendHealth(npc);
      // An npcDamage handler's own order stands.
      if (state.intentSequence === sequence) {
        this.fightBack(npc, attacker, reason);
      }
    }
    return true;
  }

  // A blow in a fight makes an NPC that fights back attack the striker, unless it is
  // attacking already; the game's AI takes no hostility from a blow the server keeps.
  fightBack(npc, attacker, reason) {
    const state = npc[npcState];
    if (
      !state.brain ||
      !state.fightsBack ||
      reason !== 'combat' ||
      attacker === null ||
      attacker === npc
    ) {
      return;
    }
    if (state.intent === 'attack' && state.status === 'running') {
      return;
    }
    this.attack(npc, attacker);
  }

  kill(npc, killer) {
    this.requireRunning();
    const by = this.attackerOf(killer, 'Npc.kill killer');
    if (!this.live(npc) || !npc[npcState].alive) {
      return false;
    }
    this.die(npc, by);
    return true;
  }

  die(npc, killer) {
    const state = npc[npcState];
    state.health = 0;
    state.alive = false;
    this.patrols.cancel(npc);
    this.sendHealth(npc);
    this.sendCarcassClosed(npc);
    this.population.animalDied(npc);
    // The NPCs fighting it drop it.
    this.targetDown(npc);
    this.host.emit('npcDeath', npc, killer);
    // A conversation tied to the NPC ends with it (Dialogue), and its shop.
    this.host.dialogues?.npcGone(npc);
    this.host.vendors?.npcGone(npc);
    // An npcDeath handler may have revived or removed the NPC or made it not lootable already.
    this.host.stashes.npcCorpse(npc);
  }

  // Npc.lootable: a dead NPC's body is a container for players to search while it is set.
  setLootable(npc, lootable) {
    this.requireLive(npc);
    npc[npcState].lootable = lootable;
    this.host.stashes.npcCorpse(npc);
    this.sendCarcassClosed(npc);
  }

  // Whether every game takes the game's Search and Gut off a dead animal: it is not
  // lootable and has nothing left to harvest. { op: 'npcCarcassClosed', id, closed }.
  sendCarcassClosed(npc) {
    const state = npc[npcState];
    const closed = !state.alive && !state.lootable && !this.harvests.harvestable(npc);
    if ((state.carcassClosed ?? false) !== closed) {
      state.carcassClosed = closed;
      this.host.bridge.send({ op: 'npcCarcassClosed', id: npc.id, closed });
    }
  }

  revive(npc, health) {
    this.requireRunning();
    if (!this.live(npc) || npc[npcState].alive) {
      return false;
    }
    const state = npc[npcState];
    state.alive = true;
    state.health = health;
    // The next death is a new life: the carcass can be harvested again.
    state.inventory.harvested = false;
    this.sendCarcassClosed(npc);
    this.population.animalRevived(npc);
    this.host.stashes.npcCorpse(npc);
    this.sendHealth(npc);
    this.host.emit('npcRevive', npc);
    return true;
  }

  // Npc.setAppearance: "<body>|<head>|<hair>|<beard>" in the NPC's `appearance` field (npc.hpp).
  setAppearance(npc, request) {
    this.requireRunning();
    const state = npc[npcState];
    const next = npcAppearanceOf(state.appearance, request, genderOf(state.actorClass));
    if (next === null || !this.live(npc)) {
      return false;
    }
    state.appearance = next;
    this.sendAppearance(npc);
    return true;
  }

  // Npc.setOutfit: the pieces become worn rows of the NPC's inventory and go to the
  // native server, which writes the NPC's outfit field.
  // A string is a game outfit preset (false for one the NPC's gender has none so named), an array
  // the KCDS list of items.
  setOutfit(npc, given) {
    this.requireRunning();
    if (typeof given === 'string') {
      const gender = genderOf(npc[npcState].actorClass);
      const pieces = presetPiecesOf(given, gender);
      if (pieces === null || !this.live(npc)) {
        return false;
      }
      return this.wearPieces(npc, pieces, 'setOutfit');
    }
    if (!Array.isArray(given)) {
      throw new TypeError('setOutfit takes an outfit preset or an array of items');
    }
    if (given.length > OUTFIT_PIECES_MAX) {
      throw new RangeError(`setOutfit takes at most ${OUTFIT_PIECES_MAX} items`);
    }
    const outfit = given.map((item) => outfitPieceOf(item));
    if (!this.live(npc)) {
      return false;
    }
    return this.wearPieces(npc, outfit, 'setOutfit');
  }

  // Npc.wear: setOutfit's pieces for item classes of the best quality. Nothing changes, and each
  // refused entry is logged, when one is no class worn or held.
  wear(npc, itemClasses) {
    this.requireRunning();
    const { pieces, refused } = wearPiecesOf(itemClasses, 'wear');
    if (refused.length > 0) {
      const owner = this.host.context.getStore()?.name ?? 'host';
      for (const given of refused) {
        this.host.bridge.log(owner, `Npc.wear: no item class worn or held named ${given}`);
      }
      return false;
    }
    if (!this.live(npc)) {
      return false;
    }
    return this.wearPieces(npc, pieces, 'wear');
  }

  // Makes the pieces the NPC's outfit: worn rows of its inventory in place of the earlier ones, and
  // the outfit field of the native server. Throws when the rows do not fit.
  wearPieces(npc, pieces, what) {
    if (!this.dress(npc, pieces)) {
      throw new RangeError(`${what}: the NPC's inventory has no room for the outfit`);
    }
    npc[npcState].outfit = pieces;
    this.sendOutfit(npc);
    return true;
  }

  // Puts the outfit's pieces in the NPC's inventory as worn rows, in place of the rows an earlier
  // setOutfit put there; every other row stays. False, and nothing changes, when they do not fit.
  dress(npc, outfit) {
    const state = npc[npcState];
    const groups = outfitGroupsOf(outfit);
    const leaving = [...state.outfitRows].map(([id, pieces]) => ({ id, amount: pieces.length }));
    const placing = groups.map(({ itemClass, props, pieces }) => ({
      itemClass,
      props,
      amount: pieces.length,
    }));
    // No row is watched while they change, or the change hook would take the old ones for a loss.
    const earlier = state.outfitRows;
    state.outfitRows = new Map();
    const { result } = state.inventory.dressWorn(leaving, placing);
    if (!result.ok) {
      state.outfitRows = earlier;
      return false;
    }
    groups.forEach(({ pieces }, index) => {
      state.outfitRows.set(result.items[index].id, pieces);
    });
    return true;
  }

  // The NPC's inventory changed (its change hook): units of a row setOutfit put there that are gone
  // leave the outfit, and the rest of it goes out again so every body takes the pieces off.
  outfitRowsChanged(npc) {
    const state = npc[npcState];
    const lost = [];
    for (const [id, pieces] of state.outfitRows) {
      const row = state.inventory.rows.get(id);
      const same = row && row.itemClass.id === pieces[0].item;
      const left = same ? Math.min(pieces.length, row.amount) : 0;
      lost.push(...pieces.splice(left));
      if (left === 0) {
        state.outfitRows.delete(id);
      }
    }
    if (lost.length === 0 || !this.live(npc)) {
      return;
    }
    state.outfit = state.outfit.filter((piece) => !lost.includes(piece));
    this.sendOutfit(npc);
  }

  sendOutfit(npc) {
    const pieces = npc[npcState].outfit.map((piece) => ({
      itemClass: classBytesHex(lookupItemClass(piece.item)),
      quality: piece.quality,
      health: piece.health,
    }));
    this.host.bridge.send({ op: 'npcOutfit', id: npc.id, pieces });
  }

  sendAppearance(npc) {
    const { body, head, hair, beard } = npc[npcState].appearance;
    const appearance = `${body}|${head}|${hair}|${beard}`;
    this.host.bridge.send({ op: 'npcAppearance', id: npc.id, appearance });
  }

  // Raises npcDestroy while the handle still resolves, then despawns the NPC.
  destroy(npc) {
    if (!this.live(npc)) {
      return;
    }
    // A dog (dogs/dogs.cjs) raises dogDestroy first, while the handles still resolve.
    this.host.dogs.npcDestroying(npc);
    this.patrols.cancel(npc);
    this.host.emit('npcDestroy', npc);
    this.host.dialogues?.npcGone(npc);
    this.host.vendors?.npcGone(npc);
    clearTimeout(npc[npcState].speechTimer);
    this.npcs.delete(npc.id);
    this.host.areas.forget(npc.id);
    this.population.animalGone(npc);
    this.host.stashes.npcCorpse(npc);
    this.host.stateStore.destroy(npc.id);
    this.host.bridge.send({ op: 'npcDestroy', id: npc.id });
  }

  rename(npc, value) {
    this.requireLive(npc);
    const name = nameOf(value);
    npc[npcState].name = name;
    this.host.bridge.send({ op: 'npcName', id: npc.id, name });
  }

  // The level's own person the NPC is (people.cjs, server.json levelPeople): { op:
  // 'npcLevelGuid', id, guid }, the GUID as the placement catalog writes it. Every game then uses
  // the level's own entity of that person as the body. Not part of the scripts' API.
  setLevelGuid(npc, guid) {
    this.requireLive(npc);
    if (typeof guid !== 'string' || !/^[0-9a-f]{16}$/.test(guid)) {
      throw new TypeError('a level GUID is 16 lowercase hex digits');
    }
    npc[npcState].levelGuid = guid;
    this.host.bridge.send({ op: 'npcLevelGuid', id: npc.id, guid });
  }

  // Npc.interactable: clients watch the use key against the body only while it is set.
  setInteractable(npc, interactable) {
    this.requireLive(npc);
    npc[npcState].interactable = interactable;
    this.sendInteractable(npc);
  }

  // Npc.brain: { op: 'npcBrain', id, brain }.
  setBrain(npc, brain) {
    this.requireLive(npc);
    if (npc[npcState].brain !== brain) {
      npc[npcState].brain = brain;
      this.sendBrain(npc);
    }
  }

  sendBrain(npc) {
    this.host.bridge.send({ op: 'npcBrain', id: npc.id, brain: npc[npcState].brain });
  }

  // Npc.targetable: { op: 'npcTargetable', id, targetable }.
  setLockable(npc, targetable) {
    this.requireLive(npc);
    if (npc[npcState].targetable !== targetable) {
      npc[npcState].targetable = targetable;
      this.sendTargetable(npc);
    }
  }

  setFace(npc, fragment, options) {
    this.requireLive(npc);
    if (typeof fragment !== 'string' || fragment.length > FACE_TEXT_MAX || fragment.includes('|')) {
      throw new TypeError('Npc.setExpression fragment must be a short string');
    }
    if (options !== undefined && (options === null || typeof options !== 'object')) {
      throw new TypeError('Npc.setExpression options must be an object');
    }
    const tags = options?.tags ?? '';
    if (typeof tags !== 'string' || tags.length > FACE_TEXT_MAX) {
      throw new TypeError('Npc.setExpression tags must be a short string');
    }
    if (!npc.alive) {
      return false;
    }
    const face = fragment === '' ? '' : `${fragment}|${tags}`;
    this.host.bridge.send({ op: 'npcFace', id: npc.id, face });
    return true;
  }

  setSpeaking(npc, talking, style = 'neutral') {
    this.requireLive(npc);
    if (typeof talking !== 'boolean') {
      throw new TypeError('Npc.setSpeaking talking must be true or false');
    }
    if (!TALK_STYLES.includes(style)) {
      throw new TypeError(`Npc.setSpeaking style must be one of ${TALK_STYLES.join(', ')}`);
    }
    if (!npc.alive) {
      return false;
    }
    this.host.bridge.send({ op: 'npcTalking', id: npc.id, style: talking ? style : '' });
    return true;
  }

  sendTargetable(npc) {
    this.host.bridge.send({
      op: 'npcTargetable',
      id: npc.id,
      targetable: npc[npcState].targetable && npc[npcState].attackable,
    });
  }

  // Npc.attackable: { op: 'npcAttackable', id, attackable }; it takes the lock-on with it.
  setAttackable(npc, attackable) {
    this.requireLive(npc);
    if (npc[npcState].attackable !== attackable) {
      npc[npcState].attackable = attackable;
      this.sendAttackable(npc);
      this.sendTargetable(npc);
    }
  }

  sendAttackable(npc) {
    this.host.bridge.send({
      op: 'npcAttackable',
      id: npc.id,
      attackable: npc[npcState].attackable,
    });
  }

  sendInteractable(npc) {
    const { interactable } = npc[npcState];
    this.host.bridge.send({ op: 'npcInteractable', id: npc.id, interactable });
  }

  // Npc.nameplate and its setters: the tag as every client draws it.
  changeNameplate(npc, values) {
    this.requireLive(npc);
    const tag = npc[npcState].nameplate;
    const before = JSON.stringify(tag);
    Object.assign(tag, values);
    if (JSON.stringify(tag) !== before) {
      this.sendNameplate(npc);
    }
  }

  // { op: 'npcNameplate', id, field }: "" for no tag, else the player tag's form
  // (kcd2s/integration/nameplates.hpp) "1<health 0|1>;<AARRGGBB>;<text>", the text drawn instead
  // of the NPC's name ("" draws the name).
  sendNameplate(npc) {
    const tag = npc[npcState].nameplate;
    let field = '';
    if (tag.visible) {
      const color = tag.color.toString(16).toUpperCase().padStart(8, '0');
      field = `1${tag.health ? 1 : 0};${color};${tag.text}`;
    }
    this.host.bridge.send({ op: 'npcNameplate', id: npc.id, field });
  }

  setLocomotion(npc, locomotion) {
    this.requireLive(npc);
    if (npc[npcState].locomotion !== locomotion) {
      npc[npcState].locomotion = locomotion;
      this.sendLocomotion(npc);
    }
  }

  // { op: 'npcLocomotion', id, kinematic }.
  sendLocomotion(npc) {
    const kinematic = npc[npcState].locomotion === 'kinematic';
    this.host.bridge.send({ op: 'npcLocomotion', id: npc.id, kinematic });
  }

  // Npc.pin: { op: 'npcPin', id, player } with the player's network id, 0 for the election.
  pin(npc, player) {
    this.requireLive(npc);
    let id = 0;
    if (player !== null && player !== undefined) {
      const found = typeof player === 'number' ? this.host.players.get(player) : player;
      if (!found || this.host.players.get(found.id) !== found) {
        throw new TypeError('Npc.pin takes a Player the server holds, a player id or null');
      }
      id = found.id;
    }
    npc[npcState].pinnedId = id;
    this.host.bridge.send({ op: 'npcPin', id: npc.id, player: id });
  }

  // A player left: the NPCs pinned to them go back to the election (the native server unpins
  // them too).
  playerLeft(id) {
    for (const npc of this.npcs.values()) {
      if (npc[npcState].pinnedId === id) {
        npc[npcState].pinnedId = 0;
      }
    }
  }

  freeze(npc, frozen) {
    this.requireLive(npc);
    npc[npcState].frozen = frozen;
    this.host.bridge.send({ op: 'npcFrozen', id: npc.id, frozen });
  }

  // Npc.setCombatSkill: the NPC's `defense` field (npc.hpp NpcDefense).
  setCombatSkill(npc, skill) {
    this.requireLive(npc);
    if (skill === null || typeof skill !== 'object') {
      throw new TypeError('setCombatSkill takes an object such as { defense: 0.5 }');
    }
    const { defense, damage } = skill;
    if (damage === null) {
      // The game's own damage again (npc.hpp kNpcDamageGame).
      npc[npcState].damage = null;
      this.host.bridge.send({ op: 'npcDamage', id: npc.id, damage: 1 });
    } else if (damage !== undefined) {
      if (typeof damage !== 'number' || !Number.isFinite(damage) || damage < 0 || damage > 10) {
        throw new RangeError('damage must be a number from 0 to 10, or null');
      }
      npc[npcState].damage = damage;
      this.host.bridge.send({ op: 'npcDamage', id: npc.id, damage });
    }
    if (defense === null) {
      // The game's own defences again (npc.hpp kNpcDefenseGame).
      npc[npcState].defense = null;
      this.host.bridge.send({ op: 'npcDefense', id: npc.id, defense: -1 });
    } else if (defense !== undefined) {
      if (typeof defense !== 'number' || !Number.isFinite(defense) || defense < 0 || defense > 1) {
        throw new RangeError('defense must be a number from 0 to 1, or null');
      }
      npc[npcState].defense = defense;
      this.host.bridge.send({ op: 'npcDefense', id: npc.id, defense });
    }
    return true;
  }

  // Npc.hold: no order any more (the intent field empties), and the body holds its pose.
  hold(npc) {
    this.requireLive(npc);
    const state = npc[npcState];
    // A report of an earlier order is stale from now on.
    state.intentSequence += 1;
    state.intent = 'hold';
    state.status = 'idle';
    const patrol = this.patrols.release(state);
    this.host.bridge.send({ op: 'npcIntent', id: npc.id, intent: '' });
    // Frozen by the hold, not by the resource: the next order frees it again (giveOrder).
    if (!npc.frozen) {
      this.freeze(npc, true);
      state.heldFrozen = true;
    }
    this.patrols.cancelled(npc, patrol);
  }

  // Whether an order can go to the NPC: it is here and alive.
  orderable(npc) {
    return this.live(npc) && npc[npcState].alive;
  }

  // Sends an order as "<sequence>|<kind>|<arguments>" in the NPC's `intent` field (npc.hpp
  // NpcIntent), the sequence one up per order so that a client takes each only once. `shown` is
  // what `intent` reads afterwards. An order that `ends` reads `running` until a client reports
  // its end (intentDone); one that does not stays `idle`. An order other than the next leg of the
  // NPC's own patrol (`patrol`) ends the patrol it was on, which is announced once the new order
  // stands.
  giveOrder(npc, kind, args, { shown = kind, ends = true, patrol = null } = {}) {
    // A script's order ends what the NPC uses (Npc.useObject), unless it is the use's own walk.
    this.host.objectUses?.ordered(npc);
    const state = npc[npcState];
    let replaced = null;
    if (state.patrol !== patrol) {
      replaced = this.patrols.release(state);
    }
    state.patrol = patrol;
    state.intentSequence += 1;
    state.intent = shown;
    state.status = ends ? 'running' : 'idle';
    const intent = [state.intentSequence, kind, ...args].join('|');
    // A hold froze the body; any other order needs it free to move (a resource's own freeze
    // stays).
    if (state.heldFrozen) {
      state.heldFrozen = false;
      if (npc.frozen) {
        this.freeze(npc, false);
      }
    }
    this.host.bridge.send({ op: 'npcIntent', id: npc.id, intent });
    this.patrols.cancelled(npc, replaced);
    return true;
  }

  // The Player or Npc an order names, as a handle or a network id; null when it is gone (a player
  // who left, an NPC removed, an id nobody has) or is the NPC itself. Throws for anything else.
  targetOf(npc, given, what) {
    const handle = given !== null && typeof given === 'object';
    const id = handle ? given.id : given;
    if (!Number.isSafeInteger(id)) {
      throw new TypeError(`${what} must be a Player, an Npc or a network id`);
    }
    const target = this.host.players.get(id) ?? this.npcs.get(id) ?? null;
    if (target === null || target === npc || (handle && target !== given)) {
      return null;
    }
    return target;
  }

  // Whether the target can reach the body: it is alive, in the NPC's world (or one every world
  // sees) and within `range` metres (an attack's by default) of where the NPC was last reported.
  reaches(npc, target, range = ATTACK_RANGE) {
    const state = npc[npcState];
    const worlds = [state.dimension, target.dimension];
    const sameMap = (target.map ?? npc.map) === npc.map;
    const shared = sameMap && (worlds[0] === worlds[1] || worlds.includes(GLOBAL_WORLD));
    const there = target.position;
    const distance = Math.hypot(
      there.x - state.position.x,
      there.y - state.position.y,
      there.z - state.position.z,
    );
    return target.alive !== false && shared && distance <= range;
  }

  // Npc.moveTo: "<sequence>|moveTo|<x>|<y>|<z>|<speed>|<radius>|<pathfinding>".
  moveTo(npc, position, options) {
    this.requireRunning();
    const { speed, radius, pathfinding } = moveOptionsOf(options, 'moveTo');
    const place = placeOf(position, npc[npcState].position);
    if (place === null || !this.orderable(npc)) {
      return false;
    }
    return this.giveOrder(npc, 'moveTo', this.moveArgs(place, speed, radius, pathfinding));
  }

  // The arguments of a moveTo order: "<x>|<y>|<z>|<speed>|<radius>|<pathfinding>".
  moveArgs(place, speed, radius, pathfinding) {
    return [...coordinatesOf(place), speed, decimal(radius), pathfinding];
  }

  // Npc.patrol: the route (positions, a PatrolRoute, or its id or name) is copied and its first
  // leg ordered as a move; npcs/patrols.cjs orders the others as the simulator reports each done.
  patrol(npc, route, options) {
    this.requireRunning();
    const overrides = patrolOverridesOf(options);
    const run = this.patrols.runOf(route, overrides, npc[npcState].position);
    if (run === null || !this.orderable(npc)) {
      return false;
    }
    return this.giveLeg(npc, run);
  }

  // Orders the leg `run` is on as a move: the intent has no patrol kind, its legs are moves.
  giveLeg(npc, run) {
    const leg = run.legs[run.index];
    this.patrols.noteOrdered(run);
    const args = this.moveArgs(leg.place, leg.speed, leg.radius, run.pathfinding);
    return this.giveOrder(npc, 'moveTo', args, { patrol: run });
  }

  // Npc.follow: "<sequence>|follow|<targetId>|<speed>|<radius>".
  follow(npc, target, options) {
    this.requireRunning();
    const { speed, radius } = followOptionsOf(options);
    const followed = this.targetOf(npc, target, 'follow target');
    if (followed === null || !this.orderable(npc)) {
      return false;
    }
    return this.giveOrder(npc, 'follow', [followed.id, speed, decimal(radius)]);
  }

  // Npc.flee: "<sequence>|flee|<x>|<y>|<z>|<speed>|<radius>".
  flee(npc, from, options) {
    this.requireRunning();
    const { speed, radius } = fleeOptionsOf(options);
    const place = placeOf(from, npc[npcState].position);
    if (place === null || !this.orderable(npc)) {
      return false;
    }
    return this.giveOrder(npc, 'flee', [...coordinatesOf(place), speed, decimal(radius)]);
  }

  // Npc.lookAt: "<sequence>|lookAt|entity|<id>" or "<sequence>|lookAt|point|<x>|<y>|<z>". Looking
  // has no end to report, so the status stays `idle`.
  lookAt(npc, target) {
    this.requireRunning();
    let args = null;
    if (namesEntity(target)) {
      const looked = this.targetOf(npc, target, 'lookAt target');
      args = looked === null ? null : ['entity', looked.id];
    } else {
      const place = placeOf(target, npc[npcState].position);
      args = place === null ? null : ['point', ...coordinatesOf(place)];
    }
    if (args === null || !this.orderable(npc)) {
      return false;
    }
    return this.giveOrder(npc, 'lookAt', args, { ends: false });
  }

  // Npc.attack: "<sequence>|attack|<targetId>|<weapon>".
  attack(npc, target, options) {
    this.requireRunning();
    const { weapon } = attackOptionsOf(options);
    const attacked = this.targetOf(npc, target, 'attack target');
    if (
      attacked === null ||
      !npc[npcState].brain ||
      !this.orderable(npc) ||
      !this.reaches(npc, attacked)
    ) {
      return false;
    }
    // A knocked-out player is not finished off (targetDown), and a player in a dice match or at a
    // game station cannot be fought: the order is refused.
    if (
      attacked.knockedOut === true ||
      this.host.dice?.byPlayer.has(attacked.id) ||
      this.host.crafting?.working.has(attacked.id)
    ) {
      return false;
    }
    npc[npcState].attackTarget = attacked;
    return this.giveOrder(npc, 'attack', [attacked.id, weapon]);
  }

  // A player knocked out or an NPC dead (`target`): every NPC attacking it turns to whoever still
  // fights it (struck it within STRIKER_FIGHTING_MS, up and within reach), and otherwise drops the
  // fight and stands; it does not finish off a downed player.
  targetDown(target) {
    const now = Date.now();
    for (const npc of this.npcs.values()) {
      const state = npc[npcState];
      if (!state.alive || state.intent !== 'attack' || state.attackTarget !== target) {
        continue;
      }
      const next = this.stillFighting(npc, target, now);
      if (next) {
        this.attack(npc, next);
        continue;
      }
      this.stopFighting(npc);
    }
  }

  // The most recent striker of the NPC who is still up and within reach, other than `down`.
  stillFighting(npc, down, now) {
    const strikers = npc[npcState].strikers;
    if (!strikers) {
      return null;
    }
    let best = null;
    let bestAt = 0;
    for (const [striker, at] of strikers) {
      if (striker === down || now - at > STRIKER_FIGHTING_MS || at <= bestAt) {
        continue;
      }
      const up = striker[npcState] ? striker[npcState].alive : !striker.knockedOut;
      const here = striker[npcState]
        ? this.live(striker)
        : this.host.players.get(striker.id) === striker;
      if (up && here && this.reaches(npc, striker)) {
        best = striker;
        bestAt = at;
      }
    }
    return best;
  }

  // What the NPC said on its simulator (NPC_BARK: its barks, its lines in a fight): every player
  // sees it in the speech field, as Npc.say shows, for a time that follows its length. Marked as
  // the game's own (U+2063, invisible: the field takes no control characters) so the simulator,
  // whose game showed it already, does not show it again; a localization key (U+2064) is shown in
  // each player's language. The voice file the simulator's game played with the line follows
  // between U+2062 marks with its purpose (mark, file, mark, purpose, mark): every other client
  // plays it on its copy of the NPC; a line said without a bubble has only its voice. Taken only
  // from the NPC's current simulator.
  barked({ id, from, text, localize, voice, purpose }) {
    const npc = this.npcs.get(id);
    const voiced = typeof voice === 'string' && voice.length > 0;
    if (!npc || typeof text !== 'string' || (text.length === 0 && !voiced)) {
      return false;
    }
    const state = npc[npcState];
    if (!state.alive || state.simulatorId !== from) {
      return false;
    }
    clearTimeout(state.speechTimer);
    const duration = Math.min(BARK_MAX_MS, BARK_BASE_MS + BARK_MS_PER_CHARACTER * text.length);
    state.speechSequence = (state.speechSequence ?? 0) + 1;
    const sequence = state.speechSequence;
    const spoken = voiced ? `⁢${voice}⁢${Number(purpose) >>> 0}⁢` : '';
    const marked = `⁣${localize ? '⁤' : ''}${spoken}${text}`;
    state.speechTimer = setTimeout(() => {
      state.speechTimer = undefined;
      if (this.live(npc) && state.speechSequence === sequence) {
        this.host.bridge.send({ op: 'npcSay', id: npc.id, speech: '' });
      }
    }, duration);
    state.speechTimer.unref?.();
    this.host.bridge.send({
      op: 'npcSay',
      id: npc.id,
      speech: `${sequence}|${duration}|${marked}`,
    });
    return true;
  }

  // The NPC's attack ends without an order of its own: it stands where it is, not frozen.
  stopFighting(npc) {
    const state = npc[npcState];
    state.intentSequence += 1;
    state.intent = 'hold';
    state.status = 'idle';
    state.attackTarget = null;
    this.host.bridge.send({ op: 'npcIntent', id: npc.id, intent: '' });
  }

  // The simulator's report that the NPC's order ended: { id, status, sequence? } with a status of
  // reached, blocked or failed; a report that names an earlier order's sequence is stale and
  // dropped. Raises npcIntentDone(npc, status) once the status reads so. The native report is
  // NPC_INTENT_DONE (npc.hpp), which main.cjs passes here as the npcIntentDone op.
  intentDone(report) {
    const npc = this.npcs.get(report.id);
    if (!npc || !FINISHED_STATUSES.includes(report.status)) {
      return;
    }
    const state = npc[npcState];
    if (report.sequence !== undefined && report.sequence !== state.intentSequence) {
      return;
    }
    if (state.patrol !== null) {
      this.patrols.legEnded(npc, state.patrol, report.status);
      return;
    }
    state.status = report.status;
    this.host.emit('npcIntentDone', npc, report.status);
  }

  moveToWorld(npc, world) {
    this.requireLive(npc);
    if (!validWorld(world)) {
      throw new TypeError('Invalid dimension');
    }
    npc[npcState].dimension = world;
    this.host.areas.touch('npc', npc, npc[npcState].position, world, npc[npcState].map);
    this.host.stashes.npcCorpse(npc, { moved: true });
    this.host.bridge.send({
      op: 'npcWorld',
      id: npc.id,
      dimension: onMap(npc[npcState].map, world),
    });
  }

  // Npc.playAnimation: "<sequence>|<loop>|<clip>" in the NPC's `animation` field (npc.hpp), or for
  // a fragment "<sequence>|<loop>||<fragment>|<tags>" (an empty clip).
  animate(npc, fragment, options) {
    this.requireRunning();
    const request = animationRequestOf(fragment, options);
    if (request === null || !this.live(npc)) {
      return false;
    }
    const state = npc[npcState];
    state.animationSequence = (state.animationSequence ?? 0) + 1;
    const played =
      request.clip === undefined ? `|${request.fragment}|${request.tags}` : request.clip;
    const animation = `${state.animationSequence}|${request.loop ? 1 : 0}|${played}`;
    this.host.bridge.send({ op: 'npcAnimation', id: npc.id, animation });
    const replaced = state.playing;
    state.playing = {
      sequence: state.animationSequence,
      fragment: typeof fragment === 'string' ? fragment : String(fragment ?? ''),
    };
    if (replaced) {
      this.host.emit('npcAnimationEnd', npc, replaced.fragment, 'replaced');
    }
    return true;
  }

  stopAnimation(npc) {
    this.requireRunning();
    if (!this.live(npc)) {
      return false;
    }
    this.host.bridge.send({ op: 'npcAnimation', id: npc.id, animation: '' });
    const stopped = npc[npcState].playing;
    npc[npcState].playing = null;
    if (stopped) {
      this.host.emit('npcAnimationEnd', npc, stopped.fragment, 'stopped');
    }
    return true;
  }

  // The simulator's report that the clip a request started is over:
  // { id, sequence, reason: 'finished' | 'interrupted' | 'refused' }. A report of a request
  // replaced or stopped since is dropped (that end was raised already).
  animationEnded({ id, sequence, reason }) {
    const npc = this.npcs.get(id);
    const playing = npc?.[npcState].playing;
    if (!playing || playing.sequence !== sequence) {
      return;
    }
    if (!['finished', 'interrupted', 'refused'].includes(reason)) {
      return;
    }
    npc[npcState].playing = null;
    this.host.emit('npcAnimationEnd', npc, playing.fragment, reason);
  }

  // Npc.say: "<sequence>|<milliseconds>|<text>" in the NPC's `speech` field (npc.hpp NpcSpeech).
  say(npc, given, options) {
    this.requireRunning();
    const { text, duration } = speechOf(given, options);
    if (!this.live(npc)) {
      return false;
    }
    const state = npc[npcState];
    clearTimeout(state.speechTimer);
    state.speechTimer = undefined;
    let speech = '';
    if (text.length > 0) {
      state.speechSequence = (state.speechSequence ?? 0) + 1;
      speech = `${state.speechSequence}|${duration}|${text}`;
      // The field empties when the speech is over, so a player who comes later sees nothing old.
      const sequence = state.speechSequence;
      state.speechTimer = setTimeout(() => {
        state.speechTimer = undefined;
        if (this.live(npc) && state.speechSequence === sequence) {
          this.host.bridge.send({ op: 'npcSay', id: npc.id, speech: '' });
        }
      }, duration);
      state.speechTimer.unref?.();
    }
    this.host.bridge.send({ op: 'npcSay', id: npc.id, speech });
    return true;
  }

  teleport(npc, position, facing) {
    this.requireRunning();
    if (!this.live(npc)) {
      return false;
    }
    const state = npc[npcState];
    let target = state.position;
    if (position !== undefined) {
      if (position === null || typeof position !== 'object') {
        return false;
      }
      // Partial<Vector3>: a component left out keeps the current one.
      target = {
        x: position.x ?? state.position.x,
        y: position.y ?? state.position.y,
        z: position.z ?? state.position.z,
      };
      if (positionProblem(target) !== null) {
        return false;
      }
    }
    let turned = state.rotation;
    if (facing !== undefined) {
      turned = facingOrNull(facing);
      if (!turned) {
        return false;
      }
    }
    state.position = new Vector3(target.x, target.y, target.z);
    state.rotation = copyQuaternion(turned);
    state.velocity = new Vector3(0, 0, 0);
    this.host.areas.touch('npc', npc, state.position, state.dimension, state.map);
    this.host.stashes.npcCorpse(npc, { moved: true });
    this.host.bridge.send({
      op: 'npcTeleport',
      id: npc.id,
      position: { x: target.x, y: target.y, z: target.z },
      rotation: { x: turned.x, y: turned.y, z: turned.z, w: turned.w },
    });
    return true;
  }

  // Entity.position and rotation assignments: a teleport that throws for what it cannot use.
  place(npc, position, facing) {
    this.requireLive(npc);
    if (position !== undefined) {
      positionOf(position, 'Npc.position');
    }
    if (facing !== undefined) {
      facingOf(facing, 'Npc.rotation');
    }
    this.teleport(npc, position, facing);
  }

  // npcState: the native server's report of where the NPC is (its simulator moves it).
  update(report) {
    const npc = this.npcs.get(report.id);
    if (!npc) {
      return;
    }
    const state = npc[npcState];
    const { position, rotation, velocity } = report;
    state.position = new Vector3(position.x, position.y, position.z);
    state.rotation = new Quaternion(rotation.w, rotation.x, rotation.y, rotation.z);
    if (velocity) {
      state.velocity = new Vector3(velocity.x, velocity.y, velocity.z);
    }
    if (validWorld(report.dimension)) {
      state.dimension = worldOnMap(report.dimension);
    }
    this.host.areas.touch('npc', npc, state.position, state.dimension, state.map);
  }

  // npcSimulator: another player's client (or none) simulates the NPC now.
  simulatorChanged(report) {
    const npc = this.npcs.get(report.id);
    if (!npc) {
      return;
    }
    const state = npc[npcState];
    const simulatorId = Number.isSafeInteger(report.simulatorId) ? report.simulatorId : 0;
    if (simulatorId === state.simulatorId) {
      return;
    }
    const previous = state.simulatorHandle;
    state.simulatorId = simulatorId;
    state.simulatorHandle = this.host.players.get(simulatorId) ?? null;
    this.host.emit('npcSimulatorChange', npc, state.simulatorHandle, previous);
  }

  // The resource stopped or failed: the NPCs it made go.
  resourceStopped(name) {
    for (const npc of [...this.npcs.values()]) {
      if (npc[npcState].resource === name) {
        this.destroy(npc);
      }
    }
  }
}

module.exports = { Npcs, MAX_HEALTH };
