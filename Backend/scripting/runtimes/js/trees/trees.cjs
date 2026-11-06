'use strict';

// Felling the level's trees: Trees, playerTreeChop, playerTreeFell, treeFelled,
// playerTrunkChop, playerTrunkCut, treeRegrow and treeRegrown.
//
// A tree is a level object (vegetation and brush trees are 'brush' level objects)
// whose mesh lies under objects/natural/vegetation/trees/ (not a fallen branch or a stump), or a
// mesh a script added with Trees.setSpecies. A player's swing that meets one (playerWeaponHit)
// counts as a blow when the player holds an axe, stands within reach and did not count
// a blow in the last swing interval. Each counted blow raises playerTreeChop (false refuses it); the
// last one raises playerTreeFell (false keeps the tree standing; fall.direction may be changed).
//
// The fall is the server's: the standing tree is hidden for the world (a LevelEdit, collision
// included) and the same mesh stands in as a prop turned about its base, the trunk's foot, by the
// server several times a second along a gravity curve (slow start, quick end, a small bounce), so
// every screen shows the same pose; a player who comes later sees the pose it has then. The
// resting angle follows the ground under the crown when the chopper's game answers in time. The
// trunk then lies as a static prop: blows on it (playerTrunkChop) cut it into items after trunkHits
// (playerTrunkCut, whose items may be changed or refused), into the chopper's inventory or onto the
// ground. A felled or cut tree grows back after regrowMinutes, once no player stands within
// regrowClearance metres (treeRegrow may delay it), and treeRegrown follows.
//
// Every state is per dimension and in memory: a restart brings every tree back, unless a script
// keeps Trees.state() and gives it to Trees.restore() after the restart (felled trees lie again at
// once, cut ones stay hidden, the regrowth times go on). The players of the world hear treeChop,
// treeFalling, treeFelled, trunkCut and treeRegrown in their client scripts (the reserved network
// event kcds:tree).

const { Vector3, Quaternion } = require('../math.cjs');
const { itemClasses, classBytesHex, lookupItemClass } = require('../inventory/catalog.cjs');

const TREE_FOLDER = 'objects/natural/vegetation/trees/';
const NOT_TREES = ['/fallen_trees/', 'stump'];
const CLIENT_EVENT = 'kcds:tree';
// A world left out is the global one (4294967295 in KCDS).
const GLOBAL_WORLD = 0xffffffff;
// The fall's steps and its bounce.
const FALL_STEP_MS = 33;
const BOUNCE_SECONDS = 0.35;
const BOUNCE_DEGREES = 3;
const MAX_SLOPE_DEGREES = 35;
// Regrowth is looked at once a second; a refused regrowth is asked again a minute later.
const REGROW_CHECK_MS = 1000;
const REGROW_RETRY_MS = 60000;
// A blow lands on a lying trunk within its radius plus this much: a felled tree rests on its
// branches, so the trunk may lie a metre above the line from its foot.
const TRUNK_MARGIN = 1.5;
const MAX_HITS = 1000;
const MAX_DROPS = 16;

const DEFAULTS = Object.freeze({
  enabled: true,
  hits: 8,
  trunkHits: 6,
  regrowMinutes: 30,
  regrowClearance: 10,
  reach: 3.5,
  swingSeconds: 0.4,
  height: 15,
  trunkRadius: 0.35,
  drops: Object.freeze([Object.freeze({ item: 'bsmt_woodExotic', amount: 2 })]),
  dropTo: 'inventory',
  stump: null,
  axes: null,
});
const SETTING_KEYS = new Set(Object.keys(DEFAULTS));
const SPECIES_KEYS = new Set([
  'hits',
  'trunkHits',
  'regrowMinutes',
  'height',
  'trunkRadius',
  'drops',
  'stump',
]);

function lower(path) {
  return String(path).replace(/\\/g, '/').toLowerCase();
}

