'use strict';

// Dice matches between two players: Dice and the dice events. The server deals every
// throw and checks every move by the game's scoring (scoring.cjs); dice perks are off. tables.json
// is built from a game installation by Dev/tools/dice/build_tables.py, kinds.json (the game's
// dice: each side's weight and the die's mesh) by Dev/tools/dice/build_kinds.py, badges.json (the
// game's badges) by Dev/tools/dice/build_badges.py. A player may throw six dice of the game's
// kinds (Dice.start's firstDice and secondDice); the server deals each by its weights and both
// games show it with its mesh. A player may play with a badge of the game's (Dice.start's
// firstBadge and secondBadge, badges.cjs); both games show it and play it by the game's rules.
//
// A match: the first thrower is drawn, then the players take turns. A turn starts with the six
// dice thrown at once (as the game does); the player then either keeps scoring dice and throws the
// rest (continue) or keeps scoring dice and banks the turn (pass). A throw with no scoring die is
// a bust: the turn's points are lost and the other player throws. When every die of a turn has
// scored, all six are thrown again. A banked score reaching the target wins. A match ends too when
// a player leaves the table (gaveUp), disconnects (left), does not move for three minutes on their
// turn (timedOut), their client cannot play it (failed) or a script stops it (stopped).
//
// Each player's client plays the match at the game's own dice table against the other player's
// body (op diceState: start, roll, the other's move, end) and sends the player's moves
// (clientMove); a refused or repeated move raises nothing. A throw is dealt when the thrower's
// game throws (clientMove 'throw'), so both games show it at once; a game that does not ask in
// THROW_WAIT_MS gets it anyway. The dice the thrower selects go to the other player's game as
// they are selected ('select'), so they can be watched being chosen.
//
// Against an NPC: Dice.startAgainstNpc plays a match between a player and the server,
// which plays the other seat with dice_ai.cjs at the difficulty asked for: its throws are dealt
// AI_THROW_MS into its turn, its moves made AI_MOVE_MS after its throw, and sent to the player's
// game as the other player's are. The other seat's body is the NPC the script names (an Npc), or
// one the server stands at the table for the match (a mannequin of the game's, gone after it).
//
// Chairs: Dice.sit sits a player down on a free chair of the table they stand at, the
// game's own walk and sit-down (diceState 'sit'), to wait there for a match. The chair is theirs
// while they sit on it (their game reports them seated) or play; a match between two players on the
// chairs of one table plays each from their own chair.

const crypto = require('node:crypto');

const catalog = require('./tables.json');
const kindRows = require('./kinds.json');
const { score, canScore } = require('./scoring.cjs');
const badges = require('./badges.cjs');
const ai = require('./dice_ai.cjs');

const DEFAULT_TARGET = 2000;
const MAX_TARGET = 100000;
const MAX_NAME_BYTES = 64;
// How near a table and each other both players stand to start a match, in metres.
const TABLE_REACH = 3;
const PAIR_REACH = 4;
// How near a table a player not playing stands to watch the match there, in metres.
const WATCH_REACH = 20;
// A spectator's view of the dice (Dice.spectate): how far above them, how wide, how long a move
// to new dice takes, and how far the dice must be from the view's aim to move it, in metres.
const SPECTATE_HEIGHT = 0.4;
const SPECTATE_FOV = 30;
const SPECTATE_MOVE_SECONDS = 0.4;
const SPECTATE_AIM_REACH = 0.05;
// How far from the table a die can be and still be on it, in metres (dice waiting in the cup
// are at the world's origin), and the height of the table's top above its position.
const SPECTATE_TABLE_REACH = 2;
const SPECTATE_TABLE_TOP = 0.75;
// How long the player to throw may wait before the match ends, in milliseconds.
const TURN_TIMEOUT_MS = 3 * 60 * 1000;
// How long a throw waits for the thrower's game to throw, in milliseconds.
const THROW_WAIT_MS = 30 * 1000;
// When the dice of a throw have landed (diceSettled): the thrower's game sends the
// table's things only while they move, so the dice lie still once no view came for this long
// after the throw; at the latest this long after it (a game that sends no views).
const SETTLE_QUIET_MS = 500;
const SETTLE_LATEST_MS = 4000;
// The NPC opponent: how long it waits into its turn before it throws, and after its
// throw before it moves, in milliseconds; the stand-in's soul and how far from the table it stands.
const AI_THROW_MS = 1500;
// Its first throw of a match waits for the player's sit-down and the minigame's start.
const AI_FIRST_THROW_MS = 8000;
const AI_MOVE_MS = 3000;
const STAND_IN_SOUL = 'townsman';
const STAND_IN_REACH = 1.2;
const DEFAULT_NPC_NAME = 'Opponent';
// How long a player sent to a chair has to walk to it and sit down before it is free again.
const SIT_DOWN_MS = 10 * 1000;
const ALL_DICE = 0b111111;
// The seventh die a Might badge adds to a turn, and all seven.
const EXTRA_DIE = 6;
const WITH_EXTRA_DIE = 0b1111111;
// A Resurrection badge after a bust: the turn banked, the dice thrown again, or the bust taken.
const BUST_CHOICES = Object.freeze(['bank', 'rethrow', 'bust']);
// The side value of a joker die's joker side (kinds.json).
const JOKER_SIDE = 6;
const NOBODY = 255;
const GLOBAL_WORLD = 0xffffffff;
const TABLES = Object.freeze(
  catalog.map((table) =>
    Object.freeze({
      id: table.id,
      behavior: table.behavior,
      level: table.level,
      position: Object.freeze({ ...table.position }),
      seats: Object.freeze([...table.seats]),
      // Where the two chairs stand, in seat order.
      chairPositions: Object.freeze(table.chairs.map((chair) => Object.freeze({ ...chair }))),
      // One of the level's own, not one a script placed.
      placed: false,
    }),
  ),
);

function distance(a, b) {
  return Math.hypot(a.x - b.x, a.y - b.y, a.z - b.z);
}

function checkName(name, which) {
  if (name === undefined) {
    return '';
  }
  if (
    typeof name !== 'string' ||
    Buffer.byteLength(name, 'utf8') > MAX_NAME_BYTES ||
    /[\x00-\x1f\x7f]/.test(name)
  ) {
    throw new TypeError(`Dice: ${which} is a name of at most ${MAX_NAME_BYTES} bytes`);
  }
  return name;
}

