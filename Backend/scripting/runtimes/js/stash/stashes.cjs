'use strict';

// Containers the server holds: the server API's Stash, and the body of a knocked-out
// player that others search. Each is a NetworkStash entity that every client in view
// stands in the world as the game's own container (Client/runtime/stash): the game's own Search
// prompt and transfer screen. What a container holds goes only to the player who opened it.
//
// Opening: the player's game asks; the server grants it when the player is within reach and in
// its world, nobody else holds it and no stashInteract handler returns false. The holder's game
// then shows the rows; what they move in its transfer screen the game leaves where it is and asks
// the server, which moves the units (reason 'withdraw' or 'deposit' on both sides) and shows both
// stocks again.
//
// A knocked-out player's body (Player.lootable, true by default) is such a container at the body
// while they are down: it shows their own inventory, and what a looter takes leaves it through
// the server (reason 'loot'), after a playerLoot handler may refuse each take; playerLooted
// follows. Nothing can be put into a body. Reviving the player, or their leaving, takes the
// container away.
//
// A dead NPC's body (Npc.lootable, true by default; a KCDS addition) is the same kind
// of container at the place where it died: it shows the NPC's own inventory (Npc.getInventory),
// and what a looter takes leaves it through the server (reason 'loot'), after an npcLoot handler
// may refuse each take; npcLooted follows. Nothing can be put into it. Reviving the NPC, removing
// it or making it not lootable takes the container away.
//
// A virtual stash (Stash.createStorage) is server-owned stock without a chest: no client
// stands anything for it, and Stash#open(player) grants the transfer screen from anywhere in its
// world (no distance check). It keeps the same inventory methods and events as a chest.
//
// A scripted container can be locked (Stash#locked, canBePicked, keyClass): a locked
// container is refused every open, and the lock reaches the clients that have it streamed in with
// the stashLock op. A player who opens the lock in their game, with a lock pick or the key, is
// reported (stashUnlock): the server checks it and unlocks the container. The game's lock is one
// lock for everybody, so the container is then unlocked for every player, as a picked chest in the
// game stays open; stashUnlocked(stash, player, method) follows, and the open that comes next is let
// through.
//
// A cart's storage (Cart#addStorage) is such a container on the cart: no chest of its own, each
// client puts it on the cart as it shows it (its replicated bodyOf names the cart), with the
// game's own Search prompt; within reach of the cart, Stash#open(player) opens it too. It goes
// with the cart (Cart#removeStorage), never by itself, and has no lock.
//
// A server horse's saddlebags (Horse#storage) are server-owned stock too: nothing
// stands for them, the horse's owner's game shows them in the horse's own inventory (the stashShow
// names the horse), and what the owner moves between it and themself in the game's inventory
// screen is asked of the server as a container's take and put, without opening anything: only
// the owner, within HORSE_REACH of the horse, and no more than the horse carries
// (Horse#maxCarryWeight). They go with the horse, never by themselves, and have no lock.
//
// Not yet: the level's own containers, useDirection, the idle timeout.

const fs = require('node:fs');
const path = require('node:path');

const { isKeyring, lookupItemClass } = require('../inventory/catalog.cjs');
const { OWNER, removeOwned } = require('../lifecycle/owned_objects.cjs');
const { Inventory, transfer } = require('../inventory/inventories.cjs');
const { Quaternion, Vector3, rotation, vector } = require('../math.cjs');

const GLOBAL_WORLD = 0xffffffff;
// How far from a container a player may use it, in metres (about the game's reach to a chest),
// and the slack for a body that moved while the server last heard of it.
const REACH = 5;
// How near the horse its owner reaches into its saddlebags (metres): the game offers the horse's
// inventory while the horse is near.
const HORSE_REACH = 15;
const REACH_SLACK = 1.5;
// Rows a container holds at most: Player.dropAll moves at most this many.
const MAX_STASH_ROWS = 128;
// The Locales key of the notice to a player who tries a container another has open, and how often
// one player gets it.
const IN_USE = 'stash.inUse';
const IN_USE_QUIET_MS = 3000;
const WORLD_LIMIT = 1e6;
// How a player's game opened a container's lock (stashUnlock).
const UNLOCK_METHODS = new Set(['pick', 'key']);
// The levels' own chests (Dev/tools/stash/build_level_chests.py): one row each,
// [level, guid, x, y, z, model, locked, canBePicked, difficulty].
const LEVEL_CHESTS = path.join(__dirname, 'level_chests.json');
const LEVEL_GUID = /^[0-9a-f]{16}$/;
// How far Stash.levelChests looks around a position by default, in metres.
const LEVEL_CHEST_RADIUS = 20;
let levelChestRows = null;
// A model Stash.spawn builds a container as: a game mesh path of at most this many
// bytes, a static mesh (.cgf) or a character (.cga, .cdf).
const MODEL_PATH = /^[^\0\\:]+\.(cgf|cga|cdf)$/i;
const MODEL_MAX_BYTES = 255;

// A lock-picking difficulty: a number from 0 to 1, else null.
function fraction(value) {
  return typeof value === 'number' && value >= 0 && value <= 1 ? value : null;
}

// The chests of one level, by their level GUID.
function levelChestsOf(level) {
  levelChestRows ??= JSON.parse(fs.readFileSync(LEVEL_CHESTS, 'utf8'));
  const chests = new Map();
  for (const row of levelChestRows) {
    const [own, guid, x, y, z, model, locked, canBePicked, difficulty, name = ''] = row;
    if (own === level) {
      chests.set(guid, { guid, x, y, z, model, locked, canBePicked, difficulty, name });
    }
  }
  return chests;
}

function validWorld(world) {
  return Number.isSafeInteger(world) && world >= 0 && world <= GLOBAL_WORLD;
}

// The model Stash.spawn's options name: '' for the default chest. Throws for options
// that are not an object, or a model that is not a .cgf, .cga or .cdf path.
function modelOf(options) {
  if (options === undefined || options === null) {
    return '';
  }
  if (typeof options !== 'object') {
    throw new TypeError('Stash.spawn: options is an object');
  }
  const { model } = options;
  if (model === undefined || model === '') {
    return '';
  }
  if (
    typeof model !== 'string' ||
    !MODEL_PATH.test(model) ||
    Buffer.byteLength(model, 'utf8') > MODEL_MAX_BYTES
  ) {
    throw new TypeError(
      `Stash.spawn: model must be a .cgf, .cga or .cdf path of at most ${MODEL_MAX_BYTES} bytes`,
    );
  }
  return model;
}

