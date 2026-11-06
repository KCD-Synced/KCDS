'use strict';

// World Architect's editor: the in-game world editor, to the owner's final
// design canvas (the "pro redesign"). The cursor drives it, as in any map editor: the panels and
// the world view take its presses, and holding the right mouse button looks around with the
// mouse and flies with W A S D; letting go gives the cursor back.
//
//   Modes (1-5)    Place: the game's houses, structures and doors (Prefab), the ground fitted
//                  under a house first (Terrain). Terrain: raise, lower or flatten brushes.
//                  Foliage: the level's trees and bushes (Vegetation). Collision: the author's
//                  own invisible walls, floors, ramps and boxes (collision_layer.cjs). Zones:
//                  shown, next version.
//   Tools (QWER)   Select picks one of yours under the press; Move puts the selection where you
//                  press; Rotate turns it by the angle snap; Scale grows a collision shape.
//   Content        a tile arms placing: each press in the world puts one down until Escape.
//   Details        the selection's place, turn and sizes, stepped with - and +, and Delete.
//
// Every change is one undo step (Undo and Redo in the toolbar, Ctrl+Z and Ctrl+Y). Shell
// (shell.cjs) draws the screen.

const { Catalogue, shortName } = require('./catalogue.cjs');
const {
  CollisionLayer,
  KINDS: SHAPE_KINDS,
  KIND_KEYS: SHAPE_KEYS,
  ZONE_KEYS,
} = require('./collision_layer.cjs');
const { Shell, TILE_COUNT, outlinerRows, FOLDER_ROWS } = require('./shell.cjs');
const { GLYPHS } = require('./glyphs.cjs');
const { Marks, ringPoint } = require('./marks.cjs');
const { AiCommands } = require('./ai_commands.cjs');
const { aiConnectView } = require('./ai_connect.cjs');
const { areaOf, groundObj, round, GRID_MOST, IMPORT_SCRIPT } = require('./blender_export.cjs');
const { serverScript, clientScript, readme } = require('./server_export.cjs');
const { STAGE, LAYOUT, stageFor, fitStage } = require('./theme.cjs');
const {
  MOVER_DEFAULTS,
  HINGES,
  LIMITS: MOVER_LIMITS,
  checkedMover,
  moverPose,
  moverPath,
  moverUse,
} = require('./movers.cjs');

const MODES = Object.freeze([
  {
    key: 'place',
    label: 'Place',
    icon: 'house',
    folders: ['houses', 'structures', 'doors', 'people'],
  },
  { key: 'terrain', label: 'Terrain', icon: 'terrain', folders: ['brushes'] },
  { key: 'foliage', label: 'Foliage', icon: 'tree', folders: ['trees', 'bushes'] },
  { key: 'collision', label: 'Collision', icon: 'shield', folders: ['shapes'] },
  {
    key: 'props',
    label: 'Props',
    icon: 'box',
    folders: ['furniture', 'decorations', 'fixtures', 'goods', 'work', 'rocks', 'parts', 'yours'],
  },
  {
    key: 'effects',
    label: 'Effects',
    icon: 'play',
    folders: [
      'lights',
      'sounds',
      'fire',
      'smoke',
      'weather',
      'effects_work',
      'combat',
      'effects_other',
    ],
  },
  { key: 'zones', label: 'Zones', icon: 'map', folders: ['zones'] },
]);
const TOOLS = Object.freeze([
  { key: 'select', label: 'Select', shortcut: 'Q' },
  { key: 'move', label: 'Move', shortcut: 'W' },
  { key: 'rotate', label: 'Rotate', shortcut: 'E' },
  { key: 'scale', label: 'Scale', shortcut: 'R' },
]);
const ACTIONS = Object.freeze(['Undo', 'Redo', 'Exit']);
// The outliner's filter, pressed in turn: everything, then one group.
const FILTERS = Object.freeze([
  { key: 'all', label: 'All' },
  { key: 'prefab', label: 'Buildings' },
  { key: 'vegetation', label: 'Foliage' },
  { key: 'prop', label: 'Props' },
  { key: 'effect', label: 'Effects' },
  { key: 'light', label: 'Lights' },
  { key: 'shape', label: 'Collision' },
  { key: 'level', label: 'Hidden' },
]);
// The free camera's speeds, metres a second, pressed in turn on the view's chip.
const CAMERA_SPEEDS = Object.freeze([4, 8, 12, 24, 48]);
// Plants taken away in one question to the game when the editor leaves a map.
const LEAVE_PLANTS_AT_ONCE = 512;
// The Fly speed row's steps, metres a second.
const FLY_SPEEDS = Object.freeze([1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 100, 150, 200]);
// What a search box takes while it has the keys: letters, digits, '_' (Shift and minus), '-' and
// space; Backspace takes one back, Enter ends.
const LETTERS = Object.freeze('abcdefghijklmnopqrstuvwxyz0123456789'.split(''));
const SEARCH_KEYS = Object.freeze([...LETTERS, 'minus', 'space', 'backspace', 'enter']);
const SEARCH_LENGTH = 40;
const TABS = Object.freeze(['Content', 'History', 'Validation', 'Saves', 'AI']);
// A save's name: what the game client lets reach the file system (architect/saves.hpp).
const SAVE_NAME = /^[A-Za-z0-9_-][A-Za-z0-9 _-]{0,46}[A-Za-z0-9_-]$|^[A-Za-z0-9_-]$/;
const SAVES_FOLDER = 'Documents\\KCDS\\World Architect';
// The ground materials the Paint brushes put on (Terrain.paint's names, Client/resources/
// terrain.cjs): every sector of a custom map carries them all; a game level's ground takes the ones
// it already has there.
const PAINTS = Object.freeze([
  ['grass', 'Grass'],
  ['meadow', 'Meadow'],
  ['pasture', 'Pasture'],
  ['trampled', 'Trampled earth'],
  ['soil', 'Soil'],
  ['mud', 'Mud'],
  ['stony_mud', 'Stony mud'],
  ['sand', 'Sand'],
  ['gravel', 'Gravel'],
  ['forest', 'Forest floor'],
  ['forest_leaves', 'Leaves'],
  ['cobblestones', 'Cobblestones'],
  ['paving', 'Paving'],
  ['cobblestones_grass', 'Cobbles and grass'],
  ['river_shore', 'River shore'],
]);
const PAINT_PREFIX = 'paint:';
const BRUSHES = Object.freeze([
  { key: 'raise', label: 'Raise', meta: 'Brush: lifts the ground' },
  { key: 'lower', label: 'Lower', meta: 'Brush: digs the ground' },
  { key: 'flatten', label: 'Flatten', meta: 'Brush: levels the ground' },
  { key: 'clear', label: 'Clear nature', meta: "Brush: hides the level's plants" },
  { key: 'nature', label: 'Add nature', meta: 'Brush: scatters the chosen plant' },
  ...PAINTS.map(([material, label]) => ({
    key: PAINT_PREFIX + material,
    label: `Paint ${label.toLowerCase()}`,
    meta: 'Brush: paints the ground',
    glyph: 'flatten',
  })),
]);

// The material of a Paint brush's key ('paint:grass'), null for the other brushes.
function paintOf(mode) {
  return typeof mode === 'string' && mode.startsWith(PAINT_PREFIX)
    ? mode.slice(PAINT_PREFIX.length)
    : null;
}
const FOLDER_LABELS = Object.freeze({
  brushes: 'Brushes',
  shapes: 'Shapes',
  lights: 'Lights',
  sounds: 'Sounds',
  people: 'People',
  zones: 'Areas',
});
const GLYPH_OF_FOLDER = Object.freeze({
  houses: 'house',
  structures: 'structure',
  doors: 'door',
  trees: 'tree',
  bushes: 'bush',
  furniture: 'box',
  decorations: 'box',
  fixtures: 'box',
  goods: 'box',
  work: 'box',
  rocks: 'box',
  parts: 'structure',
  yours: 'box',
  fire: 'nature',
  smoke: 'nature',
  weather: 'nature',
  effects_work: 'nature',
  combat: 'nature',
  lights: 'nature',
  sounds: 'nature',
  people: 'box',
  effects_other: 'nature',
});
const GRID_STEPS = Object.freeze([0, 0.5, 1, 2]);
const ANGLE_STEPS = Object.freeze([5, 15, 45, 90]);
// How far the pointer's line reaches into the world, metres.
const REACH = 800;
// A thing of yours picked by Select: the nearest within this many metres of the press, or of
// OWN_REACH when the ray met the level's own object (which is picked otherwise).
const PICK_RANGE = 12;
const OWN_REACH = 4;
// Your things the ray cannot meet (no solid mesh): found by their nearness to the press.
const UNMET_SOURCES = new Set(['light', 'effect', 'sound']);
// A prop of yours whose box the ray enters at most this many metres past what it met is the one
// pressed (the met point is on the prop's own surface or just behind it).
const CROSS_PAST = 0.1;
// A prop whose box is longer than this corner to corner (metres) is not taken by its box.
const CROSS_LARGEST = 8;
// A door's preview moves a frame this often, milliseconds.
const MOVER_PREVIEW_MS = 33;
// Where your own assets are in the game's files (architect/user_assets.hpp): objects/kcds/...
const USER_ASSET_PREFIX = 'objects/kcds/';
// How near a hit brush stands to your plant or house piece to be that one, metres.
const OWN_PLACE = 0.5;
// How far the cursor moves before the hover name follows it, stage pixels.
const HOVER_MOVE = 24;
// The tools that change the selection; with nothing selected the editor is back on Select.
const TRANSFORM_TOOLS = new Set(['move', 'rotate', 'scale']);

// A mesh path in one form: lower case, '/' between folders.
function meshKey(path) {
  return String(path).toLowerCase().split('\\').join('/');
}
// Fit ground: the pad under a placed prefab and its fade, metres.
const PAD_LEAST = 3;
const PAD_MOST = 20;
const PAD_FADE = 3;
// The content's pictures (studio shots): the item stands this high over the camera, in the sky,
// and is filmed from this far and at this height above its foot, by folder (metres).
const STUDIO_HEIGHT = 350;
const STUDIO_FRAMING = Object.freeze({
  houses: { distance: 17, height: 3.5 },
  structures: { distance: 15, height: 2.5 },
  doors: { distance: 4, height: 1.1 },
  trees: { distance: 18, height: 6 },
  bushes: { distance: 4.5, height: 0.8 },
  // Props are framed by their own box once they stand (fit), spaced for the largest.
  furniture: { distance: 3, height: 0.6, fit: true, spacing: 24 },
  decorations: { distance: 2, height: 0.4, fit: true, spacing: 24 },
  fixtures: { distance: 2.5, height: 0.6, fit: true, spacing: 24 },
  goods: { distance: 2.5, height: 0.5, fit: true, spacing: 24 },
  work: { distance: 3, height: 0.6, fit: true, spacing: 24 },
  rocks: { distance: 5, height: 1, fit: true, spacing: 30 },
  parts: { distance: 8, height: 2, fit: true, spacing: 40 },
  yours: { distance: 4, height: 1, fit: true, spacing: 30 },
});
// The time a placed item takes to stand before its picture is taken, milliseconds.
const STUDIO_SETTLE_MS = 1200;
const PICTURE_WIDTH = 310;
// The pictures' keys: the version of the framing, then the entry's name (a new framing takes
// them again).
const PICTURE_PREFIX = 'item2:';
// What a move or a turn poses through its handle (a building, a prop, an effect, a light, a
// sound).
const POSED_KINDS = new Set(['prefab', 'prop', 'effect', 'light', 'sound', 'npc']);
// The NPC preview (People): the game's own NPC bodies, a man and a woman, placed in this game
// only and sent walking on the navmesh.
// Not kept in the project. Their ids are the local
// NPCs' (Client/runtime/npcs/local_npcs.hpp), from NPC_IDS up, apart from the resources' own.
const PEOPLE = Object.freeze([
  { key: 'npc_man', label: 'Man', actorClass: 'NPC' },
  { key: 'npc_woman', label: 'Woman', actorClass: 'NPC_Female' },
]);
const NPC_IDS = 1000000;
// How far below a pressed point the terrain is looked for, metres.
const NPC_GROUND_REACH = 60;
// How often a walking NPC's state is asked, milliseconds.
const NPC_WALK_POLL_MS = 1000;
// A building's box: the room around each of its pieces and its least height, metres.
const PIECE_ROOM = 3;
const BUILDING_HEIGHT = 6;
// The maps World Architect works on (the debug world of each, architect/local_mode.hpp).
const MAPS = Object.freeze([
  { key: 'trosecko', label: 'Trosecko' },
  { key: 'kutnohorsko', label: 'Kutnohorsko' },
  { key: 'klaster', label: 'Klaster' },
]);

function mapLabel(key) {
  const custom = /^kcds_map_(.+)$/.exec(key ?? '');
  if (custom) {
    return `Custom: ${custom[1]}`;
  }
  return MAPS.find((map) => map.key === key)?.label ?? key;
}

// The clock's speeds (game seconds per real second; 0 holds the clock), the game's own weather
// presets (its weather test quest's names) and the wind (metres per second).
const CLOCK_SPEEDS = Object.freeze([
  { label: 'Held', speed: 0 },
  { label: '×15', speed: 15 },
  { label: '×60', speed: 60 },
  { label: '×600', speed: 600 },
]);
const WEATHERS = Object.freeze([
  { label: 'Clear', preset: 'cloudless_sunny' },
  { label: 'Few clouds', preset: 'semicloudy_clear' },
  { label: 'Overcast', preset: 'summer_overcast' },
  { label: 'Cloudy', preset: 'cloudy_no_rain' },
  { label: 'Showers', preset: 'cloudy_frequent_showers' },
  { label: 'Fog and drizzle', preset: 'foggy_drizzly' },
  { label: 'Fog', preset: 'foggy_storm_no_rain' },
  { label: 'Storm', preset: 'foggy_storm' },
]);
// What of yours goes up and down with the ground the Terrain brushes change, and the least change
// that moves it (metres).
const GROUND_FOLLOWERS = Object.freeze(['vegetation', 'prop', 'effect', 'light']);
const GROUND_FOLLOW_LEAST = 0.01;
// The pause before each content picture, and while the author is busy (milliseconds).
const PICTURE_GAP_MS = 150;
// A brush dab slower than this is logged with where its time went.
const SLOW_DAB_MS = 100;
const TYPING_PAUSE_MS = 600;
// How often an effect may play again (seconds; 0 runs on as the game made it).
const EFFECT_PULSES = Object.freeze([0, 1, 2, 5, 10]);
// Effects that play once and are gone (a blood splash, a flash, a shot): they play again every
// 2 s in the editor, so you see them where they stand.
// The game's blood is a close-up wound, 15 cm, so it starts four times as big.
const ONE_SHOT_EFFECT =
  /blood|combat|explosion|lightning|spark|hit|impact|splash|stab|slash|shot|cannon|arrow/i;
const SMALL_EFFECT = /blood/i;

function effectDefaults(name) {
  return {
    scale: SMALL_EFFECT.test(name) ? 4 : 1,
    pulse: ONE_SHOT_EFFECT.test(name) && !/leaking/i.test(name) ? 2 : 0,
  };
}

// An effect's settings kept in a project, each checked; the defaults for what is missing.
function effectSettings(kept, name) {
  const defaults = effectDefaults(name);
  const scale =
    Number.isFinite(kept.scale) && kept.scale >= 0.25 && kept.scale <= 32
      ? kept.scale
      : defaults.scale;
  const pulse = EFFECT_PULSES.includes(kept.pulse) ? kept.pulse : defaults.pulse;
  return { scale, pulse };
}

// The Time of day row's step, in hours.
const HOUR_STEP = 0.5;

// An hour of the day (12.5) as the clock shows it (12:30).
function clockText(hour) {
  const minutes = Math.round(hour * 60) % (24 * 60);
  const hours = String(Math.floor(minutes / 60)).padStart(2, '0');
  return `${hours}:${String(minutes % 60).padStart(2, '0')}`;
}

const WINDS = Object.freeze([
  { label: 'Calm', speed: 0 },
  { label: 'Breeze', speed: 3 },
  { label: 'Windy', speed: 8 },
  { label: 'Gale', speed: 16 },
]);
// The paint brush: the most plants a press, and how far up and down a spot looks for ground.
const PAINT_MOST = 20;
const PAINT_REACH = 40;
// The project file's version, and how long after a change it is saved, milliseconds.
const PROJECT_VERSION = 1;
const SAVE_DELAY_MS = 1500;
// The most characters of the project in one save message (a message holds 64 KiB; a character
// takes up to 3 bytes).
const SAVE_PIECE = 20000;
// How often the right mouse button and the shortcut keys are looked at, milliseconds.
const POLL_MS = 16;
// How often the polls' times are reported to the log, milliseconds.
const POLL_REPORT_MS = 10000;
// A held terrain stroke: a dab at most every STROKE_MS, once the cursor moved this share of the
// brush's size.
const STROKE_MS = 120;
const STROKE_SPACING = 0.35;
// How near a press must be to the game client's cursor to take its hit, stage pixels.
const PRESS_NEAR = 6;
// How near a press must be to a move arrow on the stage to grab it, stage pixels.
const ARROW_REACH = 18;
// The brush ring's points, and how far above the ground it is drawn (metres).
const RING_POINTS = 64;
const RING_COLOR = 0xf0a93b;
const RING_LIFT = 0.1;
// The plants one Add nature dab scatters at first.
const NATURE_COUNT = 6;
// Add nature plants the ground's own grass this densely (clumps a square metre), at most this many
// a dab; the ground heights are asked this many points at a time (architect/level_plants.cpp).
const GRASS_DENSITY = 1.2;
const GRASS_MOST = 160;
// The most plants the game client plants (vegetation/local_vegetation.cpp kMaxPlanted).
const PLANTS_MOST = 65536;
const HEIGHTS_AT_ONCE = 256;
// The most points whose ground is looked for one ray each when the batch question fails.
const RAY_FALLBACK_MOST = 16;
// Export for Blender asks its ground grid in larger parts (the game takes up to 1024 a question;
// 512 keep a question well inside a message's 64 KiB).
const EXPORT_HEIGHTS_AT_ONCE = 512;
// The most characters of an exported file in one message to the game (its limit is 64 KiB).
const EXPORT_PIECE = 40000;
// A World Architect custom map's name (Client/runtime/maps/custom_maps.hpp).
const CUSTOM_MAP = /^kcds_map_[a-z0-9_]{1,32}$/;
// An exported spawn's height above the ground, and how far down from the camera the ground is
// looked for, metres.
const SPAWN_LIFT = 0.5;
const SPAWN_REACH = 600;
// The light colours a light's Colour row steps through (0..1 each).
const LIGHT_COLORS = Object.freeze([
  { label: 'Warm', color: { x: 1, y: 0.72, z: 0.42 } },
  { label: 'Fire', color: { x: 1, y: 0.55, z: 0.2 } },
  { label: 'Candle', color: { x: 1, y: 0.8, z: 0.55 } },
  { label: 'White', color: { x: 1, y: 1, z: 1 } },
  { label: 'Moon', color: { x: 0.6, y: 0.7, z: 1 } },
  { label: 'Red', color: { x: 1, y: 0.15, z: 0.1 } },
  { label: 'Green', color: { x: 0.2, y: 1, z: 0.3 } },
  { label: 'Blue', color: { x: 0.2, y: 0.4, z: 1 } },
]);
// The lights in Effects: a colour, intensity and radius to start from.
const LIGHT_PRESETS = Object.freeze([
  { key: 'candle', label: 'Candle', colour: 'Candle', intensity: 1.5, radius: 3 },
  { key: 'torch', label: 'Torch', colour: 'Fire', intensity: 4, radius: 7 },
  { key: 'fireplace', label: 'Fireplace', colour: 'Fire', intensity: 6, radius: 9 },
  { key: 'lantern', label: 'Lantern', colour: 'Warm', intensity: 3, radius: 6 },
  { key: 'lamp', label: 'Lamp', colour: 'White', intensity: 6, radius: 12 },
  { key: 'moonlight', label: 'Moonlight', colour: 'Moon', intensity: 2, radius: 20 },
  { key: 'red', label: 'Red light', colour: 'Red', intensity: 4, radius: 8 },
  { key: 'green', label: 'Green light', colour: 'Green', intensity: 4, radius: 8 },
  { key: 'blue', label: 'Blue light', colour: 'Blue', intensity: 4, radius: 8 },
]);

// A world sound's content entry, and its name to read (a_l_village_amb01: village amb01).
// A preview NPC's walk to read.
function walkLabel(state) {
  const labels = {
    walking: 'Walking',
    reached: 'Arrived',
    'no path': 'No path (no navmesh)',
    refused: 'Cannot walk',
    stopped: 'Stopped',
  };
  return labels[state] ?? 'Standing';
}

function soundEntry(name) {
  return { label: soundLabel(name), name, source: 'sound' };
}

function soundLabel(name) {
  return name
    .replace(/^(a_l_|amb_)/, '')
    .split('_')
    .join(' ');
}

function lightOfPreset(key) {
  const preset = LIGHT_PRESETS.find((entry) => entry.key === key) ?? LIGHT_PRESETS[0];
  const colour = LIGHT_COLORS.find((entry) => entry.label === preset.colour);
  return {
    color: { ...colour.color },
    intensity: preset.intensity,
    radius: preset.radius,
    castShadows: false,
  };
}
// How far the cursor goes sideways for one step of a value it drags (stage pixels).
const SCRUB_PX = 12;
// Two presses on one Outliner line this close together are a double press (ms).
const DOUBLE_PRESS_MS = 500;
// A size step of the Scale tool and the Scale row (×1.1 up, ÷1.1 down).
const SCALE_STEP = 1.1;
// The selection's and the brush's marks as world drawings: their ids, and the points a drawing
// holds at most (the Gizmos mesh's limit is 1024).
const MARK_GIZMO = 'architect-mark-';
const MARK_POINTS = 1000;
// How finely a mark's line is looked at, and its edges found, to keep it inside the world view.
const CLIP_SAMPLES = 16;
const CLIP_BISECTIONS = 10;
// The Scale tool held still: the first repeat after this, then one every SCALE_REPEAT_MS.
const SCALE_HOLD_DELAY_MS = 350;
const SCALE_REPEAT_MS = 120;
// How many points of a ring are looked at to find where the cursor is on it.
const RING_SAMPLES = 120;
// How far from a selected level object its gates are found (metres).
const GATE_REACH = 15;

// A gate's name for Details: the last part of its level path or key, in words.
function gateLabel(key) {
  const last = String(key).split('/').pop() || 'Gate';
  return last.replace(/[_-]+/g, ' ').replace(/([a-z])([A-Z])/g, '$1 $2');
}
// A hidden level plant takes the grass carpet's clutter this near it along (metres).
const CLUTTER_RADIUS = 0.6;
// How long a prop's box is not asked again while none of its mesh stands yet (ms).
const PROP_BOX_RETRY_MS = 500;
const PLANTS_AT_ONCE = 256;
// How often a dragged thing is shown at the arrow's place (ms): a prefab or shape, a plant.
const DRAG_SHOW_MS = 50;
const DRAG_PLANT_MS = 150;
// The most level plants one Clear nature dab hides.
const NATURE_MOST = 256;
// Where the free camera starts: this far back and up from the player, metres.
const OVERLOOK = Object.freeze({ back: 9, up: 7 });
// Play as player puts the player where the middle of the world view looks, when that is at most
// `reach` metres from the camera, `back` metres short of it toward the camera (not in the wall
// looked at), on what is under it; else on the ground under the camera, looked for `down` metres
// down (the player started where the world had put them).
const PLAY_START = Object.freeze({ reach: 80, back: 0.6, above: 2, down: 600 });
// The shortcut keys: tools, modes, Delete, Ctrl+Z and Ctrl+Y.
const SHORTCUTS = Object.freeze([
  'q',
  'w',
  'e',
  'r',
  '1',
  '2',
  '3',
  '4',
  '5',
  '6',
  '7',
  'delete',
  'z',
  'y',
  'f',
  'd',
]);

// The rotation `a` after `b` (quaternions { x, y, z, w }).
function multiplied(a, b) {
  return {
    x: a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
    y: a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
    z: a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    w: a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
  };
}

// A Details row that only names a section (as the shell draws it: a label and nothing else).
function isSectionRow(row) {
  return (
    row.value === undefined &&
    row.step === undefined &&
    row.press === undefined &&
    row.toggle === undefined &&
    row.axis === undefined
  );
}

// The turn about the up axis (degrees) of a rotation { x, y, z, w }.
function yawOf(rotation) {
  if (!rotation) {
    return 0;
  }
  const { x, y, z, w } = rotation;
  const yaw = Math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z));
  return Math.round(degreesOf((yaw * 180) / Math.PI) * 100) / 100;
}

function degreesOf(turn) {
  return ((turn % 360) + 360) % 360;
}

function snap(value, step) {
  return step > 0 ? Math.round(value / step) * step : value;
}

// A thing's own box from its upright world box ([minX, minY, minZ, maxX, maxY, maxZ]) when it is
// turned only about the up axis (`rotation` { x, y, z, w }): { at, turn, min, max } around `at` in
// its turn, or null (tilted, or turned so near 45° that the sides cannot be told apart). The world
// box of a turned box of half sides a and b is a|cos| + b|sin| by a|sin| + b|cos|.
function ownBoxOf(box, rotation, at) {
  if (!rotation || Math.abs(rotation.x ?? 0) > 0.02 || Math.abs(rotation.y ?? 0) > 0.02) {
    return null;
  }
  const yaw = 2 * Math.atan2(rotation.z ?? 0, rotation.w ?? 1);
  const cos = Math.abs(Math.cos(yaw));
  const sin = Math.abs(Math.sin(yaw));
  const apart = cos * cos - sin * sin;
  if (Math.abs(apart) < 0.25) {
    return null;
  }
  const [minX, minY, minZ, maxX, maxY, maxZ] = box;
  const halfX = (maxX - minX) / 2;
  const halfY = (maxY - minY) / 2;
  const a = (halfX * cos - halfY * sin) / apart;
  const b = (halfY * cos - halfX * sin) / apart;
  if (!(a > 0) || !(b > 0)) {
    return null;
  }
  // The box's middle in the thing's own frame.
  const dx = (minX + maxX) / 2 - at.x;
  const dy = (minY + maxY) / 2 - at.y;
  const back = Math.cos(-yaw);
  const side = Math.sin(-yaw);
  const middleX = dx * back - dy * side;
  const middleY = dx * side + dy * back;
  return {
    at,
    turn: (yaw * 180) / Math.PI,
    min: [middleX - a, middleY - b, minZ - at.z],
    max: [middleX + a, middleY + b, maxZ - at.z],
  };
}

// Where the line from + line * t (t from 0) enters a box { at, turn (degrees about up), min, max }
// (min and max from `at`, in the box's own turn): t, 0 when `from` is inside, null when it misses.
function boxEntry(from, line, box) {
  const angle = (-(box.turn ?? 0) * Math.PI) / 180;
  const cos = Math.cos(angle);
  const sin = Math.sin(angle);
  const dx = from.x - box.at.x;
  const dy = from.y - box.at.y;
  const origin = [dx * cos - dy * sin, dx * sin + dy * cos, from.z - box.at.z];
  const direction = [line.x * cos - line.y * sin, line.x * sin + line.y * cos, line.z];
  let near = 0;
  let far = Infinity;
  for (let axis = 0; axis < 3; axis += 1) {
    const low = box.min[axis];
    const high = box.max[axis];
    if (Math.abs(direction[axis]) < 1e-9) {
      if (origin[axis] < low || origin[axis] > high) {
        return null;
      }
      continue;
    }
    let t0 = (low - origin[axis]) / direction[axis];
    let t1 = (high - origin[axis]) / direction[axis];
    if (t0 > t1) {
      [t0, t1] = [t1, t0];
    }
    near = Math.max(near, t0);
    far = Math.min(far, t1);
    if (near > far) {
      return null;
    }
  }
  return near;
}

function metres(value) {
  return value.toFixed(2);
}

// Whether two ground boxes { minX, maxX, minY, maxY } overlap, and whether a point or a box lies
// inside another.
function overlap(a, b) {
  return a.minX < b.maxX && b.minX < a.maxX && a.minY < b.maxY && b.minY < a.maxY;
}

function contains(box, point) {
  return point.x > box.minX && point.x < box.maxX && point.y > box.minY && point.y < box.maxY;
}

function within(inner, outer) {
  return (
    inner.minX >= outer.minX &&
    inner.maxX <= outer.maxX &&
    inner.minY >= outer.minY &&
    inner.maxY <= outer.maxY
  );
}

function every(milliseconds, callback) {
  const id = setInterval(callback, milliseconds);
  return () => clearInterval(id);
}