// The game's dice kinds, by id and by name (lower case).
const KINDS = Object.freeze(
  kindRows.map((row) =>
    Object.freeze({
      ...row,
      sides: Object.freeze([...row.sides]),
      weights: Object.freeze([...row.weights]),
    }),
  ),
);
const KIND_BY_KEY = new Map(
  KINDS.flatMap((kind) => [
    [kind.id, kind],
    [kind.name.toLowerCase(), kind],
  ]),
);

// A thrown die's face (1 to 6, or 7 the joker side of the game's joker dice): a die of a kind
// lands on each side as often as its weight says; the game's plain die on any side alike.
function dieFace(random, kind) {
  if (!kind) {
    return 1 + random(6);
  }
  const total = kind.weights.reduce((sum, weight) => sum + weight, 0);
  let left = random(total);
  for (let side = 0; side < kind.weights.length; side++) {
    left -= kind.weights[side];
    if (left < 0) {
      return kind.sides[side] + 1;
    }
  }
  return kind.sides[kind.sides.length - 1] + 1;
}

// Six dice a player throws, by kind id or name; undefined for the game's plain dice.
function checkDice(value, option) {
  if (value === undefined) {
    return null;
  }
  if (!Array.isArray(value) || value.length !== 6) {
    throw new TypeError(`Dice.start: ${option} is six dice kinds (a name or an id each)`);
  }
  return value.map((key) => {
    const kind = typeof key === 'string' ? KIND_BY_KEY.get(key.toLowerCase()) : undefined;
    if (!kind) {
      throw new TypeError(`Dice.start: ${option}: ${String(key)} is not a dice kind`);
    }
    return kind;
  });
}

// The faces events tell: the six dice's, the seventh's too while a Might badge's die is thrown.
function eventFaces(match) {
  const count = match.allDice === WITH_EXTRA_DIE ? 7 : 6;
  return Object.freeze(match.faces.slice(0, count));
}

// The side values of six dice ([] for the game's plain dice).
function sidesOf(dice) {
  return dice ? dice.map((kind) => [...kind.sides]) : [];
}

// The meshes six dice show with ([] for the game's plain dice).
function meshes(dice) {
  return dice ? dice.map((kind) => kind.model) : [];
}

// Where the game's sit-down on a chair starts, as it was measured live: the clip puts the
// body there in one frame wherever it stands, then walks it onto the chair. To the side of the chair
// and behind it, as seen from the chair looking at the table, and turned from that look.
const SIT_START_SIDE = 1.093;
const SIT_START_BACK = 0.673;
const SIT_START_TURN = (44.9 * Math.PI) / 180;
const LOOK_AHEAD = 2;

function yawTo(from, to) {
  return Math.atan2(-(to.x - from.x), to.y - from.y);
}

// Where a body goes to sit down on a chair without a jump, and which way it faces there (degrees).
function sitStart(table, chair) {
  const look = yawTo(chair, table.position);
  const ahead = { x: -Math.sin(look), y: Math.cos(look) };
  const side = { x: Math.cos(look), y: Math.sin(look) };
  const position = {
    x: chair.x + side.x * SIT_START_SIDE - ahead.x * SIT_START_BACK,
    y: chair.y + side.y * SIT_START_SIDE - ahead.y * SIT_START_BACK,
    z: chair.z,
  };
  const yaw = look + SIT_START_TURN;
  return {
    position,
    yaw: (yaw * 180) / Math.PI,
    // A point straight ahead from there, for Npc.lookAt.
    lookAt: {
      x: position.x - Math.sin(yaw) * LOOK_AHEAD,
      y: position.y + Math.cos(yaw) * LOOK_AHEAD,
      z: position.z,
    },
  };
}

// Where a match's stand-in NPC goes: where the sit-down on the table's chair starts, or across the
// table from the player at a table a script placed, which has no chairs we know.
function standInPlace(table, seat, playerPosition) {
  const chair = table.chairPositions?.[seat];
  if (chair) {
    return sitStart(table, chair);
  }
  const across = {
    x: table.position.x - playerPosition.x,
    y: table.position.y - playerPosition.y,
  };
  const length = Math.hypot(across.x, across.y) || 1;
  const position = {
    x: table.position.x + (across.x / length) * STAND_IN_REACH,
    y: table.position.y + (across.y / length) * STAND_IN_REACH,
    z: table.position.z,
  };
  const yaw = (yawTo(position, table.position) * 180) / Math.PI;
  return { position: { x: position.x, y: position.y, z: position.z }, yaw };
}

class DiceMatches {
  // host: players, emit; random(n): a whole number 0 to n - 1.
  constructor(host, random = (n) => crypto.randomInt(n), now = () => Date.now()) {
    this.host = host;
    this.random = random;
    this.now = now;
    // match id -> match
    this.matches = new Map();
    // player id -> match id
    this.byPlayer = new Map();
    // Player id -> the match they spectate (Dice.spectate).
    this.spectating = new Map();
    this.nextId = 1;
    // player id -> whether their game was last told that table uses are answered
    this.tablesTold = new Map();
    // player id -> the chair they hold: { table, seat, at }
    this.chairs = new Map();
  }

  // Whether some resource answers a dice table's use (diceTableUse): each ready player's game is
  // told when that changes and when it enters the world, and offers the game's "Play dice" only
  // while it is so. Called every tick with the host's answer.
  pumpTables(answered) {
    for (const player of this.host.players.values()) {
      if (!player.ready) {
        this.tablesTold.delete(player.id);
        continue;
      }
      if (this.tablesTold.get(player.id) === answered) {
        continue;
      }
      this.tablesTold.set(player.id, answered);
      this.host.bridge?.send({ op: 'diceTables', id: player.id, offered: answered });
    }
    for (const id of this.tablesTold.keys()) {
      if (!this.host.players.has(id)) {
        this.tablesTold.delete(id);
      }
    }
  }

  // The level's tables and the ones scripts placed (SmartObject.spawn).
  allTables() {
    const placed = this.host.smartObjects?.placedStations('dice') ?? [];
    // The catalog holds every level's tables, and two levels share coordinates.
    const level = this.host.statusRoutes?.settings?.map;
    return [
      ...TABLES.filter((table) => !level || !table.level || table.level === level),
      ...placed.map((table) =>
        Object.freeze({
          id: table.id,
          behavior: table.behavior,
          position: Object.freeze({ ...table.position }),
          seats: Object.freeze([...table.seats]),
          chairPositions: Object.freeze([]),
          placed: true,
          dimension: table.dimension,
          smartObject: table.smartObject,
        }),
      ),
    ];
  }