function finiteVector(value) {
  return [value.x, value.y, value.z].every((n) => Number.isFinite(n) && Math.abs(n) <= WORLD_LIMIT);
}

function distance(a, b) {
  return Math.hypot(a.x - b.x, a.y - b.y, a.z - b.z);
}

function refused(code) {
  return { ok: false, code, revision: 0, items: [] };
}

// The rows of an inventory as the bridge's inventoryShow carries them; `shownClassOf` names the class
// a row is shown as (a renamed row's view type). A body does not show its player's keyring.
function wireRows(inventory, shownClassOf, body) {
  const rows = [...inventory.rows.values()].filter((row) => !body || !isKeyring(row.itemClass));
  return rows.map((row) => [
    row.id,
    shownClassOf(row),
    row.amount,
    row.props.quality,
    row.props.health,
    row.equipped,
  ]);
}

class Stashes {
  // `host`: the resource host (bridge, emit, players, playerInventories, allocateEntityId).
  constructor(host) {
    this.host = host;
    // id -> Stash
    this.stashes = new Map();
    // A knocked-out player's id -> their body's container.
    this.bodies = new Map();
    // player id -> when they were last told a container is in use.
    this.inUseTold = new Map();
    // A dead NPC's id -> its body's container.
    this.corpses = new Map();
    this.nextStashId = 1;
  }

