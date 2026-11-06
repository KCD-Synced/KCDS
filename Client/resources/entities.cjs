'use strict';

const { EntityStateStore } = require('../../Backend/scripting/runtimes/js/shared_state.cjs');
const {
  Vector3,
  Quaternion,
  vector,
  rotation,
} = require('../../Backend/scripting/runtimes/js/math.cjs');
const {
  PlayerStat,
  DerivedPlayerStat,
  NO_STATS,
  statsFromDirectory,
  getStat,
  derivedStat,
  scaledStats,
  defineReadings,
} = require('../../Backend/scripting/runtimes/js/stats/player_stats.cjs');
const {
  progressionFromDirectory,
  levelsOf,
  perkNames,
  hasPerk,
  trackProgress,
} = require('../../Backend/scripting/runtimes/js/progression/player_progression.cjs');
const {
  NO_POSTURE,
  postureFromDirectory,
  definePostureReadings,
} = require('../../Backend/scripting/runtimes/js/posture/player_posture.cjs');
const bindings = new WeakMap();
const ZERO = Object.freeze({ x: 0, y: 0, z: 0 });
// A Mannequin tag this machine's animation system has not set.
const NO_TAG = 255;

function validId(id) {
  return Number.isInteger(id) && id > 0 && id <= 0xffffffff;
}

// A player's published look: a gender and four character-component names.
const APPEARANCE_PARTS = ['body', 'head', 'hair', 'beard'];
const NAME_MAX = 64;
const DISGUISE_MAX = 255;
// A player's name: 32 characters of up to four UTF-8 bytes each (player_identity.hpp).
const NICKNAME_MAX = 128;
const NO_PLAYER_INDEX = 0xffff;

function appearanceOf(value) {
  if (
    !value ||
    typeof value !== 'object' ||
    (value.gender !== 'male' && value.gender !== 'female') ||
    APPEARANCE_PARTS.some(
      (part) => typeof value[part] !== 'string' || value[part].length > NAME_MAX,
    )
  )
    throw new TypeError('Invalid native player appearance');
  return Object.freeze({
    gender: value.gender,
    body: value.body,
    head: value.head,
    hair: value.hair,
    beard: value.beard,
  });
}

// What a player's body wears and holds drawn: item classes as scripts compare them, 32
// lower-case hex digits.
const ITEM_CLASS = /^[0-9a-f]{32}$/;
const MAX_EQUIPMENT = 64;
const EMPTY_HANDS = Object.freeze(['', '']);

function equipmentOf(value) {
  if (
    !Array.isArray(value) ||
    value.length > MAX_EQUIPMENT ||
    !value.every((item) => typeof item === 'string' && ITEM_CLASS.test(item))
  )
    throw new TypeError('Invalid native player equipment');
  return Object.freeze([...value]);
}

function handsOf(value) {
  if (
    !Array.isArray(value) ||
    value.length !== 2 ||
    !value.every((item) => item === '' || (typeof item === 'string' && ITEM_CLASS.test(item)))
  )
    throw new TypeError('Invalid native player hands');
  return Object.freeze([value[0], value[1]]);
}

// A player's nameplate as the server set it (player.setNameplate*): shown, health bar, name, the
// text drawn instead of the nickname, its colour (0xAARRGGBB) and the title above it.
const PLAIN_NAMEPLATE = Object.freeze({
  visible: true,
  health: true,
  name: true,
  text: '',
  color: 0xffffffff,
  title: '',
  titleColor: 0,
  titleSize: 0,
});
const NAMEPLATE_TEXT_MAX = 256;

