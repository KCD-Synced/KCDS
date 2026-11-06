'use strict';

// Script-driven animations of a player's body: Player.playAnimation,
// Player.stopAnimation and the playerAnimationEnd event. See.
//
// A playAnimation is desired state the server keeps per player: a request with an id, sent to the
// player's own client ({ op: 'animation', id, kind: 'play', request, fragment, clip, tags, loop,
// lockMovement, restore, props }) and sent again, with the same id, when the player's world gets
// ready again. Other players' clients see it through the owner's replicated animation, so only the
// owner gets the order. The owner's client reports how each request ended ({ op: 'animationEnd',
// id, request, reason, exit }); the server raises playerAnimationEnd once per request.
//
// Player.playPairedAnimation puts the slave where the master stands, facing the same
// way, and after PAIR_DELAY_MS starts the master's fragment on the master and its Slave fragment on
// the slave, each order naming the other (`partner`), so the two bodies do not push each other.
// The slave's order says it is the slave (`pairedSlave`): the game's paired clips each start at
// their own place around one origin, so the slave's client moves its body from the master's place
// to where its clip starts relative to the master's.
//
// A request plays at its `speed` from its `time` (seconds in). While it runs its speed
// (setAnimationSpeed, pauseAnimation, resumeAnimation: { op: 'animation', id, kind: 'speed',
// request, speed }) and its props (setAnimationProps: { ..., kind: 'props', request, props })
// change without playing it again; both are part of the state sent again with the request.
//
// Who ends a request:
//   - finished, interrupted and refused come from the owner's client, from its animation system
//     (a loop never finishes or is interrupted by itself: only refused ends it);
//   - stopped and replaced come from the server at once, when stopAnimation or the next
//     playAnimation is called. The client's later report for that request is stale and ignored.
//   - a request with no fragment (props or tags only) raises nothing, except tags only none of
//     which the body has: refused, with the fragment ''. A clip raises its end with
//     the fragment '' and the clip's name after the reason.
// stopAnimation({ standUp: true }) starts a request of its own for the game's StandUp or GetUp,
// which only the client can name (it knows the stance), so that request's report carries the name.

const { bundledFiles } = require('../bundles.cjs');
const { lookupItemClass } = require('../inventory/catalog.cjs');
const { resolveMesh } = require('../props/catalog.cjs');
const { clipOf, propNamed } = require('./catalog.cjs');
const { rotation: rotationOf } = require('../math.cjs');

// The bounds of kcd2s/integration/player_animation.hpp.
const FRAGMENT_MAX = 64;
const CLIP_MAX = 200;
const TAGS_MAX = 128;
const JOINT_MAX = 64;
const PROPS_MAX = 2;
const OFFSET_MAX = 100;
// The fastest a request plays and the latest it starts, in seconds.
const SPEED_MAX = 10;
const TIME_MAX = 600;

// An order's clip that a resource ships starts with this (kcd2s/integration/animation_props.hpp).
const SHIPPED_CLIP_MARK = '@';

const PLAY_OPTIONS = new Set([
  'clip',
  'tags',
  'loop',
  'lockMovement',
  'upperBody',
  'gravity',
  'lockRotation',
  'speed',
  'time',
  'props',
  'target',
  'side',
  'distance',
]);
// playAnimation with a target: where the body stands at a horse, or another body,
// before it plays, as the game's camp places its groom: the side, how far from the target's
// middle it stands along that side (metres), and how long the placement takes to reach the
// player's game before the animation starts (ms).
const TARGET_SIDES = Object.freeze({ left: 1.0, right: 1.0, head: 1.5, rear: 1.7 });
const TARGET_DISTANCE_MAX = 5;
const TARGET_DELAY_MS = 400;
const STOP_OPTIONS = new Set(['standUp']);
const PAIRED_OPTIONS = new Set(['slaveFragment', 'tags', 'slaveTags', 'lockMovement']);
// The slave's placement reaches its game before the two start (ms).
const PAIR_DELAY_MS = 400;
const FACE_OPTIONS = new Set(['tags']);
// Player.setSpeaking's styles; each client loops the game's own face clip for it.
const TALK_STYLES = ['neutral', 'happy', 'drunk', 'chew'];
const DEFAULT_TALK_STYLE = 'neutral';
const PROP_KEYS = new Set([
  'item',
  'model',
  'hand',
  'joint',
  'attach',
  'rest',
  'position',
  'rotation',
  'fromAnimation',
  'untilPutDown',
  'lit',
]);
// A lit prop burns as the game's torches do: this flame and a warm light at its tip,
// LIT_TIP from where the prop hangs along its own axes (the torch's head).
const LIT_EFFECT = 'cinematics.fires.fire_torch_refr';
const LIT_TIP = { x: 0, y: 0, z: 0.45 };
const LIT_LIGHT = { color: { x: 1, y: 0.55, z: 0.25 }, intensity: 2, radius: 7 };
// The hand's own joint a prop without one hangs on.
const HAND_JOINTS = { right: 'RightHand', left: 'LeftHand' };
// What the owner's animation system reports; stopped and replaced are the server's own.
const CLIENT_REASONS = new Set(['finished', 'interrupted', 'refused']);
const EXITS = new Set(['StandUp', 'GetUp']);
// The joints the game's own hand attachments hang on (hand_right and hand_left of the human
// skeleton's male.cdf): the animations move them to where each hand holds its item, so a prop
// there sits as the game's NPCs hold it.
const HAND_SLOTS = { right: 'RightHandSlot', left: 'LeftHandSlot' };

function isObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function rejectUnknownKeys(method, value, allowed) {
  for (const key of Object.keys(value)) {
    if (!allowed.has(key)) {
      throw new TypeError(`${method} does not take the option ${key}`);
    }
  }
}

// Printable ASCII without spaces, at most `max` characters.
function wireText(name, value, max) {
  if (typeof value !== 'string') {
    throw new TypeError(`${name} must be a string`);
  }
  if (value.length > max || !/^[\x21-\x7e]*$/.test(value)) {
    throw new RangeError(`${name} must be at most ${max} printable characters without spaces`);
  }
  return value;
}

// A joint's name: the game's have spaces ("Bip01 Neck"); printable, but no '|' (the
// props field writes a space as one).
function jointText(value) {
  if (typeof value !== 'string') {
    throw new TypeError('A prop joint must be a string');
  }
  if (value.length > JOINT_MAX || !/^[\x20-\x7b\x7d\x7e]*$/.test(value)) {
    throw new RangeError(`A prop joint must be at most ${JOINT_MAX} printable characters, no '|'`);
  }
  return value;
}

function flagOption(name, value) {
  if (value !== undefined && typeof value !== 'boolean') {
    throw new TypeError(`${name} must be true or false`);
  }
  return value === true;
}

// lockRotation: true or false, or the look's limits in degrees, { yaw, up, down }: how far the
// camera turns to each side of the body's heading (1 to 180) and up and down from level (0 to 89).
// Wire: lockRotation and lookYaw, lookUp, lookDown (0 the game's own free look limits).
const LOOK_LIMIT_KEYS = new Set(['yaw', 'up', 'down']);

function lockRotationOf(value) {
  if (value === undefined || typeof value === 'boolean') {
    return { lockRotation: value === true, lookYaw: 0, lookUp: 0, lookDown: 0 };
  }
  if (!isObject(value)) {
    throw new TypeError('playAnimation lockRotation must be true, false or { yaw, up, down }');
  }
  rejectUnknownKeys('playAnimation lockRotation', value, LOOK_LIMIT_KEYS);
  return {
    lockRotation: true,
    lookYaw: Math.round(rangeOption('playAnimation lockRotation.yaw', value.yaw, 1, 180, 120)),
    lookUp: Math.round(rangeOption('playAnimation lockRotation.up', value.up, 0, 89, 70)),
    lookDown: Math.round(rangeOption('playAnimation lockRotation.down', value.down, 0, 89, 70)),
  };
}

// A number from `min` to `max`, `fallback` when not given.
function rangeOption(name, value, min, max, fallback) {
  if (value === undefined) {
    return fallback;
  }
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new TypeError(`${name} must be a number`);
  }
  if (value < min || value > max) {
    throw new RangeError(`${name} must be from ${min} to ${max}`);
  }
  return value;
}

function speedOf(name, value) {
  return rangeOption(name, value, 0, SPEED_MAX, 1);
}

function clipPath(value) {
  const clip = wireText('playAnimation clip', value, CLIP_MAX);
  const segments = clip.split('/');
  if (
    clip.startsWith('/') ||
    clip.startsWith(SHIPPED_CLIP_MARK) ||
    clip.includes('\\') ||
    segments.includes('..')
  ) {
    throw new RangeError('playAnimation clip must be a relative path inside the game or resource');
  }
  return clip;
}

// The clip as the order names it. A .caf the calling resource ships (one of the files its bundle
// sends to clients) is "@<resource>/<path>": every client plays it from the file the resource sent
// it (Client/resources/runtime.cjs shipClips). Any other clip is one of the game's, as given.
function orderClip(host, clip) {
  if (clip === '' || !/\.caf$/i.test(clip)) {
    return clip;
  }
  const owner = host.context?.getStore?.();
  const definition = owner ? host.manifests?.get(owner.name) : undefined;
  if (!definition || !bundledFiles(definition.folder, definition.manifest).includes(clip)) {
    return clip;
  }
  const shipped = `${SHIPPED_CLIP_MARK}${owner.name}/${clip}`;
  if (shipped.length > CLIP_MAX || /[^\x21-\x7e]/.test(shipped)) {
    throw new RangeError(
      `playAnimation clip ${clip} of ${owner.name} is longer than ${CLIP_MAX - 2} characters ` +
        'with the resource name, or not printable ASCII',
    );
  }
  return shipped;
}

