'use strict';

// The numbers behind a siege engine: what each kind can reach, how long its steps
// take, and the arc a shot flies. Pure functions, so the rules stay testable without a host.

const GRAVITY = 9.81;
const DEGREE = Math.PI / 180;

// Per kind, at speed 1: ranges and heights in metres, times in seconds.
//   launch: the stone leaves the engine this far above its base.
//   release: from the start of a shot until the stone is let go.
//   load: the whole reload; a trebuchet spends `draw` of it winching the arm down first.
//   angle (trebuchet): the arm lets go at one angle, so the speed is solved for the distance.
//   muzzle (cannon): it fires at one speed, so the elevation is solved for the distance.
const KINDS = Object.freeze({
  trebuchet: {
    crewSeats: 2,
    minRange: 25,
    maxRange: 250,
    defaultRange: 150,
    launch: 6,
    release: 3.5,
    load: 110,
    draw: 0.4,
    angle: 45 * DEGREE,
  },
  cannon: {
    crewSeats: 0,
    minRange: 10,
    maxRange: 200,
    defaultRange: 120,
    launch: 1.5,
    release: 0.6,
    load: 12,
    draw: 0,
    muzzle: 95,
  },
});

const horizontal = (a, b) => Math.hypot(b.x - a.x, b.y - a.y);

// Why an engine of this kind cannot throw `distance` metres, as a phrase, or '' when it can.
function rangeProblem(spec, distance) {
  if (distance < spec.minRange) {
    return 'cannot fire: the target is closer than the engine can throw';
  }
  if (distance > spec.maxRange) {
    return 'cannot fire: the target is further than the engine can throw';
  }
  return '';
}

// The arc of a shot from `from` to `to` (both { x, y, z }): how long it flies and the height it
// climbs above the launch point, which is all a client needs to draw it along a parabola.
function arcOf(spec, from, to) {
  const distance = horizontal(from, to);
  const drop = from.z - to.z;
  let angle = spec.angle;
  let speed;
  if (angle !== undefined) {
    // Trebuchet: fixed angle, speed from  d = v cos(a) t  and  drop = v sin(a) t - g t^2 / 2.
    const denominator = 2 * Math.cos(angle) ** 2 * (distance * Math.tan(angle) + drop);
    speed = Math.sqrt((GRAVITY * distance * distance) / Math.max(denominator, 1e-6));
  } else {
    // Cannon: fixed speed, the low elevation that lands the shot (clamped when out of reach).
    speed = spec.muzzle;
    const reach = (GRAVITY * distance) / (speed * speed);
    angle = 0.5 * Math.asin(Math.min(1, Math.max(0, reach)));
  }
  const climb = speed * Math.sin(angle);
  const flight = distance / Math.max(speed * Math.cos(angle), 1e-6);
  return { flight, apex: (climb * climb) / (2 * GRAVITY), angle, speed };
}

// Health taken at `distance` from the point of impact: all of `damage` at the centre, a quarter at
// the edge of `radius`, nothing beyond it.
function shareOf(damage, radius, distance) {
  if (!(damage > 0) || distance > radius) {
    return 0;
  }
  return damage * (1 - 0.75 * Math.min(1, distance / Math.max(radius, 1e-6)));
}

// Whether a reported landing point lies on the thrown arc: close to the ground line from the
// engine to the target (the world may stop a shot short, but not sideways) and not past it.
function onArc(from, to, point, tolerance = 5) {
  const dx = to.x - from.x;
  const dy = to.y - from.y;
  const length2 = dx * dx + dy * dy;
  const t = length2 === 0 ? 0 : ((point.x - from.x) * dx + (point.y - from.y) * dy) / length2;
  const along = Math.max(0, Math.min(1, t));
  const nearest = { x: from.x + dx * along, y: from.y + dy * along };
  const slack = tolerance / Math.max(Math.sqrt(length2), 1);
  return (
    t >= -slack &&
    t <= 1 + slack &&
    Math.hypot(point.x - nearest.x, point.y - nearest.y) <= tolerance
  );
}

// The quaternion { x, y, z, w } that turns an engine about the vertical to face (dx, dy).
function facingOf(dx, dy) {
  const yaw = Math.atan2(-dx, dy);
  return { x: 0, y: 0, z: Math.sin(yaw / 2), w: Math.cos(yaw / 2) };
}

module.exports = { KINDS, arcOf, facingOf, horizontal, onArc, rangeProblem, shareOf };
