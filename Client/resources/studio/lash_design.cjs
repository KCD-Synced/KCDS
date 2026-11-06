'use strict';

// A whip's lash designed on the hand's motion (Lash from motion, mode "designed"): the
// lash's shape set frame by frame the way a whip is animated, not left to a simulation (which, on
// a crack, whirled the lash round the hand or balled it up as its settings changed by a hair).
// From the hand's recorded path (lash_motion.cjs handlePath) it finds the throw: the fast move
// that stops hard, from when the hand speeds up to when it stops, and its direction. Then:
//
//   the throw   a turn runs along the lash from the hand to the tip (rolledLash): each piece swings
//               from the way it had to the throw's way, over the top, one after another, faster
//               toward the tip (rollPower); the turned pieces are the part along the throw, those
//               turning the loop, the rest as it was. The first frame of the throw is the frame
//               before it, and only the pieces in the turn move against each other, so the lash
//               rolls, it never snaps; when the turn reaches the tip the lash is straight along
//               the throw: the crack
//   the rest    the lash follows the hand like a rope drawn through the air: each piece is put
//               a piece from the one before it, toward where it was, and it sinks, the tip most
//               (drawn); it trails the hand, droops tip first after the crack and lies on the
//               floor, and it keeps no swing of its own, so it cannot whirl, bounce or ball up
//
// Every number of it can be changed (DESIGN, an animation's `design` options). The lash goes round
// twice; the second round is kept as the animation's lash track (the same format as the simulated
// one, animation.cjs lashTrackOf), its end blended into its start.

const { handlePath, wayDegrees, TRACK_SEGMENTS, TRACK_RATE } = require('./lash_motion.cjs');
const { frameOf } = require('./lash_keys.cjs');

const STEP_RATE = 400;
const PIECES = 24;
const SEAM_SECONDS = 0.2;

// How the designed lash moves, by default, and the range each may be set in:
//   turnSeconds    how long each piece takes to swing over to the throw (seconds: the loop's
//                  size; less is a tighter loop)
//   rollAfter      how long after the hand stops the turn takes to reach the tip (seconds)
//   rollPower      how the turn speeds up along the lash: 1 evenly, above 1 slow at the hand and
//                  fast at the tip (a whip's crack)
//   fallSpeed      how fast the lash's tip sinks outside the throw (m/s)
//   rootFallShare  the share of that the piece at the handle sinks (the pieces between by the
//                  square of how far along they are: the tip drops first)
//   rootKeep       how much of the handle's line the first piece keeps (a bend out of the handle,
//                  not a corner)
//   throwShare     the share of the hand's top speed that is the throw's start and end
//   stopSeconds    how soon after its fastest moment a throw stops (seconds)
const DESIGN = Object.freeze({
  turnSeconds: { value: 0.1, range: [0.02, 0.5] },
  rollAfter: { value: 0.1, range: [0, 0.6] },
  rollPower: { value: 1.6, range: [0.5, 4] },
  fallSpeed: { value: 4, range: [0, 20] },
  rootFallShare: { value: 0.15, range: [0, 1] },
  rootKeep: { value: 0.15, range: [0, 0.9] },
  throwShare: { value: 0.35, range: [0.05, 0.9] },
  stopSeconds: { value: 0.2, range: [0.02, 1] },
});

// The design's numbers: the defaults with those given (each kept in its range); unknown ones are
// left out.
function designOf(given = {}) {
  const settings = {};
  for (const [name, { value, range }] of Object.entries(DESIGN)) {
    const wanted = Number(given[name]);
    const [low, high] = range;
    settings[name] = Number.isFinite(wanted) ? Math.min(high, Math.max(low, wanted)) : value;
  }
  return settings;
}