// A prop offset in metres; a component left out is 0.
function offsetOf(name, value) {
  if (!isObject(value)) {
    throw new TypeError(`${name} must be a Vector3`);
  }
  const offset = {};
  for (const axis of ['x', 'y', 'z']) {
    const component = value[axis] ?? 0;
    if (typeof component !== 'number' || !Number.isFinite(component)) {
      throw new TypeError(`${name} needs finite numbers`);
    }
    if (Math.abs(component) > OFFSET_MAX) {
      throw new RangeError(`${name} lies at most ${OFFSET_MAX} m from the hand`);
    }
    offset[axis] = component;
  }
  return offset;
}

// A prop rotation as a unit quaternion: Euler degrees or a Quaternion.
function rotationOfProp(value) {
  if (!isObject(value)) {
    throw new TypeError('A prop rotation must be a Quaternion or Euler angles');
  }
  // Euler angles may leave an axis out (0); a quaternion names all four components.
  const isQuaternion = 'w' in value;
  const components = {};
  for (const name of isQuaternion ? ['x', 'y', 'z', 'w'] : ['x', 'y', 'z']) {
    let component = value[name];
    if (component === undefined && !isQuaternion) {
      component = 0;
    }
    if (typeof component !== 'number' || !Number.isFinite(component)) {
      throw new TypeError('A prop rotation needs finite numbers');
    }
    components[name] = component;
  }
  const quaternion = rotationOf(components);
  const length = Math.hypot(quaternion.x, quaternion.y, quaternion.z, quaternion.w);
  if (!Number.isFinite(length) || length < 1e-6) {
    throw new TypeError('A prop rotation quaternion must not be zero');
  }
  return {
    x: quaternion.x / length,
    y: quaternion.y / length,
    z: quaternion.z / length,
    w: quaternion.w / length,
  };
}

// One prop as the wire carries it: an item class (its GUID) or a prop-catalog mesh, the hand, the
// joint ('' for the hand's own) and the optional offsets.
function propOf(prop) {
  if (!isObject(prop)) {
    throw new TypeError('A prop must be an object');
  }
  rejectUnknownKeys('playAnimation prop', prop, PROP_KEYS);
  const hasItem = prop.item !== undefined;
  const hasModel = prop.model !== undefined;
  if (hasItem === hasModel) {
    throw new TypeError('A prop needs an item or a model, not both');
  }
  const wire = {};
  if (hasItem) {
    if (typeof prop.item !== 'string') {
      throw new TypeError('A prop item must be an item class name or id');
    }
    // An item class, or else a name from Animations.props(), whose mesh is used.
    const itemClass = lookupItemClass(prop.item);
    const animationProp = itemClass ? null : propNamed(prop.item);
    const mesh = animationProp ? resolveMesh(animationProp.model) : null;
    if (itemClass) {
      wire.item = itemClass.id;
    } else if (mesh !== null) {
      wire.model = mesh;
    } else {
      throw new RangeError(`Neither the item catalog nor Animations.props() carry ${prop.item}`);
    }
  } else {
    if (typeof prop.model !== 'string') {
      throw new TypeError('A prop model must be a mesh path or stem');
    }
    const mesh = resolveMesh(prop.model);
    if (mesh === null) {
      throw new RangeError(`The prop catalog does not carry ${prop.model}`);
    }
    wire.model = mesh;
  }
  if (prop.hand !== undefined && prop.hand !== 'right' && prop.hand !== 'left') {
    throw new TypeError("A prop hand must be 'right' or 'left'");
  }
  wire.hand = prop.hand ?? 'right';
  if (prop.attach !== undefined && prop.attach !== 'game' && prop.attach !== 'bone') {
    throw new TypeError("A prop attach must be 'game' or 'bone'");
  }
  if (prop.attach === 'game' && prop.joint !== undefined) {
    throw new TypeError("A prop takes attach: 'game' or a joint, not both");
  }
  if (prop.attach === 'game') {
    wire.joint = HAND_SLOTS[wire.hand];
  } else {
    wire.joint = prop.joint === undefined ? '' : jointText(prop.joint);
  }
  if (prop.position !== undefined) {
    wire.position = offsetOf('A prop position', prop.position);
  }
  if (prop.rotation !== undefined) {
    wire.rotation = rotationOfProp(prop.rotation);
  }
  // In the hand only between the animation's own pickup and put-down.
  if (flagOption('A prop fromAnimation', prop.fromAnimation)) {
    wire.fromAnimation = true;
  }
  // In the hand from the start until the animation puts it down.
  if (flagOption('A prop untilPutDown', prop.untilPutDown)) {
    if (wire.fromAnimation) {
      throw new TypeError('A prop takes fromAnimation or untilPutDown, not both');
    }
    wire.untilPutDown = true;
  }
  // The server Prop it rests as in the world before and after: hidden while the animation holds
  // the item.
  if (prop.rest !== undefined) {
    const id = typeof prop.rest === 'number' ? prop.rest : prop.rest?.id;
    if (!Number.isInteger(id) || id <= 0 || id > 0xffffffff) {
      throw new TypeError('A prop rest must be a Prop');
    }
    if (!wire.fromAnimation) {
      throw new TypeError('A prop rests in the world only with fromAnimation: true');
    }
    wire.rest = id;
  }
  // It burns: the server's flame and light at its tip, not sent with the order.
  if (flagOption('A prop lit', prop.lit)) {
    Object.defineProperty(wire, 'lit', { value: true });
  }
  return wire;
}

