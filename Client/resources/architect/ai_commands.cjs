'use strict';

// World Architect for AI assistants: the commands an MCP server
// (Dev/tools/architect-mcp) sends through the game client (architect/ai_bridge.hpp). A request is
// one JSON line { id, command, args }; the answer is { id, ok, result, message, state } or
// { id, ok: false, error }. Every change goes through the editor's own steps, so Undo undoes it
// as if done by hand. Things of the work are named by ids given here (stable while the editor
// runs).

// How far up and down from zero the ground under a point without a height is looked for.
const GROUND_SEARCH = 3000;
// A picture taken farther than this from the editor camera brings the camera there first, and
// the time the game takes to draw the people there.
const CAMERA_NEAR = 5;
const PEOPLE_SETTLE_MS = 600;
// The ground materials the paint command puts on (Terrain.paint's names).
const PAINT_MATERIALS = Object.freeze([
  'grass',
  'meadow',
  'pasture',
  'trampled',
  'soil',
  'mud',
  'stony_mud',
  'sand',
  'gravel',
  'forest',
  'forest_leaves',
  'cobblestones',
  'paving',
  'cobblestones_grass',
  'river_shore',
]);

const isNumber = (value) => typeof value === 'number' && Number.isFinite(value);

function pointOf(value, what) {
  if (!value || !isNumber(value.x) || !isNumber(value.y)) {
    throw new Error(`${what} needs { x, y } (and z where known)`);
  }
  return { x: value.x, y: value.y, z: isNumber(value.z) ? value.z : null };
}

function pointsOf(value, what) {
  if (!Array.isArray(value) || value.length === 0) {
    throw new Error(`${what} needs a list of points`);
  }
  return value.map((point) => pointOf(point, what));
}

// The points of a stroke with more between them, `spacing` metres apart at most.
function along(points, spacing) {
  const out = [points[0]];
  for (let index = 1; index < points.length; index += 1) {
    const from = points[index - 1];
    const to = points[index];
    const length = Math.hypot(to.x - from.x, to.y - from.y);
    const steps = Math.max(1, Math.ceil(length / spacing));
    for (let step = 1; step <= steps; step += 1) {
      const t = step / steps;
      out.push({ x: from.x + (to.x - from.x) * t, y: from.y + (to.y - from.y) * t });
    }
  }
  return out;
}

const rounded = (value) => Math.round(value * 1000) / 1000;
const placeOf = (at) => ({ x: rounded(at.x), y: rounded(at.y), z: rounded(at.z) });

// place_many: how many at once, and how much of a building's ground may lie in another.
const PLACE_MANY_MOST = 300;
const OVERLAP_MOST = 0.25;
// fill_row: the shortest gap left at a row's end and how far a house may pass it. By default it
// takes the game's city houses whose street front is known (house_fronts.cjs): their doors come
// with them, as nested door prefabs.
const ROW_SMALLEST = 5;
const ROW_SLACK = 2;
// A building's box has 3 m of room around its pieces (editor.cjs PIECE_ROOM); its walls stand
// about this far inside it, so a row's houses touch and its fronts stand on the street line.
const ROW_INSET = 2.5;

// A step through n items that reaches every one: the smallest number above n / 3 sharing no
// factor with n.
function strideFor(n) {
  const gcd = (a, b) => (b === 0 ? a : gcd(b, a % b));
  for (let stride = Math.max(1, Math.floor(n / 3) + 1); stride < n; stride += 1) {
    if (gcd(stride, n) === 1) {
      return stride;
    }
  }
  return 1;
}

function inside([x, y], polygon) {
  let result = false;
  for (let i = 0, j = polygon.length - 1; i < polygon.length; j = i, i += 1) {
    const [ax, ay] = polygon[i];
    const [bx, by] = polygon[j];
    if (ay > y !== by > y && x < ((bx - ax) * (y - ay)) / (by - ay) + ax) {
      result = !result;
    }
  }
  return result;
}

// How much of an outline's ground (sampled 5 x 5) lies in any of the others.
function overlapShare(outline, others) {
  if (others.length === 0) {
    return 0;
  }
  const [a, b, , d] = outline;
  let hits = 0;
  for (let i = 0; i < 5; i += 1) {
    for (let j = 0; j < 5; j += 1) {
      const u = (i + 0.5) / 5;
      const v = (j + 0.5) / 5;
      const point = [
        a[0] + (b[0] - a[0]) * u + (d[0] - a[0]) * v,
        a[1] + (b[1] - a[1]) * u + (d[1] - a[1]) * v,
      ];
      if (others.some((other) => inside(point, other))) {
        hits += 1;
      }
    }
  }
  return hits / 25;
}

// One decimal, for sizes in metres.
function round1(value) {
  return Math.round(value * 10) / 10;
}

