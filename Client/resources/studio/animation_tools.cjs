'use strict';

// The assistant's tools for an animation (the studio's MCP): changes to some keys or
// joints, whole tracks and the timing, checks and comparisons, a whip lash's shape from a few
// numbers, and what an export will be. Every function takes and gives the animation with its
// joints by name, as studio_get_animation answers it: { keys: [{ time, pose: { <joint>: [x, y,
// z] }, lift?, lash?, prop? }], blend, loop }. Nothing here touches the editor; the editor makes
// each change one step to undo (editor.cjs script).

const { quaternion, lashWay, wayAngles } = require('./animation.cjs');
const { lashQuality } = require('./lash_motion.cjs');

const MOST_KEYS = 32;
const MOST_JOINTS = 32;
const FIELD_BYTES = 2000;
const DEFINED_BYTES = 30000;
const EVENT_BYTES = 32768;
// A turn this much more than the key before's (degrees, about one axis) is a jump; a loop's end
// this far from its start is a seam; a lash this much longer or shorter than the prop is wrong.
const JUMP_DEGREES = 90;
// Degrees changing this much more than the joint turns: the blend goes the long way round.
const SPIN_DEGREES = 60;
const SEAM_DEGREES = 5;
const LASH_LENGTH_SHARE = 0.1;

// How far a joint really turns from one key's degrees to the other's (degrees, the shortest way;
// the turn x, then y, then z, as animation.cjs quaternion).
function turnBetween(a, b) {
  const qa = quaternion(a);
  const qb = quaternion(b);
  const dot = Math.abs(qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3]);
  return (2 * Math.acos(Math.min(1, dot)) * 180) / Math.PI;
}

function clone(data) {
  return JSON.parse(JSON.stringify(data));
}

function round(value, places = 1) {
  const scale = 10 ** places;
  return Math.round(value * scale) / scale || 0;
}

// The keys a change is for: { keys: [index, ...] }, { times: [s, ...] } (the keys at those times,
// a hundredth of a second either way), { from, to } (seconds) or all of them.
function keysOf(data, where = {}) {
  const all = data.keys.map((key, index) => index);
  if (Array.isArray(where.keys)) {
    return all.filter((index) => where.keys.includes(index));
  }
  if (Array.isArray(where.times)) {
    return all.filter((index) =>
      where.times.some((time) => Math.abs(data.keys[index].time - time) < 0.011),
    );
  }
  if (Number.isFinite(where.from) || Number.isFinite(where.to)) {
    const from = Number.isFinite(where.from) ? where.from : 0;
    const to = Number.isFinite(where.to) ? where.to : Infinity;
    return all.filter((index) => data.keys[index].time >= from && data.keys[index].time <= to);
  }
  return all;
}

// Some keys changed: pose { joint: [x, y, z] (set) | null (taken out) }, add { joint: [x, y, z] }
// (added to what it has), lift, lash (null takes it out), remove [joints] (from every key chosen).
function patchAnimation(data, patch) {
  const next = clone(data);
  const chosen = keysOf(next, patch);
  for (const index of chosen) {
    const key = next.keys[index];
    for (const [joint, turn] of Object.entries(patch.pose ?? {})) {
      if (turn === null) {
        delete key.pose[joint];
      } else {
        key.pose[joint] = turn.map((part) => round(Number(part) || 0));
      }
    }
    for (const [joint, turn] of Object.entries(patch.add ?? {})) {
      const now = key.pose[joint] ?? [0, 0, 0];
      key.pose[joint] = now.map((part, axis) => round(part + (Number(turn[axis]) || 0)));
    }
    for (const joint of patch.remove ?? []) {
      delete key.pose[joint];
    }
    if (patch.lift !== undefined) {
      if (patch.lift === null) {
        delete key.lift;
      } else {
        key.lift = round(patch.lift, 3);
      }
    }
    if (patch.lash !== undefined) {
      if (patch.lash === null) {
        delete key.lash;
      } else {
        key.lash = patch.lash.map((point) => point.map((part) => round(part, 3)));
      }
    }
  }
  return { animation: next, keys: chosen.length };
}

// The name of a joint's other side (Left <-> Right), or null for one in the middle.
function otherSide(joint) {
  if (joint.startsWith('Left')) {
    return `Right${joint.slice(4)}`;
  }
  if (joint.startsWith('Right')) {
    return `Left${joint.slice(5)}`;
  }
  return null;
}

