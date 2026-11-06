'use strict';

// Items carried in the arms: Player.holdItem, dropHeldItem and heldItem, the
// playerCarryingItem, playerCarryItem and playerPutDownItem events, and carryable props.
//
// holdItem tells the player's client to carry a new item of a class (the op carryOrder, kind 1,
// with the class GUID) or to take up a carryable ground item (kind 4, with the stack); dropHeldItem
// to put it down with the game's put-down (kind 2) or let go at once (kind 3). The client reports
// what its player carries in each player report (`carry`, the class GUID, "" for none) and, when
// a carry lets its item go in the world, where it lies (the op carryPutDown): a carryable ground
// item is laid there. A player who takes up a carryable ground item with the game's pick-up is
// asked about first (playerCarryingItem).

const { lookupItemClass } = require('../inventory/catalog.cjs');
const { vector } = require('../math.cjs');

// Carryable items by name, those with a walk animation: their class GUID, and whether only a
// man's body has one (the sacks, the barrel and the silver crate).
const CARRYABLE = Object.freeze({
  basket_full_wood: Object.freeze({ item: 'f864a24a-152c-462e-a8f3-6760f7ce7329', men: false }),
  basket_b_apples: Object.freeze({ item: '33195061-1fe6-48d2-adfa-6f7d04d17503', men: false }),
  stonebasket: Object.freeze({ item: '8ff44ee6-55ad-4796-9193-c2402bf22633', men: false }),
  eggbasket: Object.freeze({ item: '8ff44ee6-55ad-4796-9193-c2402bf22634', men: false }),
  shoppingbasket: Object.freeze({ item: '8ff44ee6-55ad-4796-9193-c2402bf22635', men: false }),
  firewoodChipsHand: Object.freeze({ item: '9baf48df-6673-4131-9a46-34770b44009e', men: false }),
  waterBucket: Object.freeze({ item: '979b2602-f161-4b9d-8774-738119196ba1', men: false }),
  milkBucket: Object.freeze({ item: '103d0347-3728-471d-84ec-738eb19d9fe1', men: false }),
  sack: Object.freeze({ item: '8519a72d-b2da-4938-8aed-2df96e8e1b34', men: true }),
  sack_miller: Object.freeze({ item: '596ccaec-2415-4754-8a39-eaaccb27361b', men: true }),
  barrel: Object.freeze({ item: 'fa316dc5-9168-4e0f-b330-1d8b3bcb6ff0', men: true }),
  crate_with_silver: Object.freeze({ item: '53ac7c3d-6cbf-4403-b861-086cb038604a', men: true }),
  // The game's other carryable items (EntityScript CarryableItem in its item tables), as its sack.
  kejkliri_sackcarryable: Object.freeze({
    item: '048fff11-3356-4cef-867e-a15a8b79d6f2',
    men: true,
  }),
  klasterniTajemstvi_sackcarryable: Object.freeze({
    item: '31c87d7f-6d8c-4285-80e7-5b69878ac025',
    men: true,
  }),
  kralovskeStribro_sackcarryable: Object.freeze({
    item: 'b9474efa-28a7-44e5-8a6b-23f2f3d5e121',
    men: true,
  }),
  magickySip_sackcarryable: Object.freeze({
    item: 'd277dee6-cc66-4199-9be1-9489c433cd7d',
    men: true,
  }),
  mlynaruvUcen_sackcarryable: Object.freeze({
    item: '19ef3d80-59c4-4f20-bee6-d915aaabefb4',
    men: true,
  }),
  naTroskach_sackcarryable: Object.freeze({
    item: '36af95a7-6be3-47c8-a2fd-6bff523492a0',
    men: true,
  }),
  nebakovObrana_sackcarryable: Object.freeze({
    item: '70b471e1-8770-4bfc-bac2-65e46d39d4b6',
    men: true,
  }),
  poustevnik_sackcarryable: Object.freeze({
    item: 'c3463bc9-3e00-4eaf-89d3-c9bdd4c31619',
    men: true,
  }),
  rasuvUcen_sackcarryable: Object.freeze({
    item: 'c25a4e91-550e-4fa1-a46d-071ee4760f4d',
    men: true,
  }),
  setkaniVRatbori_copperJug: Object.freeze({
    item: '919c26be-59eb-45e5-ba63-9938e9bc7719',
    men: true,
  }),
  setkaniVRatbori_copperJug_big: Object.freeze({
    item: 'a4e0109a-8f79-45af-b21a-2c34a34edef9',
    men: true,
  }),
  setkaniVRatbori_tinJug: Object.freeze({
    item: '91899c3f-36b5-44c2-a229-7f0dca4de277',
    men: true,
  }),
  setkaniVRatbori_tinJug_big: Object.freeze({
    item: 'c854ba94-bbed-4611-bc87-8f499dd951be',
    men: true,
  }),
  socky_sackcarryable: Object.freeze({ item: '59198cec-bad7-4f70-aa44-f6721433d197', men: true }),
  uchazec_sackcarryableAnvil: Object.freeze({
    item: 'c43f2eff-1908-41b7-9804-beb8b7e25e3b',
    men: true,
  }),
  uchazec_sackcarryableBed: Object.freeze({
    item: '9d976ea7-b024-4161-9a86-f2f5eb85727e',
    men: true,
  }),
  uchazec_sackcarryableGate: Object.freeze({
    item: 'b92a1615-54d2-416d-8cfd-59736cd28c6b',
    men: true,
  }),
  uchazec_sackcarryableWindow: Object.freeze({
    item: 'f2adaa07-0fbe-47dc-a2a3-3bc629c858a6',
    men: true,
  }),
});

