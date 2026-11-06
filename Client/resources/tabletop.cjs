'use strict';

// Tabletop minigames in a player's game (the server's Tabletop in
// Backend/scripting/runtimes/js/tabletop/tabletops.cjs). The server tells this game the tables of
// its dimension and whether its player sits at one, through the built-in event 'kcds:tabletop'
// that scripts never see. This module:
//   - near a free seat, shows a "Sit down" hint on script_action (U); seated, a "Stand up" one;
//   - seated, points at the board: the cell under the crosshair (the camera's ray met with the
//     board's plane), or one moved with the arrow keys when the camera cannot aim (a table's
//     camera shot), drawn as a box; mouse 1 or Enter sends the click.
// It only sends requests ({ op: 'sit' | 'stand' | 'click' }): the server decides what happens.

const { Vector3, Quaternion } = require('../../Backend/scripting/runtimes/js/math.cjs');

const EVENT = 'kcds:tabletop';
const OWNER = 'kcds:tabletop';
const SEAT_REACH = 1.6;
const POINT_EVERY_MS = 100;
const NEAR_EVERY_MS = 250;
const RAY_RANGE = 8;
// The cell marks, drawn as flat faces this far above the board's top: fills lowest, the frames and
// dots over them, the pointed cell's frame over everything. Faces on the surface itself flickered
// in it, and a board model's top lies a few millimetres off the table's `height` (at 2 to 4 mm
// the chess marks were drawn under the board).
const FILL_LIFT = 0.007;
const MARK_LIFT = 0.008;
const HOVER_LIFT = 0.009;
// A frame's outer and inner edges and a dot's radius, as parts of half a cell.
const FRAME_OUTER = 0.94;
const FRAME_INNER = 0.8;
const CELL_FILL = 0.97;
const DOT_RADIUS = 0.26;
const DOT_SEGMENTS = 20;
// How opaque each part is, as a part of the mark's own colour's alpha.
const FRAME_ALPHA = 0.9;
const FRAME_FILL_ALPHA = 0.22;
const CELL_FILL_ALPHA = 0.45;
const DOT_ALPHA = 0.75;
// The pointed cell's frame: light blue, seen on light and dark cells alike (a white one was lost on
// the light ones) and apart from the picked piece's gold.
const HOVER_COLOR = 0xff3fb6ff;
// Cells per drawing: a drawing has at most 1024 points and 3072 indices, and each face is drawn
// both ways round (a dot's 20 faces are 120 indices).
const CELLS_PER_DRAWING = 24;
// The game's actions held off while seated by default (Libs/Config/defaultProfile.xml): fighting,
// and with the mouse pointer the mouse look.
const COMBAT_ACTIONS = [
  'attack_primary',
  'attack_primary_mouse',
  'attack_special',
  'attack_abort',
  'block',
  'prepare_to_attack',
  'toggle_holster_qam_weapon',
  'toggle_holster_qam_weapon_2',
  'weapon_slot_1',
  'weapon_slot_2',
  'weapon_slot_3',
  'weapon_slot_4',
  'weapon_slot_next',
  'weapon_slot_prev',
];
const LOOK_ACTIONS = ['rotateyaw', 'rotatepitch', 'xi_rotateyaw', 'xi_rotatepitch'];
// At a chair seat the walk keys get the player up from the game's seat (W left the chess game):
// the game's "sitting" action map's exits (defaultProfile.xml), on the walk keys
// and the arrows, are held off and the keys move the pointed cell, until the Stand up hint gives
// them back.
// Held off, the seat's exits take the game's seat action map with them (Client/runtime/input,
// sitting_map_held), and with it the game's script_action (U): the Stand up hint's key is bound as
// a key of its own too.
const WALK_ACTIONS = [
  'sitting_exit_up',
  'sitting_exit_down',
  'sitting_exit_left',
  'sitting_exit_right',
  'moveforward',
  'moveback',
  'moveleft',
  'moveright',
];
const STAND_KEY = 'u';
const STEP_KEYS = [
  ['up', 1, 0],
  ['down', -1, 0],
  ['left', 0, -1],
  ['right', 0, 1],
];
const WALK_KEYS = [
  ['w', 1, 0],
  ['s', -1, 0],
  ['a', 0, -1],
  ['d', 0, 1],
];
// The keys at a table with a deck and what they ask the server (Backend/.../tabletop/decks.cjs);
// W A S D only at a chair seat, where the game's walk is held off.
const DECK_KEYS = [
  ['left', 'prev'],
  ['a', 'prev'],
  ['right', 'next'],
  ['d', 'next'],
  ['up', 'select'],
  ['w', 'select'],
  ['mouse1', 'select'],
  ['enter', 'play'],
  ['down', 'cover'],
  ['s', 'cover'],
  ['c', 'look'],
];
// The deck's keys shown as the game's key hints: pass on Space (jump), draw on E (the game's use
// key), and the close looks on holding E (as C).
const DECK_HINT_KEYS = [
  ['pass', 'jump', 'press'],
  ['draw', 'primary_action', 'press'],
  ['look', 'primary_action', 'hold'],
];
const LOOK_HOLD_SECONDS = 0.4;