  // Whether a player of `dimension` can use a table: a level table a level edit took out of
  // that world, or a placed one of another world, is none.
  usable(table, dimension) {
    if (table.placed) {
      return this.host.smartObjects.seesPlaced(dimension, table);
    }
    return !this.host.smartObjects?.removedIn(table.id, dimension);
  }

  // The dice tables; one a level edit took out of every world is listed nowhere.
  tables() {
    return this.allTables()
      .filter((table) => table.placed || this.usable(table, GLOBAL_WORLD))
      .map(({ chairPositions, ...table }) => {
        const sitters = this.sittersAt(table.id);
        return {
          ...table,
          chairs: chairPositions.map((position, seat) => ({
            position: { ...position },
            player: sitters[seat],
            // Where a body starts the game's sit-down on it, to walk there first.
            sitFrom: sitStart(table, position),
          })),
        };
      });
  }

  // The nearest table within TABLE_REACH of a position, or null; a table a level edit took out of
  // the dimension counts as none (SmartObject.remove).
  tableNear(position, dimension) {
    let nearest = null;
    for (const table of this.allTables()) {
      if (!this.usable(table, dimension)) {
        continue;
      }
      const reach = distance(position, table.position);
      if (reach <= TABLE_REACH && (!nearest || reach < nearest.reach)) {
        nearest = { table, reach };
      }
    }
    return nearest ? nearest.table : null;
  }

  // The players holding a table's two chairs ([seat 0, seat 1], null for a free chair).
  sittersAt(tableId) {
    this.freeChairs();
    const sitters = [null, null];
    for (const [id, chair] of this.chairs) {
      if (chair.table.id === tableId) {
        sitters[chair.seat] = id;
      }
    }
    return sitters;
  }

  // A chair is free again when its player is gone, or neither plays nor sits SIT_DOWN_MS after
  // being sent to it.
  freeChairs() {
    const now = this.now();
    for (const [id, chair] of [...this.chairs]) {
      const player = this.host.players.get(id);
      if (!player || !player.ready) {
        this.chairs.delete(id);
        continue;
      }
      if (this.byPlayer.has(id)) {
        chair.at = now;
        continue;
      }
      if (!player.seated && now - chair.at >= SIT_DOWN_MS) {
        this.chairs.delete(id);
      }
    }
  }

  // Sits the player down on a free chair of the table they stand at to wait for a match: their
  // game walks them to it and sits them down. The chair (0 or 1), the one they hold already, or
  // -1 when they play, stand at no table or both its chairs are taken.
  sit(playerId) {
    const player = this.host.players.get(playerId);
    if (!player || !player.ready || !player.position || this.byPlayer.has(playerId)) {
      return -1;
    }
    this.freeChairs();
    const held = this.chairs.get(playerId);
    if (held) {
      return held.seat;
    }
    const table = this.tableNear(player.position, player.dimension);
    if (!table) {
      return -1;
    }
    const sitters = this.sittersAt(table.id);
    const seat = sitters.indexOf(null);
    if (seat < 0) {
      return -1;
    }
    this.chairs.set(playerId, { table, seat, at: this.now() });
    this.host.bridge?.send({
      op: 'diceState',
      id: playerId,
      match: 0,
      kind: 'sit',
      behavior: table.behavior,
      seat,
    });
    return seat;
  }

  // The table both players stand at, or null.
  tableOf(first, second) {
    const a = first.position;
    const b = second.position;
    if (!a || !b || distance(a, b) > PAIR_REACH) {
      return null;
    }
    let nearest = null;
    for (const table of this.allTables()) {
      if (table.placed && !this.usable(table, first.dimension)) {
        continue;
      }
      const reach = Math.max(distance(a, table.position), distance(b, table.position));
      if (reach <= TABLE_REACH && (!nearest || reach < nearest.reach)) {
        nearest = { table, reach };
      }
    }
    return nearest ? nearest.table : null;
  }

  // Scripts may change a match's options just before it begins (diceMatchStarting): the options
  // the match is made with. The second player is null against the server's NPC.
  matchStarting(first, second, table, options) {
    const settings = { ...options };
    this.host.emit('diceMatchStarting', first, second, table, settings);
    return settings;
  }

