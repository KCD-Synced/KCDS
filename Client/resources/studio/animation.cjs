'use strict';

// The Asset/Animation Studio's animation: keys in time, each a pose of the
// character: turns of its joints, in degrees about the joint's parent's X, Y and Z axes. The game
// client adds the turns on top of the body's own animation (studio/studio_pose.hpp:
// { op: 'studioPose', joints: [[id, x, y, z, w], ...] }, quaternions).
//
// Between two keys each joint's turn is blended, as the animation's blend says; a joint a key does
// not turn is not turned in it:
//
//   'spline'  the default: a curve through every key (Catmull-Rom on the turns' degrees, still
//             at the first and the last key, round the end to the start when it loops), so a
//             swing flows through its keys
//   'smooth'  each key eased into and out of (a spherical blend, slow at both keys)
//   'linear'  a spherical blend at an even speed

const TURN_STEP = 5;
const TIME_STEP = 0.1;
const NEW_KEY_AFTER = 0.5;
const MOST_KEYS = 64;
const LONGEST = 60;
const BLENDS = Object.freeze(['spline', 'smooth', 'linear']);
// Events: an effect of the game's catalogue or one of its sound triggers.
const EVENT_KINDS = Object.freeze(['effect', 'sound']);
const MOST_EVENTS = 32;
const EFFECT_NAME = /^[A-Za-z0-9_]+\.[A-Za-z0-9_]+\.[A-Za-z0-9_]+$/;
const SOUND_NAME = /^[A-Za-z0-9_]{1,96}$/;
const JOINT_NAME = /^[A-Za-z0-9_]{1,48}$/;

function radians(degrees) {
  return (degrees * Math.PI) / 180;
}

function multiply(a, b) {
  const [ax, ay, az, aw] = a;
  const [bx, by, bz, bw] = b;
  return [
    aw * bx + ax * bw + ay * bz - az * by,
    aw * by - ax * bz + ay * bw + az * bx,
    aw * bz + ax * by - ay * bx + az * bw,
    aw * bw - ax * bx - ay * by - az * bz,
  ];
}

function about(axis, degrees) {
  const half = radians(degrees) / 2;
  const turn = [0, 0, 0, Math.cos(half)];
  turn[axis] = Math.sin(half);
  return turn;
}

// The turn x, then y, then z (degrees) as a quaternion (x, y, z, w).
function quaternion([x, y, z]) {
  return multiply(about(2, z), multiply(about(1, y), about(0, x)));
}

function slerp(a, b, share) {
  let dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  const to = dot < 0 ? b.map((part) => -part) : b;
  dot = Math.abs(dot);
  if (dot > 0.9995) {
    const mixed = a.map((part, index) => part + (to[index] - part) * share);
    const length = Math.hypot(...mixed);
    return mixed.map((part) => part / length);
  }
  const angle = Math.acos(dot);
  const sin = Math.sin(angle);
  const first = Math.sin((1 - share) * angle) / sin;
  const second = Math.sin(share * angle) / sin;
  return a.map((part, index) => part * first + to[index] * second);
}

const IDENTITY = Object.freeze([0, 0, 0, 1]);

function round(value, places) {
  const scale = 10 ** places;
  return Math.round(value * scale) / scale;
}

function wrapDegrees(value) {
  const wrapped = ((((value + 180) % 360) + 360) % 360) - 180;
  return wrapped === -180 ? 180 : wrapped;
}

// A key's degrees as given, up to two turns either way: a baked clip's keys go on past 180
// (they are unwrapped key to key), and wrapping them made the blend between two keys spin the
// long way round.
function keptDegrees(value) {
  return Math.max(-720, Math.min(720, value));
}

// A key's lift of the hips (metres), as the game client takes it.
function liftOf(value) {
  const lift = Number(value);
  return Number.isFinite(lift) ? round(Math.max(-1, Math.min(0.5, lift)), 3) : undefined;
}

// A key's lash shape (Lash to keys): up to 16 points [along, right, up] within 6 m, to the
// millimetre; undefined for none or a bad one.
function lashOf(value) {
  if (!Array.isArray(value) || value.length === 0 || value.length > 16) {
    return undefined;
  }
  const points = value.map((point) =>
    Array.isArray(point) && point.length === 3 && point.every((part) => Math.abs(Number(part)) <= 6)
      ? point.map((part) => round(Number(part) || 0, 3))
      : null,
  );
  return points.every(Boolean) ? points : undefined;
}

// A whip's lash track (Lash from motion: lash_motion.cjs): the lash's shape at its
// own times, `rate` frames a second from the animation's start, its `length` metres in equal
// segments, each frame the segments' ways from the handle's tip to the lash's end as two whole
// degrees each in the handle's frame: yaw (toward its right side from along it) then pitch
// (toward its up side), [yaw0, pitch0, yaw1, pitch1, ...]. With `space: 'body'` the ways are in
// the holding body's frame instead (yaw from its forward toward its right, pitch toward up), so
// the lash keeps its way when the wrist turns. A looping animation's frames go round.
const LASH_TRACK = Object.freeze({
  rates: [1, 60],
  lengths: [0.1, 6],
  mostFrames: 600,
  segments: [2, 16],
});

