'use strict';

const { randomBytes, createHash } = require('node:crypto');
const { LIMITS, CHUNK_BYTES } = require('../../../../Client/resources/entity_transfer.cjs');
const { ClientEntities } = require('../../../../Client/resources/entities.cjs');
const { directoryProgression } = require('./progression/player_progression.cjs');
const { directoryStats } = require('./stats/player_stats.cjs');
const { postureFromNative, directoryPosture } = require('./posture/player_posture.cjs');

const fingerprint = (bytes) => createHash('sha256').update(bytes).digest('hex');

// Connection cap of the native server when it does not announce one (KCDS_SERVER_MAX_PEERS
// default) and the largest cap it accepts.
const DEFAULT_MAX_PEERS = 32;
const MAX_PEERS_LIMIT = 1024;
// Entities one peer may see (the client directory limit) and entity records held at once.
const MAX_VIEW_ENTITIES = 4096;
const MAX_RECORDS = 65536;
// Largest native message parts.
const MAX_RECORDS_PER_MESSAGE = 256;
// Peers above which a changed view waits for the peer's next frame to be built.
const CROWD_PEERS = 100;
const MAX_IDS_PER_MESSAGE = 1024;
// Default SDK envelope budget: 32 KiB/s per configured peer, at most 8 MiB/s in total. With the
// default 32 peers this is the original 1 MiB/s.
const PEER_BYTES_PER_SECOND = 32 * 1024;
const MAX_TOTAL_BYTES_PER_SECOND = 8 * 1024 * 1024;
// The native host keeps at most 256 resource host messages per tick; frames above that would
// be dropped there and retried after a second.
const MAX_FRAMES_PER_PUMP = 192;
const KINDS = new Set(['player', 'horse', 'entity']);
// A directory delta frame starts with this byte (a whole directory is JSON: '{').
const DELTA_MARKER = 1;
// The fields a motion record carries, by mask bit, with their components; the largest record is
// the id, the mask and every component as a float32.
const MOTION_LAYOUT = [
  ['position', ['x', 'y', 'z']],
  ['rotation', ['w', 'x', 'y', 'z']],
  ['velocity', ['x', 'y', 'z']],
  ['gaze', ['x', 'y', 'z']],
];
const MOTION_FIELDS = new Set(MOTION_LAYOUT.map(([field]) => field));
const MOTION_RECORD_MAX = 4 + 1 + 13 * 4;

function validId(id) {
  return Number.isInteger(id) && id > 0 && id <= 0xffffffff;
}

function finiteComponents(value, fields) {
  if (value === null || typeof value !== 'object') {
    return false;
  }
  return fields.every((key) => typeof value[key] === 'number' && Number.isFinite(value[key]));
}

// One entity of a client's directory; a player's carries its published appearance and disguise,
// its name, connection slot and readiness, and its stats.
// A player's stats as the native server reports them; the client directory validates them.
const STAT_GROUPS = ['vitals', 'stats', 'scaledStats', 'abilities'];
// A player's motion as the native server reports it.
const MOTION_VECTORS = ['velocity', 'motionVelocity', 'gaze'];
// What a player's body wears and holds drawn: item classes as scripts compare them, 32
// lower-case hex digits (hands.hpp item_class_script_text).
const ITEM_CLASS = /^[0-9a-f]{32}$/;
const MAX_EQUIPMENT = 64;

// The worn item classes the native server reports, none for anything else.
function equipmentFromNative(value) {
  if (
    !Array.isArray(value) ||
    value.length > MAX_EQUIPMENT ||
    !value.every((item) => typeof item === 'string' && ITEM_CLASS.test(item))
  ) {
    return [];
  }
  return [...value];
}

// [right, left] as the native server reports them, "" for an empty hand.
function handsFromNative(value) {
  if (
    !Array.isArray(value) ||
    value.length !== 2 ||
    !value.every((item) => item === '' || (typeof item === 'string' && ITEM_CLASS.test(item)))
  ) {
    return ['', ''];
  }
  return [value[0], value[1]];
}

// `progression`: the player's (from setProgression) or undefined; `own`: the directory is the
// player's own, which alone carries its XP and perk points. Stats and progression travel as
// arrays and a perk bit set: a directory goes out again whenever anything in it moved.
// `nameplate`: what the server set on the player's plate (setNameplate), or undefined for the plain
// one; every client's Player reads it (isNameplateVisible, getNameplateText, ...).
function directoryEntry(
  record,
  nativeId,
  own = false,
  progression = undefined,
  nameplate = undefined,
) {
  const entry = {
    id: record.id,
    nativeId,
    kind: record.kind,
    dimension: record.dimension,
    position: record.position,
    rotation: record.rotation,
  };
  if (record.kind === 'player') {
    if (record.appearance !== undefined) entry.appearance = record.appearance;
    // The player's own directory carries the velocity its animation was driven by; everyone
    // else's the one measured from its positions.
    const velocity = own ? record.motionVelocity : record.velocity;
    if (velocity !== undefined) entry.velocity = velocity;
    if (record.gaze !== undefined) entry.gaze = record.gaze;
    if (Number.isInteger(record.horseId)) entry.horseId = record.horseId;
    if (record.posture !== undefined) entry.posture = directoryPosture(record.posture);
    if (typeof record.disguise === 'string') entry.disguise = record.disguise;
    // Whether its body is drawn (Player.setVisible); only a hidden one carries it.
    if (record.visible === false) entry.visible = false;
    // Whether its helmet's visor is down; only a closed one carries it.
    if (record.visorDown === true) entry.visorDown = true;
    if (typeof record.nickname === 'string') entry.nickname = record.nickname;
    if (Number.isInteger(record.playerIndex)) entry.playerIndex = record.playerIndex;
    if (typeof record.ready === 'boolean') entry.ready = record.ready;
    // A bare body with empty hands carries neither.
    if (record.equipment?.length) entry.equipment = record.equipment;
    if (record.hands && (record.hands[0] || record.hands[1])) entry.hands = record.hands;
    Object.assign(entry, directoryStats(record));
    if (progression) entry.progression = directoryProgression(progression, own);
    if (nameplate) entry.nameplate = nameplate;
  }
  return entry;
}

