'use strict';

// Prop previews: PropGhost and PropPlacer. Both are shown to one player only and
// are not props: nothing is replicated to anyone else and nothing survives the player's level
// load. The server holds the state and sends a bridge op only when something changes (no timers).
//
// Ghost: a translucent copy of one or more catalog meshes standing in one player's world.
// Server -> client ops (`ghost` is a number the server allocates, unique per run):
//   { op: 'ghostCreate', id, ghost, models, offsets, rotations, position, rotation, scale, tint,
//       hidden }       PropGhost.create(...) with the fields of the client API; tint is
//                      { r, g, b, a } or null (the meshes keep their own materials)
//   { op: 'ghostPose', id, ghost, position, rotation, scale }     PropGhost.setPose
//   { op: 'ghostTint', id, ghost, tint }                          PropGhost.setTint
//   { op: 'ghostHidden', id, ghost, hidden }                      PropGhost.setHidden
//   { op: 'ghostDestroy', id, ghost }                             PropGhost.destroy
//
// Placer: the player aims a ghost with their crosshair and confirms or cancels. The aiming loop
// is the player's own game (PropPlacer); the server hears about the end of it, and what is built
// is decided here again from the confirmed pose, never taken from the client.
// Server -> client ops:
//   { op: 'placerBegin', id, session, models, offsets, rotations, scale, validTint, invalidTint,
//       range, fallbackDistance, verticalOffset, yaw, alignToSurface, faceAwayFromPlayer,
//       keepOpen, rules, reportMoves }     PropPlacer.begin(...); a field left out stays out
//   { op: 'placerVeto', id, session, vetoed }                     PropPlacer.setRefusal
//   { op: 'placerEnd', id, session }                              PropPlacer.end
// Client -> server op, once per event the session raises:
//   { op: 'placerReport', id, session, event: 'move' | 'confirm' | 'cancel' | 'end',
//       valid?, pose?: { position, rotation, normal, surface, distance, onSurface } }
//   `move` is only sent when the session asked for it (reportMoves); `confirm` carries the pose
//   the click was accepted at; `end` is always the last report and carries no pose.

const { meshOf, physicsOf, isPrefab, WORLD_LIMIT } = require('./props.cjs');
const { Quaternion, Vector3 } = require('../math.cjs');

const SCALE_MIN = 0.01;
const SCALE_MAX = 100;
const GHOSTS_PER_PLAYER = 32;
const RANGE_MAX = 512;
// A confirm may sit this far outside a distance rule: the player kept walking after the click.
const DISTANCE_SLACK = 1.5;
const EVENTS = new Set(['move', 'confirm', 'cancel', 'end']);
const VALID_TINT = { r: 0, g: 1, b: 0, a: 0.5 };
const INVALID_TINT = { r: 1, g: 0, b: 0, a: 0.5 };
const RULE_NUMBERS = ['maxSlope', 'minDistance', 'maxDistance', 'clearance'];
const OPTION_NUMBERS = ['range', 'fallbackDistance', 'verticalOffset', 'yaw'];
const OPTION_FLAGS = ['alignToSurface', 'faceAwayFromPlayer', 'keepOpen'];

const isNumber = (value) => typeof value === 'number' && Number.isFinite(value);
const sameVector = (a, b) => a.x === b.x && a.y === b.y && a.z === b.z;
const sameQuaternion = (a, b) => a.w === b.w && sameVector(a, b);

function sameTint(a, b) {
  if (a === null || b === null) {
    return a === b;
  }
  return a.r === b.r && a.g === b.g && a.b === b.b && a.a === b.a;
}

function clamp01(value) {
  return Math.min(1, Math.max(0, value));
}

// A tint as { r, g, b, a } with each channel in 0..1; null (or undefined) for none.
function tintOf(tint) {
  if (tint === undefined || tint === null) {
    return null;
  }
  const channels = [tint.r, tint.g, tint.b, tint.a ?? 1];
  if (!channels.every(isNumber)) {
    throw new TypeError('A tint must be a Color');
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
    throw new RangeError(`Invalid ${what}`);
  }
  return point;
}

