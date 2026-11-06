'use strict';

// Tabletop minigames (the server API's Tabletop). A server owner writes only a game's
// rules; the table does the rest: a board of rows and columns at a place in the world, pieces that
// stand on cells and move between them smoothly, seats with a turn order and an optional turn timer,
// pointing (a seated player picks a piece and a cell in their own game, which only sends the
// request: the server decides), private pieces only one player sees, bets from the inventory, and
// save/load of the board.
//
// Pieces are props (Prop), so every player of the dimension sees them and a player who
// joins later sees the board as it is. Highlights are cell marks, drawn flat on the board by the
// games of the players they are for. The players' games know the tables through the built-in event
// 'kcds:tabletop' (Client/resources/tabletop.cjs): the sit and stand hints near a seat, the hover
// mark, the marks and the picked cell. Client -> server, as { op, table, ... } on the same event:
//   { op: 'sit', table, seat }   { op: 'stand', table }   { op: 'click', table, cell: [row, column] }
// Server -> client: { op: 'tables', tables: [...] } (every table of the player's dimension, with
// its seats' sitters), { op: 'seated', table, seat }, { op: 'stood', table } and
// { op: 'marks', table, marks: [{ cells, color, style }] } (the table's marks this player sees).
//
// Events (all on the server):
//   tabletopSit(table, player, seat)            refusable, before they sit
//   tabletopSeated(table, player, seat)         after they sat
//   tabletopLeave(table, player, seat, reason)  'stood', 'disconnect', 'removed'
//   tabletopCellClick(table, player, cell)      refusable: false takes the click (placing games)
//   tabletopPieceSelect(table, player, piece)   refusable
//   tabletopMoveRequest(table, player, piece, cell)  refusable; otherwise the piece moves there
//   tabletopStart(table)   tabletopTurn(table, seat, player)
//   tabletopTurnTimeout(table, seat, player)    refusable: refused keeps the turn, else it passes
//   tabletopForfeit(table, player, seat, reason)
//   tabletopEnd(table, winner, reason)          winner: a Player or null

const { OWNER, removeOwned, runningResource } = require('../lifecycle/owned_objects.cjs');
const { Vector3, Quaternion } = require('../math.cjs');
const { TabletopDecks } = require('./decks.cjs');

const EVENT = 'kcds:tabletop';
const GLOBAL_WORLD = 4294967295;
const MAX_TABLES = 64;
const MAX_CELLS = 32;
const MAX_SEATS = 8;
const MAX_PIECES = 256;
// Cell marks (highlight): how they look, the players' games drawing them flat on the board
// (Client/resources/tabletop.cjs): 'frame' a coloured border with a faint fill, 'fill' the whole
// cell tinted, 'dot' a round dot in the middle (a cell a piece may go to).
const MARK_STYLES = ['frame', 'fill', 'dot'];
const MARK_COLOR = 0xffffd27f;
const MAX_MARKS = 64;
// Chair seats: how often the table looks for players sat on the game's seat there, and how far
// (on the ground, metres) from the seat's position their body may be.
const CHAIR_CHECK_MS = 250;
const CHAIR_REACH = 0.9;

function flatDistance(a, b) {
  return Math.hypot(a.x - b.x, a.y - b.y);
}
const MAX_MOVE_MS = 5000;

// How a seated player points and clicks (Tabletop.create's controls):
//   pointer 'cursor'     the mouse pointer shows and the cell under it is pointed at, as the game's
//                        dice players pick their dice (the default with a camera shot);
//           'crosshair'  the cell under the crosshair, the view following the mouse (the default
//                        without one);
//           'keys'       the arrow keys move the pointed cell, up being away from the player;
//   block   the game actions held off while seated; by default the combat ones, so a click picks
//           and does not swing, and with the 'cursor' pointer the mouse look too.
const POINTERS = ['cursor', 'crosshair', 'keys'];
const ACTION_NAME = /^[A-Za-z0-9_]{1,64}$/;

function controlsOf(value) {
  if (value === undefined || value === null) {
    return { pointer: null, block: null };
  }
  if (typeof value !== 'object' || Array.isArray(value)) {
    throw new TypeError('Tabletop: controls is { pointer, block }');
  }
  const pointer = value.pointer ?? null;
  if (pointer !== null && !POINTERS.includes(pointer)) {
    throw new TypeError(`Tabletop: controls.pointer is ${POINTERS.join(', ')}`);
  }
  const block = value.block ?? null;
  if (
    block !== null &&
    (!Array.isArray(block) || block.length > 64 || !block.every((a) => ACTION_NAME.test(a)))
  ) {
    throw new TypeError("Tabletop: controls.block is a list of the game's action names");
  }
  return { pointer, block: block === null ? null : [...block] };
}

function finite(value, name) {
  const number = Number(value);
  if (!Number.isFinite(number)) {
    throw new TypeError(`Tabletop: ${name} must be a number`);
  }
  return number;
}

function pointOf(value, name) {
  if (!value || typeof value !== 'object') {
    throw new TypeError(`Tabletop: ${name} must be a position { x, y, z }`);
  }
  if (Array.isArray(value)) {
    return { x: finite(value[0], name), y: finite(value[1], name), z: finite(value[2], name) };
  }
  return { x: finite(value.x, name), y: finite(value.y, name), z: finite(value.z, name) };
}