  // The Stash class of the server API, an Entity.
  define(Entity) {
    const registry = this;
    class Stash {
      constructor(id) {
        const stash = registry.stashes.get(id);
        if (!stash) {
          throw new Error('Unknown stash');
        }
        return stash;
      }
      // `options.model` builds it as that mesh instead of the default chest.
      static spawn(position = {}, orientation = {}, dimension = GLOBAL_WORLD, options = {}) {
        const model = modelOf(options);
        return registry.spawn(Stash, { position, orientation, dimension, bodyOf: null, model });
      }
      static all() {
        return [...registry.stashes.values()];
      }
      static getById(id) {
        return registry.stashes.get(id) ?? null;
      }
      // Server-owned stock without a chest on any client; open it with open(player).
      static createStorage(dimension = GLOBAL_WORLD) {
        return registry.spawn(Stash, {
          position: {},
          orientation: {},
          dimension,
          bodyOf: null,
          isStorage: true,
        });
      }
      // A view of a connected player's own inventory, as a storage another player opens from
      // anywhere with open(viewer): the game's transfer screen with what the player carries and
      // wears. What the viewer takes leaves the player (worn things come off them); with
      // `put: true` the viewer may give them things too. It goes when the player leaves or the
      // resource stops.
      static playerInventory(player, options = {}) {
        const owner = player && registry.host.players.get(player.id);
        if (!owner || owner !== player) {
          throw new TypeError('Stash.playerInventory takes a connected Player');
        }
        if (options === null || typeof options !== 'object') {
          throw new TypeError('Stash.playerInventory options must be an object');
        }
        if (options.put !== undefined && typeof options.put !== 'boolean') {
          throw new TypeError('Stash.playerInventory put is true or false');
        }
        return registry.spawn(Stash, {
          position: {},
          orientation: {},
          dimension: GLOBAL_WORLD,
          bodyOf: null,
          viewOf: owner.id,
          viewPut: options.put === true,
          isStorage: true,
        });
      }
      // A scripted chest by its guid, or an adopted level chest by its level GUID, in the world
      // it was spawned or adopted in.
      static find(guid, dimension = GLOBAL_WORLD) {
        if (typeof guid !== 'string' || !validWorld(dimension)) {
          return null;
        }
        const wanted = guid.toLowerCase().replace(/^0x/, '');
        for (const stash of registry.stashes.values()) {
          const matches =
            (stash.guid !== '' && stash.guid === wanted) ||
            (stash.placedGuid !== '' && stash.placedGuid === wanted);
          if (matches && stash.dimension === dimension) {
            return stash;
          }
        }
        return null;
      }
      // The level's own chests around a position (within `radius` metres, default
      // 20), or all of the level's without one: where each stands, its model, whether the level
      // locks it and whether its lock can be picked. Each is the game's own until adopt() makes
      // it the server's.
      static levelChests(position, radius = LEVEL_CHEST_RADIUS) {
        if (typeof radius !== 'number' || !(radius > 0)) {
          throw new RangeError('radius must be a positive number of metres');
        }
        const at = position === undefined ? null : vector(position);
        if (at && !finiteVector(at)) {
          throw new RangeError('Invalid position');
        }
        const rows = [];
        for (const chest of levelChestsOf(registry.level()).values()) {
          if (at && distance(at, chest) > radius) {
            continue;
          }
          rows.push({
            placedGuid: chest.guid,
            // The level entity's own name, as the level's designers named it.
            sourceName: chest.name,
            position: new Vector3(chest.x, chest.y, chest.z),
            model: chest.model,
            lockedAtBoot: chest.locked,
            canBePicked: chest.canBePicked,
            // The level's own difficulty, null where the game makes one up as the chest resets.
            lockLevel: chest.difficulty,
          });
        }
        return rows;
      }
      // One of the level's own chests becomes the server's: its contents are the
      // server's from now on (empty at first; what the game put in it is not kept), its lock is
      // the level's until a script changes it, and every player opens it as a server chest.
      // destroy() gives it back to the game. The same chest adopted again in the same world is
      // the same Stash; null for a GUID the server's level does not have.
      static adopt(placedGuid, dimension = GLOBAL_WORLD) {
        if (typeof placedGuid !== 'string') {
          throw new TypeError('placedGuid must be a string of sixteen hex digits');
        }
        const wanted = placedGuid.toLowerCase().replace(/^0x/, '');
        if (!LEVEL_GUID.test(wanted)) {
          throw new RangeError('placedGuid must be sixteen hex digits');
        }
        const adopted = Stash.find(wanted, dimension);
        if (adopted && adopted.placedGuid === wanted) {
          return adopted;
        }
        const chest = levelChestsOf(registry.level()).get(wanted);
        if (!chest) {
          return null;
        }
        const stash = registry.spawn(Stash, {
          position: { x: chest.x, y: chest.y, z: chest.z },
          orientation: {},
          dimension,
          bodyOf: null,
          placedGuid: wanted,
          lockedAtBoot: chest.locked,
        });
        registry.setLock(stash, {
          locked: chest.locked,
          byServer: false,
          canBePicked: chest.canBePicked,
        });
        return stash;
      }
      static nearest(position, maxDistance = REACH, dimension = GLOBAL_WORLD) {
        const at = vector(position);
        let best = null;
        let bestDistance = maxDistance;
        for (const stash of registry.stashes.values()) {
          if (stash.isStorage) {
            continue;
          }
          const away = distance(at, stash.at);
          if (stash.dimension === dimension && away <= bestDistance) {
            best = stash;
            bestDistance = away;
          }
        }
        return best;
      }
      static destroyAll(dimension) {
        if (dimension !== undefined && !validWorld(dimension)) {
          throw new TypeError('Invalid dimension');
        }
        let removed = 0;
        for (const stash of [...registry.stashes.values()]) {
          if (
            stash.bodyOf === null &&
            stash.corpseOf === null &&
            stash.cartOf === null &&
            stash.horseOf === null &&
            (dimension === undefined || stash.dimension === dimension)
          ) {
            registry.destroy(stash);
            removed += 1;
          }
        }
        return removed;
      }
      // Sixteen lowercase hex digits minted from the stash id; empty for virtual stock and bodies.
      get guid() {
        return this.isStorage ||
          this.bodyOf !== null ||
          this.corpseOf !== null ||
          this.cartOf !== null ||
          this.horseOf !== null
          ? ''
          : this.stashId.toString(16).padStart(16, '0');
      }
      // An adopted level chest's level GUID, '' for any other container.
      get placedGuid() {
        return this.level;
      }
      get name() {
        if (this.level !== '') {
          return `level_chest_${this.level}`;
        }
        return this.isStorage ? '' : `spawned_stash_${this.stashId}`;
      }
      get isStorage() {
        return this.virtual;
      }
      get locked() {
        return this.lock.locked;
      }
      set locked(value) {
        registry.setLock(this, { locked: !!value, byServer: !!value });
      }
      get serverLocked() {
        return this.lock.byServer;
      }
      get canBePicked() {
        return this.lock.canBePicked;
      }
      set canBePicked(value) {
        registry.setLock(this, { canBePicked: !!value });
      }
      // The key item class GUID; empty restores the level's own (a scripted chest has none).
      get keyClass() {
        return this.lock.keyClass;
      }
      set keyClass(value) {
        // A key of its own, ahead of its area's; empty goes back to the area's.
        this.lock.ownKey = value !== '';
        registry.setLock(this, { keyClass: value === '' ? this.lock.areaKey ?? '' : value });
      }
      // How hard its lock is to pick, 0 (easy) to 1 (hard), as a script set it for every player
      // null for the game's own (the level's, or the one the game makes up).
      get lockLevel() {
        return this.lock.difficulty;
      }
      set lockLevel(value) {
        const next = value === null ? null : fraction(value);
        if (next === null && value !== null) {
          throw new RangeError('lockLevel is a number from 0 to 1, or null');
        }
        registry.setLock(this, { difficulty: next });
      }
      // Which lock the lock-picking minigame shows, 'common' or 'fancy', as a script set it for
      // every player; null for the level's.
      get lockStyle() {
        return this.lock.fanciness;
      }
      set lockStyle(value) {
        if (value !== null && value !== 'common' && value !== 'fancy') {
          throw new RangeError("lockStyle is 'common', 'fancy' or null");
        }
        registry.setLock(this, { fanciness: value });
      }
      // A scripted container's spawn model: empty for the default chest
      // and for one the level places.
      get model() {
        return this.modelPath;
      }
      // Whether the level locks it (an adopted level chest); a scripted chest never is.
      get lockedAtBoot() {
        return this.bootLocked;
      }
      get isOpen() {
        return this.holder() !== 0;
      }
      get heldById() {
        return this.holder();
      }
      get itemCount() {
        const inventory = registry.inventoryOf(this);
        if (!inventory) {
          return 0;
        }
        const owners = this.bodyOf !== null || this.viewOf !== null;
        return wireRows(inventory, (row) => row.itemClass.id, owners).length;
      }
      get position() {
        const at = registry.placeOf(this);
        return new Vector3(at.x, at.y, at.z);
      }
      get rotation() {
        return new Quaternion(this.facing.w, this.facing.x, this.facing.y, this.facing.z);
      }
      // The knocked-out player whose body this is, or null for a chest.
      get body() {
        return this.bodyOf === null ? null : registry.host.players.get(this.bodyOf) ?? null;
      }
      // For a view of a player's inventory (Stash.playerInventory), that player, otherwise null.
      get inventoryOf() {
        return this.viewOf === null ? null : registry.host.players.get(this.viewOf) ?? null;
      }
      getInventory() {
        const inventory = registry.inventoryOf(this);
        if (!inventory) {
          return null;
        }
        const { revision, items } = inventory.snapshot();
        return { revision, items };
      }
      addItem(request) {
        return registry.operate(this, (inventory) => inventory.add(request));
      }
      removeItem(request) {
        return registry.operate(this, (inventory) => inventory.remove(request));
      }
      setInventory(request) {
        return registry.operate(this, (inventory) => inventory.set(request));
      }
      // Only virtual stock and a cart's storage open from a script; any other container opens
      // with the game's use.
      open(player) {
        // A body opens with the game's own Rob, a horse's saddlebags in the horse's inventory. A
        // chest opens for a player within reach of it, as the game's use does.
        if (this.horseOf !== null || this.bodyOf !== null || this.corpseOf !== null) {
          return false;
        }
        return registry.openVirtual(this, player);
      }
      destroy() {
        const kept = this.cartOf !== null || this.horseOf !== null;
        if (this.bodyOf === null && this.corpseOf === null && !kept) {
          registry.destroy(this);
        }
      }
      toString() {
        const holder = this.holder();
        const held = holder ? `, held by ${holder}` : '';
        return `Stash(${this.stashId}, ${this.itemCount} rows${held})`;
      }
    }
    Object.setPrototypeOf(Stash.prototype, Entity.prototype);
    this.Stash ??= Stash;
    return Stash;
  }

