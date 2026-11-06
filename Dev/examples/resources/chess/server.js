'use strict';
// An example resource on the Tabletop SDK (not shipped with the server: copy this folder into
// your server's resources): chess with the real rules (rules.js: check, castling, en passant,
// promotion, checkmate, stalemate and the draws). "/chess" puts the board on the table in front of
// the player, the level's own or any other, with the game's benches at two sides; with no table
// there it stands one of its own. Two players sit down on the benches (the game's E), white moves
// first. A seated player points at a piece with the crosshair and clicks, then clicks where it
// goes: the legal moves show as green dots, the last move's cells yellow and a king in check red. The pieces slide, the knight jumps, the rook follows the king
// when castling, and a taken piece leaps off the board and stands beside it.
//
//   /chess            the board on the table in front of you, or a table of its own 2 m ahead
//   /chess table      always a table of its own (at most MAX_TABLES on the server)
//   /chess remove     takes away the table you stand nearest to, when no game is on it
//   /resign           gives up your game
//   /draw             offers a draw; the other player's /draw accepts it
//   /promote q|r|b|n  what your pawn becomes on the last rank (a queen by default)
//   console: chess <x> <y> <z> [degrees]   a table at a place
//            chess near <player id> [table]  as that player's /chess
//            chess move e2e4               the side to move plays it (demos, tests)
//
// The models are Poly Haven's "Chess Set" by Riley Queen (CC0), converted with
// Dev/tools/assets/gltf/kcds_gltf.py (stream/objects/kcds/chess, CREDITS.md).

const rules = require('./rules');

const { Tabletop, Prop, SmartObject } = kcds.world;
const { Events, Chat } = kcds.core;
const { Vector3 } = kcds.math;

const MODELS = 'objects/kcds/chess';
const TABLE_MODEL = 'table_fancy_a_square_small';
// The game's own bench for one (a smart object: the game's "Sit" with E, its walk up and
// sit-down), its sitter facing along its +y. The one "at a table" rests both hands on the board,
// in the view; this one sits with the hands in the lap. Its seat is BENCH_SEAT
// metres along its +y from the prefab's origin (Prefabs/Bench/bench_1place_low.xml).
const BENCH = 'Bench/bench_1place_low';
const BENCH_SEAT = 0;
// The table's top above the floor, the board's top above its base, and a cell (Poly Haven's board:
// 55 cm, 8 cells of 5.8 cm).
const TABLE_TOP = 0.75;
const BOARD_TOP = 0.0174;
const CELL = 0.058;
// The board's turn on the table, degrees: its light corner at each player's right (h1 and a8; seen
// live from above: a quarter turn put a light square on a1).
const BOARD_TURN = 0;
// How far from the table's middle a player sits.
const SEAT_DISTANCE = 0.68;
// A table's top in front of a player (/chess): looked for straight down at these distances ahead,
// this high above their feet.
const TABLE_SEARCH = [0.8, 1.1, 1.4];
const TABLE_LOWEST = 0.55;
const TABLE_HIGHEST = 1.2;
// Where taken pieces stand: beside the board, in two rows per side.
const GRAVEYARD_SIDE = 0.33;
const GRAVEYARD_STEP = 0.045;
const MAX_TABLES = 8;
// Tables stand at least this far apart (a table and its benches take 2 m); boards on the level's
// own tables at least this far.
const MIN_TABLE_DISTANCE = 3;
const MIN_BOARD_DISTANCE = 1.5;
// A seat by the level's own table is taken by sitting on its bench or chair within this of the
// seat's place: a tavern's long bench seats a player anywhere along it.
const LEVEL_SEAT_REACH = 1.3;
const NEW_GAME_SECONDS = 12;
// The marks: the cells a picked piece may go to (dots), the last move's two cells (tinted) and a
// king in check (a frame).
const COLOR_LEGAL = 0xff2f7d3a;
const COLOR_LAST = 0xffe8c45a;
const COLOR_CHECK = 0xffe0362e;
const NAMES = { k: 'king', q: 'queen', r: 'rook', b: 'bishop', n: 'knight', p: 'pawn' };

// table -> game
const games = new Map();

function modelOf(kind, color) {
  return `${MODELS}/${NAMES[kind]}_${color}.cgf`;
}

// A point of the table, from its own x (to the right of the black player) and y (towards white).
function place(game, x, y, z = 0) {
  const radians = (game.yaw * Math.PI) / 180;
  const cos = Math.cos(radians);
  const sin = Math.sin(radians);
  return new Vector3(game.at.x + x * cos - y * sin, game.at.y + x * sin + y * cos, game.at.z + z);
}

