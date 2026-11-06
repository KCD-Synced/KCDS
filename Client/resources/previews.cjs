'use strict';

// The client-side PropGhost and PropPlacer: previews this machine's own scripts draw
// and place, with nobody else seeing them and nothing replicated. They are the game client's
// previews of the server's PropGhost and PropPlacer (Client/runtime/props/local_previews.hpp)
// ordered from here:
//
//   { op: 'localGhost', kind: 'create' | 'pose' | 'tint' | 'hidden' | 'destroy', ghost, ... }
//   { op: 'localPlacer', kind: 'begin' | 'veto' | 'end' | 'rotate', session, ... }
//
// and a session's reports come back as { op: 'localPlacerReport', session, event, valid, pose }.
// A mesh is one of the shared prop catalog (props/catalog.cjs), as the server's previews take.
//
// Nothing here is authority: what gets built is the server's decision from the pose a `confirm`
// handler sends it. A ghost's box comes back as { op: 'localGhostBounds', ghost, min, max } when it
// changes (getBounds). Not covered: a pose's surface (always '').

const { resolveMesh } = require('../../Backend/scripting/runtimes/js/props/catalog.cjs');

const GHOSTS_MAX = 32;
const PIECES_MAX = 16;
const SCALE_MIN = 0.01;
const SCALE_MAX = 100;
const RANGE_MAX = 512;
const WORLD_LIMIT = 1e6;
const EVENTS = ['move', 'confirm', 'rejected', 'cancel', 'end'];
const VALID_TINT = { r: 0, g: 1, b: 0, a: 0.5 };
const INVALID_TINT = { r: 1, g: 0, b: 0, a: 0.5 };

const isNumber = (value) => typeof value === 'number' && Number.isFinite(value);

function clamp01(value) {
  return Math.min(1, Math.max(0, value));
}

// A Color as { r, g, b, a } in 0..1; null for none.
function tintOf(tint, what) {
  if (tint === undefined || tint === null) {
    return null;
  }
  const channels = [tint.r, tint.g, tint.b, tint.a ?? 1];
  if (!channels.every(isNumber)) {
    throw new TypeError(`${what} must be a Color`);
  }
  const [r, g, b, a] = channels.map(clamp01);
  return { r, g, b, a };
}

function scaleOf(scale = 1) {
  if (!isNumber(scale)) {
    throw new TypeError('A preview scale must be a number');
  }
  return Math.min(SCALE_MAX, Math.max(SCALE_MIN, scale));
}

// A ghost's scale: a number, or per axis a Vector3 ({ x, y, z }) whose axes are clamped as a
// number is.
function ghostScaleOf(scale = 1) {
  if (isNumber(scale)) {
    return scaleOf(scale);
  }
  if (scale !== null && typeof scale === 'object' && [scale.x, scale.y, scale.z].every(isNumber)) {
    return { x: scaleOf(scale.x), y: scaleOf(scale.y), z: scaleOf(scale.z) };
  }
  throw new TypeError('A preview scale must be a number or a Vector3');
}

// The message fields of a ghost's scale: `scale`, and `scaleAxes` for one per axis.
function scaleFields(scale) {
  if (typeof scale === 'number') {
    return { scale };
  }
  return { scale: 1, scaleAxes: { x: scale.x, y: scale.y, z: scale.z } };
}

function sameScale(a, b) {
  if (typeof a === 'number' || typeof b === 'number') {
    return a === b;
  }
  return a.x === b.x && a.y === b.y && a.z === b.z;
}

function pointOf(value, what) {
  const point = { x: value?.x, y: value?.y, z: value?.z };
  const inside = [point.x, point.y, point.z].every(
    (n) => isNumber(n) && Math.abs(n) <= WORLD_LIMIT,
  );
  if (!inside) {
    throw new RangeError(`${what} must be a Vector3 within the world`);
  }
  return point;
}

function turnOf(value) {
  if (value === undefined || value === null) {
    return { x: 0, y: 0, z: 0, w: 1 };
  }
  const turn = { x: value.x, y: value.y, z: value.z, w: value.w };
  if (![turn.x, turn.y, turn.z, turn.w].every(isNumber)) {
    throw new TypeError('A preview rotation must be a Quaternion');
  }
  return turn;
}

function meshOf(model) {
  if (typeof model !== 'string' || model === '') {
    throw new TypeError('A preview model is a mesh path of the prop catalog');
  }
  const mesh = resolveMesh(model);
  if (mesh === null) {
    throw new RangeError(`the prop catalog does not carry ${model}`);
  }
  return mesh;
}