function turnOf(value) {
  const turn = { x: value?.x, y: value?.y, z: value?.z, w: value?.w };
  if (![turn.x, turn.y, turn.z, turn.w].every(isNumber)) {
    throw new TypeError('A preview rotation must be a Quaternion');
  }
  return turn;
}

// The meshes of a preview from `model` or `models`, with the per-piece offsets and rotations
// that line up with them.
function piecesOf(options) {
  const names = options.models ?? [options.model];
  if (!Array.isArray(names) || names.length === 0 || names.length > 16) {
    throw new TypeError('A preview needs a model, or up to 16 models');
  }
  const models = names.map(meshOf);
  if (models.some(isPrefab)) {
    throw new TypeError('A preview shows meshes; a game prefab has no preview');
  }
  const offsets = (options.offsets ?? []).slice(0, models.length).map((o) => pointOf(o, 'offset'));
  const rotations = (options.rotations ?? []).slice(0, models.length).map(turnOf);
  return { models, offsets, rotations };
}

function distance(a, b) {
  return Math.hypot(a.x - b.x, a.y - b.y, a.z - b.z);
}

class PropPreviews {
  // `host`: the resource host (bridge, emit, players, props).
  constructor(host) {
    this.host = host;
    // ghost number -> ghost state
    this.ghosts = new Map();
    this.nextGhost = 1;
    // player -> session state
    this.sessions = new Map();
    this.nextSession = 1;
  }

  // The PropGhost and PropPlacer classes of the server API.
  define() {
    const registry = this;
    class PropGhost {
      constructor(state) {
        this.state = state;
      }
      static create(player, options = {}) {
        return registry.createGhost(PropGhost, player, options);
      }
      static all(player) {
        return [...registry.ghosts.values()]
          .filter((state) => !player || state.player === player)
          .map((state) => state.handle);
      }
      static destroyAll(player) {
        for (const state of [...registry.ghosts.values()]) {
          if (!player || state.player === player) {
            registry.destroyGhost(state);
          }
        }
      }
      get id() {
        return this.state.ghost;
      }
      get player() {
        return this.state.player;
      }
      // False once the ghost is destroyed or its player left.
      get valid() {
        return registry.ghosts.get(this.state.ghost) === this.state;
      }
      get model() {
        return this.state.models[0];
      }
      get position() {
        const { x, y, z } = this.state.position;
        return new Vector3(x, y, z);
      }
      get rotation() {
        const { w, x, y, z } = this.state.rotation;
        return new Quaternion(w, x, y, z);
      }
      // A number, or a Vector3 for a ghost scaled per axis.
      get scale() {
        const { scale } = this.state;
        return typeof scale === 'number' ? scale : new Vector3(scale.x, scale.y, scale.z);
      }
      get hidden() {
        return this.state.hidden;
      }
      // Moves the ghost; a field left out keeps its value. Returns true while the ghost stands.
      setPose(position, rotation, scale) {
        return registry.poseGhost(this.state, { position, rotation, scale });
      }
      // Washes the ghost in a colour; null puts its own materials back.
      setTint(tint) {
        return registry.tintGhost(this.state, tintOf(tint));
      }
      // The valid or invalid tint the ghost was created with.
      setValid(valid) {
        const state = this.state;
        return registry.tintGhost(state, valid === true ? state.validTint : state.invalidTint);
      }
      setHidden(hidden) {
        return registry.hideGhost(this.state, hidden === true);
      }
      destroy() {
        return registry.destroyGhost(this.state);
      }
      toString() {
        return `PropGhost(${this.id}, ${this.model}, ${this.valid ? 'standing' : 'gone'})`;
      }
    }

    class PropPlacerSession {
      constructor(state) {
        this.state = state;
      }
      get id() {
        return this.state.session;
      }
      get player() {
        return this.state.player;
      }
      get active() {
        return registry.sessions.get(this.state.player) === this.state;
      }
      get model() {
        return this.state.models[0];
      }
      // Refuses every spot at the player's game until cleared.
      veto(vetoed = true) {
        return registry.veto(this.state, vetoed === true);
      }
      end() {
        return registry.endSession(this.state, true);
      }
      toString() {
        return `PropPlacer(${this.id}, ${this.model}, ${this.active ? 'running' : 'ended'})`;
      }
    }

    const PropPlacer = {
      // Starts placement for a player; returns the session, or null while they already have one.
      begin(player, options = {}) {
        return registry.begin(PropPlacerSession, player, options);
      },
      // The player's running session, or null.
      get(player) {
        return registry.sessions.get(player)?.handle ?? null;
      },
    };
    return { PropGhost, PropPlacer };
  }