const CARRY_ORDER = Object.freeze({ carry: 1, putDown: 2, letGo: 3, carryStack: 4 });

// How long an order to take up a ground item holds it for that player, in milliseconds.
const PENDING_MS = 5000;
// How far from the player a ground item may lie to be taken up, and an item they let go of to
// be laid, in metres.
const TAKE_REACH = 6;
const PUT_DOWN_REACH = 5;
const GLOBAL_WORLD = 0xffffffff;

const NAME_OF_GUID = new Map(
  Object.entries(CARRYABLE).map(([name, entry]) => {
    return [entry.item, name];
  }),
);

// The carryable entry of an item name, or null.
function carryableOf(name) {
  return Object.prototype.hasOwnProperty.call(CARRYABLE, name) ? CARRYABLE[name] : null;
}

// The class a carryable ground item is laid as, by name or GUID; null for anything else.
function carryClassOf(item) {
  const name = typeof item === 'string' ? NAME_OF_GUID.get(item.toLowerCase()) ?? item : null;
  const carryable = name === null ? null : carryableOf(name);
  if (!carryable) {
    return null;
  }
  return Object.freeze({
    id: carryable.item,
    name,
    category: 'carry',
    maximumQuality: 1,
    stackable: false,
    qualityBands: false,
  });
}

// The name of the item a report's `carry` names: a carryable's, else the catalog's, else the
// GUID itself; null for none.
function carriedName(guid) {
  if (typeof guid !== 'string' || guid === '') {
    return null;
  }
  const lower = guid.toLowerCase();
  if (NAME_OF_GUID.has(lower)) {
    return NAME_OF_GUID.get(lower);
  }
  return lookupItemClass(lower)?.name ?? lower;
}

class Carries {
  // `host`: the resource host (bridge, emit, players, horses, groundItems).
  constructor(host) {
    this.host = host;
    // player -> the name of the item they carry
    this.carried = new WeakMap();
    // stack id -> { player, at }: a ground item a player was told to take up
    this.pending = new Map();
    // player -> the item their client reported let go of, which its reports may still name for
    // a moment
    this.letGo = new WeakMap();
  }

  heldItem(player) {
    return this.carried.get(player) ?? null;
  }

  // Player.holdItem: a new item by name, or a carryable ground item.
  holdItem(player, item) {
    this.host.requireLivePlayerCall(player);
    if (item !== null && typeof item === 'object' && this.isStack(item)) {
      return this.takeUp(player, item, { ask: false });
    }
    if (typeof item !== 'string') {
      throw new TypeError('holdItem takes an item name or a GroundItem');
    }
    const carryable = carryableOf(item);
    if (!carryable) {
      throw new TypeError(`No carrying walk for item: ${item}`);
    }
    if (!this.walksWith(player, carryable)) {
      throw new TypeError(`No carrying walk for item ${item} on this body`);
    }
    if (!this.canCarry(player)) {
      return false;
    }
    this.host.bridge.send({
      op: 'carryOrder',
      id: player.id,
      kind: CARRY_ORDER.carry,
      item: carryable.item,
    });
    return true;
  }