class ResourceTabletop {
  constructor({ send, camera, gizmos, input, actionHints, entities }) {
    this.send = send;
    this.camera = camera;
    this.gizmos = gizmos;
    this.keys = input.keyFacade(OWNER);
    this.controls = input.controlsFacade(OWNER);
    // What sitting down holds while seated: the pointer shown, the game actions blocked.
    this.held = { cursor: false, actions: [] };
    this.hints = actionHints.facade(OWNER);
    // A deck's draw and pass key hints shown (action -> hint id).
    this.deckHints = new Map();
    this.entities = entities;
    this.tables = new Map();
    this.seated = null;
    this.hover = null;
    // table -> its marks this player sees, and the drawing IDs they are drawn with
    this.marks = new Map();
    this.markDrawings = new Map();
    this.keyCell = null;
    this.sitHint = null;
    this.sitTarget = null;
    this.standHint = null;
    this.nearTimer = setInterval(() => this.checkSeats(), NEAR_EVERY_MS);
    // It never holds a process open (Node tests); the worker lives as long as the game.
    this.nearTimer.unref?.();
    this.pointTimer = null;
    this.drawFailed = false;
  }

  // A line in the game's log (RESOURCE client ...): seating, pointing and drawing, for live tests.
  log(text) {
    this.send({ op: 'log', resource: 'tabletop', text: `TABLETOP ${text}` });
  }

  // A 'kcds:tabletop' message from the server.
  receive(message) {
    if (!message || typeof message !== 'object') {
      return;
    }
    if (message.op === 'tables' && Array.isArray(message.tables)) {
      this.tables = new Map(message.tables.map((table) => [table.id, table]));
      if (this.seated && !this.tables.has(this.seated.table)) {
        this.standUp();
      }
      for (const id of [...this.marks.keys()]) {
        if (!this.tables.has(id)) {
          this.marks.delete(id);
        }
        this.drawMarks(id);
      }
    } else if (message.op === 'marks' && Array.isArray(message.marks)) {
      this.marks.set(message.table, message.marks);
      this.drawMarks(message.table);
    } else if (message.op === 'seated') {
      try {
        this.sitDown(message.table, message.seat);
      } catch (error) {
        this.log(`sitting down failed: ${error.message}`);
      }
    } else if (message.op === 'stood') {
      this.standUp();
    } else if (message.op === 'hand') {
      // The pieces this player holds of a table's deck (the server draws them) and their keys.
      this.log(`hand table=${message.table} pieces=${message.count} covered=${message.covered}`);
      this.showDeckHints(message);
    }
  }