// q * v: v turned by the unit quaternion q.
function turned(q, v) {
  const u = {
    x: 2 * (q.y * v.z - q.z * v.y),
    y: 2 * (q.z * v.x - q.x * v.z),
    z: 2 * (q.x * v.y - q.y * v.x),
  };
  return {
    x: v.x + q.w * u.x + (q.y * u.z - q.z * u.y),
    y: v.y + q.w * u.y + (q.z * u.x - q.x * u.z),
    z: v.z + q.w * u.z + (q.x * u.y - q.y * u.x),
  };
}

function propsOf(props) {
  if (props === undefined) {
    return [];
  }
  const list = Array.isArray(props) ? props : [props];
  if (list.length > PROPS_MAX) {
    throw new RangeError(`playAnimation holds at most ${PROPS_MAX} props`);
  }
  return list.map(propOf);
}

// The checked arguments of playAnimation as the order carries them; throws for anything the game
// cannot use.
// Where a body stands beside a target facing it: `distance` metres from the target's middle to its
// left, right, head or rear, by the target's heading (KCDS faces +y at yaw 0, turning left), and
// the yaw in degrees that faces the target.
function besideTarget(at, turn, side, distance) {
  const { x, y, z, w } = turn;
  const heading = Math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z));
  const forward = { x: -Math.sin(heading), y: Math.cos(heading) };
  const left = { x: -forward.y, y: forward.x };
  const way = {
    left: left,
    right: { x: -left.x, y: -left.y },
    head: forward,
    rear: { x: -forward.x, y: -forward.y },
  }[side];
  const position = { x: at.x + way.x * distance, y: at.y + way.y * distance, z: at.z };
  // Turned to the target: its direction from the spot.
  const yaw = (Math.atan2(way.x, -way.y) * 180) / Math.PI;
  return { position, yaw };
}

function playOrder(fragment, options) {
  if (options !== undefined && !isObject(options)) {
    throw new TypeError('playAnimation takes an options object');
  }
  const given = options ?? {};
  rejectUnknownKeys('playAnimation', given, PLAY_OPTIONS);
  if ((given.side !== undefined || given.distance !== undefined) && given.target === undefined) {
    throw new TypeError('playAnimation side and distance go with target');
  }
  const order = {
    fragment: wireText('playAnimation fragment', fragment, FRAGMENT_MAX),
    clip: given.clip === undefined ? '' : clipPath(given.clip),
    tags: given.tags === undefined ? '' : wireText('playAnimation tags', given.tags, TAGS_MAX),
    loop: flagOption('playAnimation loop', given.loop),
    lockMovement: flagOption('playAnimation lockMovement', given.lockMovement),
    // Only the upper body plays it; the legs keep walking and moving does not end it.
    upperBody: flagOption('playAnimation upperBody', given.upperBody),
    // gravity: false keeps the body where the script puts it while it runs.
    noGravity: given.gravity !== undefined && !flagOption('playAnimation gravity', given.gravity),
    // The body keeps its heading; only the head and the camera turn, within the limits
    // when it is { yaw, up, down }.
    ...lockRotationOf(given.lockRotation),
    // How fast it plays (0 holds it still) and how many seconds in it starts.
    speed: speedOf('playAnimation speed', given.speed),
    time: rangeOption('playAnimation time', given.time, 0, TIME_MAX, 0),
    props: propsOf(given.props),
  };
  if (order.fragment !== '' && order.clip !== '') {
    throw new TypeError('playAnimation plays a clip with an empty fragment');
  }
  if (order.fragment === '' && order.clip === '' && order.tags === '' && order.props.length === 0) {
    throw new TypeError('playAnimation needs a fragment, a clip, tags or props');
  }
  // Only the upper body: the fragment's clip plays on a layer of its own, which the
  // walk does not end, and every client keeps the walk's legs under it.
  if (order.upperBody && order.clip === '') {
    const clip = clipOf(order.fragment, order.tags);
    if (clip === '') {
      throw new RangeError(`playAnimation upperBody: ${order.fragment} has no clip to play`);
    }
    order.clip = clip;
    order.fragment = '';
  }
  if (order.upperBody && order.lockMovement) {
    throw new TypeError('playAnimation upperBody keeps walking: it takes no lockMovement');
  }
  return order;
}