  spawn(
    Stash,
    {
      position,
      orientation,
      dimension,
      bodyOf,
      corpseOf = null,
      cartOf = null,
      horseOf = null,
      viewOf = null,
      viewPut = false,
      isStorage = false,
      placedGuid = '',
      lockedAtBoot = false,
      model = '',
    },
  ) {
    const owner = this.host.context.getStore();
    if (
      owner &&
      (this.host.resources.get(owner.name) !== owner || !owner.active || owner.failing)
    ) {
      throw new Error('Resource is stopped');
    }
    if (!validWorld(dimension)) {
      throw new TypeError('Invalid dimension');
    }
    const at = vector(position);
    if (!finiteVector(at)) {
      throw new RangeError('Invalid stash position');
    }
    const facing = rotation(orientation);
    const stash = Object.create(Stash.prototype);
    let holder = 0;
    Object.defineProperty(stash, 'id', { value: this.host.allocateEntityId(), enumerable: true });
    Object.defineProperty(stash, 'stashId', { value: this.nextStashId, enumerable: true });
    this.nextStashId += 1;
    Object.defineProperty(stash, 'dimension', { value: dimension, enumerable: true });
    Object.defineProperty(stash, 'bodyOf', { value: bodyOf });
    // The id of the dead NPC whose body this is, or null.
    Object.defineProperty(stash, 'corpseOf', { value: corpseOf });
    // The id of the cart whose storage this is, or null.
    Object.defineProperty(stash, 'cartOf', { value: cartOf });
    // The id of the horse whose saddlebags these are, or null.
    Object.defineProperty(stash, 'horseOf', { value: horseOf });
    // The id of the player whose own inventory this view shows (Stash.playerInventory), or null,
    // and whether its holder may put into it.
    Object.defineProperty(stash, 'viewOf', { value: viewOf });
    Object.defineProperty(stash, 'viewPut', { value: viewPut });
    Object.defineProperty(stash, 'at', { value: { x: at.x, y: at.y, z: at.z } });
    Object.defineProperty(stash, 'facing', {
      value: { x: facing.x, y: facing.y, z: facing.z, w: facing.w },
    });
    // Virtual stock has no chest on any client.
    Object.defineProperty(stash, 'virtual', { value: isStorage });
    // An adopted level chest: the level's own entity every client finds by this.
    Object.defineProperty(stash, 'level', { value: placedGuid });
    Object.defineProperty(stash, 'bootLocked', { value: lockedAtBoot });
    // The mesh it is built as, empty for the default chest.
    Object.defineProperty(stash, 'modelPath', { value: model });
    // The lock: set by a script (`byServer`), whether lockpicks are offered, the key's class GUID.
    Object.defineProperty(stash, 'lock', {
      value: {
        locked: false,
        byServer: false,
        canBePicked: true,
        keyClass: '',
        difficulty: null,
        fanciness: null,
      },
    });
    Object.defineProperty(stash, 'stock', {
      value:
        bodyOf === null && corpseOf === null && viewOf === null
          ? new Inventory({ maxRows: MAX_STASH_ROWS })
          : null,
    });
    Object.defineProperty(stash, 'holder', { value: () => holder });
    Object.defineProperty(stash, 'hold', {
      value: (id) => {
        holder = id;
      },
    });
    const scripted = bodyOf === null && corpseOf === null && cartOf === null && horseOf === null;
    stash[OWNER] = scripted ? owner?.name ?? null : null;
    if (scripted && !isStorage && viewOf === null) {
      // Every client's prompt says "(empty)" by this: its own inventory stays empty until opened.
      let empty = true;
      stash.stock.onChange = () => {
        const now = stash.stock.rows.size === 0;
        if (now !== empty) {
          empty = now;
          this.host.bridge.send({ op: 'stashEmpty', id: stash.id, empty });
        }
      };
    }
    this.stashes.set(stash.id, stash);
    if (!isStorage) {
      this.host.bridge.send({
        op: 'stashSpawn',
        id: stash.id,
        position: stash.at,
        rotation: stash.facing,
        dimension,
        // A body (a knocked-out player's or a dead NPC's) or a cart's storage is searched where
        // each client shows that body or cart, without a chest of its own.
        bodyOf: bodyOf ?? corpseOf ?? cartOf ?? 0,
        // An adopted level chest stands as the level's own on every client.
        placedGuid,
        // The mesh every client builds it as, empty for the default chest.
        model,
      });
    }
    this.host.emit('stashSpawn', stash);
    return stash;
  }

  // The level the server runs ('trosecko', 'kutnohorsko' or 'klaster'; trosecko until it is set).
  level() {
    return this.host.statusRoutes?.settings?.map || 'trosecko';
  }

  // The inventory a container shows: its own, the dead NPC's for an NPC's body, or the
  // knocked-out player's for a player's.
  inventoryOf(stash) {
    if (stash.corpseOf !== null) {
      return this.host.npcs.npcs.get(stash.corpseOf)?.getInventory() ?? null;
    }
    const ownerId = stash.bodyOf ?? stash.viewOf;
    if (ownerId === null) {
      return stash.stock;
    }
    const victim = this.host.players.get(ownerId);
    return victim ? this.host.playerInventories.entryOf(victim)?.inventory ?? null : null;
  }

  // A script's change to a container's stock: stashInventoryChanged with no player, and the holder
  // sees it.
  operate(stash, operation) {
    if (this.stashes.get(stash.id) !== stash) {
      return refused('inventoryUnavailable');
    }
    if (stash.bodyOf !== null || stash.corpseOf !== null || stash.viewOf !== null) {
      // A body, or a view of a player's inventory, shows its owner's own inventory:
      // Inventory.add and the rest change it.
      return refused('bodyInventory');
    }
    const { result, change } = operation(stash.stock);
    if (change) {
      this.host.emit('stashInventoryChanged', stash, null, change);
      this.show(stash);
    }
    return result;
  }

