'use strict';

// The Asset/Animation Studio's own editor. It opens in World Architect's local mode
// when the studio was asked for (Client/runtime/studio/studio_scene.hpp): the studio's empty
// level, no map, no server. It shares World Architect's screen (architect/shell.cjs: panels,
// cursor, free camera, text overlay) and shows the studio's own work in it:
//
// - modes: Props, Physics, Animation, Textures, Bones (Animation and Textures come later);
// - the outliner: what is in the scene, the character and the props made in it; in Bones mode
//   the character's joints;
// - the details: the chosen prop's settings (a rope's segments and length, its swing test); in
//   Bones mode the chosen joint's turn and the animation's keys;
// - the content: the props the studio can make; in Bones mode the animation's keys.
//
// A prop is made by the game client (studio/studio_rope.hpp): the editor sends
// { op: 'studioRope', segments, length }, { op: 'studioRopeSweep', on } and
// { op: 'studioRopeOff' }. Bones mode draws the skeleton (studio/studio_skeleton.hpp:
// studioBones, studioPickJoint) and poses it (studio/studio_pose.hpp: studioPose).

const { Shell, TILE_COUNT, outlinerRows, FOLDER_ROWS } = require('../architect/shell.cjs');
const { aiConnectView } = require('../architect/ai_connect.cjs');
const { STAGE, LAYOUT } = require('../architect/theme.cjs');
const {
  Animation,
  eventOf,
  PROP_SETTINGS,
  wayAngles,
  lashKeysOf,
  MOST_LASH_KEYS,
} = require('./animation.cjs');
const { bakeClip } = require('./clip_bake.cjs');
const { lashKeys } = require('./lash_keys.cjs');
const { bestLashMotion, lashQuality, lashSeen, crackCheck } = require('./lash_motion.cjs');
const { designLash } = require('./lash_design.cjs');
const {
  patchAnimation,
  trackAnimation,
  retimeAnimation,
  checkAnimation,
  compareAnimations,
  lashShape,
  shapedWays,
} = require('./animation_tools.cjs');
const { OUTFITS, outfitOf, clothingOf, clothingMessage } = require('./outfits.cjs');
const { GLYPHS } = require('./glyphs.cjs');
const timeline = require('./timeline.cjs');
const placing = require('./placing.cjs');
const shapes = require('./shapes.cjs');
const camera = require('./camera.cjs');
// The game's particle effects (the server's catalogue).
const vfxCatalog = require('../../../Backend/scripting/runtimes/js/vfx/catalog.json');

const POLL_MS = 33;
const TOOLS = Object.freeze([
  { key: 'select', label: 'Select' },
  { key: 'move', label: 'Move' },
  { key: 'rotate', label: 'Rotate' },
  { key: 'scale', label: 'Scale' },
]);

const MODES = Object.freeze([
  { key: 'props', label: 'Props', icon: 'box', folders: ['physical', 'static', 'gameProps'] },
  { key: 'physics', label: 'Physics', icon: 'magnet', folders: ['physical'] },
  {
    key: 'animation',
    label: 'Animation',
    icon: 'play',
    folders: ['effects', 'sounds', 'gameClips'],
  },
  { key: 'textures', label: 'Textures', icon: 'map', folders: ['textures'] },
  { key: 'bones', label: 'Bones', icon: 'move', folders: [] },
]);

const FOLDERS = Object.freeze({
  physical: 'Physical props',
  static: 'Static props',
  gameProps: 'Game props',
  textures: 'Your textures',
  effects: 'Effects',
  sounds: 'Sounds',
  gameClips: 'Game clips',
});
// The folders of the game's own files (studio/game_files.hpp): searched and paged by the game
// client, as there are thousands.
const GAME_FOLDERS = Object.freeze({ gameProps: 'objects', gameClips: 'clips' });
const SEARCH_LENGTH = 40;
// How many of the game's files a page the assistant asks for has (the gamefiles step).
const ASSISTANT_PAGE = 50;
// How many recorded keys a message to the game client carries (studio/studio_rope.cpp's parts).
const KEYS_A_PART = 30;
// The most recorded keys (JSON) a kept project or save carries: one message to the game client
// holds 64 kB in all.
const PROJECT_KEYS_BYTES = 48000;
// The client script of an export whose whole body is animated (exportResource: a game clip, or
// an animation not top body only, a baked clip's): the motion swings an arm through the eyes and
// bows the head the camera sits in (a bare arm and the ground over the whole
// screen), so the player sees themselves through the game's third-person camera while it plays
// (Camera.setThirdPerson); V their own eyes, and back.
const BODY_VIEW = [
  "'use strict';",
  '// Exported from the Asset/Animation Studio: the whole body is animated, so the player sees',
  '// themselves from behind while it plays (V: their own eyes, and back).',
  'const { Camera, Key } = kcds.ui;',
  'const VIEW = { distance: 3.2, height: 1.5, shoulder: 0.5 };',
  'let behind = true;',
  'Camera.setThirdPerson(VIEW);',
  '',
  "Key.bind('v', () => {",
  '  behind = !behind;',
  '  Camera.setThirdPerson(behind ? VIEW : null);',
  '});',
  '',
].join('\n');

// A path in the game's packs (Objects/....cgf), as a mesh of the game's own is named.
function isGamePath(name) {
  return typeof name === 'string' && /^objects\/.+\.cgf$/i.test(name);
}

// A game path's file name without its folders and ending; a name of yours as it is.
function shortName(name) {
  return isGamePath(name)
    ? name
        .split('/')
        .at(-1)
        .replace(/\.cgf$/i, '')
    : name;
}

// Where your mesh is held, as Details shows it: a hand, free in the scene, or a joint's name.
function meshHoldLabel(place) {
  return placing.HOLDS.find((one) => one.joint === place.joint)?.label ?? place.joint;
}

// The part of a game clip that plays (of its length, 0 to 1; at least half a percent of it),
// whether the character stays in place and whether it loops.
function gameClipPartOf(part) {
  const from = clamp(Number(part?.from) || 0, [0, 0.995]);
  const to = clamp(Number.isFinite(Number(part?.to)) ? Number(part.to) : 1, [from + 0.005, 1]);
  return {
    from: Math.round(from * 10000) / 10000,
    to: Math.round(to * 10000) / 10000,
    loop: part?.loop !== false,
    inPlace: part?.inPlace !== false,
    // Only the upper body plays it: the legs keep the walk, so a player walks while it plays.
    topBody: part?.topBody === true,
  };
}

function gamePage() {
  return { query: '', page: 0, pages: 1, total: 0, ready: false, asked: false, paths: [] };
}
// Animation mode's events (studio/studio_event.hpp): the effects that suit an animation first,
// then the game's whole catalogue; the sounds known to play (any trigger works by name, MCP).
const EFFECT_PRESETS = Object.freeze([
  'WH_Particels.other.blacksmith_hit',
  'WH_Particels.dust.sweep',
  'WH_Particels.other.explosion_dust',
  'WH_Particels.other.grain_dust',
  'WH_Particels.other.water_drops',
  'WH_Particels.smokes.smoke',
]);
const EFFECTS = Object.freeze([
  ...EFFECT_PRESETS,
  ...vfxCatalog.effects
    .map((one) => (typeof one === 'string' ? one : one.name))
    .filter((name) => typeof name === 'string' && !EFFECT_PRESETS.includes(name))
    .sort(),
]);
const SOUNDS = Object.freeze(['v_horse_whistle', 'a_o_bell_kkut_kostelni']);
// The joints an event can be at, stepped in Details.
const EVENT_JOINTS = Object.freeze([
  'RightHand',
  'LeftHand',
  'Head',
  'Spine3',
  'RightFoot',
  'LeftFoot',
]);
// How many times a texture repeats across the mesh: halved or doubled a step.
const TILING_RANGE = Object.freeze([0.125, 64]);

// What the studio can make, by folder. A prop that is not ready says so and makes nothing.
const ITEMS = Object.freeze({
  physical: [
    { name: 'rope', label: 'Rope', glyph: 'rope', meta: 'Physics rope · in the hand' },
    { name: 'whip', label: 'Whip', glyph: 'whip', meta: 'Handle · physics lash' },
    { name: 'chain', label: 'Chain', glyph: 'chain', meta: 'Chain links · rope physics' },
  ],
  // Your meshes (studio/studio_mesh.hpp) and textures (studio/studio_texture.hpp) are listed
  // here as the game client finds them.
  static: [],
  textures: [],
});

// Timeline: the animation's timeline (Play, the playhead, keys) in every mode, not only in Bones
// mode's content (the owner could not find Play outside it).
// AI: connecting an AI assistant, as World Architect's AI tab (one MCP server for both).
const TABS = Object.freeze(['Content', 'Timeline', 'History', 'Saves', 'AI']);
const AI_TAB = TABS.indexOf('AI');
const TIMELINE_TAB = TABS.indexOf('Timeline');
const SAVES_TAB = TABS.indexOf('Saves');
// A save's name: what the game client lets reach the file system (studio/studio_saves.hpp).
const SAVE_NAME = /^[A-Za-z0-9_-][A-Za-z0-9 _-]{0,46}[A-Za-z0-9_-]$|^[A-Za-z0-9_-]$/;
const SAVES_FOLDER = 'Documents\\KCDS\\Asset Studio';
// The keys a save's name is typed with (while the name field has them).
const LETTERS = Object.freeze('abcdefghijklmnopqrstuvwxyz0123456789'.split(''));
const TYPING_KEYS = Object.freeze([...LETTERS, 'minus', 'space', 'backspace', 'enter']);
const SAVE_NAME_LENGTH = 40;

// The project: kept by the game client under the studio's level's name.
const PROJECT_NAME = 'kcds_studio';
// The name an exported animation gets on the server (Animations.define, playAnimation).
const ANIMATION_NAME = 'kcds_studio_animation';
const PROJECT_VERSION = 1;
const UNDO_STEPS = 50;
// Ctrl+Z and Ctrl+Y, as in World Architect; F brings the camera back to the character; Space plays
// or pauses the animation; 1 to 5 choose the modes, as World Architect's numbers do.
const MODE_KEYS = Object.freeze(['1', '2', '3', '4', '5']);
// The views as in Blender (camera.cjs): numpad 1 front, 3 side, 7 top.
const VIEW_KEYS = Object.freeze({ numpad1: 'front', numpad3: 'side', numpad7: 'top' });
const SHORTCUTS = Object.freeze(['z', 'y', 'f', 'space', ...MODE_KEYS, ...Object.keys(VIEW_KEYS)]);
// While the animation plays (or a key is dragged), the screen shows its time this often.
const TIMELINE_REFRESH_MS = 100;
// A key's drag is not ended by a button that seems up this soon after the press (the press can
// come before the game says the button is down).
const DRAG_SETTLE_MS = 150;
// Dragging a ring of the turning gizmo across turns the joint this much a pixel.
const RING_DEGREES_PER_PIXEL = 0.5;
// Where the studio stands the character (studio/studio_scene.cpp, kStage) and the camera's place
// in front of it: 2.5 m away on the diagonal, a little above its chest.
const CHARACTER_CHEST = Object.freeze({ x: 32, y: 32, z: 33.1 });
// Where the character's feet stand (studio/studio_scene.cpp kStage on the floor at 32): an
// exported mesh stands that far from each player.
const STAGE_FEET = Object.freeze([32, 32, 32]);
// A lash lying on the floor lies this far above it (its thickness).
const LASH_ABOVE_FLOOR = 0.01;
// A lash key's pieces (as a lash track's: lash_motion.cjs TRACK_SEGMENTS), and the steps the
// LASH KEYS rows shape it by (degrees; metres for Sag).
const LASH_KEY_PIECES = 12;
const LASH_STEPS = Object.freeze({
  aimSide: 5,
  aimUp: 5,
  curl: 10,
  sway: 10,
  sag: 0.05,
  tipTwist: 15,
});
// Where your mesh stands: at the character's right side as the front camera sees it (0.9 m
// across the camera's view), at hip height so the first view shows it.
const MESH_PLACE = Object.freeze([32.64, 31.36, 33.0]);
const CAMERA_FRONT = Object.freeze({ x: 33.8, y: 33.8, z: 33.6 });

const ROPE_DEFAULT = Object.freeze({ segments: 16, length: 1.2 });
// The props the game client makes: their names, first settings and messages (studio_rope.hpp,
// studio_whip.hpp). A whip's segments and length are its lash's.
const PROPS = Object.freeze({
  rope: {
    label: 'Rope',
    kind: 'Physical prop · the game’s rope',
    first: ROPE_DEFAULT,
    make: 'studioRope',
    sweep: 'studioRopeSweep',
    off: 'studioRopeOff',
  },
  // A chain: the game's rope physics drawn with its chain links; the rope's details,
  // its end pin and physics.
  chain: {
    label: 'Chain',
    kind: 'Physical prop · the game’s chain links',
    first: Object.freeze({ segments: 24, length: 1.5 }),
    make: 'studioRope',
    look: 'chain',
    sweep: 'studioRopeSweep',
    off: 'studioRopeOff',
  },
  whip: {
    label: 'Whip',
    kind: 'The game’s whip handle · a physics lash',
    first: Object.freeze({ segments: 20, length: 1.6 }),
    make: 'studioWhip',
    sweep: 'studioWhipSweep',
    off: 'studioWhipOff',
  },
});
const ROPE_LIMITS = Object.freeze({ segments: [2, 64], length: [0.2, 20] });

// How the rope or the whip's lash moves (physical_props/rope_prop.hpp RopePhysics, sent as
// { op: 'studioPropPhysics', gravity, damping, mass, air, bend, wind }): each a step in Details.
const PHYSICS_FIELDS = Object.freeze([
  { key: 'gravity', label: 'Gravity', unit: 'm/s²', step: 0.5, range: [0, 30], first: 9.8 },
  { key: 'damping', label: 'Damping', unit: '', step: 0.1, range: [0, 5], first: 0.3 },
  { key: 'mass', label: 'Weight', unit: 'kg', step: 0.05, range: [0.05, 10], first: 0.5 },
  { key: 'air', label: 'Air drag', unit: '', step: 0.05, range: [0, 2], first: 0.1 },
  { key: 'bend', label: 'Bend limit', unit: '°', step: 5, range: [0, 90], first: 0 },
  { key: 'wind', label: 'Wind', unit: 'm/s', step: 1, range: [0, 20], first: 0 },
  // 0: the game's own stiffness.
  { key: 'stiffness', label: 'Stiffness', unit: '', step: 10, range: [0, 500], first: 0 },
  { key: 'friction', label: 'Friction', unit: '', step: 0.1, range: [0, 2], first: 0.5 },
  {
    key: 'thickness',
    label: 'Thickness',
    unit: 'm',
    step: 0.005,
    range: [0.005, 0.2],
    first: 0.02,
  },
  // 1: collides with the character, props and the floor; 2: the floor only (a lash swings past
  // the body); 0: falls through everything.
  { key: 'collide', label: 'Collide', unit: '', step: 1, range: [0, 2], first: 1 },
  // A whip's lash posed instead of moved by physics (physical_props/whip_lash.hpp), to animate a
  // swing exactly with prop keys: Shape 0 physics, 1 the posed shape alone (no gravity); Curl
  // and Sway how far it bends from the handle to the tip, up (or down) and sideways.
  { key: 'shape', label: 'Shape', unit: '', step: 0.1, range: [0, 1], first: 0, whip: true },
  { key: 'curl', label: 'Curl', unit: '°', step: 15, range: [-540, 540], first: 0, whip: true },
  { key: 'sway', label: 'Sway', unit: '°', step: 15, range: [-540, 540], first: 0, whip: true },
]);

// The physics fields a prop has: a whip's lash also has its posed shape.
function physicsFieldsOf(kind) {
  return PHYSICS_FIELDS.filter((field) => !field.whip || kind === 'whip');
}

// The studio's sky (studio/studio_sky.hpp: { op: 'studioSky', hour, preset, rain }): the game's
// weather presets the studio offers.
const WEATHERS = Object.freeze([
  { preset: 'cloudless_sunny', label: 'Clear' },
  { preset: 'overcast_rain', label: 'Overcast, rain' },
  { preset: 'storm', label: 'Storm' },
]);

function skyOf(given) {
  const hour = Number(given?.hour);
  const rain = Number(given?.rain);
  const known = WEATHERS.some((one) => one.preset === given?.preset);
  return {
    hour: Number.isInteger(hour) && hour >= 0 && hour <= 23 ? hour : 12,
    preset: known ? given.preset : WEATHERS[0].preset,
    rain: Number.isFinite(rain) ? Math.min(1, Math.max(0, Math.round(rain * 10) / 10)) : 0,
  };
}

// How the hand holds a whip's handle (physical_props/whip_prop.hpp set_grip, sent as
// { op: 'studioPropGrip', pitch, twist }): 90 degrees points it out of the fist.
const GRIP_FIELDS = Object.freeze([
  { key: 'pitch', label: 'Grip angle', unit: '°', step: 15, range: [0, 180], first: 90 },
  { key: 'twist', label: 'Grip twist', unit: '°', step: 15, range: [-180, 180], first: 0 },
]);

// A set of fields' values: a kept or given set within the fields' ranges, else their first.
function settingsOf(fields, given) {
  const settings = {};
  for (const field of fields) {
    const value = Number(given?.[field.key]);
    settings[field.key] = Number.isFinite(value) ? clamp(value, field.range) : field.first;
  }
  return settings;
}

function physicsOf(given) {
  return settingsOf(PHYSICS_FIELDS, given);
}

function clamp(value, [low, high]) {
  return Math.min(high, Math.max(low, value));
}

// Your mesh as cloth (studio/studio_mesh.hpp studioMeshCloth): its weight, a step in Details.
const CLOTH_MASS = Object.freeze({ step: 0.25, range: [0.25, 10], first: 1 });

// The cloth in a mesh state: { on, mass }.
function clothOf(state) {
  const mass = Number(state?.clothMass);
  return {
    on: state?.cloth === true,
    mass: Number.isFinite(mass) ? clamp(mass, CLOTH_MASS.range) : CLOTH_MASS.first,
  };
}

// The simple props the prop menu makes (shapes.cjs): each kind's first options and
// the lengths Details step (metres, 0.05 a step).
const SHAPE_KINDS = Object.freeze([
  { shape: 'box', first: { size: [0.5, 0.5, 0.5] }, lengths: ['Width', 'Depth', 'Height'] },
  { shape: 'cylinder', first: { radius: 0.25, height: 0.5 }, lengths: ['Radius', 'Height'] },
  { shape: 'sphere', first: { radius: 0.25 }, lengths: ['Radius'] },
  { shape: 'plane', first: { size: [1, 1] }, lengths: ['Width', 'Depth'] },
]);
const SHAPE_STEP = 0.05;
const SHAPE_COLOURS = Object.freeze([
  { label: 'Light grey', colour: [0.8, 0.8, 0.8] },
  { label: 'Wood', colour: [0.45, 0.3, 0.15] },
  { label: 'Iron', colour: [0.35, 0.36, 0.38] },
  { label: 'Stone', colour: [0.55, 0.53, 0.5] },
  { label: 'Red', colour: [0.6, 0.12, 0.1] },
  { label: 'Blue', colour: [0.15, 0.3, 0.6] },
  { label: 'Green', colour: [0.2, 0.45, 0.2] },
  { label: 'White', colour: [0.95, 0.95, 0.95] },
  { label: 'Black', colour: [0.08, 0.08, 0.08] },
]);

// A made shape's options as kept (shapes.cjs make takes them), or null when they are not one.
function shapeOptionsOf(given) {
  const kind = SHAPE_KINDS.find((one) => one.shape === given?.shape);
  if (!kind) {
    return null;
  }
  const options = {
    shape: kind.shape,
    ...kind.first,
    colour: SHAPE_COLOURS[0].colour,
    surface: 'mat_wood',
  };
  for (const key of ['size', 'radius', 'height', 'colour', 'surface']) {
    if (given[key] !== undefined && key in options) {
      options[key] = given[key];
    }
  }
  try {
    shapes.make('check', options);
  } catch {
    return null;
  }
  return options;
}

// Your mesh's body in the game's physics: none, solid (things hit it) or falling (a
// rigid body of `mass` kg that falls from where it is placed and rests).
const MESH_BODIES = Object.freeze([
  { kind: 'none', label: 'None: stands as placed' },
  { kind: 'solid', label: 'Solid: things hit it' },
  { kind: 'falls', label: 'Falls: a rigid body' },
]);
const BODY_MASS = Object.freeze({ range: [0.1, 200], first: 5 });

function bodyOf(state) {
  const kind = MESH_BODIES.some((one) => one.kind === state?.body) ? state.body : 'none';
  const mass = Number(state?.bodyMass);
  const kept = Number.isFinite(mass) ? clamp(mass, BODY_MASS.range) : BODY_MASS.first;
  return { kind, mass: Math.round(kept * 100) / 100 };
}

// How far the cursor goes sideways for one step of a value it drags in Details (stage pixels),
// as World Architect's.
const SCRUB_PX = 12;

// Recording a rope's simulation into keys (studio/studio_rope.hpp studioRopeRecord):
// how long, and the most keys kept (10 s at 30 a second).
const RECORD_SECONDS = [1, 10];
const MOST_KEYS = 301;

// Recorded keys as kept: { rate, frames: [[x, y, z, ...], ...] } (world metres, the rope's start
// first), or null when it is not that.
function keysOf(given) {
  const rate = Number(given?.rate);
  const frames = given?.frames;
  if (!Number.isFinite(rate) || rate <= 0 || !Array.isArray(frames) || frames.length === 0) {
    return null;
  }
  if (frames.length > MOST_KEYS) {
    return null;
  }
  const size = Array.isArray(frames[0]) ? frames[0].length : 0;
  const fits = (frame) =>
    Array.isArray(frame) &&
    frame.length === size &&
    frame.every((part) => typeof part === 'number' && Number.isFinite(part));
  if (size === 0 || size % 3 !== 0 || !frames.every(fits)) {
    return null;
  }
  return { rate, frames: frames.map((frame) => [...frame]) };
}

function every(milliseconds, callback) {
  const timer = setInterval(callback, milliseconds);
  return () => clearInterval(timer);
}

class StudioEditor {
  // api: { NativeUI, NoClip, Key, Controls, send (to the game client), log, gameUi?(shown),
  // every? }.
  constructor(api) {
    this.api = api;
    this.every = api.every ?? every;
    this.screen = null;
    this.shell = null;
    this.stopPolling = null;
    this.flying = false;
    this.mode = 'props';
    this.tool = 'select';
    this.folder = 'physical';
    this.page = 0;
    this.tab = 0;
    this.cameraSpeed = 6;
    // The scene: the character is always there; the rope when one was made.
    this.rope = null;
    this.selected = 'character';
    this.history = [];
    this.undoStack = [];
    this.redoStack = [];
    this.keptProject = null;
    this.keysDown = new Set();
    // Bones mode: the character's joints (from the game client) and the chosen one's id.
    this.joints = [];
    this.joint = -1;
    this.outlinerPage = 0;
    // The character's animation: keys of joint turns (animation.cjs).
    this.animation = new Animation();
    // The body's own animation (its idle): held while the editor is open, unless turned on.
    this.idle = false;
    // What the character wears (outfits.cjs): Henry's clothes at first.
    this.clothing = clothingOf(null);
    // The sky: noon, clear, no rain at first.
    this.sky = skyOf(null);
    // The timeline: the playhead (seconds), a key being dragged ({ key, time }) and when the
    // screen last showed the playhead.
    this.time = 0;
    this.drag = null;
    this.shownAt = 0;
    // View as character: the character's own view instead of the free camera, and where the
    // free camera was.
    this.characterView = false;
    this.freeCameraPose = null;
    // A ring of the turning gizmo being dragged: { axis, startX, start turn, animation before }.
    this.ringDrag = null;
    // Your meshes (studio/studio_mesh.hpp): their names, the one shown and where it stands
    // ({ at, turn, scale }: placing.cjs).
    this.meshes = [];
    this.mesh = null;
    this.meshPlace = placing.meshPlaceOf(null, MESH_PLACE);
    // Your textures (studio/studio_texture.hpp): their names, and the one on the mesh
    // ({ name: '' for the mesh's own material, tiling }).
    this.textures = [];
    this.meshTexture = { name: '', tiling: 1 };
    // Your mesh as cloth (physical_props/cloth.hpp): { on, mass in kg }.
    this.meshCloth = { on: false, mass: CLOTH_MASS.first };
    // Your mesh's body (studio/studio_mesh.hpp studioMeshBody): { kind, mass in kg }.
    this.meshBody = bodyOf(null);
    // The skinned mesh of yours the character wears ('' none).
    this.skin = '';
    // The simple props made here, by mesh name: their options (shapeOptionsOf), kept with the
    // project so Details can change them.
    this.shapes = {};
    // A drag of the props' gizmo (studio/studio_prop_gizmo.hpp): { thing, before, ...placing
    // startDrag }; and the gizmo last told to the game client.
    this.propDrag = null;
    this.gizmoSent = '';
    // Nothing of the editor drawn (setUiHidden): its panels, the gizmo, the grid, the bones.
    this.uiHidden = false;
    // The kit's script steps' pointer (pick, dragto), else null: the real pointer.
    this.testPointer = null;
    // The Saves tab (studio/studio_saves.hpp): the saves found, the name being typed, and
    // whether the name field has the keys.
    this.saves = [];
    this.saveName = '';
    this.typing = false;
    // What the keys type into: 'save' (the save's name) or 'search' (the content's search).
    this.typingInto = 'save';
    // The game's own objects and clips (studio/game_files.hpp): the page the game client last
    // answered for each, with its search; and the game clip playing on the character.
    this.gameFiles = { objects: gamePage(), clips: gamePage() };
    this.gameClip = '';
    this.gameClipPart = gameClipPartOf(null);
    this.gameClipState = null;
    // A bake of the game clip into keys (bakeGameClip): its parts while they come, one waiting
    // for the joints' names, and what the last one made.
    this.bakeParts = null;
    this.bakeWaiting = null;
    this.lastBake = null;
    // The whip's lash being recorded along the animation for posed keys (lashToKeys), and what
    // the last one made.
    this.lashBaking = null;
    this.lastLash = null;
    // Whether the game client has a lash shape from the keys now (sendLash).
    this.lashSent = false;
    // The game client's last answer to the assistant's gamefiles step.
    this.gameFilesAnswer = null;
    // The floor drawn (studio/studio_floor.hpp: the toolbar's Floor chip), and the grid on it.
    this.floor = true;
    this.grid = true;
    // Recording the rope's simulation into keys: how many seconds, whether a
    // recording runs, and whether the kept keys play in place of the simulation.
    this.recordSeconds = 3;
    this.recordingKeys = false;
    this.keysPlaying = false;
    // Which part of the rope the gizmo is on: its start (where it is held) or its pinned end.
    this.ropePart = 'start';
    // A value being dragged sideways in Details (click and hold, as in World Architect).
    this.scrub = null;
    // The camera's orbit (camera.cjs) once it turned round a focus, and a middle-button drag.
    this.orbit = null;
    this.cameraDrag = null;
    // Animation mode: the joint a new effect plays at, and its size.
    this.eventJoint = 'RightHand';
    this.eventScale = 1;
    this.message = 'Choose a prop in Content: it goes in the character’s right hand.';
  }

  isOpen() {
    return this.screen !== null;
  }

  open() {
    if (this.screen) {
      return false;
    }
    const flight = this.api.NoClip.enable({ mode: 'camera', input: false });
    if (flight === 'notAllowed') {
      this.api.log('Asset/Animation Studio: the free camera is not allowed');
      return false;
    }
    this.api.NoClip.setSpeed(this.cameraSpeed);
    // The flight starts at the player's eyes, inside the head: the studio looks at the body.
    this.focusCharacter();
    this.api.gameUi?.(false);
    this.api.Controls?.showCursor();
    this.api.Controls?.disable();
    this.screen = this.api.NativeUI.createScreen();
    this.screen.on('ready', () => this.build());
    this.screen.on('input', (action, phase) => this.input(action, phase));
    this.stopPolling = this.every(POLL_MS, () => this.poll());
    // The body stands still for the studio (studio/studio_pose.hpp).
    this.api.send({ op: 'studioIdle', on: this.idle });
    return true;
  }