  // The player must have a body in the world for a preview to stand in.
  requirePlayer(player) {
    if (!player?.ready) {
      throw new Error('The player is not in the world');
    }
  }

  createGhost(PropGhost, player, options) {
    this.requirePlayer(player);
    const { models, offsets, rotations } = piecesOf(options);
    const ghosts = [...this.ghosts.values()].filter((state) => state.player === player);
    if (ghosts.length >= GHOSTS_PER_PLAYER) {
      throw new RangeError('The player already has the most previews');
    }
    const position = pointOf(options.position, 'preview position');
    const rotation = options.rotation ? turnOf(options.rotation) : { x: 0, y: 0, z: 0, w: 1 };
    const state = {
      ghost: this.nextGhost++,
      player,
      resource: this.host.context.getStore()?.name ?? null,
      models,
      offsets,
      rotations,
      position,
      rotation,
      scale: ghostScaleOf(options.scale),
      tint: tintOf(options.tint),
      hidden: false,
      validTint: tintOf(options.validTint ?? VALID_TINT),
      invalidTint: tintOf(options.invalidTint ?? INVALID_TINT),
    };
    state.handle = new PropGhost(state);
    this.ghosts.set(state.ghost, state);
    this.host.bridge.send(this.createMessage(state));
    return state.handle;
  }

  // The ghostCreate op for a preview as it stands now.
  createMessage(state) {
    return {
      op: 'ghostCreate',
      id: state.player.id,
      ghost: state.ghost,
      models: state.models,
      offsets: state.offsets,
      rotations: state.rotations,
      position: state.position,
      rotation: state.rotation,
      ...scaleFields(state.scale),
      tint: state.tint,
      hidden: state.hidden,
    };
  }

  // The player's world became ready: their game lost its previews and placement, so it is told of
  // them again.
  arrived(player) {
    for (const state of this.ghosts.values()) {
      if (state.player === player) this.host.bridge.send(this.createMessage(state));
    }
    const session = this.sessions.get(player);
    if (!session) return;
    this.host.bridge.send(session.message);
    if (session.vetoed) {
      this.host.bridge.send({
        op: 'placerVeto',
        id: player.id,
        session: session.session,
        vetoed: true,
      });
    }
  }

  standing(state) {
    return this.ghosts.get(state.ghost) === state;
  }

  poseGhost(state, { position, rotation, scale }) {
    if (!this.standing(state)) {
      return false;
    }
    const nextPosition = position ? pointOf(position, 'preview position') : state.position;
    const nextRotation = rotation ? turnOf(rotation) : state.rotation;
    const nextScale = scale === undefined ? state.scale : ghostScaleOf(scale);
    const unchanged =
      sameVector(nextPosition, state.position) &&
      sameQuaternion(nextRotation, state.rotation) &&
      sameScale(nextScale, state.scale);
    if (!unchanged) {
      Object.assign(state, { position: nextPosition, rotation: nextRotation, scale: nextScale });
      this.host.bridge.send({
        op: 'ghostPose',
        id: state.player.id,
        ghost: state.ghost,
        position: nextPosition,
        rotation: nextRotation,
        ...scaleFields(nextScale),
      });
    }
    return true;
  }

  tintGhost(state, tint) {
    if (!this.standing(state)) {
      return false;
    }
    if (!sameTint(tint, state.tint)) {
      state.tint = tint;
      this.host.bridge.send({ op: 'ghostTint', id: state.player.id, ghost: state.ghost, tint });
    }
    return true;
  }