// The Tabletop's cell of a square of the rules: white sits at the table's +y, so white's rank 1 is
// the table's last row and the a-file its last column.
function cellOf(square) {
  return { row: 7 - square.row, column: 7 - square.column };
}

function squareOf(cell) {
  return { row: 7 - cell.row, column: 7 - cell.column };
}

function sideOf(game, player) {
  const seat = game.table.seatOf(player);
  return seat < 0 ? null : game.table.seats[seat].side;
}

function seated(game) {
  return game.table.seats.map((seat) => seat.player).filter((player) => player);
}

function tell(game, text) {
  for (const player of seated(game)) {
    Chat.sendToPlayer(player, text);
  }
}

// A chess table: `at` the floor under the board's middle, `yaw` its turn. With `top` (the height of
// a table's top above `at`) the board goes on a table that is there, the level's own or any
// other, and the players sit on the benches or chairs already at its sides (a tavern's); without
// it the resource stands a table of its own. `benches` stands the game's benches at both sides
// (with a table of its own always).
function build(at, yaw, top = null, benches = top === null) {
  if (games.size >= MAX_TABLES) {
    return null;
  }
  // Not on top of another table: two tables in one place split the players between them. A
  // board on a tavern's table needs no room for benches, so the next table may have one too.
  const apart = benches ? MIN_TABLE_DISTANCE : MIN_BOARD_DISTANCE;
  for (const other of games.values()) {
    if (Math.hypot(other.at.x - at.x, other.at.y - at.y) < apart) {
      return null;
    }
  }
  const game = {
    at,
    yaw,
    top: top ?? TABLE_TOP,
    table: null,
    props: [],
    benches: [],
    graveyard: [],
    taken: { white: 0, black: 0 },
    position: null,
    history: [],
    moves: [],
    busy: false,
    promotion: new Map(),
    drawOffer: null,
    restart: null,
  };
  if (top === null) {
    game.props.push(Prop.spawn(TABLE_MODEL, at, new Vector3(0, 0, yaw)));
  }
  game.props.push(
    Prop.spawn(
      `${MODELS}/board.cgf`,
      place(game, 0, 0, game.top),
      new Vector3(0, 0, yaw + BOARD_TURN),
      1,
      'none',
    ),
  );
  const seats = [];
  for (const side of ['white', 'black']) {
    const sign = side === 'white' ? 1 : -1;
    const y = sign * SEAT_DISTANCE;
    // White faces the table's -y, black its +y.
    const facing = side === 'white' ? yaw + 180 : yaw;
    const half = (facing * Math.PI) / 360;
    if (benches) {
      game.benches.push(
        SmartObject.spawn({
          blueprint: BENCH,
          position: place(game, 0, y + sign * BENCH_SEAT),
          rotation: [0, 0, Math.sin(half), Math.cos(half)],
          name: `chess ${side}`,
        }),
      );
    }
    seats.push({
      position: place(game, 0, y),
      rotation: facing,
      side,
      // The player sits on the game's bench or chair there, ours or the level's, with its own walk
      // up and sit-down (a seat of the table's own put the body on the stool at once, live).
      chair: true,
      reach: benches ? undefined : LEVEL_SEAT_REACH,
      // No camera shot: on the game's bench they look at the board from their own eyes, in first
      // person, and the mouse looks about; the arrows pick.
    });
  }
  game.table = Tabletop.create({
    position: place(game, 0, 0, game.top),
    rotation: yaw,
    board: { rows: 8, columns: 8, cellSize: CELL, height: BOARD_TOP },
    seats,
    turnSeconds: 0,
    // The arrows (and W A S D) move the mark, the mouse only looks about: aiming the view moved
    // the mark as the camera turned.
    controls: { pointer: 'keys' },
  });
  games.set(game.table, game);
  return game;
}

function destroy(game) {
  clearTimeout(game.restart);
  for (const prop of [...game.props, ...game.graveyard]) {
    prop.destroy();
  }
  for (const bench of game.benches) {
    bench.destroy();
  }
  games.delete(game.table);
  game.table.destroy();
}

function clearGraveyard(game) {
  for (const prop of game.graveyard) {
    prop.destroy();
  }
  game.graveyard = [];
  game.taken = { white: 0, black: 0 };
}