// A whole track changed, over the keys chosen (as patchAnimation's): op
//   'copy'    joint's turns to `to` (signs [x, y, z] multiply them, [1, 1, 1] by default)
//   'mirror'  the other side's turns onto joint (signs [1, -1, -1] by default: the skeleton's
//             two sides are mirrored across its middle); joint 'all' mirrors every Left/Right pair
//             from `from` ('Right' by default) onto the other side
//   'scale'   joint's turns times factor (one number or [x, y, z])
//   'offset'  joint's turns plus [x, y, z]
//   'smooth'  joint's turns averaged with the keys either side, `passes` times (round the loop)
//   'remove'  joint's track out of every key chosen
function trackAnimation(data, change) {
  const next = clone(data);
  const chosen = keysOf(next, change);
  const { op, joint } = change;
  const take = (key, name) => key.pose[name];
  const put = (key, name, turn) => {
    if (turn) {
      key.pose[name] = turn.map((part) => round(part));
    } else {
      delete key.pose[name];
    }
  };
  if (op === 'copy' || op === 'mirror') {
    const signs = change.signs ?? (op === 'mirror' ? [1, -1, -1] : [1, 1, 1]);
    const pairs = [];
    if (op === 'mirror' && joint === 'all') {
      const from = change.from === 'Left' ? 'Left' : 'Right';
      const names = new Set(next.keys.flatMap((key) => Object.keys(key.pose)));
      for (const name of names) {
        if (name.startsWith(from) && otherSide(name)) {
          pairs.push([name, otherSide(name)]);
        }
      }
    } else if (op === 'mirror') {
      pairs.push([otherSide(joint), joint]);
    } else {
      pairs.push([joint, change.to]);
    }
    if (pairs.some(([from, to]) => !from || !to)) {
      return { error: `${op}: name a joint with a Left or Right side (or 'to' for copy)` };
    }
    for (const index of chosen) {
      const key = next.keys[index];
      for (const [from, to] of pairs) {
        const turn = take(key, from);
        put(key, to, turn ? turn.map((part, axis) => part * signs[axis]) : null);
      }
    }
    return { animation: next, keys: chosen.length, joints: pairs.map(([, to]) => to) };
  }
  if (!joint) {
    return { error: `${op}: name a joint` };
  }
  if (op === 'scale' || op === 'offset') {
    const by = Array.isArray(change.by) ? change.by : [change.by, change.by, change.by];
    for (const index of chosen) {
      const key = next.keys[index];
      const turn = take(key, joint) ?? [0, 0, 0];
      put(
        key,
        joint,
        turn.map((part, axis) => {
          const value = Number(by[axis]);
          if (op === 'scale') {
            return part * (Number.isFinite(value) ? value : 1);
          }
          return part + (Number.isFinite(value) ? value : 0);
        }),
      );
    }
    return { animation: next, keys: chosen.length };
  }
  if (op === 'smooth') {
    const passes = Math.min(10, Math.max(1, Math.round(change.passes ?? 1)));
    const count = next.keys.length;
    for (let pass = 0; pass < passes; pass += 1) {
      const before = next.keys.map((key) => take(key, joint) ?? [0, 0, 0]);
      for (const index of chosen) {
        // Round the loop at its ends (its last key is its first again); held at the ends else.
        const at = (offset) => {
          let other = index + offset;
          if (next.loop !== false) {
            other = (other + count - 1) % (count - 1);
          }
          return before[Math.min(count - 1, Math.max(0, other))];
        };
        const a = at(-1);
        const b = before[index];
        const c = at(1);
        const averaged = b.map((part, axis) => (a[axis] + 2 * part + c[axis]) / 4);
        put(next.keys[index], joint, averaged);
      }
      if (next.loop !== false && count > 1) {
        put(next.keys[count - 1], joint, take(next.keys[0], joint));
      }
    }
    return { animation: next, keys: chosen.length };
  }
  if (op === 'remove') {
    for (const index of chosen) {
      delete next.keys[index].pose[joint];
    }
    return { animation: next, keys: chosen.length };
  }
  return { error: `track: op is copy, mirror, scale, offset, smooth or remove` };
}