  // The deck's draw and pass keys as the game's own key hints, while seated at its table: their
  // labels from the server (false: no such key).
  showDeckHints(message) {
    if (!this.seated || this.seated.table !== message.table) {
      return;
    }
    for (const [action, key, mode] of DECK_HINT_KEYS) {
      const label = message.actions?.[action];
      const shown = this.deckHints.get(action);
      if (typeof label !== 'string') {
        if (shown !== undefined) {
          this.hints.remove(shown);
          this.deckHints.delete(action);
        }
      } else if (shown !== undefined) {
        this.hints.setText(shown, label);
      } else {
        const send = () =>
          this.request({ op: 'deck', table: message.table, deck: message.deck, action });
        const options = { key, text: label, mode };
        if (mode === 'hold') {
          options.holdDuration = LOOK_HOLD_SECONDS;
        }
        this.deckHints.set(action, this.hints.create(options, send));
      }
    }
  }

  removeDeckHints() {
    for (const hint of this.deckHints.values()) {
      this.hints.remove(hint);
    }
    this.deckHints.clear();
  }

  request(payload) {
    this.send({ op: 'event', name: EVENT, payload: JSON.stringify(payload) });
  }

  localPosition() {
    const record = this.entities.records.get(this.entities.localId);
    return record?.position ?? null;
  }

  // Not seated: a hint at the nearest free seat in reach.
  checkSeats() {
    if (this.seated) {
      return;
    }
    const at = this.localPosition();
    let best = null;
    if (at) {
      for (const table of this.tables.values()) {
        table.seats.forEach((seat, index) => {
          const distance = Math.hypot(
            seat.position.x - at.x,
            seat.position.y - at.y,
            seat.position.z - at.z,
          );
          // A chair seat is sat on with the game's own seat there, not with this hint.
          const free = !seat.taken && !seat.chair;
          if (free && distance <= SEAT_REACH && (!best || distance < best.distance)) {
            best = { table: table.id, seat: index, distance };
          }
        });
      }
    }
    const same =
      best &&
      this.sitTarget &&
      best.table === this.sitTarget.table &&
      best.seat === this.sitTarget.seat;
    if (same) {
      return;
    }
    this.removeSitHint();
    if (best) {
      this.sitTarget = best;
      this.sitHint = this.hints.create({ key: 'script_action', text: 'Sit down' }, () => {
        this.request({ op: 'sit', table: best.table, seat: best.seat });
      });
    }
  }

  removeSitHint() {
    if (this.sitHint !== null) {
      this.hints.remove(this.sitHint);
    }
    this.sitHint = null;
    this.sitTarget = null;
  }

  sitDown(tableId, seat) {
    this.removeSitHint();
    this.standUp();
    this.seated = { table: tableId, seat };
    this.keyCell = null;
    // At a chair seat the player gets up with the game's own seat: the hint first gives the walk
    // keys back. Elsewhere the hint stands them up.
    const chair = this.tables.get(tableId)?.seats?.[seat]?.chair === true;
    this.standHint = this.hints.create({ key: 'script_action', text: 'Stand up' }, () => {
      if (chair) {
        this.freeWalk();
      } else {
        this.request({ op: 'stand', table: tableId });
      }
    });
    if (chair) {
      this.keys.bind(STAND_KEY, 'down', () => this.freeWalk());
    }
    // A table with a deck: the keys work the hand in the player's hand, no board to point at.
    if (this.tables.get(tableId)?.deck === true) {
      this.bindDeckKeys(tableId, chair);
      for (const action of [...COMBAT_ACTIONS, ...(chair ? WALK_ACTIONS : [])]) {
        this.controls.blockAction(action);
        this.held.actions.push(action);
      }
      this.log(`seated at deck table=${tableId} seat=${seat} chair=${chair}`);
      return;
    }
    this.keys.bind('mouse1', 'down', () => this.click());
    this.keys.bind('enter', 'down', () => this.click());
    // The arrows as the player sees the board: up is away from them, right to their right; at a
    // chair W, A, S and D too.
    for (const [key, ahead, right] of [...STEP_KEYS, ...(chair ? WALK_KEYS : [])]) {
      this.keys.bind(key, 'down', () => this.step(ahead, right));
    }
    const pointer = this.pointer();
    if (pointer === 'cursor') {
      this.controls.showCursor();
      this.held.cursor = true;
    }
    for (const action of [...this.blockedActions(pointer), ...(chair ? WALK_ACTIONS : [])]) {
      this.controls.blockAction(action);
      this.held.actions.push(action);
    }
    this.log(
      `seated table=${tableId} seat=${seat} chair=${chair} pointer=${pointer} ` +
        `blocked=${this.held.actions.length}`,
    );
    // The keys point from the board's middle, so the mark shows before the first key.
    const table = this.tables.get(tableId);
    if (pointer === 'keys' && table) {
      this.keyCell = { row: Math.floor(table.rows / 2), column: Math.floor(table.columns / 2) };
      this.setHover(this.keyCell);
    }
    this.pointTimer = setInterval(() => this.point(), POINT_EVERY_MS);
    this.pointTimer.unref?.();
  }