function newGame(game) {
  clearTimeout(game.restart);
  game.restart = null;
  game.table.clear();
  game.table.clearHighlight();
  clearGraveyard(game);
  game.position = rules.initialPosition();
  game.history = [rules.positionKey(game.position)];
  game.moves = [];
  game.busy = false;
  game.drawOffer = null;
  for (let row = 0; row < 8; row++) {
    for (let column = 0; column < 8; column++) {
      const square = game.position.board[row][column];
      if (square) {
        game.table.addPiece({
          model: modelOf(square.kind, square.color),
          cell: cellOf({ row, column }),
          side: square.color,
          data: { kind: square.kind },
        });
      }
    }
  }
  game.table.start();
  tell(game, 'Chess: a new game. White moves first.');
}

// Marks the last move for everyone and a king in check.
function markBoard(game, move) {
  game.table.clearHighlight();
  if (move) {
    game.table.highlight([cellOf(move.from), cellOf(move.to)], {
      color: COLOR_LAST,
      style: 'fill',
    });
  }
  if (rules.inCheck(game.position, game.position.turn)) {
    game.table.highlight([cellOf(rules.kingOf(game.position, game.position.turn))], {
      color: COLOR_CHECK,
      style: 'frame',
    });
  }
}

// A taken piece leaps up off its cell and stands beside the board on its owner's side.
async function take(game, piece) {
  const color = piece.side;
  const kind = piece.data.kind;
  await piece.moveTo(piece.cell, { path: 'hop', ms: 350, height: 0.12 });
  piece.remove();
  const index = game.taken[color]++;
  const x =
    (color === 'white' ? -1 : 1) * (GRAVEYARD_SIDE + Math.floor(index / 8) * GRAVEYARD_STEP);
  const y = (color === 'white' ? 1 : -1) * ((index % 8) - 3.5) * GRAVEYARD_STEP;
  game.graveyard.push(
    Prop.spawn(
      modelOf(kind, color),
      place(game, x, y, game.top),
      new Vector3(0, 0, game.yaw),
      1,
      'none',
    ),
  );
}

function distance(from, to) {
  return Math.hypot(from.row - to.row, from.column - to.column);
}

// Plays a legal move on the table: what it takes, the piece, the rook of a castling, a promotion.
async function perform(game, player, move) {
  game.busy = true;
  const table = game.table;
  const before = game.position;
  const text = rules.notation(before, move);
  table.clearHighlight();
  try {
    if (move.captureAt) {
      const taken = table.pieceAt(cellOf(move.captureAt));
      if (taken) {
        await take(game, taken);
      }
    }
    const piece = table.pieceAt(cellOf(move.from));
    const ms = Math.round(260 + 110 * distance(move.from, move.to));
    if (move.kind === 'n') {
      await piece.moveTo(cellOf(move.to), { path: 'hop', ms: 520, height: 0.07 });
    } else {
      await piece.moveTo(cellOf(move.to), { path: 'slide', ms });
    }
    if (move.castle) {
      const rook = table.pieceAt(cellOf(move.rookFrom));
      await rook.moveTo(cellOf(move.rookTo), { path: 'hop', ms: 480, height: 0.05 });
    }
    if (move.promotion) {
      piece.remove();
      table.addPiece({
        model: modelOf(move.promotion, move.color),
        cell: cellOf(move.to),
        side: move.color,
        data: { kind: move.promotion },
      });
    }
  } finally {
    game.busy = false;
  }
  if (!games.has(table) || game.position !== before) {
    return;
  }
  game.position = rules.play(before, move);
  game.moves.push(text);
  game.drawOffer = null;
  const number = Math.ceil(game.moves.length / 2);
  tell(game, `Chess ${number}${move.color === 'white' ? '.' : '...'} ${text}`);
  const result = rules.status(game.position, game.history);
  game.history.push(rules.positionKey(game.position));
  markBoard(game, move);
  if (result.over) {
    const winner =
      result.result === 'draw'
        ? null
        : table.seats.findIndex((seat) => seat.side === result.result);
    table.finish(winner, result.reason);
    return;
  }
  if (result.check) {
    tell(game, 'Check!');
  }
  table.nextTurn();
}

function nearest(player) {
  let best = null;
  let bestDistance = Infinity;
  for (const game of games.values()) {
    const away = Math.hypot(player.position.x - game.at.x, player.position.y - game.at.y);
    if (away < bestDistance) {
      best = game;
      bestDistance = away;
    }
  }
  return bestDistance < 5 ? best : null;
}