// The timing changed: op 'speed' (times divided by factor; a lash track plays that much faster),
// 'trim' (only from..to seconds, starting at 0), 'shift' (every key after `from` later by `by`
// seconds). Trim and shift take a lash track out (it no longer fits the keys' times: Lash from
// motion makes it again); `lashTrack` in the answer says what happened to it.
function retimeAnimation(data, change) {
  const next = clone(data);
  let lashTrack = next.lashTrack ? 'kept' : 'none';
  if (change.op === 'speed') {
    const factor = Number(change.factor);
    if (!(factor > 0)) {
      return { error: 'speed: a factor above 0' };
    }
    next.keys.forEach((key) => {
      key.time = round(key.time / factor, 2);
    });
    if (next.lashTrack) {
      next.lashTrack.rate = Math.min(60, round(next.lashTrack.rate * factor, 3));
      lashTrack = 'faster with the keys';
    }
  } else if (change.op === 'trim') {
    const from = Number(change.from) || 0;
    const to = Number.isFinite(change.to) ? change.to : Infinity;
    next.keys = next.keys
      .filter((key) => key.time >= from - 1e-6 && key.time <= to + 1e-6)
      .map((key) => ({ ...key, time: round(key.time - from, 2) }));
    if (next.keys.length === 0) {
      return { error: 'trim: no key is left between from and to' };
    }
    next.keys[0].time = 0;
  } else if (change.op === 'shift') {
    const from = Number(change.from) || 0;
    const by = Number(change.by) || 0;
    next.keys.forEach((key, index) => {
      if (index > 0 && key.time > from) {
        key.time = round(key.time + by, 2);
      }
    });
  } else {
    return { error: 'retime: op is speed, trim or shift' };
  }
  if (change.op !== 'speed' && next.lashTrack) {
    next.lashTrack = null;
    lashTrack = 'taken out: make it again with Lash from motion';
  }
  for (let index = 1; index < next.keys.length; index += 1) {
    if (next.keys[index].time <= next.keys[index - 1].time) {
      return { error: 'the keys would not stay in time order' };
    }
  }
  return { animation: next, keys: next.keys.length, length: next.keys.at(-1).time, lashTrack };
}

// A number as the field writes it (C's %g: at most six significant digits).
function written(value) {
  return String(Number(Number(value).toPrecision(6)));
}

// How long the animation is as the player field writes it (bytes; the prop keys roughly).
function fieldBytes(data) {
  let text = 'A1LS';
  for (const key of data.keys) {
    const joints = Object.keys(key.pose)
      .sort()
      .map((name) => `${name}=${key.pose[name].map(written).join(',')}`);
    const lift = key.lift === undefined ? '' : `#${written(key.lift)}`;
    const lash = key.lash
      ? `~${key.lash.map((point) => point.map(written).join(',')).join(';')}`
      : '';
    // A prop setting is its one-letter code and its value ("@p90;c1").
    const prop = key.prop
      ? `@${Object.values(key.prop)
          .map((value) => `x${typeof value === 'number' ? written(value) : '0'}`)
          .join(';')}`
      : '';
    text += `|${written(key.time)}:${joints.join(';')}${lift}${lash}${prop}`;
  }
  // The lash track after the keys ("^rate,length:" and its frames).
  const track = data.lashTrack;
  if (track) {
    const frames = track.frames.map((frame) => frame.map(written).join(',')).join('/');
    const body = track.space === 'body' ? ',b' : '';
    text += `^${written(track.rate)},${written(track.length)}${body}:${frames}`;
  }
  return new TextEncoder().encode(text).length;
}

function lashLength(lash) {
  let length = Math.hypot(...lash[0]);
  for (let index = 1; index < lash.length; index += 1) {
    const [a, b] = [lash[index - 1], lash[index]];
    length += Math.hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
  }
  return length;
}

