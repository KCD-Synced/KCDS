'use strict';

// A whip's lash made from the hand's motion (Lash from motion): the studio records
// where the whip's handle is along the animation (studio/studio_rope.hpp, the lash's first point
// and the handle's frame, 30 a second), and this simulates the lash on it here, away from the
// game: SIM_SEGMENTS + 1 points, the first on the handle's tip, in many small steps (SIM_RATE a
// second); each step keeps every piece its length and pulls each point toward a straight line
// with the points two pieces away (stiff at the handle, loose at the tip), and the points get
// lighter toward the tip, so a wave from the hand speeds up along it (a whip's crack); gravity,
// the air and the floor act on it. A looping animation goes round SETTLE_ROUNDS times first, so
// the lash moves as it would after many rounds, and the end of the last round is blended into
// its start (no seam). What it makes is the animation's lash track (animation.cjs lashTrackOf):
// the shape at its own times, TRACK_RATE a second, played the same on every client with no
// physics.
//
// lashQuality measures a lash however it is shaped (a track or keys): its length, how sharply
// it bends, single frames that jump, and the loop's seam, so the assistant sees what is wrong.

const { frameOf } = require('./lash_keys.cjs');
const { Animation } = require('./animation.cjs');

const SIM_SEGMENTS = 24;
const TRACK_SEGMENTS = 12;
const SIM_RATE = 2400;
// Frames a second: at 30 the crack's fastest pieces turned so far from one frame to the next that
// blending them folded the lash (166° on a test swing); at 40 it bends 78° at most.
const TRACK_RATE = 40;
const SETTLE_ROUNDS = 4;
// The end of a loop's last round blended into its start over this long (seconds).
const SEAM_SECONDS = 0.3;
// A lash that is not looping holds its first pose this long first, settling.
const SETTLE_SECONDS = 1;
// Passes over the pieces each step.
const PASSES = 2;

// How it moves, by default: gravity (m/s²), slowing (each second's share), the air's drag on fast
// points (per metre), the leather's own friction (each step's share of a point's speed pulled
// toward its neighbours'), the pull toward straight each step at the handle and at the tip (0 to
// 1), how much lighter the tip is than the handle's end (0 the same, toward 1 far lighter: the
// crack), the floor's friction (each second's share) and the frames a second it keeps.
const DEFAULTS = Object.freeze({
  gravity: 9.81,
  damping: 0.3,
  air: 0.03,
  airTip: 1,
  inner: 0.02,
  stiffRoot: 0.9,
  stiffTip: 0.08,
  taper: 0.8,
  friction: 4,
  rate: TRACK_RATE,
});

// What lashQuality calls wrong: a bend sharper than this between two pieces (degrees), a length
// off by this share, a frame this many times further out from between the frames around it than
// they are apart (and more than JUMP_METRES out), a loop's last frame this many times further from
// the first than the lash usually moves.
const FOLD_DEGREES = 110;
const LENGTH_SHARE = 0.05;
const JUMP_TIMES = 3;
const JUMP_METRES = 0.15;
// Keys this far apart (seconds) are too few for a lash: a crack is faster.
const SPARSE_SECONDS = 0.05;
// A bend of the last two pieces this near the tip's fastest moment (seconds) is the crack's.
const CRACK_SECONDS = 0.1;
// A frame in which at least SNAP_SHARE of the lash's pieces each turn more than SNAP_DEGREES is a
// snap: the whole lash changed at once (a roll turns only the pieces in its loop, however fast;
// an average over the pieces called every real crack a snap at 30 frames a second).
const SNAP_DEGREES = 25;
const SNAP_SHARE = 0.6;

// The share of pieces that turn more than SNAP_DEGREES between two lists of ways, and their
// average turn (degrees).
function turnOf(a, b) {
  let turning = 0;
  let total = 0;
  b.forEach((way, index) => {
    const bend = bendOf(a[index] ?? way, way);
    total += bend;
    if (bend > SNAP_DEGREES) {
      turning += 1;
    }
  });
  return { share: turning / Math.max(1, b.length), average: total / Math.max(1, b.length) };
}
// Whether it looks like a whip's strike: once a round the lash rolls out nearly straight (its tip
// this share of its length from the handle), and it is balled up near the hand (its tip within
// COILED_REACH of its length) for at most COILED_SHARE of the round.
const STRIKE_REACH = 0.95;
const COILED_REACH = 0.5;
const COILED_SHARE = 0.15;

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

function degrees(radiansValue) {
  return (radiansValue * 180) / Math.PI;
}

// Catmull-Rom between b and c at `share` (a and d around them).
function curve(a, b, c, d, share) {
  const s2 = share * share;
  const s3 = s2 * share;
  return [0, 1, 2].map(
    (axis) =>
      0.5 *
      (2 * b[axis] +
        (c[axis] - a[axis]) * share +
        (2 * a[axis] - 5 * b[axis] + 4 * c[axis] - d[axis]) * s2 +
        (3 * b[axis] - a[axis] - 3 * c[axis] + d[axis]) * s3),
  );
}

// The highest motion (Hz) a looping hand's path keeps: a throw of a sixth of a second still
// whole, the game's uneven frames (a hand that seems to jump) gone.
const PATH_HERTZ = 8;