// Whether stopAnimation asks for the stand-up; throws for any other option.
function stopsWithStandUp(options) {
  if (options !== undefined && !isObject(options)) {
    throw new TypeError('stopAnimation takes an options object');
  }
  const given = options ?? {};
  rejectUnknownKeys('stopAnimation', given, STOP_OPTIONS);
  return flagOption('stopAnimation standUp', given.standUp);
}

class PlayerAnimations {
  constructor(host) {
    this.host = host;
    // The next request id: one counter for every player, so an id names one request only.
    this.nextRequest = 1;
    // player id -> { current, awaiting }. `current` is the request that is the player's desired
    // state: { id, kind: 'play' | 'standUp', order, ended }, `ended` once its outcome is known
    // (the state stays until stopAnimation or the next playAnimation). `awaiting` is the id of a
    // stand-up that was cut before its client named it, with the reason it was cut for.
    this.states = new Map();
    // Handles of players that are leaving; they can no longer be given an animation.
    this.departed = new WeakSet();
  }

  stateOf(player) {
    let state = this.states.get(player.id);
    if (!state) {
      // talking: the style the mouth moves in, null while it does not; talkingStyleGiven: that
      // style came with setSpeaking rather than from setTalkingStyle; talkStyle: the player's own
      // style for setSpeaking without one.
      state = {
        current: null,
        awaiting: null,
        face: null,
        talking: null,
        talkingStyleGiven: false,
        talkStyle: DEFAULT_TALK_STYLE,
      };
      this.states.set(player.id, state);
    }
    return state;
  }

  // Player.playAnimation. True once sent; false before the player has a body.
  play(player, fragment, options) {
    const order = playOrder(fragment, options);
    order.clip = orderClip(this.host, order.clip);
    if (this.host.players.get(player.id) !== player) {
      return false;
    }
    this.host.requireLivePlayerCall(player);
    if (!player.ready || this.departed.has(player)) {
      return false;
    }
    if (options?.target !== undefined) {
      return this.playAt(player, order, options);
    }
    const state = this.stateOf(player);
    const previous = state.current;
    const request = { id: this.nextRequest++, kind: 'play', order, ended: false };
    state.current = request;
    this.sendPlay(player, request);
    this.light(player, request);
    // The new request is in place before the old one's end is raised, so a handler that chains the
    // next animation from it replaces this one in turn.
    this.retire(player, state, previous, 'replaced');
    return true;
  }

  // The request's lit props burn: a flame (Vfx) and a light (Light) on the joint the
  // prop hangs on, at its tip; they go with the request. Made for the resource that played it.
  light(player, request) {
    this.douse(request);
    const flames = [];
    for (const prop of request.order.props ?? []) {
      if (!prop.lit) {
        continue;
      }
      const joint = prop.joint === '' ? HAND_JOINTS[prop.hand] : prop.joint;
      const offset = prop.position ?? { x: 0, y: 0, z: 0 };
      const tip = turned(prop.rotation ?? { x: 0, y: 0, z: 0, w: 1 }, LIT_TIP);
      const at = { x: offset.x + tip.x, y: offset.y + tip.y, z: offset.z + tip.z };
      const api = this.host.api ?? this.host.makeAPI();
      flames.push(api.Vfx.spawn(LIT_EFFECT, at, { entity: player, joint }, player.dimension));
      flames.push(api.Light.spawn(at, { entity: player, joint, ...LIT_LIGHT }));
    }
    request.flames = flames;
  }

  // The request's flames go out.
  douse(request) {
    for (const flame of request?.flames ?? []) {
      flame.destroy();
    }
    if (request) {
      request.flames = [];
    }
  }

  // Player.stopAnimation. True once sent; false before the player has a body.
  stop(player, options) {
    const standUp = stopsWithStandUp(options);
    if (this.host.players.get(player.id) !== player) {
      return false;
    }
    this.host.requireLivePlayerCall(player);
    if (!player.ready || this.departed.has(player)) {
      return false;
    }
    const state = this.stateOf(player);
    const previous = state.current;
    // There is a stance to stand up from only while an animation of the script's is the state.
    const standing = standUp && previous?.kind === 'play';
    const request = standing ? { id: this.nextRequest++, kind: 'standUp', ended: false } : null;
    state.current = request;
    this.host.bridge.send({
      op: 'animation',
      id: player.id,
      kind: 'stop',
      request: request?.id ?? 0,
      standUp: standing,
    });
    this.retire(player, state, previous, 'stopped');
    return true;
  }