// What could be wrong with it, and what an export of it will be: { problems: [text], ... }.
// lashLength: the whip's lash (metres), to check the keys' lash shapes against.
function checkAnimation(data, options = {}) {
  const problems = [];
  const keys = data.keys;
  const joints = new Set(keys.flatMap((key) => Object.keys(key.pose)));
  if (keys.length > MOST_KEYS) {
    problems.push(`${keys.length} keys: a player's animation takes at most ${MOST_KEYS}`);
  }
  keys.forEach((key, index) => {
    const count = Object.keys(key.pose).length;
    if (count > MOST_JOINTS) {
      problems.push(`key ${index}: ${count} joints (at most ${MOST_JOINTS})`);
    }
  });
  // Between keys: a joint whose degrees change much more than it really turns goes the long way
  // round (it spins); one that really turns far between two keys is a fast turn (a strike's wrist
  // snap is one), told but no problem.
  const fastTurns = [];
  for (const joint of joints) {
    for (let index = 1; index < keys.length; index += 1) {
      const a = keys[index - 1].pose[joint] ?? [0, 0, 0];
      const b = keys[index].pose[joint] ?? [0, 0, 0];
      const most = Math.max(...a.map((part, axis) => Math.abs(b[axis] - part)));
      const turned = turnBetween(a, b);
      const when = `key ${index - 1} to ${index} (${keys[index - 1].time}–${keys[index].time} s)`;
      if (most > turned + SPIN_DEGREES) {
        const change = `its degrees change ${round(most)}°, it turns ${round(turned)}°`;
        problems.push(`${joint} spins the long way from ${when}: ${change}`);
      } else if (turned > JUMP_DEGREES) {
        fastTurns.push(`${joint} turns ${round(turned)}° from ${when}`);
      }
    }
  }
  // The loop's seam.
  if (data.loop !== false && keys.length > 1) {
    const first = keys[0];
    const last = keys.at(-1);
    for (const joint of joints) {
      const a = first.pose[joint] ?? [0, 0, 0];
      const b = last.pose[joint] ?? [0, 0, 0];
      const most = Math.max(...a.map((part, axis) => Math.abs(b[axis] - part)));
      if (most > SEAM_DEGREES) {
        const off = round(most);
        problems.push(`${joint}: the last key is ${off}° from the first (a seam in the loop)`);
      }
    }
  }
  // The lash's shapes: each key's against the whip, then the lash as it plays (lash_motion.cjs
  // lashQuality: its length all along, folds, jumps, the loop's seam).
  const lashes = keys.filter((key) => key.lash);
  if (lashes.length > 0 && options.lashLength > 0 && !data.lashTrack) {
    keys.forEach((key, index) => {
      if (!key.lash) {
        return;
      }
      const length = lashLength(key.lash);
      if (Math.abs(length - options.lashLength) > options.lashLength * LASH_LENGTH_SHARE) {
        const along = round(length, 2);
        const whip = options.lashLength;
        problems.push(`key ${index}: the lash is ${along} m along, the whip's ${whip} m`);
      }
    });
  }
  const lash = lashQuality(data, options.lashLength ?? 0);
  let lashReport = null;
  if (lash) {
    const { problems: lashProblems, ...rest } = lash;
    problems.push(...lashProblems.map((text) => `lash: ${text}`));
    lashReport = rest;
  }
  const bytes = fieldBytes(data);
  if (bytes > DEFINED_BYTES) {
    problems.push(`${bytes} bytes as sent: Animations.define takes at most ${DEFINED_BYTES}`);
  }
  return {
    problems,
    fastTurns,
    keys: keys.length,
    joints: joints.size,
    length: keys.at(-1)?.time ?? 0,
    lashKeys: lashes.length,
    lashTrack: data.lashTrack
      ? {
          rate: data.lashTrack.rate,
          frames: data.lashTrack.frames.length,
          length: data.lashTrack.length,
        }
      : null,
    // How the lash plays (lashQuality without its problems, which are above): null for none.
    lash: lashReport,
    liftKeys: keys.filter((key) => key.lift !== undefined).length,
    bytes,
    sent: bytes <= FIELD_BYTES ? 'in the player field' : 'by its name (Animations.define)',
    fits: bytes <= DEFINED_BYTES && bytes + 64 <= EVENT_BYTES && keys.length <= MOST_KEYS,
  };
}

// How far two animations are apart, joint by joint: the most degrees about any axis at the keys
// of the first (the second sampled by its nearest key at or before each time).
function compareAnimations(first, second) {
  const at = (data, time) => {
    let found = data.keys[0];
    for (const key of data.keys) {
      if (key.time <= time + 1e-6) {
        found = key;
      }
    }
    return found;
  };
  const joints = new Set([
    ...first.keys.flatMap((key) => Object.keys(key.pose)),
    ...second.keys.flatMap((key) => Object.keys(key.pose)),
  ]);
  const apart = {};
  for (const joint of joints) {
    let most = 0;
    let when = 0;
    for (const key of first.keys) {
      const a = key.pose[joint] ?? [0, 0, 0];
      const b = at(second, key.time).pose[joint] ?? [0, 0, 0];
      const off = Math.max(...a.map((part, axis) => Math.abs(b[axis] - part)));
      if (off > most) {
        most = off;
        when = key.time;
      }
    }
    apart[joint] = { most: round(most), at: when };
  }
  const order = Object.entries(apart).sort((a, b) => b[1].most - a[1].most);
  return { joints: Object.fromEntries(order) };
}