function add(a, b) {
  return [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
}

function sub(a, b) {
  return [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
}

function scale(a, s) {
  return [a[0] * s, a[1] * s, a[2] * s];
}

function dot(a, b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

function size(a) {
  return Math.hypot(a[0], a[1], a[2]);
}

function unit(a, otherwise) {
  const length = size(a);
  return length > 1e-9 ? scale(a, 1 / length) : otherwise;
}

function round(value, places = 2) {
  const factor = 10 ** places;
  return Math.round(value * factor) / factor || 0;
}

// The throw in the hand's path over one round: { start, end, peak, speed, way } (seconds from the
// round's start, the hand's top speed in m/s, the throw's way in the world). `wanted.at` (seconds)
// says when the throw is instead of finding it.
function throwOf(path, seconds, loop, wanted, settings = designOf()) {
  const count = Math.round(seconds * STEP_RATE);
  const at = (index) => path.at((loop ? (index + count) % count : index) / STEP_RATE).tip;
  const speeds = [];
  for (let index = 0; index < count; index += 1) {
    speeds.push(size(sub(at(index + 1), at(index - 1))) * (STEP_RATE / 2));
  }
  // Smoothed over a fortieth of a second: the recording's frames are uneven.
  const reach = Math.round(STEP_RATE / 80);
  const smooth = speeds.map((unused, index) => {
    let total = 0;
    for (let near = -reach; near <= reach; near += 1) {
      total += speeds[(index + near + count) % count];
    }
    return total / (reach * 2 + 1);
  });
  // The throw is the fast move that stops hard: each of the hand's fastest moments scored by its
  // speed and by how far it drops in stopSeconds after (a wind-up flows on into the throw).
  const after = Math.round(settings.stopSeconds * STEP_RATE);
  const fastest = Math.max(...smooth);
  let peak = smooth.indexOf(fastest);
  let best = -1;
  for (let index = 0; index < count; index += 1) {
    const here = smooth[index];
    const before = smooth[(index - 1 + count) % count];
    const isPeak = here >= before && here > smooth[(index + 1) % count];
    // Only a move at least half the hand's top speed is a throw.
    if (!isPeak || here < fastest * 0.5) {
      continue;
    }
    let least = here;
    for (let ahead = 1; ahead <= after; ahead += 1) {
      least = Math.min(least, smooth[(index + ahead) % count]);
    }
    const score = here * (1 - least / here);
    if (score > best) {
      best = score;
      peak = index;
    }
  }
  if (Number.isFinite(wanted?.at)) {
    peak = Math.round((((wanted.at % seconds) + seconds) % seconds) * STEP_RATE) % count;
  }
  const limit = smooth[peak] * settings.throwShare;
  let start = peak;
  while (start - peak > -count / 2 && smooth[(start - 1 + count) % count] > limit) {
    start -= 1;
  }
  let end = peak;
  while (end - peak < count / 2 && smooth[(end + 1) % count] > limit) {
    end += 1;
  }
  const way = unit(sub(at(end), at(start)), [0, 1, 0]);
  return {
    start: start / STEP_RATE,
    end: end / STEP_RATE,
    peak: peak / STEP_RATE,
    speed: round(smooth[peak], 1),
    way,
  };
}

// A unit way turned from `a` toward `b` by `share` (0 to 1) at an even pace round the turn; when
// they are nearly opposite the turn goes over `over` (the loop's side).
function turned(a, b, over, share) {
  if (dot(a, b) < -0.95) {
    return share < 0.5 ? turned(a, over, over, share * 2) : turned(over, b, over, share * 2 - 1);
  }
  const angle = Math.acos(Math.max(-1, Math.min(1, dot(a, b))));
  if (angle < 1e-4) {
    return b;
  }
  const sin = Math.sin(angle);
  const first = Math.sin((1 - share) * angle) / sin;
  const second = Math.sin(share * angle) / sin;
  return unit(add(scale(a, first), scale(b, second)), b);
}

// The lash at `since` seconds into its roll: a turn runs along it from the handle to the tip, each
// piece swinging from the way it had as the throw began (`start`) to the throw's `way` over
// turnSeconds, the turn reaching the pieces further out sooner after each other (rollPower: it
// speeds up toward the tip); turned pieces are the straight part along the throw, those turning
// the loop, the rest as they were. Only the pieces in the turn move against each other, so the
// lash rolls and never snaps into its next shape.
function rolledLash(start, tip, way, over, since, rollTime, piece, ground, settings) {
  const travel = Math.max(0.01, rollTime - settings.turnSeconds);
  const out = [tip];
  for (let index = 1; index <= PIECES; index += 1) {
    const reached = travel * ((index - 1) / PIECES) ** (1 / settings.rollPower);
    const share = Math.min(1, Math.max(0, (since - reached) / settings.turnSeconds));
    const eased = share * share * (3 - 2 * share);
    const pieceWay = turned(start[index - 1], way, over, eased);
    const placed = add(out[index - 1], scale(pieceWay, piece));
    if (ground !== null && placed[2] < ground) {
      placed[2] = ground;
    }
    out.push(placed);
  }
  return out;
}

// The lash one step on, drawn by the hand: each point sinks (by the square of how far along the
// lash it is: the tip drops first and the lash bends as it falls; sinking evenly along it only
// turned it down like a rod), then is put a piece from the one before it, toward there, its first
// piece bent out of the handle's line a little (the lash goes on from the handle, it does not
// leave it at a corner); on the floor at `ground`. It keeps no speed of its own, so it cannot
// whirl or bounce.
function drawn(points, tip, along, piece, ground, settings) {
  const out = [tip];
  const fall = settings.fallSpeed / STEP_RATE;
  let before = along;
  for (let index = 1; index <= PIECES; index += 1) {
    const share = index / PIECES;
    const sink = fall * (settings.rootFallShare + (1 - settings.rootFallShare) * share * share);
    let point = add(points[index], [0, 0, -sink]);
    if (ground !== null && point[2] < ground) {
      point = [point[0], point[1], ground];
    }
    const free = unit(sub(point, out[index - 1]), [0, 0, -1]);
    // Only a bend is smoothed: a lash going back past the handle's side is left to go its way.
    const keep = index === 1 ? settings.rootKeep * Math.max(0, dot(free, before)) : 0;
    const way = unit(add(scale(free, 1 - keep), scale(before, keep)), free);
    before = way;
    const placed = add(out[index - 1], scale(way, piece));
    if (ground !== null && placed[2] < ground) {
      placed[2] = ground;
    }
    out.push(placed);
  }
  return out;
}

// The lash's pieces in a frame ({ along, right, up }: the body's or the handle's) as the track
// keeps them: TRACK_SEGMENTS ways, each [yaw, pitch] whole degrees.
function frameWays(points, handle) {
  const ratio = PIECES / TRACK_SEGMENTS;
  const ways = [];
  for (let segment = 0; segment < TRACK_SEGMENTS; segment += 1) {
    const chord = sub(points[(segment + 1) * ratio], points[segment * ratio]);
    const local = [dot(chord, handle.along), dot(chord, handle.right), dot(chord, handle.up)];
    ways.push(unit(local, [1, 0, 0]));
  }
  return ways;
}

// The designed lash track from a recording of the handle: { track, throw, design } or { error }.
// `options`: length (metres), seconds, loop, ground, rate (frames a second), throw ({ at }: the
// throw's fastest moment in seconds, when it is not to be found), design (DESIGN's numbers).
// When the recording has the body's frame (`body`: its forward and up) the track is kept in it
// (space "body": the lash keeps its way when the wrist turns), else in the handle's.
function designLash(recording, options) {
  const { length, seconds, loop } = options;
  const settings = designOf(options.design);
  const bodyFrame = Array.isArray(recording?.body) ? frameOf(recording.body) : null;
  const rate = options.rate ?? TRACK_RATE;
  const ground = Number.isFinite(options.ground) ? options.ground : null;
  const path = handlePath(recording, seconds, loop);
  if (!path) {
    return { error: 'the recording has no handle frames: record it with the whip in the hand' };
  }
  const thrown = throwOf(path, seconds, loop, options.throw, settings);
  const piece = length / PIECES;
  const rollTime = Math.max(0.05, thrown.end - thrown.start + settings.rollAfter);
  // Hanging from the hand at the start.
  const first = path.at(0).tip;
  let points = [];
  for (let index = 0; index <= PIECES; index += 1) {
    points.push(add(first, [0, 0, -piece * index]));
  }
  const rounds = loop ? 2 : 1;
  const steps = Math.round(rounds * seconds * STEP_RATE);
  const frames = loop ? Math.max(2, Math.round(seconds * rate)) : Math.floor(seconds * rate) + 1;
  const kept = { last: [], before: [] };
  // Each piece's way as the throw began, and the loop's side (rolledLash).
  let start = null;
  let over = null;
  let next = 0;
  for (let step = 0; step <= steps; step += 1) {
    const time = step / STEP_RATE;
    const inRound = loop ? time % seconds : time;
    const handle = path.at(time);
    const sinceThrow = (inRound - thrown.start + seconds) % seconds;
    const throwing = sinceThrow <= rollTime && (loop || inRound >= thrown.start);
    if (throwing) {
      // As the throw begins: each piece's way, and the side the loop turns over (up, across
      // the throw: a whip's overhead loop).
      if (start === null) {
        start = [];
        for (let index = 1; index <= PIECES; index += 1) {
          start.push(unit(sub(points[index], points[index - 1]), [0, 0, -1]));
        }
        const up = sub([0, 0, 1], scale(thrown.way, thrown.way[2]));
        over = unit(up, [1, 0, 0]);
      }
      const { way } = thrown;
      const since = sinceThrow;
      points = rolledLash(start, handle.tip, way, over, since, rollTime, piece, ground, settings);
    } else {
      start = null;
      points = drawn(points, handle.tip, handle.along, piece, ground, settings);
    }
    // The kept frames: the last round's, and the round before's for the seam.
    const roundStart = loop ? (rounds - 1) * seconds : 0;
    while (next < frames && roundStart + next / rate <= time + 0.5 / STEP_RATE) {
      kept.last[next] = frameWays(points, bodyFrame ?? handle);
      next += 1;
    }
    if (loop && time >= roundStart - SEAM_SECONDS && time < roundStart) {
      const frame = Math.round((time - (roundStart - seconds)) * rate);
      if (frame >= 0 && frame < frames && !kept.before[frame]) {
        kept.before[frame] = frameWays(points, bodyFrame ?? handle);
      }
    }
  }
  const out = [];
  for (let frame = 0; frame < frames; frame += 1) {
    const time = frame / rate;
    let ways = kept.last[frame] ?? kept.last.at(-1);
    if (loop && time > seconds - SEAM_SECONDS && kept.before[frame]) {
      const share = (time - (seconds - SEAM_SECONDS)) / SEAM_SECONDS;
      const eased = share * share * (3 - 2 * share);
      ways = ways.map((way, index) =>
        unit(add(scale(way, 1 - eased), scale(kept.before[frame][index], eased)), way),
      );
    }
    out.push(ways.flatMap(wayDegrees));
  }
  return {
    track: {
      rate,
      length: round(length, 3),
      frames: out,
      ...(bodyFrame ? { space: 'body' } : {}),
    },
    throw: {
      start: round(thrown.start),
      end: round(thrown.end),
      peak: round(thrown.peak),
      handSpeed: thrown.speed,
      way: thrown.way.map((part) => round(part)),
    },
    design: settings,
  };
}

module.exports = { designLash, throwOf, designOf, DESIGN };
