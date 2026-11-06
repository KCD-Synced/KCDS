'use strict';

// A player's pose animation from the Asset/Animation Studio (Player.setPoseAnimation, issue):
// the studio's keys of joint turns, as the NetworkPlayer field `poseAnimation` carries
// them (kcd2s/integration/pose_animation.hpp, which reads the field on every client):
//
//   "A1" <L|N loop> <S|M|I blend>, then per key "|<time>:" and its joints "<name>=<x>,<y>,<z>"
//   joined by ';'
//
// The options are the studio's animation (studio_get_animation, an exported resource):
// { keys: [{ time, pose: { <joint name>: [x, y, z] degrees }, prop? }], blend?, loop? }.
//
// A key's `prop` (the studio's prop keys) sets the player's physical prop from that
// key on: { hand, pitch, twist, gravity, damping, mass, air, bend, stiffness, collide, shape,
// curl, sway } (any of
// them; PROP_SETTINGS), written after the joints as "@<code><value>" joined by ';'. Numbers blend
// toward the next key that sets them; the hand and collide switch at the key.
//
// A key's `lift` (a baked game clip) raises the hips by that many metres (below 0
// lowers them: bent knees on the ground), -1 to 0.5, written after the joints as "#<lift>".
// A key's `lash` (the studio's Lash to keys) gives a whip's lash its shape: up to 16 points
// [along, right, up] from the handle's tip in the handle's frame, "~a,r,u;..." after the lift;
// while keys shape it the lash takes that shape, blended key to key, instead of physics.
//
// The animation's `lashTrack` (the studio's Lash from motion) shapes the lash at its own times
// instead of the keys: { rate (1 to 60 frames a second), length (0.1 to 6 metres), frames: [[yaw,
// pitch, ...] each segment's way from the handle's tip out, degrees in the handle's frame, 2 to 16
// segments, 2 to LASH_TRACK.mostFrames frames], space?: 'handle' | 'body' (the ways in the holding
// body's frame: the lash keeps its way when the wrist turns) }, written after the keys as
// "^<rate>,<length>[,b]:" and its frames joined by '/'.
//
// An animation defined by name (Animations.define, animation/studio_animations.cjs) may be up to
// MAX_DEFINED_BYTES: one longer than the field goes to every client once by its name and the
// field names it ("R1" <L|N> <name>).

const MAX_BYTES = 2000;
const MAX_DEFINED_BYTES = 30000;
const LIFT_RANGE = Object.freeze([-1, 0.5]);
// A key's lash shape (the studio's Lash to keys): points from the handle's tip in its frame.
const MAX_LASH_POINTS = 16;
const FARTHEST_LASH_POINT = 6;
// A lash track (kcd2s/integration/pose_animation.hpp PoseLashTrack).
const LASH_TRACK = Object.freeze({
  rates: [1, 60],
  lengths: [0.1, 6],
  mostFrames: 600,
  segments: [2, 16],
});
// The prop settings a key can set: their code in the field (kcd2s/integration/physical_prop.hpp
// physical_prop_setting) and range. The hand is 'RightHand' (0) or 'LeftHand' (1).
const PROP_SETTINGS = Object.freeze({
  hand: { code: 'h', hands: ['RightHand', 'LeftHand'] },
  pitch: { code: 'p', range: [0, 180] },
  twist: { code: 't', range: [-180, 180] },
  gravity: { code: 'g', range: [0, 30] },
  damping: { code: 'd', range: [0, 5] },
  mass: { code: 'm', range: [0.05, 10] },
  air: { code: 'a', range: [0, 2] },
  bend: { code: 'b', range: [0, 90] },
  stiffness: { code: 's', range: [0, 500] },
  collide: { code: 'c', choices: [0, 1, 2] },
  // A whip's lash posed instead of moved by physics: shape 0 physics, 1 the posed shape (no
  // gravity); curl and sway the degrees it bends from the handle to the tip.
  shape: { code: 'k', range: [0, 1] },
  curl: { code: 'u', range: [-540, 540] },
  sway: { code: 'w', range: [-540, 540] },
});
const MAX_KEYS = 32;
const MAX_JOINTS = 32;
const LONGEST = 60;
const BLENDS = Object.freeze({ spline: 'S', smooth: 'M', linear: 'I' });
const JOINT_NAME = /^[A-Za-z0-9_]{1,48}$/;

// A number as the field writes it (C's %g: at most six significant digits).
function written(value) {
  return String(Number(value.toPrecision(6)));
}