class ArchitectEditor {
  // api: { NativeUI, NoClip, World, Prefab, Terrain, Vegetation, Collision, Gizmos, Key, Vector3,
  // send (to the game client: the local level edits),
  // prefabNames, log, gameUi?(shown), every? }. gameUi puts the game's own interface away while
  // the editor is open and back when it closes.
  constructor(api) {
    this.api = api;
    this.every = api.every ?? every;
    this.screen = null;
    this.shell = null;
    this.stopPolling = null;
    this.flying = false;
    this.keysDown = new Set();
    this.mode = 'place';
    this.tool = 'select';
    this.folder = 'houses';
    this.page = 0;
    this.tab = 0;
    this.armed = null;
    this.fitGround = true;
    this.gridStep = 1;
    this.angleStep = 15;
    this.turn = 0;
    this.terrainMode = 'raise';
    this.brushSize = 6;
    this.brushStrength = 1;
    this.placed = [];
    this.selected = null;
    this.outlinerPage = 0;
    this.outlinerQuery = '';
    this.outlinerFilter = 0;
    this.collapsed = new Set();
    this.contentQuery = '';
    // The search box that has the keys: 'outliner', 'content' or null.
    this.typing = null;
    this.cameraSpeed = 12;
    // The foliage paint brush: how many plants a press scatters, and over what radius (metres).
    this.paintCount = 1;
    this.paintRadius = 6;
    // The plant the Add nature brush scatters (the last chosen in Foliage).
    this.naturePlant = null;
    // How many plants one Add nature dab scatters (its own count: the Foliage brush starts at one).
    this.natureCount = NATURE_COUNT;
    // How densely Add nature plants the ground's own grass (clumps a square metre).
    this.grassDensity = GRASS_DENSITY;
    // The circles of the level's grass carpet Clear nature hid, and the next one's id.
    this.grassPatches = [];
    this.nextGrass = 1;
    // The Saves tab: the name being typed and the saves found on this PC.
    this.saveName = '';
    this.saves = [];
    // The last line from the cursor into the world (pointAt) and what moves it on.
    this.sight = null;
    this.sightEpoch = 0;
    // The time of day and the weather the author looks at the work in (architect/world_look):
    // the hour, an index in CLOCK_SPEEDS, WEATHERS and WINDS. Kept in the project.
    this.look = { hour: 12, clock: 0, weather: 0, wind: 1, weatherHeld: true };
    this.random = api.random ?? Math.random;
    // The project: the map's name, the ground edits in order (replayed when it is opened again),
    // the changes not saved yet and the save waiting to be made.
    this.map = 'trosecko';
    // The menu bar's map switch: null, 'choose' (the maps shown) or the map asked about.
    this.mapPicker = null;
    this.terrainEdits = [];
    this.unsaved = 0;
    this.saveTimer = null;
    this.keptProject = null;
    this.later = api.later ?? ((milliseconds, callback) => setTimeout(callback, milliseconds));
    // The see-through preview of the armed item at the cursor, and what it shows.
    this.ghost = null;
    this.ghostOf = null;
    // The selection's and the target's marks in the world, and the prefabs' pieces by name.
    this.marks = new Marks({ Gizmos: api.Gizmos });
    this.pieceCache = new Map();
    // The level's own objects the author hid (local level edits), and the next edit's id.
    this.hiddenLevel = [];
    // The level's entities moved where they stand (a door, a cart), each with its edit.
    this.movedLevel = [];
    // The level's loose objects (stones, logs: brushes) moved up or down with the ground the
    // Terrain brushes changed: { mesh, home, at, edit }.
    this.groundMoves = [];
    this.nextLevelEdit = 1;
    this.lastPress = null;
    // The content's pictures by entry name: the overlay's key of each (pictures sent once).
    this.pictures = new Map();
    this.undoStack = [];
    this.redoStack = [];
    this.message = 'Choose something in Content, then press in the world.';
    this.collision = new CollisionLayer({
      Collision: api.Collision,
      Gizmos: api.Gizmos,
      Vector3: api.Vector3,
    });
    this.catalogue = new Catalogue({
      prefabNames: api.prefabNames,
      vegetationKinds: () => api.Vegetation.kinds(),
      propMeshes: api.propMeshes,
      userMeshes: () => this.userAssets().meshes,
      effectNames: () => api.Vfx?.list?.() ?? [],
    });
    // The next id of a prop placed in this game (props/local_props.hpp).
    this.nextProp = 1;
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
      this.api.log('World Architect: this server does not allow the free camera');
      return false;
    }
    this.api.NoClip.setSpeed(this.cameraSpeed);
    this.overlook();
    this.api.gameUi?.(false);
    this.api.Controls?.unblockAction?.('open_pause_menu');
    // The author's own character is no part of the work: hidden until Play as player.
    this.api.send({ op: 'architectPlayer', hidden: true });
    // The game's pointer while the panels have it (a native screen holds no cursor of its own),
    // and the player's own controls off.
    this.api.Controls?.showCursor();
    this.api.Controls?.disable();
    this.catalogue.refresh();
    this.firstPlant = undefined;
    // Your own assets folder is looked at again (and mounted) when the editor opens.
    this.assets = null;
    if (this.parked) {
      // Back from Play as player: the screen built before is shown again (building it takes a
      // frame a clip, seconds;).
      this.screen = this.parked.screen;
      this.shell = this.parked.shell;
      this.parked = null;
      this.screen.setVisible(true);
      this.screen.focus();
      this.refresh();
    } else {
      this.screen = this.api.NativeUI.createScreen();
      this.screen.on('ready', () => this.build());
      this.screen.on('input', (action, phase) => this.input(action, phase));
    }
    this.pollTimes = { since: Date.now(), count: 0, total: 0, most: 0 };
    // The game client follows the cursor itself from now on (architect/cursor_pick.hpp).
    this.nativeCursor = null;
    this.api.send({ op: 'architectCursor', on: true });
    this.stopPolling = this.every(POLL_MS, () => this.timedPoll());
    return true;
  }

  // The free camera starts where the player stands, inside their body: it goes up and back,
  // looking at that place.
  overlook() {
    const { NoClip, Vector3 } = this.api;
    const at = NoClip.getPosition?.();
    if (!at || !NoClip.setPosition || !NoClip.lookAt) {
      return;
    }
    NoClip.setPosition(new Vector3(at.x, at.y - OVERLOOK.back, at.z + OVERLOOK.up));
    NoClip.lookAt(new Vector3(at.x, at.y, at.z));
    this.sightChanged();
  }

  // Where Play as player puts the player (PLAY_START): { x, y, z, yaw } with the camera's yaw, or
  // null when no ground is found (the player stays where they are).
  playerStart() {
    const { NoClip, World } = this.api;
    const camera = NoClip.getPosition?.();
    const sight = NoClip.getRotation?.();
    if (!camera || !sight || !World?.raycast) {
      return null;
    }
    const ground = (x, y, top) => {
      const hit = World.raycast(
        { x, y, z: top },
        { x, y, z: top - PLAY_START.down },
        { mode: 'ground' },
      );
      return hit?.position ? { x, y, z: hit.position.z, yaw: sight.yaw } : null;
    };
    const { view } = LAYOUT;
    const looked = this.lineAt(view.x + view.width / 2, view.y + view.height / 2)?.position;
    if (looked) {
      const dx = looked.x - camera.x;
      const dy = looked.y - camera.y;
      const flat = Math.hypot(dx, dy);
      if (Math.hypot(dx, dy, looked.z - camera.z) <= PLAY_START.reach) {
        const back = flat > PLAY_START.back ? PLAY_START.back / flat : 0;
        const found = ground(
          looked.x - dx * back,
          looked.y - dy * back,
          looked.z + PLAY_START.above,
        );
        if (found) {
          return found;
        }
      }
    }
    return ground(camera.x, camera.y, camera.z);
  }

  // `park`: Play as player keeps the screen, hidden, for the way back (Esc). Any other close (a map
  // switch, the world going) closes it: a level change takes its drawings with it, and a kept one
  // came back as bare text. `start`: where the player then starts
  // (playerStart), or null to stay.
  close({ park = false, start = null } = {}) {
    if (!park && this.parked) {
      this.parked.screen.close();
      this.parked = null;
    }
    if (!this.screen) {
      return false;
    }
    this.stopPolling?.();
    this.stopPolling = null;
    if (this.unsaved > 0) {
      this.save();
    }
    this.collision.hide();
    this.dropGhost();
    this.marks.clear();
    this.showRing(null);
    // The marks are the game's drawings: taken away, or they stay in the world while playing.
    if (this.api.Gizmos?.set) {
      this.drawMarksInWorld([]);
    }
    this.typing = null;
    if (park) {
      // Kept, hidden, for the way back (open shows it again at once). The text and pictures are
      // the game client's overlay, not the screen's: cleared, or they stay over the game
      // ("play sim kept ui"); the way back draws them again.
      this.shell?.clear();
      this.screen.setVisible(false);
      this.screen.blur();
      this.parked = { screen: this.screen, shell: this.shell };
    } else {
      // The texts and pictures are the game client's overlay: cleared, or they stay over the game
      // when the editor closes for good too (the AI's close).
      this.shell?.clear();
      this.screen.close();
    }
    this.screen = null;
    this.shell = null;
    if (!this.flying) {
      this.api.Controls?.hideCursor();
    }
    this.flying = false;
    this.api.Controls?.enable();
    this.api.NoClip.disable();
    this.api.send(
      start
        ? { op: 'architectPlayer', hidden: false, start }
        : { op: 'architectPlayer', hidden: false },
    );
    // Playing as the player: Esc brings the editor back (local_mode.hpp), not the pause menu.
    this.api.Controls?.blockAction?.('open_pause_menu');
    this.api.send({ op: 'architectCursor', on: false });
    this.nativeCursor = null;
    this.api.gameUi?.(true);
    return true;
  }

  build() {
    if (!this.screen) {
      return;
    }
    const handlers = {
      view: (x, y) => this.pressWorld(x, y),
      camSpeed: () => this.turnCameraSpeed(),
      tool: (index) => this.useTool(TOOLS[index]?.key),
      snap: (index) => this.pressSnap(index),
      mode: (index) => this.useMode(MODES[index]?.key),
      undo: () => this.pressAction(0),
      redo: () => this.pressAction(1),
      play: () => this.pressAction(2),
      outlinerSearch: () => this.startTyping('outliner'),
      outlinerFilter: () => this.turnOutlinerFilter(),
      outliner: (row) => this.pressOutliner(row),
      outlinerPage: (step) => this.turnOutlinerPage(step),
      detail: (row, side) => this.pressDetail(row, side),
      tab: (index) => this.pressTab(index),
      mapPicker: (state) => {
        this.mapPicker = state;
        this.refresh();
      },
      mapChoose: (index) => this.chooseMap(this.allMaps()[index].key),
      mapSwitch: () => this.switchMap(),
      architect: () => this.pressTab(3),
      leave: () => this.leave(),
      saveName: () => this.startTyping('save'),
      saveAs: () => this.saveAs(),
      saveRow: (row, action) => this.pressSave(row, action),
      aiCopy: (row) => this.copyAi(row),
      issue: (index) => this.pressIssue(index),
      folder: (index) => this.pressFolder(index),
      contentSearch: () => this.startTyping('content'),
      tile: (index) => this.pressTile(index),
      page: (step) => this.turnPage(step),
    };
    // Built hidden and shown whole: every clip call waits about a frame, so a screen built in
    // sight would appear block by block.
    this.screen.setVisible(false);
    this.shellPressed = (panel, px, py, took) => {
      this.api.log(
        `World Architect: press ${panel} ${Math.round(px)} ${Math.round(py)} (${took} ms)`,
      );
    };
    this.shell = new Shell(this.screen.root, handlers, GLYPHS, (message) => {
      this.api.send(message);
    });
    this.shell.pressed = this.shellPressed;
    this.shell.build();
    this.rebuildProject();
    this.refresh();
    this.screen.setVisible(true);
    this.screen.focus();
    // The game client's loading screen covered the world until now (architect/local_mode.hpp).
    this.api.send({ op: 'editorShown' });
  }

  // Escape: stop typing in a search box, then stop placing, then let go of the selection.
  input(action, phase) {
    // A parked screen (while playing as the player) takes no keys.
    if ((this.parked && !this.screen) || phase !== 'press' || action !== 'kcds_ui_back') {
      return;
    }
    if (this.typing) {
      // The game's screen sends Backspace as "back" too: in a search box it takes a letter
      // (type()), only Escape ends the typing (the search lost the keys after a
      // delete).
      if (this.api.Key.isDown('backspace')) {
        return;
      }
      this.typing = null;
    } else if (this.armed) {
      this.armed = null;
      this.message = 'Stopped placing.';
    } else {
      this.select(null);
      this.message = 'Nothing selected.';
    }
    this.refresh();
  }

  // A poll, timed: every POLL_REPORT_MS the log hears how long they took (each question to the game
  // waits for its next frame, so this follows the game's frame rate).
  timedPoll() {
    const started = Date.now();
    this.poll();
    const times = this.pollTimes;
    const took = Date.now() - started;
    times.count += 1;
    times.total += took;
    times.most = Math.max(times.most, took);
    if (started - times.since >= POLL_REPORT_MS) {
      const average = Math.round(times.total / times.count);
      this.api.log(
        `World Architect: ${times.count} polls, ${average} ms on average, ${times.most} ms at most`,
      );
      this.pollTimes = { since: started, count: 0, total: 0, most: 0 };
    }
  }

  // The right mouse button held looks and flies; otherwise the keys go to the search box that
  // has them, or are the shortcuts.
  poll() {
    const { Key, NoClip } = this.api;
    this.pollWalks();
    const looking = Key.isDown('mouse2');
    if (looking && !this.flying) {
      this.flying = true;
      this.typing = null;
      this.api.Controls?.hideCursor();
      NoClip.setInputEnabled(true);
      this.screen?.blur();
    } else if (!looking && this.flying) {
      this.flying = false;
      NoClip.setInputEnabled(false);
      this.api.Controls?.showCursor();
      this.screen?.focus();
    }
    if (this.flying) {
      // Looking around: no ring, preview or target box (the cursor is away).
      this.keysDown.clear();
      this.sightChanged();
      this.showRing(null);
      this.ghost?.setHidden(true);
      this.ghostPose = null;
      this.marks.target(null);
      // The hover name goes at once (it stayed until the button was let go).
      this.setHover(null);
      // The axes mark turns with the camera (refresh redraws only what changed).
      this.refresh();
      return;
    }
    this.strokeOn();
    this.dragOn();
    this.scrubOn();
    this.scaleHoldOn();
    this.followCursor();
    this.drawOverlay();
    const keys = this.typing ? SEARCH_KEYS : SHORTCUTS;
    for (const key of keys) {
      const down = Key.isDown(key);
      if (down && !this.keysDown.has(key)) {
        this.keysDown.add(key);
        if (this.typing) {
          this.type(key, Key.isDown('shift'));
        } else {
          this.shortcut(key, Key.isDown('ctrl'));
        }
      } else if (!down) {
        this.keysDown.delete(key);
      }
    }
  }

  startTyping(box) {
    this.typing = box;
    this.keysDown.clear();
    this.refresh();
  }

  // A key typed into the search box that has the keys.
  type(key, shift) {
    this.lastTyped = Date.now();
    const fields = { outliner: 'outlinerQuery', content: 'contentQuery', save: 'saveName' };
    const field = fields[this.typing] ?? 'contentQuery';
    let query = this[field];
    if (key === 'enter') {
      this.typing = null;
    } else if (key === 'backspace') {
      query = query.slice(0, -1);
    } else if (query.length < SEARCH_LENGTH) {
      if (key === 'space') {
        query += ' ';
      } else if (key === 'minus') {
        query += shift ? '_' : '-';
      } else {
        query += key;
      }
    }
    this[field] = query;
    if (field === 'contentQuery') {
      this.page = 0;
    } else if (field === 'outlinerQuery') {
      this.outlinerPage = 0;
    }
    this.refresh();
  }

  shortcut(key, ctrl) {
    if (ctrl && key === 'z') {
      this.pressAction(0);
      return;
    }
    if (ctrl && key === 'y') {
      this.pressAction(1);
      return;
    }
    if (ctrl && key === 'd') {
      this.duplicateSelected();
      this.refresh();
      return;
    }
    if (ctrl) {
      return;
    }
    if (key === 'e' && this.armed) {
      // While placing, E turns the item (and its preview) by the angle snap.
      this.turn = degreesOf(this.turn + this.angleStep);
      this.ghostPose = null;
      this.message = `Turned to ${this.turn}° (E)`;
      this.refresh();
      return;
    }
    const tool = TOOLS.find((entry) => entry.shortcut.toLowerCase() === key);
    if (tool) {
      this.useTool(tool.key);
      return;
    }
    const mode = MODES[Number(key) - 1];
    if (mode) {
      this.useMode(mode.key);
      return;
    }
    if (key === 'delete') {
      this.deleteSelected();
      this.refresh();
    }
    if (key === 'f') {
      this.focusSelected();
    }
  }

  // Where the pointer at stage point (x, y) meets the world: its hit, or null.
  pointAt(x, y) {
    // The game client's own hit for the cursor where it is now: no question to ask.
    const native = this.nativeCursor;
    if (native && !this.testCursor && native.x === x && native.y === y) {
      return native.hit;
    }
    // Each question to the game waits for its next frame and holds the script meanwhile, so the
    // last answer stands while neither the cursor nor the camera nor the world moved.
    const key = `${x},${y},${this.sightEpoch}`;
    if (this.sight?.key === key) {
      return this.sight.hit;
    }
    const hit = this.lineAt(x, y);
    this.sight = { key, hit };
    return hit;
  }

  // Something moved the camera or changed the world: the next pointAt asks the game again.
  sightChanged() {
    this.sightEpoch += 1;
  }

  // The editor's stage: the window's shape, 1080 high or more (theme.cjs stageFor).
  stageSize() {
    return { width: LAYOUT.width, height: LAYOUT.height };
  }

  // A point of the game's own 1920 by 1080 stage (stretched over the window) on the editor's stage.
  fromGameStage(x, y) {
    return { x: (x * LAYOUT.width) / STAGE.width, y: (y * LAYOUT.height) / STAGE.height };
  }

  // The game window's size in pixels: the editor's stage takes its shape, and the panels move
  // when it changed.
  windowSized(width, height) {
    const stage = stageFor(width, height);
    if (fitStage(stage.width, stage.height)) {
      this.shell?.placePanels();
      this.refresh();
    }
  }

  lineAt(x, y) {
    const stage = this.stageSize();
    const screenX = (x / stage.width) * 2 - 1;
    const screenY = 1 - (y / stage.height) * 2;
    const from = this.api.NoClip.getPosition();
    const direction = this.api.NoClip.screenLine(screenX, screenY);
    if (!from || !direction) {
      return null;
    }
    const to = {
      x: from.x + direction.x * REACH,
      y: from.y + direction.y * REACH,
      z: from.z + direction.z * REACH,
    };
    // 'anything': doors and the level's other movable things too, not only what gives cover.
    return this.api.World.raycast(from, to, { mode: 'anything' });
  }

  // Runs a command and keeps it for Undo.
  // The cursor's stage x anywhere over the editor (null when not known).
  scrubX() {
    const cursor = this.testCursor ?? this.nativeCursor;
    return cursor && Number.isFinite(cursor.x) ? cursor.x : null;
  }

  // While a value's button stays down: a step for every SCRUB_PX the cursor went sideways (right
  // up, left down); let go, the steps are one undo step.
  scrubOn() {
    const scrub = this.scrub;
    if (!scrub) {
      return;
    }
    if (!this.api.Key.isDown('mouse1')) {
      this.scrub = null;
      this.mergeSteps(scrub.undoAt);
      this.refresh();
      return;
    }
    const x = this.scrubX();
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
    const dir = Math.sign(wanted - scrub.applied);
    while (scrub.applied !== wanted) {
      row.step(dir);
      scrub.applied += dir;
    }
    this.refresh();
  }

  // The undo steps made since `from` as one (a scrub's many small steps).
  mergeSteps(from) {
    const steps = this.undoStack.splice(from);
    if (steps.length <= 1) {
      this.undoStack.push(...steps);
      return;
    }
    this.undoStack.push({
      label: steps[0].label,
      redo: () => {
        for (const step of steps) {
          step.redo();
        }
        return true;
      },
      undo: () => {
        for (const step of [...steps].reverse()) {
          step.undo();
        }
      },
    });
  }

  run(command) {
    if (!command.redo()) {
      return false;
    }
    this.undoStack.push(command);
    this.redoStack = [];
    this.changed();
    return true;
  }

  undo() {
    const command = this.undoStack.pop();
    if (command) {
      command.undo();
      this.redoStack.push(command);
      this.message = `Undone: ${command.label}`;
      this.changed();
    }
  }

  redo() {
    const command = this.redoStack.pop();
    if (command && command.redo()) {
      this.undoStack.push(command);
      this.message = `Redone: ${command.label}`;
      this.changed();
    }
  }

  // ---- The project on disk ----

  // A change: saved a moment later (several changes in a row are saved once).
  changed() {
    this.unsaved += 1;
    if (this.saveTimer === null) {
      this.saveTimer = this.later(SAVE_DELAY_MS, () => {
        // A save made meanwhile (on Exit, or before a map switch) leaves nothing to save.
        if (this.saveTimer === null) {
          return;
        }
        this.saveTimer = null;
        this.save();
      });
    }
  }

  // What the author built, as the project file keeps it.
  project() {
    const point = (at) => ({ x: at.x, y: at.y, z: at.z });
    return {
      version: PROJECT_VERSION,
      map: this.map,
      terrain: this.terrainEdits.map((edit) => ({
        mode: edit.mode,
        at: point(edit.at),
        size: edit.size,
        strength: edit.strength,
      })),
      buildings: this.placed
        .filter((record) => record.entry.source === 'prefab')
        .map((record) => ({
          name: record.entry.name,
          at: point(record.at),
          turn: record.turn,
          fit: Boolean(record.pad),
        })),
      plants: this.placed
        .filter((record) => record.entry.source === 'vegetation')
        .map((record) => ({
          name: record.entry.name,
          at: point(record.at),
          turn: record.turn,
          ...(record.scale && record.scale !== 1 ? { scale: record.scale } : {}),
        })),
      lights: this.placed
        .filter((record) => record.entry.source === 'light')
        .map((record) => ({
          name: record.entry.name,
          at: point(record.at),
          light: { ...record.light, color: { ...record.light.color } },
        })),
      sounds: this.placed
        .filter((record) => record.entry.source === 'sound')
        .map((record) => ({ name: record.entry.name, at: point(record.at) })),
      effects: this.placed
        .filter((record) => record.entry.source === 'effect')
        .map((record) => ({
          name: record.entry.name,
          at: point(record.at),
          turn: record.turn,
          ...(record.fx ? { scale: record.fx.scale, pulse: record.fx.pulse } : {}),
        })),
      props: this.placed
        .filter((record) => record.entry.source === 'prop')
        .map((record) => ({
          name: record.entry.name,
          at: point(record.at),
          turn: record.turn,
          ...(record.tilt ? { tilt: { ...record.tilt } } : {}),
          ...(record.scale && record.scale !== 1 ? { scale: record.scale } : {}),
          ...(record.material ? { material: record.material } : {}),
          ...(record.mover ? { mover: { ...record.mover } } : {}),
        })),
      shapes: this.collision.shapes.map((shape) => ({
        kind: shape.kind,
        ground: point(shape.ground),
        size: { ...shape.size },
        turn: shape.turn,
      })),
      // The zones as a server's Area.create takes them (KCDS-DOCS/areas.md).
      areas: this.collision.shapes
        .filter((shape) => SHAPE_KINDS[shape.kind].zone)
        .map((shape) => this.collision.areaOf(shape)),
      look: { ...this.look },
      grass: this.grassPatches.map((patch) => ({ x: patch.x, y: patch.y, radius: patch.radius })),
      grounded: this.groundMoves.map((move) => ({
        mesh: move.mesh,
        home: point(move.home),
        at: point(move.at),
      })),
      moved: this.movedLevel.map((thing) => ({
        kind: thing.kind,
        object: {
          kind: thing.object.kind,
          name: thing.object.name,
          guid: thing.object.guid ?? '',
          class: thing.object.class ?? '',
          at: point(thing.object.at),
          rotation: { ...(thing.object.rotation ?? { x: 0, y: 0, z: 0, w: 1 }) },
          scale: point(thing.object.scale ?? { x: 1, y: 1, z: 1 }),
        },
        at: point(thing.at),
        turn: thing.turn,
        ...((thing.scale ?? 1) !== 1 ? { scale: thing.scale } : {}),
      })),
      hidden: this.hiddenLevel.map((thing) => ({
        kind: thing.kind,
        object: {
          kind: thing.object.kind,
          name: thing.object.name,
          guid: thing.object.guid ?? '',
          class: thing.object.class ?? '',
          at: point(thing.object.at),
          // Its turn and box, so the box drawn around it later is its own (a hidden thing can
          // no longer be met by a ray to ask again).
          ...(thing.object.rotation ? { rotation: { ...thing.object.rotation } } : {}),
        },
        ...(Array.isArray(thing.box) ? { box: [...thing.box] } : {}),
      })),
    };
  }

  save() {
    if (this.saveTimer !== null) {
      this.saveTimer = null;
    }
    // A message holds 64 KiB: a larger project goes in parts, written once the last came
    // (architect/project_store.hpp; a large save used to be dropped).
    const text = JSON.stringify(this.project());
    const parts = Math.max(1, Math.ceil(text.length / SAVE_PIECE));
    for (let part = 0; part < parts; part += 1) {
      const data = text.slice(part * SAVE_PIECE, (part + 1) * SAVE_PIECE);
      this.api.send(
        parts === 1
          ? { op: 'architectSave', map: this.map, data }
          : { op: 'architectSave', map: this.map, data, part, parts },
      );
    }
    this.unsaved = 0;
    this.refresh();
  }

  // The project the game client kept for the map, rebuilt once the editor's screen stands.
  projectKept(map, data, part = 0, parts = 1) {
    // A project larger than a message (64 KiB) comes in parts, in order.
    if (Number.isInteger(parts) && parts > 1) {
      if (part === 0) {
        this.projectParts = [];
      }
      if (!this.projectParts || part !== this.projectParts.length) {
        this.projectParts = null;
        return;
      }
      this.projectParts.push(data);
      if (this.projectParts.length < parts) {
        return;
      }
      data = this.projectParts.join('');
      this.projectParts = null;
    }
    this.map = map || this.map;
    let project = null;
    try {
      project = data ? JSON.parse(data) : null;
    } catch {
      this.api.log('World Architect: the kept project could not be read');
    }
    this.keptProject = project && project.version === PROJECT_VERSION ? project : null;
    if (this.shell) {
      this.rebuildProject();
    }
  }

  // Builds the kept project again, as it was: the ground first, then the hidden objects, the
  // buildings, the plants and the shapes. None of it is an undo step.
  rebuildProject({ merge = false } = {}) {
    const project = this.keptProject;
    this.keptProject = null;
    if (!project) {
      return;
    }
    // A merge adds the save's things; the light stays as the author has it.
    if (!merge) {
      this.restoreLook(project.look);
    }
    for (const patch of project.grass ?? []) {
      this.hideGrass({ x: patch.x, y: patch.y }, patch.radius);
    }
    const { Terrain, Prefab, Vegetation, Vector3 } = this.api;
    for (const edit of project.terrain ?? []) {
      this.applyTerrain(edit.mode, edit.at, edit.size, edit.strength);
    }
    for (const kept of project.grounded ?? []) {
      if (typeof kept?.mesh !== 'string' || !kept.home || !kept.at) {
        continue;
      }
      const move = { mesh: kept.mesh, home: { ...kept.home }, at: { ...kept.at } };
      move.edit = this.nextLevelEdit;
      this.nextLevelEdit += 1;
      this.groundMoves.push(move);
      this.sendGroundMove(move);
    }
    for (const moved of project.moved ?? []) {
      if ((moved.object?.kind === 'entity' && !moved.object.guid) || !moved.at) {
        continue;
      }
      const thing = {
        level: true,
        object: { ...moved.object },
        kind: moved.kind,
        at: { ...moved.at },
        turn: Number.isFinite(moved.turn) ? moved.turn : 0,
        scale:
          Number.isFinite(moved.scale) && moved.scale >= 0.1 && moved.scale <= 10 ? moved.scale : 1,
        hidden: false,
        edit: 0,
      };
      this.movedLevel.push(thing);
      this.sendEntityPose(thing);
    }
    for (const hidden of project.hidden ?? []) {
      const object = { ...hidden.object, at: hidden.object.at };
      const box =
        Array.isArray(hidden.box) && hidden.box.length === 6 && hidden.box.every(Number.isFinite)
          ? [...hidden.box]
          : null;
      const thing = {
        level: true,
        object,
        box,
        kind: hidden.kind,
        at: { ...object.at },
        turn: object.rotation ? yawOf(object.rotation) : 0,
        hidden: false,
        edit: 0,
      };
      this.hideLevel(thing);
    }
    for (const building of project.buildings ?? []) {
      const entry = this.entryOf('prefab', building.name);
      const record = { entry, at: building.at, turn: building.turn, handle: null, pad: null };
      if (building.fit) {
        record.pad = this.levelPad(record);
      }
      const position = new Vector3(record.at.x, record.at.y, record.at.z);
      record.handle = Prefab.place(entry.name, position, { x: 0, y: 0, z: record.turn });
      if (record.handle) {
        this.placed.push(record);
        this.coverGrass(record);
      }
    }
    const plants = (project.plants ?? []).map((plant) => ({
      entry: this.entryOf('vegetation', plant.name),
      at: plant.at,
      turn: plant.turn,
      scale: plant.scale ?? 1,
    }));
    this.plantAll(plants).forEach((handle, index) => {
      if (handle) {
        this.placed.push({ ...plants[index], handle, pad: null });
      }
    });
    for (const kept of project.lights ?? []) {
      const entry = this.lightEntry(kept.name);
      const record = { entry, at: kept.at, turn: 0, handle: null, pad: null };
      record.light = { ...lightOfPreset(kept.name), ...(kept.light ?? {}) };
      record.handle = this.lightHandle(record);
      if (record.handle) {
        this.placed.push(record);
      }
    }
    for (const kept of project.sounds ?? []) {
      const record = {
        entry: soundEntry(kept.name),
        at: kept.at,
        turn: 0,
        handle: null,
        pad: null,
      };
      record.handle = this.soundHandle(kept.name, kept.at);
      if (record.handle) {
        this.placed.push(record);
      }
    }
    for (const effect of project.effects ?? []) {
      const entry = this.entryOf('effect', effect.name);
      const fx = effectSettings(effect, effect.name);
      const handle = this.effectHandle(entry.name, effect.at, effect.turn, fx);
      this.placed.push({ entry, at: effect.at, turn: effect.turn, fx, handle, pad: null });
    }
    // Your own models and materials are read from your assets folder, which is mounted when it is
    // first asked for: before the props that use them are built, or they stay empty (owner
    // the opened church's walls and tower did not draw until placed again).
    const yours = (name) => typeof name === 'string' && name.startsWith(USER_ASSET_PREFIX);
    if ((project.props ?? []).some((prop) => yours(prop.name) || yours(prop.material))) {
      this.userAssets();
    }
    for (const prop of project.props ?? []) {
      const entry = this.entryOf('prop', prop.name);
      const tilt = prop.tilt ?? null;
      const scale = prop.scale ?? 1;
      const material = typeof prop.material === 'string' ? prop.material : null;
      const handle = this.propHandle(entry.name, prop.at, prop.turn, tilt, scale, material);
      const mover = checkedMover(prop.mover);
      this.placed.push({
        entry,
        at: prop.at,
        turn: prop.turn,
        tilt,
        scale,
        material,
        handle,
        pad: null,
        ...(mover ? { mover } : {}),
      });
    }
    for (const kept of project.shapes ?? []) {
      this.collision.kind = kept.kind;
      const shape = this.collision.make(kept.ground, kept.turn);
      shape.size = { ...kept.size };
      this.collision.build(shape);
    }
    this.select(null);
    const count = this.placed.length + this.collision.shapes.length + this.hiddenLevel.length;
    this.message = `Opened the ${this.map} project: ${count} objects`;
    this.refresh();
  }

  // A catalogue entry by its source and full name.
  entryOf(source, name) {
    return { label: shortName(name), name, source };
  }

  // ---- The world view ----

  pressWorld(x, y) {
    const { view } = LAYOUT;
    const inside =
      x >= view.x && x < view.x + view.width && y >= view.y && y < view.y + view.height;
    if (!inside || this.flying) {
      return;
    }
    // A press on a move arrow drags it (the arrow may point at the sky: no ground needed).
    if (this.mode !== 'terrain' && !this.armed && this.grabArrow(x, y)) {
      return;
    }
    // The game client's hit for the cursor when the press is where it is (it carries the box).
    const native = this.nativeCursor;
    const close = native && native.hit && Math.hypot(native.x - x, native.y - y) <= PRESS_NEAR;
    const hit = close ? native.hit : this.pointAt(x, y);
    if (!hit) {
      this.message = 'Nothing there: press on the ground.';
      this.refresh();
      return;
    }
    const at = { x: hit.position.x, y: hit.position.y, z: hit.position.z };
    this.lastPress = at;
    this.lastHit = hit;
    if (this.walker) {
      this.walkTo(this.walker, at);
    } else if (this.mode === 'terrain') {
      this.shapeGround(at);
    } else if (this.armed) {
      this.putDown(at);
    } else {
      this.useToolAt(at);
    }
    this.refresh();
  }

  // The armed content where the press met the ground, on the grid. One press puts one down and
  // the editor is back to selecting; with Shift held it keeps placing.
  putDown(at) {
    if (this.armed.source === 'vegetation' && this.paintCount > 1) {
      // The paint brush keeps painting until Escape.
      this.paint(at);
      return;
    }
    const spot = { x: snap(at.x, this.gridStep), y: snap(at.y, this.gridStep), z: at.z };
    if (this.armed.source === 'shape') {
      this.addShape(spot);
    } else {
      this.place(spot);
    }
    if (!this.api.Key?.isDown('shift')) {
      this.armed = null;
      this.tool = 'select';
      this.dropGhost();
    }
  }

  // The paint brush: the armed plant scattered paintCount times within paintRadius of the press,
  // each on the ground under its spot, each turned at random; one undo step for all.
  paint(at) {
    const entry = this.armed;
    const { Vegetation } = this.api;
    const spots = this.scatter(at, this.paintRadius, this.paintCount);
    const records = [];
    const command = {
      label: `paint ${spots.length} × ${entry.label}`,
      redo: () => {
        records.length = 0;
        for (const spot of spots) {
          const handle = Vegetation.plant(entry.name, spot.at, { turn: spot.turn });
          if (handle) {
            const record = { entry, at: spot.at, turn: spot.turn, handle, pad: null };
            records.push(record);
            this.placed.push(record);
          }
        }
        return records.length > 0;
      },
      undo: () => {
        for (const record of records) {
          this.takeAway(record);
        }
      },
    };
    this.message = this.run(command)
      ? `Painted ${records.length} × ${entry.label}`
      : `The game could not plant ${entry.label} there.`;
  }

  // `count` spots at random within `radius` of `at`, each on the ground there and turned at random
  // (the spots without ground left out).
  scatter(at, radius, count) {
    const points = [];
    for (let index = 0; index < count; index += 1) {
      const angle = this.random() * Math.PI * 2;
      const reach = Math.sqrt(this.random()) * radius;
      points.push({
        x: at.x + Math.cos(angle) * reach,
        y: at.y + Math.sin(angle) * reach,
        z: at.z,
      });
    }
    const grounds = this.groundsOf(points);
    const spots = [];
    points.forEach((point, index) => {
      if (grounds[index] !== null) {
        spots.push({
          at: { x: point.x, y: point.y, z: grounds[index] },
          turn: Math.floor(this.random() * 360),
        });
      }
    });
    return spots;
  }

  // The terrain's height under each point (null where none is found within `reach`; tree tops
  // and rocks do not count): one question to the game for them all, else one ray each.
  groundsOf(points, reach = PAINT_REACH, batch = HEIGHTS_AT_ONCE) {
    if (this.api.groundHeights) {
      const grounds = [];
      for (let first = 0; first < points.length; first += batch) {
        const part = points.slice(first, first + batch);
        const found = this.api.groundHeights(
          part.map((p) => [p.x, p.y, p.z]),
          reach,
        );
        if (!Array.isArray(found) || found.length !== part.length) {
          break;
        }
        grounds.push(...found.map((z) => (typeof z === 'number' ? z : null)));
      }
      if (grounds.length === points.length) {
        return grounds;
      }
    }
    // One ray each waits a game frame: only for a few points (hundreds held the editor for
    // seconds when the batch question failed).
    if (this.api.groundHeights && points.length > RAY_FALLBACK_MOST) {
      return points.map(() => null);
    }
    return points.map(({ x, y, z }) => {
      const ground = this.api.World.raycast(
        { x, y, z: z + reach },
        { x, y, z: z - reach },
        { mode: 'ground' },
      );
      return ground ? ground.position.z : null;
    });
  }

  // ---- The preview at the cursor ----

  // The see-through preview follows the cursor over the world while an item is armed.
  followCursor() {
    const entry = this.armed;
    if (!this.screen) {
      return;
    }
    if (this.mode === 'terrain') {
      this.dropGhost();
      this.marks.target(null);
      const cursor = this.cursorOverView();
      const hit = cursor ? this.pointAt(cursor.x, cursor.y) : null;
      this.showRing(hit ? hit.position : null);
      return;
    }
    this.showRing(null);
    if (!entry || entry.source === 'shape') {
      this.dropGhost();
      this.markTarget();
      return;
    }
    this.marks.target(null);
    const cursor = this.cursorOverView();
    if (!cursor) {
      this.ghost?.setHidden(true);
      return;
    }
    const hit = this.pointAt(cursor.x, cursor.y);
    if (!hit) {
      this.ghost?.setHidden(true);
      return;
    }
    if (this.ghostOf !== entry) {
      this.makeGhost(entry, hit.position);
    }
    if (!this.ghost) {
      return;
    }
    const at = hit.position;
    const spot = { x: snap(at.x, this.gridStep), y: snap(at.y, this.gridStep), z: at.z };
    const key = `${spot.x},${spot.y},${spot.z.toFixed(2)},${this.turn}`;
    if (this.ghostPose !== key) {
      this.ghostPose = key;
      this.ghost.setHidden(false);
      // A preview's turn is a quaternion (previews.cjs turnOf): the turn about the up axis.
      const half = (this.turn * Math.PI) / 360;
      this.ghost.setPose(new this.api.Vector3(spot.x, spot.y, spot.z), {
        x: 0,
        y: 0,
        z: Math.sin(half),
        w: Math.cos(half),
      });
    }
  }

  // The world's X, Y and Z as the camera sees them: for each, its direction on the stage (right
  // and down positive, length up to 1), for the axes mark in the view's corner.
  viewAxes() {
    const { NoClip, Vector3 } = this.api;
    const eye = NoClip.getPosition?.();
    const ahead = eye && NoClip.screenLine?.(0, 0);
    if (!ahead || !NoClip.toScreen) {
      return null;
    }
    const centre = { x: eye.x + ahead.x * 10, y: eye.y + ahead.y * 10, z: eye.z + ahead.z * 10 };
    const origin = NoClip.toScreen(new Vector3(centre.x, centre.y, centre.z));
    if (!origin) {
      return null;
    }
    const axes = {};
    for (const [name, step] of Object.entries({ x: [1, 0, 0], y: [0, 1, 0], z: [0, 0, 1] })) {
      const end = NoClip.toScreen(
        new Vector3(centre.x + step[0], centre.y + step[1], centre.z + step[2]),
      );
      if (!end) {
        axes[name] = { x: 0, y: 0 };
        continue;
      }
      const dx = end.x - origin.x;
      const dy = origin.y - end.y;
      const length = Math.hypot(dx, dy) || 1;
      const shrink = Math.min(1, length * 6) / length;
      axes[name] = {
        x: Math.round(dx * shrink * 100) / 100,
        y: Math.round(dy * shrink * 100) / 100,
      };
    }
    return axes;
  }

  // ---- Dragging a move arrow ----

  // A stage point as the camera sees a world point, or null behind it.
  stagePoint(point) {
    const { NoClip, Vector3 } = this.api;
    const screen = NoClip.toScreen?.(new Vector3(point.x, point.y, point.z));
    if (!screen) {
      return null;
    }
    const stage = this.stageSize();
    return { x: ((screen.x + 1) / 2) * stage.width, y: ((1 - screen.y) / 2) * stage.height };
  }

  // How far along the arrow's on-stage line the stage point (x, y) lies, as a share of the arrow.
  alongArrow(arrow, x, y) {
    const tip = {
      x: arrow.from.x + arrow.direction.x * arrow.length,
      y: arrow.from.y + arrow.direction.y * arrow.length,
      z: arrow.from.z + arrow.direction.z * arrow.length,
    };
    const a = this.stagePoint(arrow.from);
    const b = this.stagePoint(tip);
    if (!a || !b) {
      return null;
    }
    const dx = b.x - a.x;
    const dy = b.y - a.y;
    const span = dx * dx + dy * dy;
    if (span < 1) {
      return null;
    }
    const share = ((x - a.x) * dx + (y - a.y) * dy) / span;
    const nearX = a.x + dx * Math.max(0, Math.min(1, share));
    const nearY = a.y + dy * Math.max(0, Math.min(1, share));
    return { share, distance: Math.hypot(x - nearX, y - nearY) };
  }

  // A press on one of the selection's arrows starts dragging it along its axis.
  grabArrow(x, y) {
    const kind = this.selectionKind();
    if (kind && this.tool === 'rotate') {
      return this.grabRing(x, y);
    }
    if (!kind || (this.tool !== 'select' && this.tool !== 'move')) {
      return false;
    }
    let best = null;
    for (const arrow of this.marks.arrows) {
      const along = this.alongArrow(arrow, x, y);
      if (along && along.distance <= ARROW_REACH && (!best || along.distance < best.distance)) {
        best = { arrow, share: along.share, distance: along.distance };
      }
    }
    if (!best) {
      return false;
    }
    this.drag = {
      arrow: best.arrow,
      share: best.share,
      start: { ...this.selectedAt() },
      at: { ...this.selectedAt() },
    };
    this.message = `Dragging along ${best.arrow.axis.toUpperCase()}`;
    this.refresh();
    return true;
  }

  // Where on a ring the stage point (x, y) is: the angle of the ring's nearest point to it and how
  // far that point is on the stage; null when the ring is not on the stage.
  onRing(ring, x, y) {
    let best = null;
    for (let step = 0; step < RING_SAMPLES; step += 1) {
      const angle = (step / RING_SAMPLES) * Math.PI * 2;
      const point = this.stagePoint(ringPoint(ring, angle));
      if (!point) {
        continue;
      }
      const distance = Math.hypot(point.x - x, point.y - y);
      if (!best || distance < best.distance) {
        best = { angle, distance };
      }
    }
    return best;
  }

  // A press near one of the Rotate tool's rings starts turning the selection
  // ("a ring to turn, not X Y Z"; then "another rotate axis"): the Z ring turns it about the up
  // axis, the X and Y rings tilt it about its own axes. The cursor's place along the ring gives
  // the angle, so the ring turns the way the cursor goes from any side.
  grabRing(x, y) {
    let best = null;
    for (const ring of this.marks.rings ?? []) {
      const on = this.onRing(ring, x, y);
      if (on && on.distance <= ARROW_REACH && (!best || on.distance < best.on.distance)) {
        best = { ring, on };
      }
    }
    if (!best) {
      return false;
    }
    let thing = this.selected;
    if (best.ring.axis !== 'z' && thing.level) {
      // The level's object becomes your copy where it stands, which then tilts.
      this.moveLevel(thing, { ...thing.at }, thing.turn);
      if (this.selectionKind() !== 'prop') {
        return false;
      }
      thing = this.selected;
    }
    this.drag = {
      ring: best.ring,
      startAngle: best.on.angle,
      startTurn: thing.turn ?? 0,
      turn: thing.turn ?? 0,
      tiltBy: 0,
      start: { ...this.selectedAt() },
      at: { ...this.selectedAt() },
    };
    this.message =
      best.ring.axis === 'z'
        ? 'Turning: drag round the ring'
        : `Tilting about ${best.ring.axis.toUpperCase()}: drag round the ring`;
    this.refresh();
    return true;
  }

  // Whether a level object tilts on the X and Y rings: a brush of the catalog's meshes, which the
  // first tilt makes your copy (as a move does).
  tiltsAsCopy(thing) {
    if (!thing.level || thing.kind === 'vegetation' || thing.object.kind !== 'brush') {
      return false;
    }
    return Boolean(this.api.meshKnown?.(thing.object.name));
  }

  // A tilt of a prop about its own X or Y axis by `degrees`, from the rotation it has: its new full
  // rotation (kept as its tilt) and the turn read from it.
  tilted(thing, axis, degrees) {
    const base = thing.tilt ?? { x: 0, y: 0, z: 0, w: 1 };
    const half = ((thing.turn - (thing.tilt ? yawOf(thing.tilt) : 0)) * Math.PI) / 360;
    const full = multiplied({ x: 0, y: 0, z: Math.sin(half), w: Math.cos(half) }, base);
    const angle = (degrees * Math.PI) / 360;
    const about = {
      x: axis === 'x' ? Math.sin(angle) : 0,
      y: axis === 'y' ? Math.sin(angle) : 0,
      z: 0,
      w: Math.cos(angle),
    };
    const tilt = multiplied(full, about);
    return { tilt, turn: yawOf(tilt) };
  }

  // Tilts the selected prop as one undo step.
  tiltSelected(axis, degrees) {
    const thing = this.selected;
    const before = { tilt: thing.tilt ?? null, turn: thing.turn };
    const after = this.tilted(thing, axis, degrees);
    const apply = (pose) => {
      thing.tilt = pose.tilt;
      thing.turn = pose.turn;
      thing.handle?.setTilt?.(pose.tilt, thing.at, pose.turn);
      return true;
    };
    this.run({ label: 'tilt', redo: () => apply(after), undo: () => apply(before) });
    this.message = `${this.nameOf(thing)}: tilted ${degrees}° about ${axis.toUpperCase()}`;
  }

  // While the ring is held: the turn follows the cursor round it, in angle steps; let go, one step.
  ringOn(drag, thing) {
    if (drag.ring.axis !== 'z') {
      this.tiltRingOn(drag, thing);
      return;
    }
    if (!this.api.Key.isDown('mouse1') || !thing) {
      this.drag = null;
      drag.ghost?.destroy?.();
      if (thing && drag.turn !== drag.startTurn) {
        if (this.selectionKind() === 'shape') {
          thing.turn = drag.startTurn;
        }
        this.moveSelected(drag.start, drag.turn);
      } else if (thing) {
        this.showPose(thing, drag.start, drag, drag.startTurn);
      }
      this.refresh();
      return;
    }
    const cursor = this.testCursor ?? this.nativeCursor;
    if (!cursor) {
      return;
    }
    const swept = this.sweptOnRing(drag, cursor);
    if (swept === null) {
      return;
    }
    const step = this.angleStep > 0 ? this.angleStep : 1;
    const turn = degreesOf(drag.startTurn + Math.round(swept / step) * step);
    if (turn === drag.turn) {
      return;
    }
    drag.turn = turn;
    this.marks.select({ ...this.boxOf(thing), turn });
    this.showPose(thing, drag.start, drag, turn);
  }

  // How far round its ring (degrees, counterclockwise about its axis) the cursor went since the
  // press, -180 to 180; null off the stage.
  sweptOnRing(drag, cursor) {
    const on = this.onRing(drag.ring, cursor.x, cursor.y);
    if (!on) {
      return null;
    }
    let swept = ((on.angle - drag.startAngle) * 180) / Math.PI;
    swept = ((((swept + 180) % 360) + 360) % 360) - 180;
    return swept;
  }

  // While an X or Y ring is held: the prop tilts with the cursor round it (in angle steps), shown
  // live; let go, one undo step.
  tiltRingOn(drag, thing) {
    if (!this.api.Key.isDown('mouse1') || !thing) {
      this.drag = null;
      if (thing) {
        // Back to where it was, then the tilt as one step.
        thing.handle?.setTilt?.(thing.tilt ?? null, thing.at, thing.turn);
        if (drag.tiltBy !== 0) {
          this.tiltSelected(drag.ring.axis, drag.tiltBy);
        }
      }
      this.refresh();
      return;
    }
    const cursor = this.testCursor ?? this.nativeCursor;
    const swept = cursor ? this.sweptOnRing(drag, cursor) : null;
    if (swept === null) {
      return;
    }
    const step = this.angleStep > 0 ? this.angleStep : 1;
    const tiltBy = Math.round(swept / step) * step;
    if (tiltBy === drag.tiltBy) {
      return;
    }
    drag.tiltBy = tiltBy;
    const pose = this.tilted(thing, drag.ring.axis, tiltBy);
    thing.handle?.setTilt?.(pose.tilt, thing.at, pose.turn);
    this.message = `Tilting ${tiltBy}° about ${drag.ring.axis.toUpperCase()}`;
  }

  // While the left button stays down the selection's box slides along the arrow; let go, the thing
  // moves there as one step.
  dragOn() {
    const drag = this.drag;
    if (!drag) {
      return;
    }
    if (drag.ring) {
      this.ringOn(drag, this.selected);
      return;
    }
    const thing = this.selected;
    if (!this.api.Key.isDown('mouse1') || !thing) {
      this.drag = null;
      drag.ghost?.destroy?.();
      const moved = Math.hypot(
        drag.at.x - drag.start.x,
        drag.at.y - drag.start.y,
        drag.at.z - drag.start.z,
      );
      if (thing) {
        // The move is one undo step (from where the drag started: the thing's own place is not
        // changed while it is shown along the arrow); a drag back to the start shows it there.
        if (moved > 0.001) {
          if (this.selectionKind() === 'shape') {
            // A shape is shown by its own place: the step starts from where the drag began.
            thing.ground = { ...drag.start };
          }
          this.moveSelected(drag.at, thing.turn);
        } else {
          this.showPose(thing, drag.start, drag);
        }
      }
      this.refresh();
      return;
    }
    const cursor = this.cursorOverView();
    const along = cursor ? this.alongArrow(drag.arrow, cursor.x, cursor.y) : null;
    if (!along) {
      return;
    }
    const { direction, length } = drag.arrow;
    // The way along the arrow goes in grid steps, from where the thing was: it stays on its line
    // (snapping x and y on their own threw a turned wall off it).
    let distance = (along.share - drag.share) * length;
    if (direction.z === 0) {
      distance = snap(distance, this.gridStep);
    }
    const at = {
      x: drag.start.x + direction.x * distance,
      y: drag.start.y + direction.y * distance,
      z: drag.start.z + direction.z * distance,
    };
    drag.at = at;
    const box = this.boxOf(thing);
    this.marks.select({ ...box, at });
    // The thing itself follows the arrow while it is held,
    // a few times a second: a plant is planted again each time.
    const now = Date.now();
    const every = this.selectionKind() === 'vegetation' ? DRAG_PLANT_MS : DRAG_SHOW_MS;
    if (now - (drag.shownAt ?? 0) >= every) {
      drag.shownAt = now;
      this.showPose(thing, at, drag);
    }
  }

  // Shows the dragged thing at `at` without a step (the drag's end makes the step).
  showPose(thing, at, drag, turn = thing.turn) {
    const kind = this.selectionKind();
    const shown = drag.shown ?? drag.start;
    const shownTurn = drag.shownTurn ?? thing.turn;
    if (Math.hypot(at.x - shown.x, at.y - shown.y, at.z - shown.z) < 0.001 && turn === shownTurn) {
      return;
    }
    drag.shown = { ...at };
    drag.shownTurn = turn;
    if (POSED_KINDS.has(kind)) {
      thing.handle?.setPose(at, { x: 0, y: 0, z: turn });
    } else if (kind === 'shape') {
      thing.ground = { ...at };
      thing.turn = turn;
      this.collision.update(thing);
    } else if (kind === 'vegetation') {
      const handle = this.api.Vegetation.plant(thing.entry.name, at, {
        turn,
        scale: thing.scale ?? 1,
      });
      if (handle) {
        thing.handle?.remove();
        thing.handle = handle;
      }
    } else if (kind === 'level') {
      // The level's object stays until the drag ends (then your copy stands there): a
      // see-through preview of its mesh follows the arrow meanwhile.
      if (thing.object.kind === 'entity' && thing.object.guid) {
        // An entity moves itself while dragged (its edit; the drag's end makes the step).
        const kept = { at: thing.at, turn: thing.turn };
        thing.at = { ...at };
        thing.turn = turn;
        this.sendEntityPose(thing);
        thing.at = kept.at;
        thing.turn = kept.turn;
      } else {
        this.dragPreview(drag, thing, at, turn);
      }
    }
  }

  // The see-through preview of a level object dragged with the arrows, made once a drag.
  dragPreview(drag, thing, at, turn = thing.turn) {
    const { PropGhost, Vector3 } = this.api;
    if (!drag.ghost && PropGhost?.editorPreview && !drag.ghostFailed) {
      try {
        drag.ghost = PropGhost.editorPreview({
          models: [thing.object.name],
          offsets: [new Vector3(0, 0, 0)],
          rotations: [{ x: 0, y: 0, z: 0, w: 1 }],
          position: new Vector3(at.x, at.y, at.z),
        });
      } catch {
        drag.ghostFailed = true;
      }
    }
    const half = ((turn ?? 0) * Math.PI) / 360;
    drag.ghost?.setHidden?.(false);
    drag.ghost?.setPose?.(new Vector3(at.x, at.y, at.z), {
      x: 0,
      y: 0,
      z: Math.sin(half),
      w: Math.cos(half),
    });
  }

  // The brush's ring on the ground around `at` as the camera sees it: RING_POINTS points of the
  // circle at its height, put on the stage (drawn by the shell under the panels).
  showRing(at) {
    this.ringAt = at && this.api.NoClip.toScreen ? { x: at.x, y: at.y, z: at.z } : null;
    this.ringPoints = this.ringAt ? new Array(RING_POINTS).fill(null) : null;
  }

  // The marks and the brush ring as the camera sees them, drawn by the shell under the panels.
  drawOverlay() {
    if (!this.shell) {
      return;
    }
    const world = this.marks.lines();
    if (this.ringAt) {
      const { x, y, z } = this.ringAt;
      for (let index = 0; index < RING_POINTS; index += 1) {
        const a = (index / RING_POINTS) * Math.PI * 2;
        const b = ((index + 1) / RING_POINTS) * Math.PI * 2;
        world.push({
          from: {
            x: x + Math.cos(a) * this.brushSize,
            y: y + Math.sin(a) * this.brushSize,
            z: z + RING_LIFT,
          },
          to: {
            x: x + Math.cos(b) * this.brushSize,
            y: y + Math.sin(b) * this.brushSize,
            z: z + RING_LIFT,
          },
          color: RING_COLOR,
          width: 2,
        });
      }
    }
    if (this.api.Gizmos?.set) {
      this.drawMarksInWorld(world);
      // The collision shapes and zones too, cut to the view as the camera moves (only what changed
      // goes to the game).
      this.collision.view ??= {
        inView: (point) => this.insideView(point),
        clip: (line) => this.inView(line),
      };
      this.collision.draw();
      return;
    }
    const lines = [];
    for (const line of world) {
      const from = this.stagePoint(line.from);
      const to = this.stagePoint(line.to);
      if (from && to) {
        lines.push({ from, to, color: line.color, width: line.width });
      }
    }
    this.shell.drawLines?.(lines);
  }

  // Whether a world point shows in the world view (between the panels).
  insideView(point) {
    const { view } = LAYOUT;
    const stage = this.stagePoint(point);
    return (
      stage !== null &&
      stage.x >= view.x &&
      stage.x <= view.x + view.width &&
      stage.y >= view.y &&
      stage.y <= view.y + view.height
    );
  }

  // The parts of a world line that the world view shows (between the panels), as world lines:
  // the line looked at in CLIP_SAMPLES steps, each edge found between a step in and one out.
  inView(line) {
    const pointAt = (t) => ({
      x: line.from.x + (line.to.x - line.from.x) * t,
      y: line.from.y + (line.to.y - line.from.y) * t,
      z: line.from.z + (line.to.z - line.from.z) * t,
    });
    const inside = (t) => this.insideView(pointAt(t));
    const edge = (outT, inT) => {
      let out = outT;
      let into = inT;
      for (let step = 0; step < CLIP_BISECTIONS; step += 1) {
        const middle = (out + into) / 2;
        if (inside(middle)) {
          into = middle;
        } else {
          out = middle;
        }
      }
      return into;
    };
    const flags = [];
    for (let step = 0; step <= CLIP_SAMPLES; step += 1) {
      flags.push(inside(step / CLIP_SAMPLES));
    }
    if (flags.every(Boolean)) {
      return [line];
    }
    const parts = [];
    let start = null;
    for (let step = 0; step <= CLIP_SAMPLES; step += 1) {
      const t = step / CLIP_SAMPLES;
      const before = (step - 1) / CLIP_SAMPLES;
      if (flags[step] && start === null) {
        start = step === 0 ? 0 : edge(before, t);
      }
      const last = step === CLIP_SAMPLES;
      if (start !== null && (!flags[step] || last)) {
        const end = flags[step] ? 1 : edge(t, before);
        if (end > start) {
          parts.push({ ...line, from: pointAt(start), to: pointAt(end) });
        }
        start = null;
      }
    }
    return parts;
  }

  // The marks as the game's own world drawings (Gizmos), drawn each frame from the camera of that
  // frame, so they stay on the thing while the camera moves (the axes swam
  // until the camera stopped, drawn on the stage from a camera a few frames old). Sent again only
  // when they change; a drawing per colour and width, in the world's lines, seen through walls.
  drawMarksInWorld(world) {
    // Only what the world view shows: the game draws them over the whole window, panels too
    const shown = [];
    for (const line of world) {
      shown.push(...this.inView(line));
    }
    const signature = JSON.stringify(shown, (key, value) =>
      typeof value === 'number' ? Math.round(value * 1000) / 1000 : value,
    );
    if (signature === this.marksSignature) {
      return;
    }
    this.marksSignature = signature;
    const groups = [];
    const current = new Map();
    for (const line of shown) {
      const key = `${line.color}:${line.width}`;
      let group = current.get(key);
      if (!group || group.points.length + 3 > MARK_POINTS) {
        group = { color: line.color, width: line.width, points: [], indices: [] };
        groups.push(group);
        current.set(key, group);
      }
      const first = group.points.length;
      // A line as a flat triangle (from, to, to): its wireframe draws the line (a mesh has at least
      // three points).
      group.points.push(line.from, line.to, line.to);
      group.indices.push(first, first + 1, first + 2);
    }
    let count = 0;
    for (const group of groups) {
      this.api.Gizmos.set(
        `${MARK_GIZMO}${count}`,
        { type: 'mesh', points: group.points, indices: group.indices, wireframe: true },
        {
          color: (0xff000000 | group.color) >>> 0,
          width: Math.min(8, Math.max(0.5, group.width)),
          depthTest: false,
        },
      );
      count += 1;
    }
    for (let index = count; index < (this.marksDrawn ?? 0); index += 1) {
      this.api.Gizmos.remove(`${MARK_GIZMO}${index}`);
    }
    this.marksDrawn = count;
  }

  // The cursor and what it points at, as the game client saw them this frame.
  cursorAt(data) {
    const x = Number(data?.x);
    const y = Number(data?.y);
    if (!Number.isFinite(x) || !Number.isFinite(y)) {
      return;
    }
    let hit = data.hit ? this.api.hitOf?.(data.hit) ?? null : null;
    // The thing's box in the world, when the game client knows it (a level house, a door).
    const box = data.hit?.box;
    if (hit && Array.isArray(box) && box.length === 6 && box.every(Number.isFinite)) {
      hit = { ...hit, box };
    }
    const window = data.window;
    if (Array.isArray(window) && window.length === 2 && window.every(Number.isFinite)) {
      this.windowSized(window[0], window[1]);
    }
    const at = this.fromGameStage(x, y);
    this.nativeCursor = { x: at.x, y: at.y, over: data.over === true, hit };
  }

  // The cursor's place over the world view in stage pixels, or null when it is not over it.
  cursorOverView() {
    const native = this.testCursor ? null : this.nativeCursor;
    if (native) {
      const { view } = LAYOUT;
      const over =
        native.over &&
        native.x >= view.x &&
        native.x < view.x + view.width &&
        native.y >= view.y &&
        native.y < view.y + view.height;
      return over ? { x: native.x, y: native.y } : null;
    }
    const root = this.screen?.root;
    if (!root && !this.testCursor) {
      return null;
    }
    // A test script's 'hover x y' stands in for the mouse (the test hooks only).
    const mouse = this.testCursor
      ? null
      : this.fromGameStage(root.get('_xmouse'), root.get('_ymouse'));
    const x = this.testCursor?.x ?? mouse.x;
    const y = this.testCursor?.y ?? mouse.y;
    const { view } = LAYOUT;
    const over =
      typeof x === 'number' &&
      typeof y === 'number' &&
      x >= view.x &&
      x < view.x + view.width &&
      y >= view.y &&
      y < view.y + view.height;
    return over ? { x, y } : null;
  }

  // The box around what a press at the cursor would select, and its name beside the cursor.
  markTarget() {
    const cursor = this.tool === 'select' ? this.cursorOverView() : null;
    const hit = cursor ? this.pointAt(cursor.x, cursor.y) : null;
    if (!hit) {
      this.marks.target(null);
      this.setHover(null);
      return;
    }
    const at = { x: hit.position.x, y: hit.position.y, z: hit.position.z };
    const thing = this.thingAt(at, hit);
    this.marks.target(thing && thing !== this.selected ? this.boxOf(thing) : null);
    this.setHover(thing ? { x: cursor.x, y: cursor.y, thing } : null);
  }

  // The name of what is under the cursor, shown beside it.
  // Drawn again when the thing changes or the cursor moved HOVER_MOVE.
  setHover(hover) {
    const before = this.hover ?? null;
    const label = hover ? this.nameOf(hover.thing) : '';
    if (!hover && !before) {
      return;
    }
    const same =
      hover &&
      before &&
      before.label === label &&
      Math.hypot(before.x - hover.x, before.y - hover.y) < HOVER_MOVE;
    if (same) {
      return;
    }
    if (!hover) {
      this.hover = null;
    } else {
      const { thing } = hover;
      let kind = 'Yours';
      if (thing.level) {
        kind = thing.object.kind === 'entity' ? 'Level entity' : 'Level object';
        if (thing.object.unseen) {
          kind = 'Level collision, drawn nowhere';
        }
      }
      this.hover = { x: hover.x, y: hover.y, label, kind };
    }
    this.refresh();
  }

  // A prefab's pieces, asked of the game once.
  piecesOf(name) {
    if (!this.pieceCache.has(name)) {
      this.pieceCache.set(name, this.api.prefabPieces?.(name) ?? []);
    }
    return this.pieceCache.get(name);
  }

  // The box of a thing around its foot, in its own turn: { at, turn, min, max }.
  boxOf(thing) {
    // A level object whose box the game client gave (upright, in the world's axes): its own box in
    // its turn when it is turned about the up axis only (the box of a turned
    // door stood far out of it), else that box around its place, upright.
    if (thing.level && thing.box) {
      const [minX, minY, minZ, maxX, maxY, maxZ] = thing.box;
      const { x, y, z } = thing.at;
      const own = ownBoxOf(thing.box, thing.object?.rotation, thing.at);
      if (own) {
        return { ...own, tilts: this.tiltsAsCopy(thing) };
      }
      return {
        at: thing.at,
        turn: 0,
        min: [minX - x, minY - y, minZ - z],
        max: [maxX - x, maxY - y, maxZ - z],
        tilts: this.tiltsAsCopy(thing),
      };
    }
    const kind = thing.level ? 'level' : thing.serial !== undefined ? 'shape' : thing.entry.source;
    if (kind === 'shape') {
      const { x, y, z } = thing.size;
      return {
        at: thing.ground,
        turn: thing.turn,
        min: [-x / 2, -y / 2, 0],
        max: [x / 2, y / 2, z],
      };
    }
    let min = [-1.5, -1.5, 0];
    let max = [1.5, 1.5, 3];
    const tree = (thing.level ? thing.object.name : thing.entry.name).includes('/trees/');
    if (kind === 'vegetation' || (kind === 'level' && thing.kind === 'vegetation')) {
      min = tree ? [-1.5, -1.5, 0] : [-1, -1, 0];
      max = tree ? [1.5, 1.5, 9] : [1, 1, 1.6];
    } else if (kind === 'prop') {
      const box = this.propBox(thing.entry.name);
      min = box ? box.min : [-0.5, -0.5, 0];
      max = box ? box.max : [0.5, 0.5, 1];
    } else if (kind === 'prefab') {
      const pieces = this.piecesOf(thing.entry.name);
      if (pieces.length > 0) {
        // The pieces' places with room for the meshes around them (a wall piece's middle is
        // its place), as tall as the highest piece's top and at least BUILDING_HEIGHT.
        min = [Infinity, Infinity, 0];
        max = [-Infinity, -Infinity, BUILDING_HEIGHT];
        for (const piece of pieces) {
          const [px, py, pz] = piece.offset;
          min = [Math.min(min[0], px - PIECE_ROOM), Math.min(min[1], py - PIECE_ROOM), 0];
          max = [
            Math.max(max[0], px + PIECE_ROOM),
            Math.max(max[1], py + PIECE_ROOM),
            Math.max(max[2], pz + 3),
          ];
        }
      } else {
        min = [-4, -4, 0];
        max = [4, 4, 6];
      }
    }
    const scale = thing.scale ?? 1;
    return {
      at: thing.at,
      turn: thing.turn ?? 0,
      min: min.map((value) => value * scale),
      max: max.map((value) => value * scale),
      // Props (and the level's objects as your copies) tilt on the Rotate tool's X and Y rings.
      tilts: kind === 'prop' || this.tiltsAsCopy(thing),
    };
  }

  // The preview of an armed item: a tree's or bush's mesh, or a prefab's brushes and models.
  makeGhost(entry, position) {
    this.dropGhost();
    this.ghostOf = entry;
    const { PropGhost } = this.api;
    if (!PropGhost?.editorPreview) {
      return;
    }
    let pieces = [{ mesh: entry.name, offset: [0, 0, 0], rotation: [0, 0, 0, 1] }];
    if (entry.source === 'prefab') {
      pieces = this.piecesOf(entry.name);
    }
    const known = pieces.slice(0, 16);
    if (known.length === 0) {
      return;
    }
    const { Vector3 } = this.api;
    try {
      this.ghost = PropGhost.editorPreview({
        models: known.map((piece) => piece.mesh),
        offsets: known.map((piece) => new Vector3(...piece.offset)),
        rotations: known.map((piece) => {
          const [x, y, z, w] = piece.rotation;
          return { x, y, z, w };
        }),
        position: new Vector3(position.x, position.y, position.z),
      });
    } catch (error) {
      this.api.log(`World Architect: no preview of ${entry.name}: ${error.message}`);
      this.ghost = null;
    }
  }

  dropGhost() {
    this.ghost?.destroy();
    this.ghost = null;
    this.ghostOf = null;
    this.ghostPose = null;
  }

  useToolAt(at) {
    if (this.tool === 'select') {
      this.pick(at);
      return;
    }
    const kind = this.selectionKind();
    if (!kind) {
      // Nothing selected yet: a press with Move, Rotate or Scale selects what is there
      // (with Move on, a gate could not be selected).
      this.pick(at);
      return;
    }
    if (this.tool === 'move') {
      const spot = { x: snap(at.x, this.gridStep), y: snap(at.y, this.gridStep), z: at.z };
      this.moveSelected(spot, this.selected.turn);
    } else if (this.tool === 'rotate') {
      this.moveSelected(this.selectedAt(), degreesOf(this.selected.turn + this.angleStep));
    } else if (this.tool === 'scale') {
      const undoAt = this.undoStack.length;
      this.scaleSelected(1);
      if (this.api.Key.isDown('mouse1')) {
        const cursor = this.testCursor ?? this.nativeCursor;
        this.scaleHold = {
          undoAt,
          x: cursor?.x ?? 0,
          y: cursor?.y ?? 0,
          applied: 0,
          dragged: false,
          next: Date.now() + SCALE_HOLD_DELAY_MS,
        };
      }
    }
  }

  // While the Scale tool's press is held:
  // held still, it grows a step every SCALE_REPEAT_MS; moved, it grows to the right or
  // up and shrinks to the left or down, a step every SCRUB_PX. Let go, all of it one undo step.
  scaleHoldOn() {
    const hold = this.scaleHold;
    if (!hold) {
      return;
    }
    if (!this.api.Key.isDown('mouse1') || !this.selected) {
      this.scaleHold = null;
      this.mergeSteps(hold.undoAt);
      this.refresh();
      return;
    }
    const cursor = this.testCursor ?? this.nativeCursor;
    const moved = cursor ? cursor.x - hold.x + (hold.y - cursor.y) : 0;
    if (Math.abs(moved) >= SCRUB_PX) {
      hold.dragged = true;
    }
    if (hold.dragged) {
      const wanted = Math.trunc(moved / SCRUB_PX);
      while (hold.applied !== wanted) {
        const dir = Math.sign(wanted - hold.applied);
        this.scaleSelected(dir);
        hold.applied += dir;
      }
    } else if (Date.now() >= hold.next) {
      this.scaleSelected(1);
      hold.next = Date.now() + SCALE_REPEAT_MS;
    }
    this.refresh();
  }

  scaleSelected(dir = 1) {
    const kind = this.selectionKind();
    if (kind === 'level') {
      this.scaleLevel(this.selected, dir);
      return;
    }
    if (kind === 'vegetation' || kind === 'prop') {
      this.resizeSelected(dir);
      return;
    }
    if (kind !== 'shape') {
      const what = {
        prefab: 'Buildings keep the size the game built them at',
        effect: 'Effects keep the size the game made them at',
        light: 'A light grows with its Radius in Details',
      };
      this.message = `${what[kind] ?? 'This keeps its size'} (plants, props and collision scale).`;
      return;
    }
    const shape = this.selected;
    const size = {
      x: this.collision.resized(shape, 'x', dir).x,
      y: this.collision.resized(shape, 'y', dir).y,
      z: this.collision.resized(shape, 'z', dir).z,
    };
    this.changeShape('scale', () => ({ size }));
    this.message = `Scaled ${this.nameOf(shape)}`;
  }

  // A plant's or prop's size up or down a step (SCALE_STEP), as one undo step; a plant within the
  // game's sizes (Vegetation.plant: 1/64 to 255/64), a prop within 0.1 to 10.
  resizeSelected(dir) {
    const thing = this.selected;
    const kind = this.selectionKind();
    const before = thing.scale ?? 1;
    const [least, most] = kind === 'vegetation' ? [0.25, 3.9] : [0.1, 10];
    const after =
      Math.round(Math.min(most, Math.max(least, before * SCALE_STEP ** dir)) * 100) / 100;
    if (after === before) {
      this.message = `${this.nameOf(thing)} is already as ${
        dir > 0 ? 'large' : 'small'
      } as it goes.`;
      return;
    }
    const apply = (value) => {
      thing.scale = value;
      if (kind === 'prop') {
        thing.handle?.setScale(value, thing.at, thing.turn);
        return true;
      }
      const handle = this.api.Vegetation.plant(thing.entry.name, thing.at, {
        turn: thing.turn,
        scale: value,
      });
      if (!handle) {
        return false;
      }
      thing.handle?.remove();
      thing.handle = handle;
      return true;
    };
    this.run({ label: 'scale', redo: () => apply(after), undo: () => apply(before) });
    this.message = `${this.nameOf(thing)}: size ×${after}`;
  }

  place(at) {
    const entry = this.armed;
    const { Prefab, Terrain, Vegetation, Vector3 } = this.api;
    const record = { entry, at: { ...at }, turn: this.turn, handle: null, pad: null };
    record.tilt = this.placeTilt ?? null;
    const command = {
      label: entry.label,
      redo: () => {
        if (entry.source === 'prefab') {
          if (this.fitGround) {
            record.pad = this.levelPad(record);
          }
          const position = new Vector3(record.at.x, record.at.y, record.at.z);
          record.handle = Prefab.place(entry.name, position, { x: 0, y: 0, z: record.turn });
        } else if (entry.source === 'light') {
          record.light ??= lightOfPreset(entry.name);
          record.handle = this.lightHandle(record);
        } else if (entry.source === 'sound') {
          record.handle = this.soundHandle(entry.name, record.at);
        } else if (entry.source === 'npc') {
          record.handle = this.npcHandle(record);
        } else if (entry.source === 'effect') {
          record.fx ??= effectDefaults(entry.name);
          record.handle = this.effectHandle(entry.name, record.at, record.turn, record.fx);
        } else if (entry.source === 'prop') {
          record.handle = this.propHandle(entry.name, record.at, record.turn, record.tilt);
        } else {
          record.handle = Vegetation.plant(entry.name, record.at, { turn: record.turn });
        }
        if (!record.handle) {
          record.pad?.undo();
          return false;
        }
        if (entry.source === 'prefab') {
          this.coverGrass(record);
        }
        this.placed.push(record);
        this.select(record);
        return true;
      },
      undo: () => {
        this.takeAway(record);
      },
    };
    this.message = this.run(command)
      ? `Placed ${entry.label} at ${at.x.toFixed(1)} ${at.y.toFixed(1)}`
      : `The game could not place ${entry.label} there.`;
  }

  takeAway(record) {
    this.uncoverGrass(record);
    record.handle?.destroy?.();
    record.handle?.remove?.();
    record.handle = null;
    record.pad?.undo();
    record.pad = null;
    this.placed = this.placed.filter((item) => item !== record);
    if (this.selected === record) {
      this.select(null);
    }
  }

  // One ground edit, kept in the project's list; null when the game would not make it.
  applyTerrain(mode, at, size, strength) {
    const { Terrain } = this.api;
    let handle = null;
    const material = paintOf(mode);
    if (material) {
      // Full in the inner half, blended into the ground around over the outer half.
      handle = Terrain.paint(at, size / 2, material, size / 2);
    } else if (mode === 'flatten') {
      handle = Terrain.flatten(at, size, at.z, size);
    } else {
      handle = Terrain.raise(at, size, mode === 'raise' ? strength : -strength, size);
    }
    if (!handle) {
      return null;
    }
    const edit = { mode, at: { ...at }, size, strength, handle };
    this.terrainEdits.push(edit);
    return edit;
  }

  // A press in Terrain mode starts a stroke: its first dab now, more while the left button is held
  // (poll, strokeOn), all of them one undo step. Flatten levels the whole stroke to the height
  // where it started.
  shapeGround(at) {
    const mode = this.terrainMode;
    const stroke = {
      mode,
      size: this.brushSize,
      strength: this.brushStrength,
      height: at.z,
      dabs: [],
      edits: [],
      last: Date.now(),
    };
    const command = {
      label: `terrain ${mode}`,
      redo: () => {
        stroke.edits = stroke.dabs.map((dab) => this.dab(stroke, dab)).filter(Boolean);
        return stroke.edits.length > 0;
      },
      undo: () => {
        for (const edit of [...stroke.edits].reverse()) {
          if (edit.undo) {
            edit.undo();
          } else {
            edit.handle.undo();
            edit.followUndo?.();
          }
        }
        this.terrainEdits = this.terrainEdits.filter((item) => !stroke.edits.includes(item));
      },
    };
    stroke.dabs.push({ ...at });
    if (!this.run(command)) {
      this.message = 'The ground could not be shaped there.';
      if (mode === 'nature' && this.plantCount() >= PLANTS_MOST) {
        this.message = `No more plants: the editor holds ${PLANTS_MOST}. Clear nature somewhere first.`;
      }
      return;
    }
    this.stroke = stroke;
    this.message = `Terrain ${mode}: ${stroke.size} m at ${at.x.toFixed(1)} ${at.y.toFixed(1)}`;
  }

  // One dab of a stroke; the editor's own plants within it moved onto the new ground.
  dab(stroke, at) {
    if (stroke.mode === 'clear') {
      return this.clearNature(at, stroke.size);
    }
    if (stroke.mode === 'nature') {
      return this.addNature(at, stroke.size);
    }
    if (paintOf(stroke.mode)) {
      // The ground's look only: nothing on it moves.
      const painted = this.applyTerrain(stroke.mode, at, stroke.size, stroke.strength);
      this.sightChanged();
      return painted;
    }
    const spot = stroke.mode === 'flatten' ? { ...at, z: stroke.height } : at;
    const started = Date.now();
    const followers = this.groundFollowers(at, stroke.size);
    const asked = Date.now();
    const edit = this.applyTerrain(stroke.mode, spot, stroke.size, stroke.strength);
    const shaped = Date.now();
    this.sightChanged();
    if (edit) {
      edit.followUndo = this.followGround(followers);
    }
    const took = Date.now() - started;
    if (took > SLOW_DAB_MS) {
      this.api.log(
        `World Architect: ${stroke.mode} dab ${took} ms (what follows ${asked - started} ms, ` +
          `${followers.things.length} things; ground ${shaped - asked} ms; following ` +
          `${Date.now() - shaped} ms)`,
      );
    }
    return edit;
  }

  // While the left button stays down over the view, the stroke goes on where the cursor is.
  strokeOn() {
    const stroke = this.stroke;
    if (!stroke) {
      return;
    }
    if (this.mode !== 'terrain' || !this.api.Key.isDown('mouse1')) {
      this.stroke = null;
      if (stroke.dabs.length > 1) {
        this.message = `Terrain ${stroke.mode}: ${stroke.dabs.length} dabs, one step`;
        this.refresh();
      }
      return;
    }
    const now = Date.now();
    if (now - stroke.last < STROKE_MS) {
      return;
    }
    const cursor = this.cursorOverView();
    const hit = cursor ? this.pointAt(cursor.x, cursor.y) : null;
    if (!hit) {
      return;
    }
    const at = { x: hit.position.x, y: hit.position.y, z: hit.position.z };
    const previous = stroke.dabs[stroke.dabs.length - 1];
    if (Math.hypot(at.x - previous.x, at.y - previous.y) < stroke.size * STROKE_SPACING) {
      return;
    }
    stroke.last = now;
    const edit = this.dab(stroke, at);
    if (edit) {
      stroke.dabs.push(at);
      stroke.edits.push(edit);
    }
  }

  // A Clear nature dab: the level's own plants within `size` metres hidden in this game, and the
  // editor's own plants there taken away. Its undo shows and plants them again.
  clearNature(at, size) {
    const started = Date.now();
    const plants = this.api.levelPlants?.(at, size, NATURE_MOST) ?? [];
    const asked = Date.now();
    const hidden = [];
    // What is hidden already, by mesh and place (a look through thousands for each plant held
    // the editor up while nature was cleared).
    const keyOf = (mesh, place) =>
      `${mesh}@${Math.round(place.x * 10)},${Math.round(place.y * 10)}`;
    const hiddenKeys = new Set(
      this.hiddenLevel.map((thing) => keyOf(thing.object.name, thing.object.at)),
    );
    for (const plant of plants) {
      const object = { kind: 'brush', name: plant.mesh, guid: '', class: '', at: plant.at };
      if (hiddenKeys.has(keyOf(plant.mesh, plant.at))) {
        continue;
      }
      const thing = {
        level: true,
        object,
        kind: 'vegetation',
        at: { ...plant.at },
        turn: 0,
        hidden: false,
        edit: 0,
      };
      this.hideLevel(thing);
      hidden.push(thing);
    }
    const own = this.placed.filter((record) => {
      const near = Math.hypot(record.at.x - at.x, record.at.y - at.y) <= size;
      return record.entry.source === 'vegetation' && near;
    });
    // Your own plants there go in one question to the game (Add nature plants thousands: one
    // question each held the editor a frame each).
    const hiding = Date.now();
    this.takeAwayAll(own);
    // The level's grass carpet there (merged meshes, terrain/merged_grass.hpp).
    const grass = this.hideGrass(at, size);
    const took = Date.now() - started;
    if (took > SLOW_DAB_MS) {
      this.api.log(
        `World Architect: Clear nature dab ${took} ms (asking ${asked - started} ms, ` +
          `${plants.length} level plants ${hiding - asked} ms, ${own.length} of yours ` +
          `${Date.now() - hiding} ms)`,
      );
    }
    return {
      undo: () => {
        this.showGrass(grass);
        for (const thing of hidden) {
          this.api.send({ op: 'localLevelEdit', kind: 'remove', id: thing.edit });
          thing.hidden = false;
        }
        const shown = new Set(hidden);
        this.hiddenLevel = this.hiddenLevel.filter((thing) => !shown.has(thing));
        this.restoreAll(own);
        this.select(null);
      },
    };
  }

  // The level's grass carpet hidden in a circle; kept in the project.
  hideGrass(at, radius) {
    const patch = { id: this.nextGrass, x: at.x, y: at.y, radius };
    this.nextGrass += 1;
    this.api.send({ op: 'architectGrass', kind: 'hide', ...patch });
    this.grassPatches.push(patch);
    return patch;
  }

  showGrass(patch) {
    this.api.send({ op: 'architectGrass', kind: 'show', id: patch.id });
    this.grassPatches = this.grassPatches.filter((item) => item !== patch);
  }

  // An Add nature dab: the grass the ground around already grows (the level's grass carpet
  // under the brush, its kinds as often as they grow there), scattered within `size` metres on
  // the terrain and turned at random. Where the ground grows none, the plant last chosen in
  // Foliage, as many as the paint brush's count.
  addNature(at, size) {
    const kinds = this.grassKindsAt(at, size);
    let pick = null;
    let count = Math.max(1, this.natureCount);
    if (kinds.length > 0) {
      const total = kinds.reduce((sum, kind) => sum + kind.count, 0);
      pick = () => {
        let left = this.random() * total;
        for (const kind of kinds) {
          left -= kind.count;
          if (left <= 0) {
            return this.entryOf('vegetation', kind.mesh);
          }
        }
        return this.entryOf('vegetation', kinds[0].mesh);
      };
      const area = Math.PI * size * size;
      count = Math.min(GRASS_MOST, Math.max(1, Math.round(area * this.grassDensity)));
    } else {
      const entry = this.natureEntry();
      if (!entry) {
        return null;
      }
      pick = () => entry;
    }
    const spots = this.scatter(at, size, count).map((spot) => ({ ...spot, entry: pick() }));
    const handles = this.plantAll(spots);
    const records = [];
    spots.forEach((spot, index) => {
      if (handles[index]) {
        const record = { entry: spot.entry, at: spot.at, turn: spot.turn, handle: handles[index] };
        record.pad = null;
        records.push(record);
        this.placed.push(record);
      }
    });
    if (records.length === 0) {
      return null;
    }
    return {
      undo: () => this.takeAwayAll(records),
    };
  }

  // How many of your plants stand now.
  plantCount() {
    return this.placed.filter((record) => record.entry?.source === 'vegetation').length;
  }

  // The grass kinds the ground grows around `at`: asked again only a brush's width further on
  // (each question waits a game frame; a stroke dabs every STROKE_MS).
  grassKindsAt(at, size) {
    const known = this.grassKindsNear;
    if (known && known.size === size && Math.hypot(known.x - at.x, known.y - at.y) < size) {
      return known.kinds;
    }
    const kinds = (this.api.grassKinds?.(at, size) ?? []).filter((kind) => kind.count > 0);
    this.grassKindsNear = { x: at.x, y: at.y, size, kinds };
    return kinds;
  }

  // Plants each { entry, at, turn } in as few questions to the game as it takes; a handle or null
  // each.
  plantAll(spots) {
    const { Vegetation } = this.api;
    if (!this.api.plantMany) {
      return spots.map((spot) =>
        Vegetation.plant(spot.entry.name, spot.at, { turn: spot.turn, scale: spot.scale ?? 1 }),
      );
    }
    const handles = [];
    for (let first = 0; first < spots.length; first += PLANTS_AT_ONCE) {
      const part = spots.slice(first, first + PLANTS_AT_ONCE).map((spot) => ({
        kind: spot.entry.name,
        position: spot.at,
        turn: spot.turn,
        scale: spot.scale ?? 1,
      }));
      handles.push(...this.api.plantMany(part));
    }
    return handles;
  }

  // Plants many of your plants again at once (PLANTS_AT_ONCE a question), as restore does each.
  restoreAll(records) {
    const handles = this.plantAll(records);
    records.forEach((record, index) => {
      record.handle = handles[index] ?? null;
      if (record.handle) {
        this.placed.push(record);
      }
    });
  }

  // Takes many plants away at once (one question to the game), as takeAway does each.
  takeAwayAll(records) {
    if (!this.api.removeMany) {
      for (const record of records) {
        this.takeAway(record);
      }
      return;
    }
    this.api.removeMany(records.map((record) => record.handle));
    for (const record of records) {
      record.handle = null;
    }
    // A set: thousands of plants looked up in a list of thousands held the editor a second.
    const gone = new Set(records);
    this.placed = this.placed.filter((item) => !gone.has(item));
    if (gone.has(this.selected)) {
      this.select(null);
    }
  }

  // The plant Add nature scatters: the one last chosen in Foliage, else the first the game has
  // (asked once per opening: the panel asks for it each refresh, and the list has hundreds).
  natureEntry() {
    if (this.naturePlant) {
      return this.naturePlant;
    }
    if (this.firstPlant === undefined) {
      const first = this.api.Vegetation.kinds?.()[0];
      this.firstPlant = first ? this.entryOf('vegetation', first) : null;
    }
    return this.firstPlant;
  }

  // What stands within `size` metres of `at` and goes up and down with the ground there
  // ("lowering terrain, props should follow it"): your plants, props, effects and
  // lights, and the level's loose objects; with the ground under each before the change.
  groundFollowers(at, size) {
    const near = (point) => Math.hypot(point.x - at.x, point.y - at.y) <= size;
    const own = this.placed.filter(
      (record) => GROUND_FOLLOWERS.includes(record.entry.source) && near(record.at),
    );
    const level = (this.api.levelBrushes?.(at, size) ?? []).map((brush) =>
      this.groundMoveOf(brush),
    );
    const things = [
      ...own.map((record) => ({ record, at: record.at })),
      ...level.map((move) => ({ move, at: move.at })),
    ];
    const before = things.length > 0 ? this.groundsOf(things.map((thing) => thing.at)) : [];
    return { things, before };
  }

  // The ground move of a level brush where it stands now (one it was moved by already, or new).
  groundMoveOf(brush) {
    const known = this.groundMoves.find(
      (move) =>
        move.mesh === brush.mesh &&
        Math.hypot(move.at.x - brush.at.x, move.at.y - brush.at.y, move.at.z - brush.at.z) < 0.05,
    );
    return known ?? { mesh: brush.mesh, home: { ...brush.at }, at: { ...brush.at }, edit: 0 };
  }

  // Moves the followers by how much the ground under each went up or down; a function that moves
  // them back (the dab's undo), null when nothing moved.
  followGround({ things, before }) {
    if (things.length === 0) {
      return null;
    }
    const after = this.groundsOf(things.map((thing) => thing.at));
    const moved = [];
    things.forEach((thing, index) => {
      if (before[index] === null || after[index] === null) {
        return;
      }
      const delta = after[index] - before[index];
      if (Math.abs(delta) >= GROUND_FOLLOW_LEAST) {
        moved.push({ thing, delta });
      }
    });
    if (moved.length === 0) {
      return null;
    }
    this.shiftFollowers(moved, 1);
    return () => this.shiftFollowers(moved, -1);
  }

  // Each follower up or down by its delta times `sign`; plants planted again in one question.
  shiftFollowers(moved, sign) {
    const plants = [];
    for (const { thing, delta } of moved) {
      const lift = delta * sign;
      if (thing.move) {
        this.shiftLevelBrush(thing.move, lift);
        continue;
      }
      const record = thing.record;
      record.at = { ...record.at, z: record.at.z + lift };
      if (record.entry.source === 'vegetation') {
        plants.push(record);
      } else {
        record.handle?.setPose?.(record.at, { x: 0, y: 0, z: record.turn ?? 0 });
      }
    }
    if (plants.length > 0) {
      this.replantAll(plants);
    }
  }

  // A level brush up or down by `lift` metres: a level edit with its new place (none once it is
  // back where the level put it).
  shiftLevelBrush(move, lift) {
    move.at = { ...move.at, z: move.at.z + lift };
    if (Math.abs(move.at.z - move.home.z) < 0.001) {
      if (move.edit) {
        this.api.send({ op: 'localLevelEdit', kind: 'remove', id: move.edit });
      }
      move.edit = 0;
      this.groundMoves = this.groundMoves.filter((item) => item !== move);
      return;
    }
    if (!move.edit) {
      move.edit = this.nextLevelEdit;
      this.nextLevelEdit += 1;
      this.groundMoves.push(move);
    }
    this.sendGroundMove(move);
  }

  sendGroundMove(move) {
    this.api.send({
      op: 'localLevelEdit',
      kind: 'set',
      id: move.edit,
      object: 'brush',
      name: move.mesh,
      guid: '',
      at: { x: move.home.x, y: move.home.y, z: move.home.z },
      hidden: false,
      position: { x: move.at.x, y: move.at.y, z: move.at.z },
    });
  }

  // Your plants planted again where their records say (many in one question to the game).
  replantAll(records) {
    const old = records.map((record) => record.handle);
    const handles = this.plantAll(records);
    if (this.api.removeMany) {
      this.api.removeMany(old);
    } else {
      for (const handle of old) {
        handle?.remove?.();
      }
    }
    records.forEach((record, index) => {
      record.handle = handles[index] ?? null;
    });
  }

  addShape(at) {
    const layer = this.collision;
    layer.kind = this.armed.name;
    const shape = layer.make(at, this.turn);
    const label = SHAPE_KINDS[shape.kind].label.toLowerCase();
    const added = this.run({
      label,
      redo: () => {
        const built = layer.build(shape);
        if (built) {
          this.select(shape);
        }
        return built;
      },
      undo: () => {
        layer.unbuild(shape);
        if (this.selected === shape) {
          this.select(null);
        }
      },
    });
    this.message = added ? `Added ${label}` : `The game could not add a ${label} there.`;
  }

  // ---- Selection ----

  select(thing) {
    this.selected = thing;
    // Nothing selected: back to Select at once, from Move, Rotate or Scale.
    if (!thing && TRANSFORM_TOOLS.has(this.tool)) {
      this.tool = 'select';
    }
    this.collision.selected = this.selectionKind() === 'shape' ? thing : null;
    this.collision.draw();
  }

  // 'prefab', 'vegetation', 'shape', 'level' or null.
  selectionKind() {
    const thing = this.selected;
    if (!thing) {
      return null;
    }
    if (thing.level) {
      return 'level';
    }
    if (thing.serial !== undefined) {
      return 'shape';
    }
    return thing.entry.source;
  }

  selectedAt() {
    if (this.selectionKind() === 'shape') {
      return { ...this.selected.ground };
    }
    return { ...this.selected.at };
  }

  nameOf(thing) {
    if (thing.level) {
      const { name, class: entityClass, unseen } = thing.object;
      // The level's invisible collision (a building's own, left where the building was deleted).
      if (unseen) {
        return 'Invisible collision';
      }
      const file = name.slice(name.lastIndexOf('/') + 1).replace(/\.cgf$/, '');
      return file || entityClass || 'Level object';
    }
    if (thing.serial !== undefined) {
      return `${SHAPE_KINDS[thing.kind].label} ${thing.serial}`;
    }
    return thing.entry.label;
  }

  // What a press with Select picks: a collision shape of yours whose footprint holds it; else the
  // thing of yours the ray met; else the level's own object it met (or a light, effect or sound
  // of yours within OWN_REACH metres); else a thing of yours within PICK_RANGE of it.
  pick(at) {
    const thing = this.thingAt(at, this.lastHit);
    this.select(thing);
    this.message = thing ? `Selected ${this.nameOf(thing)}` : 'Nothing there to select.';
  }

  // What a press with Select at `at` (the ray's hit `hit`) would pick, or null.
  thingAt(at, hit) {
    const shape = this.collision.at(at);
    if (shape) {
      return shape;
    }
    // What the ray met, when it is a thing of yours (your prop, house, plant or person).
    // A prop of yours the ray went through before it met something: a mesh the ray does not
    // meet (a press on a placed doorway took the level's wall behind it,
    // or the invisible collision of your church behind that).
    const crossed = this.propCrossed(at);
    if (crossed) {
      return crossed;
    }
    const own = this.ownThingHit(hit);
    if (own) {
      return own;
    }
    // The level's own object the ray met is taken over a thing of yours standing near it: only
    // what the ray cannot meet (a light, an effect, a sound) is found by its nearness then
    // (in a house a press on its table always took the house).
    const levelObject = hit?.levelObject ?? null;
    let best = null;
    let bestDistance = levelObject ? OWN_REACH : PICK_RANGE;
    for (const record of this.placed) {
      if (levelObject && !UNMET_SOURCES.has(record.entry.source)) {
        continue;
      }
      const distance = Math.hypot(record.at.x - at.x, record.at.y - at.y);
      if (distance < bestDistance) {
        best = record;
        bestDistance = distance;
      }
    }
    if (!best && levelObject) {
      best = this.levelThing(hit);
    }
    return best;
  }

  // The prop of yours whose box (boxOf, turned about the up axis) the line from the camera to `at`
  // enters first, or null. Only props: a house's box is rough, and a press inside it on the
  // level's table must take the table.
  propCrossed(at) {
    const camera = this.api.NoClip.getPosition?.();
    if (!camera) {
      return null;
    }
    const line = { x: at.x - camera.x, y: at.y - camera.y, z: at.z - camera.z };
    const length = Math.hypot(line.x, line.y, line.z);
    if (!(length > 0)) {
      return null;
    }
    let best = null;
    let bestEntry = 1 + CROSS_PAST / length;
    for (const record of this.placed) {
      if (record.entry.source !== 'prop') {
        continue;
      }
      const box = this.boxOf(record);
      // A box this large (a church's piece, its collision) holds what is in it: the ray meets
      // that, and the prop by its own mesh.
      const size = Math.hypot(...box.max.map((value, axis) => value - box.min[axis]));
      if (size > CROSS_LARGEST) {
        continue;
      }
      const entry = boxEntry(camera, line, box);
      // Not a box the camera is in: every press would take it.
      if (entry !== null && entry > 0 && entry < bestEntry) {
        best = record;
        bestEntry = entry;
      }
    }
    return best;
  }

  // The thing of yours a hit met, or null: your prop, house or person by the name the game client
  // gives its entity (kcds_local_prop_<id>, kcds_local_prefab_<id>, kcds_local_npc_<id>); your
  // plant, or a piece of your house the game made a brush, by its mesh where it stands.
  ownThingHit(hit) {
    // The game client names the entity in the hit, or (a cursor hit) only in its level object.
    const levelName = hit?.levelObject?.kind === 'entity' ? hit.levelObject.name : '';
    const named = /^kcds_local_(prop|prefab|npc)_(\d+)$/.exec(hit?.entityName || levelName || '');
    if (named) {
      const id = Number(named[2]);
      return (
        this.placed.find((record) => {
          if (record.entry.source !== named[1]) {
            return false;
          }
          return named[1] === 'npc' ? record.npcId === id : record.handle?.id === id;
        }) ?? null
      );
    }
    const object = hit?.levelObject;
    if (!object || object.kind !== 'brush' || !object.name) {
      return null;
    }
    const mesh = meshKey(object.name);
    for (const record of this.placed) {
      const { source, name } = record.entry;
      if (source === 'vegetation' && meshKey(name) === mesh) {
        const near = Math.hypot(record.at.x - object.at.x, record.at.y - object.at.y);
        if (near < OWN_PLACE) {
          return record;
        }
      }
      if (source === 'prefab' && this.isPieceOf(record, mesh, object.at)) {
        return record;
      }
    }
    return null;
  }

  // Whether a brush with mesh `mesh` at `at` is one of your house's pieces where it stands.
  isPieceOf(record, mesh, at) {
    const half = ((record.turn ?? 0) * Math.PI) / 180;
    const cos = Math.cos(half);
    const sin = Math.sin(half);
    return this.piecesOf(record.entry.name).some((piece) => {
      if (meshKey(piece.mesh) !== mesh || !Array.isArray(piece.offset)) {
        return false;
      }
      const [dx, dy, dz] = piece.offset;
      const x = record.at.x + dx * cos - dy * sin;
      const y = record.at.y + dx * sin + dy * cos;
      const z = record.at.z + dz;
      return Math.hypot(x - at.x, y - at.y, z - at.z) < OWN_PLACE;
    });
  }

  // The level's own object a hit names, as a selection; the one already hidden when it is.
  levelThing(hit) {
    const object = hit.levelObject;
    const same = this.hiddenLevel.find((thing) => {
      const there = thing.object.at;
      const near = Math.hypot(there.x - object.at.x, there.y - object.at.y, there.z - object.at.z);
      return thing.object.name === object.name && near < 0.05;
    });
    if (same) {
      return same;
    }
    return {
      level: true,
      object,
      box: hit.box ?? null,
      kind: hit.kind,
      at: { x: object.at.x, y: object.at.y, z: object.at.z },
      turn: yawOf(object.rotation),
      hidden: false,
      edit: 0,
    };
  }

  // Hides the level's own object in this game, or shows it again, as one step.
  setLevelHidden(thing, hidden) {
    const name = this.nameOf(thing);
    const apply = (hide) => {
      if (hide) {
        this.hideLevel(thing);
        return true;
      }
      this.showLevel(thing);
      return true;
    };
    this.run({
      label: `${hidden ? 'hide' : 'show'} ${name}`,
      redo: () => apply(hidden),
      undo: () => apply(!hidden),
    });
    this.message = `${hidden ? 'Hidden' : 'Shown again'}: ${name}`;
  }

  // Hides a level object in this game (a local level edit) and lists it.
  hideLevel(thing) {
    if (this.movedLevel.includes(thing)) {
      // Moved where it stands: hidden there, under the same edit.
      thing.hidden = true;
      this.sendEntityPose(thing);
      if (!this.hiddenLevel.includes(thing)) {
        this.hiddenLevel.push(thing);
      }
      return;
    }
    thing.edit = this.nextLevelEdit;
    this.nextLevelEdit += 1;
    const object = thing.object;
    this.api.send({
      op: 'localLevelEdit',
      kind: 'set',
      id: thing.edit,
      object: object.kind,
      name: object.name,
      guid: object.guid ?? '',
      at: { x: object.at.x, y: object.at.y, z: object.at.z },
      hidden: true,
    });
    if (thing.kind === 'vegetation') {
      // Ground clutter (sticks, stones, small plants) is drawn in the level's grass carpet, with no
      // render node of its own to hide (Del left marsh sticks standing): the
      // carpet's instances right at it are hidden too, kept with the hidden object, not as a patch.
      thing.clutter = this.nextGrass;
      this.nextGrass += 1;
      const { x, y } = object.at;
      this.api.send({
        op: 'architectGrass',
        kind: 'hide',
        id: thing.clutter,
        x,
        y,
        radius: CLUTTER_RADIUS,
      });
    }
    thing.hidden = true;
    if (!this.hiddenLevel.includes(thing)) {
      this.hiddenLevel.push(thing);
    }
  }

  // Shows a hidden level object again (and the carpet's clutter at it).
  showLevel(thing) {
    if (this.movedLevel.includes(thing)) {
      thing.hidden = false;
      this.sendEntityPose(thing);
    } else {
      this.api.send({ op: 'localLevelEdit', kind: 'remove', id: thing.edit });
    }
    if (thing.clutter) {
      this.api.send({ op: 'architectGrass', kind: 'show', id: thing.clutter });
      thing.clutter = 0;
    }
    thing.hidden = false;
    this.hiddenLevel = this.hiddenLevel.filter((item) => item !== thing);
  }

  // A level object dragged with the arrows: it is hidden and your own copy stands at the new place
  // (a prop of its mesh, or a plant of its kind), selected, as one step. The level's own objects
  // do not move; their copies do (arrows on a level object did nothing).
  moveLevel(thing, at, turn = thing.turn) {
    const object = thing.object;
    if (object.kind === 'entity' && object.guid) {
      this.moveLevelEntity(thing, at, turn);
      return;
    }
    const plant = thing.kind === 'vegetation';
    const source = plant ? 'vegetation' : 'prop';
    if (!plant && !this.api.meshKnown?.(object.name)) {
      this.message =
        "This level object can be hidden but not moved: it is not a mesh of the game's catalog.";
      return;
    }
    const entry = this.entryOf(source, object.name);
    const record = { entry, at: { ...at }, turn, handle: null, pad: null };
    if ((thing.scale ?? 1) !== 1) {
      record.scale = thing.scale;
    }
    const { x, y, z, w } = object.rotation ?? { x: 0, y: 0, z: 0, w: 1 };
    record.tilt = plant ? null : { x, y, z, w };
    const wasHidden = thing.hidden;
    this.run({
      label: `move ${this.nameOf(thing)}`,
      redo: () => {
        record.handle = plant
          ? this.api.Vegetation.plant(entry.name, record.at, { turn: record.turn })
          : this.propHandle(entry.name, record.at, record.turn, record.tilt, record.scale ?? 1);
        if (!record.handle) {
          return false;
        }
        if (!thing.hidden) {
          this.hideLevel(thing);
        }
        this.placed.push(record);
        this.select(record);
        return true;
      },
      undo: () => {
        this.takeAway(record);
        if (!wasHidden) {
          this.showLevel(thing);
        }
        this.select(thing);
      },
    });
    this.message = `Moved ${this.nameOf(thing)}: your copy stands there, the level's is hidden.`;
  }

  // A level object's size up or down a step where it stands, by its level edit
  // ("scale the level's objects"): an entity or a building, a fence, a rock; the level's plants
  // keep the size the game gave them. One undo step; kept in the project.
  scaleLevel(thing, dir) {
    if (thing.kind === 'vegetation') {
      this.message = "The level's trees and plants keep their size (yours scale).";
      return;
    }
    const before = thing.scale ?? 1;
    const after = Math.round(Math.min(10, Math.max(0.1, before * SCALE_STEP ** dir)) * 100) / 100;
    if (after === before) {
      return;
    }
    const apply = (value) => {
      thing.scale = value;
      this.sendEntityPose(thing);
      if (this.atHome(thing)) {
        this.movedLevel = this.movedLevel.filter((item) => item !== thing);
      } else if (!this.movedLevel.includes(thing)) {
        this.movedLevel.push(thing);
      }
      this.changed();
      return true;
    };
    this.run({ label: 'scale', redo: () => apply(after), undo: () => apply(before) });
    this.message = `${this.nameOf(thing)}: size ×${after}`;
  }

  // A level entity (a door, a cart, anything the level made with a GUID) moved where it stands, by
  // a level edit with its new place and turn: it keeps what it does (a door still swings), and Undo
  // puts it back (a door could not be moved, its copy hid a brush instead).
  moveLevelEntity(thing, at, turn) {
    const before = { at: { ...thing.at }, turn: thing.turn ?? 0 };
    const after = { at: { ...at }, turn };
    const apply = (pose) => {
      thing.at = { ...pose.at };
      thing.turn = pose.turn;
      this.sendEntityPose(thing);
      const home = this.atHome(thing);
      if (home) {
        this.movedLevel = this.movedLevel.filter((item) => item !== thing);
      } else if (!this.movedLevel.includes(thing)) {
        this.movedLevel.push(thing);
      }
      this.changed();
      return true;
    };
    this.run({
      label: `move ${this.nameOf(thing)}`,
      redo: () => apply(after),
      undo: () => apply(before),
    });
    this.message = `Moved ${this.nameOf(thing)} where it stands.`;
  }

  // Whether a level entity stands where the level put it, turned as the level turned it.
  atHome(thing) {
    const home = thing.object.at;
    const turned = Math.abs(degreesOf((thing.turn ?? 0) - yawOf(thing.object.rotation)));
    return (
      Math.hypot(thing.at.x - home.x, thing.at.y - home.y, thing.at.z - home.z) < 0.001 &&
      (turned < 0.01 || turned > 359.99) &&
      (thing.scale ?? 1) === 1
    );
  }

  // The level edit of a level entity: where it stands and its turn (from the level's rotation),
  // and whether it is hidden; none while it is the level's own again.
  sendEntityPose(thing) {
    const object = thing.object;
    if (!thing.edit) {
      thing.edit = this.nextLevelEdit;
      this.nextLevelEdit += 1;
    }
    if (this.atHome(thing) && !thing.hidden) {
      this.api.send({ op: 'localLevelEdit', kind: 'remove', id: thing.edit });
      return;
    }
    const base = object.rotation ?? { x: 0, y: 0, z: 0, w: 1 };
    const half = (((thing.turn ?? 0) - yawOf(base)) * Math.PI) / 360;
    const entity = object.kind === 'entity';
    // The level's own size times yours (an edit's scale is the whole size).
    const scale = (thing.scale ?? 1) === 1 ? {} : { scale: (object.scale?.x ?? 1) * thing.scale };
    this.api.send({
      op: 'localLevelEdit',
      kind: 'set',
      id: thing.edit,
      object: entity ? 'entity' : 'brush',
      name: object.name,
      guid: entity ? object.guid : '',
      at: { x: object.at.x, y: object.at.y, z: object.at.z },
      hidden: thing.hidden === true,
      // A brush moves as your copy (moveLevel); in place it only grows or shrinks.
      ...(entity
        ? {
            position: { x: thing.at.x, y: thing.at.y, z: thing.at.z },
            rotation: multiplied({ x: 0, y: 0, z: Math.sin(half), w: Math.cos(half) }, base),
          }
        : {}),
      ...scale,
    });
  }

  // Puts the selection at a place and turn as one step.
  moveSelected(at, turn) {
    const thing = this.selected;
    const kind = this.selectionKind();
    if (kind === 'level') {
      this.moveLevel(thing, at, turn);
      return;
    }
    const before = { at: this.selectedAt(), turn: thing.turn };
    const after = { at: { ...at }, turn };
    const apply = (pose) => {
      thing.turn = pose.turn;
      if (kind === 'shape') {
        thing.ground = { ...pose.at };
        this.collision.update(thing);
        return true;
      }
      thing.at = { ...pose.at };
      if (POSED_KINDS.has(kind)) {
        thing.handle?.setPose(pose.at, { x: 0, y: 0, z: pose.turn });
        this.coverGrass(thing);
        return true;
      }
      // A plant is planted again at its new place.
      thing.handle?.remove();
      thing.handle = this.api.Vegetation.plant(thing.entry.name, pose.at, {
        turn: pose.turn,
        scale: thing.scale ?? 1,
      });
      return Boolean(thing.handle);
    };
    const label = turn === before.turn ? 'move' : 'rotate';
    this.run({ label, redo: () => apply(after), undo: () => apply(before) });
    this.message = `${label === 'move' ? 'Moved' : 'Turned'} ${this.nameOf(thing)}`;
  }

  // Changes the selected shape as one step: `change(shape)` gives its new { size }.
  changeShape(label, change) {
    const shape = this.selected;
    const before = { size: { ...shape.size } };
    const after = change(shape);
    const apply = (state) => {
      shape.size = { ...state.size };
      this.collision.update(shape);
    };
    this.run({
      label,
      redo: () => {
        apply(after);
        return true;
      },
      undo: () => apply(before),
    });
  }

  deleteSelected() {
    const thing = this.selected;
    const kind = this.selectionKind();
    if (!kind) {
      this.message = 'Nothing selected to delete.';
      return;
    }
    const name = this.nameOf(thing);
    if (kind === 'level') {
      if (!thing.hidden) {
        this.setLevelHidden(thing, true);
      }
      return;
    }
    if (kind === 'shape') {
      this.run({
        label: `delete ${name}`,
        redo: () => {
          this.collision.unbuild(thing);
          this.select(null);
          return true;
        },
        undo: () => {
          if (this.collision.build(thing)) {
            this.select(thing);
          }
        },
      });
    } else {
      this.run({
        label: `delete ${name}`,
        redo: () => {
          this.takeAway(thing);
          return true;
        },
        undo: () => this.restore(thing),
      });
    }
    this.message = `Deleted ${name}`;
  }

  // Flies the camera to the selection, far enough to see all of it.
  focusSelected() {
    this.sightChanged();
    const thing = this.selectionKind() ? this.selected : null;
    if (!thing) {
      this.message = 'Select something to focus on (F).';
      this.refresh();
      return;
    }
    const box = this.boxOf(thing);
    const size = Math.max(box.max[0] - box.min[0], box.max[1] - box.min[1], box.max[2]);
    const centre = new this.api.Vector3(box.at.x, box.at.y, box.at.z + box.max[2] / 2);
    this.api.NoClip.focus(centre, Math.min(4096, Math.max(4, size * 1.6 + 4)));
    this.message = `Focused on ${this.nameOf(thing)}`;
    this.refresh();
  }

  // A copy of the selection beside it (its width further along X), selected; one step.
  duplicateSelected() {
    const thing = this.selected;
    const kind = this.selectionKind();
    if (!kind || kind === 'level') {
      this.message = 'Select something of yours to duplicate (Ctrl+D).';
      return;
    }
    const box = this.boxOf(thing);
    const step = Math.max(1, box.max[0] - box.min[0]) + 1;
    if (kind === 'shape') {
      const layer = this.collision;
      const ground = { x: thing.ground.x + step, y: thing.ground.y, z: thing.ground.z };
      layer.kind = thing.kind;
      const copy = layer.make(ground, thing.turn);
      copy.size = { ...thing.size };
      this.run({
        label: `duplicate ${this.nameOf(thing)}`,
        redo: () => {
          const built = layer.build(copy);
          if (built) {
            this.select(copy);
          }
          return built;
        },
        undo: () => {
          layer.unbuild(copy);
          if (this.selected === copy) {
            this.select(null);
          }
        },
      });
      this.message = `Duplicated ${this.nameOf(thing)}`;
      return;
    }
    const armed = this.armed;
    const turn = this.turn;
    this.armed = thing.entry;
    this.turn = thing.turn;
    // A copy of a level object's copy keeps its tilt too.
    this.placeTilt = thing.tilt ?? null;
    this.place({ x: thing.at.x + step, y: thing.at.y, z: thing.at.z });
    this.placeTilt = null;
    this.armed = armed;
    this.turn = turn;
  }

  // The ground under a building levelled to its foot: only its footprint (the box of its pieces,
  // PAD_LEAST to PAD_MOST metres from its centre) and PAD_FADE around it, so the author's own
  // shaping further out stays as it is.
  levelPad(record) {
    const box = this.boxOf(record);
    const half = Math.hypot(box.max[0] - box.min[0], box.max[1] - box.min[1]) / 2;
    const radius = Math.min(PAD_MOST, Math.max(PAD_LEAST, half));
    return this.api.Terrain.flatten(record.at, radius, record.at.z, PAD_FADE);
  }

  // Builds a taken-away building or plant again where it stood.
  restore(record) {
    const { Prefab, Terrain, Vegetation, Vector3 } = this.api;
    if (record.entry.source === 'prefab') {
      if (this.fitGround) {
        record.pad = this.levelPad(record);
      }
      const position = new Vector3(record.at.x, record.at.y, record.at.z);
      record.handle = Prefab.place(record.entry.name, position, { x: 0, y: 0, z: record.turn });
    } else if (record.entry.source === 'light') {
      record.handle = this.lightHandle(record);
    } else if (record.entry.source === 'sound') {
      record.handle = this.soundHandle(record.entry.name, record.at);
    } else if (record.entry.source === 'npc') {
      record.handle = this.npcHandle(record);
    } else if (record.entry.source === 'effect') {
      record.fx ??= effectDefaults(record.entry.name);
      record.handle = this.effectHandle(record.entry.name, record.at, record.turn, record.fx);
    } else if (record.entry.source === 'prop') {
      record.handle = this.propHandle(
        record.entry.name,
        record.at,
        record.turn,
        record.tilt,
        record.scale ?? 1,
        record.material ?? null,
      );
    } else {
      record.handle = Vegetation.plant(record.entry.name, record.at, {
        turn: record.turn,
        scale: record.scale ?? 1,
      });
    }
    if (record.handle) {
      this.placed.push(record);
      this.coverGrass(record);
      this.select(record);
    }
  }

  // The level's grass carpet under a placed building, hidden in this game
  // (grass grew through a house's floor once the carpet followed the levelled ground): circles as
  // wide as the building is deep, in a row along its long side, so little grass beside it goes.
  // Kept with the building (not a project patch): placing, moving or rebuilding it covers again.
  coverGrass(record) {
    this.uncoverGrass(record);
    if (record.entry?.source !== 'prefab') {
      return;
    }
    const box = this.boxOf(record);
    const width = box.max[0] - box.min[0];
    const depth = box.max[1] - box.min[1];
    const long = width >= depth ? 0 : 1;
    const radius = Math.max(1, Math.min(width, depth) / 2);
    const from = box.min[long] + radius;
    const to = box.max[long] - radius;
    const middle = [(box.min[0] + box.max[0]) / 2, (box.min[1] + box.max[1]) / 2];
    const turn = ((record.turn ?? 0) * Math.PI) / 180;
    record.grassCover = [];
    const steps = Math.max(0, Math.ceil((to - from) / radius));
    for (let step = 0; step <= steps; step += 1) {
      const along = steps === 0 ? (from + to) / 2 : from + ((to - from) * step) / steps;
      const local = long === 0 ? [along, middle[1]] : [middle[0], along];
      const x = record.at.x + local[0] * Math.cos(turn) - local[1] * Math.sin(turn);
      const y = record.at.y + local[0] * Math.sin(turn) + local[1] * Math.cos(turn);
      const id = this.nextGrass;
      this.nextGrass += 1;
      this.api.send({ op: 'architectGrass', kind: 'hide', id, x, y, radius });
      record.grassCover.push(id);
    }
  }

  uncoverGrass(record) {
    for (const id of record.grassCover ?? []) {
      this.api.send({ op: 'architectGrass', kind: 'show', id });
    }
    record.grassCover = [];
  }

  // A light of the editor: a point light (the client Light) with the record's colour, intensity,
  // radius and shadows, at its place; a handle with setPose, update (the light's settings) and
  // destroy.
  lightHandle(record) {
    const { Light, Vector3 } = this.api;
    if (!Light) {
      return null;
    }
    const settings = () => {
      const { color, intensity, radius, castShadows } = record.light;
      return { color: new Vector3(color.x, color.y, color.z), intensity, radius, castShadows };
    };
    let light = null;
    try {
      light = Light.spawn(new Vector3(record.at.x, record.at.y, record.at.z), settings());
    } catch (error) {
      this.message = `The light could not be made: ${error.message}`;
      return null;
    }
    return {
      setPose: (position) => {
        light.position = new Vector3(position.x, position.y, position.z);
      },
      update: () => {
        const next = settings();
        light.color = next.color;
        light.intensity = next.intensity;
        light.radius = next.radius;
        light.castShadows = next.castShadows;
      },
      destroy: () => light.destroy(),
    };
  }

  // A preview NPC: the game's own body at a point, in this game only (localNpcCreate).
  npcHandle(record) {
    if (!this.api.send) {
      return null;
    }
    this.npcSerial = (this.npcSerial ?? 0) + 1;
    const id = NPC_IDS + this.npcSerial;
    const person = PEOPLE.find((entry) => entry.key === record.entry.name) ?? PEOPLE[0];
    const rotation = (turn) => {
      const half = (turn * Math.PI) / 360;
      return { x: 0, y: 0, z: Math.sin(half), w: Math.cos(half) };
    };
    const point = (place) => ({ x: place.x, y: place.y, z: place.z });
    // On the terrain under the point: a press on a tree's crown or a roof would put it up there.
    const [ground] = this.groundsOf([record.at], NPC_GROUND_REACH);
    if (typeof ground === 'number') {
      record.at = { ...record.at, z: ground };
    }
    record.npcId = id;
    record.walk = '';
    this.api.send({
      op: 'localNpcCreate',
      id,
      soul: '',
      actorClass: person.actorClass,
      name: person.label,
      position: point(record.at),
      rotation: rotation(record.turn ?? 0),
    });
    return {
      setPose: (position, turn) =>
        this.api.send({
          op: 'localNpcPlace',
          id,
          position: point(position),
          rotation: rotation(turn?.z ?? 0),
        }),
      destroy: () => {
        if (this.walker === record) {
          this.walker = null;
        }
        this.api.send({ op: 'localNpcRemove', id });
      },
    };
  }

  // Sends a preview NPC walking to a point by the game's own walk on the navmesh.
  walkTo(record, at) {
    this.walker = null;
    if (!record.handle || !record.npcId) {
      return;
    }
    this.api.send({
      op: 'localNpcWalk',
      id: record.npcId,
      position: { x: at.x, y: at.y, z: at.z },
      speed: 1,
    });
    record.walk = 'walking';
    record.walkAsked = 0;
    // Where it was sent: an Export for server has the server's NPC walk there too.
    record.walkTarget = { x: at.x, y: at.y, z: at.z };
    this.message = `${record.entry.label} walks to ${at.x.toFixed(1)} ${at.y.toFixed(1)}.`;
  }

  // The walking NPCs' states, asked once a second (reached, or no path where there is no
  // navmesh); `now` asks at once.
  pollWalks(now = false) {
    const time = Date.now();
    for (const record of this.placed) {
      if (record.entry.source !== 'npc' || record.walk !== 'walking') {
        continue;
      }
      if (!now && time - (record.walkAsked ?? 0) < NPC_WALK_POLL_MS) {
        continue;
      }
      record.walkAsked = time;
      const state = this.api.npcWalkState?.(record.npcId);
      if (typeof state === 'string' && state !== '' && state !== record.walk) {
        record.walk = state;
        this.message =
          state === 'no path'
            ? `${record.entry.label} found no way there: no navmesh between.`
            : `${record.entry.label}: ${walkLabel(state)}.`;
        this.refresh();
      }
    }
  }

  // The game's world sounds that loop (an ambience, a fire, bees), asked once.
  worldSounds() {
    if (!this.soundList?.length) {
      const asked = this.api.worldSounds?.();
      this.soundList = Array.isArray(asked) ? asked : [];
    }
    return this.soundList;
  }

  // A sound playing at a point in this game until taken away (architect/level_sounds.hpp).
  soundHandle(name, at) {
    if (!this.api.send) {
      return null;
    }
    this.soundSerial = (this.soundSerial ?? 0) + 1;
    const id = this.soundSerial;
    const point = (place) => ({ x: place.x, y: place.y, z: place.z });
    this.api.send({ op: 'localSoundStart', id, trigger: name, at: point(at) });
    return {
      setPose: (position) => this.api.send({ op: 'localSoundMove', id, at: point(position) }),
      destroy: () => this.api.send({ op: 'localSoundStop', id }),
    };
  }

  lightEntry(key) {
    const preset = LIGHT_PRESETS.find((entry) => entry.key === key) ?? LIGHT_PRESETS[0];
    return { label: preset.label, name: preset.key, source: 'light' };
  }

  // The work as a server's resource (server_export.cjs): what every player then sees.
  work() {
    const at = (point) => ({ x: round(point.x), y: round(point.y), z: round(point.z) });
    const yaw = (degrees) => {
      const half = (degrees * Math.PI) / 360;
      return { x: 0, y: 0, z: Math.sin(half), w: Math.cos(half) };
    };
    const of = (source) => this.placed.filter((record) => record.entry.source === source);
    const levelEdits = [];
    for (const thing of this.hiddenLevel) {
      const object = thing.object;
      levelEdits.push(
        object.kind === 'entity'
          ? { kind: 'entity', guid: object.guid, at: at(object.at), hidden: true }
          : { name: object.name, at: at(object.at), hidden: true },
      );
    }
    for (const thing of this.movedLevel) {
      if (this.hiddenLevel.includes(thing)) {
        continue;
      }
      const object = thing.object;
      const scale = (thing.scale ?? 1) === 1 ? {} : { scale: (object.scale?.x ?? 1) * thing.scale };
      if (object.kind === 'entity') {
        const base = object.rotation ?? { x: 0, y: 0, z: 0, w: 1 };
        levelEdits.push({
          kind: 'entity',
          guid: object.guid,
          at: at(object.at),
          position: at(thing.at),
          rotation: multiplied(yaw((thing.turn ?? 0) - yawOf(base)), base),
          ...scale,
        });
      } else {
        levelEdits.push({ name: object.name, at: at(object.at), ...scale });
      }
    }
    for (const move of this.groundMoves) {
      levelEdits.push({ name: move.mesh, at: at(move.home), position: at(move.at) });
    }
    const shapes = this.collision.shapes;
    return {
      map: this.map,
      spawns: this.spawnsOf(shapes),
      buildings: of('prefab').map((record) => ({
        name: record.entry.name,
        at: at(record.at),
        turn: record.turn,
      })),
      props: of('prop').map((record) => {
        const rotationAt = (degrees) =>
          record.tilt ? multiplied(yaw(degrees - yawOf(record.tilt)), record.tilt) : yaw(degrees);
        const prop = {
          name: record.entry.name,
          at: at(record.at),
          rotation: rotationAt(record.turn),
          scale: record.scale ?? 1,
        };
        if (record.mover) {
          // A door or gate of yours: its way open as poses the server glides it through, and
          // where players use it (KCDS-DOCS/world-architect.md).
          const box = this.moverBox(record);
          const use = moverUse(record.at, record.turn, box);
          prop.door = {
            path: moverPath(record.at, record.turn, box, record.mover).map((pose) => ({
              at: at(pose.at),
              rotation: rotationAt(pose.turn),
            })),
            seconds: record.mover.seconds,
            closeAfter: record.mover.closeAfter,
            use: { at: at(use.at), reach: use.reach, height: use.height },
          };
        }
        return prop;
      }),
      levelEdits,
      areas: shapes
        .filter((shape) => SHAPE_KINDS[shape.kind].zone)
        .map((shape) => this.collision.areaOf(shape)),
      effects: of('effect').map((record) => ({
        name: record.entry.name,
        at: at(record.at),
        turn: record.turn,
        scale: record.fx?.scale ?? 1,
        pulse: record.fx?.pulse ?? 0,
      })),
      lights: of('light').map((record) => ({ at: at(record.at), light: { ...record.light } })),
      sounds: of('sound').map((record) => ({ name: record.entry.name, at: at(record.at) })),
      // The People the author tried a walk with:
      // each a server NPC where it stood, walking where it was sent.
      people: of('npc').map((record) => {
        const person = PEOPLE.find((entry) => entry.key === record.entry.name) ?? PEOPLE[0];
        return {
          class: person.actorClass,
          name: person.label,
          at: at(record.at),
          turn: record.turn ?? 0,
          ...(record.walkTarget ? { walkTo: at(record.walkTarget) } : {}),
        };
      }),
      // A fitted building's pad first (as it was made when the building was placed), then the
      // brushes' edits in the order they were made.
      terrain: [
        ...of('prefab')
          .filter((record) => record.pad)
          .map((record) => {
            const box = this.boxOf(record);
            const half = Math.hypot(box.max[0] - box.min[0], box.max[1] - box.min[1]) / 2;
            const size = Math.min(PAD_MOST, Math.max(PAD_LEAST, half));
            return { mode: 'pad', at: at(record.at), size, fade: PAD_FADE };
          }),
        ...this.terrainEdits.map((edit) => ({
          mode: edit.mode,
          at: at(edit.at),
          size: edit.size,
          strength: edit.strength,
        })),
      ],
      plants: of('vegetation').map((record) => ({
        name: record.entry.name,
        at: at(record.at),
        turn: record.turn,
        scale: record.scale ?? 1,
      })),
      shapes: shapes
        .filter((shape) => !SHAPE_KINDS[shape.kind].zone)
        .map((shape) => ({
          center: at(this.collision.centerOf(shape)),
          size: { ...shape.size },
          turn: shape.turn,
          pitch: shape.pitch,
        })),
    };
  }

  // Where the exported server lets players appear: the middle of every spawn area, a little above
  // its floor; on a custom map without one, the ground under the camera (the map has no spawn
  // point of its own and the server's would be off it); else none (the server's own stands).
  spawnsOf(shapes) {
    const lifted = (point) => ({ x: point.x, y: point.y, z: point.z + SPAWN_LIFT });
    const areas = shapes
      .filter((shape) => shape.kind === 'spawn')
      .map((shape) => ({ at: lifted(shape.ground), turn: shape.turn }));
    if (areas.length > 0 || !CUSTOM_MAP.test(this.map)) {
      return areas;
    }
    const eye = this.api.NoClip?.getPosition?.();
    if (!eye) {
      return [];
    }
    const [ground] = this.groundsOf([eye], SPAWN_REACH);
    return typeof ground === 'number'
      ? [{ at: lifted({ x: eye.x, y: eye.y, z: ground }), turn: 0 }]
      : [];
  }

  // Export for server: the work as a resource folder architect_<map> in the
  // exports folder, ready to copy into a server's resources; { folder, ... } or throws. The
  // folder's path goes on the clipboard. On a custom map the map itself goes in too
  // (maps/<map>/), for the server to send its players.
  exportForServer() {
    const work = this.work();
    const name = `architect_${this.map}`;
    const files = [
      {
        name: 'package.json',
        text: JSON.stringify(
          {
            name,
            version: '1.0.0',
            kcds: { serverScripts: ['server.js'], clientScripts: ['client.js'] },
          },
          null,
          2,
        ),
      },
      { name: 'server.js', text: serverScript() },
      { name: 'client.js', text: clientScript() },
      { name: 'work.json', text: JSON.stringify(work) },
      { name: 'README.txt', text: readme(name, this.map) },
    ];
    let path = this.writeExport(name, files);
    // A custom map goes with the work: the server sends it to its players.
    if (typeof path === 'string' && CUSTOM_MAP.test(this.map)) {
      path = this.api.exportFiles?.(name, [], this.map);
    }
    if (typeof path !== 'string') {
      this.message = 'The export could not be written.';
      this.refresh();
      throw new Error(this.message);
    }
    this.api.send({ op: 'architectCopy', text: path });
    this.message = `Exported for a server (folder copied): ${path}`;
    this.refresh();
    return {
      folder: path,
      buildings: work.buildings.length,
      props: work.props.length,
      levelEdits: work.levelEdits.length,
      plants: work.plants.length,
      terrain: work.terrain.length,
      people: work.people.length,
    };
  }

  // Writes files into exports\<folder>, each in pieces (a message holds 64 KiB); the folder's
  // path, or null.
  writeExport(folder, files) {
    let path = null;
    for (const file of files) {
      for (let start = 0; start === 0 || start < file.text.length; start += EXPORT_PIECE) {
        const piece = file.text.slice(start, start + EXPORT_PIECE);
        path = this.api.exportFiles?.(folder, [
          { name: file.name, text: piece, append: start > 0 },
        ]);
        if (typeof path !== 'string') {
          return null;
        }
      }
    }
    return path;
  }

  // Export for Blender (blender_export.cjs): the work and the ground around it into the exports
  // folder; { folder, things, ground } or throws. The folder's path goes on the clipboard.
  exportForBlender() {
    const things = [...this.placed, ...this.collision.shapes];
    const footOf = (thing) => (thing.serial !== undefined ? thing.ground : thing.at);
    const area = areaOf(
      things.map((thing) => footOf(thing)),
      this.api.NoClip.getPosition?.(),
    );
    const count = Math.min(GRID_MOST, Math.max(2, Math.round(area.side / 2) + 1));
    const step = area.side / (count - 1);
    const grid = [];
    for (let row = 0; row < count; row += 1) {
      for (let column = 0; column < count; column += 1) {
        grid.push({ x: area.x + column * step, y: area.y + row * step, z: 0 });
      }
    }
    const heights = this.groundsOf(grid, 4000, EXPORT_HEIGHTS_AT_ONCE);
    const yaw = (degrees) => {
      const half = (degrees * Math.PI) / 360;
      return { x: 0, y: 0, z: Math.sin(half), w: Math.cos(half) };
    };
    const scene = {
      name: `${this.map}`,
      map: this.map,
      origin: { x: round(area.x), y: round(area.y), z: 0 },
      things: things.map((thing) => {
        const shape = thing.serial !== undefined;
        const at = footOf(thing);
        const box = this.boxOf(thing);
        const rotation = thing.tilt
          ? multiplied(yaw((thing.turn ?? 0) - yawOf(thing.tilt)), thing.tilt)
          : yaw(thing.turn ?? 0);
        return {
          kind: shape ? (SHAPE_KINDS[thing.kind].zone ? 'zone' : 'collision') : thing.entry.source,
          model: shape ? thing.kind : thing.entry.name,
          label: this.nameOf(thing),
          at: { x: round(at.x - area.x), y: round(at.y - area.y), z: round(at.z) },
          rotation,
          scale: thing.scale ?? 1,
          box: { min: box.min.map(round), max: box.max.map(round) },
        };
      }),
      areas: this.collision.shapes
        .filter((shape) => SHAPE_KINDS[shape.kind].zone)
        .map((shape) => this.collision.areaOf(shape)),
    };
    const now = new Date();
    const two = (value) => String(value).padStart(2, '0');
    const stamp =
      `${now.getFullYear()}${two(now.getMonth() + 1)}${two(now.getDate())}` +
      `${two(now.getHours())}${two(now.getMinutes())}`;
    const folder = `${this.map}-${stamp}`;
    const files = [
      { name: 'ground.obj', text: groundObj(area, count, heights) },
      { name: 'scene.json', text: JSON.stringify(scene, null, 2) },
      { name: 'import_kcds.py', text: IMPORT_SCRIPT },
    ];
    const path = this.writeExport(folder, files);
    if (typeof path !== 'string') {
      this.message = 'The export could not be written.';
      this.refresh();
      throw new Error(this.message);
    }
    this.api.send({ op: 'architectCopy', text: path });
    this.message = `Exported for Blender (folder copied): ${path}`;
    this.refresh();
    return { folder: path, things: scene.things.length, ground: count * count };
  }

  // Your own assets (architect/user_assets.hpp): { folder, meshes, materials, refused }, asked once
  // an opening.
  userAssets() {
    if (!this.assets) {
      const answer = this.api.userAssets?.();
      this.assets = {
        folder: typeof answer?.folder === 'string' ? answer.folder : '',
        meshes: Array.isArray(answer?.meshes) ? answer.meshes : [],
        materials: Array.isArray(answer?.materials) ? answer.materials : [],
        refused: Array.isArray(answer?.refused) ? answer.refused : [],
      };
    }
    return this.assets;
  }

  // A prop drawn with another material (null: its own), as one undo step.
  changeMaterial(thing, material) {
    const before = thing.material ?? null;
    const apply = (value) => {
      thing.material = value;
      thing.handle?.setMaterial?.(value, thing.at, thing.turn);
      return true;
    };
    this.run({ label: 'material', redo: () => apply(material), undo: () => apply(before) });
    this.message = `${this.nameOf(thing)}: ${material ? shortName(material) : 'its own material'}`;
  }

  // An effect's size or repeat changed as one undo step: `change(fx)` gives the new settings.
  changeEffect(label, change) {
    const thing = this.selected;
    const before = { ...thing.fx };
    const after = { ...before, ...change(before) };
    const apply = (settings) => {
      thing.fx = { ...settings };
      thing.handle?.update?.(thing.fx);
      return true;
    };
    this.run({ label, redo: () => apply(after), undo: () => apply(before) });
  }

  // A light's setting changed as one undo step: `change(light)` gives the new settings.
  changeLight(label, change) {
    const thing = this.selected;
    const before = { ...thing.light, color: { ...thing.light.color } };
    const after = { ...before, ...change(before) };
    const apply = (settings) => {
      thing.light = { ...settings, color: { ...settings.color } };
      thing.handle?.update();
      return true;
    };
    this.run({ label, redo: () => apply(after), undo: () => apply(before) });
  }

  // A particle effect of the game running at `at`, turned `turn` degrees, in this game (the client
  // Vfx): a handle with setPose (the effect is started again there) and destroy.
  effectHandle(name, at, turn, fx = effectDefaults(name)) {
    const { Vfx, Vector3 } = this.api;
    if (!Vfx) {
      return null;
    }
    let settings = fx;
    let place = { at, turn };
    const start = (position, degrees) =>
      Vfx.spawn(name, new Vector3(position.x, position.y, position.z), {
        rotation: { x: 0, y: 0, z: degrees },
        prime: true,
        scale: settings.scale,
        // A short effect (a blood splash, a flash) plays again every `pulse` seconds.
        ...(settings.pulse > 0 ? { pulsePeriod: settings.pulse } : {}),
      });
    let running = start(at, turn);
    return {
      setPose: (position, orientation) => {
        running?.destroy();
        place = { at: position, turn: orientation?.z ?? 0 };
        running = start(place.at, place.turn);
      },
      // New settings: started again with them.
      update: (next) => {
        settings = next;
        running?.destroy();
        running = start(place.at, place.turn);
      },
      destroy: () => {
        running?.destroy();
        running = null;
      },
    };
  }

  // A prop mesh's box in its own frame ({ min, max }), as the game measures a placed one; asked
  // once a mesh (again at most every PROP_BOX_RETRY_MS while none of it stands yet), null till then.
  propBox(mesh) {
    this.propBoxes ??= new Map();
    const known = this.propBoxes.get(mesh);
    if (known?.box) {
      return known.box;
    }
    const now = Date.now();
    if (known && now - known.asked < PROP_BOX_RETRY_MS) {
      return null;
    }
    const answer = this.api.propBounds?.(mesh);
    const usable = Array.isArray(answer?.min) && Array.isArray(answer?.max);
    this.propBoxes.set(mesh, {
      asked: now,
      box: usable ? { min: answer.min, max: answer.max } : null,
    });
    return usable ? { min: answer.min, max: answer.max } : null;
  }

  // A prop of the game's catalog standing solid at `at`, turned `turn` degrees, in this game
  // (props/local_props.hpp): a handle with setPose and destroy, as a prefab's.
  // `tilt`: the full rotation of a level object's copy ({ x, y, z, w }), kept under later turns
  // (a fallen fence stays fallen); the turn then turns it about the up axis from its own yaw.
  propHandle(mesh, at, turn, tilt = null, scale = 1, material = null) {
    const id = this.nextProp;
    this.nextProp += 1;
    let size = scale;
    let rotation = tilt;
    let look = material;
    const send = (position, degrees) => {
      const base = rotation ?? { x: 0, y: 0, z: 0, w: 1 };
      const half = ((degrees - (rotation ? yawOf(rotation) : 0)) * Math.PI) / 360;
      const yaw = { x: 0, y: 0, z: Math.sin(half), w: Math.cos(half) };
      this.api.send({
        op: 'localProp',
        kind: 'set',
        id,
        mesh,
        position: { x: position.x, y: position.y, z: position.z },
        rotation: multiplied(yaw, base),
        scale: size,
        ...(look ? { material: look } : {}),
      });
    };
    send(at, turn);
    return {
      id,
      setPose: (position, orientation) => send(position, orientation?.z ?? 0),
      // A new size, at the same place and turn.
      setScale: (value, position, degrees) => {
        size = value;
        send(position, degrees);
      },
      // Another material (null: the mesh's own), at the same place.
      setMaterial: (value, position, degrees) => {
        look = value;
        send(position, degrees);
      },
      // A new full rotation (a tilt; null: upright), at the same place.
      setTilt: (value, position, degrees) => {
        rotation = value;
        send(position, degrees);
      },
      destroy: () => this.api.send({ op: 'localProp', kind: 'remove', id }),
    };
  }

  // ---- Toolbar ----

  // A tool by its key ('select', 'move', 'rotate', 'scale'); stops placing.
  useTool(key) {
    const tool = TOOLS.find((entry) => entry.key === key);
    if (!tool) {
      return false;
    }
    this.tool = tool.key;
    this.armed = null;
    this.message = `${tool.label} tool.`;
    this.refresh();
    return true;
  }

  // A mode by its key ('place', 'terrain', 'foliage', 'collision'); false for Zones (next
  // version) and other names.
  useMode(key) {
    const mode = MODES.find((entry) => entry.key === key);
    if (!mode) {
      return false;
    }
    if (mode.later) {
      this.message = `${mode.label} come in the next version.`;
      this.refresh();
      return false;
    }
    if (this.mode !== mode.key) {
      this.mode = mode.key;
      this.folder = mode.folders[0];
      this.page = 0;
      this.armed = null;
      // Another mode works on other things: what was selected is let go.
      this.select(null);
    }
    if (mode.key === 'collision' || mode.key === 'zones') {
      this.collision.show();
    } else {
      this.collision.hide();
    }
    this.message = `${mode.label} mode.`;
    this.refresh();
    return true;
  }

  snapLabels() {
    return [
      this.gridStep > 0 ? `Grid ${this.gridStep} m` : 'Grid off',
      `Angle ${this.angleStep}°`,
      `Fit ground ${this.fitGround ? 'on' : 'off'}`,
    ];
  }

  pressSnap(index) {
    if (index === 0) {
      const next = (GRID_STEPS.indexOf(this.gridStep) + 1) % GRID_STEPS.length;
      this.gridStep = GRID_STEPS[next];
    } else if (index === 1) {
      const next = (ANGLE_STEPS.indexOf(this.angleStep) + 1) % ANGLE_STEPS.length;
      this.angleStep = ANGLE_STEPS[next];
    } else if (index === 2) {
      this.fitGround = !this.fitGround;
    }
    this.refresh();
  }

  pressAction(index) {
    const name = ACTIONS[index];
    if (name === 'Undo') {
      this.undo();
    } else if (name === 'Redo') {
      this.redo();
    } else if (name === 'Exit') {
      // Play as player: the player starts where the camera looks, and the screen is kept for the
      // way back. A start that cannot be found never keeps the editor open.
      let start = null;
      try {
        start = this.playerStart();
      } catch (error) {
        this.api.log(`World Architect: no start for Play as player (${error})`);
      }
      this.close({ park: true, start });
      return;
    }
    this.refresh();
  }

  // ---- Outliner ----

  // The outliner's lines: each group's name and count, then its things (none while the group is
  // folded), through the filter and the search.
  outlinerLines() {
    const filter = FILTERS[this.outlinerFilter].key;
    const query = this.outlinerQuery.trim().toLowerCase();
    const groups = [
      ['prefab', 'Buildings', 'house', this.placed.filter((r) => r.entry.source === 'prefab')],
      ['vegetation', 'Foliage', 'tree', this.placed.filter((r) => r.entry.source === 'vegetation')],
      ['prop', 'Props', 'box', this.placed.filter((r) => r.entry.source === 'prop')],
      ['effect', 'Effects', 'play', this.placed.filter((r) => r.entry.source === 'effect')],
      ['light', 'Lights', 'play', this.placed.filter((r) => r.entry.source === 'light')],
      ['sound', 'Sounds', 'play', this.placed.filter((r) => r.entry.source === 'sound')],
      ['npc', 'People', 'box', this.placed.filter((r) => r.entry.source === 'npc')],
      [
        'shape',
        'Collision',
        'box',
        this.collision.shapes.filter((shape) => !SHAPE_KINDS[shape.kind].zone),
      ],
      ['level', 'Hidden from the level', 'tree', this.hiddenLevel],
    ];
    const zones = this.collision.shapes.filter((shape) => SHAPE_KINDS[shape.kind].zone);
    if (zones.length > 0) {
      groups.push(['zone', 'Zones', 'map', zones]);
    }
    if (this.movedLevel.length > 0) {
      groups.push(['moved', 'Moved in the level', 'box', this.movedLevel]);
    }
    const lines = [];
    for (const [key, name, glyph, all] of groups) {
      const joined =
        (filter === 'level' && key === 'moved') || (filter === 'shape' && key === 'zone');
      if (filter !== 'all' && filter !== key && !joined) {
        continue;
      }
      const things = all.filter((thing) => this.nameOf(thing).toLowerCase().includes(query));
      const collapsed = this.collapsed.has(key);
      lines.push({ group: key, name, type: String(things.length), collapsed, thing: null });
      if (collapsed) {
        continue;
      }
      for (const thing of things) {
        const types = {
          prefab: 'Prefab',
          shape: 'Shape',
          vegetation: 'Plant',
          level: 'Hidden',
          moved: 'Moved',
          zone: 'Zone',
        };
        const type = types[key];
        lines.push({ group: null, name: this.nameOf(thing), type, icon: glyph, thing });
      }
    }
    return lines;
  }

  // A press on a group folds or unfolds it; on a thing, selects it.
  pressOutliner(row) {
    const line = this.outlinerLines()[this.outlinerPage * outlinerRows() + row];
    if (!line) {
      return;
    }
    if (line.group) {
      if (this.collapsed.has(line.group)) {
        this.collapsed.delete(line.group);
      } else {
        this.collapsed.add(line.group);
      }
    } else {
      // A second press on the same line soon after: the camera flies to it.
      const now = Date.now();
      const again =
        this.lastOutlinerPress?.thing === line.thing &&
        now - this.lastOutlinerPress.time <= DOUBLE_PRESS_MS;
      this.lastOutlinerPress = { thing: line.thing, time: now };
      this.select(line.thing);
      if (again) {
        this.focusSelected();
        this.lastOutlinerPress = null;
        return;
      }
      this.message = `Selected ${this.nameOf(line.thing)} (press again to fly there)`;
    }
    this.refresh();
  }

  turnOutlinerFilter() {
    this.outlinerFilter = (this.outlinerFilter + 1) % FILTERS.length;
    this.outlinerPage = 0;
    this.message = `Outliner shows: ${FILTERS[this.outlinerFilter].label}`;
    this.refresh();
  }

  // The CAMERA rows: how fast the free camera flies, 1 to 200 m/s;
  // held and dragged sideways it goes through the steps like any value.
  cameraRows() {
    return [
      { label: 'CAMERA' },
      {
        label: 'Fly speed',
        value: `${this.cameraSpeed} m/s`,
        step: (dir) => {
          const at = FLY_SPEEDS.findIndex((speed) => speed >= this.cameraSpeed);
          const from = at < 0 ? FLY_SPEEDS.length - 1 : at;
          const next = FLY_SPEEDS[Math.min(FLY_SPEEDS.length - 1, Math.max(0, from + dir))];
          this.setCameraSpeed(next);
        },
      },
    ];
  }

  setCameraSpeed(speed) {
    this.cameraSpeed = speed;
    this.api.NoClip.setSpeed(speed);
    this.refresh();
  }

  turnCameraSpeed() {
    const next = (CAMERA_SPEEDS.indexOf(this.cameraSpeed) + 1) % CAMERA_SPEEDS.length;
    this.cameraSpeed = CAMERA_SPEEDS[next];
    this.api.NoClip.setSpeed(this.cameraSpeed);
    this.refresh();
  }

  turnOutlinerPage(step) {
    const pages = Math.max(1, Math.ceil(this.outlinerLines().length / outlinerRows()));
    this.outlinerPage = (this.outlinerPage + step + pages) % pages;
    this.refresh();
  }

  // ---- Details ----

  // The details panel: { title, kind, rows } with rows of { label, value, step(dir), press() }.
  // The Details panel with the sections the author folded shut (their arrow pressed): a folded
  // section shows only its header.
  details() {
    const all = this.allDetails();
    this.foldedSections ??= new Set();
    const rows = [];
    let folded = false;
    for (const row of all.rows ?? []) {
      if (isSectionRow(row)) {
        folded = this.foldedSections.has(row.label);
        rows.push({ ...row, section: true, folded });
      } else if (!folded) {
        rows.push(row);
      }
    }
    return { ...all, rows };
  }

  // Every Details view ends with WORLD (time and weather), whatever is selected
  // ("weather control and time missing").
  allDetails() {
    const view = this.viewDetails();
    if (view.rows.some((row) => row.label === 'WORLD')) {
      return view;
    }
    return {
      ...view,
      rows: [...view.rows, ...this.cameraRows(), ...this.navmeshRows(), ...this.worldRows()],
    };
  }

  viewDetails() {
    if (this.mode === 'terrain') {
      return this.brushDetails();
    }
    const kind = this.selectionKind();
    if (kind) {
      return this.selectionDetails(kind);
    }
    if (this.armed) {
      return {
        icon: MODES.find((mode) => mode.key === this.mode).icon,
        title: this.armed.label,
        kind: 'Placing: press in the world, Escape stops',
        rows: [
          { label: 'PLACEMENT' },
          {
            label: 'Rotation',
            value: `${this.turn}°`,
            step: (dir) => {
              this.turn = degreesOf(this.turn + dir * this.angleStep);
            },
          },
          ...this.paintRows(),
          ...this.snapRows(),
        ],
      };
    }
    return {
      icon: 'select',
      title: 'Nothing selected',
      kind: 'Choose in Content, or Select (Q) and press',
      rows: [
        { label: 'SNAPS' },
        ...this.snapRows(),
        { label: 'EXPORT' },
        { label: 'Blender', value: 'Export for Blender', press: () => this.exportForBlender() },
        { label: 'Server', value: 'Export for server', press: () => this.exportForServer() },
      ],
    };
  }

  // ---- The map ----

  // A map chosen in the menu bar: the current one closes the list, another asks first.
  chooseMap(map) {
    this.mapPicker = map === this.map ? null : map;
    this.refresh();
  }

  // The maps the editor opens: the game's three, then the custom maps the game lists on this PC
  // (maps/custom_maps.hpp), asked again each time.
  allMaps() {
    const found = this.api.customMaps?.();
    const custom = Array.isArray(found)
      ? found
          .filter((name) => typeof name === 'string' && CUSTOM_MAP.test(name))
          .map((name) => ({ key: name, label: mapLabel(name) }))
      : [];
    return [...MAPS, ...custom];
  }

  // The switch the author confirmed: the work is saved and taken out of this world, the editor
  // closes and the game client loads the other map (architect/local_mode.hpp), where the editor
  // opens again with that map's project.
  switchMap() {
    const map = this.mapPicker;
    this.mapPicker = null;
    if (!this.allMaps().some((entry) => entry.key === map) || map === this.map) {
      this.refresh();
      return;
    }
    this.save();
    this.leaveWorld();
    this.close();
    this.api.send({ op: 'architectMap', map });
  }

  // Leave in the menu bar: the work is saved and taken out of this world, the editor closes and the
  // game client goes back to the main menu and its server list (architect/local_mode.hpp).
  leave() {
    this.save();
    this.leaveWorld();
    this.close();
    // Closing parks the pause menu for Esc back to the editor; there is no editor to go back to.
    this.api.Controls?.unblockAction?.('open_pause_menu');
    this.api.send({ op: 'architectLeave' });
  }

  // Takes the editor's work out of the world (it stays in the project file) and forgets it.
  leaveWorld() {
    // The plants first, many in each question to the game: one question each (a frame each) held
    // a map of 4,096 plants for over a minute.
    const plants = this.placed.filter((record) => record.entry?.source === 'vegetation');
    for (let start = 0; start < plants.length; start += LEAVE_PLANTS_AT_ONCE) {
      this.takeAwayAll(plants.slice(start, start + LEAVE_PLANTS_AT_ONCE));
    }
    for (const record of [...this.placed]) {
      this.takeAway(record);
    }
    for (const shape of [...this.collision.shapes]) {
      this.collision.unbuild(shape);
    }
    this.collision.shapes = [];
    for (const thing of [...this.hiddenLevel, ...this.movedLevel, ...this.groundMoves]) {
      this.api.send({ op: 'localLevelEdit', kind: 'remove', id: thing.edit });
    }
    this.groundMoves = [];
    this.hiddenLevel = [];
    this.movedLevel = [];
    for (const patch of [...this.grassPatches]) {
      this.showGrass(patch);
    }
    for (const edit of [...this.terrainEdits].reverse()) {
      edit.handle?.undo();
    }
    this.terrainEdits = [];
    this.undoStack = [];
    this.redoStack = [];
    this.selected = null;
    this.armed = null;
    this.stroke = null;
    this.unsaved = 0;
  }

  // ---- Time of day and weather ----

  // The WORLD rows: each step changes the game's clock or sky at once.
  // NAVMESH (custom maps): make the map's navmesh again from the ground as it is now, without
  // your buildings, props and collision shapes, for the next start. On the game's own maps the
  // section says why there is nothing to bake.
  navmeshRows() {
    if (!CUSTOM_MAP.test(this.map)) {
      return [
        { label: 'NAVMESH' },
        { label: 'Game map', value: "The game's own (bake: custom maps)" },
      ];
    }
    return [
      { label: 'NAVMESH' },
      { label: 'Bake', value: 'For the next start', press: () => this.bakeNavmesh() },
    ];
  }

  // The ground people cannot walk on: the footprint (its box turned, on the ground plane) of
  // every building, prop and solid collision shape of yours.
  footprints() {
    // A prop's real size, asked again for the bake (a prop placed a moment ago had its mesh still
    // loading: its 1 m stand-in left nothing out, a fountain).
    for (const record of this.placed) {
      if (record.entry.source === 'prop' && !this.propBoxes?.get(record.entry.name)?.box) {
        this.propBoxes?.delete(record.entry.name);
      }
    }
    const solid = (record) => {
      if (record.serial !== undefined) {
        return !SHAPE_KINDS[record.kind]?.zone;
      }
      return record.entry.source === 'prefab' || record.entry.source === 'prop';
    };
    return [...this.placed, ...this.collision.shapes].filter(solid).map((thing) => {
      const box = this.boxOf(thing);
      const turn = ((box.turn ?? 0) * Math.PI) / 180;
      const cos = Math.cos(turn);
      const sin = Math.sin(turn);
      const xs = [];
      const ys = [];
      for (const x of [box.min[0], box.max[0]]) {
        for (const y of [box.min[1], box.max[1]]) {
          xs.push(box.at.x + x * cos - y * sin);
          ys.push(box.at.y + x * sin + y * cos);
        }
      }
      return {
        minX: Math.min(...xs),
        minY: Math.min(...ys),
        maxX: Math.max(...xs),
        maxY: Math.max(...ys),
      };
    });
  }

  // Bakes the custom map's navmesh in the game client (architect/navmesh_bake.hpp): taken at the
  // next start.
  bakeNavmesh() {
    const answer = this.api.bakeNavmesh?.(this.footprints());
    this.message =
      answer && typeof answer === 'object'
        ? `Navmesh baked: ${answer.walkable} walkable squares, ${answer.leftOut} under your work. ` +
          'It is used from the next start.'
        : 'The navmesh could not be baked (custom maps only).';
    this.refresh();
    return answer ?? null;
  }

  worldRows() {
    const look = this.look;
    const cycle = (key, list) => (dir) => {
      look[key] = (look[key] + dir + list.length) % list.length;
      this.applyLook();
    };
    return [
      { label: 'WORLD' },
      {
        label: 'Time of day',
        value: clockText(look.hour),
        step: (dir) => {
          look.hour = (look.hour + dir * HOUR_STEP + 24) % 24;
          this.applyLook();
        },
      },
      {
        label: 'Freeze time',
        toggle: look.clock === 0,
        press: () => {
          // Running again, at the speed it ran at before (×60 when it never ran).
          look.clock = look.clock === 0 ? look.runningClock ?? 2 : 0;
          this.applyLook();
        },
      },
      {
        label: 'Clock speed',
        value: CLOCK_SPEEDS[look.clock].label,
        step: (dir) => {
          cycle('clock', CLOCK_SPEEDS)(dir);
          if (look.clock !== 0) {
            look.runningClock = look.clock;
          }
        },
      },
      {
        label: 'Weather',
        value: look.weatherHeld ? WEATHERS[look.weather].label : "The game's own",
        step: (dir) => {
          look.weatherHeld = true;
          cycle('weather', WEATHERS)(dir);
        },
      },
      {
        label: 'Freeze weather',
        toggle: look.weatherHeld,
        press: () => {
          look.weatherHeld = !look.weatherHeld;
          this.applyLook();
        },
      },
      { label: 'Wind', value: WINDS[look.wind].label, step: cycle('wind', WINDS) },
    ];
  }

  // Sends the look to the game client and keeps it in the project.
  applyLook() {
    this.sendLook();
    this.changed();
  }

  sendLook() {
    const { look } = this;
    this.api.send({
      op: 'architectWorld',
      hour: look.hour,
      speed: CLOCK_SPEEDS[look.clock].speed,
      // Not frozen: the game's own weather runs ('' leaves it).
      weather: look.weatherHeld ? WEATHERS[look.weather].preset : '',
      wind: WINDS[look.wind].speed,
    });
  }

  // A look kept in a project, each value checked against its list.
  restoreLook(kept) {
    if (!kept) {
      return;
    }
    const within = (value, count) => Number.isInteger(value) && value >= 0 && value < count;
    const hour =
      Number.isFinite(kept.hour) && kept.hour >= 0 && kept.hour < 24
        ? Math.round(kept.hour / HOUR_STEP) * HOUR_STEP
        : 12;
    this.look = {
      hour: hour % 24,
      weatherHeld: kept.weatherHeld !== false,
      clock: within(kept.clock, CLOCK_SPEEDS.length) ? kept.clock : 0,
      weather: within(kept.weather, WEATHERS.length) ? kept.weather : 0,
      wind: within(kept.wind, WINDS.length) ? kept.wind : 1,
    };
    this.sendLook();
  }

  // The paint brush's rows, for a plant only.
  paintRows() {
    if (this.armed?.source !== 'vegetation') {
      return [];
    }
    return [
      { label: 'PAINT BRUSH' },
      {
        label: 'Plants a press',
        value: this.paintCount === 1 ? '1 (single)' : String(this.paintCount),
        step: (dir) => {
          this.paintCount = Math.min(PAINT_MOST, Math.max(1, this.paintCount + dir));
        },
      },
      {
        label: 'Radius',
        value: `${this.paintRadius} m`,
        step: (dir) => {
          this.paintRadius = Math.min(30, Math.max(2, this.paintRadius + dir * 2));
        },
      },
    ];
  }

  snapRows() {
    return [
      { label: 'Grid', value: this.snapLabels()[0], press: () => this.pressSnap(0) },
      { label: 'Angle', value: `${this.angleStep}°`, press: () => this.pressSnap(1) },
      { label: 'Fit ground', toggle: this.fitGround, press: () => this.pressSnap(2) },
    ];
  }

  brushDetails() {
    const brush = BRUSHES.find((entry) => entry.key === this.terrainMode);
    return {
      icon: 'terrain',
      title: `${brush.label} brush`,
      kind: 'Press or hold in the world: one step a stroke',
      rows: [
        { label: 'BRUSH' },
        {
          label: 'Brush',
          value: brush.label,
          press: () => {
            const next = (BRUSHES.indexOf(brush) + 1) % BRUSHES.length;
            this.terrainMode = BRUSHES[next].key;
          },
        },
        {
          label: 'Size',
          value: `${this.brushSize} m`,
          step: (dir) => {
            this.brushSize = Math.min(40, Math.max(1, this.brushSize + dir));
          },
        },
        ...this.brushRows(brush.key),
      ],
    };
  }

  // The rows a brush adds: the ground brushes' strength, Add nature's plant and count.
  brushRows(key) {
    if (key === 'clear') {
      return [];
    }
    if (key === 'nature') {
      const plant = this.natureEntry();
      return [
        { label: 'NATURE' },
        { label: 'Plants', value: "The ground's own grass" },
        {
          label: 'Density',
          value: `${this.grassDensity.toFixed(1)} / m²`,
          step: (dir) => {
            const next = Math.round((this.grassDensity + dir * 0.2) * 10) / 10;
            this.grassDensity = Math.min(3, Math.max(0.2, next));
          },
        },
        { label: 'Where none grows', value: plant ? plant.label : 'Choose one in Foliage' },
        {
          label: 'Plants a dab',
          value: String(this.natureCount),
          step: (dir) => {
            this.natureCount = Math.min(PAINT_MOST, Math.max(1, this.natureCount + dir));
          },
        },
      ];
    }
    return [
      {
        label: 'Strength',
        value: `${this.brushStrength} m`,
        step: (dir) => {
          this.brushStrength = Math.min(5, Math.max(0.25, this.brushStrength + dir * 0.25));
        },
      },
    ];
  }

  // A level entity's own properties: its Properties as the
  // level set them, asked once a selection; a bool toggles, a number steps (and drags), a text
  // shows. A change is made in this game only and is not kept in the project.
  propertyRows(thing) {
    const guid = thing.object.guid;
    if (!guid) {
      return [];
    }
    if (thing.properties === undefined) {
      const answer = this.api.entityProperties?.(guid);
      thing.properties = Array.isArray(answer) ? answer : [];
    }
    if (thing.properties.length === 0) {
      return [];
    }
    const rows = [{ label: 'PROPERTIES' }];
    for (const property of thing.properties) {
      const set = (value) => {
        property.value = value;
        this.api.send({ op: 'localEntityProperty', guid, key: property.key, value });
        this.message = `${property.key}: ${value}`;
      };
      if (property.type === 'bool') {
        rows.push({
          label: property.key,
          toggle: property.value,
          press: () => set(!property.value),
        });
      } else if (property.type === 'number') {
        const step = Number.isInteger(property.value) ? 1 : 0.1;
        rows.push({
          label: property.key,
          value: String(Math.round(property.value * 1000) / 1000),
          step: (dir) => set(Math.round((property.value + dir * step) * 1000) / 1000),
        });
      } else {
        rows.push({ label: property.key, value: property.value === '' ? '-' : property.value });
      }
    }
    return rows;
  }

  // A level door's rows: open and close it in this game to see it swing.
  // Its state is asked once a selection (localDoorState); no
  // rows for an entity that is no door.
  doorRows(thing) {
    const guid = thing.object.guid;
    if (!guid) {
      return [];
    }
    if (thing.door === undefined) {
      const answer = this.api.doorState?.(guid);
      thing.door = answer?.door
        ? { open: answer.open === true, locked: answer.locked === true }
        : null;
    }
    const door = thing.door;
    if (!door) {
      return [];
    }
    return [
      { label: 'DOOR' },
      {
        label: 'Open',
        toggle: door.open,
        press: () => this.swingDoor(thing, !door.open),
      },
      { label: 'Locked in the level', value: door.locked ? 'yes' : 'no' },
    ];
  }

  // The gates within GATE_REACH of a level object (the drawbridge, the portcullis, the levels' gate
  // series): a row each to step it through its states and watch it move (localGate). Asked once
  // a selection.
  gateRows(thing) {
    if (thing.gates === undefined) {
      const answer = this.api.gatesNear?.(
        { x: thing.at.x, y: thing.at.y, z: thing.at.z },
        GATE_REACH,
      );
      thing.gates = Array.isArray(answer) ? answer : [];
    }
    if (thing.gates.length === 0) {
      return [];
    }
    const rows = [{ label: 'GATE' }];
    for (const gate of thing.gates) {
      const shown = gate.state >= 0 ? gate.states[gate.state] : "the level's";
      rows.push({
        label: gateLabel(gate.key),
        value: shown ?? '-',
        step: (dir) => {
          const count = gate.states.length;
          const from = gate.state >= 0 ? gate.state : 0;
          const next = (from + dir + count) % count;
          this.api.send({ op: 'localGate', gate: gate.gate, state: next });
          gate.state = next;
          this.message = `${gateLabel(gate.key)}: ${gate.states[next]}`;
        },
      });
    }
    return rows;
  }

  // A prop of yours as a door or gate: whether it opens, about which side, how far it turns,
  // rises and slides, how long it takes, when it closes by itself, and a preview.
  moverRows(thing) {
    const mover = thing.mover;
    const rows = [
      { label: 'DOOR / GATE' },
      {
        label: 'Opens',
        toggle: Boolean(mover),
        press: () =>
          this.changeMover(thing, mover ? null : { ...MOVER_DEFAULTS }, mover ? 'still' : 'opens'),
      },
    ];
    if (!mover) {
      return rows;
    }
    const stepped = (key, label, size, shown) => ({
      label,
      value: shown(mover[key]),
      step: (dir) => {
        const value = Math.round((mover[key] + dir * size) * 100) / 100;
        const [low, high] = MOVER_LIMITS[key];
        this.changeMover(thing, { ...mover, [key]: Math.min(high, Math.max(low, value)) }, label);
      },
    });
    rows.push(
      {
        label: 'Hinge',
        value: mover.hinge,
        step: (dir) => {
          const at = HINGES.indexOf(mover.hinge);
          const hinge = HINGES[(at + dir + HINGES.length) % HINGES.length];
          this.changeMover(thing, { ...mover, hinge }, 'hinge');
        },
      },
      stepped('turn', 'Open turn', 15, (value) => `${value}°`),
      stepped('lift', 'Lift', 0.25, (value) => `${value.toFixed(2)} m`),
      stepped('slide', 'Slide', 0.25, (value) => `${value.toFixed(2)} m`),
      stepped('seconds', 'Seconds', 0.5, (value) => `${value.toFixed(1)} s`),
      stepped('closeAfter', 'Closes after', 5, (value) =>
        value > 0 ? `${value} s` : 'stays open',
      ),
      {
        label: 'Preview',
        value: thing.moverOpen ? 'Close it' : 'Open it',
        press: () => this.previewMover(thing, !thing.moverOpen),
      },
    );
    return rows;
  }

  // The box a mover turns about: the prop's mesh box at its size, or null before the game knows it.
  moverBox(record) {
    const box = this.propBox(record.entry.name);
    if (!box) {
      return null;
    }
    const scale = record.scale ?? 1;
    return { min: box.min.map((v) => v * scale), max: box.max.map((v) => v * scale) };
  }

  // A prop's mover changed (null: it is still again) as one undo step; it shows closed.
  changeMover(thing, next, label) {
    const before = thing.mover ? { ...thing.mover } : null;
    const after = next ? { ...next } : null;
    const apply = (mover) => {
      this.stopMoverPreview(thing);
      if (mover) {
        thing.mover = { ...mover };
      } else {
        delete thing.mover;
      }
      return true;
    };
    this.run({
      label: `door ${label}`,
      redo: () => apply(after),
      undo: () => apply(before),
    });
  }

  // Shows the mover opening (or closing) in this game as a server shows it: along its way over its
  // seconds. Nothing is kept; a move of the prop shows it closed again.
  previewMover(thing, open) {
    const mover = thing.mover;
    if (!mover || !thing.handle) {
      return;
    }
    this.stopMoverPreview(thing);
    const box = this.moverBox(thing);
    const started = Date.now();
    const from = thing.moverShare ?? 0;
    const to = open ? 1 : 0;
    const total = Math.max(1, Math.abs(to - from) * mover.seconds * 1000);
    const show = () => {
      const done = Math.min(1, (Date.now() - started) / total);
      const share = from + (to - from) * done;
      const pose = moverPose(thing.at, thing.turn, box, mover, share);
      thing.handle.setPose(pose.at, { z: pose.turn });
      thing.moverShare = share;
      if (done >= 1) {
        this.stopMoverPreview(thing, false);
      }
    };
    thing.moverOpen = open;
    thing.moverStop = this.every(MOVER_PREVIEW_MS, show);
    show();
    this.message = `${this.nameOf(thing)}: ${open ? 'opening' : 'closing'} (preview)`;
    this.refresh();
  }

  // Ends a mover's preview; `closed` (the default) shows the prop at its own place again.
  stopMoverPreview(thing, closed = true) {
    if (typeof thing.moverStop === 'function') {
      thing.moverStop();
    }
    thing.moverStop = null;
    if (closed && thing.moverShare) {
      thing.handle?.setPose(thing.at, { z: thing.turn });
      thing.moverShare = 0;
      thing.moverOpen = false;
    }
  }

  // A level mesh made your copy where it stands, which then opens and closes.
  levelToMover(thing) {
    this.moveLevel(thing, { ...thing.at }, thing.turn ?? 0);
    const copy = this.selected;
    if (copy && copy !== thing && copy.entry?.source === 'prop') {
      this.changeMover(copy, { ...MOVER_DEFAULTS }, 'opens');
      this.message = `${this.nameOf(copy)}: your copy opens and closes; set it below.`;
    }
    this.refresh();
  }

  // Opens or closes a level door in this game with the game's own door move (not an undo step:
  // the door is the level's, and the level's state comes back with the next load).
  swingDoor(thing, open) {
    this.api.send({ op: 'localDoor', guid: thing.object.guid, open });
    thing.door = { ...thing.door, open, locked: open ? false : thing.door.locked };
    this.message = `${this.nameOf(thing)}: ${open ? 'opening' : 'closing'}`;
    this.refresh();
  }

  levelDetails(thing) {
    const kinds = {
      vegetation: 'Level tree or plant',
      brush: 'Level object',
      entity: 'Level entity',
    };
    const object = thing.object;
    const rows = [
      { label: 'TRANSFORM' },
      { label: 'Location X', axis: 'x', value: metres(thing.at.x) },
      { label: 'Location Y', axis: 'y', value: metres(thing.at.y) },
      { label: 'Location Z', axis: 'z', value: metres(thing.at.z) },
      { label: 'FROM THE LEVEL' },
      {
        label: 'Hidden in this game',
        toggle: thing.hidden,
        press: () => {
          this.setLevelHidden(thing, !thing.hidden);
        },
      },
    ];
    if (thing.kind !== 'vegetation') {
      rows.push({
        label: 'Scale',
        value: `×${(thing.scale ?? 1).toFixed(2)}`,
        step: (dir) => this.scaleLevel(thing, dir),
      });
    }
    if (object.kind === 'entity') {
      rows.push({ label: 'Class', value: object.class || '-' });
      rows.push(...this.doorRows(thing));
      rows.push(...this.propertyRows(thing));
    } else if (thing.kind !== 'vegetation') {
      // A door or gate the level made a still mesh: your copy of it opens and closes.
      rows.push(
        { label: 'DOOR / GATE' },
        {
          label: 'Make it open',
          value: 'Your copy',
          press: () => this.levelToMover(thing),
        },
      );
    }
    rows.push(...this.gateRows(thing));
    return {
      icon: thing.kind === 'vegetation' ? 'tree' : 'box',
      title: this.nameOf(thing),
      kind: `${kinds[thing.kind] ?? 'Level object'} · from the game`,
      rows,
    };
  }

  selectionDetails(kind) {
    if (kind === 'level') {
      return this.levelDetails(this.selected);
    }
    const thing = this.selected;
    const at = this.selectedAt();
    const step = this.gridStep > 0 ? this.gridStep : 0.5;
    const nudge = (dx, dy) => {
      this.moveSelected({ x: at.x + dx, y: at.y + dy, z: at.z }, thing.turn);
    };
    const rows = [
      { label: 'TRANSFORM' },
      {
        label: 'Location X',
        axis: 'x',
        value: metres(at.x),
        step: (dir) => nudge(dir * step, 0),
      },
      {
        label: 'Location Y',
        axis: 'y',
        value: metres(at.y),
        step: (dir) => nudge(0, dir * step),
      },
      { label: 'Location Z', axis: 'z', value: metres(at.z) },
      {
        label: 'Rotation',
        value: `${thing.turn}°`,
        step: (dir) => this.moveSelected(at, degreesOf(thing.turn + dir * this.angleStep)),
      },
    ];
    if (kind === 'prop') {
      const materials = [null, ...this.userAssets().materials];
      const at = Math.max(0, materials.indexOf(thing.material ?? null));
      rows.push({
        label: 'Material',
        value: thing.material ? shortName(thing.material) : 'Its own',
        step: (dir) => {
          const next = materials[(at + dir + materials.length) % materials.length];
          this.changeMaterial(thing, next);
        },
      });
    }
    if (kind === 'vegetation' || kind === 'prop') {
      rows.push({
        label: 'Scale',
        value: `×${(thing.scale ?? 1).toFixed(2)}`,
        step: (dir) => this.resizeSelected(dir),
      });
    }
    if (kind === 'prop') {
      rows.push(...this.moverRows(thing));
    }
    let type = 'Foliage · solid, bends in the wind';
    const zone = kind === 'shape' && SHAPE_KINDS[thing.kind].zone === true;
    if (kind === 'shape') {
      type = zone
        ? 'Zone · yours · an area, blocks nothing'
        : 'Collision · yours · invisible, solid';
      rows.push({ label: 'SIZE' });
      const sides = [
        ['x', 'Width'],
        ['y', 'Depth'],
        ['z', 'Height'],
      ];
      for (const [axis, name] of sides) {
        rows.push({
          label: name,
          value: `${thing.size[axis].toFixed(1)} m`,
          step: (dir) => {
            this.changeShape('size', (shape) => ({
              size: this.collision.resized(shape, axis, dir),
            }));
          },
        });
      }
      if (zone) {
        rows.push(
          { label: 'AREA' },
          { label: 'Labels', value: SHAPE_KINDS[thing.kind].labels.join(', ') },
          { label: 'Area id', value: `architect-zone-${thing.serial}` },
        );
      } else {
        rows.push({ label: 'Stops', value: 'players, horses' });
      }
    } else if (kind === 'light') {
      type = 'Light · yours';
      const light = thing.light;
      const colorIndex = LIGHT_COLORS.findIndex(
        (entry) =>
          entry.color.x === light.color.x &&
          entry.color.y === light.color.y &&
          entry.color.z === light.color.z,
      );
      rows.push(
        { label: 'LIGHT' },
        {
          label: 'Colour',
          value: colorIndex >= 0 ? LIGHT_COLORS[colorIndex].label : 'Own',
          step: (dir) => {
            const next =
              LIGHT_COLORS[
                (Math.max(0, colorIndex) + dir + LIGHT_COLORS.length) % LIGHT_COLORS.length
              ];
            this.changeLight('light colour', () => ({ color: { ...next.color } }));
          },
        },
        {
          label: 'Intensity',
          value: light.intensity.toFixed(1),
          step: (dir) =>
            this.changeLight('light intensity', (now) => ({
              intensity:
                Math.round(Math.min(100, Math.max(0, now.intensity + dir * 0.5)) * 10) / 10,
            })),
        },
        {
          label: 'Radius',
          value: `${light.radius.toFixed(1)} m`,
          step: (dir) =>
            this.changeLight('light radius', (now) => ({
              radius: Math.round(Math.min(100, Math.max(0.5, now.radius + dir * 0.5)) * 10) / 10,
            })),
        },
        {
          label: 'Shadows',
          toggle: light.castShadows,
          press: () =>
            this.changeLight('light shadows', (now) => ({ castShadows: !now.castShadows })),
        },
      );
    } else if (kind === 'npc') {
      type = 'NPC · preview, not kept';
      rows.push(
        { label: 'WALK' },
        {
          label: 'Walk to',
          value: this.walker === thing ? 'Press the ground' : 'Choose a place',
          press: () => {
            this.walker = thing;
            this.message = 'Press where the NPC should walk to.';
            this.refresh();
          },
        },
        { label: 'Walk', value: walkLabel(thing.walk) },
      );
    } else if (kind === 'sound') {
      type = 'Sound · yours';
      rows.push({ label: 'SOUND' }, { label: 'Plays', value: thing.entry.name });
    } else if (kind === 'effect') {
      type = `Effect · ${thing.entry.name.split('.')[1] ?? ''}`;
      thing.fx ??= effectDefaults(thing.entry.name);
      const fx = thing.fx;
      const pulseAt = Math.max(0, EFFECT_PULSES.indexOf(fx.pulse));
      rows.push(
        { label: 'EFFECT' },
        {
          label: 'Size',
          value: `×${fx.scale}`,
          step: (dir) =>
            this.changeEffect('effect size', (now) => ({
              scale: Math.round(Math.min(32, Math.max(0.25, now.scale * 1.25 ** dir)) * 100) / 100,
            })),
        },
        {
          label: 'Plays again',
          value: fx.pulse > 0 ? `every ${fx.pulse} s` : 'no (runs on)',
          step: (dir) =>
            this.changeEffect('effect repeat', () => ({
              pulse: EFFECT_PULSES[(pulseAt + dir + EFFECT_PULSES.length) % EFFECT_PULSES.length],
            })),
        },
      );
    } else if (kind === 'prop') {
      type = `Prop · ${thing.entry.name.split('/').slice(1, -1).join(' / ')}`;
    } else if (kind === 'prefab') {
      type = `Prefab · ${thing.entry.name.split('/').slice(0, -1).join(' / ')}`;
      rows.push(
        { label: 'PLACEMENT' },
        { label: 'Ground', value: thing.pad ? 'fitted' : 'as it was' },
      );
    }
    rows.push(
      { label: 'ACTIONS' },
      { label: 'Focus', value: 'Focus  (F)', press: () => this.focusSelected() },
      { label: 'Duplicate', value: 'Duplicate  (Ctrl+D)', press: () => this.duplicateSelected() },
      { label: 'Delete', value: 'Delete  (Del)', press: () => this.deleteSelected() },
    );
    const icons = {
      shape: 'box',
      prefab: 'house',
      vegetation: 'tree',
      prop: 'box',
      effect: 'play',
      light: 'play',
    };
    return { title: this.nameOf(thing), kind: type, icon: icons[kind], rows };
  }

  pressDetail(row, side) {
    const entry = this.details().rows[row];
    if (!entry) {
      return;
    }
    if (entry.section) {
      if (this.foldedSections.has(entry.label)) {
        this.foldedSections.delete(entry.label);
      } else {
        this.foldedSections.add(entry.label);
      }
      this.refresh();
      return;
    }
    if (entry.step) {
      // A press on a value with steps: its - or + steps once, and while the button stays down a
      // move sideways steps on (scrubOn), all of it one undo step.
      const undoAt = this.undoStack.length;
      if (side !== 0) {
        entry.step(side);
      }
      const x = this.scrubX();
      this.scrub =
        x === null ? null : { label: entry.label, startX: x, applied: side, base: side, undoAt };
    } else if (typeof entry.toggle === 'boolean' && entry.press) {
      entry.press();
    } else if (entry.press) {
      entry.press();
    }
    this.refresh();
  }

  // ---- Content browser ----

  folderLabel(key) {
    if (FOLDER_LABELS[key]) {
      return FOLDER_LABELS[key];
    }
    return this.catalogue.categories().find((entry) => entry.key === key).label;
  }

  // The items of a folder: { name, meta, glyph, entry } with entry { label, name, source }.
  itemsOf(folder) {
    if (folder === 'brushes') {
      return BRUSHES.map((brush) => ({
        name: brush.label,
        meta: brush.meta,
        glyph: brush.glyph ?? brush.key,
        entry: { label: brush.label, name: brush.key, source: 'brush' },
      }));
    }
    if (folder === 'lights') {
      return LIGHT_PRESETS.map((preset) => ({
        name: preset.label,
        meta: `Light · ${preset.radius} m`,
        glyph: 'nature',
        entry: { label: preset.label, name: preset.key, source: 'light' },
      }));
    }
    if (folder === 'people') {
      return PEOPLE.map((person) => ({
        name: person.label,
        meta: 'NPC · walks on the navmesh',
        glyph: 'box',
        entry: { label: person.label, name: person.key, source: 'npc' },
      }));
    }
    if (folder === 'sounds') {
      return this.worldSounds().map(({ name, group }) => ({
        name: soundLabel(name),
        meta: group ? `Sound · ${group}` : 'Sound',
        glyph: 'nature',
        entry: soundEntry(name),
      }));
    }
    if (folder === 'shapes' || folder === 'zones') {
      return (folder === 'zones' ? ZONE_KEYS : SHAPE_KEYS).map((key) => {
        const { label, size } = SHAPE_KINDS[key];
        return {
          name: label,
          meta: `${size.x} × ${size.y} × ${size.z} m`,
          glyph: key,
          entry: { label, name: key, source: 'shape' },
        };
      });
    }
    const glyph = GLYPH_OF_FOLDER[folder];
    const metas = { vegetation: 'Foliage', prop: 'Prop', prefab: 'Prefab', effect: 'Effect' };
    return this.catalogue
      .entries(folder)
      .map((entry) => ({ name: entry.label, meta: metas[entry.source], glyph, entry }));
  }

  // The current folder's items through the content search.
  items() {
    const query = this.contentQuery.trim().toLowerCase();
    const all = this.itemsOf(this.folder);
    return query === '' ? all : all.filter((item) => item.name.toLowerCase().includes(query));
  }

  // ---- Validation ----

  // What may be wrong with the work so far, most serious first: { level, text, object, thing }.
  validate() {
    // Asked on every redraw: worked out again only when the work changed (thousands of plants
    // made every redraw slow).
    const key = [
      this.placed.length,
      this.collision.shapes.length,
      this.hiddenLevel.length,
      this.undoStack.length,
      this.redoStack?.length ?? 0,
    ].join(':');
    if (this.issuesCache?.key === key) {
      return this.issuesCache.issues;
    }
    const issues = this.findIssues();
    this.issuesCache = { key, issues };
    return issues;
  }

  findIssues() {
    const issues = [];
    const buildings = this.placed.filter((record) => record.entry.source === 'prefab');
    const plants = this.placed.filter((record) => record.entry.source === 'vegetation');
    const shapes = this.collision.shapes;
    for (let first = 0; first < buildings.length; first += 1) {
      for (let second = first + 1; second < buildings.length; second += 1) {
        if (overlap(this.flatBox(buildings[first]), this.flatBox(buildings[second]))) {
          issues.push({
            level: 'Warning',
            text: `Two buildings overlap: ${this.nameOf(buildings[first])} and ${this.nameOf(
              buildings[second],
            )}`,
            object: this.nameOf(buildings[second]),
            thing: buildings[second],
          });
        }
      }
    }
    const solids = [...buildings, ...shapes].map((thing) => ({ thing, box: this.flatBox(thing) }));
    for (const plant of plants) {
      const inside = solids.find((solid) => contains(solid.box, plant.at))?.thing;
      if (inside) {
        issues.push({
          level: 'Warning',
          text: `A plant stands inside ${this.nameOf(inside)}`,
          object: this.nameOf(plant),
          thing: plant,
        });
      }
    }
    for (const shape of shapes) {
      const building = buildings.find((thing) => {
        return within(this.flatBox(shape), this.flatBox(thing));
      });
      if (building) {
        issues.push({
          level: 'Note',
          text: `A collision shape lies wholly inside ${this.nameOf(
            building,
          )}, which is solid already`,
          object: this.nameOf(shape),
          thing: shape,
        });
      }
    }
    if (this.hiddenLevel.length > 0) {
      const count = this.hiddenLevel.length;
      issues.push({
        level: 'Note',
        text: `${count} of the level's own object${
          count === 1 ? ' is' : 's are'
        } hidden in this game`,
        object: 'Hidden from the level',
        thing: this.hiddenLevel[0],
      });
    }
    return issues;
  }

  // A thing's box on the ground plane: its four corners turned, as { minX, maxX, minY, maxY }.
  flatBox(thing) {
    const box = this.boxOf(thing);
    const turn = (box.turn * Math.PI) / 180;
    const xs = [];
    const ys = [];
    for (const px of [box.min[0], box.max[0]]) {
      for (const py of [box.min[1], box.max[1]]) {
        xs.push(box.at.x + px * Math.cos(turn) - py * Math.sin(turn));
        ys.push(box.at.y + px * Math.sin(turn) + py * Math.cos(turn));
      }
    }
    return {
      minX: Math.min(...xs),
      maxX: Math.max(...xs),
      minY: Math.min(...ys),
      maxY: Math.max(...ys),
    };
  }

  issuesView() {
    const issues = this.validate();
    const warnings = issues.filter((issue) => issue.level === 'Warning').length;
    const notes = issues.length - warnings;
    const shown =
      issues.length > 0
        ? issues
        : [{ level: 'Passed', text: 'No problems found in what you built.', object: '' }];
    return {
      issues: shown.map((issue) => ({
        level: issue.level,
        text: issue.text,
        object: issue.object,
      })),
      issueSummary: `${warnings} warning${warnings === 1 ? '' : 's'} · ${notes} note${
        notes === 1 ? '' : 's'
      } · a press goes to it`,
    };
  }

  // A press on an issue selects its object and flies the camera to it.
  pressIssue(index) {
    const issue = this.validate()[index];
    if (!issue?.thing) {
      return;
    }
    this.select(issue.thing);
    this.focusSelected();
  }

  pressTab(index) {
    this.tab = index;
    if (TABS[index] === 'Saves') {
      this.refreshSaves();
    }
    if (TABS[index] === 'AI') {
      this.ai = this.api.aiBridge?.() ?? null;
    }
    this.refresh();
  }

  // ---- AI assistants (architect/ai_bridge.hpp, Dev/tools/architect-mcp) ----

  // The AI tab: whether an assistant is connected, and what to give an AI app to connect one
  // (ai_connect.cjs, shared with the Asset/Animation Studio).
  aiView() {
    return aiConnectView(this.ai);
  }

  // Copies an AI tab row to the Windows clipboard.
  copyAi(row) {
    const entry = this.aiView().rows[row];
    if (!entry) {
      return;
    }
    this.api.send({ op: 'architectCopy', text: entry.text });
    this.message = `Copied: ${entry.label}. Paste it into your AI app.`;
    this.refresh();
  }

  // ---- Saves on this PC ----

  savesView() {
    const typing = this.typing === 'save';
    const named = this.saveName !== '';
    const two = (value) => String(value).padStart(2, '0');
    return {
      typing,
      named,
      name: named || typing ? `${this.saveName}${typing ? '|' : ''}` : 'Name of the save',
      folder: `Kept in ${SAVES_FOLDER}`,
      rows: this.saves.map((save) => {
        const when = new Date(Number(save.time) * 1000);
        const date = `${when.getFullYear()}-${two(when.getMonth() + 1)}-${two(when.getDate())}`;
        const time = `${two(when.getHours())}:${two(when.getMinutes())}`;
        const size = `${Math.max(1, Math.round(Number(save.bytes) / 1024))} kB`;
        return { name: save.name, when: `${date} ${time}  ·  ${size}` };
      }),
    };
  }

  // The saves the game client finds, newest first.
  refreshSaves() {
    const found = this.api.savesList?.();
    this.saves = Array.isArray(found)
      ? found
          .filter((save) => typeof save?.name === 'string')
          .sort((a, b) => Number(b.time) - Number(a.time))
      : [];
  }

  // The work as it is now, kept under the typed name.
  saveAs() {
    const name = this.saveName.trim();
    this.typing = null;
    if (!SAVE_NAME.test(name)) {
      this.message = 'A save is named with letters, digits, spaces, - or _ (up to 48).';
      this.refresh();
      return;
    }
    // In parts like the project (a message holds 64 KiB).
    const text = JSON.stringify(this.project());
    const parts = Math.max(1, Math.ceil(text.length / SAVE_PIECE));
    for (let part = 0; part < parts; part += 1) {
      const data = text.slice(part * SAVE_PIECE, (part + 1) * SAVE_PIECE);
      this.api.send(
        parts === 1
          ? { op: 'architectSaveAs', name, data }
          : { op: 'architectSaveAs', name, data, part, parts },
      );
    }
    this.message = `Saved "${name}" to ${SAVES_FOLDER}.`;
    this.refreshSaves();
    this.refresh();
  }

  // Load (the save instead of the work), Merge (the save added to the work) or Delete.
  pressSave(row, action) {
    const save = this.saves[row];
    if (!save) {
      return;
    }
    if (action === 'delete') {
      this.api.send({ op: 'architectSaveDelete', name: save.name });
      this.message = `Deleted "${save.name}".`;
      this.refreshSaves();
      this.refresh();
      return;
    }
    let project = null;
    try {
      project = JSON.parse(this.api.saveRead?.(save.name) ?? 'null');
    } catch {
      project = null;
    }
    if (!project || project.version !== PROJECT_VERSION) {
      this.message = `"${save.name}" could not be read.`;
    } else if (project.map !== this.map) {
      this.message = `"${save.name}" is for ${mapLabel(project.map)}: switch to that map first.`;
    } else if (action === 'load') {
      this.leaveWorld();
      this.keptProject = project;
      this.rebuildProject();
      this.changed();
      this.message = `Loaded "${save.name}".`;
    } else {
      this.keptProject = project;
      this.rebuildProject({ merge: true });
      this.changed();
      this.message = `Merged "${save.name}" into the work.`;
    }
    this.refresh();
  }

  pressFolder(index) {
    const key = MODES.find((mode) => mode.key === this.mode).folders[index];
    if (key) {
      this.folder = key;
      this.page = 0;
      this.refresh();
    }
  }

  pressTile(index) {
    const item = this.items()[this.page * TILE_COUNT + index];
    if (item) {
      this.arm(item.entry);
    }
  }

  // Arms a content entry: a brush becomes the terrain brush; anything else is put down with each
  // press in the world.
  arm(entry) {
    if (entry.source === 'brush') {
      this.terrainMode = entry.name;
      this.message = `${entry.label} brush: press in the world.`;
    } else {
      this.armed = entry;
      if (entry.source === 'vegetation') {
        this.naturePlant = entry;
      }
      this.select(null);
      this.message = `${entry.label}: press in the world to put it down, Escape stops.`;
    }
    this.refresh();
  }

  turnPage(step) {
    const pages = Math.max(1, Math.ceil(this.items().length / TILE_COUNT));
    this.page = (this.page + step + pages) % pages;
    this.refresh();
  }

  // A content entry by its name: a catalogue prefab or vegetation kind (full path), a terrain
  // brush ('raise', 'lower', 'flatten') or a collision shape ('wall', 'floor', 'ramp', 'box'). Its
  // mode, folder and page are shown and it is armed.
  choose(name) {
    for (const mode of MODES) {
      for (const folder of mode.folders) {
        const items = this.itemsOf(folder);
        const index = items.findIndex((item) => item.entry.name === name);
        if (index >= 0) {
          this.useMode(mode.key);
          this.folder = folder;
          this.page = Math.floor(index / TILE_COUNT);
          this.arm(items[index].entry);
          return true;
        }
      }
    }
    return false;
  }

  // Sends a PNG to the client's overlay once, under a key (64 KiB a message at most).
  sendPicture(key, bytes) {
    const piece = 36 * 1024;
    for (let offset = 0; offset === 0 || offset < bytes.length; offset += piece) {
      const part = bytes.subarray(offset, offset + piece);
      let binary = '';
      for (const byte of part) {
        binary += String.fromCharCode(byte);
      }
      this.api.send({
        op: 'architectPictureData',
        key,
        offset,
        total: bytes.length,
        data: btoa(binary),
      });
    }
  }

  // The pictures of the shown tiles that have none yet, a page at a time in the background while
  // the editor is open and nobody flies or types.
  async bakePictures() {
    if (this.baking || !this.screen || this.tab !== 0 || !STUDIO_FRAMING[this.folder]) {
      return;
    }
    this.baking = true;
    try {
      while (this.screen && !this.flying && this.tab === 0) {
        const framing = STUDIO_FRAMING[this.folder];
        const start = this.page * TILE_COUNT;
        const shown = this.items().slice(start, start + TILE_COUNT);
        const missing = shown.filter((item) => !this.pictures.has(item.entry.name));
        if (!framing || missing.length === 0) {
          break;
        }
        await this.takePictures(
          missing.map((item) => item.entry),
          framing,
        );
        this.refresh();
      }
    } finally {
      this.baking = false;
    }
  }

  // Places the items side by side in the sky over the camera, waits once for them to stand,
  // films each and takes them away again.
  async takePictures(entries, framing) {
    const { Prefab, Vegetation, Render, NoClip, Vector3 } = this.api;
    const camera = NoClip.getPosition();
    if (!camera) {
      for (const entry of entries) {
        this.pictures.set(entry.name, null);
      }
      return;
    }
    const spacing = framing.spacing ?? framing.distance * 3;
    const standing = entries.map((entry, index) => {
      const foot = {
        x: camera.x + index * spacing,
        y: camera.y,
        z: camera.z + STUDIO_HEIGHT,
      };
      let handle = null;
      try {
        if (entry.source === 'prefab') {
          const at = new Vector3(foot.x, foot.y, foot.z);
          handle = Prefab.place(entry.name, at, { x: 0, y: 0, z: 0 });
        } else if (entry.source === 'prop') {
          handle = this.propHandle(entry.name, foot, 0);
        } else {
          handle = Vegetation.plant(entry.name, foot, { turn: 0 });
        }
      } catch {
        handle = null;
      }
      return { entry, foot, handle };
    });
    await new Promise((resolve) => setTimeout(resolve, STUDIO_SETTLE_MS));
    for (const { entry, foot, handle } of standing) {
      if (!handle || !this.screen) {
        this.pictures.set(entry.name, null);
        continue;
      }
      let { distance, height } = framing;
      const box = framing.fit ? this.propBox(entry.name) : null;
      if (box) {
        // Far enough to see the whole box, looking at its middle.
        const size = Math.max(
          box.max[0] - box.min[0],
          box.max[1] - box.min[1],
          box.max[2] - box.min[2],
        );
        distance = Math.max(0.8, size * 1.7);
        height = (box.min[2] + box.max[2]) / 2;
      }
      // One picture at a time, with the editor's own work first (the editor
      // lagged until every picture was taken).
      await this.quietMoment();
      if (!this.screen) {
        break;
      }
      try {
        const shot = await Render.view({
          position: new Vector3(
            foot.x + distance * 0.7,
            foot.y - distance * 0.7,
            foot.z + height + distance * 0.35,
          ),
          lookAt: new Vector3(foot.x, foot.y, foot.z + height),
          fov: 40,
          width: PICTURE_WIDTH,
        });
        const key = `${PICTURE_PREFIX}${entry.name}`;
        this.sendPicture(key, dataBytes(shot.png));
        this.pictures.set(entry.name, key);
        this.refresh();
      } catch (error) {
        this.api.log(`World Architect: no picture of ${entry.name}: ${error.message}`);
        this.pictures.set(entry.name, null);
      }
    }
    for (const { handle } of standing) {
      handle?.destroy?.();
      handle?.remove?.();
    }
  }

  // Waits until nobody types, drags, holds a button or flies, then PICTURE_GAP_MS more: a picture
  // (a second render of the world) never takes a frame the author is using.
  async quietMoment() {
    // Typing pauses them only for a moment after each key (a search box that keeps the keys
    // left the tiles loading for good).
    const busy = () =>
      Date.now() - (this.lastTyped ?? 0) < TYPING_PAUSE_MS ||
      this.drag ||
      this.scrub ||
      this.scaleHold ||
      this.stroke ||
      this.flying ||
      this.api.Key.isDown('mouse1');
    do {
      await new Promise((resolve) => setTimeout(resolve, PICTURE_GAP_MS));
    } while (this.screen && busy());
  }

  // The pictures the game client keeps from earlier sessions: no need to take them again.
  picturesKept(keys) {
    for (const key of keys) {
      if (typeof key === 'string' && key.startsWith(PICTURE_PREFIX)) {
        this.pictures.set(key.slice(PICTURE_PREFIX.length), key);
      }
    }
    this.refresh();
  }

  // A picture of the current view, given to the first tile's entry (a test of the pictures).
  async pictureTest() {
    const { Render, NoClip } = this.api;
    const from = NoClip.getPosition();
    const ahead = NoClip.screenLine(0, 0);
    const shot = await Render.view({
      position: from,
      lookAt: { x: from.x + ahead.x * 10, y: from.y + ahead.y * 10, z: from.z + ahead.z * 10 },
      width: 310,
    });
    const bytes = dataBytes(shot.png);
    const first = this.items()[this.page * TILE_COUNT];
    this.sendPicture('test', bytes);
    if (first) {
      this.pictures.set(first.entry.name, 'test');
    }
    this.refresh();
    return { width: shot.width, height: shot.height, bytes: bytes.length };
  }

  // ---- State and drawing ----

  state() {
    return {
      open: this.isOpen(),
      mode: this.mode,
      tool: this.tool,
      chosen: this.armed ? this.armed.name : null,
      selected: this.selectionKind() ? this.nameOf(this.selected) : null,
      placed: this.placed.length,
      shapes: this.collision.shapes.length,
      steps: this.undoStack.length,
      message: this.message,
    };
  }

  refresh() {
    this.sightChanged();
    if (this.shell) {
      const selected = this.selectionKind() ? this.selected : null;
      // The Rotate tool shows a ring to turn the selection; the others its move arrows.
      this.marks.ringMode = this.tool === 'rotate';
      this.marks.select(selected ? this.boxOf(selected) : null);
      this.shell.render(this.view());
      this.drawOverlay();
      this.bakePictures();
    }
  }

  // What the screen shows now (Shell.render).
  view() {
    const snaps = [
      { label: this.gridStep > 0 ? `${this.gridStep} m` : 'Grid off', on: this.gridStep > 0 },
      { label: `${this.angleStep}°`, on: true },
      { label: 'Surface', on: this.fitGround },
    ];
    return {
      camera: { speed: this.cameraSpeed, axes: this.viewAxes(), hover: this.hover ?? null },
      menu: {
        map: this.map,
        mapLabel: mapLabel(this.map),
        maps: this.allMaps().map((map) => ({ ...map, current: map.key === this.map })),
        picker: this.mapPicker === null || this.mapPicker === 'choose' ? this.mapPicker : 'confirm',
        question: this.mapPicker
          ? `Switch to ${mapLabel(this.mapPicker)}? Your work here is saved first.`
          : '',
        unsaved: this.unsaved,
        saved:
          this.unsaved === 0
            ? 'Saved'
            : `${this.unsaved} unsaved change${this.unsaved === 1 ? '' : 's'}`,
      },
      toolbar: {
        tools: TOOLS.map((tool) => tool.key),
        tool: TOOLS.findIndex((tool) => tool.key === this.tool),
        snaps,
        modes: MODES.map((mode, index) => ({
          label: `${mode.label}  ${index + 1}${mode.later ? ' (later)' : ''}`,
          icon: mode.icon,
          later: Boolean(mode.later),
        })),
        mode: MODES.findIndex((mode) => mode.key === this.mode),
      },
      outliner: this.outlinerView(),
      details: this.details(),
      dock: this.dockView(),
      status: this.statusView(),
    };
  }

  outlinerView() {
    const lines = this.outlinerLines();
    const pages = Math.max(1, Math.ceil(lines.length / outlinerRows()));
    this.outlinerPage = Math.min(this.outlinerPage, pages - 1);
    const start = this.outlinerPage * outlinerRows();
    const shown = lines.slice(start, start + outlinerRows());
    const total = this.placed.length + this.collision.shapes.length;
    const filter = FILTERS[this.outlinerFilter];
    let search = `Search ${total} objects`;
    if (this.outlinerQuery !== '' || this.typing === 'outliner') {
      search = `${this.outlinerQuery}${this.typing === 'outliner' ? '|' : ''}`;
    }
    if (filter.key !== 'all' && this.outlinerQuery === '' && this.typing !== 'outliner') {
      search = `${filter.label} only`;
    }
    const selected = this.selectionKind() ? 1 : 0;
    return {
      search,
      query: this.outlinerQuery !== '',
      typing: this.typing === 'outliner',
      filtered: filter.key !== 'all',
      lines: shown,
      chosen: shown.findIndex((line) => line.thing !== null && line.thing === this.selected),
      footer: `${total} objects · ${selected} selected · page ${this.outlinerPage + 1} / ${pages}`,
      pager: ['<<', `${this.outlinerPage + 1} / ${pages}`, '>>'],
    };
  }

  dockView() {
    const folders = MODES.find((mode) => mode.key === this.mode).folders;
    const count = this.undoStack.length;
    const done = [...this.undoStack]
      .reverse()
      .map((command, index) => `${count - index}.  ${command.label}`);
    const undone = [...this.redoStack]
      .reverse()
      .map((command) => `      ${command.label}  (undone)`);
    const history = [...undone, ...done].slice(0, 8);
    const items = this.items();
    const pages = Math.max(1, Math.ceil(items.length / TILE_COUNT));
    this.page = Math.min(this.page, pages - 1);
    const start = this.page * TILE_COUNT;
    const shown = items.slice(start, start + TILE_COUNT);
    const modeLabel = MODES.find((mode) => mode.key === this.mode).label;
    const crumb = `${modeLabel}   ›   ${this.folderLabel(this.folder)}`;
    const armedName = this.mode === 'terrain' ? this.terrainMode : this.armed?.name;
    let search = 'Search this folder';
    if (this.contentQuery !== '' || this.typing === 'content') {
      search = `${this.contentQuery}${this.typing === 'content' ? '|' : ''}`;
    }
    return {
      tab: this.tab,
      tabs: TABS,
      folders: folders.slice(0, FOLDER_ROWS).map((key) => ({
        label: this.folderLabel(key),
        count: String(this.itemsOf(key).length),
      })),
      folder: folders.indexOf(this.folder),
      crumb,
      // The path's width in the game's small font, for the chip after it.
      crumbWidth: Math.round(crumb.length * 7.4),
      chip: `All ${items.length}`,
      search,
      query: this.contentQuery !== '',
      typing: this.typing === 'content',
      pager: ['<<', `${this.page + 1} / ${pages}`, '>>'],
      tiles: shown.map((item) => ({
        name: item.name,
        meta: item.meta,
        glyph: item.glyph,
        picture: this.pictures.get(item.entry.name) || null,
        loading: Boolean(STUDIO_FRAMING[this.folder]) && !this.pictures.has(item.entry.name),
      })),
      tile: shown.findIndex((item) => item.entry.name === armedName),
      history: history.length > 0 ? history : ['No changes yet.'],
      ...this.issuesView(),
      saves: this.savesView(),
      ai: this.aiView(),
    };
  }

  statusView() {
    const selected = this.selectionKind()
      ? `1 selected · ${this.nameOf(this.selected)}`
      : 'Nothing selected';
    const press = this.lastPress;
    const cursor = press
      ? `Cursor ${metres(press.x)}, ${metres(press.y)}, ${metres(press.z)}`
      : 'Cursor -';
    const total = this.placed.length + this.collision.shapes.length;
    const grid = this.gridStep > 0 ? `Grid ${this.gridStep} m` : 'Grid off';
    return {
      selected: `${selected} · ${this.message}`,
      cursor,
      snaps: `${grid} · ${this.angleStep}°`,
      objects: `${total} objects`,
    };
  }
}