// The meshes of `model` or `models` with the offsets and rotations that line up with them.
function piecesOf(options) {
  const names = options.models ?? [options.model];
  if (!Array.isArray(names) || names.length === 0 || names.length > PIECES_MAX) {
    throw new TypeError(`A preview needs a model, or up to ${PIECES_MAX} models`);
  }
  const models = names.map(meshOf);
  const offsets = (options.offsets ?? []).slice(0, models.length).map((o) => pointOf(o, 'offset'));
  const rotations = (options.rotations ?? []).slice(0, models.length).map(turnOf);
  return { models, offsets, rotations };
}

function numberOption(options, name, fallback, low, high) {
  const value = options[name] ?? fallback;
  if (!isNumber(value) || value < low || value > high) {
    throw new RangeError(`PropPlacer.begin ${name} must be a number from ${low} to ${high}`);
  }
  return value;
}

function flagOption(options, name, fallback) {
  const value = options[name] ?? fallback;
  if (typeof value !== 'boolean') {
    throw new TypeError(`PropPlacer.begin ${name} must be true or false`);
  }
  return value;
}

class ResourcePreviews {
  // send: to the game client; Vector3 and Quaternion: the SDK's classes for the poses.
  constructor({ send, Vector3, Quaternion, log = () => {} }) {
    this.send = send;
    this.Vector3 = Vector3;
    this.Quaternion = Quaternion;
    this.log = log;
    // ghost number -> { owner, valid }
    this.ghosts = new Map();
    this.nextGhost = 1;
    // The running session: { number, owner, pose, valid }, or null.
    this.session = null;
    this.nextSession = 1;
    // owner -> event -> Set of handlers
    this.handlers = new Map();
  }

  createGhost(PropGhost, owner, options) {
    if (options === null || typeof options !== 'object') {
      throw new TypeError('PropGhost.create takes an options object');
    }
    return this.createGhostOf(PropGhost, owner, piecesOf(options), {
      ...options,
      seeThrough: false,
    });
  }

  // World Architect's previews (Client/resources/architect/editor.cjs): any of the game's own
  // meshes or characters, not only the prop catalog's (a door's .cdf), as the game client's
  // ghosts load them (props/ghosts.hpp spawn_mesh). Not offered to resource scripts.
  createEditorGhost(PropGhost, owner, options) {
    const meshes = options.models;
    const gameMesh = /^objects[\\/][a-z0-9_\-./\\ ]+\.(cgf|cdf|chr)$/i;
    if (!Array.isArray(meshes) || meshes.length === 0 || meshes.length > PIECES_MAX) {
      throw new TypeError(`An editor preview needs up to ${PIECES_MAX} meshes`);
    }
    for (const mesh of meshes) {
      if (typeof mesh !== 'string' || !gameMesh.test(mesh) || mesh.includes('..')) {
        throw new TypeError(`An editor preview's mesh is a game path: ${mesh}`);
      }
    }
    const offsets = (options.offsets ?? [])
      .slice(0, meshes.length)
      .map((o) => pointOf(o, 'offset'));
    const rotations = (options.rotations ?? []).slice(0, meshes.length).map(turnOf);
    const pieces = {
      models: meshes.map((mesh) => mesh.replace(/\\/g, '/').toLowerCase()),
      offsets,
      rotations,
    };
    return this.createGhostOf(PropGhost, owner, pieces, { ...options, seeThrough: true });
  }

  createGhostOf(PropGhost, owner, pieces, options) {
    const { models, offsets, rotations } = pieces;
    const position = pointOf(options.position, 'PropGhost.create position');
    const rotation = turnOf(options.rotation);
    const scale = ghostScaleOf(options.scale);
    const tint = tintOf(options.tint, 'PropGhost.create tint');
    const live = [...this.ghosts.values()].filter((ghost) => ghost.valid).length;
    if (live >= GHOSTS_MAX) {
      return null;
    }
    const number = this.nextGhost;
    this.nextGhost += 1;
    this.ghosts.set(number, { owner, valid: true });
    this.send({
      op: 'localGhost',
      kind: 'create',
      ghost: number,
      models,
      offsets,
      rotations,
      position,
      rotation,
      ...scaleFields(scale),
      tint,
      hidden: false,
      seeThrough: options.seeThrough === true,
    });
    return new PropGhost(number);
  }

