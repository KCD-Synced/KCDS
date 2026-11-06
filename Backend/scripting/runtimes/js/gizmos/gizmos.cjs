'use strict';

// Debug shapes in the players' worlds: the server API's Gizmo. A gizmo is a line, a
// sphere, a box or a text the players' games draw with the renderer's aux geometry for a time (or
// until it is cleared); the server keeps nothing but which resource drew which id, so a stopped
// resource takes its gizmos with it.
//
// Display goes through these bridge ops ("Gizmos"):
//   { op: 'gizmoDraw', gizmo, shape, position, end | radius | size, rotation | text, textSize,
//     color: { r, g, b, a }, durationMs, players? }
//   { op: 'gizmoClear', gizmo, players? }  (gizmo 0 clears every gizmo)
// Without `players` every ready player draws it. Drawing an id that is drawn replaces it.

const { rotation, vector } = require('../math.cjs');

// A point farther out than this lies outside any level.
const WORLD_LIMIT = 1e6;
const DURATION_MAX_MS = 3600000;
const TEXT_MAX = 128;
const SIZE_MAX = 1000;
const TEXT_SIZE_MAX = 20;

function pointOf(value, name) {
  const point = vector(value ?? {});
  for (const axis of ['x', 'y', 'z']) {
    if (!Number.isFinite(point[axis]) || Math.abs(point[axis]) > WORLD_LIMIT) {
      throw new TypeError(`Gizmo ${name} must be a point { x, y, z }`);
    }
  }
  return { x: point.x, y: point.y, z: point.z };
}

function amountOf(value, fallback, max, name) {
  const amount = value ?? fallback;
  if (typeof amount !== 'number' || !Number.isFinite(amount) || amount <= 0 || amount > max) {
    throw new TypeError(`Gizmo ${name} must be a number above 0 and at most ${max}`);
  }
  return amount;
}

// A colour of 0 to 1 channels; opaque white when none is given.
function colorOf(value) {
  if (value === undefined) {
    return { r: 1, g: 1, b: 1, a: 1 };
  }
  const color = {};
  for (const channel of ['r', 'g', 'b', 'a']) {
    const level = value?.[channel] ?? (channel === 'a' ? 1 : 0);
    if (typeof level !== 'number' || !Number.isFinite(level) || level < 0 || level > 1) {
      throw new TypeError('Gizmo color channels r, g, b, a are 0 to 1');
    }
    color[channel] = level;
  }
  return color;
}

function durationOf(value) {
  const duration = value ?? 0;
  if (!Number.isSafeInteger(duration) || duration < 0 || duration > DURATION_MAX_MS) {
    throw new TypeError(`Gizmo durationMs must be 0 (until cleared) to ${DURATION_MAX_MS}`);
  }
  return duration;
}

// Printable text only, as the client draws it.
function textOf(value) {
  if (typeof value !== 'string' || value.length === 0) {
    throw new TypeError('Gizmo text must be a non-empty string');
  }
  return [...value]
    .map((character) => (character < ' ' ? ' ' : character))
    .join('')
    .slice(0, TEXT_MAX);
}

class Gizmos {
  // `host`: the resource host (bridge, players, the running resource).
  constructor(host) {
    this.host = host;
    this.nextId = 1;
    // gizmo id -> the name of the resource that drew it
    this.owners = new Map();
  }

  // The IDs of the players named in `players` (handles or ids), or null for every ready player.
  playersOf(players) {
    if (players === undefined) {
      return null;
    }
    if (!Array.isArray(players)) {
      throw new TypeError('Gizmo players must be an array of players or their IDs');
    }
    const ids = [];
    for (const player of players) {
      const id = typeof player === 'number' ? player : player?.id;
      if (!Number.isSafeInteger(id) || id <= 0) {
        throw new TypeError('Gizmo players must be players or their IDs');
      }
      ids.push(id);
    }
    return ids;
  }

  draw(shape, fields, options) {
    const gizmo = options?.id ?? this.nextId++;
    if (!Number.isSafeInteger(gizmo) || gizmo <= 0 || gizmo > 0xffffffff) {
      throw new TypeError('Gizmo id must be a whole number above 0');
    }
    const players = this.playersOf(options?.players);
    const op = {
      op: 'gizmoDraw',
      gizmo,
      shape,
      ...fields,
      color: colorOf(options?.color),
      durationMs: durationOf(options?.durationMs),
    };
    if (players) {
      op.players = players;
    }
    this.host.bridge.send(op);
    this.owners.set(gizmo, this.host.context?.getStore()?.name ?? null);
    return gizmo;
  }

  clear(gizmo, players) {
    const id = gizmo ?? 0;
    if (!Number.isSafeInteger(id) || id < 0 || id > 0xffffffff) {
      throw new TypeError('Gizmo id must be a whole number (0 or none clears every gizmo)');
    }
    const ids = this.playersOf(players);
    const op = { op: 'gizmoClear', gizmo: id };
    if (ids) {
      op.players = ids;
    }
    this.host.bridge.send(op);
    if (id === 0) {
      this.owners.clear();
    } else {
      this.owners.delete(id);
    }
  }

  // A stopped resource's gizmos are cleared for everyone.
  resourceStopped(name) {
    for (const [gizmo, owner] of [...this.owners]) {
      if (owner === name) {
        this.clear(gizmo);
      }
    }
  }

  define() {
    const registry = this;
    return Object.freeze({
      // A line from `from` to `to`. Options: color, durationMs (0: until cleared), players, id.
      line(from, to, options) {
        return registry.draw(
          'line',
          { position: pointOf(from, 'from'), end: pointOf(to, 'to') },
          options,
        );
      },
      // A sphere drawn as three great circles.
      sphere(center, radius, options) {
        return registry.draw(
          'sphere',
          {
            position: pointOf(center, 'center'),
            radius: amountOf(radius, 1, SIZE_MAX, 'radius'),
          },
          options,
        );
      },
      // A box of half sizes `size` ({ x, y, z }), turned by options.rotation.
      box(center, size, options) {
        const half = pointOf(size ?? { x: 0.5, y: 0.5, z: 0.5 }, 'size');
        for (const axis of ['x', 'y', 'z']) {
          amountOf(half[axis], 0.5, SIZE_MAX, 'size');
        }
        const turn = rotation(options?.rotation ?? { x: 0, y: 0, z: 0, w: 1 });
        return registry.draw(
          'box',
          {
            position: pointOf(center, 'center'),
            size: half,
            rotation: { x: turn.x, y: turn.y, z: turn.z, w: turn.w },
          },
          options,
        );
      },
      // A label at a point, at most 128 characters.
      text(position, text, options) {
        return registry.draw(
          'text',
          {
            position: pointOf(position, 'position'),
            text: textOf(text),
            textSize: amountOf(options?.textSize, 1.5, TEXT_SIZE_MAX, 'textSize'),
          },
          options,
        );
      },
      // Clears one gizmo, or every gizmo without an id; for `players` only, when given.
      clear(gizmo, players) {
        registry.clear(gizmo, players);
      },
    });
  }
}

module.exports = { Gizmos };