  close() {
    if (!this.screen) {
      return false;
    }
    this.stopPolling?.();
    this.stopPolling = null;
    if (this.mode === 'bones') {
      // The bones are drawn only while the editor shows them.
      this.sendBones(false);
    }
    this.stopPlaying();
    // The player plays again: the body's own animation and no studio turns.
    this.api.send({ op: 'studioIdle', on: true });
    this.api.send({ op: 'studioPose', joints: [] });
    this.shell?.clear();
    this.screen.close();
    this.screen = null;
    this.shell = null;
    if (!this.flying) {
      this.api.Controls?.hideCursor();
    }
    this.flying = false;
    this.characterView = false;
    this.api.Controls?.enable();
    this.api.NoClip.disable();
    this.api.gameUi?.(true);
    return true;
  }

  build() {
    if (!this.screen) {
      return;
    }
    const handlers = {
      view: (x, y) => this.pressView(x, y),
      viewRelease: (x) => this.releaseView(x),
      camSpeed: () => this.turnCameraSpeed(),
      tool: (index) => this.useTool(TOOLS[index]?.key),
      snap: (index) => this.pressSnap(index),
      mode: (index) => this.useMode(MODES[index]?.key),
      undo: () => {
        this.undo();
        this.refresh();
      },
      redo: () => {
        this.redo();
        this.refresh();
      },
      play: () => this.toggleCharacterView(),
      outlinerSearch: () => {},
      outlinerFilter: () => {},
      outliner: (row) => this.pressOutliner(row),
      outlinerPage: (step) => this.turnOutlinerPage(step),
      detail: (row, side) => this.pressDetail(row, side),
      tab: (index) => this.pressTab(index),
      issue: () => {},
      folder: (index) => this.pressFolder(index),
      contentSearch: () => this.pressContentSearch(),
      tile: (index) => this.pressTile(index),
      page: (step) => this.turnPage(step),
      dockPress: (x, y) => this.pressTimeline(x, y),
      dockRelease: (x, y) => this.releaseTimeline(x, y),
      // The editor's name in the menu bar opens the Saves tab, as World Architect's does.
      architect: () => this.pressTab(SAVES_TAB),
      leave: () => this.leave(),
      aiCopy: (row) => this.copyAi(row),
      saveName: () => this.startTyping('save'),
      saveAs: () => this.saveAs(),
      saveRow: (row, action) => this.pressSave(row, action),
    };
    this.screen.setVisible(false);
    this.shell = new Shell(
      this.screen.root,
      handlers,
      GLYPHS,
      (message) => this.api.send(message),
      timeline.renderTimeline,
    );
    this.shell.build();
    if (this.mode === 'bones') {
      this.sendBones(true);
    }
    this.rebuildProject();
    this.refresh();
    this.screen.setVisible(true);
    this.screen.focus();
    // The game client's loading screen covered the world until now (architect/local_mode.hpp).
    this.api.send({ op: 'editorShown' });
  }

  // Escape lets go of the selection.
  input(action, phase) {
    if (phase !== 'press' || action !== 'kcds_ui_back') {
      return;
    }
    // Escape: stop typing a save's name first.
    if (this.typing) {
      this.typing = false;
      this.refresh();
      return;
    }
    this.selected = null;
    this.message = 'Nothing selected.';
    this.refresh();
  }

  // The right mouse button held looks and flies, as in World Architect.
  poll() {
    const { Key, NoClip } = this.api;
    // The character's own view has no free camera to fly.
    const looking = Key.isDown('mouse2') && !this.characterView;
    if (looking && !this.flying) {
      this.flying = true;
      // A flight leaves the orbit: the next one turns round the focus from where it lands.
      this.orbit = null;
      this.api.Controls?.hideCursor();
      NoClip.setInputEnabled(true);
      this.screen?.blur();
    } else if (!looking && this.flying) {
      this.flying = false;
      NoClip.setInputEnabled(false);
      this.api.Controls?.showCursor();
      this.screen?.focus();
    }
    if (this.animation.playing) {
      this.playFrame();
    }
    if (this.drag) {
      // The drag ends when the button is up (the game's release event came at once or not at
      // all on a held key: the key let go and did not follow).
      if (this.api.Key.isDown('mouse1') || this.now() - this.drag.startedAt < DRAG_SETTLE_MS) {
        this.followDrag();
      } else {
        const pointer = this.shell?.dockPointer();
        this.endKeyDrag(pointer ? pointer.x : null);
      }
    }
    if (this.ringDrag) {
      this.followRing();
    }
    if (this.propDrag) {
      this.followPropDrag();
    }
    if (this.scrub) {
      this.scrubOn();
    }
    if (!this.flying && !this.characterView) {
      this.followCameraDrag();
    }
    if (this.flying) {
      this.keysDown.clear();
      return;
    }
    if (this.typing) {
      this.pollTyping();
      return;
    }
    for (const key of SHORTCUTS) {
      const down = Key.isDown(key);
      if (down && !this.keysDown.has(key)) {
        this.keysDown.add(key);
        if (MODE_KEYS.includes(key) && !Key.isDown('ctrl')) {
          this.useMode(MODES[MODE_KEYS.indexOf(key)]?.key);
        } else if (key === 'f') {
          this.frameChosen();
        } else if (VIEW_KEYS[key]) {
          this.showView(VIEW_KEYS[key]);
        } else if (key === 'space') {
          if (this.mode === 'bones' || this.tab === TIMELINE_TAB) {
            this.setPlaying(!this.animation.playing);
            this.refresh();
          }
        } else if (Key.isDown('ctrl')) {
          if (key === 'z') {
            this.undo();
          } else {
            this.redo();
          }
          this.refresh();
        }
      } else if (!down) {
        this.keysDown.delete(key);
      }
    }
  }

  // View as character: the character's own first-person view (the game's camera; the free camera
  // stops), the editor still open; again, the free camera comes back where it was.
  toggleCharacterView() {
    const { NoClip, Vector3 } = this.api;
    if (!this.characterView) {
      const at = NoClip.getPosition();
      const forward = NoClip.getForward?.();
      this.freeCameraPose = at && forward ? { at, forward } : null;
      if (this.flying) {
        this.flying = false;
        this.api.Controls?.showCursor();
        this.screen?.focus();
      }
      NoClip.disable();
      this.characterView = true;
      this.message = 'The character’s own view. Press Free camera to fly again.';
    } else {
      NoClip.enable({ mode: 'camera', input: false });
      NoClip.setSpeed(this.cameraSpeed);
      this.characterView = false;
      const pose = this.freeCameraPose;
      if (pose) {
        NoClip.setPose(
          new Vector3(pose.at.x, pose.at.y, pose.at.z),
          new Vector3(pose.forward.x, pose.forward.y, pose.forward.z),
        );
      } else {
        this.focusCharacter();
      }
      this.message = 'Free camera: hold the right mouse button to fly.';
    }
    this.refresh();
  }

  // The camera in front of the character, looking at it (at the start and on F). One pose: the
  // position and the aim go down together.
  // ---- The camera as in Blender (camera.cjs) ----

  // What the camera turns round: the chosen prop or mesh, else the character's chest.
  focusPoint() {
    if (this.selected === 'rope' && this.rope && !this.rope.hold.joint) {
      return [...this.rope.hold.at];
    }
    if (this.selected === 'mesh' && this.mesh && !this.meshPlace.joint) {
      return [...this.meshPlace.at];
    }
    return [CHARACTER_CHEST.x, CHARACTER_CHEST.y, CHARACTER_CHEST.z];
  }

  setOrbit(orbit) {
    const { Vector3, NoClip } = this.api;
    const { place, forward } = camera.poseOf(orbit);
    this.orbit = orbit;
    NoClip.setPose(new Vector3(...place), new Vector3(...forward));
  }

  // The middle mouse button held over the view: a drag orbits round the focus, with Shift pans,
  // with Ctrl zooms (up nearer, down farther).
  followCameraDrag() {
    const { Key, NoClip } = this.api;
    const down = Key.isDown('mouse3');
    const pointer = down ? this.pointer() : null;
    if (!down || !pointer) {
      this.cameraDrag = null;
      return;
    }
    if (!this.cameraDrag) {
      const at = NoClip.getPosition?.();
      const target = this.orbit?.target ?? this.focusPoint();
      const orbit = at
        ? camera.orbitOf([at.x, at.y, at.z], target)
        : camera.viewOf('front', target);
      this.cameraDrag = { orbit, x: pointer.x, y: pointer.y };
      return;
    }
    const how = Key.isDown('shift') ? 'pan' : Key.isDown('ctrl') ? 'zoom' : 'orbit';
    const dx = pointer.x - this.cameraDrag.x;
    const dy = pointer.y - this.cameraDrag.y;
    if (dx === 0 && dy === 0) {
      return;
    }
    this.cameraDrag = {
      orbit: camera.dragged(this.cameraDrag.orbit, how, dx, dy),
      x: pointer.x,
      y: pointer.y,
    };
    this.setOrbit(this.cameraDrag.orbit);
  }

  // Front, side or top (numpad 1, 3, 7) of the focus.
  showView(name) {
    if (this.characterView) {
      this.toggleCharacterView();
    }
    this.setOrbit(camera.viewOf(name, this.focusPoint()));
    this.message = `${name[0].toUpperCase()}${name.slice(1)} view (numpad 1 front, 3 side, 7 top).`;
    this.refresh();
  }

  // F: the camera to what is chosen (a free prop or the mesh), else in front of the character.
  frameChosen() {
    const focus = this.focusPoint();
    const chosen =
      (this.selected === 'rope' && this.rope && !this.rope.hold.joint) ||
      (this.selected === 'mesh' && this.mesh);
    if (!chosen) {
      this.orbit = null;
      this.focusCharacter();
      this.refresh();
      return;
    }
    if (this.characterView) {
      this.toggleCharacterView();
    }
    this.setOrbit({ target: focus, yaw: Math.PI / 4, pitch: 0.35, distance: 2 });
    this.message = 'The camera on what is chosen (F). Middle button: orbit, Shift pan, Ctrl zoom.';
    this.refresh();
  }

  focusCharacter() {
    if (this.characterView) {
      this.toggleCharacterView();
    }
    const { Vector3, NoClip } = this.api;
    const position = new Vector3(CAMERA_FRONT.x, CAMERA_FRONT.y, CAMERA_FRONT.z);
    const forward = new Vector3(
      CHARACTER_CHEST.x - CAMERA_FRONT.x,
      CHARACTER_CHEST.y - CAMERA_FRONT.y,
      CHARACTER_CHEST.z - CAMERA_FRONT.z,
    );
    NoClip.setPose(position, forward);
    this.message = 'Camera in front of the character (F). Hold the right mouse button to fly.';
  }

  refresh() {
    if (this.shell && !this.uiHidden) {
      this.shell.render(this.view());
    }
    this.sendGizmo();
  }

  // The editor drawn or not: hidden, only the scene shows (to watch or record it): no panels or
  // their texts, no gizmo, no grid, no bones. Shown again, all of it is as it was.
  setUiHidden(hidden) {
    if (this.uiHidden === hidden) {
      return { hidden };
    }
    this.uiHidden = hidden;
    if (hidden) {
      this.shell?.hide();
    }
    this.api.send({ op: 'studioGrid', on: hidden ? false : this.grid });
    if (this.mode === 'bones') {
      this.sendBones(!hidden);
    }
    this.refresh();
    return { hidden };
  }

  // ---- Actions ----

  useTool(key) {
    if (key) {
      this.tool = key;
      this.refresh();
    }
  }

  useMode(key) {
    const mode = MODES.find((one) => one.key === key);
    if (!mode) {
      return;
    }
    if (mode.later) {
      this.message = `${mode.label} comes later.`;
      this.refresh();
      return;
    }
    const wasBones = this.mode === 'bones';
    this.mode = key;
    this.folder = mode.folders[0] ?? this.folder;
    this.page = 0;
    this.outlinerPage = 0;
    if (key === 'bones') {
      // The skeleton view (studio/studio_skeleton.hpp): the character's bones over its body.
      this.sendBones(true);
      this.message = 'Bones: press a joint in the view or the outliner, then turn it in Details.';
    } else {
      if (wasBones) {
        this.sendBones(false);
      }
      if (key === 'textures') {
        // Your pictures as they are now in KCDS\studio\textures.
        this.api.send({ op: 'studioTextureList' });
        if (this.mesh) {
          this.selected = 'mesh';
        }
      }
      const messages = {
        physics: 'Physics: choose the rope, then its settings in Details.',
        textures: 'Textures: press one to put it on your mesh (Props › Static props shows one).',
        animation:
          'Animation: set the playhead, then press an effect or a sound: it plays there each time.',
      };
      this.message =
        messages[key] ?? 'Choose a prop in Content: it goes in the character’s right hand.';
    }
    this.refresh();
  }

  // The character's joints from the game client (studio/studio_skeleton.hpp): names and parent
  // ids in the skeleton's order.
  jointsKept(names, parents) {
    this.joints = names.map((name, id) => ({
      id,
      name: String(name),
      parent: Number.isInteger(parents[id]) ? parents[id] : -1,
    }));
    // A bake that waited for the joints' names becomes the animation now.
    if (this.bakeWaiting) {
      const baked = this.bakeWaiting;
      this.bakeWaiting = null;
      this.sendBones(this.mode === 'bones');
      this.useBake(baked);
    }
    this.refresh();
  }

  // ---- A game clip baked into keys (clip_bake.cjs, studio/studio_game_clip.hpp) ----

  // One round of the part playing recorded by the game client and made the animation.
  bakeGameClip() {
    if (!this.gameClip || !(this.gameClipState?.seconds > 0)) {
      this.message = 'Play a game clip first: its part is baked into keys.';
      return;
    }
    this.bakeParts = { path: this.gameClip, parts: [] };
    this.api.send({ op: 'studioGameClipBake' });
    const { from, to } = this.gameClipPart;
    const seconds = ((to - from) * this.gameClipState.seconds).toFixed(1);
    this.message = `Baking: one round of the part (${seconds} s) is recorded…`;
    this.refresh();
  }

  // A part of the bake from the game client; with the last one the keys are made.
  gameClipBakedKept(data) {
    const bake = this.bakeParts;
    if (!bake || data.path !== bake.path) {
      return;
    }
    if (data.failed) {
      this.bakeParts = null;
      this.message = 'The bake failed: play the clip again from the start and bake.';
      this.refresh();
      return;
    }
    bake.parts[Number(data.part)] = data;
    const count = Number(data.parts);
    if (bake.parts.filter(Boolean).length < count) {
      return;
    }
    this.bakeParts = null;
    const first = bake.parts[0];
    const baked = bakeClip({
      seconds: first.seconds,
      names: first.names,
      parents: first.parents,
      idle: first.idle,
      idleHips: first.idleHips,
      frames: bake.parts.flatMap((part) => part.frames ?? []),
    });
    if (baked.error) {
      this.message = `The bake failed: ${baked.error}.`;
      this.refresh();
      return;
    }
    this.lastBake = {
      clip: bake.path,
      loopFrom: baked.loopFrom,
      loopTo: baked.loopTo,
      keys: baked.keys.length,
      joints: baked.joints,
      seam: baked.seam,
    };
    if (this.joints.length === 0) {
      // The joints' names come with the skeleton's view.
      this.bakeWaiting = baked;
      this.sendBones(true);
      return;
    }
    this.useBake(baked);
  }

  // The baked keys as the animation (one step to undo); the game clip stops, the animation plays.
  useBake(baked) {
    this.playGameClip('');
    const result = this.setAnimation(
      JSON.stringify({ keys: baked.keys, blend: baked.blend, loop: true, play: true }),
    );
    if (result.error) {
      this.message = `The bake failed: ${result.error}.`;
    } else {
      const span = (baked.loopTo - baked.loopFrom).toFixed(2);
      this.message =
        `Baked: ${result.keys} keys, ${baked.joints} joints, the loop ${span} s ` +
        `(${baked.loopFrom}–${baked.loopTo} s of the part); legs included, in place.`;
    }
    this.refresh();
  }

  chooseJoint(id) {
    const joint = this.joints[id];
    if (!joint) {
      return;
    }
    this.joint = id;
    this.sendBones(true);
    this.message = `${joint.name} chosen: turn it in Details.`;
    // The outliner shows the page with the chosen joint.
    this.outlinerPage = Math.floor(id / outlinerRows());
  }

  // The skeleton view on or off, with the chosen joint and where the editor shows the world
  // ([left, bottom, right, top], -1 to 1): the bones are drawn only there.
  sendBones(wanted) {
    // Not while the editor is hidden (setUiHidden): only the scene shows then.
    const on = wanted && !this.uiHidden;
    const ring = on && this.ringDrag ? this.ringDrag.axis : -1;
    this.api.send({
      op: 'studioBones',
      on,
      joint: on ? this.joint : -1,
      view: this.viewRect(),
      ring,
    });
  }

  // Where the editor shows the world on the screen: [left, bottom, right, top], -1 to 1.
  viewRect() {
    const stage = this.screen?.stage?.() ?? STAGE;
    const { view } = LAYOUT;
    return [
      (view.x / stage.width) * 2 - 1,
      1 - ((view.y + view.height) / stage.height) * 2,
      ((view.x + view.width) / stage.width) * 2 - 1,
      1 - (view.y / stage.height) * 2,
    ].map((part) => Math.round(part * 10000) / 10000);
  }

  // The line into the world through a place on the stage: { from, direction }, or null.
  lineAt(x, y) {
    const stage = this.screen?.stage?.() ?? STAGE;
    const from = this.api.NoClip.getPosition?.();
    const direction = this.api.NoClip.screenLine?.(
      (x / stage.width) * 2 - 1,
      1 - (y / stage.height) * 2,
    );
    if (!from || !direction) {
      return null;
    }
    return { from: [from.x, from.y, from.z], direction: [direction.x, direction.y, direction.z] };
  }

  // A press in the world's view: in Bones mode the game client picks the joint nearest it
  // (studioPickJoint, answered in jointPicked); else the prop or mesh there, or a handle of the
  // props' gizmo (studioPickProp, answered in propPicked).
  pressView(x, y) {
    const { view } = LAYOUT;
    const inside =
      x >= view.x && x < view.x + view.width && y >= view.y && y < view.y + view.height;
    if (!inside || this.flying) {
      return;
    }
    const line = this.lineAt(x, y);
    if (!line) {
      return;
    }
    const op = this.mode === 'bones' ? 'studioPickJoint' : 'studioPickProp';
    this.api.send({ op, from: line.from, direction: line.direction });
  }

  releaseView(x) {
    if (this.propDrag) {
      this.releasePropDrag();
    } else {
      this.releaseRing(x);
    }
  }

  // ---- The props' gizmo: move, turn and size the prop and your mesh in the view ----

  // What the gizmo is on: the chosen prop or mesh outside Bones mode, else nothing.
  gizmoThing() {
    if (this.mode === 'bones' || this.mode === 'animation' || this.characterView) {
      return '';
    }
    if (this.selected === 'rope' && this.rope) {
      // The rope's pinned end when it was picked, else its start.
      return this.ropePart === 'end' && this.rope.end?.pinned ? 'end' : 'prop';
    }
    return this.selected === 'mesh' && this.mesh ? 'mesh' : '';
  }

  // The gizmo's tool: Select moves too; Scale sizes only a mesh; the rope's end only moves.
  gizmoTool() {
    if (this.gizmoThing() === 'end') {
      return 'move';
    }
    if (this.tool === 'rotate') {
      return 'rotate';
    }
    return this.tool === 'scale' && this.gizmoThing() === 'mesh' ? 'scale' : 'move';
  }

  // The gizmo to the game client when it changed.
  sendGizmo() {
    const message = {
      op: 'studioPropGizmo',
      thing: this.uiHidden ? null : this.gizmoThing(),
      tool: this.gizmoTool(),
      axis: this.propDrag ? this.propDrag.axis : -1,
      prop: Boolean(this.rope) && this.mode !== 'bones',
      view: this.viewRect(),
    };
    // The rope's pinned end, so it is picked and moved like the prop (only the start was).
    if (this.rope?.end?.pinned && this.mode !== 'bones') {
      message.end =
        this.propDrag?.thing === 'end' && this.propDrag.now
          ? this.propDrag.now.at
          : this.rope.end.at;
    }
    const text = JSON.stringify(message);
    if (text !== this.gizmoSent) {
      this.gizmoSent = text;
      this.api.send(message);
    }
  }

  // The game client's answer to a press: the thing there and the handle pressed (-1 for none),
  // and where the thing is now.
  propPicked(thing, axis, at, turn) {
    if (thing === 'prop' && this.rope) {
      this.selected = 'rope';
      this.ropePart = 'start';
    } else if (thing === 'end' && this.rope?.end?.pinned) {
      this.selected = 'rope';
      this.ropePart = 'end';
    } else if (thing === 'mesh' && this.mesh) {
      this.selected = 'mesh';
    } else {
      this.message = 'Nothing there: press on the prop, your mesh or a handle of the gizmo.';
      this.refresh();
      return;
    }
    if (axis >= 0 && axis <= 2) {
      this.startPropDrag(thing, axis, at, turn);
    } else {
      const names = { prop: PROPS[this.rope?.kind ?? 'rope'].label, end: 'Rope end' };
      this.message = `${names[thing] ?? this.mesh} chosen: drag a handle of the gizmo.`;
    }
    this.refresh();
  }

  // A press on a handle: dragging moves, turns or sizes the thing, one undo step. A prop held in
  // a hand is set free where it is.
  startPropDrag(thing, axis, at, turn) {
    const pointer = this.pointer();
    if (!pointer) {
      return;
    }
    // In a hand the prop starts where the game client found it; free, where the editor keeps it.
    let place = { ...this.meshPlace };
    if (thing === 'mesh' && (this.meshPlace.joint || this.meshKeysOn)) {
      // A mesh held in a joint is set free where it is, as a held prop; one the mesh keys move
      // starts where they put it.
      const found = placing.holdOf({ at, turn });
      place = { at: found.at, turn: found.turn, scale: this.meshPlace.scale, joint: '' };
    } else if (thing === 'prop') {
      const found = placing.holdOf({ at, turn });
      place = this.rope.hold.joint ? found : { ...this.rope.hold };
    } else if (thing === 'end') {
      place = { at: [...this.rope.end.at], turn: [0, 0, 0] };
    }
    const tool = this.gizmoTool();
    this.propDrag = {
      thing,
      before: this.snapshot({ label: '' }),
      ...placing.startDrag(tool, axis, place, pointer, this.lineAt(pointer.x, pointer.y)),
    };
    const verb = { move: 'Moving', rotate: 'Turning', scale: 'Sizing' }[tool];
    this.message = `${verb} along ${'XYZ'[axis]}: drag, then let go.`;
  }

  // The place the drag gives now, shown live (not yet an undo step).
  followPropDrag() {
    const pointer = this.pointer();
    if (!pointer) {
      return;
    }
    const place = placing.dragPlace(this.propDrag, pointer, this.lineAt(pointer.x, pointer.y));
    this.propDrag.now = place;
    this.showPlace(this.propDrag.thing, place);
  }

  // A place shown in the game: the prop free there, or the mesh there.
  showPlace(thing, place) {
    if (thing === 'prop') {
      this.api.send({ op: 'studioPropHold', joint: '', at: place.at, turn: place.turn });
    } else if (thing === 'end') {
      this.api.send({ op: 'studioRopeEnd', on: true, at: place.at });
      this.sendGizmo();
    } else {
      this.api.send({ op: 'studioMeshPlace', at: place.at, turn: place.turn, scale: place.scale });
    }
  }

  releasePropDrag() {
    const drag = this.propDrag;
    this.propDrag = null;
    const place = drag.now;
    if (!place) {
      this.refresh();
      return;
    }
    const names = { prop: PROPS[this.rope?.kind ?? 'rope'].label, end: 'Rope end' };
    const label = `${names[drag.thing] ?? this.mesh} ${
      { move: 'moved', rotate: 'turned', scale: 'sized' }[drag.tool]
    }`;
    if (drag.thing === 'prop' && this.rope) {
      this.commit({ ...this.rope, hold: { joint: '', at: place.at, turn: place.turn } }, label);
    } else if (drag.thing === 'end' && this.rope?.end) {
      this.commit({ ...this.rope, end: { ...this.rope.end, at: place.at } }, label);
    } else if (drag.thing === 'mesh' && this.mesh) {
      // Dragged, the mesh is free in the scene where it was let go (a held one is set free).
      this.commitMesh({ ...this.meshState(), ...place, joint: '' }, label);
    }
    this.message = `${label}: ${place.at.map((part) => part.toFixed(2)).join(' ')}`;
    this.refresh();
  }

  jointPicked(id, ring = -1) {
    if (ring >= 0 && id === this.joint && this.joints[id]) {
      this.startRing(ring);
    } else if (id >= 0 && this.joints[id]) {
      this.chooseJoint(id);
    } else {
      this.message = 'No joint there: press on a ring of the skeleton.';
    }
    this.refresh();
  }

  // The pointer on the stage while it is over the world's view; the kit's script steps
  // (pick, dragto) stand in for it in tests.
  pointer() {
    return this.testPointer ?? this.shell?.viewPointer() ?? null;
  }

  // ---- The turning gizmo: a ring of the chosen joint dragged across ----

  // A press on a ring: dragging across turns the joint about that axis, one undo step.
  startRing(axis) {
    const pointer = this.pointer();
    if (!pointer) {
      return;
    }
    this.stopPlaying();
    const before = this.animation.toJSON();
    // Auto key: the turn goes into a key at the playhead.
    if (this.keyHere() < 0) {
      this.animation.addKeyAt(this.time);
    }
    this.animation.key = this.keyHere();
    this.ringDrag = {
      axis,
      startX: pointer.x,
      start: [...this.animation.turnOf(this.joint)],
      before,
    };
    this.sendBones(true);
    this.message = `Turning ${this.joints[this.joint].name} about ${'XYZ'[axis]}: drag across.`;
  }

  // The turn the pointer's way across gives: half a degree a pixel, in whole degrees.
  ringTurn(x) {
    const drag = this.ringDrag;
    const turn = [...drag.start];
    turn[drag.axis] = turn[drag.axis] + Math.round((x - drag.startX) * RING_DEGREES_PER_PIXEL);
    return turn;
  }

  followRing() {
    const pointer = this.pointer();
    if (!pointer) {
      return;
    }
    const turn = this.ringTurn(pointer.x);
    const now = this.animation.turnOf(this.joint);
    if (turn.every((part, axis) => Math.round(part) === Math.round(now[axis]))) {
      return;
    }
    this.animation.setTurn(this.joint, turn);
    this.sendPose();
    if (this.now() - this.shownAt >= TIMELINE_REFRESH_MS) {
      this.shownAt = this.now();
      this.refresh();
    }
  }

  releaseRing(x) {
    const drag = this.ringDrag;
    if (!drag) {
      return;
    }
    this.animation.setTurn(this.joint, this.ringTurn(x));
    this.ringDrag = null;
    const name = this.joints[this.joint]?.name ?? 'Joint';
    const label = `${name} turned`;
    this.undoStack.push({
      scene: this.rope ? { ...this.rope } : null,
      animation: drag.before,
      label,
    });
    this.undoStack.length = Math.min(this.undoStack.length, UNDO_STEPS);
    this.redoStack = [];
    this.note(label);
    this.sendPose();
    this.sendBones(true);
    this.save();
    const turn = this.animation.turnOf(this.joint);
    this.message = `${name}: ${turn.map((part) => `${part}°`).join(' ')}`;
    this.refresh();
  }

  // ---- The animation: joint turns in keys (animation.cjs) ----