function validIdList(list, limit) {
  return Array.isArray(list) && list.length <= limit && list.every(validId);
}

// Budget defaults that follow the configured connection cap.
function defaultBudget(maxPeers) {
  return {
    bytesPerSecond: Math.min(maxPeers * PEER_BYTES_PER_SECOND, MAX_TOTAL_BYTES_PER_SECOND),
    framesPerPump: Math.min(MAX_FRAMES_PER_PUMP, Math.max(64, maxPeers * 2)),
  };
}

// Native host supplies the authoritative visible set and ownership. Resources
// cannot submit views. Only one frame is in flight per peer, bounding pipe load.
//
// The native host reports changed-state deltas: entity records (entityRecords /
// entityRemoved) shared by every view and, per peer, the members that entered or
// left its view (entityViewDelta). A peer's directory is rebuilt only when its
// membership changed, or, at most every `directoryInterval` seconds, when a
// member's record changed and the peer is ready for its next frame.
//
// What goes out: a peer's first directory whole, then a delta against the directory
// it acknowledged: new entries whole, changed entries as patches of the fields that changed (a
// moving player is its position and rotation), the ids that left. The whole directory again when
// a delta would be larger, or when the client could not apply one (its acknowledgment asks to
// resync). A delta is binary: a marker byte, the JSON part's length and the JSON part, then the
// entries whose only change is where they are, as fixed records (id, which of position and
// rotation, float32s): what moves every tick costs bytes, not text. A frame that fits one chunk is one entityFrame message;
// chunk data travels as raw bytes.
class EntitySender {
  constructor({
    store,
    send,
    session = () => randomBytes(16).toString('hex'),
    maxPeers = DEFAULT_MAX_PEERS,
    bytesPerSecond,
    burstBytes = 64 * 1024,
    framesPerPump,
    directoryInterval = 0.1,
    onError = () => {},
  }) {
    this.explicitBudget = {
      bytesPerSecond: bytesPerSecond !== undefined,
      framesPerPump: framesPerPump !== undefined,
    };
    this.burstBytes = burstBytes;
    this.bytesPerSecond = bytesPerSecond;
    this.framesPerPump = framesPerPump;
    this.directoryInterval = directoryInterval;
    this.configure({ maxPeers });
    this.credit = burstBytes;
    this.lastPumpAt = null;
    this.peerCursor = 0;
    this.store = store;
    this.send = send;
    this.session = session;
    this.onError = onError;
    this.peers = new Map();
    this.views = new Map();
    // Native entity id -> shared public record; script id -> native id.
    this.records = new Map();
    this.recordIds = new Map();
    this.recordClock = 0;
    // Players' progression by script id, apart from the records.
    this.progressions = new Map();
    // Players' nameplates by script id, while the server set something on them.
    this.nameplates = new Map();
  }
  // Applies the native connection cap. Budgets that were not given explicitly follow it.
  configure({ maxPeers }) {
    if (!Number.isInteger(maxPeers) || maxPeers < 1 || maxPeers > MAX_PEERS_LIMIT) {
      throw new RangeError('Invalid native peer limit');
    }
    const defaults = defaultBudget(maxPeers);
    const bytesPerSecond = this.explicitBudget.bytesPerSecond
      ? this.bytesPerSecond
      : defaults.bytesPerSecond;
    const framesPerPump = this.explicitBudget.framesPerPump
      ? this.framesPerPump
      : defaults.framesPerPump;
    if (
      !Number.isSafeInteger(bytesPerSecond) ||
      bytesPerSecond < 1 ||
      !Number.isSafeInteger(this.burstBytes) ||
      this.burstBytes < CHUNK_BYTES * 2 ||
      !Number.isSafeInteger(framesPerPump) ||
      framesPerPump < 1 ||
      typeof this.directoryInterval !== 'number' ||
      !(this.directoryInterval >= 0)
    ) {
      throw new RangeError('Invalid entity sender budget');
    }
    this.maxPeers = maxPeers;
    this.bytesPerSecond = bytesPerSecond;
    this.framesPerPump = framesPerPump;
  }
  // Legacy whole-view message, kept for tools that still send complete views.
  receiveViewPart(message) {
    const { peer, revision, offset, total, entities, localPlayerId } = message;
    if (
      !Number.isInteger(peer) ||
      peer < 1 ||
      peer > 0xffffffff ||
      !Number.isSafeInteger(revision) ||
      revision < 1 ||
      !Number.isInteger(offset) ||
      offset < 0 ||
      !Number.isInteger(total) ||
      total < 0 ||
      total > MAX_VIEW_ENTITIES ||
      !Array.isArray(entities) ||
      entities.length > 32 ||
      offset + entities.length > total
    ) {
      throw new TypeError('Invalid native view part');
    }
    if (offset === 0) {
      if (!this.views.has(peer) && this.views.size >= this.maxPeers)
        throw new RangeError('Native view limit exceeded');
      this.views.set(peer, { revision, total, localPlayerId, entities: [] });
    }
    const view = this.views.get(peer);
    if (
      !view ||
      view.revision !== revision ||
      view.total !== total ||
      view.localPlayerId !== localPlayerId ||
      offset !== view.entities.length
    ) {
      this.views.delete(peer);
      throw new TypeError('Native view part is out of sequence');
    }
    view.entities.push(...entities);
    if (view.entities.length !== total) return false;
    this.views.delete(peer);
    this.updateView(peer, view);
    return true;
  }
  // Native entity records that changed since they were last reported.
  applyRecords(message) {
    const entities = message?.entities;
    if (!Array.isArray(entities) || entities.length > MAX_RECORDS_PER_MESSAGE) {
      throw new TypeError('Invalid native entity records');
    }
    const batch = new Map();
    for (const entry of entities) {
      this.validateRecord(entry);
      if (batch.has(entry.nativeId)) {
        throw new TypeError('Duplicate native entity record');
      }
      const owner = this.recordIds.get(entry.id);
      if (owner !== undefined && owner !== entry.nativeId) {
        throw new TypeError('Native entity id is already in use');
      }
      batch.set(entry.nativeId, entry);
    }
    let added = 0;
    for (const nativeId of batch.keys()) {
      if (!this.records.has(nativeId)) {
        added += 1;
      }
    }
    if (this.records.size + added > MAX_RECORDS) {
      throw new RangeError('Native entity record limit exceeded');
    }
    for (const entry of batch.values()) {
      this.storeRecord(entry);
    }
  }
  // Native entities that no view references any more.
  removeRecords(message) {
    const ids = message?.ids;
    if (!validIdList(ids, MAX_IDS_PER_MESSAGE)) {
      throw new TypeError('Invalid native entity removal');
    }
    for (const nativeId of ids) {
      const record = this.records.get(nativeId);
      if (record && record.viewers > 0) {
        throw new TypeError('Native entity is still in a view');
      }
    }
    for (const nativeId of ids) {
      this.deleteRecord(nativeId);
    }
  }
  // Members that entered or left one peer's view. The final part carries the owned
  // members and the local player and publishes the view.
  applyViewDelta(message) {
    const { peer: peerId, localPlayerId, added, removed, owned, final } = message ?? {};
    if (
      !validId(peerId) ||
      (localPlayerId !== 0 && !validId(localPlayerId)) ||
      !validIdList(added, MAX_IDS_PER_MESSAGE) ||
      !validIdList(removed, MAX_IDS_PER_MESSAGE) ||
      typeof final !== 'boolean' ||
      (final && !validIdList(owned, MAX_VIEW_ENTITIES))
    ) {
      throw new TypeError('Invalid native view delta');
    }
    const existing = this.peers.get(peerId);
    const members = existing ? existing.members : new Map();
    const removing = new Set(removed);
    for (const nativeId of removed) {
      if (!members.has(nativeId)) {
        throw new TypeError('Native view delta removes an unknown member');
      }
    }
    const adding = new Set();
    for (const nativeId of added) {
      if (!this.records.has(nativeId)) {
        throw new TypeError('Native view delta adds an unknown entity');
      }
      if ((members.has(nativeId) && !removing.has(nativeId)) || adding.has(nativeId)) {
        throw new TypeError('Native view delta adds a member twice');
      }
      adding.add(nativeId);
    }
    if (members.size - removing.size + adding.size > MAX_VIEW_ENTITIES) {
      throw new RangeError('Native view exceeds the SDK limit');
    }
    if (final) {
      const isMember = (nativeId) =>
        adding.has(nativeId) || (members.has(nativeId) && !removing.has(nativeId));
      for (const nativeId of owned) {
        if (!isMember(nativeId)) {
          throw new TypeError('Native ownership names a non-member');
        }
      }
      if (localPlayerId !== 0) {
        if (!isMember(localPlayerId) || this.records.get(localPlayerId).kind !== 'player') {
          throw new TypeError('Local player is not in the native directory');
        }
      }
    }
    const peer = existing ?? this.createPeer(peerId);
    for (const nativeId of removed) {
      this.removeMember(peer, nativeId);
    }
    for (const nativeId of added) {
      this.addMember(peer, nativeId, false);
    }
    if (!final) return false;
    const ownedSet = new Set(owned);
    for (const nativeId of peer.members.keys()) {
      peer.members.set(nativeId, ownedSet.has(nativeId));
    }
    peer.localPlayerId = localPlayerId;
    this.membershipChanged(peer);
    if (this.peers.size > CROWD_PEERS && peer.directoryBytes) {
      // In a crowd every join changes every peer's view: building and hashing each whole
      // directory at once held the scripts for many seconds. The peer's next frame builds it.
      peer.directoryStale = true;
    } else {
      this.publishDirectory(peer);
    }
    return true;
  }
  // Replaces a peer's whole view (tests and the legacy message). Validated before
  // anything changes, so a malformed view keeps the previous one.
  updateView(peerId, view) {
    if (!validId(peerId)) {
      throw new TypeError('Invalid native peer');
    }
    // Use the same directory validator as the receiving SDK. Ownership metadata
    // is deliberately not exposed in the client's public directory payload.
    const validator = new ClientEntities();
    validator.updateDirectory(view);
    const owners = new Map();
    for (const entry of view.entities) {
      if (typeof entry.isOwner !== 'boolean') throw new TypeError('Invalid native ownership');
      owners.set(entry.id, entry.isOwner);
    }
    const preview = this.directoryBytes(
      view.localPlayerId,
      [...validator.records.values()].map((record) => directoryEntry(record, record.nativeId)),
    );
    if (preview.length > LIMITS.directory) throw new RangeError('Native directory is too large');
    if (!this.peers.has(peerId) && this.peers.size >= this.maxPeers) {
      throw new RangeError('Native peer limit exceeded');
    }
    const peer = this.peers.get(peerId) ?? this.createPeer(peerId);
    const next = new Map();
    for (const record of validator.records.values()) {
      next.set(record.nativeId, owners.get(record.id));
    }
    for (const nativeId of [...peer.members.keys()]) {
      if (!next.has(nativeId)) {
        this.removeMember(peer, nativeId);
        if (this.records.get(nativeId)?.viewers === 0) this.deleteRecord(nativeId);
      }
    }
    for (const record of validator.records.values()) {
      const stale = this.recordIds.get(record.id);
      if (stale !== undefined && stale !== record.nativeId) {
        this.releaseScriptId(stale);
      }
      this.storeRecord(record);
      if (!peer.members.has(record.nativeId)) this.addMember(peer, record.nativeId, false);
      peer.members.set(record.nativeId, next.get(record.nativeId));
    }
    peer.localPlayerId = view.localPlayerId;
    this.membershipChanged(peer);
    this.publishDirectory(peer);
  }
  validateRecord(entry) {
    if (
      !entry ||
      !validId(entry.id) ||
      !validId(entry.nativeId) ||
      !Number.isInteger(entry.dimension) ||
      entry.dimension < 0 ||
      entry.dimension > 0xffffffff ||
      !KINDS.has(entry.kind) ||
      !finiteComponents(entry.position, ['x', 'y', 'z']) ||
      !finiteComponents(entry.rotation, ['w', 'x', 'y', 'z'])
    ) {
      throw new TypeError('Invalid native entity entry');
    }
  }
  storeRecord(entry) {
    let record = this.records.get(entry.nativeId);
    if (!record) {
      record = { nativeId: entry.nativeId, viewers: 0 };
      this.records.set(entry.nativeId, record);
    } else if (record.id !== entry.id) {
      this.recordIds.delete(record.id);
    }
    record.id = entry.id;
    record.kind = entry.kind;
    record.dimension = entry.dimension;
    record.position = { x: entry.position.x, y: entry.position.y, z: entry.position.z };
    record.rotation = {
      w: entry.rotation.w,
      x: entry.rotation.x,
      y: entry.rotation.y,
      z: entry.rotation.z,
    };
    // A player's published look; the client directory validates it.
    if (entry.kind === 'player') {
      record.appearance = entry.appearance;
      record.disguise = entry.disguise;
      record.visible = entry.visible !== false;
      record.visorDown = entry.visorDown === true;
      record.nickname = entry.nickname;
      record.playerIndex = entry.playerIndex;
      record.ready = entry.ready;
      for (const group of STAT_GROUPS) record[group] = entry[group];
      for (const field of MOTION_VECTORS) {
        record[field] = finiteComponents(entry[field], ['x', 'y', 'z'])
          ? { x: entry[field].x, y: entry[field].y, z: entry[field].z }
          : undefined;
      }
      record.horseId = validId(entry.horseId) ? entry.horseId : 0;
      record.posture = postureFromNative(entry);
      record.equipment = equipmentFromNative(entry.equipment);
      record.hands = handsFromNative(entry.hands);
    } else {
      delete record.appearance;
      delete record.disguise;
      delete record.visible;
      delete record.visorDown;
      delete record.nickname;
      delete record.playerIndex;
      delete record.ready;
      for (const group of STAT_GROUPS) delete record[group];
      for (const field of MOTION_VECTORS) delete record[field];
      delete record.horseId;
      delete record.posture;
      delete record.equipment;
      delete record.hands;
    }
    this.recordClock += 1;
    record.changedAt = this.recordClock;
    this.recordIds.set(entry.id, entry.nativeId);
  }
  // A player's progression (parsed, or null when it is gone). Directories holding the player are
  // rebuilt; nothing else changes.
  setProgression(id, progression) {
    if (progression) this.progressions.set(id, progression);
    else this.progressions.delete(id);
    this.touch(id);
  }
  // A player's nameplate as the server set it ({ visible, health, name, text, color, title,
  // titleColor, titleSize }, or null for the plain one). Directories holding the player are
  // rebuilt.
  setNameplate(id, nameplate) {
    if (nameplate) this.nameplates.set(id, nameplate);
    else this.nameplates.delete(id);
    this.touch(id);
  }
  // The player's record changed outside its native report: every directory holding it is
  // rebuilt.
  touch(id) {
    const record = this.records.get(this.recordIds.get(id));
    if (record) {
      this.recordClock += 1;
      record.changedAt = this.recordClock;
    }
  }
  deleteRecord(nativeId) {
    const record = this.records.get(nativeId);
    if (!record) return;
    this.records.delete(nativeId);
    if (this.recordIds.get(record.id) === nativeId) this.recordIds.delete(record.id);
  }
  // Drops a record whose script id now belongs to another native entity (whole-view path).
  releaseScriptId(nativeId) {
    for (const peer of this.peers.values()) {
      if (peer.members.has(nativeId)) {
        this.removeMember(peer, nativeId);
        this.membershipChanged(peer);
        this.publishDirectory(peer);
      }
    }
    this.deleteRecord(nativeId);
  }
  createPeer(peerId) {
    if (this.peers.size >= this.maxPeers) throw new RangeError('Native peer limit exceeded');
    const session = this.session();
    if (!/^[a-f0-9]{32}$/.test(session)) throw new TypeError('Invalid native session token');
    const peer = {
      session,
      delivery: 0,
      pending: null,
      active: null,
      revisions: { directory: 0, state: 0 },
      directoryHash: null,
      // Native id -> whether this peer owns the entity: the authoritative view.
      members: new Map(),
      localPlayerId: 0,
      directory: { localPlayerId: 0, entities: [] },
      directoryBytes: null,
      desiredDirectoryHash: null,
      owners: new Map(),
      builtAt: 0,
      nextDirectoryCheckAt: 0,
      stateIdle: null,
      // Entity id -> native id in the directory the client has committed.
      committedEntities: new Map(),
      // The directory the client has committed, entry by entry as JSON text (id -> { text,
      // fields: key -> text }), and its revision: what a delta is worked out against.
      entryTexts: new Map(),
      committedTexts: new Map(),
      committedRevision: 0,
      states: new Map(),
      ready: false,
      nextFrameAt: 0,
      stateCursor: 0,
    };
    this.peers.set(peerId, peer);
    return peer;
  }
  addMember(peer, nativeId, owned) {
    peer.members.set(nativeId, owned);
    this.records.get(nativeId).viewers += 1;
  }
  removeMember(peer, nativeId) {
    peer.members.delete(nativeId);
    const record = this.records.get(nativeId);
    if (record && record.viewers > 0) record.viewers -= 1;
  }
  // Cancel an unfinished state if its entity left the view or owner scope
  // narrowed. Never commit a projected owner snapshot after ownership changed.
  // Forget delivered state of entities that left the view.
  membershipChanged(peer) {
    peer.stateIdle = null;
    if (peer.active?.kind === 'state') {
      const active = peer.active;
      const record = this.records.get(active.nativeId);
      if (
        !peer.members.has(active.nativeId) ||
        record?.id !== active.id ||
        peer.members.get(active.nativeId) !== active.isOwner
      ) {
        peer.active = null;
        peer.pending = null;
      }
    }
    for (const [id, cache] of peer.states) {
      const record = this.records.get(cache.nativeId);
      if (!peer.members.has(cache.nativeId) || record?.id !== id) peer.states.delete(id);
    }
  }
  directoryBytes(localPlayerId, entities) {
    const sorted = [...entities].sort((left, right) => left.id - right.id);
    return Buffer.from(JSON.stringify({ localPlayerId, entities: sorted }));
  }
  // A record's directory entry and its text, built once per change of the record and shared by
  // every directory that holds it: the player's own (with its XP and animation velocity) and
  // everyone else's.
  sharedEntry(record, own) {
    let cache = record.directoryEntries;
    if (!cache || cache.changedAt !== record.changedAt) {
      cache = { changedAt: record.changedAt, own: null, other: null };
      record.directoryEntries = cache;
    }
    const variant = own ? 'own' : 'other';
    if (!cache[variant]) {
      const progression = this.progressions.get(record.id);
      const nameplate = this.nameplates.get(record.id);
      cache[variant] = entryText(
        directoryEntry(record, record.nativeId, own, progression, nameplate),
      );
    }
    return cache[variant];
  }
  // Builds the directory the client should hold from the current records.
  publishDirectory(peer) {
    const texts = [];
    const owners = new Map();
    for (const [nativeId, isOwner] of peer.members) {
      const record = this.records.get(nativeId);
      texts.push(this.sharedEntry(record, record.id === peer.localPlayerId));
      owners.set(record.id, isOwner);
    }
    texts.sort((left, right) => left.value.id - right.value.id);
    const entities = texts.map((entry) => entry.value);
    const directory = { localPlayerId: peer.localPlayerId, entities };
    // The same bytes as JSON.stringify(directory), from the shared entry texts.
    const bytes = Buffer.from(
      `{"localPlayerId":${JSON.stringify(peer.localPlayerId)},` +
        `"entities":[${texts.map((entry) => entry.text).join(',')}]}`,
    );
    const entryTexts = new Map(texts.map((entry) => [entry.value.id, entry]));
    peer.builtAt = this.recordClock;
    if (bytes.length > LIMITS.directory) {
      // Keep the previous directory; a smaller view publishes again.
      this.onError(new RangeError('Native directory is too large'));
      return false;
    }
    peer.directory = directory;
    peer.directoryBytes = bytes;
    peer.entryTexts = entryTexts;
    peer.desiredDirectoryHash = fingerprint(bytes);
    peer.owners = owners;
    return true;
  }
  // Rebuilds the directory when a member's record changed since it was built.
  // Checked at most every `directoryInterval`, and only for a peer that is about
  // to choose its next frame, so moving entities cost what is actually sent.
  refreshDirectory(peer, now) {
    if (peer.directoryStale) {
      // A changed view in a crowd: built at most once per interval, longer by each hundred peers.
      if (now < (peer.nextViewBuildAt ?? 0)) return;
      const hundreds = Math.ceil(this.peers.size / CROWD_PEERS);
      peer.nextViewBuildAt = now + this.directoryInterval * Math.max(1, hundreds);
      peer.nextDirectoryCheckAt = now + this.directoryInterval;
      peer.directoryStale = false;
      this.publishDirectory(peer);
      return;
    }
    if (peer.builtAt === this.recordClock || now < peer.nextDirectoryCheckAt) return;
    peer.nextDirectoryCheckAt = now + this.directoryInterval;
    let changed = false;
    for (const nativeId of peer.members.keys()) {
      if (this.records.get(nativeId).changedAt > peer.builtAt) {
        changed = true;
        break;
      }
    }
    if (changed) this.publishDirectory(peer);
    else peer.builtAt = this.recordClock;
  }
  // The client's resource runtime started anew after it had this peer's session: the
  // new one holds no entities, so the peer gets a new session and everything in its view again.
  // False for a peer that is not there or never had its session.
  restartPeer(peerId) {
    const peer = this.peers.get(peerId);
    if (!peer || !peer.ready) return false;
    const session = this.session();
    if (!/^[a-f0-9]{32}$/.test(session)) throw new TypeError('Invalid native session token');
    peer.session = session;
    peer.delivery = 0;
    peer.pending = null;
    peer.active = null;
    peer.revisions = { directory: 0, state: 0 };
    peer.directoryHash = null;
    peer.stateIdle = null;
    peer.committedEntities = new Map();
    peer.committedTexts = new Map();
    peer.committedRevision = 0;
    peer.states = new Map();
    peer.ready = false;
    peer.nextFrameAt = 0;
    peer.stateCursor = 0;
    return true;
  }
  disconnect(peerId) {
    const peer = this.peers.get(peerId);
    if (peer) {
      for (const nativeId of [...peer.members.keys()]) this.removeMember(peer, nativeId);
    }
    this.peers.delete(peerId);
    this.views.delete(peerId);
  }
  acknowledge(peerId, message) {
    const peer = this.peers.get(peerId);
    if (
      !peer ||
      !peer.pending ||
      !Number.isSafeInteger(message.delivery) ||
      message.delivery < 1 ||
      message.session !== peer.session ||
      message.delivery !== peer.pending?.message.delivery
    )
      return false;
    const pending = peer.pending;
    peer.pending = null;
    const op = pending.message.op;
    if (op === 'entitySession') peer.ready = true;
    else if (op === 'entityCommit' || op === 'entityFrame') {
      const active = peer.active;
      if (active.kind === 'directory' && message.resync === true) {
        // The client could not apply the delta: the whole directory goes again.
        peer.directoryHash = null;
        peer.committedTexts = new Map();
        peer.committedRevision = 0;
      } else if (active.kind === 'directory') {
        peer.directoryHash = active.hash;
        peer.committedTexts = active.texts;
        peer.committedRevision = active.revision;
        this.commitDirectory(peer, active.entities);
      } else peer.states.set(active.id, active.cache);
      peer.lastKind = active.kind;
      peer.active = null;
    } else {
      peer.active.offset = pending.nextOffset;
      peer.active.started = true;
    }
    return true;
  }
  // The client discards an entity's state together with its directory record and
  // ignores state for an entity it does not hold. Forget delivered state for every
  // entity the committed directory no longer contains, so a returning entity
  // receives its state again.
  commitDirectory(peer, entities) {
    peer.committedEntities = new Map(entities.map((entity) => [entity.id, entity.nativeId]));
    for (const [id, cache] of peer.states) {
      if (peer.committedEntities.get(id) !== cache.nativeId) peer.states.delete(id);
    }
  }
  // The directory the peer should hold: a delta against the one it committed when it has one and
  // the delta is smaller, the whole directory otherwise.
  directoryFrame(peer) {
    const delta = peer.directoryHash ? this.directoryDelta(peer) : null;
    return {
      kind: 'directory',
      bytes: delta && delta.length < peer.directoryBytes.length ? delta : peer.directoryBytes,
      hash: peer.desiredDirectoryHash,
      entities: peer.directory.entities,
      texts: peer.entryTexts,
    };
  }
  // A delta against the peer's committed directory, as bytes (DELTA_MARKER, u32 JSON length, the
  // JSON { base, localPlayerId, upserts, patches, removed }, then the motion records). Patches are
  // { id, set: { changed fields }, unset: [fields gone] }.
  directoryDelta(peer) {
    const upserts = [];
    const patches = [];
    const moves = [];
    for (const [id, entry] of peer.entryTexts) {
      const committed = peer.committedTexts.get(id);
      if (!committed) {
        upserts.push(entry.text);
      } else if (committed.text !== entry.text) {
        const changed = changedFields(committed, entry);
        const moved = changed.every((key) => MOTION_FIELDS.has(key) && entry.fields.has(key));
        if (moved) {
          moves.push({ id, changed, entry: entry.value });
        } else {
          patches.push(patchText(id, committed, entry));
        }
      }
    }
    const removed = [];
    for (const id of peer.committedTexts.keys()) {
      if (!peer.entryTexts.has(id)) removed.push(id);
    }
    const json = Buffer.from(
      `{"base":${peer.committedRevision},"localPlayerId":${peer.directory.localPlayerId},` +
        `"upserts":[${upserts.join(',')}],"patches":[${patches.join(',')}],` +
        `"removed":${JSON.stringify(removed)}}`,
    );
    return Buffer.concat([deltaHeader(json.length), json, motionRecords(moves)]);
  }
  // True while nothing that could produce a state frame changed since the last
  // full scan found nothing to send.
  stateIdle(peer) {
    const idle = peer.stateIdle;
    return (
      idle !== null &&
      typeof this.store.changeClock === 'number' &&
      idle.clock === this.store.changeClock &&
      idle.directory === peer.directory &&
      idle.committed === peer.committedEntities
    );
  }
  choose(peer, now = 0) {
    this.refreshDirectory(peer, now);
    if (!peer.directoryBytes) return null;
    const directoryHash = peer.desiredDirectoryHash;
    if (
      directoryHash !== peer.directoryHash &&
      (!peer.directoryHash || peer.lastKind === 'state')
    ) {
      return this.directoryFrame(peer);
    }
    const entries = peer.directory.entities;
    if (!this.stateIdle(peer)) {
      for (let index = 0; index < entries.length; ++index) {
        const cursor = peer.stateCursor++ % entries.length;
        const entry = entries[cursor];
        const record = this.store.records.get(entry.id);
        if (!record) continue;
        // State waits until the client holds this entity incarnation in its directory.
        if (peer.committedEntities.get(entry.id) !== entry.nativeId) continue;
        const isOwner = peer.owners.get(entry.id);
        const previous = peer.states.get(entry.id);
        if (
          previous?.record === record &&
          previous.version === record.version &&
          previous.nativeId === entry.nativeId &&
          previous.isOwner === isOwner
        )
          continue;
        const cache = { record, version: record.version, nativeId: entry.nativeId, isOwner };
        const bytes = Buffer.from(
          JSON.stringify({
            id: entry.id,
            nativeId: entry.nativeId,
            entries: this.store.project(entry.id, isOwner),
          }),
        );
        if (bytes.length > LIMITS.state) throw new RangeError('Projected state is too large');
        const hash = fingerprint(bytes);
        cache.hash = hash;
        if (previous?.hash !== hash) {
          return {
            kind: 'state',
            bytes,
            hash,
            cache,
            id: entry.id,
            nativeId: entry.nativeId,
            isOwner,
          };
        }
        peer.states.set(entry.id, cache);
      }
      peer.stateIdle = {
        clock: this.store.changeClock,
        directory: peer.directory,
        committed: peer.committedEntities,
      };
    }
    if (directoryHash !== peer.directoryHash) {
      return this.directoryFrame(peer);
    }
    return null;
  }
  pump(now) {
    if (typeof now !== 'number' || !Number.isFinite(now))
      throw new TypeError('Invalid sender clock');
    if (this.lastPumpAt !== null && now < this.lastPumpAt)
      throw new RangeError('Entity sender clock moved backwards');
    if (this.lastPumpAt !== null) {
      this.credit = Math.min(
        this.burstBytes,
        this.credit + (now - this.lastPumpAt) * this.bytesPerSecond,
      );
    }
    this.lastPumpAt = now;
    const peers = [...this.peers.entries()];
    const start = peers.length ? this.peerCursor % peers.length : 0;
    let sent = 0;
    // Rotate the first serviced peer. A busy early peer cannot consume every
    // tick's shared budget ahead of the same late peers indefinitely.
    for (let index = 0; index < peers.length; ++index) {
      if (sent >= this.framesPerPump) break;
      const cursor = (start + index) % peers.length;
      const [id, peer] = peers[cursor];
      this.peerCursor = (cursor + 1) % peers.length;
      if (now < peer.nextFrameAt) continue;
      // Limit each peer to 50 messages/sec, under the authenticated IPC request
      // quota. A dropped acknowledgment retries the exact frame after one second.
      if (peer.pending) {
        if (now - peer.pending.sentAt >= 1) {
          if (!this.takeCredit(peer.pending.message)) {
            this.peerCursor = cursor;
            break;
          }
          this.send(id, peer.pending.message);
          peer.pending.sentAt = now;
          ++sent;
        }
        continue;
      }
      let message;
      let nextOffset = 0;
      if (!peer.ready) message = { op: 'entitySession', session: peer.session };
      else {
        if (!peer.active) {
          peer.active = this.choose(peer, now);
          if (!peer.active) continue;
          const active = peer.active;
          active.revision = ++peer.revisions[active.kind];
          if (!Number.isSafeInteger(active.revision))
            throw new RangeError('Entity revision exhausted');
          active.offset = 0;
          active.started = false;
        }
        const active = peer.active;
        const identity = { session: peer.session, kind: active.kind, revision: active.revision };
        if (!active.started && active.bytes.length <= CHUNK_BYTES)
          message = { op: 'entityFrame', ...identity, data: active.bytes };
        else if (!active.started)
          message = { op: 'entityBegin', ...identity, size: active.bytes.length };
        else if (active.offset < active.bytes.length) {
          nextOffset = Math.min(active.offset + CHUNK_BYTES, active.bytes.length);
          message = {
            op: 'entityChunk',
            ...identity,
            offset: active.offset,
            data: active.bytes.subarray(active.offset, nextOffset),
          };
        } else message = { op: 'entityCommit', ...identity };
      }
      message.delivery = peer.delivery + 1;
      if (!Number.isSafeInteger(message.delivery))
        throw new RangeError('Entity delivery exhausted');
      if (!this.takeCredit(message)) {
        // Reserve this peer's turn until credit refills. Skipping a larger
        // frame in favor of small frames could starve it on every pump.
        this.peerCursor = cursor;
        break;
      }
      peer.delivery = message.delivery;
      peer.pending = { message, nextOffset, sentAt: now };
      peer.nextFrameAt = now + 0.02;
      this.send(id, message);
      ++sent;
    }
  }
  takeCredit(message) {
    // SDK envelope bytes only: native KSTP headers/retries and other lanes
    // need their own transport budget. This is not a UDP bandwidth measurement.
    const bytes = messageBytes(message);
    if (bytes > this.credit) return false;
    this.credit -= bytes;
    return true;
  }
}