// The catalogue's size filter: { width, depth, height } each [min, max] in metres; null for none.
function sizeRange(args) {
  const range = {};
  let any = false;
  for (const axis of ['width', 'depth', 'height']) {
    const min = args[`min${axis[0].toUpperCase()}${axis.slice(1)}`];
    const max = args[`max${axis[0].toUpperCase()}${axis.slice(1)}`];
    if (min === undefined && max === undefined) {
      continue;
    }
    any = true;
    range[axis] = [Number(min ?? -Infinity), Number(max ?? Infinity)];
  }
  return any ? range : null;
}

function fits(size, range) {
  return Object.entries(range).every(
    ([axis, [min, max]]) => size[axis] >= min && size[axis] <= max,
  );
}

const { HOUSE_FRONTS } = require('./house_fronts.cjs');

class AiCommands {
  constructor(editor, tables) {
    this.editor = editor;
    this.tables = tables;
    // Thing -> id, and the next id.
    this.ids = new WeakMap();
    this.nextId = 1;
    // Level objects picked (not hidden yet): listed so that they have ids.
    this.picked = [];
  }

  idOf(thing) {
    if (!this.ids.has(thing)) {
      this.ids.set(thing, this.nextId);
      this.nextId += 1;
    }
    return this.ids.get(thing);
  }

  // Every thing of the work: placed buildings and plants, collision shapes, hidden level objects.
  things() {
    const editor = this.editor;
    const listed = [...editor.placed, ...editor.collision.shapes, ...editor.hiddenLevel];
    return [...listed, ...this.picked.filter((thing) => !listed.includes(thing))];
  }

  describe(thing) {
    const editor = this.editor;
    const selected = editor.selected;
    editor.selected = thing;
    const kind = editor.selectionKind();
    const at = editor.selectedAt();
    editor.selected = selected;
    const described = {
      id: this.idOf(thing),
      kind,
      name: kind === 'level' ? thing.object.name : kind === 'shape' ? thing.kind : thing.entry.name,
      label: editor.nameOf(thing),
      at: placeOf(at),
      turn: thing.turn ?? 0,
    };
    if (kind === 'shape') {
      described.size = { ...thing.size };
    }
    if (kind === 'level') {
      described.hidden = Boolean(thing.hidden);
      // An entity of the level (a door, a light, a spawn) says its class: properties, set_property
      // and door take only these.
      if (thing.object.kind === 'entity') {
        described.entity = thing.object.class ?? true;
      }
    }
    if ((thing.scale ?? 1) !== 1) {
      described.scale = thing.scale;
    }
    // Its box in its own frame (before its turn, around its place), metres: how wide, deep and
    // tall it stands, to line things up (house fronts side by side) without overlaps. A prop the
    // game has not measured yet has a 1 m stand-in box until it is listed again.
    const box = kind === 'shape' ? null : editor.boxOf(thing);
    if (box) {
      const round = (value) => Math.round(value * 10) / 10;
      described.box = { min: box.min.map(round), max: box.max.map(round) };
    }
    return described;
  }

  thingById(id) {
    const thing = this.things().find((item) => this.ids.get(item) === id);
    if (!thing) {
      throw new Error(`no thing with id ${id} (list the things first)`);
    }
    return thing;
  }

  // The ground's height under the points that have no z.
  grounded(points) {
    const missing = points.filter((point) => point.z === null);
    if (missing.length > 0) {
      // Looked for from far above to far below: the point gives no height to start from.
      const grounds = this.editor.groundsOf(
        missing.map((point) => ({ ...point, z: 0 })),
        GROUND_SEARCH,
      );
      missing.forEach((point, index) => {
        if (grounds[index] === null) {
          throw new Error(`no ground found at ${point.x} ${point.y}`);
        }
        point.z = grounds[index];
      });
    }
    return points;
  }

  // A stroke of a terrain or nature brush over the points, one undo step (as held by hand).
  stroke(mode, points, size, strength) {
    const editor = this.editor;
    editor.useMode('terrain');
    editor.terrainMode = mode;
    if (isNumber(size)) {
      editor.brushSize = Math.max(1, Math.min(60, size));
    }
    if (isNumber(strength)) {
      editor.brushStrength = Math.max(0.05, Math.min(20, strength));
    }
    const grounded = this.grounded(points);
    const steps = editor.undoStack.length;
    editor.shapeGround(grounded[0]);
    const stroke = editor.stroke;
    editor.stroke = null;
    if (!stroke) {
      throw new Error(editor.message || 'the brush did nothing there');
    }
    for (const at of grounded.slice(1)) {
      const edit = editor.dab(stroke, at);
      if (edit) {
        stroke.dabs.push(at);
        stroke.edits.push(edit);
      }
    }
    editor.changed?.();
    return { dabs: stroke.dabs.length, steps: editor.undoStack.length - steps };
  }

