'use strict';

// A game clip baked into a studio animation: the game client records one round of
// the part of the clip that plays (studio/studio_game_clip.hpp studioGameClipBake: each frame
// the body's joints turned in the body's space, the hips' place) and this makes keys of it, the
// studio's own animation, that plays on players as every studio animation does
// (Animations.define, player.playAnimation(name)). The legs are the clip's own.
//
// - Each joint's turn is kept against its parent's, as a turn on top of the body's own pose
//   before the clip (the idle): key turn = clip turn * idle turn^-1, in degrees x, y, z about the
//   parent's axes (animation.cjs quaternion: z * y * x).
// - The skeleton's root is not baked: a cutscene walks and turns the whole body with it. The
//   hips' slow turning (over HEADING_SECONDS) is taken out too, so the body faces its player's
//   forward; a strike's twist stays.
// - The hips stay over the body's place; how far they go up or down is each key's lift (bent
//   knees stand on the ground).
// - Only a loop of the part is kept: the stretch, SHORTEST_LOOP to LONGEST_LOOP long, that ends
//   in the pose it starts with, the body standing (its hips not wandering, its legs not
//   stepping). What is left between its end and its start is spread over the loop, so it goes
//   round without a seam.
// - The joints that hardly turn (QUIET_DEGREES) are left out, and at most MOST_KEYS keys made.

const HEADING_SECONDS = 1;
const KEYS_A_SECOND = 12;
const MOST_KEYS = 32;
const MOST_JOINTS = 32;
const SHORTEST_LOOP = 1.2;
const LONGEST_LOOP = 4;
const LOOP_STEP = 1 / 15;
// The seam is also compared this many steps after the loop's end and start (0.2 s).
const SEAM_LATER = 3;
const QUIET_DEGREES = 1.5;
// How much a loop's hips wandering (degrees a metre) and its legs' stepping count in its cost.
const WANDER_WEIGHT = 200;
const STEP_WEIGHT = 0.4;
const LEG_JOINT = /(UpLeg|Leg|Foot|ToeBase)$/;

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

function conjugate([x, y, z, w]) {
  return [-x, -y, -z, w];
}

function normalized(q) {
  const length = Math.hypot(q[0], q[1], q[2], q[3]) || 1;
  return q.map((part) => part / length);
}

function slerp(a, b, share) {
  let dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  const to = dot < 0 ? b.map((part) => -part) : b;
  dot = Math.abs(dot);
  if (dot > 0.9995) {
    return normalized(a.map((part, index) => part + (to[index] - part) * share));
  }
  const angle = Math.acos(Math.min(1, dot));
  const sine = Math.sin(angle);
  const wa = Math.sin((1 - share) * angle) / sine;
  const wb = Math.sin(share * angle) / sine;
  return a.map((part, index) => part * wa + to[index] * wb);
}

// The angle between two turns, degrees.
function angleBetween(a, b) {
  const dot = Math.abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
  return (2 * Math.acos(Math.min(1, dot)) * 180) / Math.PI;
}

function aboutZ(degrees) {
  const half = (degrees * Math.PI) / 360;
  return [0, 0, Math.sin(half), Math.cos(half)];
}

// The turn's heading about z, degrees counterclockwise: where its forward (+y) points.
function headingOf(q) {
  const [x, y, z, w] = q;
  // The forward turned: (2(xy - wz), 1 - 2(x² + z²), ...).
  const fx = 2 * (x * y - w * z);
  const fy = 1 - 2 * (x * x + z * z);
  return (Math.atan2(-fx, fy) * 180) / Math.PI;
}

// Degrees x, y, z (the turn z * y * x) of a quaternion, the solution nearest `near` (the key
// before), each angle within 180 of it.
function eulerOf(q, near) {
  const [x, y, z, w] = q;
  const r00 = 1 - 2 * (y * y + z * z);
  const r10 = 2 * (x * y + w * z);
  const r20 = 2 * (x * z - w * y);
  const r21 = 2 * (y * z + w * x);
  const r22 = 1 - 2 * (x * x + y * y);
  const degrees = 180 / Math.PI;
  const b = Math.asin(Math.max(-1, Math.min(1, -r20))) * degrees;
  const a = Math.atan2(r21, r22) * degrees;
  const c = Math.atan2(r10, r00) * degrees;
  const unwrapped = (angles) =>
    angles.map((angle, axis) => {
      if (!near) {
        return angle;
      }
      let value = angle;
      while (value - near[axis] > 180) {
        value -= 360;
      }
      while (value - near[axis] < -180) {
        value += 360;
      }
      return value;
    });
  const first = unwrapped([a, b, c]);
  const second = unwrapped([a + 180, 180 - b, c + 180]);
  if (!near) {
    return first;
  }
  const distance = (angles) =>
    angles.reduce((sum, angle, axis) => sum + Math.abs(angle - near[axis]), 0);
  return distance(first) <= distance(second) ? first : second;
}