  // The pose at the playhead to the game client, and the prop as its prop keys set it there.
  sendPose() {
    this.api.send(this.animation.messageAt(this.time));
    this.sendLash(this.time);
    this.applyPropKeys(this.time);
    this.applyMeshKeys(this.time);
  }

  // The key under the playhead, or -1 (turning a joint there adds one).
  keyHere() {
    return this.animation.keyAt(this.time);
  }

  // The playhead to `time`: the body shows the animation there; a key there is chosen.
  setTime(time) {
    this.stopPlaying();
    this.time = Math.max(0, Math.round(time * 100) / 100);
    const here = this.keyHere();
    if (here >= 0) {
      this.animation.key = here;
    }
    this.sendPose();
    this.message =
      here >= 0
        ? `Key ${here + 1} at ${this.time.toFixed(2)} s.`
        : `${this.time.toFixed(2)} s: turning a joint here adds a key.`;
  }

  // One change of the animation: the animation before goes on the undo stack.
  changeAnimation(label, change) {
    const before = this.animation.toJSON();
    if (!change()) {
      return false;
    }
    // A lash posed by hand plays as keyed: its track baked again for the animation as it is now.
    this.animation.bakeLashKeys();
    this.undoStack.push({ scene: this.rope ? { ...this.rope } : null, animation: before, label });
    this.undoStack.length = Math.min(this.undoStack.length, UNDO_STEPS);
    this.redoStack = [];
    this.note(label);
    this.sendPose();
    this.save();
    return true;
  }

  // ---- Events (studio/studio_event.hpp): effects and sounds at times of the animation ----

  // Plays an event now in the studio.
  playEvent(event) {
    const message = { op: 'studioEvent', kind: event.kind, name: event.name };
    if (event.kind === 'effect') {
      message.joint = event.joint ?? '';
      message.scale = event.scale ?? 1;
    }
    this.api.send(message);
  }

  // An effect or a sound at the playhead (an effect at the joint chosen in Animation mode), one
  // undo step; it plays once at once so it can be seen or heard.
  addEventHere(kind, name) {
    const event = { time: this.time, kind, name };
    if (kind === 'effect') {
      event.joint = this.eventJoint;
      event.scale = this.eventScale;
    }
    const short = name.split('.').at(-1);
    const added = this.changeAnimation(`Event ${short} at ${this.time.toFixed(2)} s`, () =>
      this.animation.addEvent(event),
    );
    if (added) {
      this.playEvent(eventOf(event));
      this.message = `${short} at ${this.time.toFixed(
        2,
      )} s: it plays each time the playhead passes.`;
    } else {
      this.message = 'That event could not be added (at most 32).';
    }
  }

  removeEvent(index) {
    const event = this.animation.events[index];
    if (!event) {
      return;
    }
    const short = event.name.split('.').at(-1);
    this.changeAnimation(`Event ${short} removed`, () => this.animation.removeEvent(index));
    this.message = `${short} removed.`;
  }

  // The rows of Animation mode's Details: the events, the joint and size for new effects, and
  // the playhead.
  // The game's clip on the character (Game clips): where it is, the part of it that plays, in
  // place or moving, looped or once.
  gameClipRows() {
    if (!this.gameClip) {
      return [];
    }
    const part = this.gameClipPart;
    const state = this.gameClipState;
    const seconds = state?.seconds > 0 ? state.seconds : 0;
    const at = (share) =>
      seconds ? `${(share * seconds).toFixed(2)} s` : `${Math.round(share * 1000) / 10} %`;
    // A step of a quarter of a second (of a hundredth while the clip's length is not known yet).
    const step = seconds ? 0.25 / seconds : 0.01;
    const time = state && state.time >= 0 ? state.time : null;
    return [
      { label: 'GAME CLIP' },
      {
        label: 'Clip',
        value: this.gameClip
          .split('/')
          .at(-1)
          .replace(/\.caf$/i, ''),
      },
      {
        label: 'Time',
        value: time === null ? '…' : `${at(time)}${seconds ? ` of ${seconds.toFixed(1)} s` : ''}`,
      },
      {
        label: 'From',
        value: at(part.from),
        step: (dir) => this.setGameClipPart({ from: part.from + dir * step }),
      },
      {
        label: 'To',
        value: at(part.to),
        step: (dir) => this.setGameClipPart({ to: part.to + dir * step }),
      },
      {
        label: 'Part',
        value: 'From here',
        press: () => time !== null && this.setGameClipPart({ from: time }),
      },
      {
        label: '',
        value: 'To here',
        press: () => time !== null && this.setGameClipPart({ to: time }),
      },
      {
        label: 'Whole clip',
        value: 'Play all of it',
        press: () => this.setGameClipPart({ from: 0, to: 1 }),
      },
      {
        label: 'Top body only',
        toggle: part.topBody,
        press: () => this.setGameClipPart({ topBody: !part.topBody }),
      },
      // The part made the studio's own animation (clip_bake.cjs): keys of the whole body, legs
      // included, its best loop, in place and facing forward; it plays on players as every
      // studio animation does.
      {
        label: 'Bake',
        value: this.bakeParts ? 'Baking…' : 'Bake to keys',
        press: () => this.bakeGameClip(),
      },
      {
        label: 'In place',
        toggle: part.inPlace,
        press: () => this.setGameClipPart({ inPlace: !part.inPlace }),
      },
      { label: 'Loop', toggle: part.loop, press: () => this.setGameClipPart({ loop: !part.loop }) },
      { label: 'Stop', value: 'Stop the clip', press: () => this.playGameClip('') },
    ];
  }

  eventDetails() {
    const { events } = this.animation;
    const rows = [
      ...this.gameClipRows(),
      { label: 'NEW EFFECT' },
      {
        label: 'At joint',
        value: this.eventJoint,
        step: (dir) => {
          const index = EVENT_JOINTS.indexOf(this.eventJoint);
          this.eventJoint = EVENT_JOINTS[(index + dir + EVENT_JOINTS.length) % EVENT_JOINTS.length];
        },
      },
      {
        label: 'Size',
        value: `${this.eventScale} ×`,
        step: (dir) => {
          this.eventScale = clamp(
            Math.round(this.eventScale * (dir > 0 ? 2 : 0.5) * 100) / 100,
            [0.125, 8],
          );
        },
      },
      { label: 'PLAYHEAD' },
      {
        label: 'Time',
        value: `${this.time.toFixed(2)} s`,
        step: (dir) => this.setTime(this.time + dir * 0.1),
      },
      {
        label: 'Play',
        toggle: this.animation.playing,
        press: () => this.setPlaying(!this.animation.playing),
      },
      { label: `EVENTS  ${events.length}` },
    ];
    events.slice(0, 10).forEach((event, index) => {
      const where = event.kind === 'effect' && event.joint ? ` · ${event.joint}` : '';
      rows.push({
        label: `${event.time.toFixed(2)} s`,
        value: `${event.name.split('.').at(-1)}${where}  ✕`,
        press: () => this.removeEvent(index),
      });
    });
    if (events.length === 0) {
      rows.push({ label: 'None yet', value: 'Press an effect or a sound' });
    }
    return {
      icon: 'play',
      title: 'Animation events',
      kind: 'Effects and sounds as the animation plays',
      rows,
    };
  }

  turnJoint(axis, direction) {
    const joint = this.joints[this.joint];
    if (!joint) {
      this.message = 'Choose a joint first.';
      return;
    }
    this.stopPlaying();
    let turn = null;
    // Auto key: a turn where there is no key adds one there first (with the pose there).
    this.changeAnimation(`${joint.name} turned`, () => {
      if (this.keyHere() < 0) {
        this.animation.addKeyAt(this.time);
      }
      this.animation.key = this.keyHere();
      turn = this.animation.turn(joint.id, axis, direction);
      return true;
    });
    this.message = `${joint.name}: ${turn.map((part) => `${part}°`).join(' ')}`;
  }

  resetJoint() {
    const joint = this.joints[this.joint];
    if (joint && this.keyHere() >= 0) {
      this.stopPlaying();
      this.changeAnimation(`${joint.name} back to its pose`, () => {
        this.animation.key = this.keyHere();
        this.animation.resetJoint(joint.id);
        return true;
      });
    }
  }

  chooseKey(index) {
    const key = this.animation.keys[index];
    if (key) {
      this.animation.key = index;
      this.setTime(key.time);
    }
  }

  // A key at the playhead with the pose there (+ Key).
  addKey() {
    this.stopPlaying();
    if (this.changeAnimation('Key added', () => this.animation.addKeyAt(this.time))) {
      this.message = `Key ${this.animation.key + 1} added at ${this.time.toFixed(2)} s.`;
    } else if (this.keyHere() >= 0) {
      this.message = 'There is a key here already.';
    } else {
      this.message = 'No more keys.';
    }
  }

  // The key at the playhead goes (− Key); the first key stays.
  removeKey() {
    this.stopPlaying();
    const here = this.keyHere();
    if (here < 0) {
      this.message = 'No key at the playhead: put the playhead on a key.';
      return;
    }
    this.animation.key = here;
    if (!this.changeAnimation('Key removed', () => this.animation.removeKey())) {
      this.message = 'The animation keeps one key.';
    }
  }

  // The key at the playhead a tenth of a second earlier or later (Details' Time).
  moveKey(direction) {
    this.stopPlaying();
    const here = this.keyHere();
    if (here <= 0) {
      this.message = 'The first key stays at 0 s; put the playhead on another key.';
      return;
    }
    const to = this.animation.keys[here].time + Math.sign(direction) * 0.1;
    if (this.changeAnimation('Key moved', () => this.animation.moveKeyTo(here, to))) {
      this.time = this.animation.keys[here].time;
    } else {
      this.message = 'A key stays between its neighbours.';
    }
  }

  // The whole animation from JSON (the assistant's studio_set_animation): { keys: [{ time,
  // pose: { <joint name or id>: [x, y, z] degrees } }], blend?, loop?, play? }. One step for
  // undo. Answers what was made, or why not.
  setAnimation(text) {
    let data = null;
    try {
      data = JSON.parse(text);
    } catch {
      return { error: 'the animation is not JSON' };
    }
    if (!data || !Array.isArray(data.keys) || data.keys.length === 0) {
      return { error: 'keys: a list of { time, pose } is needed' };
    }
    if (this.joints.length === 0) {
      return { error: 'the joints are not known yet: show Bones (studio_bones) first' };
    }
    const byName = new Map(this.joints.map((joint) => [joint.name.toLowerCase(), joint.id]));
    const unknown = new Set();
    const keys = data.keys.map((one) => {
      const pose = {};
      for (const [name, turn] of Object.entries(one?.pose ?? {})) {
        const id = /^\d+$/.test(name) ? Number(name) : byName.get(name.toLowerCase());
        if (id === undefined || !this.joints[id]) {
          unknown.add(name);
        } else {
          pose[id] = turn;
        }
      }
      // The key's prop settings and the hips' lift (a baked clip's):
      // kept as given (Animation.restore checks them).
      const key = { time: one?.time, pose };
      if (one?.lift !== undefined) {
        key.lift = one.lift;
      }
      if (one?.lash !== undefined) {
        key.lash = one.lash;
      }
      if (one?.prop) {
        key.prop = one.prop;
      }
      return key;
    });
    const next = new Animation();
    const { blend, loop, events, lashTrack } = data;
    if (!next.restore({ keys, blend, loop, events, lashTrack })) {
      return { error: 'times must be 0 to 60 s, each key later than the one before' };
    }
    // Events left out: not an effect (Library.group.name) or sound name, or a bad time.
    const given = Array.isArray(data.events) ? data.events.length : 0;
    this.stopPlaying();
    this.changeAnimation('Animation set', () => {
      this.animation.restore(next.toJSON());
      return true;
    });
    this.setTime(0);
    if (data.play !== false && this.animation.keys.length > 1) {
      this.setPlaying(true);
    }
    return {
      keys: this.animation.keys.length,
      length: this.animation.length(),
      blend: this.animation.blend,
      loop: this.animation.loop,
      playing: this.animation.playing,
      unknownJoints: [...unknown],
      events: this.animation.events.length,
      eventsLeftOut: given - this.animation.events.length,
    };
  }

  // The animation with its joints by name.
  animationByName() {
    const data = this.animation.toJSON();
    for (const key of data.keys) {
      key.pose = Object.fromEntries(
        Object.entries(key.pose).map(([id, turn]) => [this.joints[id]?.name ?? id, turn]),
      );
    }
    return data;
  }

  // ---- The timeline (timeline.cjs): presses, a dragged key, the playhead while playing ----

  timelineView() {
    const { keys } = this.animation;
    const turned = new Set();
    keys.forEach((one) => Object.keys(one.pose).forEach((joint) => turned.add(Number(joint))));
    // The chosen joint's track comes first, so it is always shown.
    const order = [...turned].sort((a, b) => a - b);
    if (turned.has(this.joint)) {
      order.splice(order.indexOf(this.joint), 1);
      order.unshift(this.joint);
    }
    const tracks = order.map((joint) => ({
      label: this.joints[joint]?.name ?? `Joint ${joint}`,
      joint,
      chosen: joint === this.joint,
      keys: keys.flatMap((one, index) => (one.pose[joint] ? [index] : [])),
    }));
    const length = this.animation.length();
    return {
      time: this.time,
      span: timeline.spanOf(Math.max(length, this.drag?.time ?? 0, this.time)),
      length,
      // The lash's own keys (posed by hand) on their row, the one at the playhead chosen.
      lashKeys: (this.animation.lashKeys?.keys ?? []).map((key) => key.time),
      lashKey: this.lashKeyHere(),
      playing: this.animation.playing,
      loop: this.animation.loop,
      blend: this.animation.blend,
      key: this.keyHere(),
      keys: keys.map((one) => one.time),
      tracks,
      drag: this.drag,
    };
  }

  pressTimeline(x, y) {
    const view = this.timelineView();
    const press = timeline.hit(view, x, y);
    if (!press) {
      return;
    }
    const actions = {
      start: () => this.setTime(0),
      end: () => this.setTime(this.animation.length()),
      play: () => this.setPlaying(!this.animation.playing),
      addKey: () => this.addKey(),
      removeKey: () => this.removeKey(),
      loop: () => {
        this.animation.loop = !this.animation.loop;
      },
      blend: () => {
        const blend = this.animation.nextBlend(press.direction);
        this.sendPose();
        this.save();
        this.message = `Blend: ${blend}.`;
      },
      key: () => {
        this.chooseKey(press.key);
        if (press.joint !== null && this.joints[press.joint]) {
          this.chooseJoint(press.joint);
        }
        // A key but the first can be dragged to another time, while the button is held.
        if (press.key > 0) {
          const time = this.animation.keys[press.key].time;
          this.drag = { key: press.key, time, startedAt: this.now() };
        }
      },
      scrub: () => {
        this.setTime(press.time);
        if (press.joint !== null && this.joints[press.joint]) {
          this.chooseJoint(press.joint);
        }
      },
      joint: () => this.chooseJoint(press.joint),
      // A lash key: the playhead to it (its rows shape it there).
      lashKey: () => {
        this.setTime(press.time);
        this.message = `Lash key ${press.index + 1}: shape it in the whip's LASH KEYS.`;
      },
    };
    actions[press.kind]?.();
    this.refresh();
  }

  // The game's release on the timeline: ends a key's drag, unless the button is still held (a
  // release the game sent while the key was being dragged).
  releaseTimeline(x) {
    if (!this.drag || this.api.Key.isDown('mouse1')) {
      return;
    }
    this.endKeyDrag(x);
  }

  // A dragged key let go at `x` across the dock (null: where it was last shown).
  endKeyDrag(x) {
    const drag = this.drag;
    if (!drag) {
      return;
    }
    this.drag = null;
    const to = x === null ? drag.time : timeline.timeAt(this.timelineView(), x);
    if (this.changeAnimation('Key moved', () => this.animation.moveKeyTo(drag.key, to))) {
      this.time = this.animation.keys[drag.key].time;
      this.message = `Key ${drag.key + 1} moved to ${this.time.toFixed(2)} s.`;
    }
    this.refresh();
  }

  // A dragged key follows the pointer on the screen until it is let go.
  followDrag() {
    const pointer = this.shell?.dockPointer();
    if (!pointer || this.now() - this.shownAt < TIMELINE_REFRESH_MS / 2) {
      return;
    }
    const time = timeline.timeAt(this.timelineView(), pointer.x);
    if (time !== this.drag.time) {
      this.drag.time = time;
      this.shownAt = this.now();
      this.refresh();
    }
  }

  setPlaying(on) {
    if (on && this.animation.keys.length < 2) {
      this.message = 'Add a second key to play the animation.';
      return;
    }
    if (on) {
      // Playing starts at the playhead (from the start when it is at the end).
      const from = this.time >= this.animation.length() ? 0 : this.time;
      this.animation.playing = true;
      this.animation.startedAt = this.now() - from * 1000;
      this.message = `Playing ${this.animation.length().toFixed(2)} s.`;
    } else {
      this.stopPlaying();
      this.message = 'Stopped.';
    }
  }

  // ---- Your meshes (studio/studio_mesh.hpp): models converted from Blender ----

  // The joints' geometry the game client answered (studio/studio_skeleton.hpp studioJointInfo).
  jointInfoKept(info) {
    this.jointInfo = info;
  }

  // The meshes the game client found (KCDS\studio\meshes). A made shape whose files are gone is
  // forgotten (not one being written now).
  meshesKept(names) {
    this.meshes = names.map(String);
    const gone = Object.keys(this.shapes).filter(
      (name) => !this.meshes.includes(name) && name !== this.writingShape,
    );
    if (gone.length > 0) {
      for (const name of gone) {
        delete this.shapes[name];
      }
      this.save();
    }
    this.refresh();
  }

  // Shows a mesh beside the character ('' takes it away), one undo step. Another mesh stands
  // where the one before stood.
  showMesh(name) {
    if (!name) {
      if (this.mesh) {
        this.commitMesh(null, `${this.mesh} removed`);
      }
      this.message = 'The mesh is gone.';
      return;
    }
    if (name === this.mesh) {
      // The one shown: its file again (made anew, studio_make_mesh or an export from Blender).
      this.reloadMesh();
      return;
    }
    const place = this.mesh ? this.meshPlace : placing.meshPlaceOf(null, MESH_PLACE);
    this.commitMesh({ name, ...place }, `${shortName(name)} shown`);
    this.message = `${shortName(name)} beside the character.`;
  }

  // Loads the mesh's file again where it stands (after a new export from Blender).
  reloadMesh() {
    if (this.mesh) {
      this.applyMesh(this.meshState(), true);
      this.message = `${this.mesh} loaded again.`;
    }
  }

  // One step of the mesh's place, turn or size (Details), one undo step.
  stepMesh(part, axis, direction) {
    if (!this.mesh) {
      return;
    }
    const place = { ...this.meshPlace };
    if (part === 'at') {
      place.at = placing.stepPlace(place.at, axis, direction);
    } else if (part === 'turn') {
      place.turn = placing.stepTurn(place.turn, axis, direction);
    } else {
      place.scale =
        Math.round(Math.min(20, Math.max(0.05, place.scale * (direction > 0 ? 1.25 : 0.8))) * 100) /
        100;
    }
    // The mesh keeps its texture (meshState) with the new place.
    this.commitMesh({ ...this.meshState(), ...place }, `${this.mesh} placed`);
  }

  // The grid and the world's axes on the floor (studio/studio_grid.hpp).
  setGrid(on) {
    this.grid = on;
    this.api.send({ op: 'studioGrid', on });
    this.message = on ? 'Grid: one-metre squares, X red, Y green.' : 'No grid.';
  }

  // The floor drawn or not (the character still stands on it).
  setFloor(on) {
    this.floor = on;
    this.api.send({ op: 'studioFloor', on });
    this.message = on
      ? 'The floor is drawn.'
      : 'No floor drawn: the character stands on nothing seen.';
  }

  // ---- Where the prop is held (studio/studio_hold.hpp) ----

  // The next hold: right hand, left hand, free in the scene (where it is now).
  changeHold(direction) {
    if (!this.rope) {
      return;
    }
    const hold = placing.nextHold(this.rope.hold, direction);
    const label = `Held in: ${placing.holdLabel(hold)}`;
    this.commit({ ...this.rope, hold }, label);
    this.message = hold.joint
      ? `${label}.`
      : 'Free in the scene: drag the gizmo’s arrows, or set the place and turn in Details.';
  }

  // Where your mesh is held: a hand of the character or free in the scene, one undo
  // step. In a hand it starts in the palm, free it stands beside the character.
  changeMeshHold(direction) {
    if (!this.mesh) {
      return;
    }
    const joint = placing.nextHold({ joint: this.meshPlace.joint }, direction).joint;
    const place = placing.meshPlaceOf({ scale: this.meshPlace.scale, joint }, MESH_PLACE);
    const label = `${shortName(this.mesh)} held in: ${meshHoldLabel(place)}`;
    this.commitMesh({ ...this.meshState(), ...place }, label);
    this.message = joint
      ? `${label}: Place and Turn are now in the hand.`
      : `${label}: it stands beside the character.`;
  }

  // The rope's end pinned to its point or free, one undo step.
  setEndPinned(pinned) {
    if (!this.rope?.end) {
      return;
    }
    this.commit(
      { ...this.rope, end: { ...this.rope.end, pinned } },
      pinned ? 'Rope end pinned' : 'Rope end free',
    );
    this.message = pinned
      ? 'The rope’s end is pinned: press it in the view and drag its arrows, or set End X, Y, Z in Details.'
      : 'The rope’s end hangs free.';
  }

  // One step of the pinned end's place, one undo step.
  stepEnd(axis, direction) {
    if (!this.rope?.end?.pinned) {
      return;
    }
    const at = placing.stepPlace(this.rope.end.at, axis, direction);
    this.commit({ ...this.rope, end: { ...this.rope.end, at } }, 'Rope end moved');
  }

  // One step of a free prop's place or turn (Details), one undo step.
  stepHold(part, axis, direction) {
    if (!this.rope || this.rope.hold.joint) {
      return;
    }
    const hold = { ...this.rope.hold };
    if (part === 'at') {
      hold.at = placing.stepPlace(hold.at, axis, direction);
    } else {
      hold.turn = placing.stepTurn(hold.turn, axis, direction);
    }
    this.commit({ ...this.rope, hold }, `${PROPS[this.rope.kind].label} placed`);
  }

  // ---- The sky (studio/studio_sky.hpp) ----

  // One step of the time of day (an hour, round the clock), the weather or the rain (a tenth).
  changeSky(field, direction) {
    const step = Math.sign(direction || 1);
    const next = { ...this.sky };
    if (field === 'hour') {
      next.hour = (this.sky.hour + step + 24) % 24;
    } else if (field === 'preset') {
      const index = WEATHERS.findIndex((one) => one.preset === this.sky.preset);
      next.preset = WEATHERS[(index + step + WEATHERS.length) % WEATHERS.length].preset;
    } else {
      next.rain = Math.min(1, Math.max(0, Math.round((this.sky.rain + step * 0.1) * 10) / 10));
    }
    this.setSky(next);
  }

  setSky(given) {
    this.sky = skyOf(given);
    this.api.send({ op: 'studioSky', ...this.sky });
    const weather = WEATHERS.find((one) => one.preset === this.sky.preset);
    this.message = `Sky: ${this.sky.hour}:00, ${weather.label}`;
    this.note(this.message);
    this.save();
  }

  // ---- The character's clothes (outfits.cjs) ----

  setClothing(clothing, message) {
    this.clothing = clothingOf(clothing);
    this.api.send(clothingMessage(this.clothing));
    this.note(message);
    this.message = message;
    this.save();
  }

  // The next or the previous outfit, every piece on.
  changeOutfit(direction) {
    const index = OUTFITS.findIndex((outfit) => outfit.key === this.clothing.outfit);
    const next = OUTFITS[(index + OUTFITS.length + Math.sign(direction || 1)) % OUTFITS.length];
    this.setClothing({ outfit: next.key, off: [] }, `Wears: ${next.label}`);
  }

  // A piece of the outfit off or on again.
  togglePiece(name) {
    const off = this.clothing.off.includes(name)
      ? this.clothing.off.filter((one) => one !== name)
      : [...this.clothing.off, name];
    const on = !off.includes(name);
    this.setClothing({ ...this.clothing, off }, `${name} ${on ? 'on' : 'off'}`);
  }

  // The body's own animation on (its idle plays under the turns) or held still (the default).
  setIdle(on) {
    this.idle = on;
    this.api.send({ op: 'studioIdle', on });
    this.message = on ? 'The body’s idle plays.' : 'The body stands still.';
  }

  stopPlaying() {
    if (this.animation.playing) {
      this.animation.playing = false;
      this.sendPose();
    }
  }

  // ---- Prop keys: the prop's hand, grip and physics set at animation keys ----

  // The prop's own settings (Physics mode, its grip, the hand it is held in) as a prop key holds
  // them.
  propSettings() {
    const settings = {};
    for (const field of physicsFieldsOf(this.rope.kind)) {
      if (PROP_SETTINGS[field.key]) {
        settings[field.key] = this.rope.physics[field.key];
      }
    }
    settings.hand = this.rope.hold?.joint === 'LeftHand' ? 'LeftHand' : 'RightHand';
    if (this.rope.kind === 'whip' && this.rope.grip) {
      settings.pitch = this.rope.grip.pitch;
      settings.twist = this.rope.grip.twist;
    }
    return settings;
  }

  // The prop key of the key at the playhead, or null.
  propKeyHere() {
    const here = this.keyHere();
    return here >= 0 ? this.animation.keys[here].prop ?? null : null;
  }

  // Your mesh where the mesh keys put it at `time`: told to the game client once
  // per change; without mesh keys it stands where it was placed.
  applyMeshKeys(time) {
    const keyed = this.mesh ? this.animation.meshAt(time) : null;
    if (!keyed) {
      if (this.meshKeysOn && this.mesh) {
        this.meshKeysOn = false;
        this.api.send({ op: 'studioMeshPlace', ...this.meshPlace });
      }
      return;
    }
    this.meshKeysOn = true;
    const message = {
      op: 'studioMeshPlace',
      at: keyed.at,
      turn: keyed.turn,
      scale: this.meshPlace.scale,
      joint: this.meshPlace.joint,
    };
    const text = JSON.stringify(message);
    if (text !== this.meshKeySent) {
      this.meshKeySent = text;
      this.api.send(message);
    }
  }

  // The key at the playhead keeps your mesh's place and turn as they are now (on), or no longer
  // places it (off); one undo step. Between mesh keys the mesh moves as the keys blend.
  setMeshKey(on) {
    if (!this.mesh) {
      this.message = 'Show a mesh first (Props › Static props): a mesh key places it at a key.';
      return;
    }
    const here = this.keyHere();
    if (here < 0) {
      this.message = 'Put the playhead on a key first (Key − and +), then set the mesh key there.';
      return;
    }
    this.stopPlaying();
    this.animation.key = here;
    const mesh = on ? { at: [...this.meshPlace.at], turn: [...this.meshPlace.turn] } : null;
    const label = on ? `Mesh key at key ${here + 1}` : `Mesh key of key ${here + 1} removed`;
    this.changeAnimation(label, () => {
      this.animation.setMeshKey(mesh);
      return true;
    });
    this.meshKeySent = '';
    this.applyMeshKeys(this.time);
    this.message = on
      ? `Key ${here + 1} places ${shortName(
          this.mesh,
        )} where it stands: move it, then set the next key.`
      : `Key ${here + 1} no longer places ${shortName(this.mesh)}.`;
  }