  start(firstId, secondId, options = {}) {
    const first = this.host.players.get(firstId);
    const second = this.host.players.get(secondId);
    if (!first || !second || !first.ready || !second.ready) {
      throw new Error('Dice.start: both players must be connected and in the world');
    }
    if (firstId === secondId) {
      throw new Error('Dice.start: a player cannot play against themselves');
    }
    if (this.byPlayer.has(firstId) || this.byPlayer.has(secondId)) {
      throw new Error('Dice.start: a player is already playing');
    }
    if (!options || typeof options !== 'object') {
      throw new TypeError('Dice.start: options is an object');
    }
    this.freeChairs();
    const firstChair = this.chairs.get(firstId);
    const secondChair = this.chairs.get(secondId);
    const onChairs = firstChair && secondChair && firstChair.table === secondChair.table;
    const table = onChairs ? firstChair.table : this.tableOf(first, second);
    if (!table) {
      throw new Error('Dice.start: both players must stand together at a dice table');
    }
    options = this.matchStarting(first, second, table, options);
    const target = options.targetScore ?? DEFAULT_TARGET;
    if (!Number.isInteger(target) || target < 1 || target > MAX_TARGET) {
      throw new TypeError(`Dice.start: targetScore is a whole number 1 to ${MAX_TARGET}`);
    }
    const names = [
      checkName(options.firstName, 'firstName'),
      checkName(options.secondName, 'secondName'),
    ];
    const dice = [
      checkDice(options.firstDice, 'firstDice'),
      checkDice(options.secondDice, 'secondDice'),
    ];
    const chosen = [
      badges.checkBadge(options.firstBadge, 'firstBadge'),
      badges.checkBadge(options.secondBadge, 'secondBadge'),
    ];
    // The badge each seat plays with, after the other's Defence badge.
    const inEffect = [badges.inEffect(chosen[0], chosen[1]), badges.inEffect(chosen[1], chosen[0])];
    // Players on the table's chairs play each from their own: the seats follow the chairs.
    const players = [firstId, secondId];
    if (onChairs && firstChair.seat === 1) {
      players.reverse();
      names.reverse();
      dice.reverse();
      chosen.reverse();
      inEffect.reverse();
    }
    const match = {
      id: this.nextId++,
      table,
      players,
      names,
      // Each seat's six dice kinds, null for the game's plain dice.
      dice,
      // Each seat's badge as chosen and as it plays (null for none), and the scoring rules it
      // gives.
      badges: chosen,
      inEffect,
      rules: inEffect.map((badge) => badges.rulesOf(badge)),
      // How many more times each seat may use its badge.
      uses: inEffect.map((badge) => badges.usesOf(badge)),
      target,
      scores: inEffect.map((badge) => badges.headStart(badge, target)),
      seat: this.random(2),
      turn: 0,
      roll: 0,
      moveIndex: 0,
      turnScore: 0,
      // The seven dice's faces (the seventh 0 without a Might badge's die this turn).
      faces: [0, 0, 0, 0, 0, 0, 0],
      // Dice still to throw this turn, and the dice the last throw threw.
      available: ALL_DICE,
      rolledMask: 0,
      // The thrower's dice this turn (six, seven with a Might badge's die).
      allDice: ALL_DICE,
      // Whether a bust waits for the thrower's Resurrection badge, and since when.
      bustWaiting: false,
      bustSince: 0,
      lastMoveAt: this.now(),
      over: false,
      // Players not playing who were shown the table's things (their game puts them back at the
      // end).
      watchers: new Set(),
      spectators: new Map(),
    };
    this.matches.set(match.id, match);
    this.byPlayer.set(firstId, match.id);
    this.byPlayer.set(secondId, match.id);
    const [seatFirst, seatSecond] = players.map((id) => this.host.players.get(id));
    // Players in a match cannot be fought: the NPCs fighting them stop or turn away.
    for (const player of [seatFirst, seatSecond]) {
      this.host.npcs?.targetDown(player);
    }
    this.host.emit('diceMatchStart', match.id, seatFirst, seatSecond, target);
    for (const seat of [0, 1]) {
      this.send(match, seat, {
        kind: 'start',
        opponent: match.players[1 - seat],
        behavior: table.behavior,
        seat,
        first: seat === match.seat,
        target,
        ownName: match.names[seat],
        opponentName: match.names[1 - seat],
        ownDice: meshes(match.dice[seat]),
        opponentDice: meshes(match.dice[1 - seat]),
        ownSides: sidesOf(match.dice[seat]),
        opponentSides: sidesOf(match.dice[1 - seat]),
        ownBadge: badges.wireOf(match.badges[seat]),
        opponentBadge: badges.wireOf(match.badges[1 - seat]),
      });
    }
    this.beginTurn(match);
    return match.id;
  }

  // Dice.startAgainstNpc: a match between a player and the server's NPC opponent at
  // the table the player stands at (or sits at). The match id.
  startAgainstNpc(playerId, options = {}) {
    const player = this.host.players.get(playerId);
    if (!player || !player.ready) {
      throw new Error('Dice.startAgainstNpc: the player must be connected and in the world');
    }
    if (this.byPlayer.has(playerId)) {
      throw new Error('Dice.startAgainstNpc: the player is already playing');
    }
    if (!options || typeof options !== 'object') {
      throw new TypeError('Dice.startAgainstNpc: options is an object');
    }
    this.freeChairs();
    const chair = this.chairs.get(playerId);
    const table = chair?.table ?? this.tableNear(player.position, player.dimension);
    if (!table) {
      throw new Error('Dice.startAgainstNpc: the player must stand at a dice table');
    }
    options = this.matchStarting(player, null, table, options);
    const target = options.targetScore ?? DEFAULT_TARGET;
    if (!Number.isInteger(target) || target < 1 || target > MAX_TARGET) {
      throw new TypeError(`Dice.startAgainstNpc: targetScore is a whole number 1 to ${MAX_TARGET}`);
    }
    const skill = ai.skillOf(options.difficulty);
    const given = options.npc;
    const npcId = given === undefined || given === null ? 0 : given.id ?? given;
    if (npcId !== 0 && (!Number.isInteger(npcId) || !this.host.npcs?.npcs?.has(npcId))) {
      throw new TypeError('Dice.startAgainstNpc: npc is an Npc');
    }
    const npcName =
      options.npcName === undefined
        ? (npcId !== 0 ? this.host.npcs.npcs.get(npcId)?.name : '') || DEFAULT_NPC_NAME
        : checkName(options.npcName, 'npcName');
    const playerName = checkName(options.playerName, 'playerName');
    const dice = [
      checkDice(options.playerDice, 'playerDice'),
      checkDice(options.npcDice, 'npcDice'),
    ];
    const chosen = [
      badges.checkBadge(options.playerBadge, 'playerBadge'),
      badges.checkBadge(options.npcBadge, 'npcBadge'),
    ];
    const inEffect = [badges.inEffect(chosen[0], chosen[1]), badges.inEffect(chosen[1], chosen[0])];
    // The player plays from their chair: the NPC has the other seat.
    const own = chair && chair.table === table ? chair.seat : 0;
    // The NPC's body: the script's, or a stand-in on the table's other chair.
    let body = npcId;
    let standIn = null;
    // An order of the script's would hold the body off its chair.
    if (body !== 0) {
      this.host.npcs.npcs.get(body)?.hold?.();
    }
    if (body === 0) {
      const place = standInPlace(table, 1 - own, player.position);
      const { Npc } = this.host.api ?? this.host.makeAPI();
      standIn = Npc.create({
        soul: STAND_IN_SOUL,
        name: npcName,
        position: place.position,
        rotation: { x: 0, y: 0, z: place.yaw },
        dimension: player.dimension,
        interactable: false,
        targetable: false,
        invulnerable: true,
      });
      // The player's game runs it from its first body: a body that changed when its simulator came
      // would leave the running minigame with the old one, and the client stopped there.
      standIn.pin(player);
      body = standIn.id;
    }
    const seats = (pair) => (own === 1 ? [pair[1], pair[0]] : pair);
    const names = seats([playerName, npcName]);
    const match = {
      id: this.nextId++,
      table,
      players: seats([playerId, body]),
      names,
      dice: seats(dice),
      badges: seats(chosen),
      inEffect: seats(inEffect),
      rules: seats(inEffect).map((badge) => badges.rulesOf(badge)),
      uses: seats(inEffect).map((badge) => badges.usesOf(badge)),
      target,
      scores: seats(inEffect).map((badge) => badges.headStart(badge, target)),
      seat: this.random(2),
      turn: 0,
      roll: 0,
      moveIndex: 0,
      turnScore: 0,
      faces: [0, 0, 0, 0, 0, 0, 0],
      available: ALL_DICE,
      rolledMask: 0,
      allDice: ALL_DICE,
      bustWaiting: false,
      bustSince: 0,
      lastMoveAt: this.now(),
      thrownAt: 0,
      over: false,
      watchers: new Set(),
      spectators: new Map(),
      // The seat the server plays, its skill and the mannequin it stood there (null for the
      // script's NPC).
      ai: { seat: 1 - own, skill, standIn },
    };
    this.matches.set(match.id, match);
    this.byPlayer.set(playerId, match.id);
    this.host.npcs?.targetDown(player);
    const [seatFirst, seatSecond] = match.players.map((id) => this.host.players.get(id) ?? null);
    this.host.emit('diceMatchStart', match.id, seatFirst, seatSecond, target);
    this.send(match, own, {
      kind: 'start',
      opponent: body,
      behavior: table.behavior,
      seat: own,
      first: own === match.seat,
      target,
      ownName: match.names[own],
      opponentName: match.names[1 - own],
      ownDice: meshes(match.dice[own]),
      opponentDice: meshes(match.dice[1 - own]),
      ownSides: sidesOf(match.dice[own]),
      opponentSides: sidesOf(match.dice[1 - own]),
      ownBadge: badges.wireOf(match.badges[own]),
      opponentBadge: badges.wireOf(match.badges[1 - own]),
    });
    this.beginTurn(match);
    return match.id;
  }