// The parts' frames as { time, hips: [x, y, z], turns: [[x, y, z, w], ...] }, by time.
function framesOf(baked) {
  const count = baked.names.length;
  const width = 4 + 4 * count;
  const frames = [];
  for (let at = 0; at + width <= baked.frames.length; at += width) {
    const turns = [];
    for (let joint = 0; joint < count; joint += 1) {
      const from = at + 4 + joint * 4;
      turns.push(normalized(baked.frames.slice(from, from + 4)));
    }
    frames.push({ time: baked.frames[at], hips: baked.frames.slice(at + 1, at + 4), turns });
  }
  frames.sort((a, b) => a.time - b.time);
  return frames.filter((frame, index) => index === 0 || frame.time > frames[index - 1].time);
}

// Each joint's turn against its parent's (the root's against the body).
function relativeOf(turns, parents) {
  return turns.map((turn, joint) =>
    parents[joint] < 0 ? turn : multiply(conjugate(turns[parents[joint]]), turn),
  );
}

// The frame at `time` (seconds into the part), blended from the two around it.
function frameAt(frames, time) {
  let next = frames.findIndex((frame) => frame.time >= time);
  if (next <= 0) {
    return next === 0 ? frames[0] : frames[frames.length - 1];
  }
  const before = frames[next - 1];
  const after = frames[next];
  const share = (time - before.time) / (after.time - before.time);
  return {
    time,
    hips: before.hips.map((part, axis) => part + (after.hips[axis] - part) * share),
    turns: before.turns.map((turn, joint) => slerp(turn, after.turns[joint], share)),
  };
}

// The stretch of the frames that loops best: its start and end (seconds) and its cost.
function bestLoop(frames, parents, names) {
  const length = frames[frames.length - 1].time;
  const legs = names.map((name) => LEG_JOINT.test(name));
  // The hips without their heading: the bake takes their slow turning out, so it is no seam.
  const hips = names.findIndex((name) => /^Hips$/i.test(name));
  const relative = (time) => {
    const turns = relativeOf(frameAt(frames, time).turns, parents);
    if (hips >= 0) {
      turns[hips] = multiply(aboutZ(-headingOf(turns[hips])), turns[hips]);
    }
    return turns;
  };
  const samples = [];
  for (let time = 0; time <= length + 1e-6; time += LOOP_STEP) {
    samples.push({ time, frame: frameAt(frames, time), turns: relative(time) });
  }
  // The legs' travel from one sample to the next (degrees), summed up to each sample.
  const travel = [0];
  for (let index = 1; index < samples.length; index += 1) {
    let step = 0;
    samples[index].turns.forEach((turn, joint) => {
      if (legs[joint]) {
        step += angleBetween(turn, samples[index - 1].turns[joint]);
      }
    });
    travel.push(travel[index - 1] + step);
  }
  let best = null;
  for (let start = 0; start < samples.length; start += 1) {
    for (let end = start + 1; end < samples.length; end += 1) {
      const span = samples[end].time - samples[start].time;
      if (span < SHORTEST_LOOP) {
        continue;
      }
      if (span > LONGEST_LOOP) {
        break;
      }
      // The pose at the end against the start's, and a moment later too (the same pose moving
      // the other way is no loop).
      const later = end + SEAM_LATER;
      if (later >= samples.length) {
        break;
      }
      let seam = 0;
      for (const [from, to] of [
        [start, end],
        [start + SEAM_LATER, later],
      ]) {
        samples[to].turns.forEach((turn, joint) => {
          if (parents[joint] >= 0) {
            seam += angleBetween(turn, samples[from].turns[joint]);
          }
        });
      }
      const a = samples[start].frame.hips;
      const b = samples[end].frame.hips;
      const wander = Math.hypot(b[0] - a[0], b[1] - a[1]) * WANDER_WEIGHT;
      const stepping = ((travel[end] - travel[start]) / span) * STEP_WEIGHT;
      const cost = seam + wander + stepping;
      if (!best || cost < best.cost) {
        best = { from: samples[start].time, to: samples[end].time, cost, seam, wander, stepping };
      }
    }
  }
  return best ?? { from: 0, to: length, cost: Infinity, seam: 0, wander: 0, stepping: 0 };
}