// A lash track as the animation keeps it, or undefined for none or a bad one.
function lashTrackOf(value) {
  if (!value || typeof value !== 'object' || !Array.isArray(value.frames)) {
    return undefined;
  }
  const rate = Number(value.rate);
  const length = Number(value.length);
  const [lowRate, highRate] = LASH_TRACK.rates;
  const [shortest, longest] = LASH_TRACK.lengths;
  if (!(rate >= lowRate && rate <= highRate) || !(length >= shortest && length <= longest)) {
    return undefined;
  }
  const { frames } = value;
  if (frames.length < 2 || frames.length > LASH_TRACK.mostFrames || !Array.isArray(frames[0])) {
    return undefined;
  }
  const numbers = frames[0].length;
  const [fewest, most] = LASH_TRACK.segments;
  if (numbers % 2 !== 0 || numbers / 2 < fewest || numbers / 2 > most) {
    return undefined;
  }
  const good = frames.every(
    (frame) =>
      Array.isArray(frame) &&
      frame.length === numbers &&
      frame.every((part, index) => {
        const limit = index % 2 === 0 ? 180 : 90;
        return typeof part === 'number' && Math.abs(part) <= limit;
      }),
  );
  if (!good) {
    return undefined;
  }
  const track = {
    rate: round(rate, 3),
    length: round(length, 3),
    frames: frames.map((frame) => frame.map((part) => round(part, 1))),
  };
  // In the holding body's frame (forward, right, up) instead of the handle's.
  if (value.space === 'body') {
    track.space = 'body';
  }
  return track;
}

// A whip's lash keys (the lash posed by hand): { length, keys: [{ time, ways }] },
// each key the lash's whole shape at its time, its pieces' ways in the holding body's frame as
// [yaw, pitch] degrees (lashWay). Between keys each piece turns smoothly through them (a curve
// through the keys' ways, round the loop when it loops); the animation's lash track is baked of
// them (bakeLashKeys), so a player's lash is exactly what was keyed, with no gravity or physics.
const MOST_LASH_KEYS = 64;

function lashKeyOf(value) {
  const time = Number(value?.time);
  if (!Number.isFinite(time) || time < 0 || time > LONGEST || !Array.isArray(value.ways)) {
    return undefined;
  }
  const { ways } = value;
  const [fewest, most] = LASH_TRACK.segments;
  if (ways.length < fewest || ways.length > most) {
    return undefined;
  }
  const good = ways.every(
    (way) =>
      Array.isArray(way) &&
      way.length === 2 &&
      Math.abs(Number(way[0])) <= 180 &&
      Math.abs(Number(way[1])) <= 90,
  );
  if (!good) {
    return undefined;
  }
  const kept = ways.map((way) => way.map((part) => round(Number(part), 1)));
  return { time: round(time, 2), ways: kept };
}

function lashKeysOf(value) {
  const length = Number(value?.length);
  const [shortest, longest] = LASH_TRACK.lengths;
  if (!(length >= shortest && length <= longest) || !Array.isArray(value.keys)) {
    return null;
  }
  const keys = value.keys.slice(0, MOST_LASH_KEYS).map(lashKeyOf).filter(Boolean);
  keys.sort((a, b) => a.time - b.time);
  const count = keys[0]?.ways.length;
  const same = keys.filter((key) => key.ways.length === count);
  return same.length > 0 ? { length: round(length, 3), keys: same } : null;
}

// The [yaw, pitch] degrees of a unit way (lashWay's inverse).
function wayAngles(way) {
  const pitch = (Math.asin(Math.max(-1, Math.min(1, way[2]))) * 180) / Math.PI;
  const yaw = Math.abs(way[2]) > 0.99999 ? 0 : (Math.atan2(way[1], way[0]) * 180) / Math.PI;
  return [round(yaw, 1), round(pitch, 1)];
}