  // Player.playPairedAnimation: the slave goes where the master stands, then both
  // play, the master `fragment` and the slave its Slave fragment (or `slaveFragment`). True when it
  // went out; false when either has no body yet, they are the same player or in other worlds.
  // Invalid arguments throw. Each request ends as any other (playerAnimationEnd).
  playPaired(master, slave, fragment, options) {
    if (options !== undefined && !isObject(options)) {
      throw new TypeError('playPairedAnimation takes an options object');
    }
    const given = options ?? {};
    rejectUnknownKeys('playPairedAnimation', given, PAIRED_OPTIONS);
    const slaveFragment =
      given.slaveFragment === undefined
        ? `${wireText('playPairedAnimation fragment', fragment, FRAGMENT_MAX - 5)}Slave`
        : given.slaveFragment;
    const lockMovement =
      given.lockMovement === undefined
        ? true
        : flagOption('playPairedAnimation lockMovement', given.lockMovement);
    const masterOrder = playOrder(fragment, { tags: given.tags, lockMovement });
    const slaveOrder = playOrder(slaveFragment, { tags: given.slaveTags, lockMovement });
    if (masterOrder.fragment === '' || slaveOrder.fragment === '') {
      throw new TypeError('playPairedAnimation plays two fragments');
    }
    const live = (player) =>
      this.host.players.get(player?.id) === player && player.ready && !this.departed.has(player);
    if (!live(master) || !live(slave) || master === slave) {
      return false;
    }
    this.host.requireLivePlayerCall(master);
    this.host.requireLivePlayerCall(slave);
    if (master.dimension !== slave.dimension || !master.position) {
      return false;
    }
    if (!this.host.spawnPlayer(slave, master.position, master.rotation)) {
      return false;
    }
    masterOrder.partner = slave.id;
    slaveOrder.partner = master.id;
    slaveOrder.pairedSlave = true;
    const start = () => {
      if (!live(master) || !live(slave)) {
        return;
      }
      this.start(master, masterOrder);
      this.start(slave, slaveOrder);
    };
    setTimeout(start, this.pairDelayMs ?? PAIR_DELAY_MS).unref?.();
    return true;
  }

  // playAnimation with a target: the body is put beside it first, turned to it,
  // as the game's camp places a groom at its horse, and the animation starts once its game has the
  // placement. False for a target that is not in the player's dimension or has no position.
  playAt(player, order, options) {
    const target = options.target;
    const side = options.side ?? 'left';
    if (!Object.hasOwn(TARGET_SIDES, side)) {
      throw new RangeError(`playAnimation side is one of ${Object.keys(TARGET_SIDES).join(', ')}`);
    }
    const distance = rangeOption(
      'playAnimation distance',
      options.distance,
      0.2,
      TARGET_DISTANCE_MAX,
      TARGET_SIDES[side],
    );
    const at = target?.position;
    const turn = target?.rotation;
    const placed = at && [at.x, at.y, at.z].every(Number.isFinite) && turn;
    if (
      !placed ||
      target === player ||
      (target.dimension ?? player.dimension) !== player.dimension
    ) {
      return false;
    }
    const spot = besideTarget(at, turn, side, distance);
    if (!this.host.spawnPlayer(player, spot.position, { x: 0, y: 0, z: spot.yaw })) {
      return false;
    }
    const start = () => {
      if (
        this.host.players.get(player.id) === player &&
        player.ready &&
        !this.departed.has(player)
      ) {
        this.start(player, order);
        this.light(player, this.stateOf(player).current);
      }
    };
    setTimeout(start, this.targetDelayMs ?? TARGET_DELAY_MS).unref?.();
    return true;
  }

  // A checked order becomes the player's request: sent, and the one before it replaced.
  start(player, order) {
    const state = this.stateOf(player);
    const previous = state.current;
    const request = { id: this.nextRequest++, kind: 'play', order, ended: false };
    state.current = request;
    this.sendPlay(player, request);
    this.retire(player, state, previous, 'replaced');
  }

  // The running request of the player's that a change applies to, else null.
  running(player) {
    if (this.host.players.get(player.id) !== player) {
      return null;
    }
    this.host.requireLivePlayerCall(player);
    if (!player.ready || this.departed.has(player)) {
      return null;
    }
    const request = this.states.get(player.id)?.current;
    return request?.kind === 'play' && !request.ended ? request : null;
  }

  // The speed the request plays at: none while paused.
  static effectiveSpeed(request) {
    return request.paused ? 0 : request.order.speed;
  }

  sendSpeed(player, request) {
    this.host.bridge.send({
      op: 'animation',
      id: player.id,
      kind: 'speed',
      request: request.id,
      speed: PlayerAnimations.effectiveSpeed(request),
    });
  }