function nameplateOf(value) {
  const flag = (key) => {
    if (typeof value[key] !== 'boolean') throw new TypeError('Invalid native player nameplate');
    return value[key];
  };
  const text = (key) => {
    if (typeof value[key] !== 'string' || value[key].length > NAMEPLATE_TEXT_MAX)
      throw new TypeError('Invalid native player nameplate');
    return value[key];
  };
  const whole = (key, max) => {
    if (!Number.isInteger(value[key]) || value[key] < 0 || value[key] > max)
      throw new TypeError('Invalid native player nameplate');
    return value[key];
  };
  if (value === null || typeof value !== 'object')
    throw new TypeError('Invalid native player nameplate');
  return Object.freeze({
    visible: flag('visible'),
    health: flag('health'),
    name: flag('name'),
    text: text('text'),
    color: whole('color', 0xffffffff),
    title: text('title'),
    titleColor: whole('titleColor', 0xffffffff),
    titleSize: whole('titleSize', 0xff),
  });
}

function nicknameOf(value) {
  if (typeof value !== 'string' || value.length > NICKNAME_MAX)
    throw new TypeError('Invalid native player nickname');
  return value;
}

function playerIndexOf(value) {
  if (!Number.isInteger(value) || value < 0 || value > NO_PLAYER_INDEX)
    throw new TypeError('Invalid native player index');
  return value;
}

function disguiseOf(value) {
  if (typeof value !== 'string' || value.length > DISGUISE_MAX)
    throw new TypeError('Invalid native player disguise');
  return value === '' ? null : value;
}

const DEFAULT_APPEARANCE = Object.freeze({
  gender: 'male',
  body: '',
  head: '',
  hair: '',
  beard: '',
});

function components(value, fields) {
  if (
    !value ||
    fields.some((key) => typeof value[key] !== 'number' || !Number.isFinite(value[key]))
  )
    throw new TypeError('Invalid native entity transform');
}

