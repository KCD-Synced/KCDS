'use strict';

// Live check of the kcds.* SDK: runs on playerReady and on /apicheck, prints each result to the
// server log and a summary to the player's chat. Test kit only, never shipped to servers.

const { Events, Chat, Exports, Messages, Runtime, Http } = kcds.core;
const { Vector3, Quaternion, Color } = kcds.math;
const { Player, Inventory, Items } = kcds.players;
const { Horse } = kcds.characters;
const { Area, Prop, World } = kcds.world;

const SERVER_TYPES = {
  core: [
    'Bans',
    'Chat',
    'Events',
    'Exports',
    'Http',
    'Imports',
    'Locales',
    'Messages',
    'Queue',
    'Runtime',
    'SharedState',
  ],
  math: ['Color', 'Quaternion', 'Vector2', 'Vector3', 'Vector4'],
  players: [
    'Appearances',
    'Buffs',
    'DerivedPlayerStat',
    'Entity',
    'Inventory',
    'Items',
    'Needs',
    'Player',
    'PlayerStat',
    'Progression',
    'Trade',
  ],
  characters: [
    'Animations',
    'Dialogue',
    'Dog',
    'Horse',
    'HorseLooks',
    'Npc',
    'PatrolRoute',
    'Quest',
    'Vendor',
  ],
  world: [
    'Area',
    'Blip',
    'Cart',
    'CookPot',
    'Crafting',
    'Dice',
    'Door',
    'Gate',
    'Gathering',
    'Grave',
    'GroundItem',
    'LevelEdit',
    'Marker',
    'Prop',
    'Recipe',
    'SiegeEngine',
    'SmartObject',
    'Stash',
    'StonePile',
    'Trees',
    'Vfx',
    'World',
  ],
};
const REMOVED_GLOBALS = ['Events', 'Player', 'Door', 'Vector3', 'Runtime', 'SharedState'];

let joinRequests = 0;
let lastDeviceId = null;

Events.on('playerConnecting', async (request) => {
  joinRequests += 1;
  lastDeviceId = request.deviceId;
  request.update('API check: looking you up');
});

Exports.register('ping', () => 'pong');
Messages.handle('ask', (payload, reply) => reply({ echoed: payload }));

function report(lines, name, ok, detail = '') {
  const state = ok ? 'PASS' : 'FAIL';
  lines.push({ name, ok });
  console.log(`[api-check] ${state} server ${name}${detail ? ` (${detail})` : ''}`);
}

async function check(lines, name, body) {
  try {
    const result = await body();
    report(lines, name, result !== false, typeof result === 'string' ? result : '');
  } catch (problem) {
    report(lines, name, false, String(problem && problem.message));
  }
}