  // Player.setAnimationSpeed: the running request plays at `speed` (a paused one once
  // resumed). True when the order went out; false without a running request.
  setSpeed(player, speed) {
    const value = speedOf('setAnimationSpeed speed', speed ?? NaN);
    const request = this.running(player);
    if (!request) {
      return false;
    }
    request.order.speed = value;
    this.sendSpeed(player, request);
    return true;
  }

  // Player.pauseAnimation and resumeAnimation: the running request holds still where
  // it is, or plays on at its speed. True when the order went out; false without a running request.
  setPaused(player, paused) {
    const request = this.running(player);
    if (!request) {
      return false;
    }
    request.paused = paused;
    this.sendSpeed(player, request);
    return true;
  }

  // The running request's speed (0 while paused), null without one.
  speedOfPlayer(player) {
    const request = this.states.get(player.id)?.current;
    return request?.kind === 'play' && !request.ended
      ? PlayerAnimations.effectiveSpeed(request)
      : null;
  }

  // Player.setAnimationProps: the running request's props, as playAnimation takes
  // them, without playing it again. True when the order went out; false without a running request.
  setProps(player, props) {
    const list = propsOf(props);
    const request = this.running(player);
    if (!request) {
      return false;
    }
    request.order.props = list;
    this.host.bridge.send({
      op: 'animation',
      id: player.id,
      kind: 'props',
      request: request.id,
      props: list,
    });
    this.light(player, request);
    return true;
  }

  // The owner's client reported how a request ended.
  report(player, e) {
    const state = player ? this.states.get(player.id) : undefined;
    if (!state || !Number.isSafeInteger(e.request)) {
      return;
    }
    const exit = EXITS.has(e.exit) ? e.exit : '';
    if (state.current?.id === e.request) {
      this.reportCurrent(player, state, e.reason, exit);
    } else if (state.awaiting?.id === e.request) {
      // A stand-up that was cut before its name was known: the client's report names it now.
      const reason = CLIENT_REASONS.has(e.reason) ? e.reason : state.awaiting.reason;
      state.awaiting = null;
      this.raise(player, exit, reason);
    }
    // Anything else is a stale or duplicate report.
  }

  reportCurrent(player, state, reason, exit) {
    const request = state.current;
    if (request.ended || !CLIENT_REASONS.has(reason)) {
      return;
    }
    if (request.kind === 'standUp') {
      // Done, whether or not the game authors a way out of the stance (no exit: nothing to raise).
      state.current = null;
      request.ended = true;
      this.raise(player, exit, reason);
      return;
    }
    // A loop restarts by itself after a pass or an interruption; only a refusal ends it.
    if (exit !== '' || (request.order.loop && reason !== 'refused')) {
      return;
    }
    request.ended = true;
    // The server's own follow-up (carts/carts.cjs: a seat's idle after its climb), before the
    // resources hear of the end.
    request.onEnd?.(reason);
    this.raise(player, request.order.fragment, reason, request.order.clip);
  }

  // The owner's client: a prop of the running request that follows the animation went into the
  // hand or was put down. Raises propPickedUp or propPlaced (player, prop) with
  // { index, fragment, hand, item, model, rest }: the item class id or the mesh it showed, and
  // the Prop it rests as in the world (null without one).
  propCue(player, e) {
    const state = player ? this.states.get(player.id) : undefined;
    const request = state?.current;
    if (!request || request.kind !== 'play' || request.ended || request.id !== e.request) {
      return;
    }
    const prop = request.order.props?.[e.prop];
    if (!prop || (!prop.fromAnimation && !prop.untilPutDown)) {
      return;
    }
    const info = {
      index: e.prop,
      fragment: request.order.fragment,
      hand: prop.hand,
      item: prop.item ?? null,
      model: prop.model ?? null,
      rest: prop.rest ? this.host.props?.props.get(prop.rest) ?? null : null,
    };
    this.host.emit(e.taken === true ? 'propPickedUp' : 'propPlaced', player, info);
  }

  // The server ends the request that stopAnimation or the next playAnimation took the place of.
  retire(player, state, previous, reason) {
    this.douse(previous);
    if (!previous || previous.ended) {
      return;
    }
    previous.ended = true;
    if (previous.kind === 'standUp') {
      state.awaiting = { id: previous.id, reason };
      return;
    }
    this.raise(player, previous.order.fragment, reason, previous.order.clip);
  }

  // A request without a fragment raises nothing, except a refusal (tags none of which the body
  // has) and a clip's end: both with the fragment '', a clip with its name after the
  // reason (a server team asked, scenes timed by hand without it).
  raise(player, fragment, reason, clip = '') {
    if (fragment === '' && clip === '' && reason !== 'refused') {
      return;
    }
    this.host.emit('playerAnimationEnd', player, fragment, reason, clip);
  }

