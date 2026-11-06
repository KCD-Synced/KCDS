'use strict';

// The studio's camera as in Blender: orbit round a focus with the middle mouse
// button, pan with Shift, zoom with Ctrl, the front, side and top views (numpad 1, 3, 7) and
// framing what is chosen (F). The free camera is set as one pose (NoClip.setPose: a place and a
// direction); these are its numbers.
//
// An orbit is { target: [x, y, z], yaw, pitch, distance }: the camera at `distance` from the
// target, `yaw` radians round the world's z from +x, `pitch` radians up from level.

const ORBIT_RADIANS_PER_PIXEL = 0.008;
const PAN_PER_PIXEL = 0.0015; // times the distance
const ZOOM_PIXELS_PER_DOUBLE = 200;
const PITCH_LIMIT = 1.5;
// An orbit stops just below level: the floor is not seen from under it.
const LOWEST_PITCH = -0.1;
const DISTANCE_RANGE = Object.freeze([0.3, 60]);
// The views' directions (where the camera stands from the target) and how far.
const VIEWS = Object.freeze({
  front: { yaw: -Math.PI / 2, pitch: 0 }, // from -y, looking along +y (Blender's numpad 1)
  side: { yaw: 0, pitch: 0 }, // from +x, looking along -x (numpad 3)
  top: { yaw: -Math.PI / 2, pitch: PITCH_LIMIT }, // from above (numpad 7)
});
const VIEW_DISTANCE = 3;

function clamp(value, low, high) {
  return Math.min(high, Math.max(low, value));
}

// The orbit a camera at `place` makes round `target`.
function orbitOf(place, target) {
  const offset = place.map((part, axis) => part - target[axis]);
  const distance = clamp(Math.hypot(...offset), ...DISTANCE_RANGE);
  return {
    target: [...target],
    yaw: Math.atan2(offset[1], offset[0]),
    pitch: clamp(
      Math.asin(clamp(offset[2] / (Math.hypot(...offset) || 1), -1, 1)),
      -PITCH_LIMIT,
      PITCH_LIMIT,
    ),
    distance,
  };
}

// The camera's place and direction for an orbit.
function poseOf(orbit) {
  const { target, yaw, pitch, distance } = orbit;
  const place = [
    target[0] + distance * Math.cos(pitch) * Math.cos(yaw),
    target[1] + distance * Math.cos(pitch) * Math.sin(yaw),
    target[2] + distance * Math.sin(pitch),
  ];
  const forward = target.map((part, axis) => part - place[axis]);
  return { place, forward };
}

// A drag of the middle button by (dx, dy) pixels: 'orbit', 'pan' or 'zoom'.
function dragged(orbit, how, dx, dy) {
  const next = { ...orbit, target: [...orbit.target] };
  if (how === 'zoom') {
    next.distance = clamp(orbit.distance * 2 ** (dy / ZOOM_PIXELS_PER_DOUBLE), ...DISTANCE_RANGE);
  } else if (how === 'pan') {
    // Right and up on the screen, as the camera sees them.
    const right = [-Math.sin(orbit.yaw), Math.cos(orbit.yaw), 0];
    const up = [
      -Math.sin(orbit.pitch) * Math.cos(orbit.yaw),
      -Math.sin(orbit.pitch) * Math.sin(orbit.yaw),
      Math.cos(orbit.pitch),
    ];
    // The scene follows the pointer: the focus moves the other way (the screen's y is down).
    const scale = orbit.distance * PAN_PER_PIXEL;
    for (let axis = 0; axis < 3; axis += 1) {
      next.target[axis] += (-right[axis] * dx + up[axis] * dy) * scale;
    }
  } else {
    // Up on the screen lifts the camera over the scene; it stays a little above the floor.
    next.yaw = orbit.yaw - dx * ORBIT_RADIANS_PER_PIXEL;
    next.pitch = clamp(orbit.pitch - dy * ORBIT_RADIANS_PER_PIXEL, LOWEST_PITCH, PITCH_LIMIT);
  }
  return next;
}

// A view (front, side, top) of the target.
function viewOf(name, target) {
  const view = VIEWS[name];
  return view
    ? { target: [...target], yaw: view.yaw, pitch: view.pitch, distance: VIEW_DISTANCE }
    : null;
}

module.exports = { orbitOf, poseOf, dragged, viewOf, VIEWS };
