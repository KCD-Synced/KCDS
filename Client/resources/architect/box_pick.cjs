'use strict';

// World Architect's picks of many things at once: what a box dragged on the stage holds (your
// things and the level's own) and what stands inside a building. Pure; the editor asks the game
// for the things and their places (editor.cjs selectInBox, selectAllInBuilding).

// How near two places of the level's same object are to be the same object (metres).
const SAME_PLACE = 0.05;

// The ground a box on the stage covers, from the points the camera sees at its corners and its
// middle (null where it sees none): their middle, and the radius on the ground that reaches every
// one of them grown by `margin`, at most `most`. Null when the camera sees no ground in the box.
function groundSpan(points, margin, most) {
  const found = points.filter(Boolean);
  if (found.length === 0) {
    return null;
  }
  const centre = {
    x: found.reduce((sum, point) => sum + point.x, 0) / found.length,
    y: found.reduce((sum, point) => sum + point.y, 0) / found.length,
    z: found.reduce((sum, point) => sum + point.z, 0) / found.length,
  };
  const reach = Math.max(
    ...found.map((point) => Math.hypot(point.x - centre.x, point.y - centre.y)),
  );
  return { centre, radius: Math.min(most, reach + margin) };
}

// Whether a stage point (null: behind the camera) lies in the stage box { minX, maxX, minY, maxY }.
function insideStageBox(point, box) {
  return Boolean(
    point &&
      point.x >= box.minX &&
      point.x <= box.maxX &&
      point.y >= box.minY &&
      point.y <= box.maxY,
  );
}

// Whether `at` lies inside `box` ({ at, turn, min, max }: its corners from its place `at` in its
// own turn, `turn` degrees about the up axis), grown by `margin` metres on every side.
function insideTurnedBox(box, at, margin = 0) {
  const angle = (-(box.turn ?? 0) * Math.PI) / 180;
  const cos = Math.cos(angle);
  const sin = Math.sin(angle);
  const dx = at.x - box.at.x;
  const dy = at.y - box.at.y;
  const own = [dx * cos - dy * sin, dx * sin + dy * cos, at.z - box.at.z];
  return own.every(
    (value, axis) => value >= box.min[axis] - margin && value <= box.max[axis] + margin,
  );
}

// Whether two selections are the same thing: one object, or the level's same object (its kind and
// name where it stands; the editor makes a new selection each time it finds one).
function sameThing(a, b) {
  if (a === b) {
    return true;
  }
  if (!a?.level || !b?.level) {
    return false;
  }
  return (
    a.object?.kind === b.object?.kind &&
    a.object?.name === b.object?.name &&
    Math.hypot(a.at.x - b.at.x, a.at.y - b.at.y) < SAME_PLACE
  );
}

// The group with `found` added to it (with `add`) or in its place, each thing once.
function mergeGroup(group, found, add) {
  const merged = add ? [...group] : [];
  for (const thing of found) {
    if (!merged.some((other) => sameThing(other, thing))) {
      merged.push(thing);
    }
  }
  return merged;
}

module.exports = { groundSpan, insideStageBox, insideTurnedBox, sameThing, mergeGroup, SAME_PLACE };