  hideGhost(state, hidden) {
    if (!this.standing(state)) {
      return false;
    }
    if (hidden !== state.hidden) {
      state.hidden = hidden;
      this.host.bridge.send({ op: 'ghostHidden', id: state.player.id, ghost: state.ghost, hidden });
    }
    return true;
  }

  // `silent`: the player's game is gone already, so there is nobody to tell.
  destroyGhost(state, silent = false) {
    if (!this.standing(state)) {
      return false;
    }
    this.ghosts.delete(state.ghost);
    if (!silent) {
      this.host.bridge.send({ op: 'ghostDestroy', id: state.player.id, ghost: state.ghost });
    }
    return true;
  }

  // The rules the player's game tints the preview by, checked on the server again at a confirm.
  rulesOf(rules = {}) {
    const checked = {};
    for (const name of RULE_NUMBERS) {
      if (rules[name] === undefined) continue;
      if (!isNumber(rules[name]) || rules[name] < 0) {
        throw new RangeError(`Placement rule ${name} must be a number from 0`);
      }
      checked[name] = rules[name];
    }
    if (rules.surfaces !== undefined) {
      if (!Array.isArray(rules.surfaces) || !rules.surfaces.every((s) => typeof s === 'string')) {
        throw new TypeError('Placement rule surfaces must be a list of surface names');
      }
      checked.surfaces = [...rules.surfaces];
    }
    if (rules.clearanceClasses !== undefined) {
      const classes = rules.clearanceClasses;
      if (!Array.isArray(classes) || !classes.every((c) => typeof c === 'string')) {
        throw new TypeError('Placement rule clearanceClasses must be a list of class names');
      }
      checked.clearanceClasses = [...classes];
    }
    if (rules.requireSurface !== undefined) {
      checked.requireSurface = rules.requireSurface === true;
    }
    return checked;
  }

  begin(PropPlacerSession, player, options) {
    this.requirePlayer(player);
    if (this.sessions.has(player)) {
      return null;
    }
    const { models, offsets, rotations } = piecesOf(options);
    // `spawn` makes a confirm build a real Prop of the first mesh, with this collision.
    const spawn = options.spawn ? physicsOf(options.spawn.physics ?? 'static') : null;
    if (spawn && models.length > 1) {
      throw new TypeError('A placement that spawns a prop takes one model');
    }
    const message = {
      op: 'placerBegin',
      id: player.id,
      session: this.nextSession++,
      models,
      offsets,
      rotations,
      scale: scaleOf(options.scale),
      reportMoves: options.reportMoves === true,
      rules: this.rulesOf(options.rules),
    };
    if (options.validTint) message.validTint = tintOf(options.validTint);
    if (options.invalidTint) message.invalidTint = tintOf(options.invalidTint);
    for (const name of OPTION_NUMBERS) {
      if (options[name] === undefined) continue;
      if (!isNumber(options[name])) {
        throw new TypeError(`Placement option ${name} must be a number`);
      }
      message[name] =
        name === 'range' ? Math.min(RANGE_MAX, Math.max(1, options[name])) : options[name];
    }
    for (const name of OPTION_FLAGS) {
      if (options[name] !== undefined) message[name] = options[name] === true;
    }
    const state = {
      session: message.session,
      message,
      vetoed: false,
      player,
      resource: this.host.context.getStore()?.name ?? null,
      models,
      scale: message.scale,
      rules: message.rules,
      spawn,
      reportMoves: message.reportMoves,
    };
    state.handle = new PropPlacerSession(state);
    this.sessions.set(player, state);
    this.host.bridge.send(message);
    return state.handle;
  }

  veto(state, vetoed) {
    if (this.sessions.get(state.player) !== state) {
      return false;
    }
    state.vetoed = vetoed === true;
    this.host.bridge.send({
      op: 'placerVeto',
      id: state.player.id,
      session: state.session,
      vetoed,
    });
    return true;
  }

  // Ends a session and tells the scripts. `tellClient`: the player's game is still up and has
  // not ended it itself.
  endSession(state, tellClient) {
    if (this.sessions.get(state.player) !== state) {
      return false;
    }
    this.sessions.delete(state.player);
    if (tellClient) {
      this.host.bridge.send({ op: 'placerEnd', id: state.player.id, session: state.session });
    }
    this.host.emit('propPlaceEnd', state.player, state.handle);
    return true;
  }