// Solves a x = b (Gauss-Jordan with the largest pivot; a is n × n), changing a and b.
function solved(a, b) {
  const n = b.length;
  for (let column = 0; column < n; column += 1) {
    let pivot = column;
    for (let row = column + 1; row < n; row += 1) {
      if (Math.abs(a[row][column]) > Math.abs(a[pivot][column])) {
        pivot = row;
      }
    }
    [a[column], a[pivot]] = [a[pivot], a[column]];
    [b[column], b[pivot]] = [b[pivot], b[column]];
    const lead = a[column][column] || 1e-12;
    for (let row = 0; row < n; row += 1) {
      if (row === column) {
        continue;
      }
      const factor = a[row][column] / lead;
      for (let k = column; k < n; k += 1) {
        a[row][k] -= factor * a[column][k];
      }
      b[row] -= factor * b[column];
    }
  }
  return b.map((value, row) => value / (a[row][row] || 1e-12));
}

// A looping path from samples ({ time, values: [numbers] } over one or more rounds of `period`):
// a smooth curve that goes round (a sum of sines and cosines up to `harmonics`), fitted to all
// of them, so every round's samples count and a hitch in one is outvoted.
function periodicFit(samples, period, harmonics) {
  const terms = harmonics * 2 + 1;
  const basis = (time) => {
    const row = [1];
    for (let h = 1; h <= harmonics; h += 1) {
      const angle = (2 * Math.PI * h * time) / period;
      row.push(Math.cos(angle), Math.sin(angle));
    }
    return row;
  };
  const width = samples[0].values.length;
  const normal = Array.from({ length: terms }, () => new Array(terms).fill(0));
  const right = Array.from({ length: width }, () => new Array(terms).fill(0));
  for (const sample of samples) {
    const row = basis(sample.time);
    for (let i = 0; i < terms; i += 1) {
      for (let j = 0; j < terms; j += 1) {
        normal[i][j] += row[i] * row[j];
      }
      for (let k = 0; k < width; k += 1) {
        right[k][i] += row[i] * sample.values[k];
      }
    }
  }
  // A little pull toward no motion keeps it steady where samples are few.
  for (let i = 1; i < terms; i += 1) {
    normal[i][i] += 1e-6 * samples.length;
  }
  const weights = right.map((column) =>
    solved(
      normal.map((row) => [...row]),
      [...column],
    ),
  );
  return (time) => {
    const row = basis(time);
    return weights.map((w) => w.reduce((total, value, index) => total + value * row[index], 0));
  };
}

// Where the handle is at any time from a recording ({ rate, frames, handles }: each frame's first
// point the handle's tip, each handle its frame): { tip, along, right, up }, smoothly between the
// recorded frames, round the loop when it loops; null when the recording has no handle. A loop's
// recording may hold more than one round: they are all fitted as one smooth loop.
function handlePath(recording, seconds, loop) {
  const { rate, frames, handles } = recording ?? {};
  if (!(rate > 0) || !Array.isArray(frames) || !Array.isArray(handles)) {
    return null;
  }
  if (handles.length !== frames.length || frames.length < 4) {
    return null;
  }
  const wanted = Math.round(seconds * rate);
  const count = loop ? Math.max(4, Math.min(frames.length, wanted)) : frames.length;
  const period = count / rate;
  if (loop && frames.length >= count) {
    return fittedPath(recording, period);
  }
  // The game draws fewer frames than the recording keeps when it runs slower than `rate`: then a
  // pose is kept twice or more, the hand standing still and then jumping. Each pose is taken once,
  // at the time of the last frame that kept it (when the game drew it), so the path is smooth.
  const samples = [];
  for (let index = 0; index < count; index += 1) {
    const frame = frameOf(handles[index]);
    if (!frame || frames[index].length < 3) {
      return null;
    }
    const tip = frames[index].slice(0, 3);
    const sample = { time: index / rate, tip, along: frame.along, up: frame.up };
    const before = samples.at(-1);
    const sameTip = before && size(sub(before.tip, tip)) < 1e-6;
    if (sameTip && dot(before.along, frame.along) > 1 - 1e-9) {
      samples[samples.length - 1] = sample;
    } else {
      samples.push(sample);
    }
  }
  if (samples.length < 4) {
    return null;
  }
  // The sample at `at` (an index, round the loop when it loops) and its time, a period later for
  // each round past the last sample.
  const total = samples.length;
  const sampleAt = (at) => {
    if (!loop) {
      return samples[Math.min(total - 1, Math.max(0, at))];
    }
    const rounds = Math.floor(at / total);
    const sample = samples[at - rounds * total];
    return { ...sample, time: sample.time + rounds * period };
  };
  return {
    rounds: period,
    at(time) {
      let when = loop ? ((time % period) + period) % period : time;
      // The last sample at or before `when` (a loop's first ones are after its last, a round on).
      let first = samples.findLastIndex((sample) => sample.time <= when);
      if (first === -1) {
        if (!loop) {
          first = 0;
          when = samples[0].time;
        } else {
          first = total - 1;
          when += period;
        }
      }
      const [a, b, c, d] = [first - 1, first, first + 1, first + 2].map(sampleAt);
      const span = c.time - b.time;
      const share = span > 1e-6 ? Math.min(1, Math.max(0, (when - b.time) / span)) : 0;
      const tip = curve(a.tip, b.tip, c.tip, d.tip, share);
      const blended = frameOf([
        ...add(scale(b.along, 1 - share), scale(c.along, share)),
        ...add(scale(b.up, 1 - share), scale(c.up, share)),
      ]);
      const frame = blended ?? frameOf([...b.along, ...b.up]);
      return { tip, along: frame.along, right: frame.right, up: frame.up };
    },
  };
}