// The lash keys' pieces' ways at `time` (unit ways): a Catmull-Rom curve through the keys' ways,
// piece by piece, round the loop when it loops (`length` the animation's).
function lashKeysWays(lashKeys, time, loop, length) {
  const { keys } = lashKeys;
  const count = keys.length;
  const ways = (key) => key.ways.map(([yaw, pitch]) => lashWay(yaw, pitch));
  if (count === 1) {
    return ways(keys[0]);
  }
  const at = (index) => {
    if (loop && length > 0) {
      const rounds = Math.floor(index / count);
      const key = keys[((index % count) + count) % count];
      return { time: key.time + rounds * length, ways: ways(key) };
    }
    const key = keys[Math.min(count - 1, Math.max(0, index))];
    return { time: key.time, ways: ways(key) };
  };
  let when = time;
  if (loop && length > 0) {
    when = ((time % length) + length) % length;
  }
  let first = keys.findLastIndex((key) => key.time <= when);
  if (first === -1) {
    if (!loop) {
      return ways(keys[0]);
    }
    first = count - 1;
    when += length;
  }
  if (!loop && first === count - 1) {
    return ways(keys[count - 1]);
  }
  const [a, b, c, d] = [first - 1, first, first + 1, first + 2].map(at);
  const span = c.time - b.time;
  const share = span > 1e-6 ? Math.min(1, Math.max(0, (when - b.time) / span)) : 0;
  const s2 = share * share;
  const s3 = s2 * share;
  return b.ways.map((way, piece) => {
    const mixed = [0, 1, 2].map((axis) => {
      const p0 = a.ways[piece][axis];
      const p1 = way[axis];
      const p2 = c.ways[piece][axis];
      const p3 = d.ways[piece][axis];
      return (
        0.5 *
        (2 * p1 +
          (p2 - p0) * share +
          (2 * p0 - 5 * p1 + 4 * p2 - p3) * s2 +
          (3 * p1 - p0 - 3 * p2 + p3) * s3)
      );
    });
    const size = Math.hypot(...mixed);
    return size > 1e-6 ? mixed.map((part) => part / size) : way;
  });
}

// A segment's way from its yaw and pitch (degrees) in the handle's frame [along, right, up].
function lashWay(yaw, pitch) {
  const y = radians(yaw);
  const p = radians(pitch);
  return [Math.cos(p) * Math.cos(y), Math.cos(p) * Math.sin(y), Math.sin(p)];
}

// The way between two unit ways at `share`, a unit way again (the first when they are opposite).
function wayBetween(a, b, share) {
  const mixed = a.map((part, axis) => part + (b[axis] - part) * share);
  const length = Math.hypot(...mixed);
  return length > 1e-6 ? mixed.map((part) => part / length) : a;
}

// The lash's points at `time` from its track (as the game client plays it:
// kcd2s/integration/pose_animation.hpp pose_lash_at): the two frames around it, each segment's way
// blended, the segments chained from the handle's tip, so the lash is always its whole length.
function lashTrackAt(track, time, loop) {
  const count = track.frames.length;
  const at = Math.max(0, time) * track.rate;
  let first = Math.floor(at);
  let second = first + 1;
  let share = at - first;
  if (loop) {
    first %= count;
    second %= count;
  } else if (first >= count - 1) {
    first = count - 1;
    second = count - 1;
    share = 0;
  }
  const a = track.frames[first];
  const b = track.frames[second];
  const segments = a.length / 2;
  const piece = track.length / segments;
  const points = [];
  let point = [0, 0, 0];
  for (let segment = 0; segment < segments; segment += 1) {
    const from = lashWay(a[segment * 2], a[segment * 2 + 1]);
    const to = lashWay(b[segment * 2], b[segment * 2 + 1]);
    const way = wayBetween(from, to, share);
    point = point.map((part, axis) => part + way[axis] * piece);
    points.push(point.map((part) => round(part, 4)));
  }
  return points;
}

// Two keys' lash shapes blended at `share` piece by piece: each piece's way blended and its
// length between the two, chained from the handle's tip (blending the points themselves drew a
// bent lash shorter between two keys: it coiled into the hand).
function lashBetween(from, to, share) {
  const points = [];
  let point = [0, 0, 0];
  let fromBefore = [0, 0, 0];
  let toBefore = [0, 0, 0];
  for (let index = 0; index < from.length; index += 1) {
    const a = from[index].map((part, axis) => part - fromBefore[axis]);
    const b = to[index].map((part, axis) => part - toBefore[axis]);
    fromBefore = from[index];
    toBefore = to[index];
    const aLength = Math.hypot(...a);
    const bLength = Math.hypot(...b);
    const aWay = aLength > 1e-6 ? a.map((part) => part / aLength) : [1, 0, 0];
    const bWay = bLength > 1e-6 ? b.map((part) => part / bLength) : aWay;
    const way = wayBetween(aWay, bWay, share);
    const length = aLength + (bLength - aLength) * share;
    point = point.map((part, axis) => part + way[axis] * length);
    points.push(point.map((part) => round(part, 4)));
  }
  return points;
}

// What a prop key can set: the prop's hand, grip and physics from that key on, as
// the server's Player.setPoseAnimation takes them (Backend/scripting/runtimes/js/animation/
// pose_animation.cjs PROP_SETTINGS, kcd2s/integration/physical_prop.hpp). Numbers blend toward
// the next key that sets them; the hand and collide switch at the key.
const PROP_SETTINGS = Object.freeze({
  hand: { hands: ['RightHand', 'LeftHand'] },
  pitch: { range: [0, 180] },
  twist: { range: [-180, 180] },
  gravity: { range: [0, 30] },
  damping: { range: [0, 5] },
  mass: { range: [0.05, 10] },
  air: { range: [0, 2] },
  bend: { range: [0, 90] },
  stiffness: { range: [0, 500] },
  collide: { choices: [0, 1, 2] },
  // A whip's lash posed instead of moved by physics (shape 0 to 1) and that shape's bend.
  shape: { range: [0, 1] },
  curl: { range: [-540, 540] },
  sway: { range: [-540, 540] },
});

