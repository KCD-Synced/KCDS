'use strict';

// Npc.useObject: an NPC sits on a seat or lies on a bed of a placed SmartObject the way the
// game's own people do, from the game's own data (Libs/SmartObjects.xml through
// smart_objects/use_helpers.json): it walks to where the helper's sit-down (or lie-down) fragment
// starts on the side of the seat's approach helper (use_motion.json), plays it onto the seat, then
// holds the seat's idle until stopUsingObject plays the stand-up. Everything it plays is Npc.playAnimation, so nearby players see it and a player who
// comes later sees the NPC on its seat. A seat holds one NPC at a time.
//
//   npcObjectUseStarted(npc, object, place)          once it sits or lies
//   npcObjectUseEnded(npc, object, place, reason)    'stopped' (stood up), 'blocked' (it could not
//       walk there), 'interrupted' (another order, an animation of a script, a hit, death,
//       removal) or 'refused' (its body lacks the fragment)

const HELPERS = require('../smart_objects/use_helpers.json');
const MOTION = require('../smart_objects/use_motion.json');
const { BLUEPRINTS, worldPoint } = require('../smart_objects/smart_objects.cjs');

const useState = Symbol('kcds.npcObjectUse');
// How near the approach helper the walk ends, and how far out towards it (a share of the way) the
// NPC stands after it stood up.
const APPROACH_RADIUS = 0.3;
const LEAVE_SHARE = 0.45;

// a * b for quaternions { x, y, z, w }.
function multiply(a, b) {
  return {
    w: a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    x: a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
    y: a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
    z: a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
  };
}

// The vector [x, y, z] turned by quaternion q.
function rotate(q, [x, y, z]) {
  const tx = 2 * (q.y * z - q.z * y);
  const ty = 2 * (q.z * x - q.x * z);
  const tz = 2 * (q.x * y - q.y * x);
  return {
    x: x + q.w * tx + (q.y * tz - q.z * ty),
    y: y + q.w * ty + (q.z * tx - q.x * tz),
    z: z + q.w * tz + (q.x * ty - q.y * tx),
  };
}

const quaternion = ([x, y, z, w]) => ({ x, y, z, w });

// The levels' seats and beds (smart_objects/level_seats.json, built by
// Dev/tools/smart_objects/build_level_seats.py): read the first time a script asks.
let levelSeatsLoaded = null;
function levelSeatTable() {
  levelSeatsLoaded ??= require('../smart_objects/level_seats.json');
  return levelSeatsLoaded;
}
// How far SmartObject.levelSeats looks at most, and how many it answers.
const LEVEL_SEATS_RADIUS = 100;
const LEVEL_SEATS_MOST = 64;
// The catalog row behind a level seat's handle.
const levelSeatRows = new WeakMap();

// A turn by `degrees` counter-clockwise about the up axis.
function yawTurn(degrees) {
  const half = (degrees * Math.PI) / 360;
  return { x: 0, y: 0, z: Math.sin(half), w: Math.cos(half) };
}

// Where the game's own sit-down or lie-down `down` starts so that it ends on the seat at `seatAt`:
// the fragment moves and turns the body (use_motion.json), so it starts that motion back from the
// seat. The body ends facing the seat's way or the opposite one (a bed is lain on across its
// helper); of the two, the start on the side of the approach helper `stand` is the one the game
// uses. Null for a fragment whose motion is not known.
function downStart(down, seatAt, facing, stand) {
  const motion = MOTION[down.join('|')];
  if (!motion) {
    return null;
  }
  const [right, forward, turn] = motion;
  let best = null;
  for (const end of [facing, multiply(facing, yawTurn(180))]) {
    const begin = multiply(end, yawTurn(-turn));
    const moved = rotate(begin, [right, forward, 0]);
    const at = { x: seatAt.x - moved.x, y: seatAt.y - moved.y, z: seatAt.z };
    const distance = Math.hypot(at.x - stand.x, at.y - stand.y);
    if (best === null || distance < best.distance) {
      best = { at, facing: begin, distance };
    }
  }
  return best;
}

class ObjectUses {
  constructor(host) {
    this.host = host;
    // "<object id>:<place>", or "level:<guid>" for a level seat -> the NPC on it.
    this.taken = new Map();
    // A level seat's handle by its GUID (SmartObject.levelSeats gives the same one each time).
    this.levelSeatHandles = new Map();
    host.on('npcIntentDone', (npc, status) => this.walked(npc, status));
    host.on('npcAnimationEnd', (npc, fragment, reason) => this.animated(npc, reason));
    host.on('npcDeath', (npc) => this.end(npc, 'interrupted'));
    host.on('npcDestroy', (npc) => this.end(npc, 'interrupted'));
  }

  // The seats and beds of a placed object, by place: [{ place, category, free }].
  places(object) {
    return this.seatsOf(object).seats.map(({ place, key, helperClass }) => ({
      place,
      category: HELPERS[helperClass].category,
      free: !this.taken.has(key),
    }));
  }

