'use strict';

// Quests: the server API's Quest, a journal entry written
// into the game of the players it is given to. The server holds the quest, its progress and its
// objectives; a client only shows them. A quest goes to its one owner (`player`) or to every
// player in its dimension (the global world is every world), is sent again when it changes and
// again when a player's world is ready again, so a player who joins late or reloads still finds it
// in their journal (markers/presence.cjs decides who is shown what).
//
// Following a quest (track, untrack) is the player's: the server tells the client to follow it or
// to stop, the client reports what it then follows, and the report fires questTrackingChanged
// (quest, player, tracked), only on a change and only for a quest that player has. What a player
// follows is kept, so it is restored with the journal when their world is ready again.
//
// The ops sent to a player's client:
//   { op: 'questShow', id: <player id>, quest, key, title, description, type, progress, announce,
//     objectives: [{ text, progress, optional, position: { x, y, z } | null }] }
//   { op: 'questHide', id, quest }
//   { op: 'questTrack', id, quest, tracked }
// and the report from a client: { op: 'questTracking', id, quest, tracked }.
//
// A resource's quests go when it stops; a player's own quests go when they leave the server.

const { GLOBAL_WORLD, plain, positionOf, worldOf } = require('../markers/values.cjs');
const { validWorld } = require('../npcs/npc_values.cjs');

const questState = Symbol('kcds.questState');

const KEY_PATTERN = /^[A-Za-z0-9_-]{1,64}$/;
const QUEST_TYPES = ['main', 'side', 'activity', 'event', 'micro', 'racing'];
const QUEST_PROGRESS = ['active', 'done', 'failed'];
const MAX_OBJECTIVES = 8;
// What travels to the clients, in UTF-8 bytes (kcd2s/integration/quests.hpp).
const TITLE_BYTES = 256;
const DESCRIPTION_BYTES = 4096;
const OBJECTIVE_BYTES = 512;
const encoder = new TextEncoder();

// Text cut to `bytes` bytes of UTF-8 without splitting a character. Control characters become
// spaces; a description keeps its line breaks and tabs.
function textOf(value, bytes, lines, name) {
  if (typeof value !== 'string') {
    throw new TypeError(`${name} takes a string`);
  }
  const kept = lines ? /[\n\r\t]/ : null;
  let text = '';
  let used = 0;
  for (const character of value) {
    const code = character.codePointAt(0);
    let next = character;
    if ((code < 0x20 || code === 0x7f) && !(kept && kept.test(character))) {
      next = ' ';
    }
    const size = encoder.encode(next).length;
    if (used + size > bytes) {
      break;
    }
    text += next;
    used += size;
  }
  return text;
}

function progressOf(value, name) {
  if (!QUEST_PROGRESS.includes(value)) {
    throw new RangeError(`${name} takes 'active', 'done' or 'failed'`);
  }
  return value;
}

function announceOf(value, name) {
  if (value === undefined) {
    return true;
  }
  if (typeof value !== 'boolean') {
    throw new TypeError(`${name} takes true or false for announce`);
  }
  return value;
}

// An objective as text or as { text, progress, optional, position }.
function objectiveOf(value, name) {
  if (typeof value === 'string') {
    return {
      text: textOf(value, OBJECTIVE_BYTES, false, name),
      progress: 'active',
      optional: false,
      position: null,
    };
  }
  if (value === null || typeof value !== 'object') {
    throw new TypeError(
      `${name} takes an objective as text or as { text, progress, optional, position }`,
    );
  }
  const optional = value.optional ?? false;
  if (typeof optional !== 'boolean') {
    throw new TypeError(`${name} takes true or false for optional`);
  }
  return {
    text: textOf(value.text, OBJECTIVE_BYTES, false, `${name} text`),
    progress: value.progress === undefined ? 'active' : progressOf(value.progress, name),
    optional,
    position:
      value.position === undefined || value.position === null
        ? null
        : positionOf(value.position, name),
  };
}

// At most MAX_OBJECTIVES of them; anything past that is dropped.
function objectivesOf(value, name) {
  if (!Array.isArray(value)) {
    throw new TypeError(`${name} takes a list of objectives`);
  }
  return value.slice(0, MAX_OBJECTIVES).map((entry) => objectiveOf(entry, name));
}