  catalogue(args) {
    const editor = this.editor;
    const query = String(args.query ?? '').toLowerCase();
    const limit = Math.max(1, Math.min(500, args.limit ?? 50));
    const folderWanted = args.folder === undefined ? null : String(args.folder);
    // Sizes: every building's (its pieces give its box at once); a prop's asks the game, so only
    // with sizes: true.
    const wantSizes = args.sizes === true;
    const range = sizeRange(args);
    const found = [];
    for (const mode of this.tables.MODES) {
      for (const folder of mode.folders) {
        if (folderWanted && folder !== folderWanted) {
          continue;
        }
        for (const item of editor.itemsOf(folder)) {
          const entry = item.entry;
          if (query && !entry.name.toLowerCase().includes(query)) {
            continue;
          }
          const row = { name: entry.name, label: entry.label, source: entry.source, folder };
          if (entry.source === 'prefab' || (wantSizes && entry.source === 'prop')) {
            const box = editor.boxOf({ entry, at: { x: 0, y: 0, z: 0 }, turn: 0 });
            row.box = { min: box.min.map(round1), max: box.max.map(round1) };
            row.size = {
              width: round1(box.max[0] - box.min[0]),
              depth: round1(box.max[1] - box.min[1]),
              height: round1(box.max[2] - box.min[2]),
            };
            if (HOUSE_FRONTS[entry.name]) {
              row.front = HOUSE_FRONTS[entry.name];
            }
          }
          if (range && !(row.size && fits(row.size, range))) {
            continue;
          }
          found.push(row);
        }
      }
    }
    return { total: found.length, items: found.slice(0, limit) };
  }

  place(args) {
    const editor = this.editor;
    const name = String(args.name ?? '');
    const [at] = this.grounded([pointOf(args.at, 'place')]);
    if (!editor.choose(name)) {
      throw new Error(`"${name}" is not in the content (search it with catalogue)`);
    }
    const entry = editor.armed;
    editor.turn = isNumber(args.turn) ? args.turn : 0;
    if (typeof args.fitGround === 'boolean') {
      editor.fitGround = args.fitGround;
    }
    const before = new Set(this.things());
    if (entry.source === 'shape') {
      editor.addShape(at);
    } else {
      editor.place(at);
    }
    editor.armed = null;
    editor.tool = 'select';
    editor.dropGhost?.();
    const made = this.things().find((thing) => !before.has(thing));
    if (!made) {
      throw new Error(editor.message || `the game could not put "${name}" there`);
    }
    return this.describe(made);
  }

  // A building's ground outline (four corners) where it would stand.
  // inset: metres taken off each side (a building's box has room around its pieces).
  outlineOf(entry, at, turn, inset = 0) {
    const box = this.editor.boxOf({ entry, at, turn });
    const t = (turn * Math.PI) / 180;
    const c = Math.cos(t);
    const s = Math.sin(t);
    const [x0, y0] = [box.min[0] + inset, box.min[1] + inset];
    const [x1, y1] = [box.max[0] - inset, box.max[1] - inset];
    const corners = [
      [x0, y0],
      [x1, y0],
      [x1, y1],
      [x0, y1],
    ];
    return corners.map(([x, y]) => [at.x + x * c - y * s, at.y + x * s + y * c]);
  }

  // The outlines of the buildings and props placed now.
  standingOutlines(inset = 0) {
    return this.editor.placed
      .filter((record) => record.entry?.source === 'prefab' || record.entry?.source === 'prop')
      .map((record) => this.outlineOf(record.entry, record.at, record.turn ?? 0, inset));
  }

  // Many placements in one call: [{ name, at, turn }]. With avoidOverlap, a building that would
  // stand in another (more than a quarter of its ground) is left out and named in skipped.
  placeMany(args) {
    const items = Array.isArray(args.items) ? args.items : null;
    if (!items || items.length === 0 || items.length > PLACE_MANY_MOST) {
      throw new Error(`place_many takes 1 to ${PLACE_MANY_MOST} items`);
    }
    const avoid = args.avoidOverlap === true;
    const inset = isNumber(args.inset) ? args.inset : 0;
    const outlines = avoid ? this.standingOutlines(inset) : [];
    const placed = [];
    const skipped = [];
    for (const item of items) {
      const name = String(item.name ?? '');
      const entry = this.catalogueEntry(name);
      if (avoid && entry) {
        const at = pointOf(item.at, 'place_many');
        const outline = this.outlineOf(entry, at, isNumber(item.turn) ? item.turn : 0, inset);
        if (overlapShare(outline, outlines) > OVERLAP_MOST) {
          skipped.push({ name, at: item.at, reason: 'stands in another' });
          continue;
        }
        outlines.push(outline);
      }
      try {
        const made = this.place({ fitGround: args.fitGround, ...item });
        placed.push({ id: made.id, name: made.name });
      } catch (error) {
        skipped.push({ name, at: item.at, reason: String(error.message ?? error) });
      }
    }
    return { placed, skipped };
  }