// A looping hand's path fitted over every recorded round (periodicFit): each pose once, at the
// time of the last frame that kept it (the game drew fewer frames than the recording keeps).
function fittedPath(recording, period) {
  const { rate, frames, handles } = recording;
  const samples = [];
  let before = null;
  for (let index = 0; index < frames.length; index += 1) {
    const frame = frameOf(handles[index]);
    if (!frame || frames[index].length < 3) {
      return null;
    }
    const values = [...frames[index].slice(0, 3), ...frame.along, ...frame.up];
    const same = before && values.every((value, k) => Math.abs(value - before.values[k]) < 1e-6);
    const sample = { time: index / rate, values };
    if (same) {
      samples[samples.length - 1] = sample;
    } else {
      samples.push(sample);
    }
    before = sample;
  }
  if (samples.length < 8) {
    return null;
  }
  const harmonics = Math.max(2, Math.min(24, Math.floor(period * PATH_HERTZ)));
  const fit = periodicFit(samples, period, harmonics);
  return {
    rounds: frames.length / rate,
    at(time) {
      const values = fit(time);
      const tip = values.slice(0, 3);
      const frame = frameOf(values.slice(3, 9)) ?? frameOf([...values.slice(3, 6), 0, 0, 1]);
      return { tip, along: frame.along, right: frame.right, up: frame.up };
    },
  };
}

// The lash's pieces as the track keeps them: TRACK_SEGMENTS ways in the handle's frame, each
// [yaw, pitch] whole degrees (animation.cjs lashWay).
function trackWays(points, handle) {
  const ratio = SIM_SEGMENTS / TRACK_SEGMENTS;
  const ways = [];
  for (let segment = 0; segment < TRACK_SEGMENTS; segment += 1) {
    const chord = sub(points[(segment + 1) * ratio], points[segment * ratio]);
    const local = [dot(chord, handle.along), dot(chord, handle.right), dot(chord, handle.up)];
    ways.push(unit(local, [1, 0, 0]));
  }
  return ways;
}

function waysBetween(a, b, share) {
  return a.map((way, index) => unit(add(scale(way, 1 - share), scale(b[index], share)), way));
}

function wayDegrees(way) {
  const pitch = degrees(Math.asin(Math.max(-1, Math.min(1, way[2]))));
  const yaw = Math.abs(way[2]) > 0.99999 ? 0 : degrees(Math.atan2(way[1], way[0]));
  return [Math.round(yaw) || 0, Math.round(pitch) || 0];
}

