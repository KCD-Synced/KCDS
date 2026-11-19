'use strict';

// Where a player's hands are held (Player.setHandTarget): every game solves that player's arm
// (shoulder, elbow, hand) so the hand reaches the target each frame, on top of whatever the body
// plays: walking, running, a clip, a held lantern. The server keeps both hands in the player's
// NetworkPlayer field "handTargets" (kcd2s/integration/hand_targets.hpp; the format there).

const { rotation: rotationOf } = require('../math.cjs');

const HANDS = ['right', 'left'];
// How far a target may be from where it is measured (metres), the longest blend (seconds) and the
// blend when none is given.
const MAX_REACH = 3;
const MAX_BLEND = 10;
const DEFAULT_BLEND = 0.2;
const JOINT_NAME = /^[A-Za-z0-9_ ]{1,48}$/;

function finite(value) {
  return typeof value === 'number' && Number.isFinite(value);
}

function pointOf(value, what) {
  if (value === null || typeof value !== 'object' || ![value.x, value.y, value.z].every(finite)) {
    throw new TypeError(`Player.setHandTarget: ${what} must be { x, y, z } in metres`);
  }
  if (Math.hypot(value.x, value.y, value.z) > MAX_REACH) {
    throw new RangeError(`Player.setHandTarget: ${what} is more than ${MAX_REACH} m away`);
  }
  return { x: value.x, y: value.y, z: value.z };
}

// A quaternion { x, y, z, w } or Euler degrees { x, y, z }, as Player.rotation takes them.
function turnOf(value, what) {
  if (value === null || typeof value !== 'object') {
    throw new TypeError(`Player.setHandTarget: ${what} must be a Quaternion or Euler degrees`);
  }
  const parts =
    'w' in value
      ? [value.x, value.y, value.z, value.w]
      : [value.x ?? 0, value.y ?? 0, value.z ?? 0];
  if (!parts.every(finite)) {
    throw new TypeError(`Player.setHandTarget: ${what} must be a Quaternion or Euler degrees`);
  }
  const q = rotationOf(value);
  const size = Math.hypot(q.x, q.y, q.z, q.w);
  if (!(size > 1e-6)) {
    throw new TypeError(`Player.setHandTarget: ${what} has no turn`);
  }
  return { x: q.x / size, y: q.y / size, z: q.z / size, w: q.w / size };
}

// The checked target of Player.setHandTarget(hand, options): { position, relativeTo, rotation,
// pole, weight, blend, firstPerson: { position, rotation } | null }.
function handTargetOf(hand, options) {
  if (!HANDS.includes(hand)) {
    throw new TypeError("Player.setHandTarget: hand must be 'right' or 'left'");
  }
  if (options === null || typeof options !== 'object') {
    throw new TypeError('Player.setHandTarget: options must be an object, or null to let go');
  }
  const target = {
    position: pointOf(options.position, 'position'),
    relativeTo: '',
    rotation: null,
    pole: null,
    weight: 1,
    blend: DEFAULT_BLEND,
    firstPerson: null,
  };
  if (options.relativeTo !== undefined && options.relativeTo !== null) {
    if (typeof options.relativeTo !== 'string' || !JOINT_NAME.test(options.relativeTo)) {
      throw new TypeError('Player.setHandTarget: relativeTo must be a joint name');
    }
    target.relativeTo = options.relativeTo;
  }
  if (options.rotation !== undefined && options.rotation !== null) {
    target.rotation = turnOf(options.rotation, 'rotation');
  }
  if (options.pole !== undefined && options.pole !== null) {
    target.pole = pointOf(options.pole, 'pole');
  }
  if (options.weight !== undefined) {
    if (!finite(options.weight) || options.weight < 0 || options.weight > 1) {
      throw new RangeError('Player.setHandTarget: weight is from 0 to 1');
    }
    target.weight = options.weight;
  }
  if (options.blend !== undefined) {
    if (!finite(options.blend) || options.blend < 0 || options.blend > MAX_BLEND) {
      throw new RangeError(`Player.setHandTarget: blend is from 0 to ${MAX_BLEND} seconds`);
    }
    target.blend = options.blend;
  }
  if (options.firstPerson !== undefined && options.firstPerson !== null) {
    const view = options.firstPerson;
    if (typeof view !== 'object') {
      throw new TypeError('Player.setHandTarget: firstPerson must be { position?, rotation? }');
    }
    target.firstPerson = {
      position: view.position === undefined ? null : pointOf(view.position, 'firstPerson.position'),
      rotation: view.rotation === undefined ? null : turnOf(view.rotation, 'firstPerson.rotation'),
    };
  }
  return target;
}

// A point in whole millimetres, "x,y,z".
function mm(point) {
  const parts = [point.x, point.y, point.z];
  return parts.map((n) => Math.round(n * 1000)).join(',');
}

// A rotation in ten-thousandths, "x,y,z,w".
function tenThousandths(q) {
  const parts = [q.x, q.y, q.z, q.w];
  return parts.map((n) => Math.round(n * 10000)).join(',');
}

// The field for both hands ({ right, left }, each a target or null), "" for none.
function fieldOf(targets) {
  const entries = [];
  for (const hand of HANDS) {
    const target = targets[hand];
    if (!target) {
      continue;
    }
    const view = target.firstPerson;
    entries.push(
      [
        hand === 'right' ? 'r' : 'l',
        target.relativeTo,
        mm(target.position),
        target.rotation ? tenThousandths(target.rotation) : '',
        target.pole ? mm(target.pole) : '',
        Math.round(target.weight * 100),
        Math.round(target.blend * 1000),
        view?.position ? mm(view.position) : '',
        view?.rotation ? tenThousandths(view.rotation) : '',
      ].join('|'),
    );
  }
  return entries.join(';');
}

module.exports = { handTargetOf, fieldOf, HANDS, MAX_REACH, MAX_BLEND, DEFAULT_BLEND };