// ---- Test builds: the kit's lines (Client/runtime/architect/local_mode.hpp) ----

// The bytes of a data URL's base64 payload.
function dataBytes(url) {
  const text = atob(String(url).slice(String(url).indexOf(',') + 1));
  const bytes = new Uint8Array(text.length);
  for (let index = 0; index < text.length; index += 1) {
    bytes[index] = text.charCodeAt(index);
  }
  return bytes;
}

// One line of the kit's script: what the editor does with it and its state after, as text.
async function runScript(editor, line) {
  const [word, ...rest] = line.trim().split(/\s+/);
  const number = (index) => Number(rest[index]);
  const steps = {
    open: () => editor.open(),
    close: () => editor.close(),
    mode: () => editor.useMode(rest[0]),
    tool: () => editor.useTool(rest[0]),
    choose: () => editor.choose(rest[0]),
    press: () => editor.pressWorld(number(0), number(1)),
    undo: () => editor.pressAction(0),
    redo: () => editor.pressAction(1),
    // Play as player, as its button does (the screen parked for Esc).
    play: () => editor.pressAction(ACTIONS.indexOf('Exit')),
    // `crossed x y`: the press's line and each prop of yours within 15 m of the camera: its box and
    // where the line enters it.
    crossed: () => {
      const hit = editor.lineAt(number(0), number(1));
      const camera = editor.api.NoClip.getPosition();
      const at = hit?.position;
      if (!at || !camera) {
        return null;
      }
      const line = { x: at.x - camera.x, y: at.y - camera.y, z: at.z - camera.z };
      return {
        camera: { x: camera.x, y: camera.y, z: camera.z },
        hit: at,
        level: hit.levelObject?.name ?? null,
        props: editor.placed
          .filter((record) => record.entry.source === 'prop')
          .filter((record) => Math.hypot(record.at.x - camera.x, record.at.y - camera.y) < 15)
          .map((record) => {
            const box = editor.boxOf(record);
            return { name: record.entry.label, box, entry: boxEntry(camera, line, box) };
          }),
      };
    },
    // Nothing selected (Details then shows the snaps, the exports and the world).
    deselect: () => {
      editor.select(null);
      editor.refresh();
    },
    // `stagepress x y`: a press at a stage point, as a click there; `wheel n`: n wheel notches.
    stagepress: () => editor.shell?.routePress(number(0), number(1), Date.now()),
    wheel: () => editor.shell?.wheel(number(0)),
    // `maplistrows n`: the map list shows n rows at once, so a few maps already scroll.
    maplistrows: () => {
      if (editor.shell) {
        editor.shell.mapListMost = Math.max(1, number(0));
      }
    },
    detail: () => editor.pressDetail(number(0), number(1)),
    outliner: () => editor.pressOutliner(number(0)),
    tile: () => editor.pressTile(number(0)),
    folder: () => editor.pressFolder(number(0)),
    filter: () => editor.turnOutlinerFilter(),
    find: () => {
      editor.outlinerQuery = rest.join(' ');
      editor.refresh();
    },
    search: () => {
      editor.contentQuery = rest.join(' ');
      editor.page = 0;
      editor.refresh();
    },
    picture: () => editor.pictureTest(),
    focus: () => editor.focusSelected(),
    // `view x y z tx ty tz`: the camera at a place looking at another in one pose (lookat aims
    // from the last reported place, so after campos in the same tick it aims from the old one).
    view: () => {
      const { Vector3 } = editor.api;
      const at = new Vector3(number(0), number(1), number(2));
      const forward = new Vector3(number(3) - at.x, number(4) - at.y, number(5) - at.z);
      editor.sightChanged();
      return editor.api.NoClip.setPose(at, forward);
    },
    look: () => {
      editor.restoreLook({
        hour: number(0),
        clock: number(1),
        weather: number(2),
        wind: number(3),
      });
      return editor.look;
    },
    map: () => {
      editor.chooseMap(rest[0]);
      editor.switchMap();
      return rest[0];
    },
    // The stage size, the hit at stage point (x, y) and that hit put back on the stage.
    probe: () => {
      const stage = editor.stageSize();
      const hit = editor.lineAt(number(0), number(1));
      const back = hit ? editor.api.NoClip.toScreen(hit.position) : null;
      const root = editor.screen?.root;
      return {
        stage,
        hit: hit ? hit.position : null,
        back: back
          ? { x: ((back.x + 1) / 2) * stage.width, y: ((1 - back.y) / 2) * stage.height }
          : null,
        mouse: root ? { x: root.get('_xmouse'), y: root.get('_ymouse') } : null,
      };
    },
    hover: () => {
      editor.testCursor = rest.length > 0 ? { x: number(0), y: number(1) } : null;
      editor.followCursor();
      return { ghost: Boolean(editor.ghost), pose: editor.ghostPose ?? null };
    },
    paint: () => {
      editor.paintCount = number(0);
      editor.refresh();
    },
    campos: () => {
      editor.sightChanged();
      return editor.api.NoClip.setPosition(new editor.api.Vector3(number(0), number(1), number(2)));
    },
    lookat: () => {
      editor.sightChanged();
      return editor.api.NoClip.lookAt(new editor.api.Vector3(number(0), number(1), number(2)));
    },
    camera: () => editor.api.NoClip.getPosition(),
    // Where the Rotate tool's ring about `axis` shows at angle t (radians), stage pixels.
    ringat: () => {
      const ring = (editor.marks.rings ?? []).find((entry) => entry.axis === rest[0]);
      return ring ? editor.stagePoint(ringPoint(ring, number(1))) : null;
    },
    // The level's loose brushes whose mesh names hold `word` within `radius` m of a point.
    brushes: () => {
      const at = { x: number(0), y: number(1), z: number(2) };
      const found = editor.api.levelBrushes?.(at, number(3), 256) ?? [];
      return found.filter((brush) => brush.mesh.includes(rest[4] ?? '')).slice(0, 12);
    },
    // An AI assistant's JSON request line, as the AI bridge hands it over.
    ai: () => {
      if (!editor.aiCommands) {
        editor.aiCommands = new AiCommands(editor, { MODES, MAPS, CLOCK_SPEEDS, WEATHERS, WINDS });
      }
      return editor.aiCommands.handle(rest.join(' '));
    },
    dup: () => editor.duplicateSelected(),
    del: () => editor.deleteSelected(),
    tab: () => editor.pressTab(number(0)),
    issues: () => editor.validate().map((issue) => issue.text),
    pieces: () => {
      const pieces = editor.api.prefabPieces?.(rest[0]) ?? [];
      const known = pieces.filter((piece) => editor.api.meshKnown?.(piece.mesh));
      return { pieces: pieces.length, known: known.length, first: pieces[0] ?? null };
    },
  };
  const step = steps[word];
  if (!step) {
    return `unknown ${word}`;
  }
  const result = await step();
  return `${line} -> ${JSON.stringify(result ?? null)} ${JSON.stringify(editor.state())}`;
}