function isTreeMesh(mesh) {
  const path = lower(mesh);
  return path.startsWith(TREE_FOLDER) && !NOT_TREES.some((part) => path.includes(part));
}

function round(value) {
  return Math.round(value * 100) / 100;
}

function idOf(mesh, at) {
  return `${lower(mesh)}@${round(at.x)},${round(at.y)},${round(at.z)}`;
}

function horizontal(vector) {
  const length = Math.hypot(vector.x, vector.y);
  return length > 1e-6 ? new Vector3(vector.x / length, vector.y / length, 0) : null;
}

// The melee weapons named like an axe, as rightHandItem gives classes (32 hex digits).
function defaultAxes() {
  return new Set(
    itemClasses()
      .filter((entry) => entry.category === 'meleeWeapon' && /axe/i.test(entry.name))
      .map((entry) => classBytesHex(entry)),
  );
}

function axesOf(names) {
  const set = new Set();
  for (const name of names) {
    const entry = lookupItemClass(name);
    if (!entry) {
      throw new RangeError(`Trees: '${name}' is not an item class`);
    }
    set.add(classBytesHex(entry));
  }
  return set;
}

function positive(value, name, integer, max) {
  const valid =
    Number.isFinite(value) && value > 0 && value <= max && (!integer || Number.isInteger(value));
  if (!valid) {
    throw new RangeError(
      `Trees: ${name} must be ${integer ? 'a whole number' : 'a number'} above 0, at most ${max}`,
    );
  }
  return value;
}

function dropsOf(list) {
  if (!Array.isArray(list) || list.length > MAX_DROPS) {
    throw new TypeError(`Trees: drops must be a list of at most ${MAX_DROPS} { item, amount }`);
  }
  return Object.freeze(
    list.map((drop) => {
      if (!drop || !lookupItemClass(drop.item)) {
        throw new RangeError(`Trees: drop item '${drop?.item}' is not an item class`);
      }
      return Object.freeze({
        item: drop.item,
        amount: positive(drop.amount, 'a drop amount', true, 10000),
      });
    }),
  );
}

// The settings a definition gives, checked; `keys` the ones it may hold.
function settingsOf(given, keys) {
  if (given === null || typeof given !== 'object') {
    throw new TypeError('Trees: settings must be an object');
  }
  const out = {};
  for (const [key, value] of Object.entries(given)) {
    if (!keys.has(key)) {
      throw new TypeError(`Trees: unknown setting '${key}'`);
    }
    if (key === 'enabled') {
      out.enabled = value === true;
    } else if (key === 'hits' || key === 'trunkHits') {
      out[key] = positive(value, key, true, MAX_HITS);
    } else if (key === 'regrowMinutes') {
      out[key] = positive(value, key, false, 525600);
    } else if (key === 'regrowClearance') {
      if (!Number.isFinite(value) || value < 0 || value > 500) {
        throw new RangeError('Trees: regrowClearance must be 0 to 500 metres');
      }
      out[key] = value;
    } else if (key === 'reach' || key === 'height' || key === 'trunkRadius') {
      out[key] = positive(value, key, false, 100);
    } else if (key === 'swingSeconds') {
      out[key] = positive(value, key, false, 10);
    } else if (key === 'drops') {
      out.drops = dropsOf(value);
    } else if (key === 'dropTo') {
      if (value !== 'inventory' && value !== 'ground') {
        throw new RangeError("Trees: dropTo is 'inventory' or 'ground'");
      }
      out.dropTo = value;
    } else if (key === 'stump') {
      if (value !== null && typeof value !== 'string') {
        throw new TypeError('Trees: stump is a prop model or null');
      }
      out.stump = value;
    } else if (key === 'axes') {
      if (value !== null && !Array.isArray(value)) {
        throw new TypeError('Trees: axes is a list of item classes or null');
      }
      out.axes = value === null ? null : [...value];
      if (value !== null) {
        axesOf(value);
      }
    }
  }
  return out;
}