  // The server's turn in a match against its NPC: it throws, then moves, each after a
  // moment, so the player's game shows it as another player's.
  playAi(match, now) {
    const seat = match.ai.seat;
    if (match.over || match.seat !== seat) {
      return;
    }
    if (match.awaitingThrow) {
      const wait = match.turn === 1 ? AI_FIRST_THROW_MS : AI_THROW_MS;
      if (now - match.throwAskedAt >= wait) {
        this.deal(match);
      }
      return;
    }
    if (match.bustWaiting) {
      // It does not use a Resurrection badge: the bust stands.
      this.send(match, 1 - seat, { kind: 'move', move: 'bustChoice', held: 2 });
      this.bust(match);
      return;
    }
    if (match.roll === 0 || now - match.thrownAt < AI_MOVE_MS) {
      return;
    }
    const move = ai.choose(
      {
        faces: match.faces,
        rolledMask: match.rolledMask,
        available: match.available,
        allDice: match.allDice,
        rules: match.rules[seat],
        turnScore: match.turnScore,
        own: match.scores[seat],
        other: match.scores[1 - seat],
        target: match.target,
      },
      match.ai.skill,
      () => this.random(1000) / 1000,
    );
    if (!move || !this.play(match, move.kind, move.held)) {
      // Nothing to keep (a bust was already taken) or a move the rules refused: it banks nothing.
      this.bust(match);
    }
  }

  // Tells a seat's client about its match (op diceState); the server's own seat has no client.
  send(match, seat, fields) {
    if (match.ai && seat === match.ai.seat) {
      return;
    }
    this.host.bridge?.send({
      op: 'diceState',
      id: match.players[seat],
      match: match.id,
      ...fields,
    });
  }

  event(match, fields = {}) {
    return Object.freeze({
      match: match.id,
      player: match.players[match.seat],
      seat: match.seat,
      turn: match.turn,
      roll: match.roll,
      moveIndex: match.moveIndex,
      turnScore: match.turnScore,
      totalScore: match.scores[match.seat],
      faces: Object.freeze([]),
      rolledMask: 0,
      heldMask: 0,
      points: 0,
      reason: '',
      ...fields,
    });
  }

  beginTurn(match) {
    match.turn += 1;
    match.roll = 0;
    match.turnScore = 0;
    match.allDice = ALL_DICE;
    match.available = ALL_DICE;
    match.rolledMask = 0;
    match.faces[EXTRA_DIE] = 0;
    match.bustWaiting = false;
    match.lastMoveAt = this.now();
    this.host.emit('diceTurnStart', this.event(match));
    this.awaitThrow(match);
  }

  // The next throw waits for the thrower's game to throw.
  awaitThrow(match) {
    match.awaitingThrow = true;
    match.throwAskedAt = this.now();
  }

  // Throws the dice still to throw; a bust ends the turn (or waits for the thrower's Resurrection
  // badge).
  deal(match) {
    match.awaitingThrow = false;
    match.roll += 1;
    match.thrownAt = this.now();
    this.throwDice(match, match.available, 'rolled');
  }

  // Throws these dice (a mask of the seven) for the thrower, tells both games, then checks the
  // dice on the table for a bust. reason: 'rolled', or the badge that threw them again.
  throwDice(match, mask, reason) {
    for (let die = 0; die < 7; die++) {
      if (mask & (1 << die)) {
        match.faces[die] = dieFace(this.random, match.dice[match.seat]?.[die]);
      }
    }
    if (reason === 'rolled') {
      match.rolledMask = mask;
    }
    const rolled = match.faces.filter((face, die) => match.rolledMask & (1 << die));
    const bust = !canScore(rolled, match.rules[match.seat]);
    const rollEvent = this.event(match, {
      faces: eventFaces(match),
      rolledMask: mask,
      reason: bust ? 'bust' : reason,
    });
    this.host.emit('diceRoll', rollEvent);
    // The same throw again once its dice lie on the table.
    const thrown = this.now();
    match.settling = { since: thrown, viewAt: thrown, event: rollEvent };
    for (const seat of [0, 1]) {
      this.send(match, seat, {
        kind: 'roll',
        mine: seat === match.seat,
        faces: [...match.faces],
        rolled: mask,
        bust,
      });
    }
    if (!bust) {
      return;
    }
    const badge = match.inEffect[match.seat];
    if (badge?.effect === 'antiBust' && match.uses[match.seat] > 0) {
      match.bustWaiting = true;
      match.bustSince = this.now();
      return;
    }
    this.bust(match);
  }