class ClientEntities {
  constructor({
    ownerAlive = () => true,
    onChange = () => {},
    onError = () => {},
    force = () => {
      throw new Error('Native entity force bridge is not available');
    },
    localBuffs = () => [],
    send = () => {},
    // Asks the game client where it draws an engine entity now, or a joint of its body (issue):
    // (entity, joint) -> { p: [x, y, z] } or null.
    pose = () => null,
  } = {}) {
    // Network id -> the engine entity of that player's body on this machine, from
    // the native client.
    this.engineIds = new Map();
    // Network id -> the Mannequin tags this machine's animation system picked for that player's
    // body: [move speed, move direction, stance], from the native client.
    this.bodyTags = new Map();
    // Network id -> { engineId, position: { x, y, z }, name } of the living server NPCs with a body
    // on this machine (Follow), from the native client.
    this.npcBodies = new Map();
    this.records = new Map();
    this.facades = new WeakMap();
    this.localId = 0;
    this.ownerAlive = ownerAlive;
    this.force = force;
    this.localBuffs = localBuffs;
    // To the native client (Player.setLockable).
    this.send = send;
    this.pose = pose;
    this.state = new EntityStateStore({ side: 'client', ownerAlive, onChange, onError });
  }
  requireHandle(handle) {
    const binding = bindings.get(handle);
    if (
      !binding ||
      binding.registry !== this ||
      this.records.get(binding.record.id) !== binding.record
    )
      throw new Error('Entity no longer exists');
    return binding;
  }
  facade(owner) {
    if (owner && this.facades.has(owner)) return this.facades.get(owner);
    const registry = this;
    class Entity {
      constructor(id) {
        if (!validId(id)) throw new TypeError('Invalid entity ID');
        const record = registry.records.get(id);
        if (!record) throw new Error('Unknown entity');
        bindings.set(this, { registry, record, owner });
        Object.defineProperty(this, 'id', { value: id, enumerable: true });
      }
      get dimension() {
        return registry.requireHandle(this).record.dimension;
      }
      get position() {
        const value = registry.requireHandle(this).record.position;
        return new Vector3(value.x, value.y, value.z);
      }
      set position(value) {
        registry.write(this, 'position', vector(value));
      }
      get rotation() {
        const value = registry.requireHandle(this).record.rotation;
        return new Quaternion(value.w, value.x, value.y, value.z);
      }
      set rotation(value) {
        registry.write(this, 'rotation', rotation(value));
      }
      get state() {
        const { record } = registry.requireHandle(this);
        return registry.state.forOwner(record.bag, owner);
      }
      toString() {
        return `Entity(${this.id})`;
      }
    }
    // A player this client sees: an Entity with what its own client published about its body.
    class Player extends Entity {
      constructor(id) {
        super(id);
        if (registry.records.get(id).kind !== 'player') throw new Error('Unknown player');
      }
      // What the player's body looks like, as its own client last published it.
      get appearance() {
        return registry.requireHandle(this).record.appearance;
      }
      // The prop-catalog mesh the player's body is drawn as, or null.
      get disguise() {
        return registry.requireHandle(this).record.disguise;
      }
      // Whether the player's body is drawn at all, as its own client publishes it (the server's
      // setVisible).
      get visible() {
        return registry.requireHandle(this).record.visible;
      }
      // Whether the player's helmet has its visor down, according to their client.
      get visorDown() {
        return registry.requireHandle(this).record.visorDown;
      }
      // Keeps this player's body out of this machine's combat lock-on and opponent picker (false)
      // or lets it back (true), until changed or the session ends. A swing that
      // lands anyway still lands: damage is the server's to refuse.
      setLockable(targetable) {
        if (typeof targetable !== 'boolean') {
          throw new TypeError('setLockable takes true or false');
        }
        const { record } = registry.requireHandle(this);
        registry.send({ op: 'playerTargetable', id: record.id, targetable });
      }
      // Takes this player's body on this machine out of the collisions (false) or puts it back
      // (true), until changed or the session ends: another player walks through it as
      // through a seated player's, as at a dice table. The player's own body is not changed.
      setCollision(collides) {
        if (typeof collides !== 'boolean') {
          throw new TypeError('setCollision takes true or false');
        }
        const { record } = registry.requireHandle(this);
        registry.send({ op: 'playerCollision', id: record.id, collides });
      }
      // The item classes the body wears (32 hex digits each), and the classes drawn in each hand
      // ("" for an empty one), as the player's own client last published them.
      get equipment() {
        return [...registry.requireHandle(this).record.equipment];
      }
      get rightHandItem() {
        return registry.requireHandle(this).record.hands[0];
      }
      get leftHandItem() {
        return registry.requireHandle(this).record.hands[1];
      }
      // The name the player connected under; "" once their body is gone.
      get nickname() {
        const record = registry.records.get(this.id);
        return record && record.kind === 'player' ? record.nickname : '';
      }
      // The connection slot the player holds, 65535 when unassigned.
      get playerIndex() {
        return registry.requireHandle(this).record.playerIndex;
      }
      // True once the body has its pose and its soul object.
      get ready() {
        return registry.requireHandle(this).record.ready;
      }
      // Strength, agility and vitality as the engine's relative values; zeros before the
      // player's client reported them or once their body is gone.
      get scaledStats() {
        return scaledStats(registry.statsOf(this.id));
      }
      // Every track's level as the player's client last reported it; zeros before.
      get levels() {
        return levelsOf(registry.progressionOf(this.id));
      }
      // How the body moves and looks: the local player's velocity is the one its
      // animation was driven by, everyone else's measured from its positions; zero without one.
      get velocity() {
        const value = registry.requireHandle(this).record.velocity;
        return new Vector3(value.x, value.y, value.z);
      }
      get gaze() {
        const value = registry.requireHandle(this).record.gaze;
        return new Vector3(value.x, value.y, value.z);
      }
      // True if this object represents the local player.
      get local() {
        return this.id === registry.localId;
      }
      // The engine entity behind the player on this machine, 0 before it exists.
      get entityId() {
        return registry.engineIds.get(this.id) ?? 0;
      }
      // Where this machine's game draws the body now: another player's body moves
      // smoothly between the network updates that move position; null before the body is here.
      get renderPosition() {
        return registry.drawnAt(this.id, '');
      }
      // Where a joint of the body is drawn now ('Bip01 Head', 'Bip01 R Hand', or the game's own
      // names 'Head', 'RightHand'), null when the body or the joint is not here.
      jointPosition(joint) {
        if (typeof joint !== 'string' || joint.length === 0 || joint.length > 64) {
          throw new TypeError('jointPosition takes a joint name');
        }
        return registry.drawnAt(this.id, joint);
      }
      // How a joint of the body is turned now, in the world, as a Quaternion; null when the body
      // or the joint is not here. With jointPosition it places a prop on the joint.
      jointRotation(joint) {
        if (typeof joint !== 'string' || joint.length === 0 || joint.length > 64) {
          throw new TypeError('jointRotation takes a joint name');
        }
        return registry.drawnTurn(this.id, joint);
      }
      // The network id of the horse the player rides, 0 on foot.
      get horseId() {
        return registry.requireHandle(this).record.horseId;
      }
      // The locomotion pace, movement direction and stance tags this machine's animation
      // system picked for the body: raw tag ids of its own tag definition, 255 for none.
      get movePace() {
        return registry.bodyTags.get(this.id)?.[0] ?? NO_TAG;
      }
      get moveDirection() {
        return registry.bodyTags.get(this.id)?.[1] ?? NO_TAG;
      }
      get bodyStance() {
        return registry.bodyTags.get(this.id)?.[2] ?? NO_TAG;
      }
      // The status effects on the local player's body; none for any other player, whose client
      // does not publish them.
      get buffs() {
        return this.id === registry.localId ? registry.localBuffs() : [];
      }
      // Every perk the player owns, by name.
      get perks() {
        return perkNames(registry.progressionOf(this.id));
      }
      // The local player's level, XP towards the next level, its cost and unspent points; null
      // for any other player. Throws for an unknown track.
      trackOf(track) {
        return trackProgress(registry.progressionOf(this.id), track);
      }
      // Whether the player owns the perk (a name or GUID).
      hasPerk(perk) {
        return hasPerk(registry.progressionOf(this.id), perk);
      }
      // The latest reported writable stat (PlayerStat), 0 without a snapshot. Throws for an
      // unknown stat.
      getStat(stat) {
        return getStat(registry.statsOf(this.id), stat);
      }
      // The latest reported derived stat (DerivedPlayerStat), 0 without a snapshot. Throws for
      // an unknown stat.
      derivedStat(stat) {
        return derivedStat(registry.statsOf(this.id), stat);
      }
      // What the server set on the player's nameplate (player.setNameplate* on the server), as every
      // screen draws it; this machine's own Nameplates settings come on top.
      isNameplateVisible() {
        return registry.nameplateOf(this.id).visible;
      }
      isNameplateHealthVisible() {
        return registry.nameplateOf(this.id).health;
      }
      isNameplateNameVisible() {
        return registry.nameplateOf(this.id).name;
      }
      getNameplateText() {
        return registry.nameplateOf(this.id).text;
      }
      getNameplateColor() {
        return registry.nameplateOf(this.id).color;
      }
      getNameplateTitle() {
        return registry.nameplateOf(this.id).title;
      }
      toString() {
        const local = registry.localId === this.id ? ', local' : '';
        return `Player(${this.id}, ${JSON.stringify(this.nickname)}${local})`;
      }
    }
    // health, maxHealth ... poisoning, strength, agility, vitality, skills, scaledSkills, level
    // and healthPercent, as the player's own client last published them.
    defineReadings(Player.prototype, (player) => registry.statsOf(player.id));
    definePostureReadings(Player.prototype, (player) => registry.postureOf(player.id));
    const api = {
      Entity,
      Player,
      SharedState: registry.state.SharedState,
      PlayerStat,
      DerivedPlayerStat,
    };
    Object.defineProperty(api, 'LocalPlayer', {
      enumerable: true,
      get: () =>
        registry.localId && registry.records.has(registry.localId)
          ? new Player(registry.localId)
          : null,
    });
    if (owner) this.facades.set(owner, api);
    return api;
  }
  // Where the game client draws a player's body or a joint of it now ('' for the body), as a
  // Vector3; null without a body here or such a joint.
  drawnAt(id, joint) {
    const entity = this.engineIds.get(id) ?? 0;
    if (!entity) {
      return null;
    }
    const reply = this.pose(entity, joint);
    const p = reply?.p;
    if (!Array.isArray(p) || p.length !== 3 || !p.every(Number.isFinite)) {
      return null;
    }
    return new Vector3(p[0], p[1], p[2]);
  }
  // How the game client turns a joint of a player's body now, as a Quaternion; null
  // without a body here or such a joint. The game answers x, y, z, w.
  drawnTurn(id, joint) {
    const entity = this.engineIds.get(id) ?? 0;
    if (!entity) {
      return null;
    }
    const reply = this.pose(entity, joint);
    const q = reply?.q;
    if (!Array.isArray(q) || q.length !== 4 || !q.every(Number.isFinite)) {
      return null;
    }
    return new Quaternion(q[3], q[0], q[1], q[2]);
  }
  // A player's stats snapshot, or none once their body left this client's view.
  statsOf(id) {
    const record = this.records.get(id);
    return record && record.kind === 'player' ? record.statSnapshot : NO_STATS;
  }
  // How a player's body stands, the defaults once their body left this client's view.
  postureOf(id) {
    const record = this.records.get(id);
    return record && record.kind === 'player' ? record.posture : NO_POSTURE;
  }
  // A player's progression, or none once their body left this client's view.
  progressionOf(id) {
    const record = this.records.get(id);
    return record && record.kind === 'player' ? record.progressionState : null;
  }
  // The nameplate the server set on a player, the plain one once their body left this view.
  nameplateOf(id) {
    const record = this.records.get(id);
    return record && record.kind === 'player' ? record.nameplate : PLAIN_NAMEPLATE;
  }
  write(handle, field, value) {
    const { record, owner } = this.requireHandle(handle);
    if (!this.ownerAlive(owner)) throw new Error('Resource is stopped');
    this.force({ id: record.id, nativeId: record.nativeId, field, value });
  }
  updateDirectory(directory) {
    if (
      !directory ||
      !Array.isArray(directory.entities) ||
      directory.entities.length > 4096 ||
      (!validId(directory.localPlayerId) && directory.localPlayerId !== 0)
    )
      throw new TypeError('Invalid native entity directory');
    const next = new Map();
    for (const entry of directory.entities) {
      if (
        !validId(entry?.id) ||
        !validId(entry.nativeId) ||
        next.has(entry.id) ||
        !Number.isInteger(entry.dimension) ||
        entry.dimension < 0 ||
        entry.dimension > 0xffffffff ||
        !['player', 'horse', 'entity'].includes(entry.kind)
      )
        throw new TypeError('Invalid native entity entry');
      components(entry.position, ['x', 'y', 'z']);
      components(entry.rotation, ['w', 'x', 'y', 'z']);
      const data = {
        id: entry.id,
        nativeId: entry.nativeId,
        kind: entry.kind,
        dimension: entry.dimension,
        position: vector(entry.position),
        rotation: rotation(entry.rotation),
      };
      if (entry.kind === 'player') {
        data.appearance =
          entry.appearance === undefined ? DEFAULT_APPEARANCE : appearanceOf(entry.appearance);
        data.disguise = entry.disguise === undefined ? null : disguiseOf(entry.disguise);
        if (entry.visible !== undefined && typeof entry.visible !== 'boolean')
          throw new TypeError('Invalid native player visibility');
        data.visible = entry.visible !== false;
        if (entry.visorDown !== undefined && typeof entry.visorDown !== 'boolean')
          throw new TypeError('Invalid native player visor');
        data.visorDown = entry.visorDown === true;
        data.nickname = entry.nickname === undefined ? '' : nicknameOf(entry.nickname);
        data.playerIndex =
          entry.playerIndex === undefined ? NO_PLAYER_INDEX : playerIndexOf(entry.playerIndex);
        if (entry.ready !== undefined && typeof entry.ready !== 'boolean')
          throw new TypeError('Invalid native player readiness');
        data.ready = entry.ready === true;
        data.statSnapshot = statsFromDirectory(entry);
        data.velocity = entry.velocity === undefined ? ZERO : vector(entry.velocity);
        data.gaze = entry.gaze === undefined ? ZERO : vector(entry.gaze);
        if (entry.horseId !== undefined && entry.horseId !== 0 && !validId(entry.horseId))
          throw new TypeError('Invalid native player horse');
        data.horseId = entry.horseId ?? 0;
        data.posture = postureFromDirectory(entry.posture);
        data.progressionState = progressionFromDirectory(entry.progression);
        data.equipment =
          entry.equipment === undefined ? Object.freeze([]) : equipmentOf(entry.equipment);
        data.hands = entry.hands === undefined ? EMPTY_HANDS : handsOf(entry.hands);
        data.nameplate =
          entry.nameplate === undefined ? PLAIN_NAMEPLATE : nameplateOf(entry.nameplate);
      }
      next.set(entry.id, data);
    }
    if (directory.localPlayerId && next.get(directory.localPlayerId)?.kind !== 'player')
      throw new TypeError('Local player is not in the native directory');
    for (const [id, record] of this.records) {
      if (next.get(id)?.nativeId !== record.nativeId || next.get(id)?.kind !== record.kind) {
        this.state.destroy(id);
        this.records.delete(id);
      }
    }
    for (const [id, data] of next) {
      const record = this.records.get(id);
      if (record) Object.assign(record, data);
      else {
        this.state.attach(id);
        this.records.set(id, { ...data, bag: this.state.bag(id) });
      }
    }
    this.localId = directory.localPlayerId;
  }
  applyState({ id, nativeId, entries }) {
    const record = this.records.get(id);
    if (!record || record.nativeId !== nativeId) return false;
    this.state.replace(id, entries);
    return true;
  }
  // { ids: [[network id, engine id]] } from the native client: the engine entities of the players'
  // bodies on this machine. A malformed message is dropped.
  updateEngineIds({ ids }) {
    if (!Array.isArray(ids) || ids.length > 4096) return;
    const next = new Map();
    for (const pair of ids) {
      if (!Array.isArray(pair) || !validId(pair[0]) || !validId(pair[1])) return;
      next.set(pair[0], pair[1]);
    }
    this.engineIds = next;
  }
  // { npcs: [[network id, engine id, x, y, z, name]] } from the native client: the living server
  // NPCs with a body on this machine. A malformed message is dropped.
  updateNpcBodies({ npcs }) {
    if (!Array.isArray(npcs) || npcs.length > 4096) return;
    const next = new Map();
    for (const row of npcs) {
      if (!Array.isArray(row) || row.length !== 6) return;
      const [id, engineId, x, y, z, name] = row;
      if (!validId(id) || !validId(engineId) || typeof name !== 'string') return;
      if (![x, y, z].every(Number.isFinite)) return;
      next.set(id, { engineId, position: { x, y, z }, name });
    }
    this.npcBodies = next;
  }
  // { tags: [[network id, move speed, move direction, stance]] } from the native client: the
  // Mannequin tags of the players' bodies on this machine, each a tag id or 255 for none. A
  // malformed message is dropped.
  updateBodyTags({ tags }) {
    if (!Array.isArray(tags) || tags.length > 4096) return;
    const next = new Map();
    const validTag = (tag) => Number.isInteger(tag) && tag >= 0 && tag <= NO_TAG;
    for (const entry of tags) {
      if (!Array.isArray(entry) || entry.length !== 4 || !validId(entry[0])) return;
      if (!entry.slice(1).every(validTag)) return;
      next.set(entry[0], Object.freeze(entry.slice(1)));
    }
    this.bodyTags = next;
  }
  removeResource(owner) {
    this.state.removeResource(owner);
  }
  disconnect() {
    this.state.disconnect();
    this.records.clear();
    this.engineIds = new Map();
    this.bodyTags = new Map();
    this.npcBodies = new Map();
    this.localId = 0;
  }
}

module.exports = { ClientEntities };