// A table's or a seat's camera: a camera shot (Camera.startShot's options), 'eyes', or none.
function cameraOf(value, name) {
  if (value === undefined || value === null) {
    return null;
  }
  if (value === 'eyes' || (typeof value === 'object' && !Array.isArray(value))) {
    return value;
  }
  throw new TypeError(`Tabletop: ${name} is a camera shot { position, lookAt, ... } or 'eyes'`);
}

// A turn about the vertical axis: degrees, or a Quaternion { w, x, y, z }.
function yawOf(value) {
  if (value === undefined || value === null) {
    return new Quaternion(1, 0, 0, 0);
  }
  if (typeof value === 'number') {
    return Quaternion.fromAxisAngle(
      new Vector3(0, 0, 1),
      (finite(value, 'rotation') * Math.PI) / 180,
    );
  }
  if (typeof value === 'object' && 'w' in value) {
    return new Quaternion(value.w, value.x, value.y, value.z).normalize();
  }
  if (typeof value === 'object' && 'z' in value) {
    return yawOf(finite(value.z, 'rotation'));
  }
  throw new TypeError('Tabletop: rotation must be degrees or a Quaternion');
}

function wholeIn(value, low, high, name) {
  const number = finite(value, name);
  if (!Number.isInteger(number) || number < low || number > high) {
    throw new RangeError(`Tabletop: ${name} must be a whole number from ${low} to ${high}`);
  }
  return number;
}

function amountIn(value, fallback, low, high, name) {
  if (value === undefined) {
    return fallback;
  }
  const number = finite(value, name);
  if (number < low || number > high) {
    throw new RangeError(`Tabletop: ${name} must be from ${low} to ${high}`);
  }
  return number;
}

class Tabletops {
  constructor(host) {
    this.host = host;
    this.tables = new Map();
    this.nextId = 1;
    this.chairTimer = null;
    // Decks of pieces players hold at the tables: cards, tiles (TabletopDeck).
    this.decks = new TabletopDecks(this);
  }

  // Chair seats (a seat's `chair: true`): the player sits on the game's own seat there, a chair or
  // bench placed with SmartObject.spawn or the level's, with the game's E and its walk up and
  // sit-down. Their game reports them seated (Player.seated); within CHAIR_REACH of a free chair
  // seat they take it, and getting up from the game's seat stands them up from the table.
  watchChairs() {
    const any = [...this.tables.values()].some((table) => table.seats.some((seat) => seat.chair));
    if (!any) {
      clearInterval(this.chairTimer);
      this.chairTimer = null;
      return;
    }
    if (this.chairTimer === null) {
      this.chairTimer = setInterval(() => this.checkChairs(), CHAIR_CHECK_MS);
      this.chairTimer.unref?.();
    }
  }

  checkChairs() {
    for (const table of [...this.tables.values()]) {
      table.seats.forEach((seat, index) => {
        if (!seat.chair) {
          return;
        }
        if (seat.playerId !== null) {
          const sitter = this.player(seat.playerId);
          if (sitter && !sitter.seated) {
            this.leave(table, sitter, 'stood');
          }
          return;
        }
        for (const player of this.host.players.values()) {
          const ready =
            player.ready &&
            player.seated &&
            player.position &&
            player.dimension === table.dimension &&
            !this.seatedAnywhere(player);
          if (ready && flatDistance(player.position, seat.at) <= seat.reach) {
            this.sit(table, player, index, true);
            return;
          }
        }
      });
    }
  }

  seatedAnywhere(player) {
    for (const table of this.tables.values()) {
      if (this.seatIndex(table, player) >= 0) {
        return true;
      }
    }
    return false;
  }

  // The Tabletop class of the server API; every resource gets the same one.
  define() {
    this.Tabletop ??= this.makeClass();
    return this.Tabletop;
  }

  get api() {
    return this.host.api;
  }

  isPlayer(value) {
    return value instanceof this.api.Player;
  }

  // A cell as { row, column }: from { row, column }, [row, column] or a piece's cell.
  cellOf(table, value) {
    let row;
    let column;
    if (Array.isArray(value)) {
      [row, column] = value;
    } else if (value && typeof value === 'object') {
      ({ row, column } = value);
    } else {
      throw new TypeError('Tabletop: a cell is { row, column } or [row, column]');
    }
    return {
      row: wholeIn(row, 0, table.rows - 1, 'row'),
      column: wholeIn(column, 0, table.columns - 1, 'column'),
    };
  }

  // Where a cell's middle is in the world, `level` pieces up its stack.
  worldOf(table, cell, level = 0) {
    const local = new Vector3(
      (cell.column - (table.columns - 1) / 2) * table.cellSize,
      (cell.row - (table.rows - 1) / 2) * table.cellSize,
      table.height + level * table.stackHeight,
    );
    const turned = table.yaw.rotateVector(local);
    return new Vector3(table.at.x + turned.x, table.at.y + turned.y, table.at.z + turned.z);
  }