// The lash simulated along the handle's path: { track, tipSpeed, tipSpeedAt, onFloor }.
// `options`: length (metres), seconds (the animation's), loop, ground (the floor's height, or
// none), and DEFAULTS' gravity, damping, air, stiffRoot, stiffTip, friction, rate.
function simulateLash(path, options) {
  const settings = { ...DEFAULTS, ...options };
  const { length, seconds, loop } = settings;
  const ground = Number.isFinite(settings.ground) ? settings.ground : null;
  const piece = length / SIM_SEGMENTS;
  const step = 1 / SIM_RATE;
  const keep = 1 - settings.damping * step;
  const rub = Math.max(0, 1 - settings.friction * step);
  const start = loop ? 0 : -SETTLE_SECONDS;
  const end = loop ? (SETTLE_ROUNDS + 1) * seconds : seconds;
  // When to keep the lash's shape: the last round's frames, and the round before's for the seam.
  const frames = loop
    ? Math.max(2, Math.round(seconds * settings.rate))
    : Math.floor(seconds * settings.rate) + 1;
  const last = loop ? SETTLE_ROUNDS * seconds : 0;
  const captures = [];
  for (let frame = 0; frame < frames; frame += 1) {
    const time = frame / settings.rate;
    captures.push({ at: last + time, frame, round: 'last' });
    if (loop && time > seconds - SEAM_SECONDS) {
      captures.push({ at: last - seconds + time, frame, round: 'before' });
    }
  }
  captures.sort((a, b) => a.at - b.at);
  const kept = { last: [], before: [] };

  const first = path.at(Math.max(0, start));
  // The points, where they were a step before (their speed), and how easily each moves (the
  // handle's tip not at all; lighter toward the tip).
  const points = [];
  const before = [];
  const loose = [0];
  for (let index = 0; index <= SIM_SEGMENTS; index += 1) {
    points.push(add(first.tip, scale(first.along, piece * index)));
    before.push(points[index]);
    if (index > 0) {
      loose.push(1 / (1 - settings.taper * (index / SIM_SEGMENTS)));
    }
  }
  // How stiff each point's bend is (toward straight with the points around it).
  const stiffness = [];
  for (let index = 0; index <= SIM_SEGMENTS; index += 1) {
    const share = index / SIM_SEGMENTS;
    stiffness.push(settings.stiffRoot + (settings.stiffTip - settings.stiffRoot) * share);
  }
  // Two points kept `apart` metres apart, moved as their looseness says, by `share` of the way.
  const keepApart = (a, b, apart, share, wa, wb) => {
    const total = wa + wb;
    if (total <= 0) {
      return;
    }
    const between = sub(b, a);
    const distance = size(between);
    if (distance < 1e-9) {
      return;
    }
    const off = ((distance - apart) / distance) * share;
    const move = scale(between, off / total);
    for (let axis = 0; axis < 3; axis += 1) {
      a[axis] += move[axis] * wa;
      b[axis] -= move[axis] * wb;
    }
  };
  let next = 0;
  let tipSpeed = 0;
  let tipSpeedAt = 0;
  let onFloor = 0;
  const steps = Math.ceil((end - start) * SIM_RATE);
  for (let count = 0; count <= steps; count += 1) {
    const time = start + count * step;
    const handle = path.at(Math.max(0, time));
    // Each point on with its speed, gravity and the air: the thinner the lash, the more the air
    // slows it for its weight (airTip times more at the tip than at the handle), so the tip's
    // speed goes in the crack instead of whirling the lash round the hand after it.
    for (let index = 1; index <= SIM_SEGMENTS; index += 1) {
      const speed = scale(sub(points[index], before[index]), 1 / step);
      const thin = 1 + (settings.airTip - 1) * (index / SIM_SEGMENTS);
      const drag = scale(speed, -settings.air * thin * size(speed));
      const pull = add([0, 0, -settings.gravity], drag);
      const moved = add(points[index], add(scale(speed, keep * step), scale(pull, step * step)));
      before[index] = points[index];
      points[index] = moved;
    }
    points[0] = handle.tip;
    // Behind the handle's tip, a point the lash's first bend keeps straight with: the handle.
    const behind = sub(handle.tip, scale(handle.along, piece));
    for (let pass = 0; pass < PASSES; pass += 1) {
      keepApart(behind.slice(), points[1], piece * 2, stiffness[0], 0, loose[1]);
      for (let index = 1; index < SIM_SEGMENTS; index += 1) {
        const share = stiffness[index];
        const [before, after] = [index - 1, index + 1];
        keepApart(points[before], points[after], piece * 2, share, loose[before], loose[after]);
      }
      for (let index = 1; index <= SIM_SEGMENTS; index += 1) {
        keepApart(points[index - 1], points[index], piece, 1, loose[index - 1], loose[index]);
      }
    }
    // The leather's own friction: each point's speed pulled toward the speed of the points around
    // it, so a wave runs down the lash once instead of ringing up and down it.
    if (settings.inner > 0) {
      const speeds = points.map((point, index) => sub(point, before[index]));
      for (let index = 1; index <= SIM_SEGMENTS; index += 1) {
        const last = index === SIM_SEGMENTS;
        const around = last
          ? speeds[index - 1]
          : scale(add(speeds[index - 1], speeds[index + 1]), 0.5);
        const kept = add(speeds[index], scale(sub(around, speeds[index]), settings.inner));
        before[index] = sub(points[index], kept);
      }
    }
    for (let index = 1; index <= SIM_SEGMENTS; index += 1) {
      if (ground !== null && points[index][2] < ground) {
        points[index][2] = ground;
        // On the floor it slides with friction and does not go on down.
        const slid = sub(points[index], before[index]);
        before[index] = sub(points[index], [slid[0] * rub, slid[1] * rub, 0]);
        if (time >= last) {
          onFloor += 1;
        }
      }
    }
    if (time >= last) {
      const speed = size(sub(points[SIM_SEGMENTS], before[SIM_SEGMENTS])) / step;
      if (speed > tipSpeed) {
        tipSpeed = speed;
        tipSpeedAt = time - last;
      }
    }
    while (next < captures.length && captures[next].at <= time + step / 2) {
      kept[captures[next].round][captures[next].frame] = trackWays(points, handle);
      next += 1;
    }
  }
  const out = [];
  for (let frame = 0; frame < frames; frame += 1) {
    const time = frame / settings.rate;
    let ways = kept.last[frame];
    if (loop && time > seconds - SEAM_SECONDS && kept.before[frame]) {
      const share = (time - (seconds - SEAM_SECONDS)) / SEAM_SECONDS;
      const eased = share * share * (3 - 2 * share);
      ways = waysBetween(ways, kept.before[frame], eased);
    }
    out.push(ways.flatMap(wayDegrees));
  }
  return {
    track: { rate: settings.rate, length: round(length, 3), frames: out },
    tipSpeed: round(tipSpeed, 1),
    tipSpeedAt: round(tipSpeedAt, 2),
    // How much of the last round the lash lay on the floor (pieces × steps, as a share).
    onFloor: round(onFloor / Math.max(1, SIM_SEGMENTS * seconds * SIM_RATE), 3),
  };
}

// The lash track from a recording of the handle along the animation: { track, ... } or
// { error }. `options` as simulateLash's.
function lashMotion(recording, options) {
  const path = handlePath(recording, options.seconds, options.loop);
  if (!path) {
    return { error: 'the recording has no handle frames: record it with the whip in the hand' };
  }
  if (options.loop && path.rounds + 1e-6 < options.seconds - 1 / recording.rate) {
    const recorded = round(path.rounds);
    return { error: `the recording is ${recorded} s, the animation ${options.seconds} s` };
  }
  return simulateLash(path, options);
}

