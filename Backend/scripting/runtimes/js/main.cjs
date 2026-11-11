'use strict';
const path = require('node:path');
const readline = require('node:readline');
const { ResourceHost } = require('./host.cjs');
const { encode, decode } = require('./codec.cjs');
const { ResourceBundleStore } = require('./bundles.cjs');
const { ResourceFileServer } = require('./web/file_server.cjs');
const { EntitySender } = require('./entity_sender.cjs');
const { testAbsenceMs } = require('./horses/horse_owners.cjs');
const { watchEventLoop } = require('./health/event_loop.cjs');
const { mapsFromArguments } = require('./maps/server_maps.cjs');
const { runGuardedAction } = require('./inventory/guarded_actions.cjs');
const root = process.argv[2];
if (!root) {
  throw new Error(
    'Usage: node main.cjs <resources directory> [<first id>:<count>] [--map=<level>] ' +
      '[--maps=<level>,<map>...]',
  );
}
// Network ids reserved by the native server for entities the resources create.
const idBlock = process.argv.slice(3).find((argument) => /^\d+:\d+$/.test(argument));
const [firstEntityId, entityIdCount] = (idBlock ?? '').split(':').map(Number);
// The server's map, known before the resources start: World.level and the level's
// areas are ready in resourceStart. hostSettings repeats it later.
const mapArgument = process.argv.slice(3).find((argument) => argument.startsWith('--map='));
const map = mapArgument ? mapArgument.slice('--map='.length) : '';
// Every map of a server that runs more than one (server.json maps), the level first.
const mapsArgument = process.argv.slice(3).find((argument) => argument.startsWith('--maps='));
const maps = mapsFromArguments(map, mapsArgument ? mapsArgument.slice('--maps='.length) : '');
function send(value) {
  process.stdout.write(encode(value).toString('hex') + '\n');
}
const host = new ResourceHost(root, {
  entityIds: idBlock ? { first: firstEntityId, count: entityIdCount } : undefined,
  // A shorter wait for a departed owner's horse, for live tests only (horses/horse_owners.cjs).
  horseOwnerAbsenceMs: testAbsenceMs(process.env),
  // World exports to load once the resources run: file paths in KCDS_WORLD_RESOURCES, separated
  // like PATH.
  worldResources: (process.env.KCDS_WORLD_RESOURCES ?? '').split(path.delimiter).filter(Boolean),
  // The resources' HTTP routes listen only when the server owner sets KCDS_HTTP_PORT.
  httpPort: Number(process.env.KCDS_HTTP_PORT) || undefined,
  httpBind: process.env.KCDS_HTTP_BIND,
  // The password a join must bring; never logged or sent anywhere.
  serverPassword: process.env.KCDS_SERVER_PASSWORD,
  // Only players with a verified Discord account get in: KCDS_REQUIRE_DISCORD=on.
  requireDiscord: process.env.KCDS_REQUIRE_DISCORD === 'on',
  // The built-in KCDS-Admin: off without a port and an owner password.
  adminPort: Number(process.env.KCDS_ADMIN_PORT) || undefined,
  adminBind: process.env.KCDS_ADMIN_BIND,
  adminPassword: process.env.KCDS_ADMIN_PASSWORD,
  adminTlsCert: process.env.KCDS_ADMIN_TLS_CERT,
  adminTlsKey: process.env.KCDS_ADMIN_TLS_KEY,
  // The server's settings file KCDS-Admin edits, set by the native server.
  serverConfig: process.env.KCDS_SERVER_CONFIG,
  // Daily restarts with warnings, "HH:MM,HH:MM" in the server's local time.
  restartTimes: process.env.KCDS_RESTART_TIMES,
  send,
  log: (resource, text) => send({ op: 'log', resource: resource ?? 'host', text: String(text) }),
});
const bundles = new ResourceBundleStore();
// The client files over HTTP on TCP with the game's port number.
const fileServer = new ResourceFileServer({
  store: bundles,
  port: Number(process.env.KCDS_GAME_PORT),
  bind: process.env.KCDS_GAME_BIND,
  log: (text) => send({ op: 'log', resource: 'host', text }),
  // The server's public card, for a game that has it as a favourite by its address.
  status: host.statusRoutes,
});
// What the games are told of the catalog: its revision and hashes, and whether the files come
// over HTTP. The reply to one game carries that game's own HTTP token (web/file_server.cjs).
function catalogInfo(peer = null) {
  const http = fileServer.listening;
  return {
    op: 'catalogInfo',
    ...bundles.info(),
    ...(http ? { httpFiles: true } : {}),
    ...(http && peer !== null ? { httpToken: fileServer.tokenFor(peer) } : {}),
  };
}
// The peer cap and budgets follow the native server's hostSettings message.
const entitySender = new EntitySender({
  store: host.stateStore,
  send: (peer, message) => send({ op: 'resourceReply', peer, message }),
  onError: (error) => host.bridge.log('host', String(error)),
});
// A player's nameplate goes into the clients' directories, for their Player's nameplate readers.
host.nameplateChanged = (id, nameplate) => entitySender.setNameplate(id, nameplate);
bundles.report = (text) => host.bridge.log('host', text);
function publishBundles() {
  const definitions = host
    .order()
    .filter((name) => host.resources.has(name))
    .map((name) => ({
      ...host.manifests.get(name),
      generation: host.resources.get(name).generation,
    }));
  // A catalog that cannot be built must not stop the rest of the start, which lets players in.
  try {
    if (bundles.publish(definitions)) send({ op: 'resourceBroadcast', message: catalogInfo() });
  } catch (error) {
    host.bridge.log('host', `Client resources not sent to players: ${error.message}`);
  }
}
host.onResourcesChanged = publishBundles;
async function consoleCommand(text) {
  if (typeof text !== 'string' || Buffer.byteLength(text) > 1024) return;
  const parts = text.trim().split(/\s+/);
  const command = parts.shift();
  const name = parts[0];
  switch (command) {
    case 'start':
      await host.start(name);
      publishBundles();
      break;
    case 'stop':
      await host.stop(name);
      publishBundles();
      break;
    case 'restart':
    case 'ensure':
      await host.ensure(name);
      publishBundles();
      break;
    case 'refresh':
      host.refresh();
      break;
    case 'refreshall':
      await host.refreshAll();
      publishBundles();
      break;
    default:
      await host.emitAsync('consoleCommand', command, parts);
      break;
  }
}
// KCDS-Admin runs console commands as the server console does.
host.consoleCommand = consoleCommand;
// Client events reach only Events.fromClient handlers as (sender, payload). The sender is the Player
// handle of the connection the native server stamped on the request; the payload arrives parsed.
function receiveClientEvent(playerId, name, payload) {
  const sender = host.players.get(playerId);
  if (!sender) {
    host.bridge.log('host', `Client event ${name} dropped: unknown sender`);
    return;
  }
  // A payload that is not JSON text (an older client's plain string) arrives as that string.
  let value;
  try {
    value = JSON.parse(payload);
  } catch {
    value = payload;
  }
  // A tabletop request (sit, stand, a click on a cell) is the host's own.
  if (host.tabletops.receive(sender, name, value)) {
    return;
  }
  host.services
    .emit('client', name, [sender, value])
    .catch((error) => host.bridge.log('host', String(error)));
}
// playerResourcesReady: a player's game has downloaded and started the client resources.
function resourcesReady(playerId) {
  // The events held for them while they downloaded go first.
  host.clientResourcesStarted(playerId);
  const player = host.players.get(playerId);
  if (!player) {
    return;
  }
  host.emitAsync('playerResourcesReady', player).catch((error) => {
    host.bridge.log('host', String(error));
  });
}
function clientRequest(event) {
  // peer/player are stamped by the native server, never taken from the client's
  // own message. Client operations cannot invoke console/lifecycle commands.
  const message = event.message;
  const requestId = message?.requestId;
  const reply = (message) =>
    send({ op: 'resourceReply', peer: event.peer, message: { ...message, requestId } });
  try {
    switch (message?.op) {
      case 'catalogRequest':
        // A runtime that asks again after it had its entity session is a new one.
        entitySender.restartPeer(event.peer);
        reply(catalogInfo(event.peer));
        break;
      case 'catalogChunk':
        reply({
          op: 'catalogData',
          ...bundles.catalogChunk(message.revision, message.offset, message.length),
        });
        break;
      case 'fileRequest':
        reply({
          op: 'fileData',
          ...bundles.chunk(message.revision, message.hash, message.offset, message.length),
        });
        break;
      case 'event':
        if (
          typeof message.name !== 'string' ||
          typeof message.payload !== 'string' ||
          message.name.length > 256 ||
          Buffer.byteLength(message.payload) > 32768
        )
          throw new Error('Invalid client resource event');
        receiveClientEvent(event.player, message.name, message.payload);
        break;
      case 'entityAck':
        entitySender.acknowledge(event.peer, message);
        break;
      case 'groundItemRest':
        // Where a stack this player's game laid came to rest.
        host.groundItems.rest(host.players.get(event.player), message);
        break;
      case 'resourcesReady':
        host.bridge.log('host', `Client resources ready for player ${event.playerIndex}`);
        // The player's client resources run now: what the server sends them from here on arrives
        // playerReady can come first, while they still download.
        resourcesReady(event.player);
        break;
      default:
        throw new Error('Unsupported client resource operation');
    }
  } catch (error) {
    reply({ op: 'resourceError', error: String(error) });
  }
}
// A resource holding the shared event loop for long is logged.
watchEventLoop((text) => host.bridge.log('host', text));
// A crash report of the last run, if it ended without stopping in order.
try {
  host.serverUpdates.cleanup();
} catch (error) {
  host.bridge.log('host', `the last update's old files were not removed: ${error.message}`);
}
try {
  host.crashReports.start();
  // What a report of a run that ends with no word shows: the log as it stood a few seconds before.
  setInterval(() => {
    try {
      host.crashReports.keepLog(host.adminLog.lines);
    } catch (error) {
      host.bridge.log('host', `crash report log not kept: ${error.message}`);
    }
  }, 5000).unref();
} catch (error) {
  host.bridge.log('host', `crash reports unavailable: ${error.message}`);
}
host.setMaps(maps);
if (map) {
  host.statusRoutes.configure({ ...host.statusRoutes.settings, map });
  // The level areas of each map the server runs.
  for (const [index, level] of (maps.length ? maps : [map]).entries()) {
    try {
      host.areas.setLevel(level, undefined, index);
    } catch (error) {
      host.bridge.log('host', `Level areas of ${level}: ${error.message}`);
    }
  }
  // Each map's own herbs, read now rather than at a script's first question.
  for (let index = 0; index < Math.max(1, maps.length); index++) {
    host.gathering.herbs(index);
  }
}
let chain = Promise.all([host.startAll(), fileServer.start()]).then(() => {
  publishBundles();
  send({ op: 'ready' });
});
readline
  .createInterface({ input: process.stdin, crlfDelay: Infinity })
  .on('line', (line) => {
    if (line.length > 131072 || line.length % 2 || !/^[0-9a-f]*$/i.test(line)) {
      send({ op: 'log', resource: 'host', text: 'Malformed IPC frame' });
      return;
    }
    chain = chain
      .then(async () => {
        const e = decode(Buffer.from(line, 'hex'));
        switch (e.op) {
          case 'console':
            await consoleCommand(e.text);
            break;
          case 'clientRequest':
            clientRequest(e);
            break;
          case 'player': {
            const known = host.players.has(e.id);
            host.updatePlayer(e);
            // A new player's client events wait until their client resources run.
            if (!known && host.players.has(e.id)) host.clientResourcesPending(e.id);
            break;
          }
          case 'xpGaining':
            host.gateXp(e);
            break;
          case 'perkLearning':
            host.gatePerk(e);
            break;
          case 'playerBuffs':
            host.updateBuffs(e.id, e.buffs);
            break;
          case 'buffBlocked':
            host.buffBlocked(e);
            break;
          case 'inventoryShown':
            host.playerInventories.shown(host.players.get(e.id), e.revision);
            break;
          case 'inventoryNative':
            host.playerInventories.native(
              host.players.get(e.id),
              (e.changes ?? []).map(([kind, id, item, amount, quality, health]) => ({
                kind: ['', 'lost', 'worn', 'gained', 'equipped'][kind] ?? '',
                id,
                item,
                amount,
                quality,
                health,
              })),
              { id: e.id, epoch: e.epoch, sequence: e.sequence, revision: e.revision },
            );
            break;
          // What players' own actions do to inventories waits for Inventory.beforeCommit.
          case 'inventoryConsume':
          case 'inventoryPotConsume':
          case 'inventoryDrop':
          case 'groundItemPickup':
          case 'vendorTrade':
          case 'stashTake':
          case 'stashPut':
          case 'horseGearEquip':
          case 'horseGearRemove':
            runGuardedAction(host, e);
            break;
          case 'inventoryConsumed':
            host.playerInventories.consumed(host.players.get(e.id), e);
            break;
          case 'playerAttack':
            host.playerAttack(e);
            break;
          case 'playerDamage':
            host.playerDamage(e);
            break;
          case 'playerHit':
            host.playerHit(e);
            break;
          case 'playerHarm':
            host.playerHarm(e);
            break;
          case 'inventoryCustomUse':
            host.customItems.use(host.players.get(e.id), e.row, e.revision);
            break;
          case 'stashOpen':
            host.stashes.open(host.players.get(e.id), e.stash);
            break;
          case 'stashClose':
            host.stashes.close(host.players.get(e.id), e.stash);
            break;
          case 'stashUnlock':
            host.stashes.unlock(host.players.get(e.id), e.stash, e.method);
            break;
          case 'questTracking':
            host.quests.trackingReported(host.players.get(e.id), e.quest, e.tracked);
            break;
          case 'craftTable': {
            // What lies on the alchemy table the player brews at.
            const player = host.players.get(e.id);
            if (player) host.crafting.table(player, e.items);
            break;
          }
          case 'craftReport': {
            // What the player's game does at a crafting station.
            const player = host.players.get(e.id);
            if (player && e.report === 'prepare') {
              // Cooking, smoking or drying the player's game asks about.
              host.preparing.ask(player, e);
            } else if (player) {
              host.crafting.report(player, e, player.dimension);
            }
            break;
          }
          case 'dialogueChoice':
            host.dialogues.choice(host.players.get(e.id), e.session, e.generation, e.index);
            break;
          case 'gatherReport':
            // A herb the player's game picked.
            host.gathering.report(host.players.get(e.id), e);
            break;
          case 'voiceTalking':
            host.voiceChat.talkingChanged(e.id, e.talking === true);
            break;
          case 'voiceTierRequest':
            host.voiceChat.tierRequested(e.id, e.tier);
            break;
          case 'diceView':
            // Where the dice table's things are in the thrower's game.
            host.dice.clientView(e.id, e.match, e.things);
            break;
          case 'diceMove':
            // A move in the player's dice match.
            host.dice.clientMove(e.id, e.match, e.kind, e.held);
            break;
          case 'dialogueCancel':
            host.dialogues.cancelled(host.players.get(e.id), e.session, e.reason);
            break;
          case 'vendorClosed':
            host.vendors.closed(host.players.get(e.id), e.session, e.reason);
            break;
          case 'vendorHaggle':
            // A price the player offered in a vendor's haggle (0.4.1).
            host.vendors.haggle(
              host.players.get(e.id),
              e.session,
              e.revision,
              e.round,
              e.offered,
              e.bought,
              e.sold,
            );
            break;
          case 'worldPickup':
            host.worldPickables.pickup(host.players.get(e.id), e.key);
            break;
          case 'placerReport':
            host.propPreviews.report(host.players.get(e.id), e);
            break;
          case 'graveReport':
            host.graves.report(host.players.get(e.id), e.grave, e.position, e.dug);
            break;
          case 'graveDug':
            host.graves.dug(host.players.get(e.id), e.grave, e.position);
            break;
          case 'doorReport':
            host.doors.report(host.players.get(e.id), e.doors);
            break;
          case 'doorUse':
            if (e.npc) {
              host.doors.npcUse(host.players.get(e.id), e.npc, e.guid, e.action);
            } else {
              host.doors.use(host.players.get(e.id), e.guid, e.action, e.keySide);
            }
            break;
          case 'cartEnter':
            host.carts.enter(host.players.get(e.id), e.cart, e.seat);
            break;
          case 'cartLeave':
            host.carts.leave(host.players.get(e.id), e.cart);
            break;
          case 'cartAtStart':
            host.carts.atStart(host.players.get(e.id), e.cart);
            break;
          case 'cartDrive':
            host.carts.drive(host.players.get(e.id), e.cart, e.position, e.rotation, e.pace);
            break;
          case 'propReport':
            host.props.report(host.players.get(e.id), e);
            break;
          case 'worldRaycastReply':
          case 'worldBodiesReply':
            host.worldTraces.reply(host.players.get(e.id), e);
            break;
          case 'playerWeaponHit':
            host.worldTraces.weaponHit(host.players.get(e.id), e.hit);
            break;
          case 'cameraShotEnded':
            host.cameraShots.ended(host.players.get(e.id), e.reason);
            break;
          case 'doorNearestReply':
            host.doors.nearestReply(host.players.get(e.id), e.request, e.door, e.reason);
            break;
          case 'pickpocket':
            host.pickpocketing.report(host.players.get(e.id), e);
            break;
          case 'bodyCarryReport':
            host.bodyCarries.report(host.players.get(e.id), e);
            break;
          case 'animationEnd':
            host.playerAnimations.report(host.players.get(e.id), e);
            break;
          case 'animationPropCue':
            host.playerAnimations.propCue(host.players.get(e.id), e);
            break;
          case 'siegeImpactReport':
            host.siege.impactReport(host.players.get(e.id), e);
            break;
          case 'siegeUse':
            host.siege.use(host.players.get(e.id), e);
            break;
          case 'siegeLadderUse':
            host.siegeLadders.use(host.players.get(e.id), e.ladder, e.action);
            break;
          case 'siegeLadderSettled':
            host.siegeLadders.settled(host.players.get(e.id), e.ladder, e.pose);
            break;
          case 'stonePileTake':
            host.stonePiles.take(host.players.get(e.id), e.pile);
            break;
          case 'stoneImpact':
            host.stonePiles.impact(host.players.get(e.id), e.pile, e.position);
            break;
          case 'pilloryReleased':
            host.pillories.report(host.players.get(e.id), e);
            break;
          case 'playerWhistled':
            host.whistles.reported(host.players.get(e.id));
            break;
          case 'npcBarked':
            host.npcs.barked(e);
            break;
          case 'interactionFired':
            host.worldInteractions.fired(host.players.get(e.id), e.interaction);
            break;
          case 'shotLanded':
            host.missileLanded(host.players.get(e.id), e);
            break;
          case 'carryPutDown':
            host.carries.putDown(host.players.get(e.id), e);
            break;
          case 'inventoryQuickslots':
            host.playerInventories.quickslots(host.players.get(e.id), e.quickslots);
            break;
          case 'inventoryCensus':
            host.playerInventories.census(
              host.players.get(e.id),
              (e.rows ?? []).map(([item, amount, quality, health, equipped]) => ({
                item,
                amount,
                quality,
                health,
                equipped,
              })),
            );
            break;
          case 'playerPings':
            host.setPings(e.pings);
            break;
          case 'playerProgression':
            entitySender.setProgression(e.id, host.updateProgression(e.id, e.progression));
            break;
          case 'playerConnecting': {
            // Answered without blocking the IPC chain: a handler may wait on a queue or a lookup.
            const request = e.request;
            const identity = {
              nickname: e.nickname,
              steamId: e.steamId,
              discordId: e.discordId,
              deviceId: e.deviceId,
              ip: e.ip,
              licence: e.licence,
              licenceBinding: e.licenceBinding,
              passwordProof: e.passwordProof,
              joinToken: e.joinToken,
            };
            host
              .admitConnection(request, identity)
              .then(({ allowed, reason, password, banned, discord }) =>
                send({
                  op: 'playerConnectingResult',
                  request,
                  allowed,
                  reason,
                  password: password === true,
                  banned: banned === true,
                  discord: discord === true,
                }),
              );
            break;
          }
          case 'playerAdmitted':
            host.playerAdmitted(e.request, e.id);
            break;
          case 'joinQueued':
            host.joinQueue.queued(e);
            break;
          case 'masterListing':
            host.licence.setListing(e.id);
            // The link players can share to join this server.
            if (host.licence.listing === e.id) {
              host.bridge.log('host', `join link kcds://server/${e.id}`);
            }
            break;
          case 'joinUnqueued':
            host.joinQueue.unqueued(e);
            break;
          case 'entityWorldResult':
            host.confirmPlayerWorld(e);
            break;
          case 'horseState':
            host.updateHorse(e);
            break;
          case 'horseHealth':
            host.horseHealthReported(e);
            break;
          case 'horseStamina':
            host.horseStaminaReported(e);
            break;
          case 'npcState':
            host.npcs.update(e);
            break;
          case 'npcSimulator':
            host.npcs.simulatorChanged(e);
            break;
          case 'npcHit':
            host.npcs.hit(e);
            break;
          case 'npcIntentDone':
            host.npcs.intentDone(e);
            break;
          case 'npcAnimationEnd':
            host.npcs.animationEnded(e);
            break;
          case 'horseIntentDone':
            host.horseIntentDone(e);
            break;
          case 'npcHarvest':
            host.npcs.harvests.harvest(e);
            break;
          case 'npcInteract':
            host.npcs.harvests.interact(e);
            break;
          case 'npcEngage':
            host.npcs.engaged(e);
            break;
          case 'playerTravelStarted': {
            // A player began a fast travel: a trip to another map when it goes to a travel point.
            const traveller = host.players.get(e.id);
            if (traveller) {
              host.levelTravel.travelStarted(traveller, { x: e.x, y: e.y, z: e.z });
            }
            break;
          }
          case 'entityIds':
            host.addEntityIds(e);
            break;
          case 'clientNatives':
            host.clientNatives(e);
            break;
          case 'serverMetrics':
            // A metrics interval of the native server, for GET /metrics and the status health.
            host.statusRoutes.setMetrics(e.json);
            break;
          case 'hostSettings':
            entitySender.configure({ maxPeers: e.maxPeers });
            host.licence.setPort(e.port);
            // The status API's level, cap, version and protocol.
            host.statusRoutes.configure(e);
            // Every map the server runs, and each one's own level areas.
            const levels = Array.isArray(e.maps) && e.maps.length ? e.maps : [e.map];
            host.setMaps(levels);
            for (const [index, level] of levels.entries()) {
              try {
                host.areas.setLevel(level, undefined, index);
              } catch (error) {
                host.bridge.log('host', `Level areas of ${level}: ${error.message}`);
              }
            }
            for (let index = 0; index < levels.length; index++) {
              host.gathering.herbs(index);
            }
            break;
          case 'entityRecords':
            entitySender.applyRecords(e);
            break;
          case 'entityRemoved':
            entitySender.removeRecords(e);
            break;
          case 'entityViewDelta':
            entitySender.applyViewDelta(e);
            break;
          case 'entityViewPart':
            entitySender.receiveViewPart(e);
            break;
          case 'entityPump':
            entitySender.pump(performance.now() / 1000);
            host.pumpHitRules();
            host.pumpDiceTables();
            // Custom crafts' progress and the game's unfinished crafts that settle
            // then the inventory changes of the last tick, in order.
            host.customCrafting.tick();
            host.crafting.tick();
            // Dice matches whose player to throw waited too long.
            host.dice.tick();
            // Picked herbs whose time has come grow back.
            host.gathering.tick();
            // The games that simulate the rigid props.
            host.props.tick();
            host.playerInventories.flush();
            break;
          case 'disconnect':
            entitySender.setProgression(e.id, null);
            entitySender.disconnect(e.peer);
            fileServer.forget(e.peer);
            host.disconnect(e.id);
            break;
          case 'chat':
            host.receiveChat(e.id, e.text);
            break;
          case 'playerDied':
            host.playerDied(e.id);
            break;
          case 'refreshall':
            await host.refreshAll();
            publishBundles();
            break;
          case 'ensure':
            await host.ensure(e.name);
            publishBundles();
            break;
          case 'start':
            await host.start(e.name);
            publishBundles();
            break;
          case 'stop':
            await host.stop(e.name);
            publishBundles();
            break;
          case 'refresh':
            host.refresh();
            break;
          case 'serverStopping':
            host.crashReports.stopping();
            break;
          default:
            throw new Error('Unknown IPC operation');
        }
      })
      .catch((e) => send({ op: 'log', resource: 'host', text: e.stack ?? String(e) }));
  })
  .on('close', () => {
    chain.finally(async () => {
      try {
        host.crashReports.closed(host.adminLog.lines);
      } catch (error) {
        send({ op: 'log', resource: 'host', text: `crash report not written: ${error.message}` });
      }
      await host.shutdown();
      process.exit(0);
    });
  });
// A Ctrl+C in a terminal and a service manager's stop signal (systemd sends it to every process of
// the service) reach the host too. The native server stops it in order (serverStopping, then the
// input closes), so the host waits for that instead of ending at once: resources stop as usual and
// no false crash report is written. A native server that is gone closes the input all the same.
for (const signal of ['SIGINT', 'SIGTERM']) {
  process.on(signal, () => {});
}
process.on('unhandledRejection', (error) => {
  const owner = host.errorOwner(error);
  if (owner) host.failResource(owner, error);
  else send({ op: 'log', resource: 'host', text: 'Unhandled rejection: ' + String(error) });
});
