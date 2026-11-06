'use strict';
// Prší, the card game, at any table (TabletopDeck): /prsi puts the cards on the table in front of
// the player, the level's own table or any other, and two players sit down on its benches or chairs
// with the game's E. With no table there, it stands a table of its own with two benches.
//
// The rules, with the 32 cards (7 to the ace in the four suits):
//   - four cards each, the first card of the stack face up on the table;
//   - a card follows the top one by suit or by rank; a queen goes on anything and the suit
//     changes to the one its player holds most of (or the one they named with /suit);
//   - a seven: the next player draws two, unless they answer with a seven (then the next draws
//     four, and so on); an ace: the next player stands, unless they answer with an ace;
//   - who cannot or will not play draws a card (E), and the turn passes;
//   - who plays their last card wins. When the stack runs out, the played cards but the top one
//     are shuffled into a new stack.
//
// The holder's keys are the deck's: A, D (or the arrows) point at a card, Enter plays it, E draws,
// Space stands when an ace is played on them, S lays the hand on the table, C looks closer.

const { Tabletop, Prop, SmartObject } = kcds.world;
const { Events, Chat } = kcds.core;
const { Vector3 } = kcds.math;

const RANKS = ['7', '8', '9', 'T', 'J', 'Q', 'K', 'A'];
const SUITS = { S: 'spades', H: 'hearts', D: 'diamonds', C: 'clubs' };
const RANK_NAMES = { 7: '7', 8: '8', 9: '9', T: '10', J: 'jack', Q: 'queen', K: 'king', A: 'ace' };
const HAND = 4;
const TABLE_MODEL = 'table_fancy_a_square_small';
const BENCH = 'Bench/bench_1place_low';
const TABLE_TOP = 0.75;
// Where the two players sit: this far from the table's middle, on the game's seat there.
const SEAT_DISTANCE = 0.68;
const LEVEL_SEAT_REACH = 1.3;
// A table's top in front of a player: looked for straight down at these distances ahead, this high
// above their feet (as the chess example finds its table).
const TABLE_SEARCH = [0.8, 1.1, 1.4];
const TABLE_LOWEST = 0.55;
const TABLE_HIGHEST = 1.2;
const MIN_TABLE_DISTANCE = 1.5;
const MAX_TABLES = 8;
// A thrown card lies on the table this long after its throw starts.
const THROW_MS = 1300;
const NEW_ROUND_SECONDS = 10;

// table -> game
const games = new Map();

function cardName(card) {
  return `${RANK_NAMES[card.rank]} of ${SUITS[card.suit]}`;
}

function seated(game) {
  return game.table.seats.map((seat) => seat.player).filter((player) => player);
}

function tell(game, text) {
  for (const player of seated(game)) {
    Chat.sendToPlayer(player, `Prší: ${text}`);
  }
}

// A point of the table, from its own x and y (metres), turned with it.
function place(game, x, y, z = 0) {
  const radians = (game.yaw * Math.PI) / 180;
  const cos = Math.cos(radians);
  const sin = Math.sin(radians);
  return new Vector3(game.at.x + x * cos - y * sin, game.at.y + x * sin + y * cos, game.at.z + z);
}

// A card table: `at` the floor under the table's middle, `yaw` its turn, `top` the height of a
// table's top that is there (null: the resource stands a table of its own), `benches` the game's
// benches at its two sides.
function build(at, yaw, top, benches) {
  if (games.size >= MAX_TABLES) {
    return null;
  }
  for (const other of games.values()) {
    if (Math.hypot(other.at.x - at.x, other.at.y - at.y) < MIN_TABLE_DISTANCE) {
      return null;
    }
  }
  const game = {
    at,
    yaw,
    top: top ?? TABLE_TOP,
    props: [],
    benches: [],
    deck: null,
    round: null,
    restart: null,
  };
  if (top === null) {
    game.props.push(Prop.spawn(TABLE_MODEL, at, new Vector3(0, 0, yaw)));
  }
  const seats = [];
  for (const sign of [1, -1]) {
    const facing = sign > 0 ? yaw + 180 : yaw;
    const half = (facing * Math.PI) / 360;
    if (benches) {
      game.benches.push(
        SmartObject.spawn({
          blueprint: BENCH,
          position: place(game, 0, sign * SEAT_DISTANCE),
          rotation: [0, 0, Math.sin(half), Math.cos(half)],
          name: 'prsi',
        }),
      );
    }
    seats.push({
      position: place(game, 0, sign * SEAT_DISTANCE),
      rotation: facing,
      chair: true,
      reach: benches ? undefined : LEVEL_SEAT_REACH,
      // No camera shot: on the game's bench they look from their own eyes, in first person.
    });
  }
  game.table = Tabletop.create({
    position: place(game, 0, 0, game.top),
    rotation: yaw,
    board: { rows: 1, columns: 1 },
    seats,
  });
  games.set(game.table, game);
  return game;
}