  makeClass() {
    const registry = this;
    const Piece = this.makePieceClass();
    this.Piece = Piece;
    class Tabletop {
      // A table: { position, rotation, dimension, board: { model, rows, columns, cellSize,
      // height, stackHeight }, seats: [{ position, rotation, side, animation, camera }],
      // turnSeconds, camera, controls: { pointer, block } }.
      static create(options = {}) {
        return registry.create(Tabletop, options);
      }
      // Every table, or those of one dimension.
      static all(dimension) {
        return [...registry.tables.values()]
          .filter((table) => dimension === undefined || table.dimension === dimension)
          .map((table) => table.handle);
      }
      static getById(id) {
        return registry.tables.get(id)?.handle ?? null;
      }
      get id() {
        return registry.stateOf(this).id;
      }
      get exists() {
        return registry.tables.get(this.id)?.handle === this;
      }
      get position() {
        const { at } = registry.stateOf(this);
        return new Vector3(at.x, at.y, at.z);
      }
      get rotation() {
        const { yaw } = registry.stateOf(this);
        return new Quaternion(yaw.w, yaw.x, yaw.y, yaw.z);
      }
      get dimension() {
        return registry.stateOf(this).dimension;
      }
      get rows() {
        return registry.stateOf(this).rows;
      }
      get columns() {
        return registry.stateOf(this).columns;
      }
      get cellSize() {
        return registry.stateOf(this).cellSize;
      }
      // The pieces on the board, in the order they were added.
      get pieces() {
        return [...registry.stateOf(this).pieces.values()].map((piece) => piece.handle);
      }
      // The seats: [{ index, side, player }] (player null for a free seat).
      get seats() {
        return registry.stateOf(this).seats.map((seat, index) => ({
          index,
          side: seat.side,
          player: seat.playerId === null ? null : registry.player(seat.playerId),
        }));
      }
      // 'idle' or 'playing'.
      get state() {
        return registry.stateOf(this).playing ? 'playing' : 'idle';
      }
      // The seat whose turn it is, -1 when no game is on.
      get turn() {
        const table = registry.stateOf(this);
        return table.playing ? table.turn : -1;
      }
      // What bets put on the table, in units of their item.
      get pot() {
        return registry.stateOf(this).pot.amount;
      }
      cellPosition(cell) {
        const table = registry.stateOf(this);
        return registry.worldOf(table, registry.cellOf(table, cell));
      }
      // The cell under a position, or null off the board.
      cellAt(position) {
        return registry.cellUnder(registry.stateOf(this), pointOf(position, 'position'));
      }
      // A piece: { model, cell, side, data, scale, rotation, visibleTo }.
      addPiece(options = {}) {
        return registry.addPiece(Piece, registry.stateOf(this), options);
      }
      // The pieces on a cell, bottom first.
      piecesAt(cell) {
        const table = registry.stateOf(this);
        return registry.stack(table, registry.cellOf(table, cell)).map((piece) => piece.handle);
      }
      // The top piece of a cell, or null.
      pieceAt(cell) {
        return this.piecesAt(cell).at(-1) ?? null;
      }
      // Marks cells, for some players or for everyone: { players, color, style }.
      highlight(cells, options = {}) {
        registry.highlight(registry.stateOf(this), cells, options);
      }
      clearHighlight(players) {
        registry.clearHighlight(registry.stateOf(this), players);
      }
      sit(player, seat) {
        return registry.sit(registry.stateOf(this), player, seat, false);
      }
      stand(player) {
        return registry.leave(registry.stateOf(this), player, 'stood');
      }
      // The seat a player sits on, -1 for none.
      seatOf(player) {
        return registry.seatIndex(registry.stateOf(this), player);
      }
      playerAt(seat) {
        const table = registry.stateOf(this);
        const index = wholeIn(seat, 0, table.seats.length - 1, 'seat');
        const id = table.seats[index].playerId;
        return id === null ? null : registry.player(id);
      }
      // Starts a game: the turn goes to `turn` (default the first seat).
      start(options = {}) {
        registry.start(registry.stateOf(this), options);
      }
      // Passes the turn to the next occupied seat; the seat it went to.
      nextTurn() {
        return registry.nextTurn(registry.stateOf(this));
      }
      // Ends the game with a winner (a seat or a Player) or none (null), and pays the pot.
      finish(winner = null, reason = 'won') {
        registry.finish(registry.stateOf(this), winner, reason);
      }
      // Takes `amount` of an item from every seated player for the pot; all or nothing.
      bet(amount, options = {}) {
        return registry.bet(registry.stateOf(this), amount, options);
      }
      // Takes every piece off the board.
      clear() {
        const table = registry.stateOf(this);
        for (const piece of [...table.pieces.values()]) {
          registry.removePiece(piece);
        }
      }
      save() {
        return registry.save(registry.stateOf(this));
      }
      load(saved) {
        registry.load(Piece, registry.stateOf(this), saved);
      }
      // A deck of pieces the players hold in their hands, playing cards by default:
      // { pieces, jokers, models, position, height, playPile, playPosition, holdAnimation }.
      createDeck(options = {}) {
        return registry.decks.create(registry.stateOf(this), options);
      }
      // The table's decks, oldest first.
      get decks() {
        return registry.decks.decksOf(registry.stateOf(this)).map((deck) => deck.handle);
      }
      // A seated player's close look at the table, as the game's dice players have (their key C);
      // again, their seat's view.
      lookCloser(player) {
        registry.decks.look(registry.stateOf(this), player);
      }
      destroy() {
        registry.destroy(registry.stateOf(this));
      }
      toString() {
        return `Tabletop(${this.id})`;
      }
    }
    return Tabletop;
  }