  // A street front from `from` to `to` lined with buildings side by side, their fronts (a town
  // house's -y side) toward `facing`, 'left' or 'right' of the walk from `from` to `to`. The
  // buildings come from `names` (default: the game's city houses, with their doors), none deeper than `depth`,
  // taken in turn so neighbours differ.
  fillRow(args) {
    const from = pointOf(args.from, 'fill_row');
    const to = pointOf(args.to, 'fill_row');
    const dx = to.x - from.x;
    const dy = to.y - from.y;
    const length = Math.hypot(dx, dy);
    if (length < 4) {
      throw new Error('fill_row needs from and to at least 4 m apart');
    }
    const ux = dx / length;
    const uy = dy / length;
    const side = args.facing === 'right' ? -1 : 1;
    const nx = -uy * side;
    const ny = ux * side;
    const turn = (Math.atan2(nx, -ny) * 180) / Math.PI;
    const depth = isNumber(args.depth) ? args.depth : 40;
    const pool = this.rowPool(args.names, depth);
    if (pool.length === 0) {
      throw new Error('fill_row found no building of that depth');
    }
    const items = [];
    let along = 0;
    // A row starts anywhere in the pool and steps through it with a stride that visits all, so two
    // rows are not the same houses in the same order.
    const random = typeof this.editor.random === 'function' ? this.editor.random() : Math.random();
    let index = Math.floor(random * pool.length);
    const stride = strideFor(pool.length);
    let tries = 0;
    while (along < length - ROW_SMALLEST && tries < pool.length) {
      const house = pool[index % pool.length];
      index += stride;
      tries += 1;
      if (house.width > length - along + ROW_SLACK) {
        continue;
      }
      // The house's front middle on the street line, and from there its origin. A house whose
      // front is its +y side turns half round more.
      const middle = along + house.width / 2;
      const fx = from.x + ux * middle;
      const fy = from.y + uy * middle;
      const plusFront = house.front === '+y';
      const houseTurn = plusFront ? turn + 180 : turn;
      const t = (houseTurn * Math.PI) / 180;
      const lx = (house.box.min[0] + house.box.max[0]) / 2;
      const ly = plusFront ? house.box.max[1] - ROW_INSET : house.box.min[1] + ROW_INSET;
      const at = {
        x: fx - (lx * Math.cos(t) - ly * Math.sin(t)),
        y: fy - (lx * Math.sin(t) + ly * Math.cos(t)),
      };
      if (isNumber(from.z)) {
        at.z = from.z;
      }
      items.push({ name: house.name, at, turn: houseTurn });
      along += house.width;
      tries = 0;
    }
    return this.placeMany({
      items,
      avoidOverlap: args.avoidOverlap !== false,
      fitGround: true,
      inset: ROW_INSET,
    });
  }

  // The buildings fill_row may use: the names given, or Kuttenberg's town houses, with sizes.
  rowPool(names, depth) {
    const wanted = Array.isArray(names) ? names.map(String) : null;
    const rows = this.catalogue({ folder: 'houses', limit: 500 }).items.filter((row) => {
      if (!row.size) {
        return false;
      }
      if (wanted) {
        return wanted.includes(row.name);
      }
      return Boolean(HOUSE_FRONTS[row.name]);
    });
    return rows
      .filter((row) => row.size.depth <= depth + ROW_SLACK)
      .map((row) => ({
        name: row.name,
        width: row.size.width - 2 * ROW_INSET,
        box: row.box,
        front: row.front ?? '-y',
      }));
  }

  // A catalogue entry by its full name, or null.
  catalogueEntry(name) {
    for (const mode of this.tables.MODES) {
      for (const folder of mode.folders) {
        for (const item of this.editor.itemsOf(folder)) {
          if (item.entry.name === name) {
            return item.entry;
          }
        }
      }
    }
    return null;
  }

  move(args) {
    const editor = this.editor;
    const thing = this.thingById(args.id);
    editor.select(thing);
    const kind = editor.selectionKind();
    const at = args.at ? this.grounded([pointOf(args.at, 'move')])[0] : editor.selectedAt();
    const turn = isNumber(args.turn) ? args.turn : thing.turn ?? 0;
    const before = new Set(this.things());
    editor.moveSelected(at, turn);
    // A level object is hidden and your copy stands there: the copy is the answer.
    const copy = kind === 'level' ? this.things().find((item) => !before.has(item)) : null;
    if (kind === 'level' && !copy) {
      throw new Error(editor.message || 'the level object could not be moved');
    }
    return this.describe(copy ?? thing);
  }