function sameObjective(a, b) {
  if (a.text !== b.text || a.progress !== b.progress || a.optional !== b.optional) {
    return false;
  }
  if (a.position === null || b.position === null) {
    return a.position === b.position;
  }
  return (
    a.position.x === b.position.x && a.position.y === b.position.y && a.position.z === b.position.z
  );
}

function playerIdOf(value, name) {
  if (value !== null && typeof value === 'object' && Number.isSafeInteger(value.id)) {
    return value.id;
  }
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new TypeError(`${name} takes the network id of a player`);
  }
  return value;
}

class Quests {
  constructor(host, presence) {
    this.host = host;
    this.presence = presence;
    // quest id -> state, in the order they were given
    this.byId = new Map();
    presence.add(this);
  }

  // The states, for the presence.
  objects() {
    return this.byId.values();
  }

  // The handle of the quest with this entity id, or undefined.
  find(id) {
    return this.byId.get(id)?.handle;
  }

  message(player, state, announce) {
    return {
      op: 'questShow',
      id: player.id,
      quest: state.id,
      key: state.key,
      title: state.title,
      description: state.description,
      type: state.type,
      progress: state.progress,
      announce,
      objectives: state.objectives.map((objective) => ({
        text: objective.text,
        progress: objective.progress,
        optional: objective.optional,
        position: objective.position === null ? null : plain(objective.position),
      })),
    };
  }

  // The presence shows the quest to a player: it is new to them (given now, their world is ready
  // again, or they came into its world). What they follow comes with it.
  show(player, state) {
    this.host.bridge.send(this.message(player, state, state.announcing));
    if (state.tracked.has(player.id)) {
      this.host.bridge.send({ op: 'questTrack', id: player.id, quest: state.id, tracked: true });
    }
  }

  hide(player, state) {
    this.host.bridge.send({ op: 'questHide', id: player.id, quest: state.id });
  }

  // The quest changed: every player who has it is sent the whole of it again.
  refresh(state, announce) {
    for (const id of state.shownTo) {
      const player = this.host.players.get(id);
      if (player) {
        this.host.bridge.send(this.message(player, state, announce));
      }
    }
  }

  // Whether a connected player has the quest, ready or not: they own it, or it is for everyone and
  // they are in its world.
  has(state, player) {
    if (state.player !== 0 && state.player !== player.id) {
      return false;
    }
    return state.world === GLOBAL_WORLD || state.world === player.dimension;
  }

  // The Quest class of the server API, an Entity.
  define(Entity) {
    const registry = this;
    class Quest {
      constructor(id) {
        const handle = registry.find(id);
        if (!handle) {
          throw new Error('Unknown quest');
        }
        return handle;
      }
      get exists() {
        return this[questState].alive;
      }
      get key() {
        return this[questState].key;
      }
      get title() {
        return this[questState].title;
      }
      set title(value) {
        registry.requireLive(this);
        registry.change(
          this[questState],
          { title: textOf(value, TITLE_BYTES, false, 'Quest.title') },
          false,
        );
      }
      get description() {
        return this[questState].description;
      }
      set description(value) {
        registry.requireLive(this);
        const description = textOf(value, DESCRIPTION_BYTES, true, 'Quest.description');
        registry.change(this[questState], { description }, false);
      }
      get type() {
        return this[questState].type;
      }
      get progress() {
        return this[questState].progress;
      }
      set progress(value) {
        this.setProgress(value, true);
      }
      get player() {
        return this[questState].player;
      }
      get objectiveCount() {
        return this[questState].objectives.length;
      }
      get dimension() {
        return this[questState].world;
      }
      toString() {
        const state = this[questState];
        return `Quest(${state.id}, ${state.key}, ${state.progress}, ${state.objectives.length} objectives)`;
      }
      remove() {
        if (this[questState].alive) {
          registry.remove(this[questState]);
        }
      }
      setProgress(progress, announce) {
        registry.requireLive(this);
        const value = progressOf(progress, 'Quest.setProgress');
        registry.change(
          this[questState],
          { progress: value },
          announceOf(announce, 'Quest.setProgress'),
        );
      }
      setObjective(index, objective, announce) {
        registry.requireLive(this);
        if (!Number.isInteger(index) || index < 0) {
          throw new TypeError('Quest.setObjective takes an index counting from zero');
        }
        const value = objectiveOf(objective, 'Quest.setObjective');
        const shout = announceOf(announce, 'Quest.setObjective');
        const state = this[questState];
        // An index past the end is ignored.
        if (index >= state.objectives.length) {
          return;
        }
        const objectives = [...state.objectives];
        objectives[index] = value;
        registry.change(state, { objectives }, shout);
      }
      setObjectives(objectives, announce) {
        registry.requireLive(this);
        const list = objectivesOf(objectives, 'Quest.setObjectives');
        registry.change(
          this[questState],
          { objectives: list },
          announceOf(announce, 'Quest.setObjectives'),
        );
      }
      objectives() {
        return this[questState].objectives.map((objective) => {
          const entry = {
            text: objective.text,
            progress: objective.progress,
            optional: objective.optional,
          };
          if (objective.position !== null) {
            entry.position = objective.position.clone();
          }
          return entry;
        });
      }
      track(player) {
        registry.requireLive(this);
        return registry.follow(this[questState], player, true, 'Quest.track');
      }
      untrack(player) {
        registry.requireLive(this);
        return registry.follow(this[questState], player, false, 'Quest.untrack');
      }
      static give(key, title, options, dimension) {
        return registry.give(Quest, key, title, options, dimension);
      }
      static all(dimension) {
        return registry.list(dimension);
      }
      static find(key, dimension, player) {
        return registry.lookup(key, dimension, player);
      }
      static getById(id) {
        return registry.find(id) ?? null;
      }
      static removeAll(dimension) {
        return registry.removeAll(dimension);
      }
    }
    Object.setPrototypeOf(Quest.prototype, Entity.prototype);
    return Quest;
  }