  // The class of a table's pieces (TabletopPiece), for instanceof.
  definePiece() {
    this.define();
    return this.Piece;
  }

  makePieceClass() {
    const registry = this;
    class TabletopPiece {
      get id() {
        return registry.pieceState(this).id;
      }
      get table() {
        return registry.pieceState(this).table.handle;
      }
      get model() {
        return registry.pieceState(this).prop.model;
      }
      // The prop that shows it.
      get prop() {
        return registry.pieceState(this).prop;
      }
      get cell() {
        return { ...registry.pieceState(this).cell };
      }
      get side() {
        return registry.pieceState(this).side;
      }
      set side(value) {
        registry.pieceState(this).side = value;
      }
      // The game's own data on the piece (a king's crown, a card's value); saved with the board.
      get data() {
        return registry.pieceState(this).data;
      }
      set data(value) {
        registry.pieceState(this).data = value ?? {};
      }
      get removed() {
        return registry.pieceState(this, true).removed;
      }
      // Moves it to a cell: { path: 'hop' (default), 'slide' or 'jump' (at once), ms, height }.
      moveTo(cell, options = {}) {
        return registry.movePiece(registry.pieceState(this), cell, options);
      }
      // Shows it to these players only (cards in a hand), or to everyone again (null).
      showOnlyTo(players) {
        const piece = registry.pieceState(this);
        piece.visibleTo = players === null || players === undefined ? null : [players].flat();
        piece.prop.showOnlyTo(piece.visibleTo);
      }
      remove() {
        registry.removePiece(registry.pieceState(this));
      }
      toString() {
        return `TabletopPiece(${this.id})`;
      }
    }
    return TabletopPiece;
  }

  stateOf(handle) {
    const table = this.byHandle?.get(handle);
    if (!table || this.tables.get(table.id) !== table) {
      throw new Error('Tabletop: the table is gone');
    }
    return table;
  }

  pieceState(handle, gone = false) {
    const piece = this.pieceByHandle?.get(handle);
    if (!piece || (piece.removed && !gone)) {
      throw new Error('Tabletop: the piece is gone');
    }
    return piece;
  }

  player(id) {
    return this.host.players.get(id) ?? null;
  }

  create(Tabletop, options) {
    if (options === null || typeof options !== 'object') {
      throw new TypeError('Tabletop.create needs options');
    }
    if (this.tables.size >= MAX_TABLES) {
      throw new RangeError(`Tabletop: at most ${MAX_TABLES} tables`);
    }
    const board = options.board ?? {};
    const dimension = options.dimension ?? GLOBAL_WORLD;
    const table = {
      id: this.nextId,
      at: pointOf(options.position, 'position'),
      yaw: yawOf(options.rotation),
      dimension,
      rows: wholeIn(board.rows ?? 8, 1, MAX_CELLS, 'board.rows'),
      columns: wholeIn(board.columns ?? 8, 1, MAX_CELLS, 'board.columns'),
      cellSize: amountIn(board.cellSize, 0.06, 0.01, 4, 'board.cellSize'),
      height: amountIn(board.height, 0, -2, 2, 'board.height'),
      stackHeight: amountIn(board.stackHeight, 0.01, 0, 1, 'board.stackHeight'),
      seats: [],
      pieces: new Map(),
      nextPiece: 1,
      playing: false,
      turn: 0,
      turnSeconds: amountIn(options.turnSeconds, 0, 0, 3600, 'turnSeconds'),
      turnTimer: null,
      camera: cameraOf(options.camera, 'camera'),
      controls: controlsOf(options.controls),
      pot: { item: null, amount: 0, from: new Map() },
      selection: new Map(),
      highlights: [],
      boardProp: null,
      [OWNER]: runningResource(this.host),
    };
    const seats = options.seats ?? [];
    if (!Array.isArray(seats) || seats.length > MAX_SEATS) {
      throw new RangeError(`Tabletop: seats is a list of at most ${MAX_SEATS}`);
    }
    for (const [index, seat] of seats.entries()) {
      if (!seat || typeof seat !== 'object') {
        throw new TypeError(`Tabletop: seat ${index} must be an object`);
      }
      table.seats.push({
        at: pointOf(seat.position, `seats[${index}].position`),
        yaw: yawOf(seat.rotation),
        side: seat.side ?? index,
        animation: seat.animation ?? null,
        // A camera shot of this seat's own (Camera.startShot's), before the table's: each player
        // looks at the board from their own side, as the game's dice players do.
        camera: cameraOf(seat.camera, `seats[${index}].camera`),
        // The game's own seat is here: the player sits on it with the game's sit-down
        // (watchChairs), not with the table's Sit down, and nothing moves or animates them. They
        // take the seat when they sit within `reach` of its position (a long bench of a tavern's
        // seats several, its seat places a little apart).
        chair: seat.chair === true,
        reach: amountIn(seat.reach, CHAIR_REACH, 0.2, 3, `seats[${index}].reach`),
        playerId: null,
      });
      if (seat.chair !== undefined && typeof seat.chair !== 'boolean') {
        throw new TypeError(`Tabletop: seats[${index}].chair is true or false`);
      }
      if (seat.chair === true && seat.animation) {
        throw new TypeError(`Tabletop: seats[${index}] is a chair: the game's seat animates it`);
      }
    }
    if (board.model) {
      table.boardProp = this.api.Prop.spawn(
        board.model,
        table.at,
        table.yaw,
        board.scale ?? 1,
        'static',
        dimension,
      );
    }
    this.nextId += 1;
    table.handle = Object.create(Tabletop.prototype);
    this.byHandle ??= new WeakMap();
    this.byHandle.set(table.handle, table);
    this.tables.set(table.id, table);
    this.watchChairs();
    this.announce(table);
    return table.handle;
  }