  // A picture of the world without the editor's panels: from the editor camera, or from
  // `position` looking at `lookAt`. Answers { png (base64), width, height } (a promise).
  async screenshot(args) {
    const { Render, NoClip } = this.editor.api;
    const width = Math.max(320, Math.min(1600, Math.floor(args.width ?? 960)));
    let from = args.position ? pointOf(args.position, 'screenshot position') : NoClip.getPosition();
    if (!from) {
      throw new Error('the editor camera is not ready');
    }
    from = { x: from.x, y: from.y, z: from.z ?? 0 };
    let target = null;
    if (args.lookAt) {
      const at = pointOf(args.lookAt, 'screenshot lookAt');
      target = { x: at.x, y: at.y, z: at.z ?? 0 };
    } else {
      const ahead = NoClip.screenLine(0, 0);
      target = { x: from.x + ahead.x * 10, y: from.y + ahead.y * 10, z: from.z + ahead.z * 10 };
    }
    // The game draws people near the editor camera only (the world's observer): a picture taken
    // elsewhere has the camera brought there first and back after (a preview
    // NPC was missing from every picture taken away from the camera).
    const home = NoClip.getPosition();
    const away =
      home && Math.hypot(home.x - from.x, home.y - from.y, home.z - from.z) > CAMERA_NEAR;
    if (away) {
      this.editor.sightChanged?.();
      NoClip.setPosition(new this.editor.api.Vector3(from.x, from.y, from.z));
      await new Promise((resolve) => setTimeout(resolve, PEOPLE_SETTLE_MS));
    }
    // The panels are in the game's own frame (the text is not): hidden while the picture is taken.
    const shell = this.editor.shell;
    shell?.setShown(false);
    let shot = null;
    try {
      shot = await Render.view({ position: from, lookAt: target, width, fov: 60 });
    } finally {
      shell?.setShown(true);
      if (away) {
        NoClip.setPosition(new this.editor.api.Vector3(home.x, home.y, home.z));
      }
    }
    const png = String(shot.png ?? '');
    return { png: png.slice(png.indexOf(',') + 1), width: shot.width, height: shot.height };
  }