  bust(match) {
    match.bustWaiting = false;
    match.turnScore = 0;
    this.host.emit('diceTurnEnd', this.event(match, { reason: 'bust' }));
    this.nextTurn(match);
  }

  nextTurn(match) {
    if (match.over) {
      return;
    }
    match.seat = 1 - match.seat;
    this.beginTurn(match);
  }

  // A player's move: 'continue' or 'pass' with the dice kept (a mask of the last throw's dice),
  // or 'badgePass', a pass with the player's Doppelganger or Warlord badge. Whether it was
  // accepted.
  move(playerId, kind, heldMask = 0) {
    const match = this.throwerMatch(playerId);
    return match ? this.play(match, kind, heldMask) : false;
  }

  // The thrower's move in their match (a player's, or the server's own for its NPC).
  play(match, kind, heldMask) {
    if (!['continue', 'pass', 'badgePass'].includes(kind)) {
      return false;
    }
    if (!Number.isInteger(heldMask) || heldMask <= 0 || (heldMask & ~match.rolledMask) !== 0) {
      return false;
    }
    const badge = match.inEffect[match.seat];
    const banking = badge?.effect === 'doubleTake' || badge?.effect === 'multiplier';
    if (kind === 'badgePass' && (!banking || match.uses[match.seat] === 0)) {
      return false;
    }
    const held = match.faces.filter((face, die) => heldMask & (1 << die));
    const points = score(held, match.rules[match.seat]);
    if (points === 0) {
      return false;
    }
    match.lastMoveAt = this.now();
    match.moveIndex += 1;
    match.available &= ~heldMask;
    this.host.emit('diceMove', this.event(match, { heldMask, points, reason: kind }));
    this.send(match, 1 - match.seat, { kind: 'move', move: kind, held: heldMask });
    if (kind === 'continue') {
      match.turnScore += points;
      if (match.available === 0) {
        match.available = match.allDice;
      }
      this.awaitThrow(match);
      return true;
    }
    let banked = match.turnScore + points;
    if (kind === 'badgePass') {
      banked = badges.bankedWith(badge, points, match.turnScore);
      this.usedBadge(match, heldMask, banked - match.turnScore - points);
    }
    this.bank(match, banked, 'pass');
    return true;
  }

  // The thrower's match while they may move (thrown, no throw or Resurrection waiting), or null.
  throwerMatch(playerId) {
    const match = this.matches.get(this.byPlayer.get(playerId));
    if (!match || match.over || match.players[match.seat] !== playerId) {
      return null;
    }
    if (match.roll === 0 || match.awaitingThrow || match.bustWaiting) {
      return null;
    }
    return match;
  }

  // The thrower banks the turn's points; reaching the target wins.
  bank(match, banked, reason) {
    match.scores[match.seat] += banked;
    match.turnScore = 0;
    match.bustWaiting = false;
    const won = match.scores[match.seat] >= match.target;
    this.host.emit(
      'diceTurnEnd',
      this.event(match, { points: banked, reason: won ? 'won' : reason }),
    );
    if (won) {
      this.end(match, match.seat, 'won');
    } else {
      this.nextTurn(match);
    }
  }

  // The thrower used their badge: one use less, and diceBadge (the badge's effect as reason, the
  // dice it changed, the points it added).
  usedBadge(match, heldMask, points) {
    match.uses[match.seat] -= 1;
    this.host.emit(
      'diceBadge',
      this.event(match, {
        heldMask,
        points,
        reason: match.inEffect[match.seat].effect,
        faces: eventFaces(match),
      }),
    );
  }

  // The thrower uses their badge on the dice of their throw (heldMask, the selected dice): a
  // Might badge's seventh die (no dice), a Fortune or Swap-out badge's dice thrown again, a
  // Transmutation or Balatro badge's die changed. Whether it was accepted.
  badge(playerId, heldMask) {
    const match = this.throwerMatch(playerId);
    if (!match || match.uses[match.seat] === 0 || !Number.isInteger(heldMask)) {
      return false;
    }
    const badge = match.inEffect[match.seat];
    if (badge.effect === 'extraDice') {
      if (heldMask !== 0 || match.allDice !== ALL_DICE) {
        return false;
      }
      match.lastMoveAt = this.now();
      const extra = 1 << EXTRA_DIE;
      match.allDice = WITH_EXTRA_DIE;
      match.available |= extra;
      match.rolledMask |= extra;
      this.send(match, 1 - match.seat, { kind: 'move', move: 'badge', held: 0 });
      this.usedBadge(match, extra, 0);
      this.throwDice(match, extra, badge.effect);
      return true;
    }
    if (heldMask <= 0 || (heldMask & ~match.rolledMask) !== 0) {
      return false;
    }
    const dice = [];
    for (let die = 0; die < 7; die++) {
      if (heldMask & (1 << die)) {
        dice.push(die);
      }
    }
    const faces = dice.map((die) => match.faces[die]);
    const jokerDice = dice.map((die) => {
      const kind = match.dice[match.seat]?.[die];
      return kind ? kind.sides.includes(JOKER_SIDE) : false;
    });
    const change = badges.changeOf(badge, faces, jokerDice);
    if (change === undefined) {
      return false;
    }
    match.lastMoveAt = this.now();
    this.send(match, 1 - match.seat, { kind: 'move', move: 'badge', held: heldMask });
    if (change === null) {
      this.usedBadge(match, heldMask, 0);
      this.throwDice(match, heldMask, badge.effect);
      return true;
    }
    dice.forEach((die, index) => {
      match.faces[die] = change[index];
    });
    this.usedBadge(match, heldMask, 0);
    return true;
  }

  // The thrower's choice after a bust with their Resurrection badge: 'bank' the turn's points,
  // 'rethrow' the dice, or take the 'bust'. Whether it was accepted.
  bustChoice(playerId, choice) {
    const match = this.matches.get(this.byPlayer.get(playerId));
    if (!match || match.over || match.players[match.seat] !== playerId || !match.bustWaiting) {
      return false;
    }
    const name = BUST_CHOICES[choice];
    if (!name) {
      return false;
    }
    match.lastMoveAt = this.now();
    match.bustWaiting = false;
    this.send(match, 1 - match.seat, { kind: 'move', move: 'bustChoice', held: choice });
    if (name === 'bust') {
      this.bust(match);
      return true;
    }
    this.usedBadge(match, match.rolledMask, 0);
    if (name === 'bank') {
      this.bank(match, match.turnScore, 'pass');
      return true;
    }
    this.awaitThrow(match);
    return true;
  }