  // Sends the container's rows to its holder; `open` asks their game to open its screen,
  // `closed` ends their hold.
  show(stash, { to = this.userOf(stash), open = false, closed = false } = {}) {
    const player = this.host.players.get(to);
    if (!player) {
      return;
    }
    const inventory = closed ? null : this.inventoryOf(stash);
    const horse = stash.horseOf === null ? {} : { horse: stash.horseOf };
    this.host.bridge.send({
      ...horse,
      op: 'stashShow',
      id: player.id,
      stash: stash.id,
      open,
      closed: closed || !inventory,
      revision: inventory ? inventory.revision : 0,
      rows: inventory
        ? wireRows(
            inventory,
            (row) => this.host.customItems.shownClassOf(row),
            stash.bodyOf !== null || stash.viewOf !== null,
          )
        : [],
    });
  }

  // A server horse's saddlebags: server-owned stock nothing stands for, shown to its
  // owner's game.
  saddlebagsFor(horse) {
    if (!this.Stash) {
      this.host.makeAPI();
    }
    return this.spawn(this.Stash, {
      position: {},
      orientation: {},
      dimension: GLOBAL_WORLD,
      bodyOf: null,
      horseOf: horse.id,
      isStorage: true,
    });
  }

  // A horse changed hands: its saddlebags leave the old owner's game and show in the new one's.
  saddlebagsOwner(horse, previousOwnerId) {
    const stash = horse.storage;
    if (!stash) {
      return;
    }
    if (previousOwnerId && previousOwnerId !== horse.ownerId) {
      this.show(stash, { to: previousOwnerId, closed: true });
    }
    this.show(stash);
  }

  // A player's world became ready: their game lost what it showed, so their horses' saddlebags
  // are shown again.
  arrived(player) {
    for (const horse of this.host.horses.values()) {
      if (horse.ownerId === player.id && horse.storage) {
        this.show(horse.storage);
      }
    }
  }

  // Who sees the container's rows: its holder, or a horse's owner for its saddlebags.
  userOf(stash) {
    if (stash.horseOf === null) {
      return stash.holder();
    }
    return this.host.horses.get(stash.horseOf)?.ownerId ?? 0;
  }

  // Whether the player's body can use the container now: in reach and in its world.
  within(player, stash) {
    const world = player.dimension;
    if (stash.dimension !== GLOBAL_WORLD && stash.dimension !== world) {
      return false;
    }
    if (stash.horseOf !== null) {
      const horse = this.host.horses.get(stash.horseOf);
      return Boolean(horse) && distance(player.position, horse.position) <= HORSE_REACH;
    }
    // Virtual stock has no place: the world is all that governs it.
    return stash.virtual || distance(player.position, this.placeOf(stash)) <= REACH + REACH_SLACK;
  }

  // Stash#locked / canBePicked / keyClass: changes the lock and tells every client (change-only).
  // A script locking a container marks it `serverLocked`; unlocking clears both. A body has no lock.
  setLock(stash, { locked, byServer, canBePicked, keyClass, difficulty, fanciness }) {
    if (this.stashes.get(stash.id) !== stash) {
      return;
    }
    const lock = stash.lock;
    const before = { ...lock };
    if (keyClass !== undefined) {
      if (typeof keyClass !== 'string') {
        throw new TypeError('keyClass must be an item class GUID');
      }
      // Empty restores the level's own; an unknown class is refused.
      const itemClass = keyClass === '' ? null : lookupItemClass(keyClass);
      if (keyClass !== '' && !itemClass) {
        this.host.log?.(`Stash ${stash.stashId}: unknown key class ${keyClass}`);
        return;
      }
      lock.keyClass = itemClass ? itemClass.id : '';
    }
    if (locked !== undefined) {
      lock.locked = locked;
      lock.byServer = byServer;
    }
    if (canBePicked !== undefined) {
      lock.canBePicked = canBePicked;
    }
    if (difficulty !== undefined) {
      lock.difficulty = difficulty;
    }
    if (fanciness !== undefined) {
      lock.fanciness = fanciness;
    }
    const changed = Object.keys(lock).some((key) => lock[key] !== before[key]);
    if (
      !changed ||
      stash.virtual ||
      stash.bodyOf !== null ||
      stash.corpseOf !== null ||
      stash.cartOf !== null ||
      stash.horseOf !== null
    ) {
      return;
    }
    this.host.bridge.send({
      op: 'stashLock',
      id: stash.id,
      locked: lock.locked,
      canBePicked: lock.canBePicked,
      keyClass: lock.keyClass,
      // The lock-picking difficulty, -1 the game's own.
      lockLevel: lock.difficulty ?? -1,
      // Which lock the minigame shows, '' the level's.
      lockStyle: lock.fanciness ?? '',
    });
  }

  // stashUnlock: the player's game opened a container's lock, with a lock pick (`pick`) or the key
  // (`key`), and has already applied it. Granted when the container is locked, the player can use
  // it (in reach, in its world, not knocked out) and the method fits the lock: a pick needs a
  // canBePicked lock, a key needs the lock's key class in the player's inventory. The container
  // is then unlocked for every player and stashUnlocked(stash, player, method) follows. A refused
  // unlock changes nothing; the player's next open is refused and their game locks it again.
  unlock(player, id, method) {
    const stash = this.stashes.get(id);
    if (!this.livePlayer(player) || !player.ready || !stash || !UNLOCK_METHODS.has(method)) {
      return false;
    }
    const lock = stash.lock;
    const fits = method === 'pick' ? lock.canBePicked : this.carriesKey(player, lock.keyClass);
    const allowed =
      lock.locked &&
      !stash.virtual &&
      stash.bodyOf === null &&
      stash.corpseOf === null &&
      stash.cartOf === null &&
      stash.horseOf === null &&
      this.within(player, stash) &&
      !player.knockedOut &&
      fits;
    if (!allowed) {
      this.host.log?.(`Stash ${stash.stashId}: unlock by ${method} of player ${player.id} refused`);
      return false;
    }
    this.setLock(stash, { locked: false, byServer: false });
    this.host.emit('stashUnlocked', stash, player, method);
    return true;
  }