  ghostCall(number, message) {
    const ghost = this.ghosts.get(number);
    if (!ghost || !ghost.valid) {
      return false;
    }
    this.send({ op: 'localGhost', ghost: number, ...message });
    return true;
  }

  destroyGhost(number) {
    const done = this.ghostCall(number, { kind: 'destroy' });
    if (done) {
      this.ghosts.get(number).valid = false;
    }
    return done;
  }

  begin(owner, options) {
    if (this.session) {
      return false;
    }
    if (options === null || typeof options !== 'object') {
      throw new TypeError('PropPlacer.begin takes an options object');
    }
    const rules = options.rules ?? {};
    if (rules === null || typeof rules !== 'object') {
      throw new TypeError('PropPlacer.begin rules must be an object');
    }
    const { models, offsets, rotations } = piecesOf(options);
    const rule = (name) => (rules[name] === undefined ? -1 : numberOption(rules, name, -1, 0, 1e6));
    const message = {
      op: 'localPlacer',
      kind: 'begin',
      session: this.nextSession,
      models,
      offsets,
      rotations,
      scale: scaleOf(options.scale),
      validTint: tintOf(options.validTint ?? VALID_TINT, 'PropPlacer.begin validTint'),
      invalidTint: tintOf(options.invalidTint ?? INVALID_TINT, 'PropPlacer.begin invalidTint'),
      range: numberOption(options, 'range', 40, 0.5, RANGE_MAX),
      fallbackDistance: numberOption(options, 'fallbackDistance', 3, 0, RANGE_MAX),
      verticalOffset: numberOption(options, 'verticalOffset', 0, -100, 100),
      yaw: numberOption(options, 'yaw', 0, -360, 360),
      alignToSurface: flagOption(options, 'alignToSurface', false),
      faceAwayFromPlayer: flagOption(options, 'faceAwayFromPlayer', true),
      keepOpen: flagOption(options, 'keepOpen', true),
      requireSurface: flagOption(rules, 'requireSurface', true),
      maxSlope: rule('maxSlope'),
      minDistance: rule('minDistance'),
      maxDistance: rule('maxDistance'),
    };
    this.session = { number: this.nextSession, owner, pose: null, valid: false };
    this.nextSession += 1;
    this.send(message);
    return true;
  }

  // Ends the running session; `end` is raised when the game client confirms it.
  end(owner) {
    const session = this.session;
    if (!session || (owner !== undefined && session.owner !== owner)) {
      return;
    }
    this.send({ op: 'localPlacer', kind: 'end', session: session.number });
    this.finish();
  }

  finish() {
    const session = this.session;
    this.session = null;
    if (session) {
      this.raise(session.owner, 'end', null, false);
    }
  }

  placerCall(message) {
    if (!this.session) {
      return false;
    }
    this.send({ op: 'localPlacer', session: this.session.number, ...message });
    return true;
  }

  on(owner, event, handler) {
    if (!EVENTS.includes(event)) {
      throw new TypeError(`PropPlacer.on takes one of ${EVENTS.join(', ')}`);
    }
    if (typeof handler !== 'function') {
      throw new TypeError('PropPlacer.on takes a handler function');
    }
    const events = this.handlers.get(owner) ?? new Map();
    this.handlers.set(owner, events);
    const set = events.get(event) ?? new Set();
    events.set(event, set);
    set.add(handler);
    return () => set.delete(handler);
  }

  raise(owner, event, pose, valid) {
    const set = this.handlers.get(owner)?.get(event);
    for (const handler of set ? [...set] : []) {
      try {
        handler(pose, valid);
      } catch (error) {
        this.log(owner, String(error));
      }
    }
  }

  poseOf(raw) {
    if (!raw || typeof raw !== 'object') {
      return null;
    }
    const { Vector3, Quaternion } = this;
    const v = (p) => new Vector3(Number(p?.x), Number(p?.y), Number(p?.z));
    const q = raw.rotation ?? {};
    return {
      position: v(raw.position),
      rotation: new Quaternion(Number(q.w), Number(q.x), Number(q.y), Number(q.z)),
      normal: v(raw.normal),
      surface: '',
      distance: Number(raw.distance),
      onSurface: raw.onSurface === true,
    };
  }