  cellUnder(table, point) {
    const back = table.yaw.clone().conjugate();
    const local = back.rotateVector(
      new Vector3(point.x - table.at.x, point.y - table.at.y, point.z - table.at.z),
    );
    const column = Math.round(local.x / table.cellSize + (table.columns - 1) / 2);
    const row = Math.round(local.y / table.cellSize + (table.rows - 1) / 2);
    if (row < 0 || row >= table.rows || column < 0 || column >= table.columns) {
      return null;
    }
    return { row, column };
  }

  stack(table, cell) {
    return [...table.pieces.values()].filter(
      (piece) => piece.cell.row === cell.row && piece.cell.column === cell.column,
    );
  }

  addPiece(Piece, table, options) {
    if (options === null || typeof options !== 'object' || typeof options.model !== 'string') {
      throw new TypeError('Tabletop.addPiece needs { model, cell }');
    }
    if (table.pieces.size >= MAX_PIECES) {
      throw new RangeError(`Tabletop: at most ${MAX_PIECES} pieces on a table`);
    }
    const cell = this.cellOf(table, options.cell);
    const level = this.stack(table, cell).length;
    const turn =
      options.rotation === undefined ? table.yaw : table.yaw.clone().mul(yawOf(options.rotation));
    const prop = this.api.Prop.spawn(
      options.model,
      this.worldOf(table, cell, level),
      turn,
      options.scale ?? 1,
      'none',
      table.dimension,
    );
    const piece = {
      id: table.nextPiece,
      table,
      cell,
      side: options.side ?? null,
      data: options.data ?? {},
      prop,
      visibleTo: null,
      turn: options.rotation ?? null,
      removed: false,
      moving: null,
    };
    table.nextPiece += 1;
    piece.handle = Object.create(Piece.prototype);
    this.pieceByHandle ??= new WeakMap();
    this.pieceByHandle.set(piece.handle, piece);
    table.pieces.set(piece.id, piece);
    if (options.visibleTo !== undefined && options.visibleTo !== null) {
      piece.handle.showOnlyTo(options.visibleTo);
    }
    return piece.handle;
  }

  // Moves a piece along a hop (an arc), a slide (straight) or at once; the players' games glide it.
  movePiece(piece, target, options) {
    const table = piece.table;
    const cell = this.cellOf(table, target);
    const path = options.path ?? 'hop';
    if (!['hop', 'slide', 'jump'].includes(path)) {
      throw new TypeError("Tabletop: path is 'hop', 'slide' or 'jump'");
    }
    const ms = amountIn(options.ms, path === 'jump' ? 0 : 400, 0, MAX_MOVE_MS, 'ms');
    const lift = amountIn(options.height, Math.max(table.cellSize, 0.05), 0, 2, 'height');
    if (piece.moving) {
      clearTimeout(piece.moving.timer);
      piece.moving.done();
      piece.moving = null;
    }
    piece.cell = cell;
    const others = this.stack(table, cell).filter((other) => other !== piece);
    const to = this.worldOf(table, cell, others.length);
    if (path === 'jump' || ms === 0) {
      piece.prop.position = to;
      return Promise.resolve(true);
    }
    // The players' games glide it (Prop.glideTo): the server sends where to once.
    piece.prop.glideTo(to, undefined, { ms: Math.round(ms), arc: path === 'hop' ? lift : 0 });
    return new Promise((resolve) => {
      const timer = setTimeout(() => {
        piece.moving = null;
        resolve(!piece.removed);
      }, ms);
      piece.moving = { timer, done: () => resolve(false) };
    });
  }

  removePiece(piece) {
    if (piece.removed) {
      return;
    }
    if (piece.moving) {
      clearTimeout(piece.moving.timer);
      piece.moving = null;
    }
    piece.removed = true;
    piece.table.pieces.delete(piece.id);
    for (const [playerId, selected] of piece.table.selection) {
      if (selected === piece) {
        piece.table.selection.delete(playerId);
      }
    }
    if (piece.prop.exists !== false) {
      piece.prop.destroy();
    }
  }

