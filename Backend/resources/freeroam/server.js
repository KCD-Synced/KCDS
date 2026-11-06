'use strict';
// Example gamemode shipped with the KCDS server. Edit it, or add your own resource folder next to
// it in resources/: every folder with a package.json "kcds" section is started with the server.

const HELP = [
  '/players - who is online',
  '/tp <player> - teleport to a player (an id, a name or part of one)',
  '/horse - a horse in front of you',
  '/revive [player] - get a knocked-out player up (yourself without one)',
  '/kovision <0-100> - how much you see while knocked out (0 is a black screen)',
  '/appearance random | reset | gender <male|female> | <head|hair|beard|body> [name] - change how you look',
  '/lookalike <player> - look like another player',
  '/visible <on|off> - show or hide your body',
  '/disguise <mesh|off> - look like a prop (a catalog path or file stem, e.g. beer_barrel)',
  '/creator [male|female|both] | stop - design your look on the character stage',
  'F3 - fly through everything (NoClip: W A S D, Space up, C down, Shift fast); F3 again lands',
];

const APPEARANCE_PARTS = ['head', 'hair', 'beard', 'body'];

// The one player a command argument names: an id, a nickname or a unique part of one
// (kcds.players.Player.find). Tells the asking player when nobody or several players match; null then.
function namedPlayer(player, text, usage) {
  if (text === undefined || text === '') {
    kcds.core.Chat.sendToPlayer(player, usage);
    return null;
  }
  const found = kcds.players.Player.find(text);
  if (found.length === 0) {
    kcds.core.Chat.sendToPlayer(player, `No player matches "${text}" (see /players).`);
    return null;
  }
  if (found.length > 1) {
    const names = found.map((other) => `${other.nickname} (${other.id})`).join(', ');
    kcds.core.Chat.sendToPlayer(player, `"${text}" matches ${found.length} players: ${names}`);
    return null;
  }
  return found[0];
}

kcds.core.Events.on('playerReady', (player) => {
  kcds.core.Chat.sendToPlayer(
    player,
    `Welcome, ${player.nickname}! T opens chat, /help lists the commands.`,
    {
      author: 'Server',
    },
  );
  kcds.core.Chat.sendToAll(`${player.nickname} joined the server.`, { author: 'Server' });
});

kcds.core.Events.on('playerDisconnect', (player) => {
  creatorAsked.delete(player.id);
  kcds.core.Chat.sendToAll(`${player.nickname} left the server.`, { author: 'Server' });
});

// The character creator (client.js) runs at the player's machine; the server only takes a look
// from a player it asked, and puts it on through setAppearance, which checks it like any other.
const creatorAsked = new Set();

function creator(player, args) {
  if (args[0] === 'stop') {
    kcds.core.Events.broadcast('freeroam:creatorStop', { player: player.id });
    return;
  }
  const genders = ['male', 'female', 'both'].includes(args[0]) ? args[0] : 'both';
  creatorAsked.add(player.id);
  kcds.core.Events.broadcast('freeroam:creator', { player: player.id, genders });
}

kcds.core.Events.fromClient('freeroam:look', (sender, look) => {
  const player = sender;
  if (!creatorAsked.delete(player.id) || look === null || typeof look !== 'object') {
    return;
  }
  let sent = false;
  try {
    sent = player.setAppearance(look);
  } catch {
    sent = false;
  }
  kcds.core.Chat.sendToPlayer(
    player,
    sent ? 'Your new look is on.' : 'That look could not be put on.',
    {
      author: 'Server',
    },
  );
});

// Players are never killed: a lethal hit knocks them out until a script revives them.
kcds.core.Events.on('playerDied', (player) => {
  kcds.core.Chat.sendToAll(
    `${player.nickname} (${player.id}) was knocked out. /revive ${player.nickname} gets them up.`,
    {
      author: 'Server',
    },
  );
});

function revive(player, args) {
  const target = args.length
    ? namedPlayer(player, args.join(' '), 'Usage: /revive [player] (see /players)')
    : player;
  if (!target) {
    return;
  }
  if (!target.revive()) {
    kcds.core.Chat.sendToPlayer(player, `${target.nickname} is not knocked out.`);
    return;
  }
  kcds.core.Chat.sendToAll(`${target.nickname} was revived by ${player.nickname}.`, {
    author: 'Server',
  });
}

function setKnockoutVision(player, args) {
  const percent = Number(args[0]);
  if (!args.length || !Number.isInteger(percent) || percent < 0 || percent > 100) {
    kcds.core.Chat.sendToPlayer(player, 'Usage: /kovision <0-100> (0 is a black screen)');
    return;
  }
  player.setKnockoutVision(percent);
  kcds.core.Chat.sendToPlayer(player, `While knocked out you see ${percent}%.`, {
    author: 'Server',
  });
}

// A new face keeps the beard when the face can wear it and shaves it otherwise.
function setHead(player, head) {
  const { gender, beard } = player.appearance;
  const fits =
    beard === '' ||
    kcds.players.Appearances.beards(head, gender).some((option) => option.name === beard);
  return player.setAppearance(fits ? { head } : { head, beard: '' });
}

