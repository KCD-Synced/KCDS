'use strict';

// The client-only EmoteWheel global: the shipped emote catalog and which gesture each
// slot of this client's wheel chooses. Every gesture is one of the game's own animations
// (Animations.list): the fragment and tags below, with the duration the game's catalog gives for
// the male body. The game has the nods for a woman without a hand tag and for a man only with one
// (femaleTags).
//
//   EmoteWheel.getCatalog()          -> [{ id, name, duration, fullBody, fragment, tags,
//                                       femaleTags }] in catalog order
//   EmoteWheel.slotTotal()        -> 6
//   EmoteWheel.getSlots()            -> one catalog id per slot, clockwise from straight up
//   EmoteWheel.assignSlot(slot, id)     -> false (nothing changes) for a bad slot or id
//   EmoteWheel.assignSlots(ids)         -> false (nothing changes) for a wrong length or a bad id
//   EmoteWheel.reset()               -> the stock wheel: wave, big wave, nod, respectful nod, bow,
//                                       cheer
//
// The layout is local to this machine and lasts for the session; the resources on it share one
// wheel, as the player has one.

const CATALOG = Object.freeze([
  {
    id: 1,
    name: 'Wave',
    fragment: 'GreetingsUpperBody',
    tags: 'l_noweapon+waveSmall',
    duration: 2.833,
    fullBody: false,
  },
  {
    id: 2,
    name: 'Big wave',
    fragment: 'GreetingsUpperBody',
    tags: 'l_noweapon+waveBig',
    duration: 4.2,
    fullBody: false,
  },
  {
    id: 3,
    name: 'Nod',
    fragment: 'GreetingsUpperBody',
    tags: 'l_noweapon+nod',
    femaleTags: 'nod',
    duration: 2.233,
    fullBody: false,
  },
  {
    id: 4,
    name: 'Respectful nod',
    fragment: 'GreetingsUpperBody',
    tags: 'l_noweapon+nodSubmissive',
    femaleTags: 'nodSubmissive',
    duration: 1.767,
    fullBody: false,
  },
  { id: 5, name: 'Bow', fragment: 'Greetings', tags: 'bowBig', duration: 3.033, fullBody: true },
  { id: 6, name: 'Cheer', fragment: 'ApplauseHappy', tags: '', duration: 8.267, fullBody: true },
  {
    id: 7,
    name: 'Sad applause',
    fragment: 'ApplauseSad',
    tags: '',
    duration: 8.167,
    fullBody: true,
  },
  { id: 8, name: 'Point', fragment: 'Pointing', tags: '', duration: 8.567, fullBody: true },
]);

const STOCK_WHEEL = Object.freeze([1, 2, 3, 4, 5, 6]);
const SLOT_COUNT = STOCK_WHEEL.length;
const BY_ID = new Map(CATALOG.map((entry) => [entry.id, entry]));

class EmoteWheelState {
  constructor() {
    this.slots = [...STOCK_WHEEL];
  }

  // A fresh public entry, with the game animation it plays (so a script plays the same
  // emote on an NPC with npc.playAnimation(fragment, { tags })); femaleTags are the tags on a
  // woman's body.
  static describe(entry) {
    return {
      id: entry.id,
      name: entry.name,
      duration: entry.duration,
      fullBody: entry.fullBody,
      fragment: entry.fragment,
      tags: entry.tags,
      femaleTags: entry.femaleTags ?? entry.tags,
    };
  }

  getCatalog() {
    return CATALOG.map((entry) => EmoteWheelState.describe(entry));
  }

  assignSlot(slot, emoteId) {
    if (!Number.isInteger(slot) || slot < 0 || slot >= SLOT_COUNT || !BY_ID.has(emoteId)) {
      return false;
    }
    this.slots[slot] = emoteId;
    return true;
  }

  assignSlots(emoteIds) {
    if (!Array.isArray(emoteIds) || emoteIds.length !== SLOT_COUNT) {
      return false;
    }
    if (!emoteIds.every((id) => BY_ID.has(id))) {
      return false;
    }
    this.slots = [...emoteIds];
    return true;
  }

  reset() {
    this.slots = [...STOCK_WHEEL];
  }

  // The game animation a catalog id plays on a 'male' or 'female' body: { fragment, tags }, or
  // null.
  animationOf(emoteId, gender = 'male') {
    const entry = BY_ID.get(emoteId);
    if (!entry) {
      return null;
    }
    const tags =
      gender === 'female' && entry.femaleTags !== undefined ? entry.femaleTags : entry.tags;
    return { fragment: entry.fragment, tags };
  }

  facade() {
    return Object.freeze({
      slotTotal: () => SLOT_COUNT,
      getSlots: () => [...this.slots],
      assignSlot: (slot, emoteId) => this.assignSlot(slot, emoteId),
      assignSlots: (emoteIds) => this.assignSlots(emoteIds),
      reset: () => this.reset(),
      getCatalog: () => this.getCatalog(),
    });
  }
}

module.exports = { EmoteWheelState, CATALOG, STOCK_WHEEL, SLOT_COUNT };