  // What useObject can use of `object`: a SmartObject a script placed (its seats by place) or a
  // seat or bed of the level (SmartObject.levelSeats, its one place 0). { name, dimension, seats:
  // [{ place, key, helperClass, seatAt, seatTurn }] }, dimension null for the level's (every
  // world has them).
  seatsOf(object) {
    const row = object !== null && typeof object === 'object' ? levelSeatRows.get(object) : null;
    if (row) {
      const [guid, helperClass, x, y, z, qx, qy, qz, qw] = row;
      return {
        name: `the level's seat ${guid}`,
        dimension: null,
        seats: [
          {
            place: 0,
            key: `level:${guid}`,
            helperClass,
            seatAt: { x, y, z },
            seatTurn: { x: qx, y: qy, z: qz, w: qw },
          },
        ],
      };
    }
    const placed = object && this.host.smartObjects.placed.get(object.id);
    if (!placed || placed !== object) {
      throw new TypeError(
        'useObject takes a SmartObject from SmartObject.spawn or a seat from SmartObject.levelSeats',
      );
    }
    const blueprint = BLUEPRINTS.get(object.blueprint.toLowerCase());
    return {
      name: object.blueprint,
      dimension: object.dimension,
      seats: blueprint.uses
        .filter(([, , , helper]) => HELPERS[helper])
        .map(([place, at, turn, helperClass]) => ({
          place,
          key: `${object.id}:${place}`,
          helperClass,
          seatAt: worldPoint(object, at),
          seatTurn: multiply(object.facing, quaternion(turn)),
        })),
    };
  }

  // SmartObject.levelSeats: the level's seats and beds within `radius` metres of `position`
  // (across the ground), nearest first, at most LEVEL_SEATS_MOST of them.
  levelSeats(position, radius = 10) {
    if (
      position === null ||
      typeof position !== 'object' ||
      ![position.x, position.y].every((n) => typeof n === 'number' && Number.isFinite(n))
    ) {
      throw new TypeError('SmartObject.levelSeats: position must be { x, y, z }');
    }
    if (typeof radius !== 'number' || !(radius > 0) || radius > LEVEL_SEATS_RADIUS) {
      throw new RangeError(
        `SmartObject.levelSeats: radius is more than 0 and at most ${LEVEL_SEATS_RADIUS} m`,
      );
    }
    const rows = levelSeatTable()[this.host.smartObjects.level()] ?? [];
    const near = [];
    for (const row of rows) {
      const far = Math.hypot(row[2] - position.x, row[3] - position.y);
      if (far <= radius) {
        near.push([far, row]);
      }
    }
    near.sort((a, b) => a[0] - b[0]);
    return near.slice(0, LEVEL_SEATS_MOST).map(([, row]) => this.levelSeat(row));
  }

  // One handle a level seat keeps (the same object every time, so a script can compare them).
  levelSeat(row) {
    let seat = this.levelSeatHandles.get(row[0]);
    if (!seat) {
      const uses = this;
      seat = Object.freeze({
        guid: row[0],
        kind: HELPERS[row[1]].category === 'Lie' ? 'bed' : 'seat',
        position: Object.freeze({ x: row[2], y: row[3], z: row[4] }),
        get free() {
          return !uses.taken.has(`level:${row[0]}`);
        },
      });
      this.levelSeatHandles.set(row[0], seat);
      levelSeatRows.set(seat, row);
    }
    return seat;
  }