  // The key at the playhead gets the prop as it is there (on), or loses its prop key (off); one
  // undo step.
  setPropKey(on) {
    if (!this.rope) {
      this.message =
        'Make a prop first (Props › Physical props): a prop key sets its grip and physics.';
      return;
    }
    const here = this.keyHere();
    if (here < 0) {
      this.message = 'Put the playhead on a key first (Key − and +), then set the prop key there.';
      return;
    }
    this.stopPlaying();
    this.animation.key = here;
    const prop = on ? { ...this.propSettings(), ...this.animation.propAt(this.time) } : null;
    const label = on ? `Prop key at key ${here + 1}` : `Prop key of key ${here + 1} removed`;
    this.changeAnimation(label, () => {
      this.animation.setProp(prop);
      return true;
    });
    this.applyPropKeys(this.time);
    this.message = on
      ? `Key ${here + 1} sets the prop: change its grip and physics below; between keys they blend.`
      : `Key ${here + 1} no longer sets the prop.`;
  }

  // One setting of the prop key at the playhead a step up or down (the hand and collide switch);
  // one undo step.
  stepPropKey(name, direction) {
    const here = this.keyHere();
    const prop = here >= 0 ? this.animation.keys[here].prop : null;
    if (!prop) {
      return;
    }
    this.stopPlaying();
    this.animation.key = here;
    const next = { ...prop };
    if (name === 'hand') {
      next.hand = prop.hand === 'LeftHand' ? 'RightHand' : 'LeftHand';
    } else if (name === 'collide') {
      next.collide = ((prop.collide ?? 1) + (direction > 0 ? 1 : 2)) % 3;
    } else {
      const field = [...PHYSICS_FIELDS, ...GRIP_FIELDS].find((one) => one.key === name);
      if (!field) {
        return;
      }
      const value = (prop[name] ?? field.first) + Math.sign(direction) * field.step;
      next[name] = Math.round(clamp(value, field.range) * 1000) / 1000;
    }
    this.changeAnimation(`Prop key ${here + 1}: ${name}`, () => {
      this.animation.setProp(next);
      return true;
    });
    this.applyPropKeys(this.time);
  }

  // The prop as the prop keys set it at `time` (while the animation plays, and at the
  // playhead): its physics, grip and hand to the game client, each only when it changed.
  // Without prop keys the prop's own settings go back once.
  applyPropKeys(time) {
    if (!this.rope) {
      return;
    }
    const keyed = this.animation.propAt(time);
    // While Lash to keys records, the lash moves by physics (the posed keys it had stay out).
    if (this.lashBaking) {
      delete keyed.shape;
      delete keyed.curl;
      delete keyed.sway;
    }
    const none = Object.keys(keyed).length === 0;
    if (none && !this.propKeysOn) {
      return;
    }
    this.propKeysOn = !none;
    const now = { ...this.propSettings(), ...keyed };
    const physics = { ...this.rope.physics };
    for (const field of PHYSICS_FIELDS) {
      if (now[field.key] !== undefined) {
        physics[field.key] = Math.round(now[field.key] * 1000) / 1000;
      }
    }
    this.sendPropOnce({ op: 'studioPropPhysics', ...physics });
    if (this.rope.kind === 'whip' && this.rope.grip) {
      // Mirrored in the left hand, as apply() sends it.
      const mirrored = now.hand === 'LeftHand' ? -1 : 1;
      this.sendPropOnce({
        op: 'studioPropGrip',
        pitch: Math.round(mirrored * now.pitch * 10) / 10 || 0,
        twist: Math.round(mirrored * now.twist * 10) / 10 || 0,
      });
    }
    // The hand, while it is held in one (free in the scene it stays free).
    if (this.rope.hold?.joint) {
      const hold = placing.holdOf({ ...this.rope.hold, joint: now.hand });
      this.sendPropOnce({ op: 'studioPropHold', ...hold });
    }
  }

  // The Details rows of the prop key at key `here` (-1: none at the playhead): PROP AT THIS KEY,
  // whether the key sets the prop and, when it does, its hand, grip and physics.
  // Details of the key at the playhead for your mesh: whether it places the mesh, and
  // where.
  meshKeyRows(here) {
    if (!this.mesh) {
      return [];
    }
    const mesh = here >= 0 ? this.animation.keys[here].mesh : null;
    const rows = [
      { label: 'MESH AT THIS KEY' },
      { label: 'Mesh key', toggle: Boolean(mesh), press: () => this.setMeshKey(!mesh) },
    ];
    if (mesh) {
      rows.push({ label: 'Place', value: mesh.at.map((part) => part.toFixed(2)).join(' ') });
      rows.push({ label: 'Turn', value: mesh.turn.map((part) => `${part}°`).join(' ') });
      rows.push({
        label: 'Set again',
        value: 'Where the mesh stands now',
        press: () => this.setMeshKey(true),
      });
    } else {
      rows.push({
        label: 'Places',
        value:
          here >= 0 ? 'Nothing: the mesh stands where it was placed' : 'Put the playhead on a key',
      });
    }
    return rows;
  }

  propKeyRows(here) {
    if (!this.rope) {
      return [];
    }
    const prop = here >= 0 ? this.animation.keys[here].prop : null;
    const rows = [
      { label: 'PROP AT THIS KEY' },
      {
        label: 'Prop key',
        toggle: Boolean(prop),
        press: () => this.setPropKey(!prop),
      },
    ];
    if (!prop) {
      rows.push({
        label: 'Sets',
        value: here >= 0 ? 'Nothing: the prop’s own settings' : 'Put the playhead on a key',
      });
      return rows;
    }
    rows.push({
      label: 'Hand',
      value: prop.hand === 'LeftHand' ? 'Left hand' : 'Right hand',
      step: (dir) => this.stepPropKey('hand', dir),
    });
    const fields = [
      ...(this.rope.kind === 'whip' ? GRIP_FIELDS : []),
      ...physicsFieldsOf(this.rope.kind).filter((field) => PROP_SETTINGS[field.key]),
    ];
    for (const field of fields) {
      const value = prop[field.key] ?? field.first;
      let shown = `${value}${field.unit ? ` ${field.unit}` : ''}`;
      if (field.key === 'bend' && value === 0) {
        shown = 'Free';
      } else if (field.key === 'collide') {
        shown = ['Off', 'On', 'Floor only'][value] ?? 'On';
      } else if (field.key === 'stiffness' && value === 0) {
        shown = 'The game’s own';
      }
      rows.push({
        label: field.label,
        value: shown,
        step: (dir) => this.stepPropKey(field.key, dir),
      });
    }
    return rows;
  }

  // A prop message to the game client unless the last one of its kind said the same.
  sendPropOnce(message) {
    this.propSent ??= {};
    const text = JSON.stringify(message);
    if (this.propSent[message.op] === text) {
      return;
    }
    this.propSent[message.op] = text;
    this.api.send(message);
  }

  playFrame() {
    const before = this.time;
    this.time = Math.round(this.animation.playTime(this.now()) * 1000) / 1000;
    this.api.send(this.animation.messageAt(this.time));
    this.sendLash(this.time);
    this.applyPropKeys(this.time);
    this.applyMeshKeys(this.time);
    // The effects and sounds whose time the playhead passed (studio/studio_event.hpp).
    if (this.time !== before) {
      for (const event of this.animation.eventsBetween(before, this.time)) {
        this.playEvent(event);
      }
    }
    // The playhead on the screen a few times a second; at once when playing stopped.
    if (!this.animation.playing || this.now() - this.shownAt >= TIMELINE_REFRESH_MS) {
      this.shownAt = this.now();
      this.refresh();
    }
  }

  now() {
    return this.api.now?.() ?? Date.now();
  }

  turnOutlinerPage(step) {
    const pages = Math.max(1, Math.ceil(this.outlinerAll().length / outlinerRows()));
    this.outlinerPage = Math.min(pages - 1, Math.max(0, this.outlinerPage + step));
    this.refresh();
  }

  turnCameraSpeed() {
    const speeds = [2, 6, 12, 24];
    const next = speeds[(speeds.indexOf(this.cameraSpeed) + 1) % speeds.length];
    this.cameraSpeed = next ?? 6;
    this.api.NoClip.setSpeed(this.cameraSpeed);
    this.refresh();
  }

  // The snaps' places hold the studio's switches: the hand, the floor, the swing test.
  pressSnap(index) {
    if (index === 0 && this.rope) {
      this.changeHold(1);
    } else if (index === 1) {
      this.setFloor(!this.floor);
    } else if (index === 3) {
      this.setGrid(!this.grid);
    } else if (index === 2 && this.rope) {
      this.setSweep(!this.rope.sweep);
    }
    this.refresh();
  }

  pressFolder(index) {
    const folders = MODES.find((mode) => mode.key === this.mode).folders;
    if (folders[index]) {
      this.folder = folders[index];
      this.page = 0;
      if (this.folder === 'static') {
        // Your meshes as they are now in KCDS\studio\meshes (a new export shows at once).
        this.api.send({ op: 'studioMeshList' });
      } else if (this.folder === 'textures') {
        this.api.send({ op: 'studioTextureList' });
      } else if (GAME_FOLDERS[this.folder]) {
        const kind = GAME_FOLDERS[this.folder];
        const kept = this.gameFiles[kind];
        this.askGameFiles(kind, kept.query, kept.page);
      }
      this.refresh();
    }
  }

  // ---- The game's own objects and clips (studio/game_files.hpp) ----

  // Asks the game client for a page of the game's objects or clips that match the search.
  askGameFiles(kind, query, page) {
    const kept = this.gameFiles[kind];
    kept.query = query;
    kept.page = page;
    kept.asked = true;
    this.api.send({ op: 'studioGameFiles', kind, query, page, size: TILE_COUNT });
  }

  // The game client's page; one for a search typed since is left for the newer answer. A page
  // of another size is the assistant's (the gamefiles step), not the tiles'.
  gameFilesKept(answer) {
    const kind = answer.kind === 'clips' ? 'clips' : 'objects';
    if (answer.size && answer.size !== TILE_COUNT) {
      this.gameFilesAnswer = answer;
      return;
    }
    const kept = this.gameFiles[kind];
    if (answer.query !== kept.query) {
      return;
    }
    if (!answer.ready) {
      kept.ready = false;
      this.refresh();
      return;
    }
    kept.ready = true;
    kept.page = answer.page;
    kept.pages = Math.max(1, answer.pages);
    kept.total = answer.total;
    kept.paths = answer.paths.filter((path) => typeof path === 'string');
    this.refresh();
  }

  // The content's search: in a game folder it takes the keys (letters, digits, space).
  pressContentSearch() {
    if (!GAME_FOLDERS[this.folder]) {
      this.message = 'Search: in Game props and Game clips.';
      this.refresh();
      return;
    }
    this.startTyping('search');
    this.message = 'Type to search the game’s files; Enter or Esc ends.';
    this.refresh();
  }

  // One of the game's clips on the character (studio/studio_game_clip.hpp), all of it and in
  // place at first; '' stops it.
  playGameClip(path, loop = true, part = {}) {
    this.gameClip = path;
    this.gameClipState = null;
    this.gameClipPart = gameClipPartOf({ loop, ...part });
    this.sendGameClip();
    this.message = path ? `The game’s clip ${path.split('/').at(-1)} plays.` : 'Game clip stopped.';
  }

  // The part of the clip playing that plays, in place or not, looped or not: it plays on.
  setGameClipPart(change) {
    if (!this.gameClip) {
      return;
    }
    this.gameClipPart = gameClipPartOf({ ...this.gameClipPart, ...change });
    this.sendGameClip();
    const { from, to } = this.gameClipPart;
    this.message = from > 0 || to < 1 ? 'Only the part From–To plays.' : 'All of the clip plays.';
  }

  // The game clip playing as an export plays it on players: its path and its part in seconds
  // (its length as the game client measured it); null without one, or before its length is
  // known.
  exportedGameClip() {
    const seconds = this.gameClipState?.seconds;
    if (!this.gameClip || !(seconds > 0)) {
      return null;
    }
    const at = (share) => Math.round(share * seconds * 100) / 100;
    return {
      clip: this.gameClip,
      from: at(this.gameClipPart.from),
      to: at(this.gameClipPart.to),
      topBody: this.gameClipPart.topBody,
    };
  }

  sendGameClip() {
    const { from, to, loop, inPlace, topBody } = this.gameClipPart;
    this.api.send({ op: 'studioGameClip', path: this.gameClip, loop, from, to, inPlace, topBody });
  }

  // Where the game client says the clip is (five times a second while it plays).
  gameClipStateKept(state) {
    if (state.path !== this.gameClip) {
      return;
    }
    this.gameClipState = state;
    if (this.mode === 'animation') {
      this.refresh();
    }
  }

  pressTab(index) {
    this.tab = index;
    if (index === SAVES_TAB) {
      this.api.send({ op: 'studioSaveList' });
      this.message = 'Saves: name the scene and press Save as, or load one.';
    }
    if (index === AI_TAB) {
      // The game client's AI bridge: its port, state, MCP server path and token.
      this.ai = this.api.aiBridge?.() ?? null;
    }
    this.typing = false;
    this.refresh();
  }

  // Copies an AI tab row to the Windows clipboard (architect/ai_bridge.hpp architectCopy).
  copyAi(row) {
    const entry = aiConnectView(this.ai).rows[row];
    if (!entry) {
      return;
    }
    this.api.send({ op: 'architectCopy', text: entry.text });
    this.message = `Copied: ${entry.label}. Paste it into your AI app.`;
    this.refresh();
  }

  // ---- Saves on this PC (studio/studio_saves.hpp), as World Architect's Saves tab ----

  savesView() {
    const named = this.saveName !== '';
    const two = (value) => String(value).padStart(2, '0');
    return {
      typing: this.typing,
      named,
      name: named || this.typing ? `${this.saveName}${this.typing ? '|' : ''}` : 'Name of the save',
      folder: `Kept in ${SAVES_FOLDER}`,
      help: ['The scene under a name:', 'the prop, the animation,', 'the mesh, clothes and sky.'],
      rows: this.saves.map((save) => {
        const when = new Date(Number(save.time) * 1000);
        const date = `${when.getFullYear()}-${two(when.getMonth() + 1)}-${two(when.getDate())}`;
        const time = `${two(when.getHours())}:${two(when.getMinutes())}`;
        const size = `${Math.max(1, Math.round(Number(save.bytes) / 1024))} kB`;
        return { name: save.name, when: `${date} ${time}  ·  ${size}` };
      }),
    };
  }

  // The saves the game client found, newest first.
  savesKept(saves) {
    this.saves = saves.filter((save) => typeof save?.name === 'string');
    this.refresh();
  }

  startTyping(into = 'save') {
    this.typing = true;
    this.typingInto = into;
    this.keysDown.clear();
    this.refresh();
  }

  // The name field has the keys: letters, digits, space, - (shift: _), backspace; Enter ends.
  pollTyping() {
    const { Key } = this.api;
    for (const key of TYPING_KEYS) {
      const down = Key.isDown(key);
      if (down && !this.keysDown.has(key)) {
        this.keysDown.add(key);
        this.type(key, Key.isDown('shift'));
      } else if (!down) {
        this.keysDown.delete(key);
      }
    }
  }

  type(key, shift) {
    if (this.typingInto === 'search') {
      this.typeSearch(key);
      return;
    }
    if (key === 'enter') {
      this.typing = false;
    } else if (key === 'backspace') {
      this.saveName = this.saveName.slice(0, -1);
    } else if (this.saveName.length < SAVE_NAME_LENGTH) {
      if (key === 'space') {
        this.saveName += ' ';
      } else if (key === 'minus') {
        this.saveName += shift ? '_' : '-';
      } else {
        this.saveName += key;
      }
    }
    this.refresh();
  }

  // A key typed into the game folder's search: each one searches again from the first page.
  typeSearch(key) {
    const kind = GAME_FOLDERS[this.folder];
    if (!kind || key === 'enter') {
      this.typing = false;
      this.refresh();
      return;
    }
    let query = this.gameFiles[kind].query;
    if (key === 'backspace') {
      query = query.slice(0, -1);
    } else if (query.length < SEARCH_LENGTH) {
      query += { space: ' ', minus: '_' }[key] ?? key;
    }
    this.askGameFiles(kind, query, 0);
    this.refresh();
  }

  // The scene as it is now, kept under the typed name.
  saveAs() {
    const name = this.saveName.trim();
    this.typing = false;
    if (!SAVE_NAME.test(name)) {
      this.message = 'A save is named with letters, digits, spaces, - or _ (up to 48).';
      this.refresh();
      return;
    }
    this.api.send({ op: 'studioSaveAs', name, data: JSON.stringify(this.project()) });
    this.note(`Saved as ${name}`);
    this.message = `Saved "${name}" to ${SAVES_FOLDER}.`;
    this.refresh();
  }

  // Load (the save instead of the scene) or Delete.
  pressSave(row, action) {
    const save = this.saves[row];
    if (!save) {
      return;
    }
    if (action === 'delete') {
      this.api.send({ op: 'studioSaveDelete', name: save.name });
      this.message = `Deleted "${save.name}".`;
    } else {
      this.api.send({ op: 'studioSaveLoad', name: save.name });
      this.message = `Loading "${save.name}"…`;
    }
    this.refresh();
  }

  // The game client's answer to Load: the save's scene instead of the one now. Undo starts
  // afresh from it.
  saveLoaded(name, data) {
    let project = null;
    try {
      project = data ? JSON.parse(data) : null;
    } catch {
      project = null;
    }
    if (!project || project.version !== PROJECT_VERSION) {
      this.message = `"${name}" could not be read.`;
      this.refresh();
      return;
    }
    // What the save does not have goes: its prop, mesh and animation replace the scene's.
    if (this.rope && !project.rope) {
      this.apply(null);
    }
    if (this.mesh && !project.mesh) {
      this.applyMesh(null);
    }
    this.stopPlaying();
    this.animation.restore(project.animation ?? { keys: [{ time: 0, pose: {} }] });
    this.sendPose();
    this.keptProject = project;
    this.rebuildProject();
    this.undoStack = [];
    this.redoStack = [];
    this.saveName = name;
    this.note(`Loaded ${name}`);
    this.save();
    this.message = `Loaded "${name}".`;
    this.refresh();
  }

  turnPage(step) {
    const kind = GAME_FOLDERS[this.folder];
    if (kind) {
      // The game client pages the game's files itself.
      const kept = this.gameFiles[kind];
      const page = Math.min(kept.pages - 1, Math.max(0, kept.page + step));
      if (page !== kept.page) {
        this.askGameFiles(kind, kept.query, page);
      }
      this.refresh();
      return;
    }
    const pages = Math.max(1, Math.ceil(this.items().length / TILE_COUNT));
    this.page = Math.min(pages - 1, Math.max(0, this.page + step));
    this.refresh();
  }

  pressTile(index) {
    // A game folder's items are its page already.
    const first = GAME_FOLDERS[this.folder] ? 0 : this.page * TILE_COUNT;
    const item = this.items()[first + index];
    if (!item) {
      return;
    }
    if (item.later) {
      this.message = `${item.label}: ${item.meta}.`;
      this.refresh();
      return;
    }
    if (item.gameClip) {
      // Pressed again, the clip stops.
      this.playGameClip(item.gameClip === this.gameClip ? '' : item.gameClip);
      this.refresh();
      return;
    }
    if (item.shape) {
      this.makeShape(item.shape);
      this.refresh();
      return;
    }
    if (item.mesh) {
      this.showMesh(item.mesh);
      this.selected = 'mesh';
      this.refresh();
      return;
    }
    if (item.effect || item.sound) {
      this.addEventHere(item.effect ? 'effect' : 'sound', item.effect ?? item.sound);
      this.refresh();
      return;
    }
    if (item.texture) {
      this.setTexture(item.texture);
      if (this.mesh) {
        this.selected = 'mesh';
      }
      this.refresh();
      return;
    }
    if (PROPS[item.name]) {
      this.makeRope({ ...PROPS[item.name].first, kind: item.name });
    }
  }

  pressOutliner(row) {
    const line = this.outlinerLines()[row];
    if (!line) {
      return;
    }
    if (typeof line.joint === 'number') {
      this.chooseJoint(line.joint);
    } else {
      this.selected = line.thing;
      this.ropePart = 'start';
      this.message = `${line.name} selected.`;
    }
    this.refresh();
  }

  pressDetail(row, side) {
    const shown = this.details().rows[row];
    if (!shown) {
      return;
    }
    if (shown.step) {
      // As in World Architect: its - or + steps once, and while the button stays down a move
      // sideways steps on (scrubOn: right up, left down), all of it one undo step.
      const undoAt = this.undoStack.length;
      if (side !== 0) {
        shown.step(side);
      }
      const x = this.detailsX();
      this.scrub =
        x === null ? null : { label: shown.label, startX: x, applied: side, base: side, undoAt };
    } else if (shown.press) {
      shown.press();
    }
    this.refresh();
  }

  // The cursor's x in the Details panel (the kit's tests stand in for it), or null.
  detailsX() {
    const pointer = this.testDetailsPointer ?? this.shell?.detailsPointer?.() ?? null;
    return pointer && Number.isFinite(pointer.x) ? pointer.x : null;
  }

  // While a value's button stays down: a step for every SCRUB_PX the cursor went sideways; let
  // go, the steps made are one undo step.
  scrubOn() {
    const scrub = this.scrub;
    if (!this.api.Key.isDown('mouse1')) {
      this.scrub = null;
      this.mergeSteps(scrub.undoAt);
      this.refresh();
      return;
    }
    const x = this.detailsX();
    if (x === null) {
      return;
    }
    const wanted = scrub.base + Math.trunc((x - scrub.startX) / SCRUB_PX);
    if (wanted === scrub.applied) {
      return;
    }
    const row = this.details().rows.find((entry) => entry.label === scrub.label && entry.step);
    if (!row) {
      this.scrub = null;
      return;
    }
    const direction = Math.sign(wanted - scrub.applied);
    while (scrub.applied !== wanted) {
      row.step(direction);
      scrub.applied += direction;
    }
    this.refresh();
  }

  // The undo steps made since `from` as one: the scene before the first of them stays, labelled
  // as the last one.
  mergeSteps(from) {
    if (this.undoStack.length - from <= 1) {
      return;
    }
    const first = this.undoStack[from];
    const last = this.undoStack[this.undoStack.length - 1];
    this.undoStack.splice(from, this.undoStack.length - from, { ...first, label: last.label });
  }

  // ---- The scene: its changes, undo and the kept project ----
  //
  // The scene is the rope's settings ({ segments, length, sweep }) or null. Every change is one
  // step: the step before goes on the undo stack, the game client makes the new scene, and the
  // project is written (architect/project_store.hpp, under the studio's level's name).

  commit(next, label) {
    this.pushStep(label);
    this.apply(next);
    this.note(label);
    this.save();
  }

  // Your mesh's change as one step: { name, at, turn, scale } or null (no mesh).
  commitMesh(next, label) {
    this.pushStep(label);
    this.applyMesh(next);
    this.note(label);
    this.save();
  }

  // The scene before a change goes on the undo stack.
  pushStep(label) {
    this.undoStack.push({
      scene: this.rope ? { ...this.rope } : null,
      mesh: this.meshState(),
      label,
    });
    this.undoStack.length = Math.min(this.undoStack.length, UNDO_STEPS);
    this.redoStack = [];
  }

  // Your mesh now: { name, at, turn, scale, texture, tiling, cloth, clothMass }, or null.
  meshState() {
    return this.mesh
      ? {
          name: this.mesh,
          ...this.meshPlace,
          texture: this.meshTexture.name,
          tiling: this.meshTexture.tiling,
          cloth: this.meshCloth.on,
          clothMass: this.meshCloth.mass,
          body: this.meshBody.kind,
          bodyMass: this.meshBody.mass,
        }
      : null;
  }

  // The texture on the mesh in a state: { name, tiling }.
  textureOf(state) {
    const name =
      typeof state?.texture === 'string' && /^[A-Za-z0-9_-]{0,48}$/.test(state.texture)
        ? state.texture
        : '';
    const tiling = Number(state?.tiling);
    return { name, tiling: Number.isFinite(tiling) ? clamp(tiling, TILING_RANGE) : 1 };
  }

  // Shows your mesh (studio/studio_mesh.hpp): another one is loaded, the same one only moved;
  // `reload` loads the file again.
  applyMesh(state, reload = false) {
    const name = state?.name ?? null;
    const place = placing.meshPlaceOf(state, MESH_PLACE);
    if (!name) {
      if (this.mesh) {
        this.api.send({ op: 'studioMesh', name: '', at: place.at });
      }
      this.mesh = null;
      this.meshPlace = place;
      this.meshTexture = { name: '', tiling: 1 };
      if (this.meshCloth.on) {
        this.api.send({ op: 'studioMeshCloth', on: false, mass: this.meshCloth.mass });
      }
      this.meshCloth = clothOf(null);
      if (this.meshBody.kind !== 'none') {
        this.api.send({ op: 'studioMeshBody', kind: 'none', mass: this.meshBody.mass });
      }
      this.meshBody = bodyOf(null);
      this.selected = this.selected === 'mesh' ? 'character' : this.selected;
      return;
    }
    const loaded = name !== this.mesh || reload;
    if (loaded) {
      // One of the game's objects goes by its path in the game's packs (studio/game_files.hpp).
      const which = isGamePath(name) ? { path: name } : { name };
      this.api.send({
        op: 'studioMesh',
        ...which,
        at: place.at,
        turn: place.turn,
        scale: place.scale,
        joint: place.joint,
      });
    } else {
      this.api.send({
        op: 'studioMeshPlace',
        at: place.at,
        turn: place.turn,
        scale: place.scale,
        joint: place.joint,
      });
    }
    // Its texture (studio/studio_texture.hpp): again on a mesh loaded anew, else when it changed.
    const texture = this.textureOf(state);
    const changed =
      texture.name !== this.meshTexture.name || texture.tiling !== this.meshTexture.tiling;
    if (changed || (loaded && texture.name)) {
      this.api.send({ op: 'studioTexture', name: texture.name, tiling: texture.tiling });
    }
    // As cloth (studio/studio_mesh.hpp): told when it changed; the game client keeps it for a
    // mesh loaded anew.
    const cloth = clothOf(state);
    if (cloth.on !== this.meshCloth.on || cloth.mass !== this.meshCloth.mass) {
      this.api.send({ op: 'studioMeshCloth', on: cloth.on, mass: cloth.mass });
    }
    // Its body (studio/studio_mesh.hpp): told when it changed; kept for a mesh loaded anew.
    const body = bodyOf(state);
    if (body.kind !== this.meshBody.kind || body.mass !== this.meshBody.mass) {
      this.api.send({ op: 'studioMeshBody', kind: body.kind, mass: body.mass });
    }
    this.mesh = name;
    this.meshPlace = place;
    this.meshTexture = texture;
    this.meshCloth = cloth;
    this.meshBody = body;
  }

  // Makes your mesh cloth or solid again, one undo step.
  setCloth(on, given = this.meshCloth.mass) {
    const mass = clamp(given, CLOTH_MASS.range);
    if (!this.mesh) {
      this.message = 'Show a mesh first (Props › Static props), then make it cloth here.';
      return;
    }
    const label = on ? `${this.mesh}: cloth, ${mass} kg` : `${this.mesh}: solid`;
    // Cloth has no body of its own besides.
    const body = on ? 'none' : this.meshBody.kind;
    this.commitMesh({ ...this.meshState(), cloth: on, clothMass: mass, body }, label);
    this.message = on
      ? `${this.mesh} is cloth: it hangs from its highest vertices. Turn it to choose the edge it hangs from.`
      : `${this.mesh} is solid again.`;
  }

  // The cloth's weight one step up or down.
  stepClothMass(direction) {
    const mass = clamp(this.meshCloth.mass + direction * CLOTH_MASS.step, CLOTH_MASS.range);
    this.setCloth(this.meshCloth.on, mass);
  }

  // Your mesh's body, one undo step: none, solid or falling (of `given` kg); a body
  // makes it solid if it was cloth.
  setBody(kind, given = this.meshBody.mass) {
    if (!this.mesh) {
      this.message = 'Show a mesh first (Props › Static props), then give it a body here.';
      return;
    }
    const body = bodyOf({ body: kind, bodyMass: given });
    const label = `${shortName(this.mesh)}: ${
      MESH_BODIES.find((one) => one.kind === body.kind).label
    }`;
    const cloth = body.kind === 'none' ? this.meshCloth.on : false;
    this.commitMesh({ ...this.meshState(), body: body.kind, bodyMass: body.mass, cloth }, label);
    if (body.kind === 'falls') {
      this.message = `${label}, ${body.mass} kg: it falls from where it is placed. Place it again to drop it again.`;
    } else {
      this.message = `${label}.`;
    }
  }