// A whip lash's shape from a few numbers: `count` points along `length` metres from the handle's
// tip, bent `curl` degrees toward the handle's up side and `sway` sideways from the handle to the
// tip (as a posed lash bends), and sagging `sag` metres down at the tip (more toward it); the
// points stay a segment apart, so the lash keeps its length.
function lashShape({ curl = 0, sway = 0, sag = 0, length = 1.6, count = 12 } = {}) {
  const segment = length / count;
  const points = [];
  let at = [0, 0, 0];
  for (let index = 0; index < count; index += 1) {
    const share = (index + 0.5) / count;
    const c = (curl * share * Math.PI) / 180;
    const s = (sway * share * Math.PI) / 180;
    let way = [Math.cos(c) * Math.cos(s), Math.cos(c) * Math.sin(s), Math.sin(c)];
    // The sag: the piece leans down by more toward the tip, then is a segment long again.
    way[2] -= (2 * sag * share) / length;
    const norm = Math.hypot(...way) || 1;
    way = way.map((part) => part / norm);
    at = at.map((part, axis) => part + way[axis] * segment);
    points.push(at.map((part) => round(part, 3)));
  }
  return points;
}

// A unit way turned `degrees` about the unit `axis` (right-handed).
function turnedAbout(way, axis, degrees) {
  const angle = (degrees * Math.PI) / 180;
  const cos = Math.cos(angle);
  const sin = Math.sin(angle);
  const across = [
    axis[1] * way[2] - axis[2] * way[1],
    axis[2] * way[0] - axis[0] * way[2],
    axis[0] * way[1] - axis[1] * way[0],
  ];
  const along = axis[0] * way[0] + axis[1] * way[1] + axis[2] * way[2];
  return [0, 1, 2].map(
    (part) => way[part] * cos + across[part] * sin + axis[part] * along * (1 - cos),
  );
}

// A lash key's ways ([yaw, pitch] degrees in the body's frame: forward, right, up) shaped:
// aimSide (degrees the whole lash turns toward the body's right), aimUp (up, across its first
// piece), curl (degrees it bends up toward the tip, down below 0), sway (toward the right toward
// the tip), tipTwist (its last third toward the right), sag (metres its tip sinks; `length` the
// lash's). Each changes what the key has, by that much.
function shapedWays(ways, change, length) {
  const vectors = ways.map(([yaw, pitch]) => lashWay(yaw, pitch));
  const up = [0, 0, 1];
  const count = vectors.length;
  const acrossOf = (way) => {
    const across = [way[1] * up[2] - way[2] * up[1], way[2] * up[0] - way[0] * up[2], 0];
    const size = Math.hypot(...across);
    return size > 1e-6 ? across.map((part) => part / size) : [1, 0, 0];
  };
  // The frame is forward, right, up: a turn about up by a positive angle goes from forward to
  // the right; about `across` (the first piece's left) by a positive angle, up.
  const side = Number(change.aimSide) || 0;
  const aimed = turnedAbout(vectors[0], up, side);
  const across = acrossOf(aimed);
  return vectors.map((way, index) => {
    const share = (index + 1) / count;
    let out = turnedAbout(way, up, side);
    out = turnedAbout(out, across, Number(change.aimUp) || 0);
    out = turnedAbout(out, across, (Number(change.curl) || 0) * share);
    out = turnedAbout(out, up, (Number(change.sway) || 0) * share);
    const tip = Math.max(0, (share - 2 / 3) * 3);
    out = turnedAbout(out, up, (Number(change.tipTwist) || 0) * tip);
    const sag = Number(change.sag) || 0;
    if (sag) {
      out = [out[0], out[1], out[2] - (2 * sag * share) / length];
    }
    const size = Math.hypot(...out) || 1;
    return wayAngles(out.map((part) => part / size));
  });
}

module.exports = {
  shapedWays,
  patchAnimation,
  trackAnimation,
  retimeAnimation,
  checkAnimation,
  compareAnimations,
  lashShape,
  fieldBytes,
  otherSide,
};