  requireLive(handle) {
    if (!handle[questState].alive) {
      throw new Error('The quest was removed');
    }
  }

  give(Quest, key, title, options, dimension) {
    if (typeof key !== 'string' || !KEY_PATTERN.test(key)) {
      throw new TypeError(
        'Quest.give takes a key of 1 to 64 letters, digits, underscores or dashes',
      );
    }
    const settings = options ?? {};
    if (settings === null || typeof settings !== 'object') {
      throw new TypeError('Quest.give takes an options object');
    }
    const type = settings.type ?? 'side';
    if (!QUEST_TYPES.includes(type)) {
      throw new RangeError(`Quest.give takes a type of ${QUEST_TYPES.join(', ')}`);
    }
    const world = worldOf(dimension, 'Quest.give');
    const owner = settings.player === undefined ? 0 : playerIdOf(settings.player, 'Quest.give');
    if (owner !== 0 && !this.host.players.has(owner)) {
      throw new TypeError('Quest.give takes the network id of a connected player');
    }
    if (settings.track !== undefined && typeof settings.track !== 'boolean') {
      throw new TypeError('Quest.give takes true or false for track');
    }
    const state = {
      id: 0,
      handle: null,
      key,
      title: textOf(title, TITLE_BYTES, false, 'Quest.give title'),
      description:
        settings.description === undefined
          ? ''
          : textOf(settings.description, DESCRIPTION_BYTES, true, 'Quest.give description'),
      type,
      progress: 'active',
      objectives:
        settings.objectives === undefined ? [] : objectivesOf(settings.objectives, 'Quest.give'),
      player: owner,
      world,
      viewers: owner === 0 ? null : new Set([owner]),
      shownTo: new Set(),
      // The players the server makes follow it, and those whose client reported following it.
      tracked: new Set(),
      following: new Set(),
      announcing: false,
      resource: this.host.context.getStore()?.name,
      alive: true,
    };
    const announce = announceOf(settings.announce, 'Quest.give');
    for (const other of this.byId.values()) {
      if (other.key === key && other.world === world && other.player === owner) {
        throw new Error(`A quest with the key ${key} already exists`);
      }
    }
    const handle = Object.create(Quest.prototype);
    Object.defineProperty(handle, 'id', { value: this.host.allocateEntityId(), enumerable: true });
    state.id = handle.id;
    state.handle = handle;
    handle[questState] = state;
    this.byId.set(state.id, state);
    if (settings.track === true) {
      for (const player of this.host.players.values()) {
        if (this.has(state, player)) {
          state.tracked.add(player.id);
        }
      }
    }
    state.announcing = announce;
    this.presence.reconcile(this, state);
    state.announcing = false;
    return handle;
  }