async function runServerChecks(player) {
  const lines = [];
  await check(lines, 'all domain types exist', () => {
    const missing = [];
    for (const [domain, names] of Object.entries(SERVER_TYPES)) {
      for (const name of names) {
        if (kcds[domain][name] === undefined) {
          missing.push(`${domain}.${name}`);
        }
      }
    }
    return missing.length === 0 ? `${Object.keys(kcds).join(',')}` : false;
  });
  await check(lines, 'old flat globals are gone', () => {
    return REMOVED_GLOBALS.every((name) => globalThis[name] === undefined);
  });
  await check(lines, 'language globals stay', () => {
    return (
      typeof setTimeout === 'function' &&
      typeof console.log === 'function' &&
      typeof fetch === 'function'
    );
  });
  await check(lines, 'Runtime', () => {
    return Runtime.isServer && !Runtime.isClient && typeof Runtime.apiVersion === 'string';
  });
  await check(lines, 'Runtime.serverVersion', () => typeof Runtime.serverVersion === 'string');
  await check(lines, 'math', () => {
    const lifted = new Vector3(1, 2, 3).add(new Vector3(0, 0, 1));
    const turned = Quaternion.fromAxisAngle(new Vector3(0, 0, 1), Math.PI / 2);
    return (
      lifted.z === 4 &&
      Math.abs(turned.rotateVector(new Vector3(0, 1, 0)).x + 1) < 0.001 &&
      Color.fromHex('#ff0000') !== null
    );
  });
  await check(lines, 'fire and listen', async () => {
    let heard = 0;
    const handler = (value) => {
      heard = value;
    };
    Events.listen('apicheck:local', handler);
    await Events.fire('apicheck:local', 7);
    return heard === 7;
  });
  await check(lines, 'toResource reaches this resource', async () => {
    let heard = 0;
    const handler = (value) => {
      heard = value;
    };
    Events.on('apicheck:targeted', handler);
    await Events.toResource('kcds_api_check', 'apicheck:targeted', 9);
    Events.off('apicheck:targeted', handler);
    return heard === 9 && Events.toPlayer === undefined;
  });
  await check(lines, 'Exports', () => Exports.get('kcds_api_check', 'ping')() === 'pong');
  await check(lines, 'Messages', async () => {
    const answer = await Messages.request('kcds_api_check', 'ask', 3);
    return answer.echoed === 3;
  });
  await check(lines, 'new message errors', async () => {
    let missing = '';
    let unhandled = '';
    await Messages.request('no_such_resource', 'ask', 1).catch((problem) => {
      missing = problem.message;
    });
    await Messages.request('kcds_api_check', 'no_such_type', 1).catch((problem) => {
      unhandled = problem.message;
    });
    return (
      missing === 'Resource is not available' &&
      unhandled === 'Resource does not handle this message type'
    );
  });
  await check(lines, 'playerConnecting and deviceId', () => {
    return joinRequests > 0 && typeof lastDeviceId === 'string';
  });
  await check(lines, 'SharedState on a player', () => {
    player.state.set('apicheck', 42);
    return player.state.get('apicheck') === 42 && player.state instanceof kcds.core.SharedState;
  });
  await check(lines, 'nameplate setters', () => {
    const textBefore = player.getNameplateText();
    const colorBefore = player.getNameplateColor();
    player.setNameplateText('API check');
    player.setNameplateColor(0xffc9a227);
    const changed =
      player.getNameplateText() === 'API check' && player.getNameplateColor() === 0xffc9a227;
    player.setNameplateText(textBefore);
    player.setNameplateColor(colorBefore);
    return changed && player.isNameplateVisible() !== undefined;
  });
  await check(lines, 'dimension', () => {
    const start = player.dimension;
    return (
      typeof start === 'number' && typeof player.setDimension === 'function' && 'deviceId' in player
    );
  });
  await check(lines, 'Area and Prop with showOnlyTo', () => {
    const at = player.position;
    const zone = Area.create({ id: 'apicheck-zone', type: 'sphere', position: at, radius: 3 });
    const barrel = Prop.spawn('barrel_a', new Vector3(at.x + 2, at.y, at.z));
    barrel.showOnlyTo(player);
    barrel.showOnlyTo(null);
    const ok = zone.contains(at) && barrel.dimension === player.dimension;
    barrel.destroy();
    zone.destroy();
    return ok;
  });
  await check(lines, 'Horse in world', () => {
    const at = player.position;
    const horse = Horse.spawn(new Vector3(at.x + 3, at.y + 3, at.z));
    const ok = horse.position.x === at.x + 3 && horse.dimension === player.dimension;
    setTimeout(() => horse.destroy(), 20000);
    return ok;
  });
  await check(lines, 'World and Inventory', () => {
    return (
      typeof World.hour === 'number' &&
      Inventory.get(player) !== null &&
      typeof Items.register === 'function'
    );
  });
  await check(lines, 'Http route', () => typeof Http === 'object');
  const failed = lines.filter((line) => !line.ok).map((line) => line.name);
  Chat.sendToPlayer(
    player,
    `API check, server: ${lines.length - failed.length}/${lines.length} OK`,
  );
  for (const name of failed) {
    Chat.sendToPlayer(player, `FAILED server: ${name}`);
  }
  player.emit('kcds_api_check:run', {});
}

Events.fromClient('kcds_api_check:result', (player, result) => {
  console.log(
    `[api-check] ${result.ok ? 'PASS' : 'FAIL'} client ${result.name}${
      result.detail ? ` (${result.detail})` : ''
    }`,
  );
});

Events.fromClient('kcds_api_check:done', (player, summary) => {
  Chat.sendToPlayer(player, `API check, client: ${summary.passed}/${summary.total} OK`);
  for (const name of summary.failed) {
    Chat.sendToPlayer(player, `FAILED client: ${name}`);
  }
  console.log(`[api-check] client finished ${summary.passed}/${summary.total}`);
});

Events.on('playerReady', (player) => {
  setTimeout(() => runServerChecks(player), 3000);
});

Events.on('playerCommand', (player, command) => {
  if (command === 'apicheck') {
    runServerChecks(player);
  }
});