  // The key of the area it stands in, unless it has one of its own.
  setAreaKey(stash, item) {
    stash.lock.areaKey = item;
    if (!stash.lock.ownKey) {
      this.setLock(stash, { keyClass: item });
    }
  }

  // Whether the player's inventory holds a key of that item class (empty: the lock has no key).
  carriesKey(player, keyClass) {
    const entry = keyClass === '' ? undefined : this.host.playerInventories.entryOf(player);
    if (!entry) {
      return false;
    }
    for (const row of entry.inventory.rows.values()) {
      if (row.itemClass.id === keyClass && row.amount > 0) {
        return true;
      }
    }
    return false;
  }

  // Stash#open(player) on virtual stock: the same grant as a chest's use, from anywhere in its
  // world. False if the stash is locked or in use, the player is unavailable or stashInteract refuses.
  openVirtual(stash, player) {
    if (!this.livePlayer(player) || !player.ready || this.stashes.get(stash.id) !== stash) {
      return false;
    }
    return this.open(player, stash.id, { quiet: true });
  }

  // Where a container is: a knocked-out player's body is their container, wherever it lies now
  // (carried away too), a cart's storage is on the cart, a horse's saddlebags on the horse; any
  // other stands where it was spawned.
  placeOf(stash) {
    const victim = stash.bodyOf === null ? undefined : this.host.players.get(stash.bodyOf);
    const cart = stash.cartOf === null ? undefined : this.host.carts.carts.get(stash.cartOf);
    const horse = stash.horseOf === null ? undefined : this.host.horses.get(stash.horseOf);
    return victim?.position ?? cart?.at ?? horse?.position ?? stash.at;
  }

  livePlayer(player) {
    return player && this.host.players.get(player.id) === player;
  }

  // A player's game asked to open a container: granted, its rows go to them and their game opens
  // its screen; refused, their game is told so.
  open(player, id, { quiet = false } = {}) {
    const stash = this.stashes.get(id);
    if (!this.livePlayer(player) || !stash) {
      if (this.livePlayer(player) && Number.isInteger(id) && id > 0) {
        this.host.bridge.send({
          op: 'stashShow',
          id: player.id,
          stash: id,
          closed: true,
          revision: 0,
          rows: [],
        });
      }
      return false;
    }
    const holder = stash.holder();
    const victim = stash.body;
    // A view of a player's inventory opens for anyone but that player, while they are here.
    const viewed = stash.viewOf === null ? null : this.host.players.get(stash.viewOf) ?? null;
    const viewOpens = stash.viewOf === null || (viewed !== null && viewed !== player);
    const allowed =
      viewOpens &&
      (holder === 0 || holder === player.id) &&
      (!stash.lock.locked || player.keyBypass === true) &&
      this.within(player, stash) &&
      !player.knockedOut &&
      (victim === null || (victim !== player && victim.knockedOut && victim.lootable)) &&
      (stash.corpseOf === null || this.lootableCorpse(stash) !== null) &&
      this.host.emit('stashInteract', player, stash, 'open');
    if (!allowed) {
      if (!quiet && holder !== 0 && holder !== player.id) {
        this.tellInUse(player);
      }
      this.show(stash, { to: player.id, closed: true });
      return false;
    }
    stash.hold(player.id);
    if (holder !== player.id) {
      this.host.emit('stashOpen', stash, player);
    }
    if (stash.corpseOf !== null) {
      // Opening a dead animal's carcass is its harvest: the yield of the server's
      // table is in it before its rows go out, once per life.
      this.host.npcs.harvests.harvest({ id: stash.corpseOf, playerId: player.id });
    }
    this.show(stash, { open: true });
    return true;
  }

  // A player tried a container another has open: the notice (Locales key 'stash.inUse'), once in a
  // while, however often they press the key.
  tellInUse(player) {
    const now = Date.now();
    if (now - (this.inUseTold.get(player.id) ?? -Infinity) < IN_USE_QUIET_MS) {
      return;
    }
    if (this.inUseTold.size >= 4096) {
      this.inUseTold.clear();
    }
    this.inUseTold.set(player.id, now);
    this.host.itemOverrides.notice(player, this.host.locales.text(player, IN_USE), {
      center: true,
    });
  }

  // The holder's screen closed, or the server ends their hold (`reason`).
  close(player, id, reason = 'closed') {
    const stash = this.stashes.get(id);
    if (!stash || !player || stash.holder() !== player.id) {
      return false;
    }
    stash.hold(0);
    if (reason !== 'closed' && this.livePlayer(player)) {
      this.show(stash, { to: player.id, closed: true });
    }
    this.host.emit('stashClose', stash, player, reason);
    return true;
  }

  // Whether the player holds the container and can still reach it; a holder who cannot loses it.
  holding(player, stash) {
    if (!this.livePlayer(player) || !stash) {
      return false;
    }
    // A horse's saddlebags are its owner's, near the horse; nobody holds them.
    if (stash.horseOf !== null) {
      return this.userOf(stash) === player.id && this.within(player, stash) && !player.knockedOut;
    }
    if (stash.holder() !== player.id) {
      return false;
    }
    if (!this.within(player, stash) || player.knockedOut) {
      this.close(player, stash.id, 'lostAccess');
      return false;
    }
    return true;
  }

  // Whether a horse's saddlebags take `amount` units of the player's `row` more and stay within
  // what the horse carries (Horse#maxCarryWeight; null: the game's own, which the server does not
  // know, so no limit here).
  horseCarries(stash, source, row, amount) {
    const horse = this.host.horses.get(stash.horseOf);
    const limit = horse?.maxCarryWeight ?? null;
    const moving = source.rows.get(row);
    if (limit === null || !moving) {
      return true;
    }
    const weightOf = (itemClass) => this.host.itemOverrides.effectiveOf(itemClass).weight;
    let carried = 0;
    for (const held of stash.stock.rows.values()) {
      carried += weightOf(held.itemClass) * held.amount;
    }
    return carried + weightOf(moving.itemClass) * Math.min(amount, moving.amount) <= limit;
  }