function poseAnimationOf(options, where = 'Player.setPoseAnimation', maxBytes = MAX_BYTES) {
  if (options === null || typeof options !== 'object' || !Array.isArray(options.keys)) {
    throw new TypeError(`${where} takes { keys: [{ time, pose }], blend?, loop? } or null`);
  }
  const blend = options.blend ?? 'spline';
  if (!Object.hasOwn(BLENDS, blend)) {
    throw new TypeError(`${where}: blend is 'spline', 'smooth' or 'linear'`);
  }
  if (options.loop !== undefined && typeof options.loop !== 'boolean') {
    throw new TypeError(`${where}: loop is true or false`);
  }
  const { keys } = options;
  if (keys.length === 0 || keys.length > MAX_KEYS) {
    throw new RangeError(`${where}: 1 to ${MAX_KEYS} keys`);
  }
  const clean = [];
  let previous = -1;
  keys.forEach((key, index) => {
    const time = key?.time;
    if (typeof time !== 'number' || !Number.isFinite(time)) {
      throw new TypeError(`${where}: key ${index} needs a time in seconds`);
    }
    if ((index === 0 && time !== 0) || time <= previous || time > LONGEST) {
      throw new RangeError(
        `${where}: the first key is at 0, each later than the one before, up to ${LONGEST} s`,
      );
    }
    previous = time;
    const pose = key.pose ?? {};
    if (typeof pose !== 'object' || Array.isArray(pose)) {
      throw new TypeError(`${where}: key ${index}'s pose is { joint: [x, y, z] }`);
    }
    const joints = Object.entries(pose);
    if (joints.length > MAX_JOINTS) {
      throw new RangeError(`${where}: at most ${MAX_JOINTS} joints a key`);
    }
    const turns = {};
    for (const [name, turn] of joints) {
      if (!JOINT_NAME.test(name)) {
        throw new TypeError(`${where}: '${name}' is not a joint name`);
      }
      if (
        !Array.isArray(turn) ||
        turn.length !== 3 ||
        !turn.every((part) => typeof part === 'number' && Number.isFinite(part))
      ) {
        throw new TypeError(`${where}: ${name}'s turn is [x, y, z] degrees`);
      }
      turns[name] = turn.map((part) => Number(part.toPrecision(6)));
    }
    const cleanKey = { time: Number(time.toPrecision(6)), pose: turns };
    if (key.lift !== undefined) {
      const [low, high] = LIFT_RANGE;
      if (typeof key.lift !== 'number' || !(key.lift >= low && key.lift <= high)) {
        throw new RangeError(`${where}: key ${index}'s lift is ${low} to ${high} metres`);
      }
      cleanKey.lift = Number(key.lift.toPrecision(6));
    }
    if (key.lash !== undefined) {
      cleanKey.lash = lashOf(key.lash, `${where}: key ${index}'s lash`);
    }
    if (key.prop !== undefined) {
      cleanKey.prop = propOf(key.prop, `${where}: key ${index}'s prop`);
    }
    clean.push(cleanKey);
  });
  const animation = { keys: clean, blend, loop: options.loop !== false };
  if (options.lashTrack !== undefined && options.lashTrack !== null) {
    animation.lashTrack = lashTrackOf(options.lashTrack, `${where}: lashTrack`);
  }
  const field = poseAnimationField(animation);
  if (Buffer.byteLength(field) > maxBytes) {
    throw new RangeError(`${where}: the animation is longer than ${maxBytes} bytes as sent`);
  }
  return { animation, field };
}

// A key's lash shape checked: 1 to MAX_LASH_POINTS points [along, right, up] in the handle's
// frame, each within FARTHEST_LASH_POINT metres; a copy.
function lashOf(lash, what) {
  if (!Array.isArray(lash) || lash.length === 0 || lash.length > MAX_LASH_POINTS) {
    throw new TypeError(`${what} is 1 to ${MAX_LASH_POINTS} points [along, right, up]`);
  }
  return lash.map((point) => {
    if (
      !Array.isArray(point) ||
      point.length !== 3 ||
      !point.every((part) => typeof part === 'number' && Math.abs(part) <= FARTHEST_LASH_POINT)
    ) {
      throw new RangeError(`${what}: each point is three numbers within ${FARTHEST_LASH_POINT} m`);
    }
    return point.map((part) => Number(part.toPrecision(6)));
  });
}