  // The next body of MESH_BODIES (direction 1 or -1).
  stepBody(direction) {
    const index = MESH_BODIES.findIndex((one) => one.kind === this.meshBody.kind);
    const next = MESH_BODIES[(index + direction + MESH_BODIES.length) % MESH_BODIES.length];
    this.setBody(next.kind);
  }

  // The falling body's weight 1.25 times up or down.
  stepBodyMass(direction) {
    this.setBody(this.meshBody.kind, this.meshBody.mass * (direction > 0 ? 1.25 : 0.8));
  }

  // ---- Simple props made here (shapes.cjs) ----

  // A new box, cylinder, sphere or plane under a free name (box_1, box_2, ...), written by the
  // game client into the meshes folder and then shown (meshWrittenKept).
  makeShape(kind) {
    const options = shapeOptionsOf({ shape: kind });
    if (!options) {
      return;
    }
    let number = 1;
    while (this.meshes.includes(`${kind}_${number}`) || this.shapes[`${kind}_${number}`]) {
      number += 1;
    }
    this.writeShape(`${kind}_${number}`, options);
  }

  // Writes a made shape anew with these options (the game client answers meshWrittenKept).
  writeShape(name, options) {
    let made;
    try {
      made = shapes.shapeMessage(name, options);
    } catch (error) {
      this.message = `${name}: ${error.message}.`;
      return;
    }
    this.shapes[name] = options;
    this.writingShape = name;
    this.api.send(made.message);
    this.message = `Making ${name}: ${made.vertices} vertices, ${made.triangles} triangles…`;
  }

  // The game client wrote a made shape's files: the list of meshes asked for anew and the shape
  // shown (the one shown loads its file again).
  meshWrittenKept(answer) {
    if (this.writingShape === answer.name) {
      this.writingShape = null;
    }
    if (!answer.ok) {
      this.message = `${answer.name}: ${answer.error}.`;
      this.refresh();
      return;
    }
    if (!this.meshes.includes(answer.name)) {
      this.meshes = [...this.meshes, answer.name];
    }
    this.api.send({ op: 'studioMeshList' });
    this.showMesh(answer.name);
    this.selected = 'mesh';
    this.save();
    this.refresh();
  }

  // One step of a made shape's option in Details: a length (0.05 m), the colour or the surface.
  stepShape(part, direction) {
    const options = this.shapes[this.mesh];
    if (!options) {
      return;
    }
    const next = { ...options };
    if (part === 'colour') {
      const index = SHAPE_COLOURS.findIndex((one) => one.colour.join() === options.colour.join());
      next.colour =
        SHAPE_COLOURS[(index + direction + SHAPE_COLOURS.length) % SHAPE_COLOURS.length].colour;
    } else if (part === 'surface') {
      const index = shapes.SURFACES.indexOf(options.surface);
      next.surface =
        shapes.SURFACES[(index + direction + shapes.SURFACES.length) % shapes.SURFACES.length];
    } else {
      const lengths = SHAPE_KINDS.find((one) => one.shape === options.shape).lengths;
      const at = lengths.indexOf(part);
      const step = (value) =>
        Math.round(Math.min(50, Math.max(SHAPE_STEP, value + direction * SHAPE_STEP)) * 100) / 100;
      if (options.size) {
        next.size = options.size.map((value, axis) => (axis === at ? step(value) : value));
      } else if (part === 'Radius') {
        next.radius = step(options.radius);
      } else {
        next.height = step(options.height);
      }
    }
    this.writeShape(this.mesh, next);
  }

  // Details' rows of a made shape: its lengths, colour and surface.
  shapeRows() {
    const options = this.shapes[this.mesh];
    if (!options) {
      return [];
    }
    const kind = SHAPE_KINDS.find((one) => one.shape === options.shape);
    const value = (part) => {
      if (options.size) {
        return options.size[kind.lengths.indexOf(part)];
      }
      return part === 'Radius' ? options.radius : options.height;
    };
    return [
      { label: 'SHAPE' },
      { label: 'Shape', value: options.shape },
      ...kind.lengths.map((part) => ({
        label: part,
        value: `${value(part).toFixed(2)} m`,
        step: (dir) => this.stepShape(part, dir),
      })),
      {
        label: 'Colour',
        value:
          SHAPE_COLOURS.find((one) => one.colour.join() === options.colour.join())?.label ??
          'Your own',
        step: (dir) => this.stepShape('colour', dir),
      },
      { label: 'Surface', value: options.surface, step: (dir) => this.stepShape('surface', dir) },
    ];
  }

  // The game client's check of a falling body: a mesh whose collision carries no mass (a mesh of
  // yours has no collision of its own yet) cannot fall, so the game client took its body away;
  // the editor keeps it so (no undo step of its own: the game decided).
  bodiedKept(answer) {
    if (!answer.ok && this.mesh && this.meshBody.kind === 'falls') {
      this.meshBody = { ...this.meshBody, kind: 'none' };
      this.message = `${shortName(this.mesh)}: ${answer.error}.`;
      this.save();
    }
    this.refresh();
  }

  // ---- Your textures (studio/studio_texture.hpp): pictures on your mesh ----

  texturesKept(names) {
    this.textures = names.map(String);
    this.refresh();
  }

  // The game client's answer to a texture put on the mesh.
  texturedKept(answer) {
    if (answer.ok && answer.name) {
      this.message = `${answer.name} (${answer.width} × ${answer.height}) on ${
        this.mesh ?? 'the mesh'
      }.`;
    } else if (answer.ok) {
      this.message = `${this.mesh ?? 'The mesh'} has its own material again.`;
    } else {
      this.message = `The texture ${answer.name} could not be shown: ${answer.error}.`;
    }
    this.refresh();
  }

  // Puts a texture on your mesh ('' for its own material), one undo step.
  setTexture(name, tiling = this.meshTexture.tiling) {
    if (!this.mesh) {
      this.message = 'Show a mesh first (Props › Static props), then choose its texture here.';
      return;
    }
    const label = name ? `${this.mesh}: texture ${name}` : `${this.mesh}: own material`;
    this.commitMesh({ ...this.meshState(), texture: name, tiling }, label);
    this.message = label;
  }

  // The next texture of your textures ('' first: the mesh's own material).
  stepTexture(direction) {
    const all = ['', ...this.textures];
    const index = Math.max(0, all.indexOf(this.meshTexture.name));
    this.setTexture(all[(index + direction + all.length) % all.length]);
  }

  // Twice or half as many repeats of the texture.
  stepTiling(direction) {
    if (!this.mesh || !this.meshTexture.name) {
      return;
    }
    const tiling = clamp(this.meshTexture.tiling * (direction > 0 ? 2 : 0.5), TILING_RANGE);
    this.setTexture(this.meshTexture.name, tiling);
  }

  // Makes the scene in the game: the prop again with its settings, or none. A prop of another
  // kind goes first.
  apply(scene) {
    const before = this.rope;
    // The prop's own settings go out below: prop keys send theirs again from the next frame.
    this.propSent = {};
    this.propKeysOn = false;
    if (before && (!scene || before.kind !== scene.kind)) {
      this.api.send({ op: PROPS[before.kind].off });
    }
    if (!scene) {
      this.rope = null;
      this.selected = this.selected === 'rope' ? 'character' : this.selected;
      return;
    }
    const kind = PROPS[scene.kind] ? scene.kind : 'rope';
    const prop = PROPS[kind];
    const physics = physicsOf(scene.physics);
    // A change of how it moves only needs the settings; the prop stays as it is.
    const same =
      before &&
      before.kind === kind &&
      before.segments === scene.segments &&
      before.length === scene.length &&
      before.sweep === Boolean(scene.sweep);
    if (!same) {
      // Another rope: keys played back stop (they fit the rope they were recorded on).
      if (this.keysPlaying) {
        this.keysPlaying = false;
        this.api.send({ op: 'studioRopePlay', frames: [] });
      }
      const make = { op: prop.make, segments: scene.segments, length: scene.length };
      if (prop.look) {
        make.look = prop.look;
      }
      this.api.send(make);
      this.api.send({ op: prop.sweep, on: Boolean(scene.sweep) });
    }
    this.api.send({ op: 'studioPropPhysics', ...physics });
    // Where it is held (studio/studio_hold.hpp): a hand, or free in the scene.
    const hold = placing.holdOf(scene.hold);
    this.api.send({ op: 'studioPropHold', ...hold });
    this.rope = {
      kind,
      segments: scene.segments,
      length: scene.length,
      sweep: Boolean(scene.sweep),
      physics,
      hold,
    };
    // Its recorded keys, kept while it stays the same rope.
    const keys = keysOf(scene.keys);
    if (keys && (!before || before.segments === scene.segments)) {
      this.rope.keys = keys;
    }
    // A rope's end, pinned or free (studio/studio_rope.hpp studioRopeEnd); a whip's lash is free.
    if (kind === 'rope' || kind === 'chain') {
      this.rope.end = placing.endOf(scene.end);
      this.api.send({ op: 'studioRopeEnd', on: this.rope.end.pinned, at: this.rope.end.at });
    }
    if (kind === 'whip') {
      this.rope.grip = settingsOf(GRIP_FIELDS, scene.grip);
      // The left hand's joint is the right hand's mirrored: the same grip turns the other way
      // round its axes, so the handle points out of that fist too (across the body).
      const mirrored = hold.joint === 'LeftHand' ? -1 : 1;
      this.api.send({
        op: 'studioPropGrip',
        pitch: mirrored * this.rope.grip.pitch || 0,
        twist: mirrored * this.rope.grip.twist || 0,
      });
    }
  }

  // How the prop moves, one step of a field (PHYSICS_FIELDS) up or down.
  changePhysics(key, direction) {
    this.changeSetting('physics', PHYSICS_FIELDS, key, direction);
  }

  // Ignore characters (one undo step): on, the prop passes through the bodies of the character,
  // players and NPCs and lies only on the floor (collide 2); off, it collides with them too
  // (collide 1). Collide off (0) through everything stays as it is.
  setIgnoreCharacters(on) {
    if (!this.rope?.physics || this.rope.physics.collide === 0) {
      return;
    }
    const collide = on ? 2 : 1;
    if (this.rope.physics.collide === collide) {
      return;
    }
    const label = on ? 'Ignores characters' : 'Collides with characters';
    this.commit({ ...this.rope, physics: { ...this.rope.physics, collide } }, label);
    this.message = on
      ? 'It passes through the bodies of players and NPCs and lies only on the floor.'
      : 'It collides with the bodies of players and NPCs.';
  }

  // How the hand holds the whip, one step of a field (GRIP_FIELDS).
  changeGrip(key, direction) {
    this.changeSetting('grip', GRIP_FIELDS, key, direction);
  }

  // One step of a field of the prop's settings (`part`: physics or grip), as one undo step.
  changeSetting(part, fields, key, direction) {
    const field = fields.find((one) => one.key === key);
    if (!this.rope?.[part] || !field) {
      return;
    }
    const now = this.rope[part][key];
    const value = clamp(Math.round((now + direction * field.step) * 100) / 100, field.range);
    if (value === now) {
      return;
    }
    const label = `${field.label}: ${value}${field.unit ? ` ${field.unit}` : ''}`;
    this.commit({ ...this.rope, [part]: { ...this.rope[part], [key]: value } }, label);
    this.message = label;
  }

  // The scene now, for the other stack: the prop, and the animation when the step changed it.
  snapshot(step) {
    const now = {
      scene: this.rope ? { ...this.rope } : null,
      mesh: this.meshState(),
      label: step.label,
    };
    if (step.animation) {
      now.animation = this.animation.toJSON();
    }
    return now;
  }

  // Makes a step's scene: the prop and the mesh when they changed, the animation when the step
  // kept one.
  restore(step) {
    const sameProp = JSON.stringify(step.scene) === JSON.stringify(this.rope);
    if (!step.animation || !sameProp) {
      this.apply(step.scene);
    }
    if (step.mesh !== undefined && JSON.stringify(step.mesh) !== JSON.stringify(this.meshState())) {
      this.applyMesh(step.mesh);
    }
    if (step.animation) {
      this.stopPlaying();
      this.animation.restore(step.animation);
      this.sendPose();
    }
  }

  undo() {
    const step = this.undoStack.pop();
    if (!step) {
      this.message = 'Nothing to undo.';
      return;
    }
    this.redoStack.push(this.snapshot(step));
    this.restore(step);
    this.message = `Undone: ${step.label}`;
    this.save();
  }

  redo() {
    const step = this.redoStack.pop();
    if (!step) {
      this.message = 'Nothing to redo.';
      return;
    }
    this.undoStack.push(this.snapshot(step));
    this.restore(step);
    this.message = `Redone: ${step.label}`;
    this.save();
  }

  project() {
    const rope = this.rope ? { ...this.rope } : null;
    // Recorded keys too big for one message to the game client are left out of the kept project
    // and saves (8 s of a whip's lash, some 120 kB, stopped the editor's
    // messages); the prop keeps them while the studio is open, as an export leaves them out.
    if (rope?.keys && JSON.stringify(rope.keys).length > PROJECT_KEYS_BYTES) {
      delete rope.keys;
    }
    const project = {
      version: PROJECT_VERSION,
      rope,
      animation: this.animation.toJSON(),
      clothing: { ...this.clothing },
      sky: { ...this.sky },
    };
    if (this.mesh) {
      project.mesh = this.mesh;
      project.meshPlace = { ...this.meshPlace };
      if (this.meshTexture.name) {
        project.meshTexture = { ...this.meshTexture };
      }
      if (this.meshCloth.on) {
        project.meshCloth = { ...this.meshCloth };
      }
      if (this.meshBody.kind !== 'none') {
        project.meshBody = { ...this.meshBody };
      }
    }
    if (Object.keys(this.shapes).length > 0) {
      project.shapes = JSON.parse(JSON.stringify(this.shapes));
    }
    return project;
  }

  save() {
    this.api.send({ op: 'architectSave', map: PROJECT_NAME, data: JSON.stringify(this.project()) });
  }

  // Leave in the menu bar: the project is saved, the studio closes and the game client goes back
  // to the main menu and its server list (architect/local_mode.hpp).
  leave() {
    this.save();
    // The game client's prop and mesh go while their world stands, so the next studio world in
    // this game makes them afresh (the project keeps them).
    this.apply(null);
    this.applyMesh(null);
    this.close();
    this.api.send({ op: 'architectLeave' });
  }

  // The project the game client kept, made again once the editor's screen stands.
  projectKept(data) {
    let project = null;
    try {
      project = data ? JSON.parse(data) : null;
    } catch {
      this.api.log('Asset/Animation Studio: the kept project could not be read');
    }
    this.keptProject = project && project.version === PROJECT_VERSION ? project : null;
    if (this.shell) {
      this.rebuildProject();
    }
  }

  rebuildProject() {
    const kept = this.keptProject;
    this.keptProject = null;
    // The character's clothes: the kept ones, else Henry's.
    this.clothing = clothingOf(kept?.clothing);
    // The sky: the kept one, else noon and clear.
    this.sky = skyOf(kept?.sky);
    // Your mesh, when the project had one; and the list of meshes for Content.
    this.api.send({ op: 'studioMeshList' });
    // The simple props made here, with their options.
    this.shapes = {};
    for (const [name, given] of Object.entries(kept?.shapes ?? {})) {
      const options = /^[A-Za-z0-9_-]{1,48}$/.test(name) ? shapeOptionsOf(given) : null;
      if (options) {
        this.shapes[name] = options;
      }
    }
    const keptMesh =
      typeof kept?.mesh === 'string' &&
      (/^[A-Za-z0-9_-]{1,48}$/.test(kept.mesh) || isGamePath(kept.mesh));
    if (keptMesh) {
      this.applyMesh({
        name: kept.mesh,
        ...placing.meshPlaceOf(kept.meshPlace, MESH_PLACE),
        texture: kept.meshTexture?.name,
        tiling: kept.meshTexture?.tiling,
        cloth: kept.meshCloth?.on === true,
        clothMass: kept.meshCloth?.mass,
        body: kept.meshBody?.kind,
        bodyMass: kept.meshBody?.mass,
      });
    }
    this.api.send({ op: 'studioSky', ...this.sky });
    this.api.send({ op: 'studioGrid', on: this.grid });
    this.api.send(clothingMessage(this.clothing));
    if (kept && kept.rope) {
      const segments = clamp(Number(kept.rope.segments) || 0, ROPE_LIMITS.segments);
      const length = clamp(Number(kept.rope.length) || 0, ROPE_LIMITS.length);
      const kind = PROPS[kept.rope.kind] ? kept.rope.kind : 'rope';
      this.apply({
        kind,
        segments,
        length,
        sweep: Boolean(kept.rope.sweep),
        physics: kept.rope.physics,
        grip: kept.rope.grip,
        hold: kept.rope.hold,
        end: kept.rope.end,
        keys: kept.rope.keys,
      });
      this.selected = 'rope';
      const where = this.rope.hold.joint
        ? `in the ${placing.holdLabel(this.rope.hold).toLowerCase()}`
        : 'free in the scene';
      this.message = `The kept project is back: the ${kind} ${where}.`;
    }
    if (kept && this.animation.restore(kept.animation)) {
      this.sendPose();
    }
  }

  // ---- The rope ----

  makeRope(settings) {
    const kind = PROPS[settings.kind] ? settings.kind : 'rope';
    const label = PROPS[kind].label;
    // A whip ignores characters at first: its lash swings past the bodies (Ignore characters).
    const physics = settings.physics ?? (kind === 'whip' ? { collide: 2 } : undefined);
    // A new prop is held where the one before was.
    this.commit(
      { hold: this.rope?.hold, ...settings, ...(physics ? { physics } : {}), kind, sweep: false },
      `${label} made: ${settings.segments} segments, ${settings.length.toFixed(1)} m`,
    );
    this.selected = 'rope';
    const where = placing.holdLabel(this.rope.hold).toLowerCase();
    this.message = `${label}: ${where}. Change it in Details or drag the gizmo; Physics has the swing test.`;
    this.refresh();
  }

  changeRope(field, step) {
    if (!this.rope) {
      return;
    }
    const delta = field === 'segments' ? step * 2 : step * 0.1;
    const value = clamp(this.rope[field] + delta, ROPE_LIMITS[field]);
    const next = { ...this.rope };
    next[field] = field === 'length' ? Math.round(value * 10) / 10 : value;
    const shown = field === 'length' ? `${next.length.toFixed(1)} m` : String(next.segments);
    this.commit(next, `Rope ${field}: ${shown}`);
  }

  setSweep(on) {
    if (!this.rope) {
      return;
    }
    this.commit({ ...this.rope, sweep: on }, on ? 'Swing test on' : 'Swing test off');
    this.message = on
      ? 'Swing test: the rope’s start goes round a circle fast, like a whip.'
      : 'The rope follows the right hand.';
  }

  // ---- Recording the simulation into keys (studio/studio_rope.hpp) ----

  changeRecordSeconds(direction) {
    this.recordSeconds = clamp(this.recordSeconds + Math.sign(direction || 1), RECORD_SECONDS);
  }

  // The game client records the rope's points for recordSeconds; ropeRecorded gets them.
  recordKeys() {
    if (!this.rope || this.recordingKeys) {
      return;
    }
    if (this.keysPlaying) {
      this.playKeys(false);
    }
    this.recordingKeys = true;
    this.api.send({ op: 'studioRopeRecord', seconds: this.recordSeconds });
    this.message = `Recording ${this.recordSeconds} s of the simulation: move the prop now.`;
  }

  // The whip's lash along the animation as frames (lash_keys.cjs): the animation plays from its
  // start while the game client records the lash (with the handle's frame) for one round, the
  // physics moving it; then each key keeps the lash's shape at its time. Played (here and on
  // players) the lash takes those shapes, key to key, with no physics.
  lashToKeys() {
    if (this.rope?.kind !== 'whip' || this.lashBaking || this.recordingKeys) {
      return;
    }
    const length = this.animation.length();
    if (!(length > 0) || this.animation.keys.length < 2) {
      this.message = 'Make or bake an animation first: the lash is recorded along it.';
      return;
    }
    if (this.keysPlaying) {
      this.playKeys(false);
    }
    // Physics for the recording: the lash moves by itself, as it is to look.
    this.lashBaking = { seconds: Math.min(10, length) };
    this.recordingKeys = true;
    this.setTime(0);
    this.setPlaying(true);
    this.api.send({ op: 'studioRopeRecord', seconds: this.lashBaking.seconds });
    const seconds = this.lashBaking.seconds.toFixed(1);
    this.message = `Recording the lash along the animation (${seconds} s)…`;
    this.refresh();
  }

  // The whip's lash made from the hand's motion: the animation plays from its start for one round
  // while the game client records where the handle is, then the lash is made on that path here
  // and kept as the animation's lash track at its own times, 40 frames a second. By default it is
  // designed on the hand's throw (lash_design.cjs: a loop rolls out to the crack, otherwise it
  // trails the hand and falls); `mode: 'simulated'` simulates it (lash_motion.cjs, its options
  // DEFAULTS: gravity, damping, air, airTip, inner, stiffRoot, stiffTip, taper, friction, rate).
  lashFromMotion(options = {}) {
    if (this.rope?.kind !== 'whip' || this.lashBaking || this.recordingKeys) {
      return false;
    }
    const length = this.animation.length();
    if (!(length > 0) || this.animation.keys.length < 2) {
      this.message = 'Make or bake an animation first: the lash is made along it.';
      return false;
    }
    if (length > 10) {
      this.message = 'Lash from motion takes an animation of up to 10 s.';
      return false;
    }
    if (this.keysPlaying) {
      this.playKeys(false);
    }
    // A little more than a round, so the recording covers the whole loop; two rounds of a loop
    // when they fit 10 s, so a hitch of the game in one is outvoted (lash_motion.cjs handlePath).
    const rounds = this.animation.loop && length * 2 + 0.2 <= 10 ? 2 : 1;
    this.lashBaking = { seconds: Math.min(10, length * rounds + 0.2), motion: options };
    this.recordingKeys = true;
    this.setTime(0);
    this.setPlaying(true);
    this.api.send({ op: 'studioRopeRecord', seconds: this.lashBaking.seconds });
    this.message = `Recording the hand along the animation (${length.toFixed(1)} s)…`;
    this.refresh();
    return true;
  }

  // The handle's recording made the lash track (one undo step); the keys' own lash shapes go
  // (the track shapes the lash now). What it made is lastLash, with how it plays (lashQuality).
  lashMotionKept(recording, options) {
    const started = Date.now();
    // Kept for the assistant (`lashpath`): the hand's path the lash was made on.
    this.lastHandlePath = recording;
    const { mode = 'designed', ...settings } = options;
    const given = {
      ...settings,
      length: this.rope.length,
      seconds: this.animation.length(),
      loop: this.animation.loop,
      ground: STAGE_FEET[2] + LASH_ABOVE_FLOOR,
    };
    // Designed on the hand's throw (lash_design.cjs), or simulated: the best of a few
    // stiffnesses and drags unless the options give them (bestLashMotion).
    const made =
      mode === 'simulated' ? bestLashMotion(recording, given) : designLash(recording, given);
    if (made.error) {
      this.message = `Lash from motion: ${made.error}.`;
      this.lastLash = { error: made.error };
      return;
    }
    this.changeAnimation('Lash from motion', () => {
      // It replaces a lash posed by hand (whose keys would bake over it).
      this.animation.lashKeys = null;
      this.animation.lashTrack = made.track;
      for (const key of this.animation.keys) {
        delete key.lash;
      }
      return true;
    });
    const quality = lashQuality(this.animationByName(), this.rope.length);
    this.lastLash = {
      mode,
      track: { rate: made.track.rate, frames: made.track.frames.length },
      // Designed: the throw it found in the hand's path and the design's numbers it used;
      // simulated: the tip's speed and tries.
      throw: made.throw,
      design: made.design,
      tipSpeed: made.tipSpeed,
      tipSpeedAt: made.tipSpeedAt,
      onFloor: made.onFloor,
      chosen: made.chosen,
      tried: made.tried,
      ms: Date.now() - started,
      quality,
    };
    const problems = quality?.problems.length ?? 0;
    const look = problems > 0 ? ` (${problems} to look at: check)` : '';
    this.message =
      mode === 'simulated'
        ? `The lash moves with the hand: ${made.track.frames.length} frames, its tip up to ` +
          `${made.tipSpeed} m/s${look}.`
        : `The lash cracks on the hand's throw (${made.throw.start}–${made.throw.end} s): ` +
          `${made.track.frames.length} frames${look}.`;
  }

  // ---- The lash posed by hand (animation.cjs lash keys) ----

  // The whip's LASH KEYS rows: key the lash at the playhead, shape the key there (each a step a
  // press of < or >), make keys of the lash shown, take them away.
  lashKeyRows() {
    const keys = this.animation.lashKeys?.keys ?? [];
    const here = this.lashKeyHere();
    const shaping = [
      ['Aim side', 'aimSide', '°'],
      ['Aim up', 'aimUp', '°'],
      ['Curl', 'curl', '°'],
      ['Sway', 'sway', '°'],
      ['Sag', 'sag', ' m'],
      ['Tip twist', 'tipTwist', '°'],
    ];
    return [
      { label: 'LASH KEYS' },
      {
        label: 'Lash key',
        value:
          here >= 0
            ? `Key ${here + 1} of ${keys.length} (${keys[here].time.toFixed(2)} s)`
            : 'Key the lash here',
        press: () => {
          this.addLashKey();
          this.refresh();
        },
      },
      ...shaping.map(([label, name, unit]) => ({
        label,
        value: `± ${LASH_STEPS[name]}${unit}`,
        step: (direction) => this.shapeLashKey({ [name]: Math.sign(direction) * LASH_STEPS[name] }),
      })),
      ...(here >= 0
        ? [
            {
              label: 'Remove',
              value: 'Remove this lash key',
              press: () => {
                this.removeLashKey();
                this.refresh();
              },
            },
          ]
        : []),
      {
        label: 'From the lash',
        value: 'Keys every 0.1 s',
        press: () => {
          this.lashKeysFromLash(0.1);
          this.refresh();
        },
      },
      ...(keys.length > 0
        ? [
            {
              label: 'Clear',
              value: `Take the ${keys.length} lash keys out`,
              press: () => {
                this.clearLashKeys();
                this.refresh();
              },
            },
          ]
        : []),
    ];
  }

  // The lash key at the playhead (a hundredth of a second either way), or -1.
  lashKeyHere() {
    const keys = this.animation.lashKeys?.keys ?? [];
    return keys.findIndex((key) => Math.abs(key.time - this.time) < 0.006);
  }

  // The lash's pieces' ways at the playhead in the body's frame, as it shows now: of the lash
  // keys, else of a lash track kept in the body's frame, else hanging forward and down; null
  // for a lash in the handle's frame (made before the body's frame was kept: make it again).
  lashWaysNow() {
    const keyed = this.animation.lashKeysWaysAt(this.time);
    if (keyed) {
      return keyed;
    }
    const track = this.animation.lashTrack;
    if (track && track.space !== 'body') {
      return null;
    }
    const points = track ? this.animation.lashAt(this.time) : null;
    if (points) {
      let before = [0, 0, 0];
      return points.map((point) => {
        const piece = point.map((part, axis) => part - before[axis]);
        before = point;
        const size = Math.hypot(...piece) || 1;
        return piece.map((part) => part / size);
      });
    }
    // Forward from the hand and curving down to the floor.
    return Array.from({ length: LASH_KEY_PIECES }, (unused, index) => {
      const pitch = ((-20 - 60 * (index / (LASH_KEY_PIECES - 1))) * Math.PI) / 180;
      return [Math.cos(pitch), 0, Math.sin(pitch)];
    });
  }