// The settings tried when the stiffness, the air and the leather's friction are not given
// (bestLashMotion): a crack is chaotic, so the same settings fold or ball up one hand's motion
// and not another's. On a 1.8 s overhead crack these passed every check more often than not.
const TRIES = Object.freeze(
  [0.015, 0.02].flatMap((inner) =>
    [0.05, 0.08, 0.12].flatMap((stiffTip) => [0.03, 0.06].map((air) => ({ inner, stiffTip, air }))),
  ),
);

// How bad a lash is, to choose between tries: each problem far worse than anything else, then
// how long it is balled up, how far short of straight it rolls out, its sharpest fold and jumps.
function badnessOf(quality) {
  const short = Math.max(0, 0.98 - quality.reach.most);
  return (
    quality.problems.length * 1000 +
    quality.reach.coiled * 300 +
    short * 500 +
    quality.fold.most +
    quality.jumps.length * 50
  );
}

// The lash track from a recording, the best of several tries: as lashMotion, with `chosen` (the
// settings it took) and `tried` ({ settings, fold, problems } of each). When `options` give the
// stiffness or the air, only those are tried.
function bestLashMotion(recording, options) {
  const named = ['stiffTip', 'stiffRoot', 'air', 'inner'];
  const given = named.some((name) => options[name] !== undefined);
  const tries = given ? [{}] : TRIES;
  let best = null;
  const tried = [];
  for (const settings of tries) {
    const made = lashMotion(recording, { ...options, ...settings });
    if (made.error) {
      return made;
    }
    const animation = {
      keys: [
        { time: 0, pose: {} },
        { time: options.seconds, pose: {} },
      ],
      loop: options.loop,
      lashTrack: made.track,
    };
    const quality = lashQuality(animation, options.length);
    const badness = badnessOf(quality);
    tried.push({
      settings,
      reach: quality.reach.most,
      coiled: quality.reach.coiled,
      fold: quality.fold.most,
      problems: quality.problems.length,
      badness: round(badness, 1),
    });
    if (!best || badness < best.badness) {
      best = { made, quality, badness, settings };
    }
  }
  return { ...best.made, quality: best.quality, chosen: best.settings, tried };
}

// The angle between two pieces (degrees).
function bendOf(a, b) {
  const la = size(a);
  const lb = size(b);
  if (la < 1e-6 || lb < 1e-6) {
    return 0;
  }
  return degrees(Math.acos(Math.max(-1, Math.min(1, dot(a, b) / (la * lb)))));
}