  // Npc.useObject(object, { place?, side? }). False when no seat of it is free, the place is
  // taken, the NPC is dead, or it uses something already.
  use(npc, object, options = {}) {
    if (options === null || typeof options !== 'object') {
      throw new TypeError('useObject options must be an object');
    }
    const target = this.seatsOf(object);
    const usable = target.seats;
    if (usable.length === 0) {
      throw new TypeError(`useObject: ${target.name} has no seat or bed`);
    }
    if (options.place !== undefined && !usable.some(({ place }) => place === options.place)) {
      throw new RangeError(
        `useObject: ${target.name} has no seat or bed at place ${options.place}`,
      );
    }
    if (
      !npc.alive ||
      npc[useState] ||
      (target.dimension !== null && npc.dimension !== target.dimension)
    ) {
      return false;
    }
    const seat = usable.find(
      ({ place, key }) =>
        (options.place === undefined || place === options.place) && !this.taken.has(key),
    );
    if (!seat) {
      return false;
    }
    const { place, key, helperClass, seatAt, seatTurn } = seat;
    const helper = HELPERS[helperClass];
    const approach =
      options.side === undefined
        ? helper.approaches[0]
        : helper.approaches.find((candidate) => candidate.name === options.side);
    if (!approach) {
      throw new RangeError(
        `useObject: a seat of ${target.name} is approached from ` +
          helper.approaches.map((candidate) => `'${candidate.name}'`).join(', '),
      );
    }
    // Where its approach helper is, and how the seat faces.
    const offset = rotate(seatTurn, approach.at);
    const stand = { x: seatAt.x + offset.x, y: seatAt.y + offset.y, z: seatAt.z + offset.z };
    // Where it stands up to: out of the seat towards the side it came from.
    const leave = {
      x: seatAt.x + offset.x * LEAVE_SHARE,
      y: seatAt.y + offset.y * LEAVE_SHARE,
      z: seatAt.z + offset.z * LEAVE_SHARE,
    };
    const facing = multiply(seatTurn, quaternion(approach.turn));
    // It walks to where the game's sit-down starts; without its motion, onto the seat itself.
    const start = downStart(approach.down, seatAt, facing, stand) ?? {
      at: seatAt,
      facing,
    };
    const idle = helper.idle[approach.name] ?? helper.idle.none ?? Object.values(helper.idle)[0];
    this.taken.set(key, npc);
    const state = {
      object,
      place,
      key,
      approach,
      idle,
      start,
      leave,
      phase: 'walking',
      ordering: true,
    };
    npc[useState] = state;
    const walkTo = start.at === seatAt ? stand : start.at;
    const walking = npc.moveTo(walkTo, { speed: 'walk', radius: APPROACH_RADIUS });
    state.ordering = false;
    if (!walking) {
      // No walk there (the approach point is off the map): the seat stays free.
      npc[useState] = undefined;
      this.taken.delete(key);
      return false;
    }
    return true;
  }

  // Npc.stopUsingObject: it stands up (or stops walking there). False when it uses nothing.
  stop(npc) {
    const state = npc[useState];
    if (!state) {
      return false;
    }
    if (state.phase === 'walking' || state.phase === 'sitting') {
      // Stopping on the way, or while it sits down: it ends where it is.
      if (state.phase === 'sitting') {
        state.phase = 'ending';
        npc.stopAnimation();
      }
      this.end(npc, 'stopped');
      return true;
    }
    if (state.phase === 'using') {
      state.phase = 'standing';
      const [fragment, tags] = state.approach.up;
      npc.playAnimation(fragment, { tags });
    }
    return true;
  }

  // What the NPC uses now: { object, place }, or null.
  current(npc) {
    const state = npc[useState];
    return state ? { object: state.object, place: state.place } : null;
  }

  walked(npc, status) {
    const state = npc[useState];
    if (!state || state.phase !== 'walking') {
      return;
    }
    if (status !== 'reached') {
      this.end(npc, 'blocked');
      return;
    }
    // Exactly where the sit-down starts, then the sit-down, which carries it onto the seat. The
    // walk ends first, or the body would walk on from there.
    this.host.npcs.endOrder(npc);
    npc.position = state.start.at;
    npc.rotation = state.start.facing;
    state.phase = 'sitting';
    const [fragment, tags] = state.approach.down;
    npc.playAnimation(fragment, { tags });
  }

  animated(npc, reason) {
    const state = npc[useState];
    if (!state) {
      return;
    }
    if (state.phase === 'sitting') {
      if (reason !== 'finished') {
        this.end(npc, reason === 'refused' ? 'refused' : 'interrupted');
        return;
      }
      const [fragment, tags] = state.idle;
      npc.playAnimation(fragment, { tags, loop: true });
      state.phase = 'using';
      this.host.emit('npcObjectUseStarted', npc, state.object, state.place);
      return;
    }
    if (state.phase === 'using') {
      // Its idle ended by something else than stopUsingObject: a script's animation, a hit.
      this.end(npc, 'interrupted');
      return;
    }
    if (state.phase === 'standing' && reason !== 'replaced') {
      // The stand-up carried it out of the seat; one whose motion is not known is put out of it.
      if (!MOTION[state.approach.up.join('|')]) {
        npc.position = state.leave;
      }
      this.end(npc, 'stopped');
    }
  }

  // A script's order (moveTo, follow, flee, patrol, attack) ends the use first; one sitting or
  // lying stands where it is.
  ordered(npc) {
    const state = npc[useState];
    if (!state || state.ordering) {
      return;
    }
    if (state.phase === 'using') {
      state.phase = 'ending';
      npc.stopAnimation();
    }
    this.end(npc, 'interrupted');
  }

  end(npc, reason) {
    const state = npc[useState];
    if (!state) {
      return;
    }
    npc[useState] = undefined;
    this.taken.delete(state.key);
    this.host.emit('npcObjectUseEnded', npc, state.object, state.place, reason);
  }

  // A placed object that goes ends every use of it: its NPCs stand up where they are.
  objectGone(object) {
    for (const [key, npc] of [...this.taken]) {
      if (key.startsWith(`${object.id}:`)) {
        if (npc[useState]?.phase === 'using') {
          npc.stopAnimation();
        }
        this.end(npc, 'interrupted');
      }
    }
  }
}

module.exports = { ObjectUses };