  // Marks cells for some players or for everyone. The marks are data the players' games draw flat
  // on the board (the server's wire boxes stood half in the board and flickered, live, chess).
  highlight(table, cells, options) {
    const list = Array.isArray(cells) && !Number.isInteger(cells[0]) ? cells : [cells];
    const players = options.players === undefined ? undefined : [options.players].flat();
    const style = options.style ?? 'frame';
    if (!MARK_STYLES.includes(style)) {
      throw new RangeError(`Tabletop highlight: style is one of ${MARK_STYLES.join(', ')}`);
    }
    const color = options.color ?? MARK_COLOR;
    if (!Number.isInteger(color) || color < 0 || color > 0xffffffff) {
      throw new RangeError('Tabletop highlight: color is a 0xAARRGGBB number');
    }
    if (table.highlights.length >= MAX_MARKS) {
      throw new RangeError(`Tabletop highlight: a table keeps at most ${MAX_MARKS} marks`);
    }
    const marked = list.map((value) => {
      const cell = this.cellOf(table, value);
      return [cell.row, cell.column];
    });
    if (marked.length === 0) {
      return;
    }
    table.highlights.push({
      cells: marked,
      color,
      style,
      players: players === undefined ? undefined : players.map((player) => player.id),
    });
    this.sendMarks(table);
  }

  clearHighlight(table, players) {
    const only = players === undefined ? undefined : [players].flat().map((player) => player.id);
    const before = table.highlights.length;
    table.highlights = table.highlights.filter((mark) => {
      const forThem =
        only === undefined ||
        (mark.players !== undefined && mark.players.every((id) => only.includes(id)));
      return !forThem;
    });
    if (table.highlights.length !== before) {
      this.sendMarks(table);
    }
  }

  // The marks a player sees on a table: everyone's and their own.
  marksFor(table, player) {
    return table.highlights
      .filter((mark) => mark.players === undefined || mark.players.includes(player.id))
      .map((mark) => ({ cells: mark.cells, color: mark.color, style: mark.style }));
  }

  sendMarks(table) {
    for (const player of this.host.players.values()) {
      if (player.dimension === table.dimension) {
        this.sendMarksTo(table, player);
      }
    }
  }

  sendMarksTo(table, player) {
    const marks = this.marksFor(table, player);
    try {
      this.services.emitToPlayer(player.id, EVENT, { op: 'marks', table: table.id, marks });
    } catch (error) {
      this.host.bridge.log('host', `tabletop marks: ${error.message}`);
    }
  }

  seatIndex(table, player) {
    if (!this.isPlayer(player)) {
      return -1;
    }
    return table.seats.findIndex((seat) => seat.playerId === player.id);
  }

  // The camera shot of a seat: its own, else the table's. Null for none, and for 'eyes': the
  // game's own first-person view, from the player's own eyes wherever their seat put the body (a
  // shot of the seat's place showed a body from behind at a game's bench).
  seatShot(table, index) {
    const seat = table.seats[index];
    const shot = seat?.camera ?? table.camera;
    return shot === 'eyes' ? null : shot ?? null;
  }

  // A player sits: by a script, or by their own request (`asked`), which handlers may refuse.
  sit(table, player, seatValue, asked) {
    if (!this.isPlayer(player)) {
      throw new TypeError('Tabletop.sit takes a Player');
    }
    const index = wholeIn(seatValue, 0, table.seats.length - 1, 'seat');
    const seat = table.seats[index];
    if (seat.playerId !== null || this.seatIndex(table, player) >= 0) {
      return false;
    }
    if (player.dimension !== table.dimension) {
      return false;
    }
    // A chair seat is taken by sitting on the game's seat there; asking to sit does not.
    if (seat.chair && asked && !player.seated) {
      return false;
    }
    if (!this.host.emit('tabletopSit', table.handle, player, index)) {
      return false;
    }
    seat.playerId = player.id;
    if (!seat.chair) {
      player.position = seat.at;
      player.rotation = seat.yaw;
    }
    if (seat.animation) {
      const { fragment = '', ...rest } = seat.animation;
      player.playAnimation(fragment, { loop: true, lockMovement: true, ...rest });
    }
    const shot = this.seatShot(table, index);
    if (shot) {
      this.api.Camera.startShot(player, shot);
    }
    this.host.emitToPlayerClient(player.id, EVENT, { op: 'seated', table: table.id, seat: index });
    this.announce(table);
    this.decks.playerSat(table, player);
    this.host.emit('tabletopSeated', table.handle, player, index);
    return true;
  }

  // A seated player leaves: they stood, left the server or the table went. A player who leaves a
  // game in play forfeits it.
  leave(table, player, reason) {
    const index = this.seatIndex(table, player);
    if (index < 0) {
      return false;
    }
    table.seats[index].playerId = null;
    table.selection.delete(player.id);
    this.decks.playerLeft(table, player);
    if (reason !== 'disconnect') {
      if (table.seats[index].animation) {
        player.stopAnimation();
      }
      if (table.seats[index].camera || table.camera) {
        this.api.Camera.endShot(player);
      }
      this.clearHighlight(table, player);
      this.host.emitToPlayerClient(player.id, EVENT, { op: 'stood', table: table.id });
    }
    this.host.emit('tabletopLeave', table.handle, player, index, reason);
    if (table.playing && reason !== 'removed') {
      this.host.emit('tabletopForfeit', table.handle, player, index, reason);
      const left = table.seats.filter((seat) => seat.playerId !== null);
      if (table.playing && left.length === 1) {
        this.finish(table, this.player(left[0].playerId), 'forfeit');
      } else if (table.playing && left.length === 0) {
        this.finish(table, null, 'forfeit');
      } else if (table.playing && table.turn === index) {
        this.nextTurn(table);
      }
    }
    this.announce(table);
    return true;
  }