// How good the lash of an animation (with its joints by name: { keys, loop, lashTrack? }) is,
// against a whip of `length` metres: { shapedBy, seconds, length: [shortest, longest], fold:
// { most, at, piece } (the sharpest bend but the crack's), crack: { at, fold } (when the tip is
// fastest and how sharply its last pieces turn over then), jumps: [{ at, metres }], seam,
// fastest, problems: [text] }; null when nothing shapes the lash.
function lashQuality(data, length) {
  const animation = new Animation();
  const keys = data.keys.map((key) => ({ time: key.time, pose: {}, lash: key.lash }));
  if (!animation.restore({ keys, loop: data.loop, lashTrack: data.lashTrack })) {
    return null;
  }
  if (!animation.hasLash()) {
    return null;
  }
  const seconds = animation.length();
  const track = animation.lashTrack;
  const problems = [];
  const shapedBy = track ? `a lash track, ${track.rate} frames a second` : 'its keys';
  const lashTimes = animation.keys.filter((key) => key.lash).map((key) => key.time);
  if (!track) {
    const gaps = lashTimes.slice(1).map((time, index) => time - lashTimes[index]);
    const widest = gaps.length > 0 ? Math.max(...gaps) : seconds;
    if (widest > SPARSE_SECONDS) {
      problems.push(
        `the lash is shaped only at ${lashTimes.length} keys (up to ${round(widest)} s apart): ` +
          `a whip moves faster; Lash from motion keys it at ${TRACK_RATE} frames a second`,
      );
    }
  }
  // Sampled at four times the track's frames, the frames themselves among them.
  const rate = (track?.rate ?? TRACK_RATE) * 4;
  const samples = Math.max(2, Math.round(seconds * rate));
  const shapes = [];
  for (let sample = 0; sample <= samples; sample += 1) {
    shapes.push(animation.lashAt((sample / samples) * seconds));
  }
  let shortest = Infinity;
  let longest = 0;
  // Each sample's sharpest bend between two pieces.
  const bends = [];
  shapes.forEach((shape, sample) => {
    let along = 0;
    let before = [0, 0, 0];
    let way = [1, 0, 0];
    const sharpest = { most: 0, at: round((sample / samples) * seconds), piece: 0 };
    shape.forEach((point, index) => {
      const piece = sub(point, before);
      along += size(piece);
      const bend = index === 0 ? 0 : bendOf(way, piece);
      if (bend > sharpest.most) {
        sharpest.most = round(bend, 0);
        sharpest.piece = index;
      }
      way = piece;
      before = point;
    });
    bends.push(sharpest);
    shortest = Math.min(shortest, along);
    longest = Math.max(longest, along);
  });
  const tooShort = shortest < length * (1 - LENGTH_SHARE);
  const tooLong = longest > length * (1 + LENGTH_SHARE);
  if (length > 0 && (tooShort || tooLong)) {
    const along = `${round(shortest)} to ${round(longest)} m`;
    problems.push(
      `the lash is ${along} long as it plays, the whip ${length} m (it shrinks or stretches ` +
        'between its shapes)',
    );
  }
  // How far the lash moves from one sample to the next (its most moving point), and its tip.
  const steps = [];
  const tipSteps = [];
  for (let sample = 1; sample < shapes.length; sample += 1) {
    const a = shapes[sample - 1];
    const b = shapes[sample];
    steps.push(Math.max(...b.map((point, index) => size(sub(point, a[index] ?? point)))));
    tipSteps.push(size(sub(b.at(-1), a.at(-1) ?? b.at(-1))));
  }
  // The crack: when the tip is fastest. There the tip turns over in a loop a few centimetres
  // wide, a sharp bend of its last pieces that a whip makes: told as the crack, not a kink.
  const crackSample = tipSteps.indexOf(Math.max(...tipSteps)) + 1;
  const crackAt = (crackSample / samples) * seconds;
  const lastPieces = (shapes[0]?.length ?? 0) - 2;
  const nearCrack = (bend) => {
    const apart = Math.abs(bend.at - crackAt);
    const around = animation.loop ? Math.min(apart, seconds - apart) : apart;
    return bend.piece >= lastPieces && around <= CRACK_SECONDS;
  };
  const fold = { most: 0, at: 0, piece: 0 };
  const crack = { at: round(crackAt), fold: 0 };
  for (const bend of bends) {
    if (nearCrack(bend)) {
      crack.fold = Math.max(crack.fold, bend.most);
    } else if (bend.most > fold.most) {
      Object.assign(fold, bend);
    }
  }
  if (fold.most > FOLD_DEGREES) {
    problems.push(
      `the lash folds ${fold.most}° at piece ${fold.piece} (${fold.at} s): a kink, not a whip's ` +
        'curve',
    );
  }
  // Whether it strikes: how straight it rolls out at most, and how long it is balled up.
  const along = longest > 0 ? longest : 1;
  const reaches = shapes.map((shape) => size(shape.at(-1)) / along);
  const reachMost = Math.max(...reaches);
  const reach = {
    most: round(reachMost),
    at: round((reaches.indexOf(reachMost) / samples) * seconds),
    coiled: round(reaches.filter((share) => share < COILED_REACH).length / reaches.length),
  };
  if (reach.most < STRIKE_REACH) {
    problems.push(
      `the lash never rolls out straight (its tip at most ${reach.most} of its length from the ` +
        'hand): no strike; the hand needs a fast throw that stops',
    );
  }
  if (reach.coiled > COILED_SHARE) {
    const share = Math.round(reach.coiled * 100);
    problems.push(
      `the lash is balled up near the hand ${share} % of the round (its tip within half its ` +
        'length): it dangles instead of moving with the strike',
    );
  }
  // The jumps, frame by frame: a frame far out from between the frames around it while those are
  // near each other (the lash flicks out and back in one frame; a fast swing goes through).
  const frameRate = track?.rate ?? TRACK_RATE;
  const frameCount = Math.max(2, Math.round(seconds * frameRate));
  const frameShapes = [];
  for (let frame = 0; frame <= frameCount; frame += 1) {
    frameShapes.push(animation.lashAt((frame / frameCount) * seconds));
  }
  const jumps = [];
  for (let frame = 1; frame < frameShapes.length - 1; frame += 1) {
    const [a, b, c] = [frameShapes[frame - 1], frameShapes[frame], frameShapes[frame + 1]];
    let out = 0;
    let across = 0;
    b.forEach((point, index) => {
      const between = scale(add(a[index] ?? point, c[index] ?? point), 0.5);
      out = Math.max(out, size(sub(point, between)));
      across = Math.max(across, size(sub(c[index] ?? point, a[index] ?? point)));
    });
    if (out > JUMP_METRES && out > across * JUMP_TIMES) {
      jumps.push({ at: round((frame / frameCount) * seconds), metres: round(out) });
    }
  }
  // The snaps: a frame in which the whole lash turns at once (its pieces' turn from the frame
  // before, on average). A roll turns only the pieces passing through its loop; a lash that turns
  // all of itself in a frame teleports into its next shape.
  const pieceWays = (shape) => {
    let before = [0, 0, 0];
    return shape.map((point) => {
      const way = unit(sub(point, before), [1, 0, 0]);
      before = point;
      return way;
    });
  };
  const snaps = [];
  for (let frame = 1; frame < frameShapes.length; frame += 1) {
    const a = pieceWays(frameShapes[frame - 1]);
    const b = pieceWays(frameShapes[frame]);
    const { share, average } = turnOf(a, b);
    if (share >= SNAP_SHARE) {
      snaps.push({ at: round((frame / frameCount) * seconds), degrees: round(average, 0) });
    }
  }
  if (snaps.length > 0) {
    const most = snaps.reduce((a, b) => (b.degrees > a.degrees ? b : a));
    problems.push(
      `the lash snaps into its next shape at ${snaps.length} frames (the most at ${most.at} s: ` +
        `all of it turns ${most.degrees}° in one frame) instead of rolling`,
    );
  }
  if (jumps.length > 0) {
    const first = jumps[0];
    problems.push(
      `the lash jumps at ${jumps.length} times (the first at ${first.at} s, ${first.metres} m at ` +
        'once)',
    );
  }
  // The loop's seam: the step from its end to its start against the steps just before and after
  // it (a fast move there goes on through the seam; a jump does not).
  let seam = 0;
  if (animation.loop && steps.length > 4) {
    seam = round(steps.at(-1));
    const around = Math.max(steps.at(-2), steps[0]);
    if (seam > JUMP_METRES && seam > around * JUMP_TIMES) {
      problems.push(`the loop's end is ${seam} m from its start: the lash jumps there`);
    }
  }
  const fastest = Math.max(...steps) * rate;
  return {
    shapedBy,
    seconds,
    length: [round(shortest), round(longest)],
    fold,
    crack,
    reach,
    jumps: jumps.slice(0, 8),
    snaps: snaps.slice(0, 8),
    seam,
    // The lash's fastest point against the handle (m/s).
    fastest: round(fastest, 1),
    problems,
  };
}