  // Ends a match: the turn ends with the reason, then diceMatchEnd. winner 0, 1 or NOBODY.
  end(match, winner, reason) {
    if (match.over) {
      return;
    }
    match.over = true;
    if (reason !== 'won') {
      this.host.emit('diceTurnEnd', this.event(match, { reason }));
    }
    for (const id of match.players) {
      if (this.byPlayer.get(id) === match.id) {
        this.byPlayer.delete(id);
      }
    }
    // The mannequin the server stood at the table goes.
    match.ai?.standIn?.remove();
    for (const id of match.watchers) {
      this.host.bridge?.send({ op: 'diceWatch', id, match: match.id, ended: true, things: [] });
    }
    match.watchers.clear();
    for (const id of [...match.spectators.keys()]) {
      this.endSpectate(id);
    }
    this.matches.delete(match.id);
    for (const seat of [0, 1]) {
      const result = winner === NOBODY ? 'none' : winner === seat ? 'won' : 'lost';
      this.send(match, seat, { kind: 'end', reason, result });
    }
    const [first, second] = match.players.map((id) => this.host.players.get(id) ?? null);
    this.host.emit(
      'diceMatchEnd',
      match.id,
      first,
      second,
      winner,
      reason,
      match.scores[0],
      match.scores[1],
    );
  }

  // The player left the table (gaveUp) or the server (left), or their client could not play it
  // (failed): the other player wins.
  leave(playerId, reason) {
    const match = this.matches.get(this.byPlayer.get(playerId));
    if (!match) {
      return false;
    }
    this.end(match, match.players[0] === playerId ? 1 : 0, reason);
    return true;
  }

  // A move from the player's client (op diceMove): continue or pass, or the player gave up (left
  // the game's table) or their game could not play the match (failed). Moves of another match
  // than the player's are dropped.
  clientMove(playerId, matchId, kind, heldMask) {
    if (kind === 'tableUse') {
      return this.tableUse(playerId);
    }
    if (this.byPlayer.get(playerId) !== matchId) {
      return false;
    }
    if (kind === 'gaveUp' || kind === 'failed') {
      return this.leave(playerId, kind);
    }
    if (kind === 'throw') {
      return this.throwNow(playerId);
    }
    if (kind === 'select') {
      return this.select(playerId, heldMask);
    }
    if (kind === 'badge') {
      return this.badge(playerId, heldMask);
    }
    if (kind === 'bustChoice') {
      return this.bustChoice(playerId, heldMask);
    }
    return this.move(playerId, kind, heldMask);
  }

  // The player used a dice table in their game ("Play dice", E): diceTableUse(player, table,
  // match) for the nearest table within TABLE_REACH of the player, `match` the player's match or
  // 0. The game's own reaction (an NPC's dice dialogue) stays the game's.
  tableUse(playerId) {
    const player = this.host.players.get(playerId);
    if (!player || !player.ready || !player.position) {
      return false;
    }
    const table = this.tableNear(player.position, player.dimension);
    if (!table) {
      return false;
    }
    this.host.emit('diceTableUse', player, table, this.byPlayer.get(playerId) ?? 0);
    return true;
  }

  // The thrower's game throws: the throw waiting for it is dealt.
  throwNow(playerId) {
    const match = this.matches.get(this.byPlayer.get(playerId));
    if (!match || match.over || match.players[match.seat] !== playerId || !match.awaitingThrow) {
      return false;
    }
    match.lastMoveAt = this.now();
    this.deal(match);
    return true;
  }

  // The dice the thrower has selected so far, shown to the other player's game.
  select(playerId, heldMask) {
    const match = this.matches.get(this.byPlayer.get(playerId));
    if (
      !match ||
      match.over ||
      match.players[match.seat] !== playerId ||
      match.awaitingThrow ||
      match.bustWaiting ||
      match.roll === 0 ||
      !Number.isInteger(heldMask) ||
      (heldMask & ~match.rolledMask) !== 0
    ) {
      return false;
    }
    match.lastMoveAt = this.now();
    this.send(match, 1 - match.seat, { kind: 'move', move: 'select', held: heldMask });
    return true;
  }

  // Where the table's things (its dice and cup) are in the thrower's game: the same to the
  // players nearby in the same world who do not play, for their games to show. Whether it was
  // passed on.
  clientView(playerId, matchId, things) {
    const match = this.matches.get(matchId);
    if (
      !match ||
      match.over ||
      this.byPlayer.get(playerId) !== matchId ||
      // The server's NPC has no game: its player's game tells its turn too.
      (match.players[match.seat] !== playerId && !match.ai) ||
      !Array.isArray(things)
    ) {
      return false;
    }
    if (match.settling) {
      match.settling.viewAt = this.now();
    }
    const thrower = this.host.players.get(playerId);
    for (const player of this.host.players.values()) {
      const near =
        player.dimension === thrower?.dimension &&
        player.position &&
        distance(player.position, match.table.position) <= WATCH_REACH;
      if (
        match.players.includes(player.id) ||
        !player.ready ||
        (!near && !match.spectators.has(player.id))
      ) {
        continue;
      }
      match.watchers.add(player.id);
      this.host.bridge?.send({
        op: 'diceWatch',
        id: player.id,
        match: match.id,
        ended: false,
        things,
      });
    }
    this.aimSpectators(match, things);
    return true;
  }

  // Where the dice of a throw lie on the table, or null while none are. A view's thing is
  // [key, x, y, z, rotation x, y, z, w, shown, scale, model, material]; a die's key has its top bit.
  diceCentre(match, things) {
    const table = match.table.position;
    const dice = things.filter((thing) => {
      const die = Array.isArray(thing) && String(thing[0]).charAt(0) >= '8' && thing[8] === true;
      return (
        die &&
        Math.hypot(thing[1] - table.x, thing[2] - table.y) <= SPECTATE_TABLE_REACH &&
        Math.abs(thing[3] - table.z) <= SPECTATE_TABLE_REACH
      );
    });
    if (dice.length === 0) {
      return null;
    }
    const mean = (index) => dice.reduce((sum, die) => sum + die[index], 0) / dice.length;
    return { x: mean(1), y: mean(2), z: mean(3) };
  }