function destroy(game) {
  clearTimeout(game.restart);
  game.deck?.destroy();
  for (const prop of game.props) {
    prop.destroy();
  }
  for (const bench of game.benches) {
    bench.destroy();
  }
  games.delete(game.table);
  game.table.destroy();
}

// --- a round -----------------------------------------------------------------------------------

function newRound(game) {
  clearTimeout(game.restart);
  game.restart = null;
  game.deck?.destroy();
  game.deck = game.table.createDeck({
    ranks: RANKS,
    position: { x: -0.14, y: 0 },
    playPosition: { x: 0.06, y: 0 },
    actions: { draw: false, pass: false },
  });
  // The card on top (it counts from its throw on, while it still flies), the suit to follow, the
  // sevens to answer and an ace to answer.
  game.round = { top: null, suit: null, sevens: 0, ace: false, named: new Map(), over: false };
  game.deck.shuffle();
  game.deck.deal(seated(game), HAND);
  const [first] = game.deck.dealTo('table', 1, { faceUp: true });
  game.round.top = first;
  game.round.suit = first.suit;
  game.table.start();
  tell(game, `a new round, ${cardName(first)} on the table.`);
}

// Whether a card may go on the table now.
function allowed(game, card) {
  const { round } = game;
  const lying = round.top;
  if (round.sevens > 0) {
    return card.rank === '7';
  }
  if (round.ace) {
    return card.rank === 'A';
  }
  if (card.rank === 'Q') {
    return true;
  }
  return card.suit === round.suit || (lying !== null && card.rank === lying.rank);
}

// The suit a queen's player changes to: the one they named, else the one they hold most of.
function suitAfterQueen(game, player, queen) {
  const named = game.round.named.get(player.id);
  if (named) {
    return named;
  }
  const counts = {};
  for (const card of game.deck.hand(player)) {
    if (card !== queen) {
      counts[card.suit] = (counts[card.suit] ?? 0) + 1;
    }
  }
  const best = Object.entries(counts).sort((a, b) => b[1] - a[1])[0];
  return best ? best[0] : queen.suit;
}

// The keys of the player whose turn it is, and no draw or pass for the others.
function showKeys(game) {
  const { round, table, deck } = game;
  for (const player of seated(game)) {
    if (round.over || table.playerAt(table.turn) !== player) {
      deck.setActions({ draw: false, pass: false }, player);
    } else if (round.ace) {
      deck.setActions({ draw: false, pass: 'Stand' }, player);
    } else if (round.sevens > 0) {
      deck.setActions({ draw: `Draw ${round.sevens * 2}`, pass: false }, player);
    } else {
      deck.setActions({ draw: 'Draw a card', pass: false }, player);
    }
  }
}

function gameOf(deck) {
  for (const game of games.values()) {
    if (game.deck === deck) {
      return game;
    }
  }
  return null;
}

function myTurn(game, player) {
  return !game.round.over && game.table.playerAt(game.table.turn) === player;
}

// Enough cards in the stack to draw `count`: the played ones but the top go back first.
async function stock(game, count) {
  if (game.deck.remaining < count && game.deck.pile('table').length > 1) {
    await game.deck.restock('table');
    tell(game, 'the played cards are shuffled into a new stack.');
  }
}

