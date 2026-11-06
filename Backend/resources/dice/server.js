'use strict';
// Example resource shipped with the KCDS server: a player who uses one of the
// game's dice tables ("Play dice", E) sits down on a free chair of it and waits there. When another
// player sits down on its other chair, the match starts; each plays from their own chair and both
// stay seated after it. Standing up frees the chair; two players who sit down together again play
// another match. The winner is told in the chat. /dicenpc [easy|normal|hard] at a table plays
// against the server's NPC.

const CHECK_MILLISECONDS = 500;
const TARGET_SCORE = 2000;

// table id -> the pair that last played there ("first:second"): they play again only after one of
// them stood up and sat down again.
const played = new Map();

function playerById(id) {
  return id === null ? null : kcds.players.Player.getById(id);
}

kcds.core.Events.on('diceTableUse', (player, table, match) => {
  if (match !== 0) {
    return;
  }
  const before = kcds.world.Dice.sittersAt(table.id);
  const seat = kcds.world.Dice.sit(player.id);
  if (seat < 0) {
    kcds.core.Chat.sendToPlayer(player, 'Both chairs at this dice table are taken.');
    return;
  }
  if (before[seat] === player.id) {
    return;
  }
  const other = playerById(before[1 - seat]);
  if (other) {
    kcds.core.Chat.sendToPlayer(player, `You sit down to play dice with ${other.nickname}.`);
    kcds.core.Chat.sendToPlayer(other, `${player.nickname} sits down to play dice with you.`);
  } else {
    kcds.core.Chat.sendToPlayer(
      player,
      'You sit down at the dice table. Waiting for another player.',
    );
  }
});

// Starts a match at each table whose two chairs hold seated players who do not play yet.
function startMatches() {
  for (const table of kcds.world.Dice.tables()) {
    const [first, second] = kcds.world.Dice.sittersAt(table.id).map(playerById);
    if (!first || !second) {
      played.delete(table.id);
      continue;
    }
    if (!first.seated || !second.seated) {
      continue;
    }
    if (kcds.world.Dice.gameOf(first.id) !== 0 || kcds.world.Dice.gameOf(second.id) !== 0) {
      continue;
    }
    const pair = `${first.id}:${second.id}`;
    if (played.get(table.id) === pair) {
      continue;
    }
    played.set(table.id, pair);
    try {
      kcds.world.Dice.start(first.id, second.id, {
        targetScore: TARGET_SCORE,
        firstName: first.nickname,
        secondName: second.nickname,
      });
    } catch (error) {
      for (const player of [first, second]) {
        kcds.core.Chat.sendToPlayer(player, `Dice could not start: ${error.message}`);
      }
    }
  }
}

kcds.core.Events.on('playerCommand', (player, command, args) => {
  if (command !== 'dicenpc') {
    return;
  }
  const difficulty = args[0] ?? 'normal';
  try {
    kcds.world.Dice.startAgainstNpc(player, {
      difficulty,
      targetScore: TARGET_SCORE,
      playerName: player.nickname,
    });
  } catch (error) {
    kcds.core.Chat.sendToPlayer(player, `Dice could not start: ${error.message}`);
  }
});

kcds.core.Events.on(
  'diceMatchEnd',
  (match, first, second, winner, reason, firstScore, secondScore) => {
    const won = winner === 0 ? first : winner === 1 ? second : null;
    // Against the server's NPC its seat has no player.
    const npcWon = (winner === 0 || winner === 1) && won === null && reason === 'won';
    let text = `The dice match ended without a winner (${reason}).`;
    if (won) {
      text = `${won.nickname} won at dice (${firstScore} : ${secondScore}).`;
    } else if (npcWon) {
      text = `Your opponent won at dice (${firstScore} : ${secondScore}).`;
    }
    for (const player of [first, second]) {
      if (player) {
        kcds.core.Chat.sendToPlayer(player, text);
      }
    }
  },
);

setInterval(startMatches, CHECK_MILLISECONDS);