  // Keys the lash at the playhead as it shows there (one undo step); answers the key's index.
  addLashKey() {
    if (this.rope?.kind !== 'whip') {
      this.message = 'Lash keys are for a whip: make one first.';
      return -1;
    }
    const here = this.lashKeyHere();
    if (here >= 0) {
      return here;
    }
    const ways = this.lashWaysNow();
    if (!ways) {
      this.message = 'This lash was made in the handle’s frame: make it again (Lash from motion).';
      return -1;
    }
    const keys = this.animation.lashKeys?.keys ?? [];
    if (keys.length >= MOST_LASH_KEYS) {
      this.message = `At most ${MOST_LASH_KEYS} lash keys.`;
      return -1;
    }
    const time = Math.round(this.time * 100) / 100;
    this.changeAnimation('Lash key added', () => {
      const lashKeys = this.animation.lashKeys ?? { length: this.rope.length, keys: [] };
      lashKeys.keys.push({ time, ways: ways.map((way) => wayAngles(way)) });
      lashKeys.keys.sort((a, b) => a.time - b.time);
      this.animation.lashKeys = lashKeys;
      return true;
    });
    this.message = `Lash key at ${time.toFixed(2)} s: shape it with Aim, Curl, Sway and Sag.`;
    return this.lashKeyHere();
  }

  // The lash key at the playhead shaped (made first when there is none; one undo step each):
  // { aimSide, aimUp: degrees the whole lash turns (about the body's up, and up across it),
  // curl: degrees it bends up toward the tip (down below 0), sway: sideways toward the tip,
  // sag: metres the tip sinks, tipTwist: degrees its last third turns sideways }.
  shapeLashKey(change) {
    const index = this.addLashKey();
    if (index < 0) {
      return null;
    }
    const label = `Lash key shaped (${Object.keys(change).join(', ')})`;
    this.changeAnimation(label, () => {
      const key = this.animation.lashKeys.keys[index];
      key.ways = shapedWays(key.ways, change, this.animation.lashKeys.length);
      return true;
    });
    this.refresh();
    return this.animation.lashKeys.keys[index];
  }

  removeLashKey() {
    const index = this.lashKeyHere();
    if (index < 0) {
      this.message = 'No lash key at the playhead.';
      return false;
    }
    this.changeAnimation('Lash key removed', () => {
      this.animation.lashKeys.keys.splice(index, 1);
      if (this.animation.lashKeys.keys.length === 0) {
        this.animation.lashKeys = null;
      }
      return true;
    });
    return true;
  }

  // The lash as it shows now (a lash track kept in the body's frame) made lash keys every
  // `every` seconds (one undo step), to change only the frames that need it.
  lashKeysFromLash(every = 0.1) {
    const track = this.animation.lashTrack;
    if (!track || track.space !== 'body') {
      this.message = 'Make the lash from motion first: its frames become the keys.';
      return 0;
    }
    const length = this.animation.length();
    const keys = [];
    const kept = this.time;
    for (let time = 0; time < length - 1e-6 && keys.length < MOST_LASH_KEYS; time += every) {
      this.time = time;
      keys.push({ time: Math.round(time * 100) / 100, ways: this.lashWaysNow().map(wayAngles) });
    }
    this.time = kept;
    this.changeAnimation('Lash keys from the lash', () => {
      this.animation.lashKeys = { length: track.length, keys };
      return true;
    });
    this.message = `${keys.length} lash keys, every ${every} s: change the frames you want.`;
    return keys.length;
  }

  clearLashKeys() {
    if (!this.animation.lashKeys) {
      return false;
    }
    this.changeAnimation('Lash keys cleared', () => {
      this.animation.lashKeys = null;
      return true;
    });
    return true;
  }

  // The lash's recording made the animation's lash keys (one undo step). A looping animation's
  // last key (its first pose again) gets the first key's lash, so it goes round without a seam.
  // Posed-lash settings the keys had (Shape, Curl, Sway) go: the shape is the keys' own now.
  lashKeysKept(recording) {
    const times = this.animation.keys.map((key) => key.time);
    const shapes = lashKeys(recording, times);
    if (shapes.every((shape) => shape === null)) {
      this.message = 'The lash was not recorded with its handle: try again.';
      return;
    }
    if (this.animation.loop && shapes.length > 1 && shapes[0]) {
      shapes[shapes.length - 1] = shapes[0];
    }
    this.changeAnimation('Lash to keys', () => {
      // The keys shape the lash now, not a lash track.
      this.animation.lashTrack = null;
      this.animation.keys.forEach((key, index) => {
        if (shapes[index]) {
          key.lash = shapes[index];
        }
        if (key.prop) {
          delete key.prop.shape;
          delete key.prop.curl;
          delete key.prop.sway;
          if (Object.keys(key.prop).length === 0) {
            delete key.prop;
          }
        }
      });
      return true;
    });
    const keyed = shapes.filter(Boolean).length;
    // What it made, for the assistant (gameclip state's lastLash).
    this.lastLash = { keys: keyed, points: shapes.find(Boolean)?.length ?? 0 };
    this.message =
      `The lash is keyed on ${keyed} keys as it moved: it plays exactly so on every player, ` +
      'no physics.';
  }

  // ---- The assistant's animation tools (animation_tools.cjs, studio MCP) ----

  // The joints' names, asked of the game client when they are not known yet (they come with the
  // skeleton's view); false when they did not come.
  async knownJoints() {
    if (this.joints.length > 0) {
      return true;
    }
    this.sendBones(true);
    for (let wait = 0; wait < 100 && this.joints.length === 0; wait += 1) {
      await new Promise((resolve) => setTimeout(resolve, 50));
    }
    this.sendBones(this.mode === 'bones');
    return this.joints.length > 0;
  }

  // An animation with its joints by name made the animation (one step to undo, `label`), its
  // events and top body only kept; answers what it is now (checkAnimation) or { error }.
  useNamed(data, label) {
    const byName = new Map(this.joints.map((joint) => [joint.name.toLowerCase(), joint.id]));
    const unknown = new Set();
    const keys = data.keys.map((one) => {
      const pose = {};
      for (const [name, turn] of Object.entries(one.pose ?? {})) {
        const id = byName.get(name.toLowerCase());
        if (id === undefined) {
          unknown.add(name);
        } else {
          pose[id] = turn;
        }
      }
      const key = { time: one.time, pose };
      for (const field of ['lift', 'lash', 'prop']) {
        if (one[field] !== undefined) {
          key[field] = one[field];
        }
      }
      return key;
    });
    const next = new Animation();
    const kept = this.animation.toJSON();
    const blend = data.blend ?? kept.blend;
    const loop = data.loop ?? kept.loop;
    // The lash track as the change gives it (null takes it out), else as it was.
    const lashTrack = data.lashTrack !== undefined ? data.lashTrack : kept.lashTrack;
    if (!next.restore({ ...kept, keys, blend, loop, lashTrack })) {
      return { error: 'times must be 0 to 60 s, each key later than the one before' };
    }
    this.stopPlaying();
    this.changeAnimation(label, () => {
      this.animation.restore(next.toJSON());
      return true;
    });
    this.setTime(Math.min(this.time, this.animation.length()));
    this.sendPose();
    return { ...this.animationReport(), unknownJoints: [...unknown] };
  }

  // What the animation is and could have wrong (animation_tools.cjs checkAnimation): the whip's
  // lash checked against its length.
  animationReport() {
    const lashLength = this.rope?.kind === 'whip' ? this.rope.length : 0;
    return checkAnimation(this.animationByName(), { lashLength });
  }

  // An engine console variable's value from the game client (studio/studio_floor.hpp).
  cvarKept(name, value) {
    this.cvarAnswers = { ...(this.cvarAnswers ?? {}), [name]: value };
  }

  // The lash's shape at the playhead to the game client while keys shape it ([] once they stop,
  // or while Lash to keys records with physics).
  sendLash(time) {
    const shape = this.lashBaking ? null : this.animation.lashAt(time);
    if (!shape && !this.lashSent) {
      return;
    }
    this.lashSent = Boolean(shape);
    const points = shape
      ? shape.map((point) => point.map((part) => Math.round(part * 1000) / 1000))
      : [];
    // A lash track kept in the body's frame says so (studio/studio_whip.cpp).
    const space = this.animation.lashTrack?.space === 'body' ? 'body' : 'handle';
    this.api.send({ op: 'studioLashShape', points, space });
  }

  // The recorded keys from the game client: kept with the rope (one undo step) and saved.
  // The recorded keys come in parts (studio/studio_rope.cpp: a whole recording was too big for
  // one message); the last part makes them the prop's keys.
  ropeRecorded(rate, frames, part = 0, parts = 1, handles = null, body = null) {
    if (part === 0) {
      this.arrivingKeys = [];
      this.arrivingHandles = [];
    }
    // The character's forward and up (studio_rope.cpp): a lash from motion is kept in its frame.
    if (body) {
      this.arrivingBody = body;
    }
    this.arrivingKeys = [...(this.arrivingKeys ?? []), ...frames];
    this.arrivingHandles = [...(this.arrivingHandles ?? []), ...(handles ?? [])];
    if (part + 1 < parts) {
      return;
    }
    const all = this.arrivingKeys;
    const allHandles = this.arrivingHandles;
    this.arrivingKeys = [];
    this.arrivingHandles = [];
    this.recordingKeys = false;
    if (this.lashWatching) {
      this.lashWatching = false;
      // How it is drawn (snaps) and whether it moves like a crack, phase by phase.
      const seen = lashSeen({ rate, frames: all });
      const crack = crackCheck({ rate, frames: all });
      this.lastLashLive = {
        ...seen,
        crack,
        problems: [...(seen.problems ?? []), ...(crack.problems ?? [])],
      };
      return;
    }
    if (this.lashBaking) {
      const { motion } = this.lashBaking;
      this.lashBaking = null;
      if (motion) {
        const body = this.arrivingBody ?? null;
        this.lashMotionKept({ rate, frames: all, handles: allHandles, body }, motion);
      } else {
        this.lashKeysKept({ rate, frames: all, handles: allHandles });
      }
      this.refresh();
      return;
    }
    const keys = keysOf({ rate, frames: all });
    if (!this.rope || !keys) {
      this.message = 'Nothing was recorded: the prop had no simulation to record.';
      return;
    }
    this.commit({ ...this.rope, keys }, `Recorded ${keys.frames.length} keys`);
    const seconds = (keys.frames.length / keys.rate).toFixed(1);
    this.message = `${keys.frames.length} keys (${seconds} s) recorded: Play keys shows them.`;
  }

  // The kept keys in place of the simulation, looped, or the simulation again.
  playKeys(on) {
    if (on && !this.rope?.keys) {
      return;
    }
    this.keysPlaying = on;
    if (!on) {
      this.api.send({ op: 'studioRopePlay', frames: [] });
    } else {
      // In parts of KEYS_A_PART keys, as they were recorded (one message is not big enough).
      const { rate, frames } = this.rope.keys;
      const parts = Math.max(1, Math.ceil(frames.length / KEYS_A_PART));
      for (let part = 0; part < parts; part += 1) {
        const share = frames.slice(part * KEYS_A_PART, (part + 1) * KEYS_A_PART);
        this.api.send({ op: 'studioRopePlay', rate, frames: share, part, parts });
      }
    }
    this.message = on ? 'The recorded keys play in a loop.' : 'The prop simulates again.';
  }

  removeKeys() {
    if (!this.rope?.keys) {
      return;
    }
    if (this.keysPlaying) {
      this.playKeys(false);
    }
    const { keys, ...rest } = this.rope;
    this.commit(rest, `Removed ${keys.frames.length} keys`);
  }

  removeRope() {
    if (!this.rope) {
      return;
    }
    const label = PROPS[this.rope.kind].label;
    this.commit(null, `${label} removed`);
    this.selected = 'character';
    this.message = `${label} removed.`;
  }

  // The scene as a server resource: a package and a server script that gives every
  // player who joins the prop with its settings (Player.setPhysicalProp). The game client writes
  // it to %LOCALAPPDATA%\KCDS\studio\exports\<name>\ (studio/studio_export.hpp).
  // The animation as a server script gives it (Player.setPoseAnimation): its joints by name, the
  // keys that turn something; null when it turns nothing.
  exportedAnimation() {
    const animation = this.animationByName();
    if (this.animation.topBody && this.joints.length > 0) {
      // Top body only: the keys of the joints the spine does not carry (the root, the hips, the
      // legs) stay out, so the player's walk moves them.
      // The hips' lift goes too: the walk's hips are the player's own.
      const upper = new Set(this.upperBodyJointNames());
      for (const key of animation.keys) {
        key.pose = Object.fromEntries(Object.entries(key.pose).filter(([name]) => upper.has(name)));
        delete key.lift;
      }
    }
    // Mesh keys go with the export as the server's mesh track (exportedMeshKeys), not in the
    // animation players play.
    for (const key of animation.keys) {
      delete key.mesh;
    }
    const turns = animation.keys.some((key) => Object.keys(key.pose).length > 0);
    if (!turns) {
      return null;
    }
    const exported = { keys: animation.keys, blend: animation.blend, loop: animation.loop };
    // The whip's lash track (a Lash from motion or the lash keys baked): players play it as is.
    if (animation.lashTrack) {
      exported.lashTrack = animation.lashTrack;
    }
    return exported;
  }

  // The mesh keys as the export's server moves each player's mesh: { length, loop,
  // linear, keys: [{ time, offset (metres from the player's feet), turn }] }; null without mesh
  // keys or while the mesh is held (the server's player does not hold it).
  exportedMeshKeys() {
    if (!this.mesh || this.meshPlace.joint || !this.animation.hasMeshKeys()) {
      return null;
    }
    const keys = this.animation.keys
      .filter((one) => one.mesh)
      .map((one) => ({
        time: one.time,
        offset: one.mesh.at.map(
          (part, axis) => Math.round((part - STAGE_FEET[axis]) * 1000) / 1000,
        ),
        turn: [...one.mesh.turn],
      }));
    return {
      length: this.animation.length(),
      loop: this.animation.loop,
      linear: this.animation.blend === 'linear',
      keys,
    };
  }

  // The names of the spine's joints and of all that hangs from them (the arms, the neck, the
  // head), as the game's upper-body clips move them (Client/runtime/animation/upper_body.hpp).
  upperBodyJointNames() {
    const under = (id) => {
      for (let at = id, steps = 0; at >= 0 && steps < 256; steps += 1) {
        if (/^Spine/.test(this.joints[at]?.name ?? '')) {
          return true;
        }
        at = this.joints[at]?.parent ?? -1;
      }
      return false;
    };
    return this.joints.filter((joint) => under(joint.id)).map((joint) => joint.name);
  }

  // The prop as Player.setPhysicalProp takes it: { kind, segments, length, hand, grip?, physics }.
  exportedProp() {
    const settings = this.propSettings();
    const prop = {
      kind: this.rope.kind,
      segments: this.rope.segments,
      length: this.rope.length,
      hand: settings.hand,
    };
    if (this.rope.kind === 'whip') {
      prop.grip = { pitch: settings.pitch, twist: settings.twist };
    }
    prop.physics = {};
    for (const name of ['gravity', 'damping', 'mass', 'air', 'bend', 'stiffness', 'collide']) {
      prop.physics[name] = settings[name];
    }
    // A whip's posed lash, when it is posed at all.
    if (this.rope.kind === 'whip' && (settings.shape || settings.curl || settings.sway)) {
      for (const name of ['shape', 'curl', 'sway']) {
        prop.physics[name] = settings[name] ?? 0;
      }
    }
    return prop;
  }

  exportResource() {
    // The joints' names are needed (an export before them wrote ids no player has): asked
    // first, then the export goes on.
    const posed = this.animation.keys.some((key) => Object.keys(key.pose).length > 0);
    if (this.joints.length === 0 && posed) {
      this.message = 'Export: asking the game for the joints’ names first…';
      this.knownJoints().then((known) => {
        if (known) {
          this.exportResource();
        } else {
          this.message = 'Export: the joints’ names did not come; show Bones and export again.';
        }
        this.refresh();
      });
      return null;
    }
    const animation = this.exportedAnimation();
    const gameClip = this.exportedGameClip();
    // The whole body animated (an animation not top body only, a baked clip's): the player is
    // held where they stand while it plays (lockMovement) and sees themselves from behind.
    const wholeBody = animation !== null && !this.animation.topBody;
    if (!this.rope && !animation && !this.mesh && !gameClip) {
      this.message = 'Make a prop, an animation or show a mesh first: there is nothing to export.';
      return null;
    }
    let name = 'kcds_studio_mesh';
    if (this.rope) {
      name = `kcds_studio_${this.rope.kind}`;
    } else if (animation || gameClip) {
      name = 'kcds_studio_animation';
    }
    // Your mesh: its files go in the resource's stream folder (the game client copies
    // them, studio/studio_export.hpp) and the server puts it beside each player who joins, where
    // it stood from the character in the studio.
    // One of the game's objects is in every player's game already: its own path, no files.
    const gameMesh = isGamePath(this.mesh);
    const meshModel = gameMesh
      ? this.mesh
      : this.mesh
      ? `objects/kcds/${name}/${this.mesh}.cgf`
      : null;
    // A mesh held in a joint goes with the export where a free one stands at first (the server's
    // player does not hold it).
    const standing = this.meshPlace.joint
      ? placing.meshPlaceOf({ scale: this.meshPlace.scale }, MESH_PLACE)
      : this.meshPlace;
    const meshPlace = this.mesh
      ? {
          offset: standing.at.map(
            (part, axis) => Math.round((part - STAGE_FEET[axis]) * 1000) / 1000,
          ),
          turn: [...standing.turn],
          scale: standing.scale,
        }
      : null;
    // The mesh keys, with an animation the players play.
    const meshKeys = meshModel && animation ? this.exportedMeshKeys() : null;
    // The prop as the studio has it: its hand, a whip's grip and its physics go
    // with it, so the server's player holds it as the studio's character does.
    const prop = this.rope ? this.exportedProp() : null;
    const hand = prop?.hand === 'LeftHand' ? 'left' : 'right';
    const gives = [
      prop ? `holds this prop in the ${hand} hand (Player.setPhysicalProp)` : null,
      animation ? `plays the animation ${ANIMATION_NAME} (player.playAnimation)` : null,
      gameClip ? `plays the game’s clip ${gameClip.clip.split('/').at(-1)} again and again` : null,
      meshModel ? `gets the mesh ${shortName(this.mesh)} beside them (Prop.spawn)` : null,
    ].filter(Boolean);
    const files = [
      [
        'package.json',
        `${JSON.stringify(
          {
            name,
            version: '1.0.0',
            kcds: {
              serverScripts: ['server.js'],
              ...(gameClip || wholeBody ? { clientScripts: ['client.js'] } : {}),
            },
          },
          null,
          2,
        )}\n`,
      ],
      [
        'server.js',
        [
          "'use strict';",
          `// Exported from the Asset/Animation Studio: every player who joins ${gives.join(
            ' and ',
          )}.`,
          ...(prop ? [`const PROP = ${JSON.stringify(prop)};`] : []),
          ...(animation
            ? [
                `const ANIMATION = ${JSON.stringify(animation)};`,
                '',
                '// Any resource on the server plays it by name, as one of the game’s own:',
                `// player.playAnimation('${ANIMATION_NAME}'), player.stopAnimation().`,
                `kcds.characters.Animations.define('${ANIMATION_NAME}', ANIMATION);`,
              ]
            : []),
          ...(meshModel
            ? [
                `const MESH = ${JSON.stringify(meshModel)};`,
                `const MESH_PLACE = ${JSON.stringify(meshPlace)};`,
                '// Each player’s mesh, taken away when they leave.',
                'const meshes = new Map();',
              ]
            : []),
          ...(meshKeys
            ? [
                '',
                '// The mesh keys: where the mesh stands (from the player’s feet) and how it is turned',
                '// along the animation; the server moves each player’s mesh ten times a second.',
                `const MESH_KEYS = ${JSON.stringify(meshKeys)};`,
                'const meshTracks = new Map();',
                'function meshAt(time) {',
                '  const keys = MESH_KEYS.keys;',
                '  const after = keys.findIndex((key) => key.time > time);',
                '  if (after === 0) {',
                '    return keys[0];',
                '  }',
                '  if (after < 0) {',
                '    return keys[keys.length - 1];',
                '  }',
                '  const from = keys[after - 1];',
                '  const to = keys[after];',
                '  const linear = (time - from.time) / (to.time - from.time);',
                '  const share = MESH_KEYS.linear ? linear : linear * linear * (3 - 2 * linear);',
                '  const mix = (a, b) => a.map((part, axis) => part + (b[axis] - part) * share);',
                '  return { offset: mix(from.offset, to.offset), turn: mix(from.turn, to.turn) };',
                '}',
              ]
            : []),
          ...(gameClip
            ? [
                `const GAME_CLIP = ${JSON.stringify(gameClip)};`,
                '',
                '// The game’s clip, only its part from..to seconds, again and again: each round',
                '// starts at the part’s start (player.playAnimation with its clip and time).',
                ...(gameClip.topBody
                  ? [
                      '// Only the upper body plays it: the player walks and turns meanwhile.',
                      'const rounds = new Map();',
                      'function playRound(player) {',
                      "  player.playAnimation('', { clip: GAME_CLIP.clip, time: GAME_CLIP.from, upperBody: true });",
                      '}',
                    ]
                  : [
                      '// The whole body plays it: the player does not walk or turn it meanwhile.',
                      'const rounds = new Map();',
                      'function playRound(player) {',
                      "  player.playAnimation('', {",
                      '    clip: GAME_CLIP.clip,',
                      '    time: GAME_CLIP.from,',
                      '    lockMovement: true,',
                      '    lockRotation: true,',
                      '  });',
                      '}',
                    ]),
              ]
            : []),
          '',
          'function give(player) {',
          ...(prop ? ['  player.setPhysicalProp(PROP);'] : []),
          ...(animation && wholeBody
            ? [
                '  // The whole body plays it: the player does not walk or turn it meanwhile.',
                `  player.playAnimation('${ANIMATION_NAME}', { lockMovement: true });`,
              ]
            : []),
          ...(animation && !wholeBody ? [`  player.playAnimation('${ANIMATION_NAME}');`] : []),
          ...(gameClip
            ? [
                '  playRound(player);',
                '  clearInterval(rounds.get(player.id));',
                '  rounds.set(',
                '    player.id,',
                '    setInterval(() => playRound(player), (GAME_CLIP.to - GAME_CLIP.from) * 1000),',
                '  );',
              ]
            : []),
          ...(meshModel
            ? [
                '  const { x, y, z } = player.position;',
                '  const [dx, dy, dz] = MESH_PLACE.offset;',
                '  const at = new kcds.math.Vector3(x + dx, y + dy, z + dz);',
                '  const [rx, ry, rz] = MESH_PLACE.turn;',
                '  const turn = new kcds.math.Vector3(rx, ry, rz);',
                '  meshes.set(player.id, kcds.world.Prop.spawn(MESH, at, turn, MESH_PLACE.scale));',
              ]
            : []),
          ...(meshKeys
            ? [
                '  // Along the animation from now, from where the player stands now.',
                '  const started = Date.now();',
                '  clearInterval(meshTracks.get(player.id));',
                '  meshTracks.set(',
                '    player.id,',
                '    setInterval(() => {',
                '      const elapsed = (Date.now() - started) / 1000;',
                '      const time = MESH_KEYS.loop',
                '        ? elapsed % MESH_KEYS.length',
                '        : Math.min(elapsed, MESH_KEYS.length);',
                '      const { offset, turn: keyTurn } = meshAt(time);',
                '      const place = new kcds.math.Vector3(x + offset[0], y + offset[1], z + offset[2]);',
                '      const facing = new kcds.math.Vector3(keyTurn[0], keyTurn[1], keyTurn[2]);',
                '      meshes.get(player.id)?.setPose(place, facing);',
                '    }, 100),',
                '  );',
              ]
            : []),
          '}',
          '',
          '// Every player who joins, and those already in when the resource (re)starts.',
          "kcds.core.Events.on('playerReady', give);",
          'for (const player of kcds.players.Player.all()) {',
          '  if (player.ready) {',
          '    give(player);',
          '  }',
          '}',
          ...(meshModel || gameClip
            ? [
                '',
                "kcds.core.Events.on('playerDisconnect', (player) => {",
                ...(meshModel
                  ? ['  meshes.get(player.id)?.destroy();', '  meshes.delete(player.id);']
                  : []),
                ...(meshKeys
                  ? [
                      '  clearInterval(meshTracks.get(player.id));',
                      '  meshTracks.delete(player.id);',
                    ]
                  : []),
                ...(gameClip
                  ? ['  clearInterval(rounds.get(player.id));', '  rounds.delete(player.id);']
                  : []),
                '});',
              ]
            : []),
          '',
        ].join('\n'),
      ],
    ];
    // The rope's recorded keys as data beside the scripts, when they fit one file
    // of the export (studio/studio_export.cpp: 64 KiB).
    if (this.rope?.keys) {
      const keysFile = `${JSON.stringify({
        rate: this.rope.keys.rate,
        frames: this.rope.keys.frames,
      })}\n`;
      if (keysFile.length <= 60000) {
        files.push(['rope_keys.json', keysFile]);
      }
    }
    if (gameClip || wholeBody) {
      files.push(['client.js', BODY_VIEW]);
    }
    const message = { op: 'studioExport', name, files };
    if (this.mesh && !gameMesh) {
      message.mesh = {
        name: this.mesh,
        texture: this.meshTexture.name,
        tiling: this.meshTexture.tiling,
      };
    }
    this.api.send(message);
    this.note(`Exported as ${name}`);
    this.message = `Exported as the resource ${name} (KCDS\\studio\\exports\\${name}).`;
    // What it carries, against the limits of the player field and Animations.define.
    const lashLength = prop?.kind === 'whip' ? prop.length : 0;
    const report = animation ? checkAnimation(animation, { lashLength }) : null;
    return {
      name,
      files: files.map(([path]) => path),
      ...(report ? { animation: { ...report, wholeBody } } : {}),
    };
  }

  // A line of History; a change the assistant made (a step over the MCP, script()) says so.
  note(text) {
    const who = this.byAssistant ? 'MCP · ' : '';
    this.history.unshift(`${this.history.length + 1}.  ${who}${text}`);
    this.history.length = Math.min(this.history.length, 8);
  }

  // ---- What the shell shows ----

  items() {
    // Bones mode's content is the timeline (timelineView), not tiles.
    const mode = MODES.find((one) => one.key === this.mode);
    if (mode.folders.length === 0) {
      return [];
    }
    if (GAME_FOLDERS[this.folder]) {
      return this.gameItems(GAME_FOLDERS[this.folder]);
    }
    if (this.folder === 'effects') {
      return EFFECTS.map((name) => ({
        name: `effect:${name}`,
        label: name.split('.').at(-1),
        glyph: 'play',
        meta: name.split('.').slice(0, 2).join('.'),
        effect: name,
      }));
    }
    if (this.folder === 'sounds') {
      return SOUNDS.map((name) => ({
        name: `sound:${name}`,
        label: name,
        glyph: 'play',
        meta: 'The game’s sound',
        sound: name,
      }));
    }
    if (this.folder === 'textures') {
      if (this.textures.length === 0) {
        return [
          {
            name: 'noTexture',
            label: 'No texture yet',
            glyph: 'map',
            meta: 'Put a .png in KCDS\\studio\\textures',
            later: true,
          },
        ];
      }
      return this.textures.map((name) => ({
        name: `texture:${name}`,
        label: name,
        glyph: 'map',
        meta: 'Your texture · on the mesh',
        texture: name,
      }));
    }
    if (this.folder === 'static') {
      // Your meshes, then a simple prop to make here (shapes.cjs).
      const made = SHAPE_KINDS.map((kind) => ({
        name: `shape:${kind.shape}`,
        label: `New ${kind.shape}`,
        glyph: 'box',
        meta: 'A simple prop · made here',
        shape: kind.shape,
      }));
      return [
        ...this.meshes.map((name) => ({
          name: `mesh:${name}`,
          label: name,
          glyph: 'box',
          meta: this.shapes[name]
            ? `Your ${this.shapes[name].shape} · made here`
            : 'Your mesh · Blender',
          mesh: name,
        })),
        ...made,
      ];
    }
    return ITEMS[this.folder] ?? [];
  }