  sendPlay(player, request, restore = false) {
    this.host.bridge.send({
      op: 'animation',
      id: player.id,
      kind: 'play',
      request: request.id,
      restore,
      ...request.order,
      speed: PlayerAnimations.effectiveSpeed(request),
    });
  }

  // Player.setExpression: a fragment on the face's own scope with its mood
  // tags, held beside whatever the body plays; "" hands the face back to the game. It is state, sent
  // again when the player's world gets ready. True when the order went out; false for a player
  // with no body yet. Invalid arguments throw.
  face(player, fragment, options) {
    wireText('setExpression fragment', fragment, FRAGMENT_MAX);
    if (options !== undefined && !isObject(options)) {
      throw new TypeError('setExpression options must be an object');
    }
    if (options) {
      rejectUnknownKeys('setExpression', options, FACE_OPTIONS);
    }
    const tags =
      options?.tags === undefined ? '' : wireText('setExpression tags', options.tags, TAGS_MAX);
    if (this.host.players.get(player.id) !== player) {
      return false;
    }
    this.host.requireLivePlayerCall(player);
    if (!player.ready || this.departed.has(player)) {
      return false;
    }
    const state = this.stateOf(player);
    state.face = fragment === '' ? null : { fragment, tags };
    this.sendFace(player, state.face);
    return true;
  }

  sendFace(player, face) {
    this.host.bridge.send({
      op: 'animation',
      id: player.id,
      kind: 'face',
      request: 0,
      fragment: face?.fragment ?? '',
      tags: face?.tags ?? '',
    });
  }

  // Player.setSpeaking: the mouth moves as if talking, on every screen, until told to
  // stop. The style is `style` when given, else the player's own (setTalkingStyle, `neutral` by
  // default). It is state, sent again when the player's world gets ready. True when the order went
  // out; false before the body exists. Invalid arguments throw.
  talk(player, talking, style) {
    if (typeof talking !== 'boolean') {
      throw new TypeError('setSpeaking talking must be true or false');
    }
    if (style !== undefined) {
      talkStyleOf('setSpeaking', style);
    }
    if (!this.reachable(player)) {
      return false;
    }
    const state = this.stateOf(player);
    state.talking = talking ? style ?? state.talkStyle : null;
    state.talkingStyleGiven = talking && style !== undefined;
    this.sendTalk(player, state.talking);
    return true;
  }

  // Player.setTalkingStyle: the style setSpeaking uses when it is given none; no style (or
  // `neutral`) is the default, normal speech. A player talking in their own style switches at once.
  // True when set; false for a player who left. An unknown style throws.
  setTalkStyle(player, style) {
    const chosen = style === undefined ? DEFAULT_TALK_STYLE : talkStyleOf('setTalkingStyle', style);
    if (this.host.players.get(player.id) !== player || this.departed.has(player)) {
      return false;
    }
    const state = this.stateOf(player);
    state.talkStyle = chosen;
    if (state.talking !== null && !state.talkingStyleGiven && state.talking !== chosen) {
      state.talking = chosen;
      if (player.ready) {
        this.sendTalk(player, chosen);
      }
    }
    return true;
  }

  talkStyleOf(player) {
    return this.states.get(player.id)?.talkStyle ?? DEFAULT_TALK_STYLE;
  }

  // The style the player's mouth moves in now, null while they are not talking.
  talkingOf(player) {
    return this.states.get(player.id)?.talking ?? null;
  }

  // Whether an order can go to the player's body now.
  reachable(player) {
    if (this.host.players.get(player.id) !== player) {
      return false;
    }
    this.host.requireLivePlayerCall(player);
    return player.ready && !this.departed.has(player);
  }

  sendTalk(player, style) {
    this.host.bridge.send({
      op: 'animation',
      id: player.id,
      kind: 'talk',
      request: 0,
      fragment: style ?? '',
    });
  }

  // The player's world got ready again: its animation is state, so it is sent again with the same
  // id. One that ended is only restored (props, tags, movement lock), not played again.
  arrived(player) {
    const state = this.states.get(player.id);
    if (!state) {
      return;
    }
    if (state.face) {
      this.sendFace(player, state.face);
    }
    if (state.talking) {
      this.sendTalk(player, state.talking);
    }
    state.awaiting = null;
    const request = state.current;
    if (request?.kind === 'standUp') {
      // The stand-up belonged to the body that is gone.
      state.current = null;
    } else if (request) {
      this.sendPlay(player, request, request.ended);
    }
  }

  // The player is leaving: no end is raised, the state is gone with them.
  left(player) {
    this.departed.add(player);
    this.douse(this.states.get(player.id)?.current);
    this.states.delete(player.id);
  }
}

function talkStyleOf(call, style) {
  if (!TALK_STYLES.includes(style)) {
    throw new RangeError(`${call} style must be one of ${TALK_STYLES.join(', ')}`);
  }
  return style;
}

module.exports = { PlayerAnimations, TALK_STYLES };