function changeAppearance(player, args) {
  const [what, name = ''] = args;
  let sent = false;
  if (what === 'random') {
    sent = player.setAppearance(kcds.players.Appearances.random(player.appearance.gender));
  } else if (what === 'reset') {
    sent = player.setAppearance({ head: '', hair: '', beard: '', body: '' });
  } else if (what === 'gender') {
    sent = player.setAppearance({ gender: name });
  } else if (what === 'head') {
    sent = setHead(player, name);
  } else if (APPEARANCE_PARTS.includes(what)) {
    sent = player.setAppearance({ [what]: name });
  } else {
    kcds.core.Chat.sendToPlayer(
      player,
      'Usage: /appearance random | reset | gender <male|female> | <head|hair|beard|body> [name]',
    );
    return;
  }
  if (!sent) {
    kcds.core.Chat.sendToPlayer(
      player,
      'Refused: not in the catalog, or a beard that face cannot wear.',
    );
  }
}

function lookalike(player, args) {
  const usage = 'Usage: /lookalike <another player> (see /players)';
  const model = namedPlayer(player, args.join(' '), usage);
  if (!model) {
    return;
  }
  if (model === player) {
    kcds.core.Chat.sendToPlayer(player, usage);
    return;
  }
  const wanted = model.appearance;
  if (APPEARANCE_PARTS.every((part) => wanted[part] === '')) {
    kcds.core.Chat.sendToPlayer(player, `${model.nickname} has not chosen a body yet.`);
    return;
  }
  if (!player.setAppearance(wanted)) {
    kcds.core.Chat.sendToPlayer(player, 'Refused. Your own client decides what it can wear.');
    return;
  }
  kcds.core.Chat.sendToPlayer(player, `Asked to look like ${model.nickname}.`, {
    author: 'Server',
  });
}

function setVisible(player, args) {
  if (args[0] !== 'on' && args[0] !== 'off') {
    kcds.core.Chat.sendToPlayer(player, 'Usage: /visible <on|off>');
    return;
  }
  player.setVisible(args[0] === 'on');
  kcds.core.Chat.sendToPlayer(
    player,
    args[0] === 'on' ? 'Your body is shown.' : 'Your body is hidden.',
    {
      author: 'Server',
    },
  );
}

function disguise(player, args) {
  if (!args.length) {
    kcds.core.Chat.sendToPlayer(player, 'Usage: /disguise <mesh|off>');
    return;
  }
  const model = args[0] === 'off' ? null : args[0];
  try {
    player.setDisguise(model);
  } catch (error) {
    kcds.core.Chat.sendToPlayer(player, `No such mesh: ${args[0]}`);
    return;
  }
  kcds.core.Chat.sendToPlayer(
    player,
    model ? `You look like ${model}.` : 'You look like yourself.',
    {
      author: 'Server',
    },
  );
}

function listPlayers(player) {
  const names = kcds.players.Player.all().map((other) => `${other.nickname} (${other.id})`);
  kcds.core.Chat.sendToPlayer(player, `Online (${names.length}): ${names.join(', ')}`, {
    author: 'Server',
  });
}

function teleport(player, args) {
  const target = namedPlayer(player, args.join(' '), 'Usage: /tp <player> (see /players)');
  if (!target) {
    return;
  }
  if (target === player) {
    kcds.core.Chat.sendToPlayer(player, 'You cannot teleport to yourself.');
    return;
  }
  if (
    !player.teleport(target.position.clone().add(new kcds.math.Vector3(1, 1, 0)), target.nickname)
  ) {
    kcds.core.Chat.sendToPlayer(player, `${target.nickname} is not anywhere to go to.`);
    return;
  }
  kcds.core.Chat.sendToPlayer(player, `Teleported to ${target.nickname}.`, { author: 'Server' });
}

function spawnHorse(player) {
  for (const old of kcds.characters.Horse.all()) {
    if (old.ownerId !== player.id) {
      continue;
    }
    if (old.mounted) {
      kcds.core.Chat.sendToPlayer(player, 'Dismount before replacing your horse.');
      return;
    }
    old.destroy();
  }
  const rotation = player.rotation;
  const facing = rotation.rotateVector(new kcds.math.Vector3(0, 1, 0));
  const length = Math.hypot(facing.x, facing.y);
  const direction =
    length < 1e-4
      ? new kcds.math.Vector3(0, 1, 0)
      : new kcds.math.Vector3(facing.x / length, facing.y / length, 0);
  const position = player.position.clone().add(direction.mul(3));
  const horse = kcds.characters.Horse.spawn(
    position,
    rotation,
    undefined,
    `${player.nickname}'s horse`,
  );
  horse.setDimension(player.dimension);
  horse.giveTo(player);
}

kcds.core.Events.on('playerCommand', (player, command, args) => {
  const name = command.toLowerCase();
  if (name === 'help') {
    // One chat message per line: chat text cannot contain line breaks.
    for (const line of HELP) {
      kcds.core.Chat.sendToPlayer(player, line, { author: 'Server' });
    }
    return;
  }
  if (!player.ready && (name === 'tp' || name === 'horse')) {
    kcds.core.Chat.sendToPlayer(player, 'Wait until you are in the world.');
    return;
  }
  if (name === 'players') {
    listPlayers(player);
  } else if (name === 'tp') {
    teleport(player, args);
  } else if (name === 'horse') {
    spawnHorse(player);
  } else if (name === 'revive') {
    revive(player, args);
  } else if (name === 'kovision') {
    setKnockoutVision(player, args);
  } else if (name === 'appearance') {
    changeAppearance(player, args);
  } else if (name === 'lookalike') {
    lookalike(player, args);
  } else if (name === 'visible') {
    setVisible(player, args);
  } else if (name === 'disguise') {
    disguise(player, args);
  } else if (name === 'creator') {
    creator(player, args);
  }
});