  // { op: 'localGhostBounds', ghost, min, max } from the game client: the box the ghost's pieces
  // take in the world, or none (no min and max) while hidden or loading.
  receiveBounds({ ghost, min, max }) {
    const known = this.ghosts.get(ghost);
    if (!known || !known.valid) {
      return;
    }
    const point = (value) =>
      value && isNumber(value.x) && isNumber(value.y) && isNumber(value.z)
        ? { x: value.x, y: value.y, z: value.z }
        : null;
    const low = point(min);
    const high = point(max);
    known.bounds = low && high ? { min: low, max: high } : null;
  }

  boundsOf(number) {
    const known = this.ghosts.get(number);
    if (!known || !known.valid || !known.bounds) {
      return null;
    }
    return { min: { ...known.bounds.min }, max: { ...known.bounds.max } };
  }

  // { op: 'localPlacerReport', session, event, valid, pose } from the game client.
  receiveReport({ session, event, valid, pose }) {
    const running = this.session;
    if (!running || running.number !== session || !EVENTS.includes(event)) {
      return;
    }
    if (event === 'end') {
      this.finish();
      return;
    }
    const placed = event === 'cancel' ? null : this.poseOf(pose);
    if (placed) {
      running.pose = placed;
      running.valid = valid === true;
    }
    this.raise(running.owner, event, placed, valid === true);
  }

  // The resource stopped: its session ends and its ghosts go.
  removeOwner(owner) {
    if (this.session?.owner === owner) {
      this.end(owner);
    }
    for (const [number, ghost] of this.ghosts) {
      if (ghost.owner === owner && ghost.valid) {
        this.destroyGhost(number);
      }
    }
    this.handlers.delete(owner);
  }

  // A level load takes every ghost and the session with it.
  levelChanged() {
    for (const ghost of this.ghosts.values()) {
      ghost.valid = false;
    }
    if (this.session) {
      this.finish();
    }
  }

  facade(owner) {
    const previews = this;
    class PropGhost {
      constructor(id) {
        if (!previews.ghosts.has(id)) {
          throw new Error('Unknown ghost');
        }
        Object.defineProperty(this, 'id', { value: id, enumerable: true });
      }
      get valid() {
        return previews.ghosts.get(this.id)?.valid === true;
      }
      toString() {
        return `PropGhost(${this.id}${this.valid ? '' : ', destroyed'})`;
      }
      setPose(position, rotation, scale) {
        return previews.ghostCall(this.id, {
          kind: 'pose',
          position: pointOf(position, 'PropGhost.setPose position'),
          rotation: turnOf(rotation),
          ...scaleFields(ghostScaleOf(scale)),
        });
      }
      setTint(tint) {
        return previews.ghostCall(this.id, {
          kind: 'tint',
          tint: tintOf(tint, 'PropGhost.setTint tint'),
        });
      }
      setHidden(hidden) {
        if (typeof hidden !== 'boolean') {
          throw new TypeError('PropGhost.setHidden takes true or false');
        }
        return previews.ghostCall(this.id, { kind: 'hidden', hidden });
      }
      // The box the ghost takes in the world, { min, max }, as the game last reported it (a frame
      // after a change); null before its meshes stand, while hidden and once destroyed.
      getBounds() {
        return previews.boundsOf(this.id);
      }
      destroy() {
        return previews.destroyGhost(this.id);
      }
      static create(options) {
        return previews.createGhost(PropGhost, owner, options);
      }
      static destroyAll() {
        for (const [number, ghost] of previews.ghosts) {
          if (ghost.valid) {
            previews.destroyGhost(number);
          }
        }
      }
    }
    // The editor's previews (createEditorGhost); not part of the scripts' PropGhost.
    PropGhost.editorPreview = (options) => previews.createEditorGhost(PropGhost, owner, options);
    const PropPlacer = Object.freeze({
      begin(options) {
        return previews.begin(owner, options);
      },
      end() {
        previews.end(owner);
      },
      isActive() {
        return previews.session !== null;
      },
      setRefusal(vetoed) {
        if (typeof vetoed !== 'boolean') {
          throw new TypeError('PropPlacer.setRefusal takes true or false');
        }
        return previews.placerCall({ kind: 'veto', vetoed });
      },
      rotate(degrees) {
        if (!isNumber(degrees)) {
          throw new TypeError('PropPlacer.rotate takes a number of degrees');
        }
        return previews.placerCall({ kind: 'rotate', degrees });
      },
      getPose() {
        return previews.session?.pose ?? null;
      },
      on(event, handler) {
        return previews.on(owner, event, handler);
      },
    });
    return { PropGhost, PropPlacer };
  }
}

module.exports = { ResourcePreviews };