function gameOfPlayer(player) {
  for (const game of games.values()) {
    if (game.table.seatOf(player) >= 0) {
      return game;
    }
  }
  return null;
}

Events.on('tabletopSeated', (table, player) => {
  const game = games.get(table);
  if (!game) {
    return;
  }
  Chat.sendToPlayer(
    player,
    `Chess: you play ${sideOf(game, player)}. The arrows or W A S D move the mark, Enter or ` +
      'mouse 1 picks, the mouse looks around. U, then W, gets you up.',
  );
  if (table.state === 'idle' && seated(game).length === 2) {
    newGame(game);
  } else if (seated(game).length < 2) {
    Chat.sendToPlayer(player, 'Chess: waiting for a second player.');
  }
});

Events.on('tabletopTurn', (table, seat, player) => {
  const game = games.get(table);
  if (game) {
    Chat.sendToPlayer(player, `Your move (${table.seats[seat].side}).`);
  }
});

Events.on('tabletopPieceSelect', (table, player, piece) => {
  const game = games.get(table);
  if (!game) {
    return;
  }
  if (game.busy || game.position === null || piece.side !== game.position.turn) {
    return false;
  }
  if (sideOf(game, player) !== piece.side) {
    return false;
  }
  const moves = rules.legalMoves(game.position, squareOf(piece.cell));
  if (moves.length === 0) {
    return false;
  }
  // After the table's own mark of the picked piece.
  setTimeout(() => {
    const targets = moves.map((move) => cellOf(move.to));
    table.highlight(targets, { players: [player], color: COLOR_LEGAL, style: 'dot' });
  }, 0);
  return true;
});

Events.on('tabletopMoveRequest', (table, player, piece, cell) => {
  const game = games.get(table);
  if (!game) {
    return;
  }
  if (game.busy || game.position === null || sideOf(game, player) !== game.position.turn) {
    return false;
  }
  const promotion = game.promotion.get(player.id) ?? 'q';
  const move = rules.findMove(game.position, squareOf(piece.cell), squareOf(cell), promotion);
  if (move === null) {
    markBoard(game, null);
    return false;
  }
  perform(game, player, move).catch((error) => {
    console.error(`chess: ${error.stack ?? error}`);
  });
  // The resource moves the pieces itself.
  return false;
});

Events.on('tabletopEnd', (table, winner, reason) => {
  const game = games.get(table);
  if (!game) {
    return;
  }
  const result = winner ? `${winner.nickname} wins: ${reason}` : `a draw: ${reason}`;
  tell(game, `Chess: ${result}. Moves: ${game.moves.join(' ')}`);
  console.info(`chess: ${result} after ${game.moves.length} moves`);
  game.position = null;
  game.restart = setTimeout(() => {
    game.restart = null;
    if (games.has(table) && table.state === 'idle' && seated(game).length === 2) {
      newGame(game);
    }
  }, NEW_GAME_SECONDS * 1000);
});

Events.on('playerCommand', (player, command, args) => {
  if (command === 'chess') {
    if (args[0] === 'remove') {
      const game = nearest(player);
      if (!game || game.table.state !== 'idle') {
        Chat.sendToPlayer(player, 'Chess: no free table near you.');
        return;
      }
      destroy(game);
      Chat.sendToPlayer(player, 'Chess: the table is gone.');
      return;
    }
    placeFor(player, args[0]).then((text) => {
      Chat.sendToPlayer(player, `Chess: ${text}`);
    });
    return;
  }
  const game = gameOfPlayer(player);
  if (command === 'resign') {
    if (!game || game.position === null) {
      return;
    }
    const winner = game.table.seats.findIndex((seat) => seat.player && seat.player !== player);
    game.table.finish(winner >= 0 ? winner : null, `${player.nickname} resigned`);
  } else if (command === 'draw') {
    if (!game || game.position === null) {
      return;
    }
    if (game.drawOffer && game.drawOffer !== player.id) {
      game.table.finish(null, 'agreed');
      return;
    }
    game.drawOffer = player.id;
    tell(game, `Chess: ${player.nickname} offers a draw. /draw to accept.`);
  } else if (command === 'promote') {
    const kind = String(args[0] ?? '').toLowerCase();
    if (!['q', 'r', 'b', 'n'].includes(kind)) {
      Chat.sendToPlayer(player, 'Chess: /promote q, r, b or n.');
      return;
    }
    game?.promotion.set(player.id, kind);
    Chat.sendToPlayer(player, `Chess: your pawns become a ${NAMES[kind]}.`);
  }
});