  get services() {
    return this.host.services;
  }

  start(table, options) {
    const seated = table.seats.filter((seat) => seat.playerId !== null);
    if (seated.length === 0) {
      throw new Error('Tabletop.start: nobody sits at the table');
    }
    table.playing = true;
    table.turn = -1;
    const first = options.turn ?? table.seats.findIndex((seat) => seat.playerId !== null);
    this.host.emit('tabletopStart', table.handle);
    this.giveTurn(table, wholeIn(first, 0, table.seats.length - 1, 'turn'));
  }

  giveTurn(table, index) {
    clearTimeout(table.turnTimer);
    table.turnTimer = null;
    table.turn = index;
    const player = this.player(table.seats[index].playerId);
    this.host.emit('tabletopTurn', table.handle, index, player);
    if (table.turnSeconds > 0) {
      table.turnTimer = setTimeout(() => this.turnTimedOut(table, index), table.turnSeconds * 1000);
    }
  }

  turnTimedOut(table, index) {
    table.turnTimer = null;
    if (!table.playing || table.turn !== index || this.tables.get(table.id) !== table) {
      return;
    }
    const player = this.player(table.seats[index].playerId);
    if (this.host.emit('tabletopTurnTimeout', table.handle, index, player)) {
      this.nextTurn(table);
    } else if (table.turnSeconds > 0) {
      table.turnTimer = setTimeout(() => this.turnTimedOut(table, index), table.turnSeconds * 1000);
    }
  }

  nextTurn(table) {
    if (!table.playing) {
      throw new Error('Tabletop.nextTurn: no game is on');
    }
    const count = table.seats.length;
    for (let step = 1; step <= count; step += 1) {
      const index = (table.turn + step) % count;
      if (table.seats[index].playerId !== null) {
        this.giveTurn(table, index);
        return index;
      }
    }
    return table.turn;
  }

  finish(table, winnerValue, reason) {
    if (!table.playing) {
      return;
    }
    let winner = null;
    if (typeof winnerValue === 'number') {
      winner = this.player(
        table.seats[wholeIn(winnerValue, 0, table.seats.length - 1, 'winner')].playerId,
      );
    } else if (this.isPlayer(winnerValue)) {
      winner = winnerValue;
    }
    table.playing = false;
    clearTimeout(table.turnTimer);
    table.turnTimer = null;
    table.selection.clear();
    this.payPot(table, winner);
    this.host.emit('tabletopEnd', table.handle, winner, String(reason));
  }

  async bet(table, amountValue, options) {
    const amount = wholeIn(amountValue, 1, 1000000, 'amount');
    const item = options.item ?? 'money';
    if (table.pot.amount > 0 && table.pot.item !== item) {
      throw new Error('Tabletop.bet: the pot holds another item');
    }
    const players = table.seats
      .filter((seat) => seat.playerId !== null)
      .map((seat) => this.player(seat.playerId))
      .filter(Boolean);
    const taken = [];
    for (const player of players) {
      const answer = await player.takeItem(item, amount);
      if (!answer || answer.removed < amount) {
        // All or nothing: what was taken goes back.
        for (const back of taken) {
          back.giveItem(item, amount);
        }
        if (answer?.removed > 0) {
          player.giveItem(item, answer.removed);
        }
        return false;
      }
      taken.push(player);
    }
    table.pot.item = item;
    table.pot.amount += amount * taken.length;
    for (const player of taken) {
      table.pot.from.set(player.id, (table.pot.from.get(player.id) ?? 0) + amount);
    }
    return true;
  }

  // The winner takes the pot; without a winner everyone gets back what they put in.
  payPot(table, winner) {
    const { item, amount, from } = table.pot;
    if (amount > 0) {
      if (winner) {
        winner.giveItem(item, amount);
      } else {
        for (const [playerId, share] of from) {
          this.player(playerId)?.giveItem(item, share);
        }
      }
    }
    table.pot = { item: null, amount: 0, from: new Map() };
  }

  save(table) {
    return {
      rows: table.rows,
      columns: table.columns,
      turn: table.playing ? table.turn : -1,
      pieces: [...table.pieces.values()].map((piece) => ({
        model: piece.prop.model,
        cell: [piece.cell.row, piece.cell.column],
        side: piece.side,
        data: piece.data,
        rotation: piece.turn,
        visibleTo: piece.visibleTo === null ? null : piece.visibleTo.map((player) => player.id),
      })),
    };
  }

  load(Piece, table, saved) {
    if (!saved || typeof saved !== 'object' || !Array.isArray(saved.pieces)) {
      throw new TypeError('Tabletop.load takes what save() gave');
    }
    for (const piece of [...table.pieces.values()]) {
      this.removePiece(piece);
    }
    for (const entry of saved.pieces) {
      const visibleTo =
        entry.visibleTo === null || entry.visibleTo === undefined
          ? undefined
          : entry.visibleTo.map((id) => this.player(id)).filter(Boolean);
      this.addPiece(Piece, table, {
        model: entry.model,
        cell: entry.cell,
        side: entry.side,
        data: entry.data,
        rotation: entry.rotation ?? undefined,
        visibleTo: visibleTo?.length ? visibleTo : undefined,
      });
    }
  }