// A lash track checked (LASH_TRACK): a copy.
function lashTrackOf(track, what) {
  if (track === null || typeof track !== 'object' || !Array.isArray(track.frames)) {
    throw new TypeError(`${what} is { rate, length, frames: [[yaw, pitch, ...]] }`);
  }
  const [lowRate, highRate] = LASH_TRACK.rates;
  if (typeof track.rate !== 'number' || !(track.rate >= lowRate && track.rate <= highRate)) {
    throw new RangeError(`${what}: rate is ${lowRate} to ${highRate} frames a second`);
  }
  const [shortest, longest] = LASH_TRACK.lengths;
  if (typeof track.length !== 'number' || !(track.length >= shortest && track.length <= longest)) {
    throw new RangeError(`${what}: length is ${shortest} to ${longest} metres`);
  }
  const { frames } = track;
  if (frames.length < 2 || frames.length > LASH_TRACK.mostFrames) {
    throw new RangeError(`${what}: 2 to ${LASH_TRACK.mostFrames} frames`);
  }
  const numbers = Array.isArray(frames[0]) ? frames[0].length : 0;
  const [fewest, most] = LASH_TRACK.segments;
  if (numbers % 2 !== 0 || numbers / 2 < fewest || numbers / 2 > most) {
    throw new RangeError(`${what}: each frame is ${fewest} to ${most} segments [yaw, pitch, ...]`);
  }
  if (track.space !== undefined && track.space !== 'body' && track.space !== 'handle') {
    throw new TypeError(`${what}: space is 'handle' (the default) or 'body'`);
  }
  return {
    rate: Number(track.rate.toPrecision(6)),
    length: Number(track.length.toPrecision(6)),
    ...(track.space === 'body' ? { space: 'body' } : {}),
    frames: frames.map((frame, index) => {
      const good =
        Array.isArray(frame) &&
        frame.length === numbers &&
        frame.every(
          (part, at) => typeof part === 'number' && Math.abs(part) <= (at % 2 === 0 ? 180 : 90),
        );
      if (!good) {
        throw new RangeError(
          `${what}: frame ${index} is ${numbers} numbers, yaws within 180°, pitches within 90°`,
        );
      }
      return frame.map((part) => Number(part.toPrecision(6)));
    }),
  };
}

// A key's prop settings checked: a copy with each known setting in its range.
function propOf(prop, what) {
  if (prop === null || typeof prop !== 'object' || Array.isArray(prop)) {
    throw new TypeError(`${what} is { hand?, pitch?, twist?, gravity?, damping?, ... }`);
  }
  const clean = {};
  for (const [name, value] of Object.entries(prop)) {
    const setting = PROP_SETTINGS[name];
    if (!setting) {
      throw new TypeError(`${what}: '${name}' is not a prop setting`);
    }
    if (setting.hands) {
      if (!setting.hands.includes(value)) {
        throw new TypeError(`${what}: hand is 'RightHand' or 'LeftHand'`);
      }
      clean[name] = value;
    } else if (setting.choices) {
      if (!setting.choices.includes(value)) {
        throw new RangeError(`${what}: ${name} is ${setting.choices.join(', ')}`);
      }
      clean[name] = value;
    } else {
      const [low, high] = setting.range;
      if (typeof value !== 'number' || !(value >= low && value <= high)) {
        throw new RangeError(`${what}: ${name} is ${low} to ${high}`);
      }
      clean[name] = Number(value.toPrecision(6));
    }
  }
  return clean;
}

// A key's prop as the field writes it: "@<code><value>" joined by ';', codes in order (as the
// C++ side keeps them); '' for none.
function propField(prop) {
  if (!prop) {
    return '';
  }
  const entries = Object.entries(prop).map(([name, value]) => {
    const setting = PROP_SETTINGS[name];
    const number = setting.hands ? setting.hands.indexOf(value) : value;
    return [setting.code, written(number)];
  });
  entries.sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0));
  return entries.length === 0
    ? ''
    : `@${entries.map(([code, value]) => `${code}${value}`).join(';')}`;
}

// The field's text of a checked animation; joints in name order, as the C++ side keeps them.
function poseAnimationField(animation) {
  let text = `A1${animation.loop ? 'L' : 'N'}${BLENDS[animation.blend]}`;
  for (const key of animation.keys) {
    const joints = Object.keys(key.pose)
      .sort()
      .map((name) => `${name}=${key.pose[name].map(written).join(',')}`);
    const lift = key.lift === undefined ? '' : `#${written(key.lift)}`;
    const lash = key.lash
      ? `~${key.lash.map((point) => point.map(written).join(',')).join(';')}`
      : '';
    text += `|${written(key.time)}:${joints.join(';')}${lift}${lash}${propField(key.prop)}`;
  }
  const track = animation.lashTrack;
  if (track) {
    const frames = track.frames.map((frame) => frame.map(written).join(',')).join('/');
    const body = track.space === 'body' ? ',b' : '';
    text += `^${written(track.rate)},${written(track.length)}${body}:${frames}`;
  }
  return text;
}

// The field that names an animation defined by name, played looping or once.
function poseAnimationReference(name, loop) {
  return `R1${loop ? 'L' : 'N'}${name}`;
}

module.exports = {
  poseAnimationOf,
  poseAnimationField,
  poseAnimationReference,
  propOf,
  PROP_SETTINGS,
  MAX_BYTES,
  MAX_DEFINED_BYTES,
};