// A directory entry as JSON text, whole and field by field.
function entryText(entry) {
  const fields = new Map(Object.entries(entry).map(([key, value]) => [key, JSON.stringify(value)]));
  return { text: JSON.stringify(entry), fields, value: entry };
}

// The fields that differ between the committed entry and the new one (set or gone).
function changedFields(committed, entry) {
  const changed = [];
  for (const [key, text] of entry.fields) {
    if (committed.fields.get(key) !== text) changed.push(key);
  }
  for (const key of committed.fields.keys()) {
    if (!entry.fields.has(key)) changed.push(key);
  }
  return changed;
}

function deltaHeader(jsonLength) {
  const header = Buffer.alloc(5);
  header[0] = DELTA_MARKER;
  header.writeUInt32LE(jsonLength, 1);
  return header;
}

// The motion records: u32 id, u8 mask (bit 1 position, 2 rotation, 4 velocity, 8 look
// direction), then each named field's components as float32s, in that order.
function motionRecords(moves) {
  const records = Buffer.alloc(moves.length * MOTION_RECORD_MAX);
  let at = 0;
  for (const { id, changed, entry } of moves) {
    let mask = 0;
    MOTION_LAYOUT.forEach(([field], bit) => {
      if (changed.includes(field)) mask |= 1 << bit;
    });
    records.writeUInt32LE(id, at);
    records[at + 4] = mask;
    at += 5;
    MOTION_LAYOUT.forEach(([field, components], bit) => {
      if (!(mask & (1 << bit))) return;
      for (const key of components) {
        records.writeFloatLE(entry[field][key], at);
        at += 4;
      }
    });
  }
  return records.subarray(0, at);
}

// A patch from the committed entry to the new one: { id, set: { changed fields }, unset }.
function patchText(id, committed, entry) {
  const set = [];
  for (const [key, text] of entry.fields) {
    if (committed.fields.get(key) !== text) set.push(`${JSON.stringify(key)}:${text}`);
  }
  const unset = [...committed.fields.keys()].filter((key) => !entry.fields.has(key));
  const unsetText = unset.length ? `,"unset":${JSON.stringify(unset)}` : '';
  return `{"id":${id},"set":{${set.join(',')}}${unsetText}}`;
}

// A message's envelope bytes: its JSON with raw chunk data counted as the bytes it is.
function messageBytes(message) {
  if (!(message.data instanceof Uint8Array)) {
    return Buffer.byteLength(JSON.stringify(message), 'utf8');
  }
  const { data, ...envelope } = message;
  return Buffer.byteLength(JSON.stringify(envelope), 'utf8') + data.length;
}

module.exports = { EntitySender, DEFAULT_MAX_PEERS, MAX_PEERS_LIMIT, messageBytes };