  // The keys at a table with a deck (the server's TabletopDeck does the rest): left and right
  // (A, D) point at a piece of the hand, up (W) or mouse 1 selects it, Enter plays the selected
  // pieces (else the pointed one), down (S) lays the hand face down on the table or takes it up,
  // C looks at the table close up.
  bindDeckKeys(tableId, chair) {
    const send = (action) => this.request({ op: 'deck', table: tableId, action });
    for (const [key, action] of DECK_KEYS) {
      if (chair || !WALK_KEYS.some(([walk]) => walk === key)) {
        this.keys.bind(key, 'down', () => send(action));
      }
    }
  }

  // How this seat points: the table's controls.pointer, else the pointer with a camera shot of the
  // seat's or the table's (the view does not follow the mouse), else the crosshair.
  pointer() {
    const table = this.table();
    if (!table) {
      return 'crosshair';
    }
    if (table.controls?.pointer) {
      return table.controls.pointer;
    }
    const seat = table.seats?.[this.seated.seat];
    return seat?.fixedView || table.fixedView ? 'cursor' : 'crosshair';
  }

  // The game actions held off while seated: the table's controls.block, else the combat ones (a
  // click picks, it does not swing) and, when the pointer is the cursor or the seat's camera shot
  // looks about by itself (lookView), the game's mouse look too.
  blockedActions(pointer) {
    const table = this.table();
    if (Array.isArray(table?.controls?.block)) {
      return table.controls.block;
    }
    const lookView = table?.seats?.[this.seated.seat]?.lookView || table?.lookView;
    return pointer === 'cursor' || lookView ? [...COMBAT_ACTIONS, ...LOOK_ACTIONS] : COMBAT_ACTIONS;
  }

  // At a chair seat: the walk keys are the game's again, so W gets the player up from its seat.
  freeWalk() {
    for (const [key] of WALK_KEYS) {
      this.keys.unbind(key);
    }
    this.keys.unbind(STAND_KEY);
    for (const action of WALK_ACTIONS) {
      if (this.held.actions.includes(action)) {
        this.controls.unblockAction(action);
      }
    }
    this.held.actions = this.held.actions.filter((action) => !WALK_ACTIONS.includes(action));
    if (this.standHint !== null) {
      this.hints.remove(this.standHint);
    }
    this.standHint = null;
  }

  standUp() {
    if (!this.seated) {
      return;
    }
    clearInterval(this.pointTimer);
    this.pointTimer = null;
    for (const [key] of [...STEP_KEYS, ...WALK_KEYS]) {
      this.keys.unbind(key);
    }
    this.keys.unbind('mouse1');
    this.keys.unbind('enter');
    this.keys.unbind('c');
    this.keys.unbind(STAND_KEY);
    if (this.held.cursor) {
      this.controls.hideCursor();
    }
    for (const action of this.held.actions) {
      this.controls.unblockAction(action);
    }
    this.held = { cursor: false, actions: [] };
    if (this.standHint !== null) {
      this.hints.remove(this.standHint);
    }
    this.standHint = null;
    this.removeDeckHints();
    this.log(`stood table=${this.seated.table}`);
    this.seated = null;
    this.setHover(null);
  }