  // The holder moved units of a container's row to themself.
  take(player, id, row, amount) {
    const stash = this.stashes.get(id);
    if (!this.holding(player, stash) || !Number.isInteger(amount) || amount < 1) {
      return refused('invalidRequest');
    }
    let result;
    if (stash.corpseOf !== null) {
      result = this.lootCorpse(player, stash, row, amount);
    } else if (stash.viewOf !== null) {
      result = this.takeFromView(player, stash, row, amount);
    } else if (stash.bodyOf === null) {
      result = this.withdraw(player, stash, row, amount);
    } else {
      result = this.loot(player, stash, row, amount);
    }
    // Both stocks again, also after a refusal: the game moved nothing itself.
    this.show(stash);
    return result;
  }

  withdraw(player, stash, row, amount) {
    const entry = this.host.playerInventories.entryOf(player);
    if (!entry) {
      return refused('inventoryUnavailable');
    }
    const moved = transfer(
      stash.stock,
      entry.inventory,
      { units: [{ id: row, amount }] },
      {
        source: 'withdraw',
        target: 'withdraw',
      },
    );
    if (moved.change) {
      this.host.emit('stashInventoryChanged', stash, player, moved.change);
    }
    this.host.playerInventories.record(player, moved.targetChange);
    return moved.result;
  }

  // A looter takes units of a knocked-out player's row.
  loot(player, stash, row, amount) {
    const victim = stash.body;
    const from = victim ? this.host.playerInventories.entryOf(victim) : undefined;
    const to = this.host.playerInventories.entryOf(player);
    if (!victim || !from || !to || !victim.knockedOut || !victim.lootable) {
      return refused('inventoryUnavailable');
    }
    const source = from.inventory.rows.get(row);
    if (!source || isKeyring(source.itemClass)) {
      return refused('unknownItem');
    }
    const line = { item: source.itemClass.id, row, amount };
    if (!this.host.emit('playerLoot', player, victim, line)) {
      return refused('refused');
    }
    const moved = transfer(
      from.inventory,
      to.inventory,
      { units: [{ id: row, amount }] },
      {
        source: 'loot',
        target: 'loot',
      },
    );
    this.host.playerInventories.record(victim, moved.change);
    this.host.playerInventories.record(player, moved.targetChange);
    if (moved.result.ok) {
      this.host.emit('playerLooted', player, victim, [{ item: line.item, amount }]);
    }
    return moved.result;
  }

  // The holder of a view of a player's inventory (Stash.playerInventory) takes units of that
  // player's row, worn ones too: the owner's game takes off what they wore. A
  // playerInventoryTake handler may refuse each take; playerInventoryTaken follows.
  takeFromView(player, stash, row, amount) {
    const owner = this.host.players.get(stash.viewOf);
    const from = owner ? this.host.playerInventories.entryOf(owner) : undefined;
    const to = this.host.playerInventories.entryOf(player);
    if (!owner || !from || !to) {
      return refused('inventoryUnavailable');
    }
    const source = from.inventory.rows.get(row);
    if (!source || isKeyring(source.itemClass)) {
      return refused('unknownItem');
    }
    const line = { item: source.itemClass.id, row, amount, worn: source.equipped > 0 };
    if (!this.host.emit('playerInventoryTake', player, owner, line)) {
      return refused('refused');
    }
    const moved = transfer(
      from.inventory,
      to.inventory,
      { units: [{ id: row, amount }] },
      {
        source: 'take',
        target: 'take',
      },
    );
    this.host.playerInventories.record(owner, moved.change);
    this.host.playerInventories.record(player, moved.targetChange);
    if (moved.result.ok) {
      this.host.emit('playerInventoryTaken', player, owner, [{ item: line.item, amount }]);
    }
    return moved.result;
  }

  // The holder of a view that takes puts (Stash.playerInventory's put) gives units of their own
  // row to that player. A playerInventoryGive handler may refuse it; playerInventoryGiven follows.
  putIntoView(player, stash, entry, row, amount) {
    const owner = this.host.players.get(stash.viewOf);
    const to = owner ? this.host.playerInventories.entryOf(owner) : undefined;
    const source = entry.inventory.rows.get(row);
    if (!owner || !to || !source) {
      return refused(source ? 'inventoryUnavailable' : 'unknownItem');
    }
    const line = { item: source.itemClass.id, row, amount };
    if (!this.host.emit('playerInventoryGive', player, owner, line)) {
      return refused('refused');
    }
    const moved = transfer(
      entry.inventory,
      to.inventory,
      { units: [{ id: row, amount }] },
      {
        source: 'give',
        target: 'give',
      },
    );
    this.host.playerInventories.record(player, moved.change);
    this.host.playerInventories.record(owner, moved.targetChange);
    if (moved.result.ok) {
      this.host.emit('playerInventoryGiven', player, owner, [{ item: line.item, amount }]);
    }
    return moved.result;
  }

  // The dead, lootable NPC whose body the container is, or null.
  lootableCorpse(stash) {
    const npc = this.host.npcs.npcs.get(stash.corpseOf);
    return npc && !npc.alive && npc.lootable ? npc : null;
  }

  // A looter takes units of a dead NPC's row: npcLoot(looter, npc, { item, row,
  // amount }) may refuse it, npcLooted(looter, npc, [{ item, amount }]) follows, as for a player.
  lootCorpse(player, stash, row, amount) {
    const npc = this.lootableCorpse(stash);
    const from = npc?.getInventory();
    const to = this.host.playerInventories.entryOf(player);
    if (!npc || !to) {
      return refused('inventoryUnavailable');
    }
    const source = from.rows.get(row);
    if (!source) {
      return refused('unknownItem');
    }
    const line = { item: source.itemClass.id, row, amount };
    if (!this.host.emit('npcLoot', player, npc, line)) {
      return refused('refused');
    }
    const moved = transfer(
      from,
      to.inventory,
      { units: [{ id: row, amount }] },
      {
        source: 'loot',
        target: 'loot',
      },
    );
    this.host.playerInventories.record(player, moved.targetChange);
    if (moved.result.ok) {
      this.host.emit('npcLooted', player, npc, [{ item: line.item, amount }]);
    }
    return moved.result;
  }