  // placerReport: an event of the player's running session.
  report(player, e) {
    const state = player && this.sessions.get(player);
    if (!state || e?.session !== state.session || !EVENTS.has(e.event)) {
      return;
    }
    if (e.event === 'end') {
      this.endSession(state, false);
    } else if (e.event === 'cancel') {
      this.host.emit('propPlaceCancel', player, state.handle);
    } else if (e.event === 'move') {
      this.reportMove(player, state, e);
    } else {
      this.confirm(player, state, e.pose);
    }
  }

  // The pose a report carries, or null when it is not one a level could hold.
  poseOf(pose) {
    try {
      return {
        position: pointOf(pose?.position, 'placement position'),
        rotation: turnOf(pose?.rotation),
        normal: pointOf(pose?.normal ?? { x: 0, y: 0, z: 1 }, 'placement normal'),
        surface: typeof pose?.surface === 'string' ? pose.surface.slice(0, 64) : '',
        distance: isNumber(pose?.distance) ? pose.distance : 0,
        onSurface: pose?.onSurface === true,
      };
    } catch {
      return null;
    }
  }

  // The pose as the scripts see it.
  placementOf(state, pose) {
    const { w, x, y, z } = pose.rotation;
    return {
      model: state.models[0],
      position: new Vector3(pose.position.x, pose.position.y, pose.position.z),
      rotation: new Quaternion(w, x, y, z),
      scale: state.scale,
      normal: new Vector3(pose.normal.x, pose.normal.y, pose.normal.z),
      surface: pose.surface,
      onSurface: pose.onSurface,
    };
  }

  reportMove(player, state, e) {
    const pose = state.reportMoves ? this.poseOf(e.pose) : null;
    if (pose) {
      this.host.emit(
        'propPlaceMove',
        player,
        state.handle,
        this.placementOf(state, pose),
        e.valid === true,
      );
    }
  }

  // Decides again what the player's game decided: the pose must be a possible one, inside the
  // distance rules, and no `propPlaceConfirm` handler may return false. Then the prop is built.
  confirm(player, state, reported) {
    const pose = this.poseOf(reported);
    if (!pose || !this.withinRules(player, state.rules, pose)) {
      this.host.emit(
        'propPlaceRejected',
        player,
        state.handle,
        pose && this.placementOf(state, pose),
      );
      return;
    }
    const placement = this.placementOf(state, pose);
    if (!this.host.emit('propPlaceConfirm', player, state.handle, placement)) {
      this.host.emit('propPlaceRejected', player, state.handle, placement);
      return;
    }
    if (state.spawn) {
      const { Prop } = this.host.props;
      this.host.props.spawn(Prop, {
        model: state.models[0],
        position: pose.position,
        orientation: pose.rotation,
        scale: state.scale,
        physics: state.spawn,
        dimension: player.dimension,
      });
    }
  }

  withinRules(player, rules, pose) {
    const from = distance(pose.position, player.position);
    const tooFar = rules.maxDistance !== undefined && from > rules.maxDistance + DISTANCE_SLACK;
    const tooNear = rules.minDistance !== undefined && from < rules.minDistance - DISTANCE_SLACK;
    const wrongSurface =
      rules.surfaces && pose.surface !== '' && !rules.surfaces.includes(pose.surface);
    return !tooFar && !tooNear && !wrongSurface;
  }

  // A player left: their game is gone, so their previews and session are only dropped.
  left(player) {
    for (const state of [...this.ghosts.values()]) {
      if (state.player === player) this.destroyGhost(state, true);
    }
    const session = this.sessions.get(player);
    if (session) this.endSession(session, false);
  }

  // A resource stopped: what it showed goes with it.
  resourceStopped(name) {
    for (const state of [...this.ghosts.values()]) {
      if (state.resource === name) this.destroyGhost(state);
    }
    for (const state of [...this.sessions.values()]) {
      if (state.resource === name) this.endSession(state, true);
    }
  }
}

module.exports = { PropPreviews };