  table() {
    return this.seated ? this.tables.get(this.seated.table) ?? null : null;
  }

  turnOf(table) {
    const { w, x, y, z } = table.rotation;
    return new Quaternion(w, x, y, z);
  }

  // A point in the world on the board's own axes: x along its columns, y along its rows, z up.
  toBoard(table, point) {
    const back = this.turnOf(table).conjugate();
    return back.rotateVector(
      new Vector3(
        point.x - table.position.x,
        point.y - table.position.y,
        point.z - table.position.z,
      ),
    );
  }

  // The world position of a cell's middle on the board.
  toWorld(table, cell) {
    const local = new Vector3(
      (cell.column - (table.columns - 1) / 2) * table.cellSize,
      (cell.row - (table.rows - 1) / 2) * table.cellSize,
      table.height,
    );
    const turned = this.turnOf(table).rotateVector(local);
    return {
      x: table.position.x + turned.x,
      y: table.position.y + turned.y,
      z: table.position.z + turned.z,
    };
  }

  // The cell at a point of the screen (the crosshair's 0, 0 by default): the camera's ray through
  // it met with the board's plane.
  aimedCell(table, screenX = 0, screenY = 0) {
    const ray = this.camera.rayFromScreen({ x: screenX, y: screenY, range: RAY_RANGE });
    if (!ray) {
      return null;
    }
    const from = this.toBoard(table, ray.origin);
    const to = this.toBoard(table, ray.target);
    const drop = to.z - from.z;
    if (Math.abs(drop) < 1e-6) {
      return null;
    }
    const t = (table.height - from.z) / drop;
    if (t < 0 || t > 1) {
      return null;
    }
    const x = from.x + (to.x - from.x) * t;
    const y = from.y + (to.y - from.y) * t;
    return this.cellOf(table, x, y);
  }

  cellOf(table, x, y) {
    const column = Math.round(x / table.cellSize + (table.columns - 1) / 2);
    const row = Math.round(y / table.cellSize + (table.rows - 1) / 2);
    if (row < 0 || row >= table.rows || column < 0 || column >= table.columns) {
      return null;
    }
    return { row, column };
  }

  point() {
    const table = this.table();
    if (!table) {
      return;
    }
    const pointer = this.pointer();
    if (pointer === 'cursor') {
      // The cell under the mouse pointer; the arrows' cell while it is off the board.
      const at = this.controls.cursorPosition();
      const aimed = at ? this.aimedCell(table, at.x, at.y) : null;
      if (aimed) {
        this.keyCell = null;
      }
      this.setHover(aimed ?? this.keyCell);
      return;
    }
    // With a view that does not follow the mouse the crosshair cannot aim: the arrows point.
    const fixed =
      table.fixedView || table.seats?.[this.seated.seat]?.fixedView || this.camera.active;
    const aimed = pointer === 'crosshair' && !fixed ? this.aimedCell(table) : null;
    this.setHover(aimed ?? this.keyCell);
  }

  // The board's row and column steps for "ahead" and "to the right" as this seat sees the board:
  // from the seat's place on the board's own axes, the side of the board it sits at.
  seatSteps(table) {
    const seat = table.seats?.[this.seated.seat];
    const local = seat?.position ? this.toBoard(table, seat.position) : { x: 0, y: -1 };
    if (Math.abs(local.y) >= Math.abs(local.x)) {
      // At the board's -y side the player looks along +y (rows up), its right is +x (columns up).
      const sign = local.y <= 0 ? 1 : -1;
      return { ahead: { row: sign, column: 0 }, right: { row: 0, column: sign } };
    }
    // At the -x side the player looks along +x (columns up), its right is -y (rows down).
    const sign = local.x <= 0 ? 1 : -1;
    return { ahead: { row: 0, column: sign }, right: { row: -sign, column: 0 } };
  }