// The WorldArchitect object client scripts see (kcds.ui.WorldArchitect).
function architectFacade(editor) {
  return Object.freeze({
    // The pictures the game client keeps on disk (worker.cjs 'architectPicturesKept').
    picturesKept: (keys) => editor.picturesKept(keys),
    // The project the game client keeps for the map (worker.cjs 'architectProject').
    projectKept: (map, data, part, parts) => editor.projectKept(map, data, part, parts),
    // The cursor and its hit from the game client (worker.cjs 'architectCursorAt').
    cursorAt: (data) => editor.cursorAt(data),
    // Test builds only: a line of the kit's script (worker.cjs 'architectTest').
    script: (line) => runScript(editor, line),
    // An AI assistant's request line, answered with one line (worker.cjs 'architectCommand').
    command: (text) => {
      if (!editor.aiCommands) {
        editor.aiCommands = new AiCommands(editor, { MODES, MAPS, CLOCK_SPEEDS, WEATHERS, WINDS });
      }
      return editor.aiCommands.handle(text);
    },
    open: () => editor.open(),
    close: () => editor.close(),
    isOpen: () => editor.isOpen(),
    useMode: (key) => editor.useMode(String(key)),
    useTool: (key) => editor.useTool(String(key)),
    choose: (name) => editor.choose(String(name)),
    pressAt: (x, y) => {
      if (!editor.isOpen() || !Number.isFinite(x) || !Number.isFinite(y)) {
        return false;
      }
      editor.pressWorld(x, y);
      return true;
    },
    undo: () => {
      editor.undo();
      editor.refresh();
      return editor.state().steps;
    },
    redo: () => {
      editor.redo();
      editor.refresh();
      return editor.state().steps;
    },
    state: () => editor.state(),
  });
}

module.exports = { ArchitectEditor, architectFacade, TOOLS, MODES };