function distanceToSegment(point, a, b) {
  const ab = new Vector3(b.x - a.x, b.y - a.y, b.z - a.z);
  const ap = new Vector3(point.x - a.x, point.y - a.y, point.z - a.z);
  const length = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
  const t =
    length > 0 ? Math.max(0, Math.min(1, (ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / length)) : 0;
  return Math.hypot(ap.x - ab.x * t, ap.y - ab.y * t, ap.z - ab.z * t);
}

class Trees {
  // host: players, api (LevelEdit, Prop, GroundItem, World), emit, emitToPlayerClient.
  constructor(host, now = () => Date.now()) {
    this.host = host;
    this.now = now;
    this.config = { ...DEFAULTS };
    this.axeSet = null;
    // [{ pattern (lower-case mesh prefix), settings }], the last one set first.
    this.species = [];
    // `${world}\u0000${id}` -> record
    this.records = new Map();
    this.lastBlow = new Map();
    this.regrowTimer = null;
  }

  axes() {
    if (!this.axeSet) {
      this.axeSet = this.config.axes ? axesOf(this.config.axes) : defaultAxes();
    }
    return this.axeSet;
  }

  speciesOf(mesh) {
    const path = lower(mesh);
    const own = this.species.find((entry) => path.startsWith(entry.pattern));
    if (!own && !isTreeMesh(path)) {
      return null;
    }
    return { ...this.config, ...(own?.settings ?? {}) };
  }

  key(world, id) {
    return `${world}\u0000${id}`;
  }

  // ---- Blows --------------------------------------------------------------------------------

  // A swing a player's game reported (playerWeaponHit).
  weaponHit(player, hit) {
    if (!this.config.enabled || !player || !hit || !player.ready) {
      return;
    }
    // A blow by a lying trunk is on the trunk, whatever the game names (the trunk is the tree's
    // own mesh, and the hidden tree may be named for it).
    if (this.trunkBlow(player, hit)) {
      return;
    }
    const object = hit.levelObject;
    if (object && object.kind === 'brush' && this.speciesOf(object.name)) {
      this.standingBlow(player, hit, object);
    }
  }

  // Whether this swing of the player counts: an axe in hand, close enough, not too soon.
  counts(player, hit, settings) {
    if (!(hit.distance <= settings.reach)) {
      return false;
    }
    if (!this.axes().has(String(player.rightHandItem ?? ''))) {
      return false;
    }
    const now = this.now();
    const last = this.lastBlow.get(player.id) ?? -Infinity;
    if (now - last < settings.swingSeconds * 1000) {
      return false;
    }
    this.lastBlow.set(player.id, now);
    return true;
  }

  standingBlow(player, hit, object) {
    const settings = this.speciesOf(object.name);
    if (!this.counts(player, hit, settings)) {
      return;
    }
    const world = player.dimension ?? GLOBAL_WORLD;
    const id = idOf(object.name, object.at);
    let record = this.records.get(this.key(world, id));
    if (!record) {
      record = {
        id,
        world,
        mesh: object.name,
        at: new Vector3(object.at.x, object.at.y, object.at.z),
        rotation: object.rotation,
        scale: object.scale,
        state: 'standing',
        hits: 0,
        trunkHits: 0,
        felledAt: null,
        regrowAt: null,
        lastChopAt: null,
      };
      record.handle = this.handleOf(record);
    }
    if (record.state !== 'standing') {
      return;
    }
    const blow = Object.freeze({
      number: record.hits + 1,
      of: settings.hits,
      position: new Vector3(hit.position.x, hit.position.y, hit.position.z),
    });
    const tree = record.handle;
    if (!this.host.emit('playerTreeChop', player, tree, blow)) {
      return;
    }
    record.hits += 1;
    record.lastChopAt = this.now();
    this.records.set(this.key(world, id), record);
    this.tell(record, 'treeChop', blow);
    if (record.hits >= settings.hits) {
      this.fell(record, player);
    }
    this.arm();
  }

  // A blow by a felled trunk of the player's world: true when one lies there (counted or not).
  trunkBlow(player, hit) {
    const world = player.dimension ?? GLOBAL_WORLD;
    let nearest = null;
    let nearestGap = Infinity;
    for (const record of this.records.values()) {
      if (record.world !== world || record.state !== 'felled' || !record.tip) {
        continue;
      }
      const settings = this.speciesOf(record.mesh);
      const gap = distanceToSegment(hit.position, record.at, record.tip);
      if (gap <= settings.trunkRadius + TRUNK_MARGIN && gap < nearestGap) {
        nearest = record;
        nearestGap = gap;
      }
    }
    if (!nearest) {
      return false;
    }
    const settings = this.speciesOf(nearest.mesh);
    if (!this.counts(player, hit, settings)) {
      return true;
    }
    const blow = Object.freeze({
      number: nearest.trunkHits + 1,
      of: settings.trunkHits,
      position: new Vector3(hit.position.x, hit.position.y, hit.position.z),
    });
    const tree = nearest.handle;
    if (!this.host.emit('playerTrunkChop', player, tree, blow)) {
      return true;
    }
    nearest.trunkHits += 1;
    if (nearest.trunkHits >= settings.trunkHits) {
      this.cut(nearest, player, settings, blow.position);
    }
    return true;
  }

  cut(record, player, settings, where) {
    const items = settings.drops.map((drop) => ({ item: drop.item, amount: drop.amount }));
    const tree = record.handle;
    if (!this.host.emit('playerTrunkCut', player, tree, items)) {
      record.trunkHits = settings.trunkHits - 1;
      return;
    }
    const { GroundItem } = this.host.api;
    for (const drop of items) {
      if (!drop || !lookupItemClass(drop.item) || !(drop.amount >= 1)) {
        continue;
      }
      const amount = Math.min(10000, Math.floor(drop.amount));
      if (settings.dropTo === 'ground') {
        GroundItem.spawn(drop.item, where, undefined, amount, record.world);
      } else {
        player.giveItem(drop.item, amount);
      }
    }
    record.trunk?.destroy();
    record.trunk = null;
    record.state = 'cut';
    this.tell(record, 'trunkCut', items);
  }

  // ---- The fall -----------------------------------------------------------------------------

  // Fells a standing tree: false when treeFell refused it or it does not stand.
  fell(record, player, direction) {
    if (record.state !== 'standing') {
      return false;
    }
    const settings = this.speciesOf(record.mesh);
    let away = direction ? horizontal(direction) : null;
    if (!away && player?.position) {
      away = horizontal(
        new Vector3(record.at.x - player.position.x, record.at.y - player.position.y, 0),
      );
    }
    if (!away) {
      const turn = Math.random() * Math.PI * 2;
      away = new Vector3(Math.cos(turn), Math.sin(turn), 0);
    }
    const fall = {
      direction: away,
      seconds: Math.min(4, Math.max(2, 1.2 + settings.height * 0.12)),
    };
    const tree = record.handle;
    if (player && !this.host.emit('playerTreeFell', player, tree, fall)) {
      record.hits = Math.max(0, settings.hits - 1);
      return false;
    }
    const chosen = horizontal(fall.direction ?? away) ?? away;
    const seconds = Number.isFinite(fall.seconds) ? Math.min(10, Math.max(0.5, fall.seconds)) : 3;
    const { LevelEdit, Prop } = this.host.api;
    record.state = 'falling';
    record.direction = chosen;
    record.felledAt = this.now();
    record.regrowAt = record.felledAt + settings.regrowMinutes * 60000;
    record.edit = LevelEdit.apply({ name: record.mesh, at: record.at, hidden: true }, record.world);
    const start = new Quaternion(
      record.rotation.w,
      record.rotation.x,
      record.rotation.y,
      record.rotation.z,
    );
    record.trunk = Prop.spawn(record.mesh, record.at, start, record.scale, 'none', record.world);
    if (settings.stump) {
      record.stump = Prop.spawn(settings.stump, record.at, start, 1, 'static', record.world);
    }
    this.records.set(this.key(record.world, record.id), record);
    // The axis the top falls around: horizontal, at right angles to the fall.
    const axis = new Vector3(-chosen.y, chosen.x, 0);
    record.restAngle = 90;
    this.groundAngle(record, player, chosen, settings);
    this.tell(record, 'treeFalling', { direction: chosen, seconds });
    this.animate(record, start, axis, seconds, settings);
    this.arm();
    return true;
  }

  // The resting angle on a slope: the ground under the crown, asked of the chopper's game.
  groundAngle(record, player, direction, settings) {
    if (!player || !this.host.api.World?.snapToGround) {
      return;
    }
    const reach = settings.height * 0.8;
    const under = new Vector3(
      record.at.x + direction.x * reach,
      record.at.y + direction.y * reach,
      record.at.z,
    );
    Promise.resolve(this.host.api.World.snapToGround(player, under))
      .then((answer) => {
        if (!answer?.answered || !Number.isFinite(answer.z)) {
          return;
        }
        const drop = (record.at.z - answer.z) / reach;
        const slope = (Math.atan(drop) * 180) / Math.PI;
        record.restAngle = 90 + Math.max(-MAX_SLOPE_DEGREES, Math.min(MAX_SLOPE_DEGREES, slope));
      })
      .catch(() => {});
  }

  animate(record, start, axis, seconds, settings) {
    const began = this.now();
    const step = () => {
      if (record.state !== 'falling' || !record.trunk) {
        return;
      }
      const elapsed = (this.now() - began) / 1000;
      let angle;
      if (elapsed < seconds) {
        const share = elapsed / seconds;
        angle = record.restAngle * share * share;
      } else if (elapsed < seconds + BOUNCE_SECONDS) {
        angle =
          record.restAngle -
          BOUNCE_DEGREES * Math.sin((Math.PI * (elapsed - seconds)) / BOUNCE_SECONDS);
      } else {
        angle = record.restAngle;
      }
      const pose = Quaternion.fromAxisAngle(axis, (angle * Math.PI) / 180).mul(start);
      record.trunk.setPose(record.at, pose);
      if (elapsed >= seconds + BOUNCE_SECONDS) {
        this.landed(record, pose, settings);
        return;
      }
      record.fallTimer = setTimeout(step, FALL_STEP_MS);
      record.fallTimer.unref?.();
    };
    step();
  }

  landed(record, pose, settings) {
    record.state = 'felled';
    record.pose = pose;
    record.trunk.physics = 'static';
    const length = settings.height * (record.scale?.z ?? 1);
    const along = pose.rotateVector(new Vector3(0, 0, length));
    record.tip = new Vector3(record.at.x + along.x, record.at.y + along.y, record.at.z + along.z);
    const tree = record.handle;
    this.host.emit('treeFelled', tree);
    this.tell(record, 'treeFelled');
  }

  // ---- Keeping it across a restart ----------------------------------------------------------

  // What Trees.state gives for a record: plain data, the times in epoch milliseconds.
  stateOf(record) {
    const pose = record.pose ?? null;
    return {
      id: record.id,
      dimension: record.world,
      species: record.mesh,
      at: { x: record.at.x, y: record.at.y, z: record.at.z },
      rotation: {
        w: record.rotation.w,
        x: record.rotation.x,
        y: record.rotation.y,
        z: record.rotation.z,
      },
      scale: { x: record.scale?.x ?? 1, y: record.scale?.y ?? 1, z: record.scale?.z ?? 1 },
      state: record.state === 'falling' ? 'felled' : record.state,
      hits: record.hits,
      trunkHits: record.trunkHits,
      felledAt: record.felledAt,
      regrowAt: record.regrowAt,
      pose: pose ? { w: pose.w, x: pose.x, y: pose.y, z: pose.z } : null,
    };
  }

  // A kept tree back: a felled one lies at once (at its kept pose), a cut one stays hidden, a
  // chopped one keeps its blows. False for an entry that is not a tree or is already touched.
  restoreOne(entry) {
    const valid =
      entry &&
      typeof entry.species === 'string' &&
      this.speciesOf(entry.species) &&
      entry.at &&
      [entry.at.x, entry.at.y, entry.at.z].every(Number.isFinite) &&
      ['standing', 'felled', 'cut'].includes(entry.state);
    if (!valid) {
      return false;
    }
    const world = Number.isSafeInteger(entry.dimension) ? entry.dimension : GLOBAL_WORLD;
    const turn = entry.rotation ?? { w: 1, x: 0, y: 0, z: 0 };
    const record = this.treeAt(
      {
        name: entry.species,
        at: entry.at,
        rotation: new Quaternion(turn.w, turn.x, turn.y, turn.z),
        scale: new Vector3(entry.scale?.x ?? 1, entry.scale?.y ?? 1, entry.scale?.z ?? 1),
      },
      world,
    );
    if (this.records.has(this.key(world, record.id))) {
      return false;
    }
    const settings = this.speciesOf(record.mesh);
    record.hits = Number.isInteger(entry.hits) ? Math.max(0, Math.min(entry.hits, MAX_HITS)) : 0;
    record.trunkHits = Number.isInteger(entry.trunkHits) ? Math.max(0, entry.trunkHits) : 0;
    record.lastChopAt = this.now();
    this.records.set(this.key(world, record.id), record);
    if (entry.state === 'standing') {
      this.arm();
      return true;
    }
    const { LevelEdit, Prop } = this.host.api;
    record.felledAt = Number.isFinite(entry.felledAt) ? entry.felledAt : this.now();
    record.regrowAt = Number.isFinite(entry.regrowAt)
      ? entry.regrowAt
      : this.now() + settings.regrowMinutes * 60000;
    record.edit = LevelEdit.apply({ name: record.mesh, at: record.at, hidden: true }, world);
    if (settings.stump) {
      record.stump = Prop.spawn(settings.stump, record.at, record.rotation, 1, 'static', world);
    }
    if (entry.state === 'felled') {
      const kept = entry.pose;
      const lying = kept
        ? new Quaternion(kept.w, kept.x, kept.y, kept.z)
        : Quaternion.fromAxisAngle(new Vector3(0, 1, 0), Math.PI / 2).mul(
            new Quaternion(
              record.rotation.w,
              record.rotation.x,
              record.rotation.y,
              record.rotation.z,
            ),
          );
      record.trunk = Prop.spawn(record.mesh, record.at, lying, record.scale, 'static', world);
      record.state = 'falling';
      this.landedQuietly(record, lying, settings);
    } else {
      record.state = 'cut';
    }
    this.arm();
    return true;
  }

  // A restored trunk lies where it lay: no fall, no events.
  landedQuietly(record, pose, settings) {
    record.state = 'felled';
    record.pose = pose;
    const length = settings.height * (record.scale?.z ?? 1);
    const along = pose.rotateVector(new Vector3(0, 0, length));
    record.tip = new Vector3(record.at.x + along.x, record.at.y + along.y, record.at.z + along.z);
  }

  // ---- Regrowth -----------------------------------------------------------------------------

  arm() {
    if (this.regrowTimer || this.records.size === 0) {
      return;
    }
    this.regrowTimer = setInterval(() => this.process(), REGROW_CHECK_MS);
    this.regrowTimer.unref?.();
  }

  process() {
    const now = this.now();
    for (const record of [...this.records.values()]) {
      const settings = this.speciesOf(record.mesh);
      if (record.state === 'standing') {
        // A tree left half chopped heals after the regrowth time.
        if (
          record.lastChopAt !== null &&
          now - record.lastChopAt >= settings.regrowMinutes * 60000
        ) {
          this.records.delete(this.key(record.world, record.id));
        }
        continue;
      }
      if (
        record.state === 'falling' ||
        now < record.regrowAt ||
        this.someoneNear(record, settings)
      ) {
        continue;
      }
      const tree = record.handle;
      if (!this.host.emit('treeRegrow', tree)) {
        record.regrowAt = now + REGROW_RETRY_MS;
        continue;
      }
      this.regrowNow(record);
    }
    if (this.records.size === 0 && this.regrowTimer) {
      clearInterval(this.regrowTimer);
      this.regrowTimer = null;
    }
  }

  someoneNear(record, settings) {
    for (const player of this.host.players.values()) {
      const position = player.position;
      if (!player.ready || !position || (player.dimension ?? GLOBAL_WORLD) !== record.world) {
        continue;
      }
      if (
        Math.hypot(position.x - record.at.x, position.y - record.at.y) < settings.regrowClearance
      ) {
        return true;
      }
    }
    return false;
  }

  regrowNow(record) {
    clearTimeout(record.fallTimer);
    record.edit?.restore();
    record.trunk?.destroy();
    record.stump?.destroy();
    record.edit = null;
    record.trunk = null;
    record.stump = null;
    const was = record.state;
    record.state = 'standing';
    this.records.delete(this.key(record.world, record.id));
    if (was !== 'standing') {
      const tree = record.handle;
      this.host.emit('treeRegrown', tree);
      this.tell(record, 'treeRegrown');
    }
  }

  // ---- Telling the clients ------------------------------------------------------------------

  treeData(record) {
    return {
      id: record.id,
      species: record.mesh,
      position: { x: record.at.x, y: record.at.y, z: record.at.z },
      dimension: record.world,
      state: record.state,
    };
  }

  tell(record, event, extra) {
    const payload = { event, tree: this.treeData(record), extra: extra ?? null };
    for (const player of this.host.players.values()) {
      if (player.ready && (player.dimension ?? GLOBAL_WORLD) === record.world) {
        this.host.emitToPlayerClient(player.id, CLIENT_EVENT, payload);
      }
    }
  }

  // ---- The script handle and the global ------------------------------------------------------

  handleOf(record) {
    const trees = this;
    return Object.freeze({
      get id() {
        return record.id;
      },
      get species() {
        return record.mesh;
      },
      get position() {
        return new Vector3(record.at.x, record.at.y, record.at.z);
      },
      get dimension() {
        return record.world;
      },
      get hits() {
        return record.hits;
      },
      get health() {
        return Math.max(0, trees.speciesOf(record.mesh).hits - record.hits);
      },
      get state() {
        return record.state;
      },
      get felledAt() {
        return record.felledAt;
      },
      get regrowAt() {
        return record.regrowAt;
      },
      fell(direction, player) {
        if (direction !== undefined && direction !== null && !horizontal(direction)) {
          throw new TypeError('tree.fell: direction must be a horizontal Vector3');
        }
        trees.records.set(trees.key(record.world, record.id), record);
        return trees.fell(record, player ?? null, direction ?? null);
      },
      regrow() {
        trees.regrowNow(record);
      },
      setHits(hits) {
        if (!Number.isInteger(hits) || hits < 0 || hits > MAX_HITS) {
          throw new RangeError(`tree.setHits takes a whole number from 0 to ${MAX_HITS}`);
        }
        if (record.state !== 'standing') {
          return false;
        }
        record.hits = hits;
        record.lastChopAt = trees.now();
        trees.records.set(trees.key(record.world, record.id), record);
        trees.arm();
        if (hits >= trees.speciesOf(record.mesh).hits) {
          trees.fell(record, null, null);
        }
        return true;
      },
      toJSON() {
        return {
          ...trees.treeData(record),
          hits: record.hits,
          felledAt: record.felledAt,
          regrowAt: record.regrowAt,
        };
      },
    });
  }

  // A standing tree a script names by its mesh and place (a level object), for tree.fell.
  treeAt(levelObject, world) {
    const id = idOf(levelObject.name, levelObject.at);
    const known = this.records.get(this.key(world, id));
    if (known) {
      return known;
    }
    const record = {
      id,
      world,
      mesh: levelObject.name,
      at: new Vector3(levelObject.at.x, levelObject.at.y, levelObject.at.z),
      rotation: levelObject.rotation ?? new Quaternion(1, 0, 0, 0),
      scale: levelObject.scale ?? new Vector3(1, 1, 1),
      state: 'standing',
      hits: 0,
      trunkHits: 0,
      felledAt: null,
      regrowAt: null,
      lastChopAt: null,
    };
    record.handle = this.handleOf(record);
    return record;
  }

  api() {
    const trees = this;
    const worldOf = (world) => (world === undefined || world === null ? null : world);
    return Object.freeze({
      configure(settings) {
        Object.assign(trees.config, settingsOf(settings, SETTING_KEYS));
        trees.axeSet = null;
        return { ...trees.config };
      },
      get settings() {
        return { ...trees.config };
      },
      setSpecies(prefix, settings) {
        if (typeof prefix !== 'string' || !prefix.trim()) {
          throw new TypeError('Trees.setSpecies takes a mesh path or the start of one');
        }
        const pattern = lower(prefix.trim());
        const checked = settingsOf(settings, SPECIES_KEYS);
        trees.species = [
          { pattern, settings: checked },
          ...trees.species.filter((entry) => entry.pattern !== pattern),
        ];
        return true;
      },
      isTree(mesh) {
        return typeof mesh === 'string' && trees.speciesOf(mesh) !== null;
      },
      get(id, dimension) {
        const world = worldOf(dimension) ?? GLOBAL_WORLD;
        return trees.records.get(trees.key(world, String(id)))?.handle ?? null;
      },
      of(levelObject, dimension) {
        if (!levelObject || typeof levelObject.name !== 'string' || !levelObject.at) {
          throw new TypeError(
            'Trees.of takes a level object { name, at } (WorldRayHit.levelObject)',
          );
        }
        if (!trees.speciesOf(levelObject.name)) {
          return null;
        }
        return trees.treeAt(levelObject, worldOf(dimension) ?? GLOBAL_WORLD).handle;
      },
      at(position, radius, dimension) {
        if (!position || !Number.isFinite(position.x) || !(radius > 0)) {
          throw new TypeError('Trees.at takes a position and a radius above 0');
        }
        const world = worldOf(dimension);
        return [...trees.records.values()]
          .filter((record) => world === null || record.world === world)
          .filter(
            (record) =>
              Math.hypot(
                record.at.x - position.x,
                record.at.y - position.y,
                record.at.z - position.z,
              ) <= radius,
          )
          .map((record) => record.handle);
      },
      felled(dimension) {
        const world = worldOf(dimension);
        return [...trees.records.values()]
          .filter(
            (record) => record.state !== 'standing' && (world === null || record.world === world),
          )
          .map((record) => record.handle);
      },
      state(dimension) {
        const world = worldOf(dimension);
        return [...trees.records.values()]
          .filter((record) => world === null || record.world === world)
          .map((record) => trees.stateOf(record));
      },
      restore(list) {
        if (!Array.isArray(list)) {
          throw new TypeError('Trees.restore takes the list Trees.state gave');
        }
        let restored = 0;
        for (const entry of list) {
          if (trees.restoreOne(entry)) {
            restored += 1;
          }
        }
        return restored;
      },
      regrowAll(dimension) {
        const world = worldOf(dimension);
        let count = 0;
        for (const record of [...trees.records.values()]) {
          if (world === null || record.world === world) {
            if (record.state !== 'standing') {
              count += 1;
            }
            trees.regrowNow(record);
          }
        }
        return count;
      },
    });
  }
}

module.exports = { Trees, isTreeMesh, DEFAULTS };