  // The arrow keys move the pointed cell, as the player sees the board.
  step(ahead, right) {
    const table = this.table();
    if (!table) {
      return;
    }
    const steps = this.seatSteps(table);
    const rows = steps.ahead.row * ahead + steps.right.row * right;
    const columns = steps.ahead.column * ahead + steps.right.column * right;
    const from = this.hover ?? {
      row: Math.floor(table.rows / 2),
      column: Math.floor(table.columns / 2),
    };
    this.keyCell = {
      row: Math.min(table.rows - 1, Math.max(0, from.row + rows)),
      column: Math.min(table.columns - 1, Math.max(0, from.column + columns)),
    };
    this.setHover(this.keyCell);
  }

  setHover(cell) {
    const same =
      (cell === null && this.hover === null) ||
      (cell && this.hover && cell.row === this.hover.row && cell.column === this.hover.column);
    if (same) {
      return;
    }
    // Logged only when pointing starts or stops, not for every cell.
    if ((this.hover === null) !== (cell === null)) {
      this.log(cell ? `points at ${cell.row},${cell.column}` : 'points at nothing');
    }
    this.hover = cell;
    this.gizmos.remove(OWNER, 'hover');
    this.gizmos.remove(OWNER, 'hover:fill');
    if (!cell) {
      return;
    }
    const table = this.table();
    const cells = [[cell.row, cell.column]];
    this.drawFaces('hover', this.frameFaces(table, cells, HOVER_LIFT), HOVER_COLOR, FRAME_ALPHA);
    this.drawFaces(
      'hover:fill',
      this.squareFaces(table, cells, FRAME_INNER, HOVER_LIFT),
      HOVER_COLOR,
      FRAME_FILL_ALPHA,
    );
  }

  // A point on the board's top, `x` along its columns and `y` along its rows from its middle.
  boardPoint(table, x, y, lift) {
    const turned = this.turnOf(table).rotateVector(new Vector3(x, y, table.height + lift));
    return {
      x: table.position.x + turned.x,
      y: table.position.y + turned.y,
      z: table.position.z + turned.z,
    };
  }

  cellMiddle(table, [row, column]) {
    return {
      x: (column - (table.columns - 1) / 2) * table.cellSize,
      y: (row - (table.rows - 1) / 2) * table.cellSize,
    };
  }

  // Each cell's square, `part` of half a cell from its middle: points and triangle indices.
  squareFaces(table, cells, part, lift) {
    const points = [];
    const indices = [];
    const half = (table.cellSize / 2) * part;
    for (const cell of cells) {
      const middle = this.cellMiddle(table, cell);
      const first = points.length;
      for (const [x, y] of [
        [-half, -half],
        [half, -half],
        [half, half],
        [-half, half],
      ]) {
        points.push(this.boardPoint(table, middle.x + x, middle.y + y, lift));
      }
      indices.push(first, first + 1, first + 2, first, first + 2, first + 3);
    }
    return { points, indices };
  }

  // Each cell's frame: the band between an outer and an inner square.
  frameFaces(table, cells, lift) {
    const points = [];
    const indices = [];
    const outer = (table.cellSize / 2) * FRAME_OUTER;
    const inner = (table.cellSize / 2) * FRAME_INNER;
    const corners = [
      [-1, -1],
      [1, -1],
      [1, 1],
      [-1, 1],
    ];
    for (const cell of cells) {
      const middle = this.cellMiddle(table, cell);
      const first = points.length;
      for (const size of [outer, inner]) {
        for (const [x, y] of corners) {
          points.push(this.boardPoint(table, middle.x + x * size, middle.y + y * size, lift));
        }
      }
      for (let side = 0; side < 4; side += 1) {
        const next = (side + 1) % 4;
        const [outerA, outerB] = [first + side, first + next];
        const [innerA, innerB] = [first + 4 + side, first + 4 + next];
        indices.push(outerA, outerB, innerB, outerA, innerB, innerA);
      }
    }
    return { points, indices };
  }