  // Applies changed fields and sends the quest again to the players who have it.
  change(state, fields, announce) {
    const differs = Object.entries(fields).some(([name, value]) => {
      if (name === 'objectives') {
        const now = state.objectives;
        return (
          now.length !== value.length || now.some((entry, i) => !sameObjective(entry, value[i]))
        );
      }
      return state[name] !== value;
    });
    if (!differs) {
      return;
    }
    Object.assign(state, fields);
    this.refresh(state, announce);
  }

  // track / untrack: the targets are one player who has the quest, or everyone who has it. Returns
  // how many clients were told now; one whose world is not ready is told when it is.
  follow(state, target, tracked, name) {
    const targets = [];
    if (target === undefined) {
      for (const player of this.host.players.values()) {
        if (this.has(state, player)) {
          targets.push(player);
        }
      }
    } else {
      const player = this.host.players.get(playerIdOf(target, name));
      if (!player || !this.has(state, player)) {
        return 0;
      }
      targets.push(player);
    }
    let told = 0;
    for (const player of targets) {
      if (tracked) {
        state.tracked.add(player.id);
      } else {
        state.tracked.delete(player.id);
      }
      if (state.shownTo.has(player.id)) {
        this.host.bridge.send({ op: 'questTrack', id: player.id, quest: state.id, tracked });
        told += 1;
      }
    }
    return told;
  }

  // questTracking: the player's client follows the quest now, or no longer. Only a quest that player
  // has, and only a change, fires questTrackingChanged; what they follow is kept for their next
  // world.
  trackingReported(player, quest, tracked) {
    const state = this.byId.get(quest);
    if (!player || !state || !state.alive || typeof tracked !== 'boolean') {
      return;
    }
    if (!state.shownTo.has(player.id)) {
      return;
    }
    // What the player follows is theirs: it is what their next world gets.
    if (tracked) {
      state.tracked.add(player.id);
    } else {
      state.tracked.delete(player.id);
    }
    if (state.following.has(player.id) === tracked) {
      return;
    }
    if (tracked) {
      state.following.add(player.id);
    } else {
      state.following.delete(player.id);
    }
    this.host.emit('questTrackingChanged', state.handle, player, tracked);
  }

  remove(state) {
    state.alive = false;
    this.presence.withdraw(this, state);
    this.byId.delete(state.id);
  }

  list(dimension) {
    if (dimension !== undefined && !validWorld(dimension)) {
      throw new TypeError('Invalid dimension');
    }
    const found = [];
    for (const state of this.byId.values()) {
      if (dimension === undefined || state.world === dimension) {
        found.push(state.handle);
      }
    }
    return found;
  }

  lookup(key, dimension, player) {
    if (typeof key !== 'string') {
      throw new TypeError('Quest.find takes a key');
    }
    if (dimension !== undefined && !validWorld(dimension)) {
      throw new TypeError('Invalid dimension');
    }
    const owner = player === undefined ? undefined : playerIdOf(player, 'Quest.find');
    for (const state of this.byId.values()) {
      if (state.key !== key) {
        continue;
      }
      if (dimension !== undefined && state.world !== dimension) {
        continue;
      }
      if (owner !== undefined && state.player !== owner) {
        continue;
      }
      return state.handle;
    }
    return null;
  }

  removeAll(dimension) {
    let removed = 0;
    for (const handle of this.list(dimension)) {
      this.remove(handle[questState]);
      removed += 1;
    }
    return removed;
  }

  // The player left the server: their own quests go, and they follow nothing any more.
  left(id) {
    for (const state of [...this.byId.values()]) {
      state.tracked.delete(id);
      state.following.delete(id);
      if (state.player === id) {
        state.shownTo.delete(id);
        this.remove(state);
      }
    }
  }

  // The resource stopped or failed: the quests it gave go.
  resourceStopped(name) {
    for (const state of [...this.byId.values()]) {
      if (state.resource === name) {
        this.remove(state);
      }
    }
  }
}

module.exports = { Quests, QUEST_TYPES };