// A table 2 m in front of a player, facing them.
function buildBefore(player) {
  const heading = player.rotation.rotateVector(new Vector3(0, 1, 0));
  const at = new Vector3(
    player.position.x + heading.x * 2,
    player.position.y + heading.y * 2,
    player.position.z,
  );
  const yaw = (Math.atan2(-heading.x, heading.y) * 180) / Math.PI;
  return build(at, Math.round(yaw));
}

// The top of a table in front of the player, or null: their game traces rays straight down at a
// few distances ahead, and a surface 0.55 to 1.2 m above their feet is a table's top. The board
// then goes on it, the level's own table or any other.
async function tableBefore(player) {
  const heading = player.rotation.rotateVector(new Vector3(0, 1, 0));
  const feet = player.position;
  for (const ahead of TABLE_SEARCH) {
    const x = feet.x + heading.x * ahead;
    const y = feet.y + heading.y * ahead;
    const from = new Vector3(x, y, feet.z + TABLE_HIGHEST + 0.4);
    const to = new Vector3(x, y, feet.z + TABLE_LOWEST - 0.1);
    let hit = null;
    try {
      ({ hit } = await kcds.world.World.raycast(player, from, to, { mode: 'cover' }));
    } catch {
      return null;
    }
    const height = hit ? hit.position.z - feet.z : null;
    if (height !== null && height >= TABLE_LOWEST && height <= TABLE_HIGHEST) {
      const yaw = (Math.atan2(-heading.x, heading.y) * 180) / Math.PI;
      return { at: new Vector3(x, y, feet.z), yaw: Math.round(yaw), top: height };
    }
  }
  return null;
}

// /chess: the board on the table in front of the player, any table of the level's or another,
// whose own benches or chairs the players sit on; with no table there, a table of its own.
// "benches": the game's benches at that table's sides too (a table without seats). "table":
// always a table of its own, with benches.
async function placeFor(player, mode) {
  const found = mode === 'table' ? null : await tableBefore(player);
  const benches = mode === 'benches';
  const game = found ? build(found.at, found.yaw, found.top, benches) : buildBefore(player);
  if (!game) {
    return 'no table here (another one stands too close, or there are too many)';
  }
  if (!found) {
    return 'a table. Sit down on a bench (E).';
  }
  const sit = benches
    ? 'Sit down on a bench (E).'
    : 'Sit down at its sides (E) on its bench or chair.';
  return `the board is on the table. ${sit}`;
}

// The console's move on the first table with a game on: "e2e4", "e7e8n".
function consoleMove(text) {
  const game = [...games.values()].find((candidate) => candidate.position !== null);
  const match = /^([a-h][1-8])([a-h][1-8])([qrbn])?$/.exec(String(text ?? ''));
  if (!game || !match || game.busy) {
    console.info('chess: no game, or a move such as e2e4');
    return;
  }
  const square = (name) => ({ row: Number(name[1]) - 1, column: 'abcdefgh'.indexOf(name[0]) });
  const move = rules.findMove(game.position, square(match[1]), square(match[2]), match[3] ?? 'q');
  if (!move) {
    console.info(`chess: ${text} is not legal`);
    return;
  }
  const seat = game.table.seats.find((candidate) => candidate.side === game.position.turn);
  perform(game, seat.player, move).catch((error) => {
    console.error(`chess: ${error.stack ?? error}`);
  });
}

Events.on('consoleCommand', (command, args) => {
  if (command !== 'chess') {
    return;
  }
  if (args[0] === 'near') {
    const player = kcds.players.Player.getById(Number(args[1]));
    if (!player) {
      console.info('chess: no such player');
      return;
    }
    placeFor(player, args[2]).then((text) => {
      console.info(`chess: before ${player.nickname}: ${text}`);
    });
    return;
  }
  if (args[0] === 'move') {
    consoleMove(args[1]);
    return;
  }
  const [x, y, z, yaw = 0] = args.map(Number);
  if (![x, y, z, yaw].every(Number.isFinite)) {
    console.info('usage: chess <x> <y> <z> [degrees]');
    return;
  }
  const game = build(new Vector3(x, y, z), yaw);
  console.info(
    game
      ? `chess: a table at ${x} ${y} ${z}`
      : 'chess: no table: another one stands too close, or there are too many',
  );
});