// A key's prop as the animation keeps it: the known settings in their ranges, or null for none.
function propOf(given) {
  if (!given || typeof given !== 'object' || Array.isArray(given)) {
    return null;
  }
  const prop = {};
  for (const [name, setting] of Object.entries(PROP_SETTINGS)) {
    const value = given[name];
    if (value === undefined) {
      continue;
    }
    if (setting.hands) {
      if (setting.hands.includes(value)) {
        prop[name] = value;
      }
    } else if (setting.choices) {
      if (setting.choices.includes(value)) {
        prop[name] = value;
      }
    } else if (typeof value === 'number' && Number.isFinite(value)) {
      const [low, high] = setting.range;
      prop[name] = round(Math.min(high, Math.max(low, value)), 3);
    }
  }
  return Object.keys(prop).length > 0 ? prop : null;
}

// A key's place of your mesh: { at: [x, y, z] metres, turn: [x, y, z] degrees }, as
// the mesh's place has them (in the world, or in its joint while held); null when not one.
const MESH_KEY_REACH = 1000;

function meshKeyOf(given) {
  const three = (value) =>
    Array.isArray(value) &&
    value.length === 3 &&
    value.every((part) => typeof part === 'number' && Number.isFinite(part));
  if (!given || typeof given !== 'object' || !three(given.at) || !three(given.turn)) {
    return null;
  }
  return {
    at: given.at.map((part) => round(Math.min(MESH_KEY_REACH, Math.max(-MESH_KEY_REACH, part)), 3)),
    turn: given.turn.map((part) => round(keptDegrees(part), 1)),
  };
}

class Animation {
  constructor() {
    // Each key: { time (seconds), pose: { [joint id]: [x, y, z] degrees }, prop? (propOf) }, in
    // time order.
    this.keys = [{ time: 0, pose: {} }];
    this.key = 0;
    this.playing = false;
    this.loop = true;
    this.blend = 'spline';
    // Only the upper body (the spine and what hangs from it) is animated: the legs walk.
    this.topBody = false;
    // Effects and sounds at times (eventOf), in time order.
    this.events = [];
    // A whip's lash at its own times (lashTrackOf), or null: it shapes the lash instead of the
    // keys' lash shapes.
    this.lashTrack = null;
    // The lash posed by hand (lashKeysOf), or null: when there are, the lash track is baked of
    // them (bakeLashKeys).
    this.lashKeys = null;
    this.startedAt = 0;
  }

  nextBlend(direction) {
    const index = BLENDS.indexOf(this.blend);
    this.blend = BLENDS[(index + BLENDS.length + Math.sign(direction || 1)) % BLENDS.length];
    return this.blend;
  }

  current() {
    return this.keys[this.key];
  }

  length() {
    return this.keys[this.keys.length - 1].time;
  }

  turnOf(joint) {
    return this.current().pose[joint] ?? [0, 0, 0];
  }

  // Turns a joint of the chosen key by one step about an axis (0 X, 1 Y, 2 Z), up (+1) or down.
  turn(joint, axis, direction) {
    const next = [...this.turnOf(joint)];
    // Past ±180 it goes round to the other side, unless the key's degrees are unwrapped (a baked
    // clip's), which go on.
    const turned = next[axis] + Math.sign(direction) * TURN_STEP;
    next[axis] = Math.abs(next[axis]) <= 180 ? wrapDegrees(turned) : keptDegrees(turned);
    this.setTurn(joint, next);
    return next;
  }

  setTurn(joint, turn) {
    const pose = this.current().pose;
    if (turn.every((part) => part === 0)) {
      delete pose[joint];
    } else {
      pose[joint] = turn.map((part) => keptDegrees(Number(part) || 0));
    }
  }

  resetJoint(joint) {
    delete this.current().pose[joint];
  }

  chooseKey(index) {
    if (index >= 0 && index < this.keys.length) {
      this.key = index;
      return true;
    }
    return false;
  }

  // A key after the last one, with the chosen key's pose; it becomes the chosen key.
  addKey() {
    if (this.keys.length >= MOST_KEYS) {
      return false;
    }
    const time = Math.min(LONGEST, round(this.length() + NEW_KEY_AFTER, 2));
    const pose = JSON.parse(JSON.stringify(this.current().pose));
    const key = { time, pose };
    if (this.current().prop) {
      key.prop = { ...this.current().prop };
    }
    if (this.current().mesh) {
      key.mesh = meshKeyOf(this.current().mesh);
    }
    this.keys.push(key);
    this.key = this.keys.length - 1;
    return true;
  }

  // ---- Prop keys: the prop's hand, grip and physics set at a key ----

  // Sets the chosen key's prop (propOf), or takes it away (null).
  setProp(given) {
    const prop = propOf(given);
    if (prop) {
      this.current().prop = prop;
    } else {
      delete this.current().prop;
    }
  }

  hasPropKeys() {
    return this.keys.some((one) => one.prop);
  }