  // The holder moved units of their own row into the container. Nothing goes into a body.
  put(player, id, row, amount) {
    const stash = this.stashes.get(id);
    if (!this.holding(player, stash) || !Number.isInteger(amount) || amount < 1) {
      return refused('invalidRequest');
    }
    const entry = this.host.playerInventories.entryOf(player);
    if (stash.viewOf !== null && stash.viewPut && entry) {
      const given = this.putIntoView(player, stash, entry, row, amount);
      if (!given.ok) {
        this.host.projectInventory(player, entry.inventory);
      }
      this.show(stash);
      return given;
    }
    const isBody = stash.bodyOf !== null || stash.corpseOf !== null || stash.viewOf !== null;
    if (isBody || !entry) {
      this.show(stash);
      return refused(isBody ? 'bodyInventory' : 'inventoryUnavailable');
    }
    if (stash.horseOf !== null && !this.horseCarries(stash, entry.inventory, row, amount)) {
      this.show(stash);
      return refused('tooHeavy');
    }
    const moved = transfer(
      entry.inventory,
      stash.stock,
      { units: [{ id: row, amount }] },
      {
        source: 'deposit',
        target: 'deposit',
      },
    );
    this.host.playerInventories.record(player, moved.change);
    if (moved.targetChange) {
      this.host.emit('stashInventoryChanged', stash, player, moved.targetChange);
    }
    // The player's own stock goes again with their next inventoryShow; if nothing moved, again
    // now, since their game showed the units gone.
    if (!moved.result.ok) {
      this.host.projectInventory(player, entry.inventory);
    }
    this.show(stash);
    return moved.result;
  }

  // Player.dropAll: the player's rows go into a container at their body (reason 'drop').
  dropAll(player, { keepEquipped = false } = {}) {
    const entry = this.livePlayer(player) ? this.host.playerInventories.entryOf(player) : undefined;
    if (!entry) {
      return null;
    }
    const units = [];
    for (const row of entry.inventory.rows.values()) {
      const amount = keepEquipped ? row.amount - row.equipped : row.amount;
      if (amount > 0 && units.length < MAX_STASH_ROWS) {
        units.push({ id: row.id, amount });
      }
    }
    if (units.length === 0) {
      return null;
    }
    if (!this.Stash) {
      this.host.makeAPI();
    }
    const feet = player.position;
    const stash = this.spawn(this.Stash, {
      position: { x: feet.x, y: feet.y, z: feet.z },
      orientation: {},
      dimension: player.dimension,
      bodyOf: null,
    });
    const moved = transfer(
      entry.inventory,
      stash.stock,
      { units },
      { source: 'drop', target: 'drop' },
    );
    this.host.playerInventories.record(player, moved.change);
    if (moved.targetChange) {
      this.host.emit('stashInventoryChanged', stash, player, moved.targetChange);
    }
    return stash;
  }

  // A player was knocked out: their body becomes a container others can search, unless they are
  // not lootable.
  knockedOut(player) {
    if (!player.lootable || this.bodies.has(player.id)) {
      return null;
    }
    if (!this.Stash) {
      this.host.makeAPI();
    }
    const body = player.position;
    const stash = this.spawn(this.Stash, {
      position: { x: body.x, y: body.y, z: body.z },
      orientation: {},
      dimension: player.dimension,
      bodyOf: player.id,
    });
    this.bodies.set(player.id, stash);
    return stash;
  }

  // A player got up or left: their body's container goes; whatever they held, they let go.
  revived(player) {
    const stash = this.bodies.get(player.id);
    if (stash) {
      this.bodies.delete(player.id);
      this.destroy(stash);
    }
    for (const held of this.stashes.values()) {
      if (held.holder() === player.id) {
        this.close(player, held.id, 'lostAccess');
      }
    }
  }

  left(player) {
    for (const held of this.stashes.values()) {
      if (held.holder() === player.id) {
        this.close(player, held.id, 'disconnected');
      }
    }
    // The views of a leaving player's inventory go with them.
    for (const view of [...this.stashes.values()]) {
      if (view.viewOf === player.id) {
        this.destroy(view);
      }
    }
    this.revived(player);
  }

  // An NPC died, was revived, removed, made (un)lootable or, with `moved`, put somewhere else:
  // its body is a container while it is gone from the living and lootable, and otherwise not.
  npcCorpse(npc, { moved = false } = {}) {
    const stash = this.corpses.get(npc.id);
    const wanted = this.host.npcs.live(npc) && !npc.alive && npc.lootable;
    if (stash && (!wanted || moved)) {
      this.destroy(stash);
    }
    if (!wanted || (stash && !moved)) {
      return;
    }
    if (!this.Stash) {
      this.host.makeAPI();
    }
    const body = npc.position;
    this.corpses.set(
      npc.id,
      this.spawn(this.Stash, {
        position: { x: body.x, y: body.y, z: body.z },
        orientation: {},
        dimension: npc.dimension,
        bodyOf: null,
        corpseOf: npc.id,
      }),
    );
  }

  // The inventory of a player whose body is a container changed: the looter sees it.
  bodyChanged(player) {
    const stash = this.bodies.get(player.id);
    if (stash && stash.holder()) {
      this.show(stash);
    }
    // And whoever has a view of their inventory open (Stash.playerInventory).
    for (const view of this.stashes.values()) {
      if (view.viewOf === player.id && view.holder()) {
        this.show(view);
      }
    }
  }

  // The chests and virtual stock a stopping resource made go with it.
  resourceStopped(name) {
    removeOwned(this.stashes.values(), name, (stash) => this.destroy(stash));
  }

  destroy(stash) {
    if (this.stashes.get(stash.id) !== stash) {
      return;
    }
    const holder = this.host.players.get(stash.holder());
    if (holder) {
      this.close(holder, stash.id, 'destroyed');
    }
    this.host.emit('stashDestroy', stash);
    this.stashes.delete(stash.id);
    if (stash.bodyOf !== null && this.bodies.get(stash.bodyOf) === stash) {
      this.bodies.delete(stash.bodyOf);
    }
    if (stash.corpseOf !== null && this.corpses.get(stash.corpseOf) === stash) {
      this.corpses.delete(stash.corpseOf);
    }
    this.host.bridge.send({ op: 'stashDestroy', id: stash.id });
  }
}

module.exports = { Stashes, MAX_STASH_ROWS };