// The slow turning in the headings at the key times (degrees): the turn from the loop's start
// to its end, evenly, and what is left of the rest smoothed over HEADING_SECONDS round the loop.
function slowHeadings(headings, times, length) {
  // Unwrapped, key to key.
  const unwrapped = [headings[0]];
  for (let index = 1; index < headings.length; index += 1) {
    let heading = headings[index];
    while (heading - unwrapped[index - 1] > 180) {
      heading -= 360;
    }
    while (heading - unwrapped[index - 1] < -180) {
      heading += 360;
    }
    unwrapped.push(heading);
  }
  const start = unwrapped[0];
  const turned = unwrapped[unwrapped.length - 1] - start;
  const trend = times.map((time) => start + (turned * time) / length);
  const rest = unwrapped.map((heading, index) => heading - trend[index]);
  // The last key is the first one again: left out of the smoothing.
  const round = times.slice(0, -1);
  return times.map((time, index) => {
    let sum = 0;
    let weights = 0;
    round.forEach((other, at) => {
      let apart = Math.abs(other - time);
      apart = Math.min(apart, length - apart);
      const weight = Math.exp(-0.5 * (apart / (HEADING_SECONDS / 2)) ** 2);
      sum += rest[at] * weight;
      weights += weight;
    });
    return trend[index] + sum / weights;
  });
}

// The studio animation of a bake ({ seconds, names, parents, idle, idleHips, frames }, the parts
// put together): { keys: [{ time, pose: { <joint name>: [x, y, z] }, lift }], blend, loop,
// loopFrom, loopTo, joints } or { error }.
function bakeClip(baked) {
  const names = baked?.names ?? [];
  const parents = baked?.parents ?? [];
  if (names.length === 0 || parents.length !== names.length) {
    return { error: 'the bake names no joints' };
  }
  const frames = framesOf(baked);
  if (frames.length < 8 || frames[frames.length - 1].time < SHORTEST_LOOP) {
    return { error: `the bake is too short (${frames.length} frames)` };
  }
  const idleTurns = [];
  for (let joint = 0; joint < names.length; joint += 1) {
    idleTurns.push(normalized(baked.idle.slice(joint * 4, joint * 4 + 4)));
  }
  const idle = relativeOf(idleTurns, parents);
  const idleHips = baked.idleHips ?? [0, 0, 0];
  const loop = bestLoop(frames, parents, names);
  const length = loop.to - loop.from;
  const count = Math.min(MOST_KEYS, Math.max(2, Math.round(length * KEYS_A_SECOND) + 1));
  const times = Array.from({ length: count }, (unused, index) => (length * index) / (count - 1));
  const hips = names.findIndex((name) => /^Hips$/i.test(name));
  // The baked joints: all but the root (whose parent is the body).
  const baking = names.map((unused, joint) => parents[joint] >= 0);
  // Each key's turns on top of the idle and its lift.
  const keyed = times.map((time) => {
    const frame = frameAt(frames, loop.from + time);
    const relative = relativeOf(frame.turns, parents);
    return {
      turns: relative.map((turn, joint) => normalized(multiply(turn, conjugate(idle[joint])))),
      lift: frame.hips[2] - idleHips[2],
    };
  });
  // The hips' slow turning taken out.
  if (hips >= 0) {
    const headings = keyed.map((key) => headingOf(key.turns[hips]));
    const smooth = slowHeadings(headings, times, length);
    keyed.forEach((key, index) => {
      key.turns[hips] = normalized(multiply(aboutZ(-smooth[index]), key.turns[hips]));
    });
  }
  // What is left between the loop's end and its start, spread over it: the last key is the
  // first one again.
  const last = keyed[keyed.length - 1];
  const first = keyed[0];
  keyed.forEach((key, index) => {
    const share = index / (keyed.length - 1);
    key.turns = key.turns.map((turn, joint) => {
      const left = multiply(first.turns[joint], conjugate(last.turns[joint]));
      return normalized(multiply(slerp([0, 0, 0, 1], left, share), turn));
    });
    key.lift -= (last.lift - first.lift) * share;
  });
  // The joints that turn at all, the most turned ones first when there are too many.
  const most = names.map((unused, joint) =>
    keyed.reduce(
      (largest, key) => Math.max(largest, angleBetween(key.turns[joint], [0, 0, 0, 1])),
      0,
    ),
  );
  const kept = names
    .map((name, joint) => ({ name, joint }))
    .filter(({ joint }) => baking[joint] && most[joint] >= QUIET_DEGREES)
    .sort((a, b) => most[b.joint] - most[a.joint])
    .slice(0, MOST_JOINTS);
  const nearest = new Map();
  const keys = keyed.map((key, index) => {
    const pose = {};
    for (const { name, joint } of kept) {
      const turn = eulerOf(key.turns[joint], nearest.get(name));
      nearest.set(name, turn);
      pose[name] = turn.map((part) => Math.round(part * 10) / 10);
    }
    return {
      time: Math.round(times[index] * 100) / 100,
      pose,
      lift: Math.round(Math.max(-1, Math.min(0.5, key.lift)) * 1000) / 1000,
    };
  });
  return {
    keys,
    blend: 'spline',
    loop: true,
    loopFrom: Math.round(loop.from * 100) / 100,
    loopTo: Math.round(loop.to * 100) / 100,
    joints: kept.length,
    seam: Math.round(loop.seam),
  };
}

module.exports = { bakeClip, eulerOf, framesOf, bestLoop };