// The lash as the game drew it ({ rate, frames: the lash's world points, x y z each, a frame
// `rate` a second}: a recording while the animation plays), measured in the world: how far the
// whole lash turns from one drawn frame to the next (its pieces' turn on average: what the eye
// sees jump, however long the game took to draw the frame), the snaps (over SNAP_DEGREES: it
// teleports), and how many frames the game drew a second. What lashQuality measures in the
// track, this measures as it is seen.
function lashSeen(recording) {
  const { rate, frames } = recording ?? {};
  if (!(rate > 0) || !Array.isArray(frames) || frames.length < 3) {
    return { error: 'nothing was recorded' };
  }
  // A frame the game drew once, at the time of the last recording frame that kept it.
  const drawn = [];
  frames.forEach((frame, index) => {
    const before = drawn.at(-1);
    const same = before && frame.every((value, k) => Math.abs(value - before.points[k]) < 1e-6);
    const sample = { time: index / rate, points: frame };
    if (same) {
      drawn[drawn.length - 1] = sample;
    } else {
      drawn.push(sample);
    }
  });
  const ways = (points) => {
    const out = [];
    for (let index = 3; index + 2 < points.length; index += 3) {
      const piece = [
        points[index] - points[index - 3],
        points[index + 1] - points[index - 2],
        points[index + 2] - points[index - 1],
      ];
      out.push(unit(piece, [1, 0, 0]));
    }
    return out;
  };
  const snaps = [];
  let most = { degrees: 0, at: 0 };
  for (let index = 1; index < drawn.length; index += 1) {
    const a = ways(drawn[index - 1].points);
    const b = ways(drawn[index].points);
    const { share, average } = turnOf(a, b);
    const degrees = round(average, 0);
    if (degrees > most.degrees) {
      most = { degrees, at: round(drawn[index].time) };
    }
    if (share >= SNAP_SHARE) {
      snaps.push({ at: round(drawn[index].time), degrees, share: round(share) });
    }
  }
  const problems = [];
  if (snaps.length > 0) {
    problems.push(
      `as drawn, the lash snaps at ${snaps.length} frames (the first at ${snaps[0].at} s: ` +
        `${Math.round(snaps[0].share * 100)} % of it turns over ${SNAP_DEGREES}° from one drawn ` +
        'frame to the next, the whole lash at once)',
    );
  }
  const seconds = frames.length / rate;
  return {
    drawnFramesASecond: round(drawn.length / seconds, 0),
    most,
    snaps: snaps.slice(0, 8),
    problems,
  };
}

// Whether the lash as drawn moves like a whip's crack, phase by phase, measured on a recording of
// its world points (the first the handle's tip): the problems say which phase is wrong.
//   throw     the hand's fast move that stops (its fastest moment with the most drop after it)
//   wind-up   as the throw begins the lash lies behind the hand, against the throw
//             (trail: -1 straight behind, 1 straight ahead; behind is under CRACK.trailMost)
//   loop      during the roll the sharpest bend travels out from the handle to the tip (travel:
//             how its place along the lash follows time, 1 straight out)
//   crack     the tip is fastest after the hand's fastest moment and far faster than the hand
//             (tipOverHand)
//   after     soon after the crack the lash points ahead along the throw (ahead) and does not
//             wrap back past the hand
// A bend this sharp (degrees) between two pieces is the loop.
const LOOP_DEGREES = 30;
const CRACK = Object.freeze({
  trailMost: -0.2,
  travelLeast: 0.5,
  tipOverHandLeast: 3,
  aheadLeast: 0.4,
  afterSeconds: 0.25,
});