  // ---- Mesh keys: your mesh's place and turn at a key ----

  // Sets the chosen key's place of your mesh (meshKeyOf), or takes it away (null).
  setMeshKey(given) {
    const mesh = meshKeyOf(given);
    if (mesh) {
      this.current().mesh = mesh;
    } else {
      delete this.current().mesh;
    }
  }

  hasMeshKeys() {
    return this.keys.some((one) => one.mesh);
  }

  // Your mesh's place and turn at `time`: from the mesh key at or before it toward the next one
  // (eased as the keys blend, each turn about x, y and z on its own), the first before the first
  // and the last after the last; null without mesh keys.
  meshAt(time) {
    const meshKeys = this.keys.filter((one) => one.mesh);
    if (meshKeys.length === 0) {
      return null;
    }
    const before = meshKeys.filter((one) => one.time <= time).at(-1);
    const after = meshKeys.find((one) => one.time > time);
    if (!before || !after) {
      const only = before ?? after;
      return { at: [...only.mesh.at], turn: [...only.mesh.turn] };
    }
    const linear = (time - before.time) / (after.time - before.time);
    const share = this.blend === 'linear' ? linear : linear * linear * (3 - 2 * linear);
    const between = (from, to) =>
      from.map((part, axis) => round(part + (to[axis] - part) * share, 4));
    return {
      at: between(before.mesh.at, after.mesh.at),
      turn: between(before.mesh.turn, after.mesh.turn),
    };
  }

  // The prop's settings the keys set at `time`: each from the key at or before it that sets it
  // toward the next key that sets it (eased, even for linear), as the game client does
  // (pose_animation.hpp pose_prop_at); {} for no prop keys.
  propAt(time) {
    const values = {};
    for (const [name, setting] of Object.entries(PROP_SETTINGS)) {
      const settingKeys = this.keys.filter((one) => one.prop && one.prop[name] !== undefined);
      if (settingKeys.length === 0) {
        continue;
      }
      const before = settingKeys.filter((one) => one.time <= time).at(-1);
      const after = settingKeys.find((one) => one.time > time);
      if (!before) {
        values[name] = after.prop[name];
        continue;
      }
      if (!after || !setting.range) {
        values[name] = before.prop[name];
        continue;
      }
      const linear = (time - before.time) / (after.time - before.time);
      const share = this.blend === 'linear' ? linear : linear * linear * (3 - 2 * linear);
      values[name] = before.prop[name] + (after.prop[name] - before.prop[name]) * share;
    }
    return values;
  }

  removeKey() {
    if (this.keys.length <= 1) {
      return false;
    }
    this.keys.splice(this.key, 1);
    this.key = Math.min(this.key, this.keys.length - 1);
    if (this.key === 0 || this.keys[0].time !== 0) {
      this.keys[0].time = 0;
    }
    return true;
  }

  // Moves the chosen key in time by one step; the first key stays at 0 and a key stays between
  // its neighbours.
  moveKey(direction) {
    if (this.key === 0) {
      return false;
    }
    const before = this.keys[this.key - 1].time + TIME_STEP;
    const after = this.keys[this.key + 1]?.time ?? LONGEST + TIME_STEP;
    const time = round(this.current().time + Math.sign(direction) * TIME_STEP, 2);
    if (time < before - 1e-9 || time > after - TIME_STEP + 1e-9) {
      return false;
    }
    this.current().time = time;
    return true;
  }

  // Every turned joint at `time` (seconds): { [joint]: quaternion }.
  sample(time) {
    const keys = this.keys;
    let next = keys.findIndex((one) => one.time > time);
    if (next === -1) {
      return this.poseOf(keys[keys.length - 1]);
    }
    if (next === 0) {
      return this.poseOf(keys[0]);
    }
    const from = keys[next - 1];
    const to = keys[next];
    const linear = (time - from.time) / (to.time - from.time);
    if (this.blend === 'spline') {
      return this.splineAt(next - 1, linear);
    }
    const share = this.blend === 'smooth' ? linear * linear * (3 - 2 * linear) : linear;
    const joints = new Set([...Object.keys(from.pose), ...Object.keys(to.pose)]);
    const pose = {};
    for (const joint of joints) {
      const a = from.pose[joint] ? quaternion(from.pose[joint]) : IDENTITY;
      const b = to.pose[joint] ? quaternion(to.pose[joint]) : IDENTITY;
      pose[joint] = slerp(a, b, share);
    }
    return pose;
  }

  // The key `index` away from `from` (looping round when the animation loops; the last key is
  // the first one again then), or null past an end.
  neighbour(from, index) {
    const keys = this.keys;
    const at = from + index;
    if (at >= 0 && at < keys.length) {
      return { key: keys[at], time: keys[at].time };
    }
    if (!this.loop || keys.length < 3) {
      return null;
    }
    const length = this.length();
    if (at < 0) {
      // Before the first key: the one before the last (the last is the first again).
      const key = keys[keys.length - 1 + at];
      return { key, time: key.time - length };
    }
    const key = keys[at - keys.length + 1];
    return { key, time: key.time + length };
  }