  // Each cell's round dot in its middle.
  dotFaces(table, cells, lift) {
    const points = [];
    const indices = [];
    const radius = (table.cellSize / 2) * DOT_RADIUS;
    for (const cell of cells) {
      const middle = this.cellMiddle(table, cell);
      const center = points.length;
      points.push(this.boardPoint(table, middle.x, middle.y, lift));
      for (let i = 0; i < DOT_SEGMENTS; i += 1) {
        const angle = (2 * Math.PI * i) / DOT_SEGMENTS;
        const x = middle.x + Math.cos(angle) * radius;
        const y = middle.y + Math.sin(angle) * radius;
        points.push(this.boardPoint(table, x, y, lift));
      }
      for (let i = 0; i < DOT_SEGMENTS; i += 1) {
        const next = ((i + 1) % DOT_SEGMENTS) + 1;
        indices.push(center, center + i + 1, center + next);
      }
    }
    return { points, indices };
  }

  // Draws faces in a colour (0xAARRGGBB) at `alpha` of its own alpha; false when they could not be
  // drawn (the drawing limits).
  drawFaces(id, faces, color, alpha) {
    if (faces.indices.length === 0) {
      return false;
    }
    const opacity = Math.round(((color >>> 24) & 0xff) * alpha);
    const fill = ((opacity << 24) | (color & 0xffffff)) >>> 0;
    // Each face both ways round: the game culls a flat face seen from its back whatever the
    // drawings' render flags say (the marks seen from above were never drawn,
    // while a filled box was).
    const indices = [...faces.indices];
    for (let i = 0; i < faces.indices.length; i += 3) {
      indices.push(faces.indices[i], faces.indices[i + 2], faces.indices[i + 1]);
    }
    try {
      this.gizmos.set(
        OWNER,
        id,
        { type: 'mesh', points: faces.points, indices, wireframe: false },
        { color: fill, fillColor: fill },
      );
      return true;
    } catch (error) {
      if (!this.drawFailed) {
        this.drawFailed = true;
        this.log(`cannot draw ${id}: ${error.message}`);
      }
      return false;
    }
  }

  // Draws a table's marks again, as the server last sent them.
  drawMarks(tableId) {
    for (const id of this.markDrawings.get(tableId) ?? []) {
      this.gizmos.remove(OWNER, id);
    }
    this.markDrawings.delete(tableId);
    const table = this.tables.get(tableId);
    const marks = this.marks.get(tableId) ?? [];
    if (!table || marks.length === 0) {
      return;
    }
    const drawn = [];
    const draw = (id, faces, color, alpha) => {
      if (this.drawFaces(id, faces, color, alpha)) {
        drawn.push(id);
      }
    };
    marks.forEach((mark, index) => {
      for (let from = 0; from < mark.cells.length; from += CELLS_PER_DRAWING) {
        const cells = mark.cells.slice(from, from + CELLS_PER_DRAWING);
        const id = `mark:${tableId}:${index}:${from}`;
        if (mark.style === 'fill') {
          draw(
            id,
            this.squareFaces(table, cells, CELL_FILL, FILL_LIFT),
            mark.color,
            CELL_FILL_ALPHA,
          );
        } else if (mark.style === 'dot') {
          draw(id, this.dotFaces(table, cells, MARK_LIFT), mark.color, DOT_ALPHA);
        } else {
          draw(id, this.frameFaces(table, cells, MARK_LIFT), mark.color, FRAME_ALPHA);
          draw(
            `${id}:fill`,
            this.squareFaces(table, cells, FRAME_INNER, FILL_LIFT),
            mark.color,
            FRAME_FILL_ALPHA,
          );
        }
      }
    });
    this.markDrawings.set(tableId, drawn);
  }

  click() {
    if (this.seated && this.hover) {
      this.request({
        op: 'click',
        table: this.seated.table,
        cell: [this.hover.row, this.hover.column],
      });
    }
  }

  disconnect() {
    this.standUp();
    this.removeSitHint();
    for (const id of [...this.marks.keys()]) {
      this.marks.delete(id);
      this.drawMarks(id);
    }
    this.tables.clear();
  }
}

module.exports = { ResourceTabletop, TABLETOP_EVENT: EVENT };