// A player plays a card of their hand (their Enter, or the console's "prsi play"): true when it
// may go on the table, which the deck's throw then does.
function onPlay(deck, player, pieces) {
  const game = gameOf(deck);
  if (!game) {
    return;
  }
  const [card] = pieces;
  if (!myTurn(game, player) || pieces.length !== 1 || !allowed(game, card)) {
    Chat.sendToPlayer(
      player,
      `Prší: ${myTurn(game, player) ? 'that card does not go on the table now' : 'not your turn'}.`,
    );
    return false;
  }
  const { round } = game;
  const last = deck.hand(player).length === 1;
  round.top = card;
  round.ace = card.rank === 'A';
  round.sevens = card.rank === '7' ? round.sevens + 1 : 0;
  round.suit = card.rank === 'Q' ? suitAfterQueen(game, player, card) : card.suit;
  round.named.delete(player.id);
  let text = `${player.nickname} plays the ${cardName(card)}`;
  if (card.rank === 'Q') {
    text += `: ${SUITS[round.suit]} now`;
  }
  tell(game, `${text}.`);
  if (last) {
    round.over = true;
    showKeys(game);
    setTimeout(() => {
      if (games.has(game.table) && game.table.state === 'playing') {
        game.table.finish(player, 'the last card');
      }
    }, THROW_MS);
    return true;
  }
  game.table.nextTurn();
  return true;
}

// A player draws (their E, or "prsi draw"): the resource deals as many as the sevens say.
function onDraw(deck, player) {
  const game = gameOf(deck);
  if (!game || !myTurn(game, player) || game.round.ace) {
    return false;
  }
  const count = game.round.sevens > 0 ? game.round.sevens * 2 : 1;
  game.round.sevens = 0;
  stock(game, count).then(() => {
    if (game.deck === deck && deck.exists) {
      deck.deal(player, count);
      tell(game, `${player.nickname} draws ${count === 1 ? 'a card' : `${count} cards`}.`);
      game.table.nextTurn();
    }
  });
  // The resource deals them itself, as many as the sevens say.
  return false;
}

// A player stands after an ace (their Space, or "prsi stand").
function onPass(deck, player) {
  const game = gameOf(deck);
  if (game && myTurn(game, player) && game.round.ace) {
    game.round.ace = false;
    tell(game, `${player.nickname} stands.`);
    game.table.nextTurn();
  }
}

Events.on('deckPlayRequest', onPlay);
Events.on('deckDrawRequest', onDraw);
Events.on('deckPass', onPass);

Events.on('tabletopTurn', (table, seat, player) => {
  const game = games.get(table);
  if (game?.round) {
    showKeys(game);
    Chat.sendToPlayer(player, `Prší: your turn, ${SUITS[game.round.suit]} on the table.`);
  }
});

Events.on('tabletopSeated', (table, player) => {
  const game = games.get(table);
  if (!game) {
    return;
  }
  Chat.sendToPlayer(
    player,
    'Prší: A and D point at a card, Enter plays it, E draws; C shows your cards, again the table.',
  );
  if (table.state === 'idle' && seated(game).length === 2) {
    newRound(game);
  } else if (seated(game).length < 2) {
    Chat.sendToPlayer(player, 'Prší: waiting for a second player.');
  }
});

Events.on('tabletopEnd', (table, winner, reason) => {
  const game = games.get(table);
  if (!game) {
    return;
  }
  tell(game, winner ? `${winner.nickname} wins (${reason}).` : `no winner (${reason}).`);
  if (game.round) {
    game.round.over = true;
  }
  game.restart = setTimeout(() => {
    game.restart = null;
    if (games.has(table) && table.state === 'idle' && seated(game).length === 2) {
      newRound(game);
    }
  }, NEW_ROUND_SECONDS * 1000);
});

// --- placing a table ---------------------------------------------------------------------------

// The top of a table in front of the player, or null: their game traces rays straight down at a
// few distances ahead, and a surface 0.55 to 1.2 m above their feet is a table's top.
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