  // Between key `index` and the next at `share` (0 to 1): each joint's degrees on a
  // Catmull-Rom curve through the keys (a cubic Hermite curve, its slope at a key the change
  // from the key before to the key after; still where there is no key before or after).
  splineAt(index, share) {
    const pose = {};
    for (const [joint, turn] of Object.entries(this.splineDegrees(index, share))) {
      pose[joint] = quaternion(turn);
    }
    return pose;
  }

  // The same in degrees: { [joint]: [x, y, z] }.
  splineDegrees(index, share) {
    const k0 = this.neighbour(index, -1);
    const k1 = this.neighbour(index, 0);
    const k2 = this.neighbour(index, 1);
    const k3 = this.neighbour(index, 2);
    const span = k2.time - k1.time;
    const joints = new Set();
    for (const one of [k0, k1, k2, k3]) {
      Object.keys(one?.key.pose ?? {}).forEach((joint) => joints.add(joint));
    }
    const s2 = share * share;
    const s3 = s2 * share;
    const h00 = 2 * s3 - 3 * s2 + 1;
    const h10 = s3 - 2 * s2 + share;
    const h01 = -2 * s3 + 3 * s2;
    const h11 = s3 - s2;
    const turnIn = (one, joint) => one?.key.pose[joint] ?? [0, 0, 0];
    const slope = (before, after, joint, axis) => {
      if (!before || !after) {
        return 0;
      }
      return (
        (turnIn(after, joint)[axis] - turnIn(before, joint)[axis]) / (after.time - before.time)
      );
    };
    const turns = {};
    for (const joint of joints) {
      turns[joint] = [0, 1, 2].map((axis) => {
        const p1 = turnIn(k1, joint)[axis];
        const p2 = turnIn(k2, joint)[axis];
        const m1 = slope(k0, k2, joint, axis) * span;
        const m2 = slope(k1, k3, joint, axis) * span;
        return h00 * p1 + h10 * m1 + h01 * p2 + h11 * m2;
      });
    }
    return turns;
  }

  // Every joint's turn in degrees at `time`, as the animation plays it (a new key there gets
  // these): { [joint]: [x, y, z] }.
  turnsAt(time) {
    const keys = this.keys;
    const next = keys.findIndex((one) => one.time > time);
    const copy = (pose) => JSON.parse(JSON.stringify(pose));
    if (next === -1) {
      return copy(keys[keys.length - 1].pose);
    }
    if (next === 0) {
      return copy(keys[0].pose);
    }
    const from = keys[next - 1];
    const to = keys[next];
    const linear = (time - from.time) / (to.time - from.time);
    if (this.blend === 'spline') {
      return this.splineDegrees(next - 1, linear);
    }
    const share = this.blend === 'smooth' ? linear * linear * (3 - 2 * linear) : linear;
    const turns = {};
    for (const joint of new Set([...Object.keys(from.pose), ...Object.keys(to.pose)])) {
      const a = from.pose[joint] ?? [0, 0, 0];
      const b = to.pose[joint] ?? [0, 0, 0];
      turns[joint] = a.map((part, axis) => part + (b[axis] - part) * share);
    }
    return turns;
  }

  // Whether a key lifts the hips (a baked game clip).
  hasLift() {
    return this.keys.some((one) => one.lift !== undefined);
  }

  // How far the hips are lifted at `time` (metres): between the keys that lift them, eased (even
  // for linear), as the game client plays it (kcd2s/integration/pose_animation.hpp
  // pose_lift_at); 0 when none does.
  liftAt(time) {
    const lifting = this.keys.filter((one) => one.lift !== undefined);
    if (lifting.length === 0) {
      return 0;
    }
    const next = lifting.findIndex((one) => one.time > time);
    if (next === -1) {
      return lifting[lifting.length - 1].lift;
    }
    if (next === 0) {
      return lifting[0].lift;
    }
    const from = lifting[next - 1];
    const to = lifting[next];
    const linear = (time - from.time) / (to.time - from.time);
    const share = this.blend === 'linear' ? linear : linear * linear * (3 - 2 * linear);
    return from.lift + (to.lift - from.lift) * share;
  }

  // Whether a lash track or keys shape the whip's lash (Lash from motion, Lash to keys).
  hasLash() {
    return this.lashTrack !== null || this.keys.some((one) => one.lash);
  }

  // The lash's shape at `time`, as the game client plays it (kcd2s/integration/pose_animation.hpp
  // pose_lash_at): from the lash track when there is one, else between the keys that shape it,
  // piece by piece (lashBetween); null for none.
  lashAt(time) {
    if (this.lashTrack) {
      return lashTrackAt(this.lashTrack, time, this.loop);
    }
    const shaping = this.keys.filter((one) => one.lash);
    if (shaping.length === 0) {
      return null;
    }
    const next = shaping.findIndex((one) => one.time > time);
    if (next === -1 || next === 0) {
      return shaping[next === 0 ? 0 : shaping.length - 1].lash;
    }
    const from = shaping[next - 1];
    const to = shaping[next];
    if (from.lash.length !== to.lash.length) {
      return from.lash;
    }
    const share = (time - from.time) / (to.time - from.time);
    return lashBetween(from.lash, to.lash, share);
  }