  run(command, args) {
    const editor = this.editor;
    const { MAPS, CLOCK_SPEEDS, WEATHERS, WINDS } = this.tables;
    switch (command) {
      case 'state':
        return {
          ...editor.state(),
          map: editor.map,
          maps: (editor.allMaps?.() ?? MAPS).map((map) => map.key),
          look: {
            hour: editor.look.hour,
            clock: CLOCK_SPEEDS[editor.look.clock].label,
            weather: WEATHERS[editor.look.weather].label,
            wind: WINDS[editor.look.wind].label,
          },
          camera: editor.api.NoClip.getPosition(),
          brush: {
            mode: editor.terrainMode,
            size: editor.brushSize,
            strength: editor.brushStrength,
          },
        };
      case 'open':
        editor.open();
        return editor.isOpen();
      case 'close':
        // Play as player, as the editor's own button does: kept for the way back.
        editor.close({ park: true });
        return !editor.isOpen();
      case 'switch_map': {
        const map = String(args.map ?? '');
        const maps = editor.allMaps?.() ?? MAPS;
        if (!maps.some((entry) => entry.key === map)) {
          throw new Error(`the maps are ${maps.map((entry) => entry.key).join(', ')}`);
        }
        editor.chooseMap(map);
        editor.switchMap();
        return { switching: map, note: 'the editor opens again in the new map (about 15 s)' };
      }
      case 'camera': {
        const { NoClip, Vector3 } = editor.api;
        if (args.position) {
          const at = pointOf(args.position, 'camera position');
          editor.sightChanged();
          NoClip.setPosition(new Vector3(at.x, at.y, at.z ?? 0));
        }
        if (args.lookAt) {
          const at = pointOf(args.lookAt, 'camera lookAt');
          editor.sightChanged();
          NoClip.lookAt(new Vector3(at.x, at.y, at.z ?? 0));
        }
        return NoClip.getPosition();
      }
      case 'look': {
        const index = (list, label, fallback) => {
          if (label === undefined) {
            return fallback;
          }
          const found = list.findIndex(
            (item) => item.label.toLowerCase() === String(label).toLowerCase(),
          );
          if (found < 0) {
            throw new Error(`choose one of: ${list.map((item) => item.label).join(', ')}`);
          }
          return found;
        };
        const look = editor.look;
        let clock = index(CLOCK_SPEEDS, args.clock, look.clock);
        if (args.freeze_time === true) {
          clock = 0;
        } else if (args.freeze_time === false && clock === 0) {
          clock = look.runningClock ?? 2;
        }
        editor.restoreLook({
          hour: isNumber(args.hour) ? Math.round(args.hour * 2) / 2 : look.hour,
          clock,
          weather: index(WEATHERS, args.weather, look.weather),
          wind: index(WINDS, args.wind, look.wind),
          weatherHeld:
            typeof args.freeze_weather === 'boolean' ? args.freeze_weather : look.weatherHeld,
        });
        editor.changed?.();
        return this.run('state', {}).look;
      }
      case 'scale': {
        // Steps of the Scale tool (each ×1.1 up, ÷1.1 down) on a thing: a plant, a prop, a
        // collision shape or zone, or a level object (in place); one undo step.
        const steps = Number.isInteger(args.steps) ? args.steps : 0;
        if (steps === 0 || Math.abs(steps) > 30) {
          throw new Error('steps is a whole number from -30 to 30, not 0');
        }
        const thing = this.thingById(args.id);
        editor.select(thing);
        const undoAt = editor.undoStack.length;
        for (let step = 0; step < Math.abs(steps); step += 1) {
          editor.scaleSelected(Math.sign(steps));
        }
        editor.mergeSteps(undoAt);
        return this.describe(thing);
      }
      case 'export_blender':
        return editor.exportForBlender();
      case 'export_server':
        return editor.exportForServer();
      case 'fly_speed': {
        if (!isNumber(args.speed) || args.speed < 1 || args.speed > 200) {
          throw new Error('speed is 1 to 200 metres a second');
        }
        editor.setCameraSpeed(args.speed);
        return { speed: editor.cameraSpeed };
      }
      case 'properties': {
        // A level entity's own properties (an id from pick), as Details shows them.
        // An id from pick, or what is selected now.
        const thing = args.id !== undefined ? this.thingById(args.id) : editor.selected;
        if (!thing?.level || thing.object.kind !== 'entity') {
          throw new Error('that is no level entity: pick a door, a light or a spawn first');
        }
        editor.propertyRows(thing);
        return (thing.properties ?? []).map(({ key, type, value }) => ({ key, type, value }));
      }
      case 'set_property': {
        // Sets one of them in this game (a bool or a number; texts are only shown).
        const thing = args.id !== undefined ? this.thingById(args.id) : editor.selected;
        if (!thing?.level || thing.object.kind !== 'entity') {
          throw new Error('that is no level entity: pick one first');
        }
        editor.propertyRows(thing);
        const property = (thing.properties ?? []).find((entry) => entry.key === args.key);
        if (!property) {
          throw new Error(`it has no property ${args.key} (ask "properties")`);
        }
        const fits =
          (property.type === 'bool' && typeof args.value === 'boolean') ||
          (property.type === 'number' && isNumber(args.value));
        if (!fits) {
          throw new Error(
            `${args.key} takes a ${
              property.type === 'text' ? 'nothing (shown only)' : property.type
            }`,
          );
        }
        property.value = args.value;
        editor.api.send({
          op: 'localEntityProperty',
          guid: thing.object.guid,
          key: property.key,
          value: args.value,
        });
        return { key: property.key, value: property.value };
      }
      case 'door': {
        const thing = this.thingById(args.id);
        if (!thing?.level || thing.object.kind !== 'entity') {
          throw new Error('that is no level entity: pick a door first');
        }
        editor.doorRows(thing);
        if (!thing.door) {
          throw new Error('that level entity is no door');
        }
        editor.swingDoor(thing, args.open === true);
        return { open: thing.door.open };
      }
      case 'bake_navmesh': {
        // A custom map's navmesh from the ground as it is now, for the next start.
        const answer = editor.bakeNavmesh();
        if (!answer) {
          throw new Error(editor.message);
        }
        return answer;
      }
      case 'walk': {
        // The selected preview NPC (People) walks to a point by the game's walk on the navmesh;
        // npc_state tells how it went.
        const thing = editor.selected;
        if (!thing || thing.entry?.source !== 'npc') {
          throw new Error('select a preview NPC first (People)');
        }
        const [to] = this.grounded([pointOf(args.to, 'walk')]);
        editor.walkTo(thing, to);
        return { walking: true, to };
      }
      case 'npc_state': {
        const thing = editor.selected;
        if (!thing || thing.entry?.source !== 'npc') {
          throw new Error('select a preview NPC first (People)');
        }
        editor.pollWalks(true);
        return { walk: thing.walk || 'standing' };
      }
      case 'gates_near': {
        const at = pointOf(args.at, 'at');
        return editor.api.gatesNear?.(at, isNumber(args.radius) ? args.radius : 20) ?? [];
      }
      case 'gate': {
        const at = pointOf(args.at, 'at');
        const gates = editor.api.gatesNear?.(at, 60) ?? [];
        const gate = gates.find((entry) => entry.gate === args.gate);
        if (!gate) {
          throw new Error('no such gate within 60 m of at');
        }
        const state =
          typeof args.state === 'string' ? gate.states.indexOf(args.state) : Number(args.state);
        if (!(state >= 0 && state < gate.states.length)) {
          throw new Error(`choose one of: ${gate.states.join(', ')}`);
        }
        editor.api.send({ op: 'localGate', gate: gate.gate, state });
        return { gate: gate.gate, state: gate.states[state] };
      }
      case 'catalogue':
        return this.catalogue(args);
      case 'list': {
        const kind = args.kind ? String(args.kind) : null;
        const things = this.things().map((thing) => this.describe(thing));
        return kind ? things.filter((thing) => thing.kind === kind) : things;
      }
      case 'place':
        return this.place(args);
      case 'hide_level_named': {
        // The level's own object of a model, the nearest to a point, hidden in this game, found by
        // its model's file name, not by a ray (a building of yours over it hid the ray from it).
        const at = pointOf(args.at, 'hide_level_named');
        const file = (mesh) =>
          String(mesh ?? '')
            .replace(/\\/g, '/')
            .toLowerCase()
            .split('/')
            .pop();
        const wanted = file(args.mesh);
        const radius = isNumber(args.radius) ? args.radius : 30;
        const brushes = editor.api.levelBrushes?.(at, radius, 256, true) ?? [];
        const distance = (brush) => Math.hypot(brush.at.x - at.x, brush.at.y - at.y);
        const found = brushes
          .filter((brush) => file(brush.mesh) === wanted)
          .sort((a, b) => distance(a) - distance(b));
        if (found.length === 0) {
          const near = [...new Set(brushes.map((brush) => file(brush.mesh)))].slice(0, 20);
          throw new Error(
            `no level object ${wanted} within ${radius} m (near: ${near.join(', ')})`,
          );
        }
        const brush = found[0];
        const thing = editor.levelThing({
          levelObject: { kind: 'brush', name: brush.mesh, guid: '', class: '', at: brush.at },
          kind: 'level',
        });
        editor.setLevelHidden(thing, true);
        return this.describe(thing);
      }
      case 'show_level': {
        // A level object hidden in this game (hide_level_at, delete) shown again.
        const thing = this.thingById(args.id);
        if (!thing?.level) {
          throw new Error('show_level takes the id of a hidden level object (list kind level)');
        }
        editor.setLevelHidden(thing, false);
        return this.describe(thing);
      }
      case 'place_many':
        return this.placeMany(args);
      case 'fill_row':
        return this.fillRow(args);
      case 'select': {
        const thing = args.id !== undefined ? this.thingById(args.id) : null;
        editor.select(thing);
        return thing ? this.describe(thing) : null;
      }
      case 'move':
        return this.move(args);
      case 'delete': {
        editor.select(this.thingById(args.id));
        editor.deleteSelected();
        return true;
      }
      case 'duplicate': {
        const before = new Set(this.things());
        editor.select(this.thingById(args.id));
        editor.duplicateSelected();
        const made = this.things().find((thing) => !before.has(thing));
        if (!made) {
          throw new Error(editor.message || 'nothing was duplicated');
        }
        return this.describe(made);
      }
      case 'shape_size': {
        const shape = this.thingById(args.id);
        editor.select(shape);
        if (editor.selectionKind() !== 'shape') {
          throw new Error('only collision shapes have a size');
        }
        const size = args.size ?? {};
        editor.changeShape('size', (current) => ({
          size: {
            x: isNumber(size.x) ? size.x : current.size.x,
            y: isNumber(size.y) ? size.y : current.size.y,
            z: isNumber(size.z) ? size.z : current.size.z,
          },
        }));
        return this.describe(shape);
      }
      case 'pick': {
        // What a click with Select straight down onto (x, y) takes: your thing nearest it, else the
        // level's own object there (given an id, to move or hide it). With `from` and `to` instead,
        // what that line meets first, as a click from a camera there: a door in a wall, a lamp
        // under a roof.
        let hit = null;
        let ground = null;
        if (args.from && args.to) {
          const from = pointOf(args.from, 'pick from');
          const to = pointOf(args.to, 'pick to');
          hit = editor.api.World.raycast(
            { x: from.x, y: from.y, z: from.z ?? 0 },
            { x: to.x, y: to.y, z: to.z ?? 0 },
            { mode: 'anything' },
          );
          if (!hit) {
            throw new Error('the line meets nothing');
          }
          ground = hit.position;
        } else {
          const at = pointOf(args.at, 'pick');
          const top = isNumber(at.z) ? at.z + 30 : 2000;
          hit = editor.api.World.raycast(
            { x: at.x, y: at.y, z: top },
            { x: at.x, y: at.y, z: top - 4000 },
            { mode: 'anything' },
          );
          ground = hit ? hit.position : { x: at.x, y: at.y, z: at.z ?? 0 };
        }
        const thing = editor.thingAt(ground, hit);
        if (!thing) {
          throw new Error('nothing there to pick');
        }
        if (thing.level && !this.picked.includes(thing)) {
          this.picked.push(thing);
        }
        editor.select(thing);
        return this.describe(thing);
      }
      case 'hide_level_at': {
        // The level's own object a ray down onto (x, y) meets, hidden in this game.
        const at = pointOf(args.at, 'hide_level_at');
        const top = isNumber(at.z) ? at.z + 30 : 2000;
        const hit = editor.api.World.raycast(
          { x: at.x, y: at.y, z: top },
          { x: at.x, y: at.y, z: top - 4000 },
          { mode: 'anything' },
        );
        // A thing of yours there is not the level's (it hid your copy of a table as a level
        // object): delete it instead.
        if (editor.ownThingHit(hit)) {
          throw new Error('that is a thing of yours, not the level\'s: use "delete"');
        }
        const thing = hit?.levelObject ? editor.levelThing(hit) : null;
        if (!thing) {
          throw new Error('no level object there');
        }
        editor.select(thing);
        editor.deleteSelected();
        return this.describe(thing);
      }
      case 'terrain': {
        const mode = String(args.mode ?? '');
        if (!['raise', 'lower', 'flatten'].includes(mode)) {
          throw new Error('mode is raise, lower or flatten');
        }
        return this.stroke(mode, pointsOf(args.points, 'terrain'), args.size, args.strength);
      }
      case 'paint': {
        const material = String(args.material ?? '');
        if (!PAINT_MATERIALS.includes(material)) {
          throw new Error(`material is one of ${PAINT_MATERIALS.join(', ')}`);
        }
        // A road or a patch: dabs half a brush radius apart along the whole line, not just at
        // the points given.
        const size = isNumber(args.size) ? Math.max(1, Math.min(60, args.size)) : null;
        const points = pointsOf(args.points, 'paint');
        return this.stroke(`paint:${material}`, along(points, (size ?? 6) / 2), size ?? undefined);
      }
      case 'materials':
        return PAINT_MATERIALS;
      // Your own models and materials (the assets folder), the folder looked
      // at again: what was added since (a texture the MCP server just made) is loaded now.
      case 'assets': {
        editor.assets = null;
        // The content lists again too: a model made since is placed by its name.
        editor.catalogue.refresh();
        return editor.userAssets();
      }
      // A placed prop drawn with one of your materials (null: its own), one undo step.
      case 'set_material': {
        const thing = this.thingById(args.id);
        if (thing.level || thing.entry?.source !== 'prop') {
          throw new Error('only a prop takes another material (place the model as a prop)');
        }
        const material =
          args.material === null || args.material === undefined ? null : String(args.material);
        if (material !== null && !editor.userAssets().materials.includes(material)) {
          throw new Error(`no material ${material} of yours (assets lists them)`);
        }
        editor.changeMaterial(thing, material);
        return this.describe(thing);
      }
      // A custom map the MCP server just made on this PC: the game lists it (switch_map then
      // opens it).
      case 'map_added': {
        const map = String(args.map ?? '');
        if (!/^kcds_map_[a-z0-9_]{1,32}$/.test(map)) {
          throw new Error('map is kcds_map_<name>');
        }
        editor.api.send({ op: 'architectMapAdded', map });
        return { listed: map };
      }
      case 'clear_nature':
        return this.stroke('clear', pointsOf(args.points, 'clear_nature'), args.size);
      case 'add_nature':
        return this.stroke('nature', pointsOf(args.points, 'add_nature'), args.size);
      case 'undo':
      case 'redo': {
        const count = Math.max(1, Math.min(100, args.count ?? 1));
        for (let index = 0; index < count; index += 1) {
          if (command === 'undo') {
            editor.undo();
          } else {
            editor.redo();
          }
        }
        return { steps: editor.undoStack.length };
      }
      case 'history':
        return {
          done: editor.undoStack.map((step) => step.label),
          undone: (editor.redoStack ?? []).map((step) => step.label),
        };
      case 'ground_height':
        return this.editor.groundsOf(
          pointsOf(args.points, 'ground_height').map((point) => ({ ...point, z: point.z ?? 0 })),
          GROUND_SEARCH,
        );
      case 'raycast': {
        const from = pointOf(args.from, 'raycast from');
        const to = pointOf(args.to, 'raycast to');
        const hit = editor.api.World.raycast(
          { ...from, z: from.z ?? 0 },
          { ...to, z: to.z ?? 0 },
          { mode: String(args.mode ?? 'anything') },
        );
        return hit
          ? {
              position: placeOf(hit.position),
              level: hit.levelObject
                ? String(hit.levelObject.model ?? hit.levelObject.name ?? '')
                : null,
            }
          : null;
      }
      case 'screenshot':
        return this.screenshot(args);
      case 'saves':
        editor.refreshSaves();
        return editor.saves.map((save) => ({
          name: save.name,
          bytes: save.bytes,
          time: save.time,
        }));
      case 'save': {
        editor.saveName = String(args.name ?? '');
        editor.saveAs();
        return { saved: editor.saveName };
      }
      case 'load':
      case 'merge':
      case 'delete_save': {
        editor.refreshSaves();
        const row = editor.saves.findIndex((save) => save.name === String(args.name ?? ''));
        if (row < 0) {
          throw new Error(`no save named "${args.name}" (list them with saves)`);
        }
        editor.pressSave(row, command === 'delete_save' ? 'delete' : command);
        return { done: command, name: args.name };
      }
      default:
        throw new Error(`unknown command "${command}"`);
    }
  }

  // One request line -> one answer line (a promise of it for a command that waits).
  handle(text) {
    let request = null;
    try {
      request = JSON.parse(text);
    } catch {
      return JSON.stringify({ id: null, ok: false, error: 'a request is one JSON line' });
    }
    const id = request?.id ?? null;
    try {
      const editor = this.editor;
      if (!editor.isOpen() && !['open', 'state'].includes(request.command)) {
        throw new Error('the editor is not open (command "open")');
      }
      editor.message = '';
      const result = this.run(String(request.command ?? ''), request.args ?? {});
      const answer = (value) => {
        editor.refresh();
        return JSON.stringify({
          id,
          ok: true,
          result: value,
          message: editor.message,
          state: editor.state(),
        });
      };
      const failed = (error) =>
        JSON.stringify({ id, ok: false, error: String(error?.message ?? error) });
      // A command that waits for the game (screenshot) answers when it is done.
      return typeof result?.then === 'function' ? result.then(answer, failed) : answer(result);
    } catch (error) {
      return JSON.stringify({ id, ok: false, error: String(error?.message ?? error) });
    }
  }
}

module.exports = { AiCommands };