// /prsi: on the table in front of the player, sitting on its own benches or chairs; "benches":
// the game's benches at its sides too; with no table there (or "table"), a table of its own.
async function placeFor(player, mode) {
  const found = mode === 'table' ? null : await tableBefore(player);
  if (found) {
    const benches = mode === 'benches';
    const game = build(found.at, found.yaw, found.top, benches);
    if (!game) {
      return 'another table with cards is too close, or there are too many.';
    }
    return benches
      ? 'the cards are on the table. Sit down on a bench (E).'
      : 'the cards are on the table. Sit down at its sides (E).';
  }
  const heading = player.rotation.rotateVector(new Vector3(0, 1, 0));
  const at = new Vector3(
    player.position.x + heading.x * 2,
    player.position.y + heading.y * 2,
    player.position.z,
  );
  const yaw = Math.round((Math.atan2(-heading.x, heading.y) * 180) / Math.PI);
  return build(at, yaw, null, true)
    ? 'a table. Sit down on a bench (E).'
    : 'no room for a table here.';
}

function nearest(player) {
  let best = null;
  let bestDistance = 5;
  for (const game of games.values()) {
    const away = Math.hypot(player.position.x - game.at.x, player.position.y - game.at.y);
    if (away < bestDistance) {
      best = game;
      bestDistance = away;
    }
  }
  return best;
}

Events.on('playerCommand', (player, command, args) => {
  if (command === 'prsi') {
    if (args[0] === 'remove') {
      const game = nearest(player);
      if (!game || game.table.state !== 'idle') {
        Chat.sendToPlayer(player, 'Prší: no free table near you.');
        return;
      }
      destroy(game);
      Chat.sendToPlayer(player, 'Prší: the table is gone.');
      return;
    }
    placeFor(player, args[0]).then((text) => Chat.sendToPlayer(player, `Prší: ${text}`));
    return;
  }
  if (command === 'suit') {
    // /suit h|d|s|c: the suit your next queen changes to.
    const suit = String(args[0] ?? '').toUpperCase();
    for (const game of games.values()) {
      if (game.table.seatOf(player) >= 0 && game.round && SUITS[suit]) {
        game.round.named.set(player.id, suit);
        Chat.sendToPlayer(player, `Prší: your next queen changes to ${SUITS[suit]}.`);
        return;
      }
    }
    Chat.sendToPlayer(player, 'Prší: /suit h, d, s or c at a Prší table.');
  }
});

// The server console: "prsi near <player id> [benches|table]" as that player's /prsi, and
// "prsi sit <player id>" seats them at the nearest table's first free seat.
// A player's moves by their id, as their keys make them:
//   prsi hand <id>                   their cards (by index) and the card on the table
//   prsi play <id> <index>           they play that card of their hand
//   prsi draw <id>, prsi stand <id>  they draw, or stand after an ace
Events.on('consoleCommand', (command, args) => {
  if (command !== 'prsi') {
    return;
  }
  const player = kcds.players.Player.getById(Number(args[1]));
  if (!player) {
    console.info('prsi: no such player');
    return;
  }
  if (args[0] === 'near') {
    placeFor(player, args[2]).then((text) =>
      console.info(`prsi: before ${player.nickname}: ${text}`),
    );
    return;
  }
  if (args[0] === 'sit') {
    const game = nearest(player);
    const seat = game ? game.table.seats.findIndex((one) => one.player === null) : -1;
    const sat = seat >= 0 && game.table.sit(player, seat);
    console.info(`prsi: ${player.nickname} ${sat ? `sits on seat ${seat}` : 'found no free seat'}`);
    return;
  }
  const game = [...games.values()].find((one) => one.table.seatOf(player) >= 0 && one.deck);
  if (!game) {
    console.info(`prsi: ${player.nickname} plays at no table`);
    return;
  }
  const { deck } = game;
  const hand = deck.hand(player);
  if (args[0] === 'hand') {
    const cards = hand.map((card, index) => `${index}:${cardName(card)}`).join(', ');
    const top = game.round.top ? cardName(game.round.top) : 'nothing';
    console.info(
      `prsi: ${player.nickname} holds ${cards}; ${top} on the table, ${SUITS[game.round.suit]}`,
    );
  } else if (args[0] === 'play') {
    const card = hand[Number(args[2])];
    if (card && onPlay(deck, player, [card])) {
      deck.play(player, [card]);
    }
  } else if (args[0] === 'draw') {
    onDraw(deck, player);
  } else if (args[0] === 'stand') {
    onPass(deck, player);
  }
});