  // Has the player take up a carryable ground item: asked about first when the player's own
  // game started it (`ask`). False when it is gone, out of reach or another world's, someone
  // else is taking it, they cannot carry, or a handler refused.
  takeUp(player, stack, { ask }) {
    if (!stack.carryable || !this.isStack(stack) || !this.canCarry(player)) {
      return false;
    }
    if (stack.dimension !== GLOBAL_WORLD && stack.dimension !== player.dimension) {
      return false;
    }
    const where = player.position;
    const at = stack.position;
    if (Math.hypot(where.x - at.x, where.y - at.y, where.z - at.z) > TAKE_REACH) {
      return false;
    }
    const taking = this.pending.get(stack.id);
    if (taking && taking.player !== player && Date.now() - taking.at < PENDING_MS) {
      return false;
    }
    const name = stack.klass.name;
    const carryable = carryableOf(name);
    if (carryable && !this.walksWith(player, carryable)) {
      return false;
    }
    if (ask && !this.host.emit('playerCarryingItem', player, name, stack)) {
      return false;
    }
    this.pending.set(stack.id, { player, at: Date.now() });
    this.host.bridge.send({
      op: 'carryOrder',
      id: player.id,
      kind: CARRY_ORDER.carryStack,
      stack: stack.id,
    });
    return true;
  }

  // Player.dropHeldItem.
  dropHeldItem(player, options) {
    if (options === null || typeof options !== 'object') {
      throw new TypeError('dropHeldItem options must be an object');
    }
    this.host.requireLivePlayerCall(player);
    if (!player.ready || !this.carried.get(player)) {
      return false;
    }
    const kind = options.immediate ? CARRY_ORDER.letGo : CARRY_ORDER.putDown;
    this.host.bridge.send({ op: 'carryOrder', id: player.id, kind });
    return true;
  }

  // What the player's client reports it carries (a class GUID, "" for none):
  // playerPutDownItem for the item that went, then playerCarryItem for the one that came, with
  // the ground item it was taken up from, which goes then.
  update(player, guid) {
    const item = carriedName(guid);
    if (item !== null && this.letGo.get(player) === item) {
      return;
    }
    this.letGo.delete(player);
    const before = this.carried.get(player) ?? null;
    if (before === item) {
      return;
    }
    if (item) {
      this.carried.set(player, item);
    } else {
      this.carried.delete(player);
    }
    if (before) {
      this.host.emit('playerPutDownItem', player, before, null);
    }
    if (!item) {
      return;
    }
    const stack = this.takenStack(player, item);
    this.host.emit('playerCarryItem', player, item, stack);
    if (stack) {
      this.pending.delete(stack.id);
      this.host.emit('groundItemPickup', stack, player);
      this.host.groundItems.destroy(stack);
    }
  }

  // The player's client let the carried item go at `position` (the op carryPutDown): a
  // carryable ground item is laid there, and playerPutDownItem hands it over. Too far from the
  // player, or not a carryable class, nothing is laid.
  putDown(player, { item: guid, position }) {
    const before = player ? this.carried.get(player) : null;
    if (!before || carriedName(guid) !== before) {
      return;
    }
    this.carried.delete(player);
    this.letGo.set(player, before);
    let prop = null;
    const at = position ? vector(position) : null;
    const where = player.position;
    const near = at && Math.hypot(where.x - at.x, where.y - at.y, where.z - at.z) <= PUT_DOWN_REACH;
    if (near && carryClassOf(before)) {
      prop = this.host.groundItems.layCarryable(before, at, player);
    }
    this.host.emit('playerPutDownItem', player, before, prop);
  }

  // The player left: their carry is over.
  left(player) {
    this.update(player, '');
    for (const [id, taking] of this.pending) {
      if (taking.player === player) {
        this.pending.delete(id);
      }
    }
  }

  // A ground item went.
  stackGone(id) {
    this.pending.delete(id);
  }

  canCarry(player) {
    const riding = [...this.host.horses.values()].some((horse) => horse.riderId === player.id);
    if (!player.ready || player.knockedOut || !(player.health > 0) || riding) {
      return false;
    }
    if (this.carried.get(player)) {
      return false;
    }
    for (const taking of this.pending.values()) {
      if (taking.player === player && Date.now() - taking.at < PENDING_MS) {
        return false;
      }
    }
    return true;
  }

  walksWith(player, carryable) {
    return !(carryable.men && player.appearance?.gender === 'female');
  }

  isStack(item) {
    return this.host.groundItems.stacks.get(item.id) === item;
  }

  // The ground item the player was told to take up, if it is of the item they carry now.
  takenStack(player, item) {
    for (const [id, taking] of this.pending) {
      const stack = this.host.groundItems.stacks.get(id);
      if (taking.player === player && stack && stack.klass.name === item) {
        return stack;
      }
    }
    return null;
  }
}

module.exports = { CARRYABLE, CARRY_ORDER, Carries, carryClassOf, carryableOf, carriedName };