  destroy(table) {
    for (const seat of table.seats) {
      const player = seat.playerId === null ? null : this.player(seat.playerId);
      if (player) {
        this.leave(table, player, 'removed');
      }
    }
    if (table.playing) {
      this.finish(table, null, 'removed');
    }
    this.clearHighlight(table);
    this.decks.tableRemoved(table);
    for (const piece of [...table.pieces.values()]) {
      this.removePiece(piece);
    }
    if (table.boardProp && table.boardProp.exists !== false) {
      table.boardProp.destroy();
    }
    clearTimeout(table.turnTimer);
    this.tables.delete(table.id);
    this.watchChairs();
    this.announce(table);
  }

  // A player's request from their game: sit, stand or a click on a cell.
  receive(player, name, payload) {
    if (name !== EVENT) {
      return false;
    }
    const table = this.tables.get(Number(payload?.table));
    if (!table || !this.isPlayer(player)) {
      return true;
    }
    try {
      if (payload.op === 'sit') {
        this.sit(table, player, payload.seat, true);
      } else if (payload.op === 'stand') {
        this.leave(table, player, 'stood');
      } else if (payload.op === 'click') {
        this.click(table, player, this.cellOf(table, payload.cell));
      } else if (payload.op === 'deck') {
        this.decks.receive(table, player, payload);
      }
    } catch (error) {
      this.host.bridge.log('host', `tabletop request: ${error.message}`);
    }
    return true;
  }

  // A seated player clicked a cell: a piece of theirs picks it, a picked piece asks to move there.
  click(table, player, cell) {
    const seat = this.seatIndex(table, player);
    if (seat < 0) {
      return;
    }
    if (table.playing && table.turn !== seat) {
      return;
    }
    // A game of its own (one that places pieces) takes the click; false ends it here.
    if (!this.host.emit('tabletopCellClick', table.handle, player, { ...cell })) {
      return;
    }
    const top = this.stack(table, cell).at(-1) ?? null;
    const picked = table.selection.get(player.id) ?? null;
    if (picked && top === picked) {
      table.selection.delete(player.id);
      this.clearHighlight(table, player);
      return;
    }
    if (top && (!picked || top.side === picked.side)) {
      if (this.host.emit('tabletopPieceSelect', table.handle, player, top.handle)) {
        table.selection.set(player.id, top);
        this.clearHighlight(table, player);
        this.highlight(table, [[top.cell.row, top.cell.column]], { players: [player] });
      }
      return;
    }
    if (!picked) {
      return;
    }
    table.selection.delete(player.id);
    this.clearHighlight(table, player);
    if (this.host.emit('tabletopMoveRequest', table.handle, player, picked.handle, { ...cell })) {
      this.movePiece(picked, cell, {});
    }
  }

  // What a player's game knows of a table: its board and seats.
  describe(table) {
    return {
      id: table.id,
      position: table.at,
      rotation: { w: table.yaw.w, x: table.yaw.x, y: table.yaw.y, z: table.yaw.z },
      rows: table.rows,
      columns: table.columns,
      cellSize: table.cellSize,
      height: table.height,
      // A camera shot of the table's own: the player's view does not follow the mouse. A shot
      // with `look` follows it within its limits (lookView): the crosshair points, and the game's
      // own mouse look is held off.
      // 'eyes' is the game's own first-person view: neither.
      fixedView: table.camera !== null && table.camera !== 'eyes' && !table.camera.look,
      lookView: table.camera !== 'eyes' && Boolean(table.camera?.look),
      controls: table.controls,
      // A table with a deck: a seated holder's keys point, select and play pieces (TabletopDeck).
      deck: this.decks.decksOf(table).length > 0,
      seats: table.seats.map((seat, index) => {
        const shot = this.seatShot(table, index);
        return {
          position: seat.at,
          taken: seat.playerId !== null,
          chair: seat.chair,
          fixedView: shot !== null && !shot.look,
          lookView: Boolean(shot?.look),
        };
      }),
    };
  }

  // Tells every player of the table's dimension the tables there.
  announce(table) {
    for (const player of this.host.players.values()) {
      if (player.dimension === table.dimension) {
        this.sendTables(player);
      }
    }
  }

  sendTables(player) {
    const tables = [...this.tables.values()]
      .filter((table) => table.dimension === player.dimension)
      .map((table) => this.describe(table));
    try {
      this.services.emitToPlayer(player.id, EVENT, { op: 'tables', tables });
    } catch (error) {
      this.host.bridge.log('host', `tabletop tables: ${error.message}`);
    }
    for (const table of this.tables.values()) {
      if (table.dimension === player.dimension && table.highlights.length > 0) {
        this.sendMarksTo(table, player);
      }
    }
  }

  // A player's game started its resources: it learns the tables of its dimension.
  playerReady(player) {
    this.sendTables(player);
  }

  playerLeft(player) {
    for (const table of this.tables.values()) {
      this.leave(table, player, 'disconnect');
    }
  }

  resourceStopped(name) {
    removeOwned(this.tables.values(), name, (table) => this.destroy(table));
  }
}

module.exports = { Tabletops, TABLETOP_EVENT: EVENT };