  // The page of the game's objects or clips the game client answered (its folders as the meta).
  gameItems(kind) {
    const kept = this.gameFiles[kind];
    const clips = kind === 'clips';
    if (!kept.ready) {
      return [
        {
          name: 'gameReading',
          label: 'Reading the game',
          glyph: clips ? 'play' : 'box',
          meta: 'The game’s packs, a moment',
          later: true,
        },
      ];
    }
    if (kept.paths.length === 0) {
      return [
        {
          name: 'gameNone',
          label: 'Nothing found',
          glyph: clips ? 'play' : 'box',
          meta: `No ${clips ? 'clip' : 'object'} has “${kept.query}”`,
          later: true,
        },
      ];
    }
    return kept.paths.map((path) => {
      const parts = path.split('/');
      const item = {
        name: path,
        label: parts.at(-1).replace(/\.(cgf|caf)$/i, ''),
        glyph: clips ? 'play' : 'box',
        meta: parts.slice(1, -1).join('/'),
      };
      return clips ? { ...item, gameClip: path } : { ...item, mesh: path };
    });
  }

  // Every outliner line: in Bones mode the character's joints, else the scene.
  outlinerAll() {
    if (this.mode === 'bones') {
      return this.joints.map((joint) => ({
        name: joint.name,
        type: joint.parent >= 0 ? this.joints[joint.parent]?.name ?? '' : 'Root',
        icon: 'move',
        thing: null,
        joint: joint.id,
        depth: 0,
      }));
    }
    const lines = [
      { group: true, name: 'Scene', type: 'Sky', icon: 'folder', thing: 'scene', depth: 0 },
      { name: 'Character', type: 'Body', icon: 'select', thing: 'character', depth: 1 },
    ];
    if (this.rope) {
      const name = PROPS[this.rope.kind].label;
      lines.push({ name, type: 'Physics', icon: 'magnet', thing: 'rope', depth: 1 });
    }
    if (this.mesh) {
      lines.push({
        name: shortName(this.mesh),
        type: 'Mesh',
        icon: 'box',
        thing: 'mesh',
        depth: 1,
      });
    }
    return lines;
  }

  // The outliner lines of the shown page.
  outlinerLines() {
    const start = this.outlinerPage * outlinerRows();
    return this.outlinerAll().slice(start, start + outlinerRows());
  }

  jointDetails() {
    const joint = this.joints[this.joint];
    const rows = [];
    if (joint) {
      const parent = this.joints[joint.parent];
      const turn = this.animation.turnOf(joint.id);
      rows.push(
        { label: 'JOINT' },
        { label: 'Name', value: `${joint.name} (${joint.id})` },
        {
          label: 'Parent',
          value: parent ? parent.name : 'None (root)',
          press: parent ? () => this.chooseJoint(parent.id) : undefined,
        },
        { label: 'TURN' },
        ...['X', 'Y', 'Z'].map((name, axis) => ({
          label: `Turn ${name}`,
          value: `${turn[axis]}°`,
          step: (direction) => this.turnJoint(axis, direction),
        })),
        { label: 'Reset', value: 'Back to the body’s pose', press: () => this.resetJoint() },
      );
    } else {
      rows.push(
        { label: 'SKELETON' },
        { label: 'Joints', value: String(this.joints.length) },
        { label: 'Choose', value: 'Press a ring in the view' },
      );
    }
    const { keys } = this.animation;
    const here = this.keyHere();
    // The key before or after the playhead (Key's - and +).
    const nextKey = (direction) => {
      const times = keys.map((one) => one.time);
      const index =
        direction > 0
          ? times.findIndex((time) => time > this.time + 0.001)
          : times.findLastIndex((time) => time < this.time - 0.001);
      if (index >= 0) {
        this.chooseKey(index);
      }
    };
    rows.push(
      { label: 'ANIMATION' },
      {
        label: 'Key',
        value: here >= 0 ? `${here + 1} of ${keys.length}` : `None here (${keys.length} keys)`,
        step: nextKey,
      },
      {
        label: 'Time',
        value: `${this.time.toFixed(2)} s`,
        step: (direction) => this.moveKey(direction),
      },
      { label: 'Add key', value: 'At the playhead', press: () => this.addKey() },
      {
        label: 'Remove key',
        value: here >= 0 ? `Key ${here + 1}` : 'No key here',
        press: () => this.removeKey(),
      },
      {
        label: 'Play',
        toggle: this.animation.playing,
        press: () => this.setPlaying(!this.animation.playing),
      },
      {
        label: 'Loop',
        toggle: this.animation.loop,
        press: () => {
          this.animation.loop = !this.animation.loop;
        },
      },
      {
        // Only the spine and what hangs from it are animated: on a server the legs keep the
        // player's walk, so they walk while it plays (the hips' and legs' keys stay out).
        label: 'Top body only',
        toggle: this.animation.topBody,
        press: () => {
          this.animation.topBody = !this.animation.topBody;
          this.message = this.animation.topBody
            ? 'Top body only: the legs keep walking while it plays; their keys stay out of the export.'
            : 'The whole body plays it.';
          this.save();
        },
      },
      { label: 'Idle animation', toggle: this.idle, press: () => this.setIdle(!this.idle) },
      {
        label: 'Export',
        value: this.rope ? `kcds_studio_${this.rope.kind}` : 'kcds_studio_animation',
        press: () => this.exportResource(),
      },
    );
    rows.push(...this.propKeyRows(here));
    rows.push(...this.meshKeyRows(here));
    return {
      icon: 'move',
      title: joint ? joint.name : 'Skeleton',
      kind: joint ? 'Joint of the character' : 'Press a joint in the view',
      rows,
    };
  }

  // The rows of a place: X, Y, Z (metres) and Turn X, Y, Z (degrees), each a step.
  placeRows(place, step) {
    const rows = [];
    for (const [axis, name] of ['X', 'Y', 'Z'].entries()) {
      rows.push({
        label: `Place ${name}`,
        value: `${place.at[axis].toFixed(2)} m`,
        axis: name.toLowerCase(),
        step: (dir) => step('at', axis, dir),
      });
    }
    for (const [axis, name] of ['X', 'Y', 'Z'].entries()) {
      rows.push({
        label: `Turn ${name}`,
        value: `${Math.round(place.turn[axis])}°`,
        axis: name.toLowerCase(),
        step: (dir) => step('turn', axis, dir),
      });
    }
    return rows;
  }

  details() {
    if (this.mode === 'bones') {
      return this.jointDetails();
    }
    if (this.mode === 'animation') {
      return this.eventDetails();
    }
    if (this.selected === 'rope' && this.rope && this.mode === 'physics') {
      // Physics mode: how the prop moves.
      const rows = [
        { label: 'PHYSICS' },
        {
          label: 'Swing test',
          toggle: this.rope.sweep,
          press: () => this.setSweep(!this.rope.sweep),
        },
        {
          label: 'Ignore characters',
          toggle: this.rope.physics.collide === 2,
          press: () => this.setIgnoreCharacters(this.rope.physics.collide !== 2),
        },
        ...physicsFieldsOf(this.rope.kind).map((field) => {
          const value = this.rope.physics[field.key];
          let shown = `${value}${field.unit ? ` ${field.unit}` : ''}`;
          if (field.key === 'bend' && value === 0) {
            shown = 'Free';
          } else if (field.key === 'collide') {
            shown = ['Off', 'On', 'Floor only'][value] ?? 'On';
          }
          return {
            label: field.label,
            value: shown,
            step: (dir) => this.changePhysics(field.key, dir),
          };
        }),
        // Recording the simulation into keys.
        { label: 'RECORD INTO KEYS' },
        {
          label: 'Seconds',
          value: `${this.recordSeconds} s`,
          step: (dir) => this.changeRecordSeconds(dir),
        },
        {
          label: 'Record',
          value: this.recordingKeys ? 'Recording…' : 'Record the simulation',
          press: () => this.recordKeys(),
        },
        ...(this.rope.keys
          ? [
              {
                label: 'Keys',
                value: `${this.rope.keys.frames.length} (${(
                  this.rope.keys.frames.length / this.rope.keys.rate
                ).toFixed(1)} s)`,
              },
              {
                label: 'Play keys',
                toggle: this.keysPlaying,
                press: () => this.playKeys(!this.keysPlaying),
              },
              { label: 'Remove keys', value: 'Remove the keys', press: () => this.removeKeys() },
            ]
          : []),
        // A whip's lash along the animation made the keys' lash shapes (lash_keys.cjs): frames,
        // not physics, on every player.
        // Or made from the hand's motion (lash_motion.cjs): the lash simulated on the hand's path
        // and kept at its own times, 40 frames a second.
        ...(this.rope.kind === 'whip'
          ? [
              {
                label: 'Lash from motion',
                value: this.lashBaking?.motion ? 'Recording the hand…' : 'Make it from the hand',
                press: () => this.lashFromMotion(),
              },
              {
                label: 'Lash to keys',
                value:
                  this.lashBaking && !this.lashBaking.motion
                    ? 'Recording the lash…'
                    : 'Lash along the animation',
                press: () => this.lashToKeys(),
              },
              ...this.lashKeyRows(),
            ]
          : []),
      ];
      const prop = PROPS[this.rope.kind];
      return { icon: 'magnet', title: prop.label, kind: prop.kind, rows };
    }
    if (this.selected === 'rope' && this.rope) {
      const rows = [
        { label: this.rope.kind === 'whip' ? 'LASH' : 'ROPE' },
        {
          label: 'Segments',
          value: String(this.rope.segments),
          step: (dir) => this.changeRope('segments', dir),
        },
        {
          label: 'Length',
          value: `${this.rope.length.toFixed(1)} m`,
          step: (dir) => this.changeRope('length', dir),
        },
        {
          label: 'Held in',
          value: placing.holdLabel(this.rope.hold),
          step: (dir) => this.changeHold(dir),
        },
        ...(this.rope.hold.joint
          ? []
          : this.placeRows(this.rope.hold, (part, axis, dir) => this.stepHold(part, axis, dir))),
        ...(this.rope.end
          ? [
              {
                label: 'Pin end',
                toggle: this.rope.end.pinned,
                press: () => this.setEndPinned(!this.rope.end.pinned),
              },
              ...(this.rope.end.pinned
                ? ['X', 'Y', 'Z'].map((name, axis) => ({
                    label: `End ${name}`,
                    value: `${this.rope.end.at[axis].toFixed(2)} m`,
                    axis: name.toLowerCase(),
                    step: (dir) => this.stepEnd(axis, dir),
                  }))
                : []),
            ]
          : []),
        ...(this.rope.grip
          ? GRIP_FIELDS.map((field) => ({
              label: field.label,
              value: `${this.rope.grip[field.key]} ${field.unit}`,
              step: (dir) => this.changeGrip(field.key, dir),
            }))
          : []),
        {
          label: 'Physics',
          value: 'Gravity, weight… (2)',
          press: () => this.useMode('physics'),
        },
        {
          label: 'Remove',
          value: `Remove the ${this.rope.kind}`,
          press: () => this.removeRope(),
        },
        // A whip's lash posed by hand, frame by frame (here as in its Physics panel).
        ...(this.rope.kind === 'whip' ? this.lashKeyRows() : []),
        { label: 'RESOURCE' },
        {
          label: 'Export',
          value: `kcds_studio_${this.rope.kind}`,
          press: () => this.exportResource(),
        },
      ];
      const prop = PROPS[this.rope.kind];
      return { icon: 'magnet', title: prop.label, kind: prop.kind, rows };
    }
    if (this.selected === 'mesh' && this.mesh && this.mode === 'physics') {
      // Physics mode: your mesh's body or your mesh as cloth.
      const cloth = this.meshCloth;
      const body = this.meshBody;
      return {
        icon: 'magnet',
        title: this.mesh,
        kind: 'Your mesh, made in Blender',
        rows: [
          { label: 'BODY' },
          {
            label: 'Body',
            value: MESH_BODIES.find((one) => one.kind === body.kind).label,
            step: (dir) => this.stepBody(dir),
          },
          {
            label: 'Body weight',
            value: body.kind === 'falls' ? `${body.mass} kg` : '—',
            step: (dir) => this.stepBodyMass(dir),
          },
          { label: 'CLOTH' },
          { label: 'Cloth', toggle: cloth.on, press: () => this.setCloth(!cloth.on) },
          {
            label: 'Weight',
            value: `${cloth.mass} kg`,
            step: (dir) => this.stepClothMass(dir),
          },
          { label: 'Hangs from', value: cloth.on ? 'Its highest vertices' : '—' },
        ],
      };
    }
    if (this.selected === 'mesh' && this.mesh) {
      return {
        icon: 'box',
        title: shortName(this.mesh),
        kind: isGamePath(this.mesh)
          ? 'The game’s own object'
          : this.shapes[this.mesh]
          ? 'A simple prop, made here'
          : 'Your mesh, made in Blender',
        rows: [
          { label: 'MESH' },
          {
            label: 'File',
            value: isGamePath(this.mesh) ? this.mesh : `KCDS\\studio\\meshes\\${this.mesh}.cgf`,
          },
          { label: 'Reload', value: 'Load the file again', press: () => this.reloadMesh() },
          ...this.shapeRows(),
          { label: 'PLACE' },
          {
            label: 'Held in',
            value: meshHoldLabel(this.meshPlace),
            step: (dir) => this.changeMeshHold(dir),
          },
          ...this.placeRows(this.meshPlace, (part, axis, dir) => this.stepMesh(part, axis, dir)),
          {
            label: 'Size',
            value: `${this.meshPlace.scale.toFixed(2)} ×`,
            step: (dir) => this.stepMesh('scale', 0, dir),
          },
          { label: 'TEXTURE' },
          {
            label: 'Texture',
            value: this.meshTexture.name || 'Own material',
            step: (dir) => this.stepTexture(dir),
          },
          {
            label: 'Tiling',
            value: this.meshTexture.name ? `${this.meshTexture.tiling} ×` : '—',
            step: (dir) => this.stepTiling(dir),
          },
          {
            label: 'Remove',
            value: `Remove ${this.mesh}`,
            press: () => {
              this.showMesh('');
              this.selected = 'character';
              this.save();
            },
          },
        ],
      };
    }
    if (this.selected === 'scene') {
      const weather = WEATHERS.find((one) => one.preset === this.sky.preset) ?? WEATHERS[0];
      return {
        icon: 'folder',
        title: 'Scene',
        kind: 'The studio’s sky and light',
        rows: [
          { label: 'SKY' },
          {
            label: 'Time of day',
            value: `${String(this.sky.hour).padStart(2, '0')}:00`,
            step: (dir) => this.changeSky('hour', dir),
          },
          { label: 'Weather', value: weather.label, step: (dir) => this.changeSky('preset', dir) },
          {
            label: 'Rain',
            value: this.sky.rain === 0 ? 'None' : `${Math.round(this.sky.rain * 100)} %`,
            step: (dir) => this.changeSky('rain', dir),
          },
        ],
      };
    }
    if (this.selected === 'character') {
      const outfit = outfitOf(this.clothing.outfit);
      return {
        icon: 'select',
        title: 'Character',
        kind: 'The body props are held by',
        rows: [
          { label: 'CHARACTER' },
          { label: 'Body', value: 'Henry (male)' },
          { label: 'Bones', value: 'Bones mode (5)', press: () => this.useMode('bones') },
          { label: 'CLOTHES' },
          {
            label: 'Outfit',
            value: outfit.label,
            step: (dir) => this.changeOutfit(dir),
          },
          ...outfit.pieces.map((piece) => ({
            label: piece.name,
            toggle: !this.clothing.off.includes(piece.name),
            press: () => this.togglePiece(piece.name),
          })),
        ],
      };
    }
    return {
      icon: 'select',
      title: 'Nothing selected',
      kind: 'Choose in the outliner or in Content',
      rows: [],
    };
  }

  view() {
    const mode = MODES.find((one) => one.key === this.mode);
    const lines = this.outlinerLines();
    const outlinerPages = Math.max(1, Math.ceil(this.outlinerAll().length / outlinerRows()));
    const bones = this.mode === 'bones';
    const items = this.items();
    // A game folder's items are the page the game client answered.
    const game = this.gameFiles[GAME_FOLDERS[this.folder]] ?? null;
    const pages = game ? game.pages : Math.max(1, Math.ceil(items.length / TILE_COUNT));
    const page = game ? game.page : this.page;
    const shown = game ? items : items.slice(this.page * TILE_COUNT, (this.page + 1) * TILE_COUNT);
    const searching = Boolean(game) && this.typing && this.typingInto === 'search';
    const crumb =
      mode.folders.length > 0
        ? `${mode.label}   ›   ${FOLDERS[this.folder] ?? ''}`
        : `${mode.label}   ›   Animation keys`;
    const objects = this.rope ? 2 : 1;
    return {
      camera: { speed: this.cameraSpeed },
      menu: {
        title: 'Asset/Animation Studio',
        map: this.saveName || 'Studio',
        saved: 'Saved · local only',
        saves: true,
      },
      toolbar: {
        tools: TOOLS.map((tool) => tool.key),
        tool: TOOLS.findIndex((tool) => tool.key === this.tool),
        snaps: [
          {
            label: this.rope
              ? placing.holdLabel(this.rope.hold).replace(' in the scene', '')
              : 'Right hand',
            on: Boolean(this.rope),
          },
          { label: 'Floor', on: this.floor },
          { label: 'Swing test', on: Boolean(this.rope?.sweep) },
          { label: 'Grid', on: this.grid },
        ],
        // A mode that comes later is drawn dim; its label stays short enough to fit.
        modes: MODES.map((one, index) => ({
          label: `${one.label}  ${index + 1}`,
          icon: one.icon,
          later: Boolean(one.later),
        })),
        mode: MODES.findIndex((one) => one.key === this.mode),
        play: this.characterView ? 'Free camera' : 'View as character',
      },
      outliner: {
        search: bones ? `Search ${this.joints.length} joints` : `Search ${objects} objects`,
        query: false,
        typing: false,
        filtered: false,
        lines,
        chosen: bones
          ? lines.findIndex((line) => line.joint === this.joint)
          : lines.findIndex((line) => line.thing !== null && line.thing === this.selected),
        footer: bones ? `${this.joints.length} joints` : `${objects} objects`,
        pager: ['<<', `${this.outlinerPage + 1} / ${outlinerPages}`, '>>'],
      },
      details: this.details(),
      dock: {
        tab: this.tab,
        tabs: TABS,
        ai: this.tab === AI_TAB ? aiConnectView(this.ai) : undefined,
        folders: mode.folders.slice(0, FOLDER_ROWS).map((key) => ({
          label: FOLDERS[key],
          count: String(
            {
              static: this.meshes.length,
              textures: this.textures.length,
              gameProps: this.gameFiles.objects.ready ? this.gameFiles.objects.total : '…',
              gameClips: this.gameFiles.clips.ready ? this.gameFiles.clips.total : '…',
            }[key] ?? (ITEMS[key] ?? []).length,
          ),
        })),
        folder: mode.folders.indexOf(this.folder),
        crumb,
        crumbWidth: Math.round(crumb.length * 7.4),
        chip: game ? `Found ${game.total}` : `All ${items.length}`,
        search: game
          ? game.query || searching
            ? `${game.query}${searching ? '|' : ''}`
            : 'Search the game’s files'
          : 'Search this folder',
        query: Boolean(game?.query),
        typing: searching,
        pager: ['<<', `${page + 1} / ${pages}`, '>>'],
        tiles: shown.map((item) => ({
          name: item.label,
          meta: item.meta,
          glyph: item.glyph,
          picture: null,
          loading: false,
        })),
        tile:
          this.folder === 'static' || this.folder === 'gameProps'
            ? shown.findIndex((item) => item.mesh && item.mesh === this.mesh)
            : this.folder === 'gameClips'
            ? shown.findIndex((item) => item.gameClip && item.gameClip === this.gameClip)
            : this.folder === 'textures'
            ? shown.findIndex((item) => item.texture && item.texture === this.meshTexture.name)
            : this.rope
            ? shown.findIndex((item) => item.name === this.rope.kind)
            : -1,
        // Bones mode draws the timeline in the content tab (timeline.cjs).
        custom: bones || this.tab === TIMELINE_TAB ? this.timelineView() : null,
        history: this.history.length > 0 ? this.history : ['No changes yet.'],
        issueSummary: 'The studio is local: nothing here goes to a server.',
        issues: [],
        saves: this.tab === SAVES_TAB ? this.savesView() : null,
      },
      status: {
        selected: this.message,
        cursor: '',
        snaps: this.animation.playing
          ? 'Playing'
          : this.rope?.sweep
          ? 'Swing test on'
          : this.rope
          ? placing.holdLabel(this.rope.hold)
          : 'Right hand',
        objects: `${objects} objects`,
      },
    };
  }