  // The key at `time` (within half a hundredth of a second), or -1.
  keyAt(time) {
    return this.keys.findIndex((one) => Math.abs(one.time - time) < 0.005);
  }

  // A key at `time` with the turns the animation has there; it becomes the chosen key. False
  // when a key is there already (it is chosen) or there are no more keys.
  addKeyAt(time) {
    const at = round(Math.min(LONGEST, Math.max(0, time)), 2);
    const existing = this.keyAt(at);
    if (existing >= 0) {
      this.key = existing;
      return false;
    }
    if (this.keys.length >= MOST_KEYS) {
      return false;
    }
    const pose = {};
    for (const [joint, turn] of Object.entries(this.turnsAt(at))) {
      const rounded = turn.map((part) => keptDegrees(round(part, 1)));
      if (rounded.some((part) => part !== 0)) {
        pose[joint] = rounded;
      }
    }
    const key = { time: at, pose };
    if (this.hasLift()) {
      key.lift = round(this.liftAt(at), 3);
    }
    this.keys.push(key);
    this.keys.sort((a, b) => a.time - b.time);
    this.key = this.keyAt(at);
    return true;
  }

  // Moves key `index` to `time`, between its neighbours (a twentieth of a second from each); the
  // first key stays at 0. False when nothing moved.
  moveKeyTo(index, time) {
    if (index <= 0 || index >= this.keys.length) {
      return false;
    }
    const gap = 0.05;
    const before = this.keys[index - 1].time + gap;
    const after = this.keys[index + 1] ? this.keys[index + 1].time - gap : LONGEST;
    const at = round(Math.min(after, Math.max(before, time)), 2);
    if (at === this.keys[index].time) {
      return false;
    }
    this.keys[index].time = at;
    return true;
  }

  poseOf(key) {
    const pose = {};
    for (const [joint, turn] of Object.entries(key.pose)) {
      pose[joint] = quaternion(turn);
    }
    return pose;
  }

  // The game client's message for a pose ({ [joint]: quaternion }).
  static message(pose, lift = 0) {
    const joints = Object.entries(pose).map(([joint, turn]) => [
      Number(joint),
      ...turn.map((part) => round(part, 6)),
    ]);
    return lift ? { op: 'studioPose', joints, lift: round(lift, 4) } : { op: 'studioPose', joints };
  }

  // The game client's message for the pose and the hips' lift at `time`.
  messageAt(time) {
    return Animation.message(this.sample(time), this.liftAt(time));
  }

  // Where playing is at `now` (milliseconds), in seconds; playing stops at the end without loop.
  playTime(now) {
    const length = this.length();
    const elapsed = Math.max(0, (now - this.startedAt) / 1000);
    if (length <= 0) {
      return 0;
    }
    if (this.loop) {
      return elapsed % length;
    }
    if (elapsed >= length) {
      this.playing = false;
      return length;
    }
    return elapsed;
  }

  toJSON() {
    const data = {
      keys: this.keys.map((one) => {
        const key = { time: one.time, pose: { ...one.pose } };
        if (one.lift !== undefined) {
          key.lift = one.lift;
        }
        if (one.lash) {
          key.lash = one.lash.map((point) => [...point]);
        }
        if (one.prop) {
          key.prop = { ...one.prop };
        }
        if (one.mesh) {
          key.mesh = meshKeyOf(one.mesh);
        }
        return key;
      }),
      blend: this.blend,
      loop: this.loop,
    };
    if (this.topBody) {
      data.topBody = true;
    }
    if (this.events.length > 0) {
      data.events = this.events.map((one) => ({ ...one }));
    }
    if (this.lashKeys) {
      data.lashKeys = {
        length: this.lashKeys.length,
        keys: this.lashKeys.keys.map((key) => ({
          time: key.time,
          ways: key.ways.map((way) => [...way]),
        })),
      };
    }
    if (this.lashTrack) {
      data.lashTrack = {
        rate: this.lashTrack.rate,
        length: this.lashTrack.length,
        frames: this.lashTrack.frames.map((frame) => [...frame]),
        ...(this.lashTrack.space ? { space: this.lashTrack.space } : {}),
      };
    }
    return data;
  }

  // ---- Events: an effect or a sound at a time ----

  // Adds { time, kind: 'effect' | 'sound', name, joint?, scale? }; false for one that is not.
  addEvent(given) {
    const event = eventOf(given);
    if (!event || this.events.length >= MOST_EVENTS) {
      return false;
    }
    this.events.push(event);
    this.events.sort((a, b) => a.time - b.time);
    return true;
  }

  removeEvent(index) {
    if (index < 0 || index >= this.events.length) {
      return false;
    }
    this.events.splice(index, 1);
    return true;
  }