  // A spectator's camera above a point of the table, looking down at it.
  shootSpectator(player, at, seconds) {
    this.host.cameraShots?.startShot(player, {
      position: { x: at.x, y: at.y - 0.05, z: at.z + SPECTATE_HEIGHT },
      lookAt: at,
      fov: SPECTATE_FOV,
      seconds,
    });
  }

  aimSpectators(match, things) {
    if (match.spectators.size === 0) {
      return;
    }
    const centre = this.diceCentre(match, things);
    if (!centre) {
      return;
    }
    for (const [id, aim] of match.spectators) {
      const player = this.host.players.get(id);
      const moved = Math.hypot(centre.x - aim.x, centre.y - aim.y, centre.z - aim.z);
      if (player && moved > SPECTATE_AIM_REACH) {
        match.spectators.set(id, centre);
        this.shootSpectator(player, centre, SPECTATE_MOVE_SECONDS);
      }
    }
  }

  // A player not in the match watches its dice from above, wherever their body stands.
  spectate(playerId, matchId) {
    const match = this.matches.get(matchId);
    const player = this.host.players.get(playerId);
    if (!match || match.over || !player?.ready || match.players.includes(playerId)) {
      return false;
    }
    if (this.byPlayer.has(playerId) || !this.host.cameraShots) {
      return false;
    }
    this.endSpectate(playerId);
    const table = match.table.position;
    const aim = { x: table.x, y: table.y, z: table.z + SPECTATE_TABLE_TOP };
    match.spectators.set(playerId, aim);
    this.spectating.set(playerId, match.id);
    this.shootSpectator(player, aim, 0);
    return true;
  }

  // The player's own view back, and false when they were not spectating.
  endSpectate(playerId) {
    const match = this.matches.get(this.spectating.get(playerId));
    if (!this.spectating.delete(playerId)) {
      return false;
    }
    match?.spectators.delete(playerId);
    const player = this.host.players.get(playerId);
    if (player) {
      this.host.cameraShots?.endShot(player);
    }
    return true;
  }

  // The player to throw waited too long: the other player wins.
  tick() {
    const now = this.now();
    this.freeChairs();
    for (const match of [...this.matches.values()]) {
      if (match.ai) {
        this.playAi(match, now);
      }
      if (match.over) {
        continue;
      }
      this.checkSettled(match, now);
      if (now - match.lastMoveAt >= TURN_TIMEOUT_MS) {
        this.end(match, 1 - match.seat, 'timedOut');
      } else if (match.awaitingThrow && now - match.throwAskedAt >= THROW_WAIT_MS) {
        this.deal(match);
      } else if (match.bustWaiting && now - match.bustSince >= THROW_WAIT_MS) {
        // A Resurrection badge not used in time: the bust stands, in both games.
        for (const seat of [0, 1]) {
          this.send(match, seat, { kind: 'move', move: 'bustChoice', held: 2 });
        }
        this.bust(match);
      }
    }
  }

  // diceSettled: the throw's dice lie still on the table.
  checkSettled(match, now) {
    const settling = match.settling;
    if (!settling) {
      return;
    }
    const quiet = now - settling.viewAt >= SETTLE_QUIET_MS && settling.viewAt > settling.since;
    if (!quiet && now - settling.since < SETTLE_LATEST_MS) {
      return;
    }
    match.settling = null;
    this.host.emit('diceSettled', settling.event);
  }

  api() {
    return Object.freeze({
      tables: () => this.tables(),
      start: (first, second, options) => this.start(first, second, options),
      startAgainstNpc: (player, options) =>
        this.startAgainstNpc(typeof player === 'number' ? player : player?.id, options),
      setNames: (match, first, second) => {
        const found = this.matches.get(match);
        const names = [checkName(first, 'first'), checkName(second, 'second')];
        if (!found) {
          return false;
        }
        found.names = names;
        for (const seat of [0, 1]) {
          this.send(found, seat, {
            kind: 'names',
            ownName: names[seat],
            opponentName: names[1 - seat],
          });
        }
        return true;
      },
      stop: (match) => {
        const found = this.matches.get(match);
        if (!found) {
          return false;
        }
        this.end(found, NOBODY, 'stopped');
        return true;
      },
      gameOf: (player) => this.byPlayer.get(player) ?? 0,
      // Sits a player down on a free chair of the table they stand at, to wait for a match; the
      // chair (0 or 1) or -1.
      sit: (player) => this.sit(player),
      // The ids of the players holding a table's two chairs, null for a free chair.
      sittersAt: (table) => this.sittersAt(table),
      // The game's dice kinds: id, name, model, sides (0 to 5 the faces 1 to 6, 6 a joker),
      // weights and dlc.
      kinds: () => KINDS,
      // The game's dice badges: id, name, effect, tier, model and dlc.
      badges: () => badges.BADGES,
      // How many more times each seat may use its badge in a match; null for no match.
      badgeUses: (match) => {
        const found = this.matches.get(match);
        return found ? [...found.uses] : null;
      },
      // A match's badges: each seat's badge name as chosen and whether it plays (a Defence badge
      // of the other player's tier cancels it), null for none; null for no match.
      badgesOf: (match) => {
        const found = this.matches.get(match);
        if (!found) {
          return null;
        }
        return found.badges.map((badge, seat) =>
          badge ? { name: badge.name, inEffect: found.inEffect[seat] !== null } : null,
        );
      },
      // A match's dice: each seat's six kind names, null for the game's plain dice; null for no
      // match.
      diceOf: (match) => {
        const found = this.matches.get(match);
        if (!found) {
          return null;
        }
        return found.dice.map((seat) => (seat ? seat.map((kind) => kind.name) : null));
      },
      spectate: (player, match) => this.spectate(player, match),
      endSpectate: (player) => this.endSpectate(player),
      // The players not playing who were shown the match's table (within 20 m of it in the
      // thrower's world when a throw went out), in the order they were first shown it.
      watchers: (match) => {
        const found = this.matches.get(match);
        return found ? [...found.watchers] : [];
      },
    });
  }
}

module.exports = { DiceMatches, TABLES };