  // Test builds only: a line of the kit's script (the studio's local test hook).
  async script(line) {
    // A long step in parts (the assistant's bridge takes lines of up to 4 KB,
    // architect/ai_bridge.cpp): `part <index> <count> <text>`; the last part runs the whole.
    const part = /^part (\d+) (\d+) ([\s\S]*)$/.exec(line);
    if (part) {
      const index = Number(part[1]);
      const count = Number(part[2]);
      if (index === 0) {
        this.scriptParts = [];
      }
      this.scriptParts = this.scriptParts ?? [];
      this.scriptParts[index] = part[3];
      if (index + 1 < count) {
        return `part ${index} -> ${JSON.stringify({ part: index, of: count })}`;
      }
      const whole = this.scriptParts.join('');
      this.scriptParts = [];
      return this.script(whole);
    }
    const [word, ...rest] = line.trim().split(/\s+/);
    const number = (index) => Number(rest[index]);
    // The step's text after its word, as given (a JSON argument keeps its spaces).
    const text = () => line.trim().slice(word.length).trim();
    const steps = {
      open: () => this.open(),
      close: () => this.close(),
      mode: () => this.useMode(rest[0]),
      folder: () => {
        this.pressFolder(number(0));
        return { folder: this.folder, tiles: this.items().map((item) => item.label) };
      },
      tile: () => this.pressTile(number(0)),
      // `tab Content|Timeline|History|Saves|AI`: the dock's tab.
      tab: () => {
        const index = TABS.findIndex(
          (name) => name.toLowerCase() === String(rest[0]).toLowerCase(),
        );
        if (index < 0) {
          return `tabs: ${TABS.join(', ')}`;
        }
        this.pressTab(index);
        return { tab: TABS[this.tab] };
      },
      outliner: () => this.pressOutliner(number(0)),
      detail: () => this.pressDetail(number(0), number(1)),
      snap: () => this.pressSnap(number(0)),
      undo: () => this.undo(),
      redo: () => this.redo(),
      // The assistant's steps (Dev/tools/architect-mcp): what to make and change.
      make: () => {
        if (!PROPS[rest[0]]) {
          return `unknown prop ${rest[0]}`;
        }
        this.makeRope({ ...PROPS[rest[0]].first, kind: rest[0] });
        return this.rope;
      },
      set: () => {
        if (!this.rope || !ROPE_LIMITS[rest[0]]) {
          return 'nothing to set';
        }
        this.changeRope(rest[0], Math.sign(number(1)) || 1);
        return this.rope;
      },
      sweep: () => {
        this.setSweep(rest[0] === 'on');
        return this.rope;
      },
      // Recording into keys: "record <seconds>", "playkeys on|off", "keys".
      record: () => {
        if (Number.isFinite(number(0))) {
          this.recordSeconds = clamp(Math.round(number(0)), RECORD_SECONDS);
        }
        this.recordKeys();
        return { recording: this.recordingKeys, seconds: this.recordSeconds };
      },
      playkeys: () => {
        this.playKeys(rest[0] === 'on');
        return { playing: this.keysPlaying };
      },
      keys: () =>
        this.rope?.keys
          ? { keys: this.rope.keys.frames.length, points: this.rope.keys.frames[0].length / 3 }
          : null,
      // `keyframes <first> <count>`: the recorded keys from `first`, at most KEYS_A_PART of them
      // (each the prop's points in world metres, start first, x y z each).
      keyframes: () => {
        if (!this.rope?.keys) {
          return null;
        }
        const first = Math.max(0, Math.floor(number(0)) || 0);
        const count = clamp(Math.floor(number(1)) || KEYS_A_PART, [1, KEYS_A_PART]);
        const { rate, frames } = this.rope.keys;
        return { rate, total: frames.length, first, frames: frames.slice(first, first + count) };
      },
      remove: () => {
        this.removeRope();
        return null;
      },
      state: () => this.state(),
      // The joints' names first: an export before them wrote joint ids no player has.
      export: async () => {
        if (!(await this.knownJoints())) {
          return 'the joints are not known: show Bones first';
        }
        return this.exportResource();
      },
      // The skeleton view: `bones on|off`, `joint <id>`, `joints` (the names and parents).
      bones: () => {
        this.useMode(rest[0] === 'off' ? 'props' : 'bones');
        return { mode: this.mode, joints: this.joints.length };
      },
      joint: () => {
        if (this.mode !== 'bones') {
          this.useMode('bones');
        }
        this.chooseJoint(number(0));
        return this.joints[this.joint] ?? `no joint ${rest[0]}`;
      },
      joints: () => this.joints,
      // Posing and keys: `turn x|y|z 1|-1`, `pose` (the chosen key), `key add|remove|<n>`
      // (add and remove at the playhead), `time <seconds>` (the playhead), `keytime 1|-1`,
      // `play on|off`, `pick <stage x> <stage y>` (a press in the view), `tl <x> <y>` and
      // `tlrelease <x> <y>` (a press and a release in the timeline, dock places).
      time: () => {
        this.setTime(number(0));
        return { time: this.time, key: this.keyHere() };
      },
      // `jointinfo <name> ...`: each joint's geometry from the game client (the body's frame:
      // right, forward, up): place, bone, its parent's axes and what +30 degrees about each
      // moves. Waits for the answer (Bones mode must be on).
      jointinfo: async () => {
        if (this.mode !== 'bones') {
          this.useMode('bones');
        }
        this.jointInfo = null;
        for (let wait = 0; wait < 40 && this.joints.length === 0; wait += 1) {
          await new Promise((resolve) => setTimeout(resolve, 50));
        }
        this.api.send({ op: 'studioJointInfo', names: rest });
        for (let wait = 0; wait < 40 && !this.jointInfo; wait += 1) {
          await new Promise((resolve) => setTimeout(resolve, 50));
        }
        return this.jointInfo ?? 'the game client did not answer';
      },
      // `tool select|move|rotate|scale`: the toolbar's tool (the props' gizmo follows it).
      tool: () => {
        this.useTool(TOOLS.some((one) => one.key === rest[0]) ? rest[0] : null);
        return { tool: this.tool, gizmo: this.gizmoThing(), as: this.gizmoTool() };
      },
      // `place <json>`: where things stand, any of { prop: { joint, at, turn },
      // mesh: { at, turn, scale } } (joint 'RightHand', 'LeftHand' or '' for free); `place`
      // alone: where they stand now.
      place: () => {
        const text = line.trim().slice(word.length).trim();
        if (text) {
          const given = JSON.parse(text);
          if (given.prop && this.rope) {
            const hold = placing.holdOf({ ...this.rope.hold, ...given.prop });
            this.commit({ ...this.rope, hold }, `${PROPS[this.rope.kind].label} placed`);
          }
          if (given.end && this.rope?.end) {
            const end = placing.endOf({ ...this.rope.end, ...given.end });
            this.commit({ ...this.rope, end }, end.pinned ? 'Rope end pinned' : 'Rope end free');
          }
          if (given.mesh && this.mesh) {
            // Into another joint (or free) it starts at that joint (or beside the character),
            // unless at and turn are given too.
            const holdChanges =
              typeof given.mesh.joint === 'string' && given.mesh.joint !== this.meshPlace.joint;
            const before = holdChanges ? { scale: this.meshPlace.scale } : this.meshPlace;
            const place = placing.meshPlaceOf({ ...before, ...given.mesh }, MESH_PLACE);
            this.commitMesh({ ...this.meshState(), ...place }, `${this.mesh} placed`);
          }
          this.refresh();
        }
        return {
          prop: this.rope
            ? { kind: this.rope.kind, ...this.rope.hold, heldIn: placing.holdLabel(this.rope.hold) }
            : null,
          end: this.rope?.end ?? null,
          mesh: this.mesh ? { name: this.mesh, ...this.meshPlace } : null,
          holds: placing.HOLDS.map((one) => one.joint),
        };
      },
      // The camera: `viewcam front|side|top|frame`, `orbitdrag orbit|pan|zoom <dx> <dy>`.
      viewcam: () => {
        if (rest[0] === 'frame') {
          this.frameChosen();
        } else {
          this.showView(rest[0]);
        }
        return this.orbit;
      },
      orbitdrag: () => {
        const at = this.api.NoClip.getPosition?.();
        const target = this.orbit?.target ?? this.focusPoint();
        const start = at
          ? camera.orbitOf([at.x, at.y, at.z], target)
          : camera.viewOf('front', target);
        this.setOrbit(camera.dragged(start, rest[0], number(1), number(2)));
        return this.orbit;
      },
      // The Saves tab: `saves` (opens it, asks for the list), `saveas <name>`, `load <name>`,
      // `deletesave <name>`.
      saves: () => {
        this.pressTab(SAVES_TAB);
        return this.saves;
      },
      saveas: () => {
        this.saveName = rest.join(' ');
        this.saveAs();
        return this.message;
      },
      load: () => {
        const row = this.saves.findIndex((save) => save.name === rest.join(' '));
        this.pressSave(row, 'load');
        return row >= 0 ? 'asked' : `no save ${rest.join(' ')}`;
      },
      deletesave: () => {
        const row = this.saves.findIndex((save) => save.name === rest.join(' '));
        this.pressSave(row, 'delete');
        return row >= 0 ? 'deleted' : `no save ${rest.join(' ')}`;
      },
      // `textures`: asks the game client for your textures; `texture <name> [tiling]` puts one
      // on your mesh (`texture` alone: its own material again).
      textures: () => {
        this.api.send({ op: 'studioTextureList' });
        return this.textures;
      },
      texture: () => {
        const tiling = Number(rest[1]);
        this.setTexture(
          rest[0] ?? '',
          Number.isFinite(tiling) && tiling > 0 ? tiling : this.meshTexture.tiling,
        );
        this.refresh();
        return { mesh: this.mesh, texture: this.meshTexture, message: this.message };
      },
      // `gameclip <Animations/....caf> [once]` plays one of the game's clips on the character
      // (studio/studio_game_clip.hpp); `gameclip` alone stops it.
      // `gameclip <path> [once | {"loop","from","to","inPlace"}]` plays it; `gameclip part
      // {...}` changes the part of the one playing; `gameclip state` says where it is.
      // The assistant's animation tools (animation_tools.cjs): each change one step to undo,
      // answered with what the animation is now and what could be wrong with it.
      // `patch <json>`: some keys' joints, lift or lash (patchAnimation).
      patch: async () => {
        if (!(await this.knownJoints())) {
          return 'the joints are not known: show Bones first';
        }
        const result = patchAnimation(this.animationByName(), JSON.parse(text()));
        return { patched: result.keys, ...this.useNamed(result.animation, 'Keys patched') };
      },
      // `track <json>`: a joint's whole track copied, mirrored, scaled, offset, smoothed or
      // taken out (trackAnimation).
      track: async () => {
        if (!(await this.knownJoints())) {
          return 'the joints are not known: show Bones first';
        }
        const change = JSON.parse(text());
        const result = trackAnimation(this.animationByName(), change);
        if (result.error) {
          return result;
        }
        return { changed: result.keys, ...this.useNamed(result.animation, `Track ${change.op}`) };
      },
      // `retime <json>`: the keys' times sped up or slowed down, trimmed or shifted.
      retime: async () => {
        if (!(await this.knownJoints())) {
          return 'the joints are not known: show Bones first';
        }
        const change = JSON.parse(text());
        const result = retimeAnimation(this.animationByName(), change);
        if (result.error) {
          return result;
        }
        return this.useNamed(result.animation, `Retimed (${change.op})`);
      },
      // `history`: History's lines (newest first, the assistant's marked MCP) and what undo and
      // redo would take back or do again.
      history: () => ({
        history: [...this.history],
        undo: this.undoStack.map((step) => step.label ?? 'a change').reverse(),
        redo: this.redoStack.map((step) => step.label ?? 'a change').reverse(),
      }),
      // `check`: what the animation is and could have wrong, and what an export of it will be.
      check: async () => {
        await this.knownJoints();
        return this.animationReport();
      },
      // `compare <json>`: how far another animation (joints by name) is from this one.
      compare: () => compareAnimations(this.animationByName(), JSON.parse(text())),
      // `lashshape <json>`: { keys?/times?/from/to, curl, sway, sag, count? }: the chosen keys'
      // lash shape made from a few numbers (lashShape), the whip's length kept.
      lashshape: async () => {
        if (this.rope?.kind !== 'whip') {
          return 'make a whip first (studio_make_prop whip)';
        }
        if (!(await this.knownJoints())) {
          return 'the joints are not known: show Bones first';
        }
        const given = JSON.parse(text());
        const lash = lashShape({ ...given, length: this.rope.length });
        const result = patchAnimation(this.animationByName(), { ...given, lash });
        return { shaped: result.keys, lash, ...this.useNamed(result.animation, 'Lash shaped') };
      },
      // `cvar <name> [value]`: an engine console variable read or set (studio/studio_floor.hpp).
      cvar: async () => {
        const name = rest[0] ?? '';
        if (!/^[A-Za-z0-9_]{1,63}$/.test(name)) {
          return 'name a console variable';
        }
        const message = { op: 'studioCVar', name };
        if (rest[1] !== undefined) {
          message.value = Math.round(number(1));
        }
        this.cvarAnswers = { ...(this.cvarAnswers ?? {}) };
        delete this.cvarAnswers[name];
        this.api.send(message);
        for (let wait = 0; wait < 40 && !(name in this.cvarAnswers); wait += 1) {
          await new Promise((resolve) => setTimeout(resolve, 50));
        }
        return name in this.cvarAnswers
          ? { name, value: this.cvarAnswers[name] }
          : 'the game client did not answer';
      },
      // `sample <json>`: { times: [s, ...], joints: [names] }: where the joints are at each time
      // as the animation poses the body (from the body's place, its frame: right, forward, up).
      sample: async () => {
        const { times = [0], joints = [] } = JSON.parse(text());
        if (!(await this.knownJoints())) {
          return 'the joints are not known: show Bones first';
        }
        const mode = this.mode;
        if (mode !== 'bones') {
          this.useMode('bones');
        }
        this.stopPlaying();
        const samples = [];
        for (const time of times.slice(0, 20)) {
          this.setTime(Math.max(0, Math.min(this.animation.length(), Number(time) || 0)));
          this.sendPose();
          // The pose reaches the body in a frame or two, the bones' places after it.
          await new Promise((resolve) => setTimeout(resolve, 250));
          this.jointInfo = null;
          this.api.send({ op: 'studioJointInfo', names: joints.slice(0, 32) });
          for (let wait = 0; wait < 40 && !this.jointInfo; wait += 1) {
            await new Promise((resolve) => setTimeout(resolve, 50));
          }
          const at = {};
          for (const joint of this.jointInfo?.joints ?? []) {
            at[joint.name] = joint.found ? joint.position : null;
          }
          samples.push({ time: this.time, joints: at });
        }
        if (mode !== 'bones') {
          this.useMode(mode);
        }
        return { frame: 'right, forward, up (metres from the body’s place)', samples };
      },
      // `frame <json>`: { target: <joint> | 'legs' | 'hand' | 'whole', view: 'front' | 'back' |
      // 'right' | 'left' | 'top', distance }: the camera put by the target, looking at it.
      frame: async () => {
        const { target = 'whole', view = 'front', distance = 2.5 } = JSON.parse(text() || '{}');
        if (!(await this.knownJoints())) {
          return 'the joints are not known: show Bones first';
        }
        const groups = {
          legs: ['LeftLeg', 'RightLeg', 'LeftFoot', 'RightFoot'],
          hand: [this.rope?.hold?.joint === 'LeftHand' ? 'LeftHand' : 'RightHand'],
          whole: ['Hips', 'Head', 'LeftFoot', 'RightFoot'],
        };
        const names = groups[target] ?? [target];
        const mode = this.mode;
        if (mode !== 'bones') {
          this.useMode('bones');
        }
        this.jointInfo = null;
        for (let wait = 0; wait < 6; wait += 1) {
          await new Promise((resolve) => setTimeout(resolve, 50));
        }
        this.api.send({ op: 'studioJointInfo', names });
        for (let wait = 0; wait < 40 && !this.jointInfo; wait += 1) {
          await new Promise((resolve) => setTimeout(resolve, 50));
        }
        if (mode !== 'bones') {
          this.useMode(mode);
        }
        const info = this.jointInfo;
        const found = (info?.joints ?? []).filter((joint) => joint.found);
        if (!info?.place || !info.forward || found.length === 0) {
          return `no ${target} on the body`;
        }
        // The target: the joints' middle, in the world.
        const middle = [0, 1, 2].map(
          (axis) => found.reduce((sum, joint) => sum + joint.position[axis], 0) / found.length,
        );
        const forward = info.forward;
        const right = [forward[1], -forward[0], 0];
        const world = [0, 1, 2].map((axis) => {
          const up = axis === 2 ? middle[2] : 0;
          return info.place[axis] + right[axis] * middle[0] + forward[axis] * middle[1] + up;
        });
        const ways = {
          front: forward,
          back: forward.map((part) => -part),
          right,
          left: right.map((part) => -part),
          top: [forward[0] * 0.1, forward[1] * 0.1, 1],
        };
        const way = ways[view] ?? ways.front;
        const length = Math.hypot(...way) || 1;
        // A little above the target (but straight over it from the top).
        const raise = (axis) => (view !== 'top' && axis === 2 ? 0.2 : 0);
        const eye = world.map((part, axis) => part + (way[axis] / length) * distance + raise(axis));
        const { Vector3 } = this.api;
        const at = new Vector3(eye[0], eye[1], eye[2]);
        const look = new Vector3(world[0] - eye[0], world[1] - eye[1], world[2] - eye[2]);
        this.api.NoClip.setPose(at, look);
        const shown = (values) => values.map((part) => Math.round(part * 100) / 100);
        return { target, view, camera: shown(eye), lookAt: shown(world) };
      },
      // `wait bake|lash`: until a bake or a Lash to keys is over (at most 30 s): what it made.
      wait: async () => {
        const busy = () => {
          if (rest[0] === 'lash') {
            return this.lashBaking !== null;
          }
          return this.bakeParts !== null || this.bakeWaiting !== null;
        };
        for (let wait = 0; wait < 600 && busy(); wait += 1) {
          await new Promise((resolve) => setTimeout(resolve, 50));
        }
        if (busy()) {
          return 'still going: wait again';
        }
        return rest[0] === 'lash' ? { lastLash: this.lastLash } : { lastBake: this.lastBake };
      },
      // `lashkeys`: the whip's lash recorded along the animation, made its posed prop keys.
      lashkeys: () => {
        this.lashToKeys();
        return { recording: this.lashBaking !== null, message: this.message };
      },
      // `lashmotion [json]`: the lash made from the hand's motion (lashFromMotion; the json its
      // options); `wait lash` then says what it made.
      lashmotion: () => {
        const given = text();
        const started = this.lashFromMotion(given ? JSON.parse(given) : {});
        return { recording: started, message: this.message };
      },
      // `lashcheck`: how the lash plays (lash_motion.cjs lashQuality), null when nothing shapes it.
      lashcheck: () => lashQuality(this.animationByName(), this.rope?.length ?? 0),
      // `lashlive`: the lash as the game draws it, recorded for a round of the playing animation
      // (up to 10 s) and measured in the world (lash_motion.cjs lashSeen); waits for it.
      lashlive: async () => {
        if (this.rope?.kind !== 'whip' || this.recordingKeys || this.lashBaking) {
          return 'a whip in the hand, and nothing else recording';
        }
        const seconds = Math.min(10, Math.max(1, this.animation.length() + 0.2));
        this.lastLashLive = null;
        this.lashWatching = true;
        this.recordingKeys = true;
        if (!this.animation.playing && this.animation.keys.length > 1) {
          this.setPlaying(true);
        }
        this.api.send({ op: 'studioRopeRecord', seconds });
        for (let wait = 0; wait < 300 && this.lashWatching; wait += 1) {
          await new Promise((resolve) => setTimeout(resolve, 50));
        }
        this.recordingKeys = false;
        return this.lastLashLive ?? 'the recording did not come';
      },
      // `lashpath`: the hand's path the last Lash from motion was made on ({ rate, frames: [the
      // handle's tip x, y, z], handles: [its along and up] }, to the millimetre), or null.
      // `lashpose <json>`: the lash posed by hand. { time } moves the playhead first; then one of
      // { add: true } (a key of the lash as it shows), { shape: { aimSide, aimUp, curl, sway, sag,
      // tipTwist } } (the key there changed by that much, made first), { ways: [[yaw, pitch],
      // ...] } (the key there exactly so, body frame), { remove: true }, { fromLash: seconds }
      // (keys of the lash shown, that far apart), { clear: true }; answers the keys.
      lashpose: () => {
        const given = JSON.parse(text() || '{}');
        if (Number.isFinite(given.time)) {
          this.stopPlaying();
          this.setTime(Math.max(0, Math.min(this.animation.length(), given.time)));
        }
        if (given.add) {
          this.addLashKey();
        } else if (given.shape) {
          this.shapeLashKey(given.shape);
        } else if (Array.isArray(given.ways)) {
          const index = this.addLashKey();
          if (index >= 0) {
            this.changeAnimation('Lash key set', () => {
              const key = { time: this.animation.lashKeys.keys[index].time, ways: given.ways };
              const checked = lashKeysOf({ length: this.animation.lashKeys.length, keys: [key] });
              if (!checked) {
                return false;
              }
              this.animation.lashKeys.keys[index] = checked.keys[0];
              return true;
            });
          }
        } else if (given.remove) {
          this.removeLashKey();
        } else if (Number.isFinite(given.fromLash)) {
          this.lashKeysFromLash(Math.max(0.03, given.fromLash));
        } else if (given.clear) {
          this.clearLashKeys();
        }
        this.refresh();
        const keys = this.animation.lashKeys?.keys ?? [];
        return {
          count: keys.length,
          here: this.lashKeyHere(),
          keys: keys.map((key) => ({ time: key.time, first: key.ways[0], last: key.ways.at(-1) })),
          message: this.message,
        };
      },
      // `ui hide|show`: the editor's panels, gizmo, grid and bones hidden (only the scene shows,
      // to watch or record it) or shown again as they were.
      ui: () => this.setUiHidden(rest[0] === 'hide'),
      lashpath: () => {
        const path = this.lastHandlePath;
        if (!path) {
          return null;
        }
        const mm = (values) => values.map((part) => Math.round(part * 1000) / 1000);
        return {
          rate: path.rate,
          frames: path.frames.map((frame) => mm(frame.slice(0, 3))),
          handles: path.handles.map(mm),
        };
      },
      gameclip: () => {
        if (rest[0] === 'state') {
          return {
            path: this.gameClip,
            ...this.gameClipPart,
            state: this.gameClipState,
            lastBake: this.lastBake,
            lastLash: this.lastLash,
          };
        }
        // `gameclip bake`: the part playing made the studio's animation (Bake to keys). The
        // whip's lash along it as posed keys: `lashkeys`.
        if (rest[0] === 'bake') {
          this.bakeGameClip();
          return { baking: this.bakeParts !== null, message: this.message };
        }
        const text = rest.slice(1).join(' ').trim();
        const options = text.startsWith('{') ? JSON.parse(text) : { loop: text !== 'once' };
        if (rest[0] === 'part') {
          this.setGameClipPart(JSON.parse(text || '{}'));
        } else {
          this.playGameClip(rest[0] ?? '', options.loop !== false, options);
        }
        return { path: this.gameClip, ...this.gameClipPart };
      },
      // `gamefiles objects|clips <page> [words]`: a page of 50 of the game's objects or clips
      // whose paths have all the words (studio/game_files.hpp). Waits for the game client.
      gamefiles: async () => {
        const kind = rest[0] === 'clips' ? 'clips' : 'objects';
        const page = Math.max(0, Math.floor(number(1)) || 0);
        const query = rest.slice(2).join(' ').slice(0, SEARCH_LENGTH);
        this.gameFilesAnswer = null;
        this.api.send({ op: 'studioGameFiles', kind, query, page, size: ASSISTANT_PAGE });
        const answered = () =>
          this.gameFilesAnswer?.ready === true &&
          this.gameFilesAnswer.kind === kind &&
          this.gameFilesAnswer.query === query;
        // The game's packs are read the first time: a few seconds.
        for (let wait = 0; wait < 170 && !answered(); wait += 1) {
          await new Promise((resolve) => setTimeout(resolve, 50));
        }
        if (!answered()) {
          return 'the game client is still reading the game’s packs: ask again';
        }
        const { pages, total, paths } = this.gameFilesAnswer;
        return { kind, query, page: this.gameFilesAnswer.page, pages, total, paths };
      },
      // `search <words>` searches the game's folder shown (Game props, Game clips); `search`
      // alone shows all of it.
      search: () => {
        const kind = GAME_FOLDERS[this.folder];
        if (!kind) {
          return `not a game folder: ${this.folder}`;
        }
        this.askGameFiles(kind, rest.join(' ').slice(0, SEARCH_LENGTH), 0);
        return { kind, query: this.gameFiles[kind].query };
      },
      // `cloth on [kg]` makes your mesh cloth, `cloth off` solid again.
      cloth: () => {
        const mass = Number(rest[1]);
        const given = Number.isFinite(mass) && mass > 0 ? mass : this.meshCloth.mass;
        this.setCloth(rest[0] === 'on', given);
        this.refresh();
        return { mesh: this.mesh, cloth: this.meshCloth, message: this.message };
      },
      // `meshkey on|off`: the key at the playhead places your mesh where it stands now, or no
      // longer; `meshkey` alone answers the mesh keys.
      meshkey: () => {
        if (rest[0] === 'on' || rest[0] === 'off') {
          this.setMeshKey(rest[0] === 'on');
          this.refresh();
        }
        return {
          mesh: this.mesh,
          keys: this.animation.keys.flatMap((one, index) =>
            one.mesh ? [{ key: index, time: one.time, ...one.mesh }] : [],
          ),
          message: this.message,
        };
      },
      // `body none|solid|falls [kg]`: your mesh's body; `body` alone answers it.
      body: () => {
        if (rest[0]) {
          if (!MESH_BODIES.some((one) => one.kind === rest[0])) {
            return `body is one of ${MESH_BODIES.map((one) => one.kind).join(', ')}`;
          }
          const mass = Number(rest[1]);
          this.setBody(rest[0], Number.isFinite(mass) && mass > 0 ? mass : this.meshBody.mass);
          this.refresh();
        }
        return {
          mesh: this.mesh,
          body: this.meshBody,
          cloth: this.meshCloth,
          message: this.message,
        };
      },
      // `meshes`: asks the game client for your meshes; `mesh <name>` shows one beside the
      // character (`mesh` alone takes it away).
      meshes: () => {
        this.api.send({ op: 'studioMeshList' });
        return this.meshes;
      },
      // `mesh <name>` shows a mesh of yours (the one shown: loads its file again),
      // `mesh Objects/....cgf` one of the game's objects, `mesh` alone takes it away.
      mesh: () => {
        this.showMesh(rest.join(' '));
        return this.mesh;
      },
      // `skin <name>` puts a skinned mesh of yours (<name>.skin in the meshes folder, made by
      // Dev/tools/assets/blender/kcds_skin.py) on the character, moving with its bones; `skin`
      // alone takes it off.
      skin: () => {
        const name = rest.join(' ');
        if (name && !/^[A-Za-z0-9_-]{1,64}$/.test(name)) {
          return 'a skin name is letters, digits, _ and - (at most 64)';
        }
        this.skin = name;
        this.api.send({ op: 'studioSkin', name });
        return { skin: this.skin };
      },
      // `sky <json>`: any of { hour, preset, rain }; `sky` alone: the sky now.
      sky: () => {
        const text = line.trim().slice(word.length).trim();
        if (text) {
          this.setSky({ ...this.sky, ...JSON.parse(text) });
        }
        return { ...this.sky, presets: WEATHERS.map((one) => one.preset) };
      },
      // `clothes <json>`: { outfit: key, off: [piece names] }; `clothes` alone: what it wears.
      clothes: () => {
        const text = line.trim().slice(word.length).trim();
        if (text) {
          const given = JSON.parse(text);
          this.setClothing(given, `Wears: ${outfitOf(given.outfit).label}`);
        }
        const outfit = outfitOf(this.clothing.outfit);
        return {
          ...this.clothing,
          outfits: OUTFITS.map((one) => ({
            key: one.key,
            label: one.label,
            pieces: one.pieces.map((piece) => piece.name),
          })),
          wearing: outfit.pieces
            .map((piece) => piece.name)
            .filter((name) => !this.clothing.off.includes(name)),
        };
      },
      // `physics <json>`: how the prop moves, any of { gravity, damping, mass, air, bend, wind }.
      physics: () => {
        if (!this.rope) {
          return 'make a rope or a whip first';
        }
        const given = JSON.parse(line.trim().slice(word.length).trim() || '{}');
        const next = { ...this.rope, physics: physicsOf({ ...this.rope.physics, ...given }) };
        if (this.rope.grip) {
          next.grip = settingsOf(GRIP_FIELDS, { ...this.rope.grip, ...given });
        }
        this.commit(next, 'Physics set');
        return { ...this.rope.physics, ...(this.rope.grip ?? {}) };
      },
      // `probe <json>` (test builds): a message to the game client as it is.
      probe: () => {
        this.api.send(JSON.parse(line.trim().slice(word.length).trim()));
        return 'sent';
      },
      // `find <text>`: the joints whose names hold the text (at most 40).
      find: () => {
        const wanted = rest.join(' ').toLowerCase();
        return this.joints
          .filter((joint) => joint.name.toLowerCase().includes(wanted))
          .slice(0, 40)
          .map((joint) => ({
            id: joint.id,
            name: joint.name,
            parent: this.joints[joint.parent]?.name ?? null,
          }));
      },
      // `animation <json>`: the whole animation at once (setAnimation); `animation` alone: the
      // animation now, its joints by name.
      animation: async () => {
        const text = line.trim().slice(word.length).trim();
        // The joints come from the game client a frame after Bones is shown.
        for (let wait = 0; wait < 40 && this.joints.length === 0; wait += 1) {
          await new Promise((resolve) => setTimeout(resolve, 50));
        }
        return text ? this.setAnimation(text) : this.animationByName();
      },
      tl: () => {
        this.pressTimeline(number(0), number(1));
        return { time: this.time, key: this.keyHere(), joint: this.joint, drag: this.drag };
      },
      tlrelease: () => {
        this.releaseTimeline(number(0), number(1));
        return this.animation.keys.map((one) => one.time);
      },
      turn: () => {
        const axis = ['x', 'y', 'z'].indexOf(rest[0]);
        if (axis < 0 || this.joint < 0) {
          return 'choose a joint, then turn x, y or z';
        }
        this.turnJoint(axis, Math.sign(number(1)) || 1);
        return { joint: this.joint, turn: this.animation.turnOf(this.joint) };
      },
      pose: () => ({ key: this.animation.key, ...this.animation.current() }),
      key: () => {
        if (rest[0] === 'add') {
          this.addKey();
        } else if (rest[0] === 'remove') {
          this.removeKey();
        } else {
          this.chooseKey(number(0) - 1);
        }
        return this.animation.toJSON();
      },
      keytime: () => {
        this.moveKey(Math.sign(number(0)) || 1);
        return this.animation.current();
      },
      play: () => {
        this.setPlaying(rest[0] !== 'off');
        return { playing: this.animation.playing, length: this.animation.length() };
      },
      // `pick <x> <y>`: a press in the view (stage places); the pointer stays there for a drag:
      // `dragto <x> <y>` moves it, `release` lets go.
      pick: () => {
        this.testPointer = { x: number(0), y: number(1) };
        this.pressView(number(0), number(1));
        return 'asked';
      },
      dragto: () => {
        this.testPointer = { x: number(0), y: number(1) };
        if (this.propDrag) {
          this.followPropDrag();
        } else if (this.ringDrag) {
          this.followRing();
        }
        return this.propDrag?.now ?? this.message;
      },
      release: () => {
        const x = this.testPointer?.x ?? 0;
        this.releaseView(x);
        this.testPointer = null;
        return this.message;
      },
      campos: () =>
        this.api.NoClip.setPosition(new this.api.Vector3(number(0), number(1), number(2))),
      lookat: () => this.api.NoClip.lookAt(new this.api.Vector3(number(0), number(1), number(2))),
      // `view x y z tx ty tz`: the camera's place and aim in one pose.
      view: () => {
        const { Vector3 } = this.api;
        const at = new Vector3(number(0), number(1), number(2));
        const forward = new Vector3(number(3) - at.x, number(4) - at.y, number(5) - at.z);
        return this.api.NoClip.setPose(at, forward);
      },
      camera: () => this.api.NoClip.getPosition(),
      focus: () => {
        this.focusCharacter();
        return true;
      },
    };
    // Only the steps listed above: a word such as "constructor" or "toString" names no step.
    const step = Object.hasOwn(steps, word) ? steps[word] : null;
    if (!step) {
      return `unknown ${word}`;
    }
    // What it changes is the assistant's in History (note).
    this.byAssistant = true;
    let result;
    try {
      result = await step();
    } finally {
      this.byAssistant = false;
    }
    // A step changes what the screen shows, as a press does.
    this.refresh();
    return `${line} -> ${JSON.stringify(result ?? null)} ${JSON.stringify(this.state())}`;
  }

  // What the assistant sees of the studio (studio_state): the mode, tab and folder, the prop
  // with its hold, grip and physics, the mesh, the animation (keys, time, playing, prop keys),
  // the game clip playing (its part and where it is) and the message the status bar shows.
  state() {
    return {
      open: this.isOpen(),
      mode: this.mode,
      tab: TABS[this.tab],
      folder: this.folder,
      selected: this.selected,
      // Its recorded keys as how many there are (keyframes gives them, a part at a time).
      rope: this.rope?.keys
        ? {
            ...this.rope,
            keys: {
              rate: this.rope.keys.rate,
              frames: this.rope.keys.frames.length,
              points: this.rope.keys.frames[0].length / 3,
            },
          }
        : this.rope,
      mesh: this.mesh ? { name: this.mesh, ...this.meshPlace } : null,
      joint: this.joints[this.joint] ?? null,
      animation: {
        key: this.animation.key,
        keys: this.animation.keys.length,
        length: this.animation.length(),
        time: this.time,
        playing: this.animation.playing,
        loop: this.animation.loop,
        propKeys: this.animation.keys.filter((one) => one.prop).length,
      },
      gameClip: this.gameClip
        ? { path: this.gameClip, ...this.gameClipPart, now: this.gameClipState }
        : null,
      message: this.message,
    };
  }
}

module.exports = { StudioEditor, MODES, ITEMS };