  // The events whose time playing passed from `from` to `to` seconds (round the end when it
  // looped: `to` smaller than `from`).
  eventsBetween(from, to) {
    if (to >= from) {
      return this.events.filter((one) => one.time > from && one.time <= to);
    }
    // Those before the end first, then those after the start again.
    return [
      ...this.events.filter((one) => one.time > from),
      ...this.events.filter((one) => one.time <= to),
    ];
  }

  // A kept animation; false (and nothing changed) when it is not one.
  restore(data) {
    if (!data || !Array.isArray(data.keys) || data.keys.length === 0) {
      return false;
    }
    const keys = [];
    for (const one of data.keys.slice(0, MOST_KEYS)) {
      const time = Number(one?.time);
      if (!Number.isFinite(time) || time < 0 || time > LONGEST) {
        return false;
      }
      const pose = {};
      for (const [joint, turn] of Object.entries(one.pose ?? {})) {
        if (/^\d+$/.test(joint) && Array.isArray(turn) && turn.length === 3) {
          pose[joint] = turn.map((part) => keptDegrees(Number(part) || 0));
        }
      }
      const key = { time: round(time, 2), pose };
      const lift = one.lift === undefined ? undefined : liftOf(one.lift);
      if (lift !== undefined) {
        key.lift = lift;
      }
      const lash = one.lash === undefined ? undefined : lashOf(one.lash);
      if (lash !== undefined) {
        key.lash = lash;
      }
      const prop = propOf(one.prop);
      if (prop) {
        key.prop = prop;
      }
      const mesh = meshKeyOf(one.mesh);
      if (mesh) {
        key.mesh = mesh;
      }
      keys.push(key);
    }
    keys.sort((a, b) => a.time - b.time);
    keys[0].time = 0;
    for (let index = 1; index < keys.length; index += 1) {
      if (keys[index].time <= keys[index - 1].time) {
        return false;
      }
    }
    this.keys = keys;
    this.key = 0;
    this.playing = false;
    this.blend = BLENDS.includes(data.blend) ? data.blend : 'spline';
    this.loop = data.loop !== false;
    this.topBody = data.topBody === true;
    this.events = (Array.isArray(data.events) ? data.events : [])
      .slice(0, MOST_EVENTS)
      .map(eventOf)
      .filter(Boolean)
      .sort((a, b) => a.time - b.time);
    this.lashTrack = lashTrackOf(data.lashTrack) ?? null;
    this.lashKeys = data.lashKeys ? lashKeysOf(data.lashKeys) : null;
    return true;
  }

  // The lash keys' ways at `time` (unit ways in the body's frame), or null without lash keys.
  lashKeysWaysAt(time) {
    return this.lashKeys ? lashKeysWays(this.lashKeys, time, this.loop, this.length()) : null;
  }

  // The lash track baked of the lash keys (40 frames a second, in the body's frame), so the
  // lash plays exactly as keyed here and on every player; no lash keys: left as it is.
  bakeLashKeys(rate = 40) {
    if (!this.lashKeys || this.lashKeys.keys.length === 0) {
      return false;
    }
    const seconds = this.length();
    const count = this.loop
      ? Math.max(2, Math.round(seconds * rate))
      : Math.floor(seconds * rate) + 1;
    const frames = [];
    for (let frame = 0; frame < Math.max(2, count); frame += 1) {
      const ways = this.lashKeysWaysAt(frame / rate);
      frames.push(ways.flatMap((way) => wayAngles(way).map((part) => Math.round(part))));
    }
    this.lashTrack = { rate, length: this.lashKeys.length, space: 'body', frames };
    return true;
  }
}

// An event as the animation keeps it, or null: a time within the animation's longest, a kind,
// the effect's (Library.group.name) or the sound trigger's name, a joint's name and a size.
function eventOf(given) {
  const time = Number(given?.time);
  const kind = given?.kind;
  const name = typeof given?.name === 'string' ? given.name : '';
  if (!Number.isFinite(time) || time < 0 || time > LONGEST || !EVENT_KINDS.includes(kind)) {
    return null;
  }
  const nameOk = kind === 'effect' ? EFFECT_NAME.test(name) : SOUND_NAME.test(name);
  if (!nameOk) {
    return null;
  }
  const event = { time: round(time, 2), kind, name };
  if (kind === 'effect') {
    if (typeof given.joint === 'string' && JOINT_NAME.test(given.joint)) {
      event.joint = given.joint;
    }
    const scale = Number(given.scale);
    if (Number.isFinite(scale) && scale > 0) {
      event.scale = Math.min(32, Math.max(0.01, round(scale, 2)));
    }
  }
  return event;
}

module.exports = {
  Animation,
  quaternion,
  slerp,
  eventOf,
  propOf,
  meshKeyOf,
  lashTrackOf,
  lashTrackAt,
  lashBetween,
  lashWay,
  wayAngles,
  lashKeysOf,
  MOST_LASH_KEYS,
  LASH_TRACK,
  BLENDS,
  TURN_STEP,
  TIME_STEP,
  PROP_SETTINGS,
};
