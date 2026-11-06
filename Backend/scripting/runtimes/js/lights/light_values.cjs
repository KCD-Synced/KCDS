'use strict';

// The checks a point light's values go through, shared by the server's Light
// (lights.cjs) and the client's (Client/resources/lights.cjs). Every check throws before anything
// changes; kcd2s/integration/lights.hpp carries the same bounds.

// A light stands within this many metres of the world's origin on every axis.
const POSITION_LIMIT = 100000;
const INTENSITY_MAX = 100;
const RADIUS_MIN = 0.1;
const RADIUS_MAX = 100;
// Warm orange, as a candle or a hearth.
const DEFAULT_COLOR = Object.freeze({ x: 1, y: 0.6, z: 0.3 });
const DEFAULT_INTENSITY = 1;
const DEFAULT_RADIUS = 5;

function finiteNumber(value) {
  return typeof value === 'number' && Number.isFinite(value);
}

// { x, y, z } of a Vector3 or a plain object with finite coordinates.
function tripleOf(value, what) {
  if (value === null || typeof value !== 'object') {
    throw new TypeError(`Light ${what} must be a Vector3 or { x, y, z }`);
  }
  const { x, y, z } = value;
  if (!finiteNumber(x) || !finiteNumber(y) || !finiteNumber(z)) {
    throw new TypeError(`Light ${what} must have finite x, y and z`);
  }
  return { x, y, z };
}

function positionOf(value) {
  const at = tripleOf(value, 'position');
  if ([at.x, at.y, at.z].some((n) => Math.abs(n) > POSITION_LIMIT)) {
    throw new RangeError(`Light position must lie within ${POSITION_LIMIT} m of the origin`);
  }
  return at;
}

function colorOf(value) {
  const color = tripleOf(value, 'color');
  if ([color.x, color.y, color.z].some((n) => n < 0 || n > 1)) {
    throw new RangeError('Light color channels must be from 0 to 1');
  }
  return color;
}

function intensityOf(value) {
  if (!finiteNumber(value)) {
    throw new TypeError('Light intensity must be a finite number');
  }
  if (value < 0 || value > INTENSITY_MAX) {
    throw new RangeError(`Light intensity must be from 0 to ${INTENSITY_MAX}`);
  }
  return value;
}

function radiusOf(value) {
  if (!finiteNumber(value)) {
    throw new TypeError('Light radius must be a finite number');
  }
  if (value < RADIUS_MIN || value > RADIUS_MAX) {
    throw new RangeError(`Light radius must be from ${RADIUS_MIN} to ${RADIUS_MAX} m`);
  }
  return value;
}

function shadowsOf(value) {
  if (typeof value !== 'boolean') {
    throw new TypeError('Light castShadows must be true or false');
  }
  return value;
}

// A light on a body: at most this far from it, its joint's name at most this long.
const OFFSET_MAX = 8;
const JOINT_MAX = 64;

// The body a light hangs on: a Player or an Npc (its network id), 0 for none.
function bodyOf(entity) {
  if (entity === undefined) {
    return 0;
  }
  if (entity === null || typeof entity !== 'object' || !Number.isInteger(entity.id)) {
    throw new TypeError('Light entity must be a Player or an Npc');
  }
  if (entity.id < 1 || entity.id > 0xffffffff) {
    throw new TypeError('Light entity must be a Player or an Npc');
  }
  return entity.id;
}

function jointOf(joint, body) {
  if (joint === undefined) {
    return '';
  }
  if (body === 0) {
    throw new TypeError('Light joint needs an entity');
  }
  if (typeof joint !== 'string') {
    throw new TypeError('Light joint must be a string');
  }
  if (joint.length === 0 || joint.length > JOINT_MAX || !/^[\x20-\x7e]+$/.test(joint)) {
    throw new RangeError(`Light joint must be 1 to ${JOINT_MAX} printable characters`);
  }
  return joint;
}

// The whole state of a new light from Light.spawn's arguments.
function lightOf(position, options) {
  if (options !== undefined && (options === null || typeof options !== 'object')) {
    throw new TypeError('Light options must be an object');
  }
  const given = options ?? {};
  const body = bodyOf(given.entity);
  const joint = jointOf(given.joint, body);
  const at = positionOf(position);
  if (body !== 0 && Math.hypot(at.x, at.y, at.z) > OFFSET_MAX) {
    throw new RangeError(`Light offset must be within ${OFFSET_MAX} m of the entity`);
  }
  return {
    body,
    joint,
    position: at,
    color: given.color === undefined ? { ...DEFAULT_COLOR } : colorOf(given.color),
    intensity: given.intensity === undefined ? DEFAULT_INTENSITY : intensityOf(given.intensity),
    radius: given.radius === undefined ? DEFAULT_RADIUS : radiusOf(given.radius),
    castShadows: given.castShadows === undefined ? false : shadowsOf(given.castShadows),
  };
}

// The checks of each changeable property, by name.
const CHECKS = Object.freeze({
  position: positionOf,
  color: colorOf,
  intensity: intensityOf,
  radius: radiusOf,
  castShadows: shadowsOf,
});

module.exports = {
  POSITION_LIMIT,
  INTENSITY_MAX,
  RADIUS_MIN,
  RADIUS_MAX,
  DEFAULT_COLOR,
  DEFAULT_INTENSITY,
  DEFAULT_RADIUS,
  CHECKS,
  lightOf,
};