function crackCheck(recording) {
  const { rate, frames } = recording ?? {};
  if (!(rate > 0) || !Array.isArray(frames) || frames.length < 10) {
    return { error: 'nothing was recorded' };
  }
  const pointsOf = (frame) => {
    const out = [];
    for (let index = 0; index + 2 < frame.length; index += 3) {
      out.push([frame[index], frame[index + 1], frame[index + 2]]);
    }
    return out;
  };
  const shapes = frames.map(pointsOf);
  const count = shapes.length;
  const speedOf = (index, which) => {
    const a = shapes[Math.max(0, index - 1)];
    const b = shapes[Math.min(count - 1, index + 1)];
    const pick = (shape) => (which === 'tip' ? shape.at(-1) : shape[0]);
    const span = (Math.min(count - 1, index + 1) - Math.max(0, index - 1)) / rate;
    return span > 0 ? size(sub(pick(b), pick(a))) / span : 0;
  };
  const hand = shapes.map((unused, index) => speedOf(index, 'hand'));
  const tip = shapes.map((unused, index) => speedOf(index, 'tip'));
  // The throw: the hand's fastest moment that drops most within a fifth of a second.
  const after = Math.max(1, Math.round(0.2 * rate));
  let peak = 0;
  let best = -1;
  const fastest = Math.max(...hand);
  hand.forEach((speed, index) => {
    if (speed < fastest * 0.5) {
      return;
    }
    const later = hand.slice(index + 1, index + 1 + after);
    const least = later.length > 0 ? Math.min(...later) : speed;
    const score = speed - least;
    if (score > best) {
      best = score;
      peak = index;
    }
  });
  let start = peak;
  while (start > 0 && hand[start - 1] > hand[peak] * 0.35) {
    start -= 1;
  }
  let end = peak;
  while (end < count - 1 && hand[end + 1] > hand[peak] * 0.35) {
    end += 1;
  }
  const throwWay = unit(sub(shapes[end][0], shapes[start][0]), [0, 1, 0]);
  const lashWayAt = (index) => unit(sub(shapes[index].at(-1), shapes[index][0]), throwWay);
  const trail = round(dot(lashWayAt(start), throwWay));
  // The sharpest bend's place along the lash through the roll (from the throw's start to the
  // tip's fastest moment after it).
  const window = Math.max(
    end + 1,
    Math.min(count - 1, peak + Math.round(CRACK.afterSeconds * rate)),
  );
  let tipPeak = peak;
  for (let index = peak; index <= window; index += 1) {
    if (tip[index] > tip[tipPeak]) {
      tipPeak = index;
    }
  }
  const places = [];
  for (let index = start; index <= tipPeak; index += 1) {
    const shape = shapes[index];
    let sharpest = 0;
    let where = 0;
    for (let piece = 1; piece + 1 < shape.length; piece += 1) {
      const bend = bendOf(sub(shape[piece], shape[piece - 1]), sub(shape[piece + 1], shape[piece]));
      if (bend > sharpest) {
        sharpest = bend;
        where = piece / (shape.length - 1);
      }
    }
    // Only a real loop counts (a nearly straight lash's sharpest bend is anywhere).
    if (sharpest >= LOOP_DEGREES) {
      places.push({ t: index, where });
    }
  }
  // How the place follows time: their correlation (1 straight out, 0 not at all).
  let travel = 0;
  if (places.length >= 3) {
    const mean = (values) => values.reduce((a, b) => a + b, 0) / values.length;
    const ts = places.map((one) => one.t);
    const ws = places.map((one) => one.where);
    const mt = mean(ts);
    const mw = mean(ws);
    let cov = 0;
    let vt = 0;
    let vw = 0;
    places.forEach((one) => {
      cov += (one.t - mt) * (one.where - mw);
      vt += (one.t - mt) ** 2;
      vw += (one.where - mw) ** 2;
    });
    travel = vt > 0 && vw > 0 ? round(cov / Math.sqrt(vt * vw)) : 0;
  }
  const tipOverHand = round(tip[tipPeak] / Math.max(0.1, hand[peak]), 1);
  const afterIndex = Math.min(count - 1, tipPeak + Math.round(CRACK.afterSeconds * rate));
  const ahead = round(dot(lashWayAt(afterIndex), throwWay));
  // Once the lash is out ahead (the roll done) it must not swing back past the hand.
  let wrapBack = false;
  let out = false;
  for (let index = tipPeak; index <= afterIndex; index += 1) {
    const along = dot(lashWayAt(index), throwWay);
    if (along >= CRACK.aheadLeast) {
      out = true;
    } else if (out && along < -0.2) {
      wrapBack = true;
    }
  }
  const problems = [];
  if (trail > CRACK.trailMost) {
    problems.push(
      `wind-up: as the throw begins the lash is not behind the hand (trail ${trail}, behind is ` +
        `under ${CRACK.trailMost}): it hangs or lies ahead, so there is nothing to roll out`,
    );
  }
  if (travel < CRACK.travelLeast) {
    problems.push(
      `loop: no loop travels from the handle to the tip in the throw (travel ${travel}, at ` +
        `least ${CRACK.travelLeast}): the lash flicks as a whole`,
    );
  }
  if (tipPeak <= peak || tipOverHand < CRACK.tipOverHandLeast) {
    problems.push(
      `crack: the tip is ${tipOverHand} times the hand's speed (at least ` +
        `${CRACK.tipOverHandLeast}) and ${tipPeak > peak ? 'after' : 'not after'} the hand: ` +
        'no crack',
    );
  }
  if (ahead < CRACK.aheadLeast || wrapBack) {
    problems.push(
      `after: ${CRACK.afterSeconds} s after the crack the lash points ${ahead} along the throw ` +
        `(at least ${CRACK.aheadLeast})${wrapBack ? ' and wraps back past the hand' : ''}`,
    );
  }
  return {
    throw: { start: round(start / rate), end: round(end / rate), handSpeed: round(hand[peak], 1) },
    trail,
    travel,
    crackAt: round(tipPeak / rate),
    tipOverHand,
    ahead,
    wrapBack,
    problems,
  };
}

module.exports = {
  crackCheck,
  CRACK,
  lashSeen,
  DEFAULTS,
  SIM_SEGMENTS,
  TRACK_SEGMENTS,
  TRACK_RATE,
  handlePath,
  wayDegrees,
  simulateLash,
  lashMotion,
  bestLashMotion,
  lashQuality,
};
