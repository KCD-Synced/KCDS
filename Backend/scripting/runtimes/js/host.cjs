'use strict';
const fs = require('node:fs'),
  path = require('node:path'),
  vm = require('node:vm');
const { AsyncLocalStorage } = require('node:async_hooks');
const { ResourceServices, describeError } = require('./services.cjs');
const { withNamespace } = require('./sdk_namespace.cjs');
const { EntityStateStore } = require('./shared_state.cjs');
const { resourceTimers } = require('./timers.cjs');
const { resourceWeb } = require('./web/resource_web.cjs');
const { HttpRoutes } = require('./web/http_routes.cjs');
const { StatusRoutes } = require('./web/status_routes.cjs');
const { RealmLinks } = require('./realms.cjs');
const { ServerPassword } = require('./admission/server_password.cjs');
const { ServerBans } = require('./admission/server_bans.cjs');
const { BansApi } = require('./admission/bans_api.cjs');
const { AdminLog } = require('./admin/admin_log.cjs');
const { AdminServer } = require('./admin/admin_server.cjs');
const { ServerSchedule } = require('./admin/server_schedule.cjs');
const { ServerBackups } = require('./admin/server_backups.cjs');
const { CrashReports } = require('./admin/crash_reports.cjs');
const { ServerUpdates } = require('./admin/server_updates.cjs');
const { createRequire } = require('node:module');
const { normalizeManifest, scriptEntries } = require('./manifest.cjs');
const { LuaResource } = require('./languages/lua_host.cjs');
const { CSharpResource } = require('./languages/csharp_host.cjs');
const { Vector2, Vector3, Vector4, Quaternion, Color, vector, rotation } = require('./math.cjs');
const {
  Appearances,
  appearanceFrom,
  defaultAppearance,
  resolveRequest,
} = require('./appearance/appearances.cjs');
const { resolveMesh } = require('./props/catalog.cjs');
const { classBytesHex, isClothing, lookupItemClass } = require('./inventory/catalog.cjs');
// Pieces a player wears outside their inventory at once (Player.wearItem).
const WORN_MAX = 8;
const { Carries } = require('./carry/player_carry.cjs');
const {
  PlayerStat,
  DerivedPlayerStat,
  NO_STATS,
  statsFrom,
  defineReadings,
  MAX_STAT_VALUE,
  getStat,
  derivedStat,
  scaledStats,
  statChanges,
  statRequest,
  statChangeRequest,
  fillRequest,
} = require('./stats/player_stats.cjs');
const { Needs } = require('./needs/needs.cjs');
const { postureFromNative, definePostureReadings } = require('./posture/player_posture.cjs');
const {
  progressionCatalog,
  TRACKS,
  lookupPerk,
  totalXp,
  addPerkTablePatch,
} = require('./progression/catalog.cjs');
const {
  ProgressionRules,
  MAX_MULTIPLIER,
  perkScreenPerk,
} = require('./progression/progression_rules.cjs');
const { TAG_BY_NAME, buffCatalog, claimableClassIds, lookupBuff } = require('./buffs/catalog.cjs');
const { CustomBuffs, MODIFIERS: BUFF_MODIFIERS } = require('./buffs/custom_buffs.cjs');
const {
  applyReport,
  buffStates,
  buffsFromReport,
  hasBuff,
  injuriesOf,
  isKnockOutBuff,
  injuryChanges,
} = require('./buffs/player_buffs.cjs');
const { PlayerInventories } = require('./inventory/player_inventories.cjs');
const { GroundItems } = require('./ground/ground_items.cjs');
const { Props } = require('./props/props.cjs');
const { PropPreviews } = require('./props/previews.cjs');
const { SiegeEngines } = require('./siege/engines.cjs');
const { SiegeLadders } = require('./siege/ladders.cjs');
const { StonePiles } = require('./siege/stones.cjs');
const { Doors } = require('./doors/doors.cjs');
const { Carts } = require('./carts/carts.cjs');
const { Vfxs } = require('./vfx/vfx.cjs');
const { ServerAudio } = require('./audio/server_audio.cjs');
const { Lights } = require('./lights/lights.cjs');
const { SmartObjects } = require('./smart_objects/smart_objects.cjs');
const { Gizmos } = require('./gizmos/gizmos.cjs');
const { LevelEdits } = require('./level/level_edits.cjs');
const { Gates } = require('./gates/gates.cjs');
const { Graves } = require('./graves/graves.cjs');
const { Tabletops } = require('./tabletop/tabletops.cjs');
const { WorldResources } = require('./level/world_resources.cjs');
const { WorldPickables } = require('./ground/world_pickables.cjs');
const { Stashes } = require('./stash/stashes.cjs');
const { Beehives } = require('./stash/beehives.cjs');
const { Pickpocketing } = require('./pickpocket/pickpocketing.cjs');
const { Locales } = require('./locales/locales.cjs');
const { BodyCarries } = require('./carry/body_carries.cjs');
const { PlayerAnimations } = require('./animation/player_animation.cjs');
const {
  poseAnimationOf,
  poseAnimationReference,
  propOf,
  PROP_SETTINGS,
} = require('./animation/pose_animation.cjs');
const { StudioAnimations } = require('./animation/studio_animations.cjs');
const { Pillories } = require('./pillory/pillory.cjs');
const { Areas } = require('./areas/areas.cjs');
const { Blips } = require('./markers/blips.cjs');
const { Markers } = require('./markers/markers.cjs');
const { Presence } = require('./markers/presence.cjs');
const { Quests } = require('./quests/quests.cjs');
const { Cuffs } = require('./cuffs/cuffs.cjs');
const { FrozenPlayers } = require('./frozen/frozen_players.cjs');
const { PlayerAttachments } = require('./player_attach/player_attach.cjs');
const { NoClipRules } = require('./noclip/noclip_rules.cjs');
const { InteractionRules } = require('./interaction_rules/interaction_rules.cjs');
const { Whistles } = require('./whistles/whistles.cjs');
const { WorldState } = require('./world/world.cjs');
const { WorldInteractions } = require('./interactions/world_interactions.cjs');
const { Freecam } = require('./camera/freecam.cjs');
const { ThirdPerson } = require('./camera/third_person.cjs');
const { CameraShots } = require('./camera/shots.cjs');
const { VoiceChat } = require('./voice/voice.cjs');
const { Dialogues } = require('./dialogue/dialogues.cjs');
const { Vendors } = require('./vendors/vendors.cjs');
const { WorldTraces } = require('./world/traces.cjs');
const { grimeFrom, cleanOrder, soilOrder } = require('./appearance/grime.cjs');
const { COVER_ITEM, needsCover } = require('./appearance/legs_cover.cjs');
const { CombatMarker } = require('./hud/combat_marker.cjs');
const { RestrictedAreaMarker } = require('./hud/restricted_area_marker.cjs');
const { PlayerNameplates } = require('./nameplates/player_nameplates.cjs');
const { JoinQueue } = require('./queue/join_queue.cjs');
const { LicenceGate } = require('./licence/licence_gate.cjs');
const animationCatalog = require('./animation/catalog.cjs');
const { PlaceNames } = require('./world/place_names.cjs');
const { CustomItems } = require('./inventory/custom_items.cjs');
const { lookupClassHex } = require('./inventory/catalog.cjs');
const { ItemOverrides } = require('./inventory/item_overrides.cjs');
const { HorseOwners, OWNER_ABSENCE_MS } = require('./horses/horse_owners.cjs');
const {
  HorseConfig,
  checkCarryWeight,
  checkHorseStat,
  MAX_STAMINA,
  MIN_SPEED,
  MAX_SPEED,
  MAX_HIT_POINTS,
} = require('./horses/horse_config.cjs');
const {
  HorseLooks,
  randomLook,
  lookOf,
  lookField,
  isGameHorseSoul,
} = require('./horses/looks.cjs');
const {
  SLOTS: HORSE_GEAR_SLOTS,
  checkTorso,
  defaultGear,
  gearList,
  gearOf,
  gearSets,
  pieceNames,
  pieceOf,
  piecesOf,
  slotOfItem,
} = require('./horses/gear.cjs');
const { metadataOf } = require('./inventory/inventories.cjs');
const { Npcs } = require('./npcs/npcs.cjs');
const { Dogs } = require('./dogs/dogs.cjs');
const { HorseOrders } = require('./horses/orders.cjs');
const { HorseBuffs } = require('./horses/horse_buffs.cjs');
const {
  HorseHealth,
  MAX_HEALTH: HORSE_MAX_HEALTH,
  DEAD,
  KNOCKED_OUT,
} = require('./horses/health.cjs');
const { damageOf } = require('./combat/player_damage.cjs');
const { RULING, playerHitOf } = require('./combat/player_hit.cjs');
const {
  progressionFromReport,
  levelsOf,
  perkNames,
  hasPerk,
  trackProgress,
  snapshotOf,
  progressionChanges,
} = require('./progression/player_progression.cjs');
const {
  addXpOrder,
  setLevelOrder,
  setXpOrder,
  removeXpOrder,
  perkOrder,
  addPointsOrder,
  respecOrder,
  restoreOrder,
  withNotice,
} = require('./progression/progression_orders.cjs');
const recipes = require('./recipes/recipes.cjs');
const { CraftingSessions } = require('./crafting/crafting.cjs');
const { FoodPreparing } = require('./preparing/preparing.cjs');
const { CookPots } = require('./cookpot/cookpots.cjs');
const { DiceMatches } = require('./dice/dice.cjs');
const { Gathering } = require('./gathering/gathering.cjs');
const { Trees } = require('./trees/trees.cjs');
const { CustomCrafting } = require('./crafting/custom.cjs');
// Trusted local resources run CommonJS in separate contexts. This is not a security sandbox.
// Last authoritative position reported by the native server for a Player handle.
const playerPosition = Symbol('kcds.playerPosition');
// Last authoritative rotation (a Quaternion) reported by the native server for a Player handle.
const playerRotation = Symbol('kcds.playerRotation');
// Last authoritative dimension, and the newest unconfirmed setDimension request.
const { People } = require('./npcs/people.cjs');
const { landmarks } = require('./world/points_of_interest.cjs');
const { replicasAround } = require('./world/replicas.cjs');
// How near a horse a player must stand to pull its rider down (metres).
const PULL_DOWN_REACH = 5;
const playerWorld = Symbol('kcds.playerWorld');
// A horse's look (horses/looks.cjs): the components every client builds its body from.
const horseLook = Symbol('kcds.horseLook');
// A horse's tack, { saddle, head, torso, shoe } (horses/gear.cjs), and whether players'
// own changes to it are refused (gearFixed).
const horseGear = Symbol('kcds.horseGear');
const horseGearLocked = Symbol('kcds.horseGearLocked');
// By slot, the metadata of the row a worn piece was put on from (null for a script's piece), and
// the names its pieces show under their own keys (horses/gear.cjs pieceNames).
const horseGearRows = Symbol('kcds.horseGearRows');
const horseGearNames = Symbol('kcds.horseGearNames');
const horseName = Symbol('kcds.horseName');
// The rider horseMounting refused, told to get off; their ride raises no events.
const horseRefusedRider = Symbol('kcds.horseRefusedRider');
// The most a horse's saddlebags carry, kg, or null for the game's own.
const horseCarry = Symbol('kcds.horseCarry');
// A horse's stamina, speed and hit points: a breed's or a resource's, null the game's.
const horseStats = Symbol('kcds.horseStats');
// The breed a horse was spawned as, or null.
const horseBreed = Symbol('kcds.horseBreed');
// A horse's saddlebags: the Stash the server holds for them.
const horseSaddlebags = Symbol('kcds.horseSaddlebags');
// Whether a lethal blow only knocks the horse out instead of killing it (a KCDS addition).
const horseKnockOut = Symbol('kcds.horseKnockOut');
// Whether a horse is removed when its owner has left (Horse.removedWithOwner).
const horseDestroyWithOwner = Symbol('kcds.horseDestroyWithOwner');
// A horse's order state, { sequence, status } (horses/orders.cjs).
const horseOrder = Symbol('kcds.horseOrder');
// A horse's health ledger, { health, state } (horses/health.cjs).
const horseLedger = Symbol('kcds.horseLedger');
// The stamina the horse's simulator last reported (HORSE_STAMINA), null before one.
const horseStamina = Symbol('kcds.horseStamina');
// The players besides its owner allowed to ride it (Horse.allowRider): a Set of player ids.
const horseRiders = Symbol('kcds.horseRiders');
// The most players a horse lets ride besides its owner (horse.hpp kMaxHorseRiders).
const MAX_HORSE_RIDERS = 32;
// Horse.dirt: how dirty its coat and tack are, set by a script.
const horseDirt = Symbol('kcds.horseDirt');
// Horse.speedScale: the share of its pace it moves at, set by a script.
const horseSpeedScale = Symbol('kcds.horseSpeedScale');
const SPEED_SCALE_MIN = 0.1;
const SPEED_SCALE_MAX = 2;
// How long player.opponents remembers a swing between two players.
const FIGHT_MEMORY_MS = 15000;
const pendingWorldRequest = Symbol('kcds.pendingWorldRequest');
// Set once the player's client took its loading screen down and playerReady was raised.
const playerPlaying = Symbol('kcds.playerPlaying');
// Set while the player lies knocked out after a lethal hit (playerDied) until revive() is sent.
const playerDown = Symbol('kcds.playerDown');
// The last health a player's game took off them: { attacker id, reason, at (ms) }, which names the
// killer and the reason of the knock-out it causes (playerDied).
const playerLastDamage = Symbol('kcds.playerLastDamage');
// How long before a knock-out its cause may have come, in ms.
const KILLING_DAMAGE_MS = 5000;
// The longest perk name, and the longest description or lore text.
const PERK_NAME_BYTES = 128;
const PERK_TEXT_BYTES = 4096;
// Whether others may search the player's body while they are knocked out.
const playerLootable = Symbol('kcds.playerLootable');
// What the player's body looks like, as its own client last published it.
const playerAppearance = Symbol('kcds.playerAppearance');
const playerLegsCover = Symbol('playerLegsCover');
// Whether the player's own client draws its body, as it last published it.
const playerVisible = Symbol('kcds.playerVisible');
const playerVisorClosed = Symbol('kcds.playerVisorClosed');
// A physical prop's options checked (Player.setPhysicalProp;
// kcd2s/integration/physical_prop.hpp): { kind, segments, length } with the kind's defaults.
const PHYSICAL_PROP_DEFAULTS = Object.freeze({
  rope: Object.freeze({ segments: 16, length: 1.2 }),
  whip: Object.freeze({ segments: 20, length: 1.6 }),
  // A rope drawn with the game's chain links.
  chain: Object.freeze({ segments: 24, length: 1.5 }),
});
// The prop's settings as the studio starts them: the field carries only the ones
// that differ.
const PHYSICAL_PROP_SETTINGS = Object.freeze({
  hand: 'RightHand',
  pitch: 90,
  twist: 0,
  gravity: 9.8,
  damping: 0.3,
  mass: 0.5,
  air: 0.1,
  bend: 0,
  stiffness: 0,
  collide: 1,
  shape: 0,
  curl: 0,
  sway: 0,
});
function physicalPropOf(options) {
  if (typeof options !== 'object' || options === null) {
    throw new TypeError(
      'Player.setPhysicalProp takes { kind, segments?, length?, hand?, grip?, physics? } or null',
    );
  }
  const defaults = PHYSICAL_PROP_DEFAULTS[options.kind];
  if (!defaults) {
    throw new TypeError("Player.setPhysicalProp: kind is 'rope', 'whip' or 'chain'");
  }
  const segments = options.segments ?? defaults.segments;
  const length = options.length ?? defaults.length;
  if (!Number.isInteger(segments) || segments < 2 || segments > 64) {
    throw new RangeError('Player.setPhysicalProp: segments is a whole number from 2 to 64');
  }
  if (typeof length !== 'number' || !(length >= 0.2 && length <= 20)) {
    throw new RangeError('Player.setPhysicalProp: length is 0.2 to 20 metres');
  }
  for (const part of ['grip', 'physics']) {
    const given = options[part];
    if (
      given !== undefined &&
      (given === null || typeof given !== 'object' || Array.isArray(given))
    ) {
      throw new TypeError(`Player.setPhysicalProp: ${part} is an object`);
    }
  }
  const { pitch, twist, ...unknownGrip } = options.grip ?? {};
  if (Object.keys(unknownGrip).length > 0) {
    throw new TypeError('Player.setPhysicalProp: grip is { pitch?, twist? }');
  }
  const physics = options.physics ?? {};
  if ('hand' in physics || 'pitch' in physics || 'twist' in physics) {
    throw new TypeError('Player.setPhysicalProp: physics is { gravity?, damping?, mass?, ... }');
  }
  const given = { ...physics };
  if (options.hand !== undefined) {
    given.hand = options.hand;
  }
  if (pitch !== undefined) {
    given.pitch = pitch;
  }
  if (twist !== undefined) {
    given.twist = twist;
  }
  const settings = { ...PHYSICAL_PROP_SETTINGS, ...propOf(given, 'Player.setPhysicalProp') };
  return {
    kind: options.kind,
    segments,
    length: Math.round(length * 100) / 100,
    hand: settings.hand,
    grip: { pitch: settings.pitch, twist: settings.twist },
    physics: {
      gravity: settings.gravity,
      damping: settings.damping,
      mass: settings.mass,
      air: settings.air,
      bend: settings.bend,
      stiffness: settings.stiffness,
      collide: settings.collide,
      shape: settings.shape,
      curl: settings.curl,
      sway: settings.sway,
    },
  };
}

// The field's text of a checked prop (kcd2s/integration/physical_prop.hpp): "whip 20 1.60",
// then " <code>=<value>" for each setting that differs from the studio's first one.
function physicalPropField(prop) {
  const settings = { hand: prop.hand, ...prop.grip, ...prop.physics };
  let field = `${prop.kind} ${prop.segments} ${prop.length.toFixed(2)}`;
  for (const name of Object.keys(PHYSICAL_PROP_SETTINGS)) {
    if (settings[name] === PHYSICAL_PROP_SETTINGS[name]) {
      continue;
    }
    const setting = PROP_SETTINGS[name];
    const value = setting.hands ? setting.hands.indexOf(settings[name]) : settings[name];
    field += ` ${setting.code}=${Number(value.toPrecision(6))}`;
  }
  return field;
}

// What a player holds as playAnimation props (props: 'held'): each hand's item class,
// in the game's grip; empty hands give none.
function heldProps(player) {
  const props = [];
  for (const [hand, hex] of [
    ['right', player.rightHandItem],
    ['left', player.leftHandItem],
  ]) {
    const itemClass = hex ? lookupClassHex(hex) : undefined;
    if (itemClass) {
      props.push({ item: itemClass.id, hand, attach: 'game' });
    }
  }
  return props;
}

// The mesh the player's body is drawn as, as its own client last published it, or null.
const playerDisguise = Symbol('kcds.playerDisguise');
// A placement a playerSpawning handler chose for a player still loading.
const pendingSpawn = Symbol('kcds.pendingSpawn');
// The player's stats as its own client last published them.
const playerStats = Symbol('kcds.playerStats');
// The player's progression as its own client last reported it.
const playerProgression = Symbol('kcds.playerProgression');
// Perks this server added or took away and the player's report has not shown yet: their
// playerPerkAdded / playerPerkRemoved name the server as the source.
const serverPerkOrders = Symbol('kcds.serverPerkOrders');
// The status effects on the player's body as their client last reported them, and
// the buffs this server asked to take off, so playerBuffRemoved names the server as the reason.
const playerBuffs = Symbol('kcds.playerBuffs');
// The item classes the player's body wears and holds, as their client reports them.
const playerEquipment = Symbol('kcds.playerEquipment');
const playerHands = Symbol('kcds.playerHands');
const playerLanguage = Symbol('kcds.playerLanguage');
const playerGrime = Symbol('kcds.playerGrime');
// Whether the player sits on a seat (a chair, a bench, a dice table's chair), as their game reports.
const playerSeated = Symbol('kcds.playerSeated');
// The stance tag the player's own game chose for their body (posture.hpp bits 48-55), 255 none.
const playerStance = Symbol('kcds.playerStance');
// A position a script moved the player to, until their own game reports the body there:
// { at: Vector3, until: ms } (playerTeleported).
const playerTeleport = Symbol('kcds.playerTeleport');
// How near the target a report must be, and for how long the server waits for one.
const TELEPORT_ARRIVED_M = 3;
const TELEPORT_WAIT_MS = 15000;
// How the player moves and looks: the velocity its own client measured from its
// positions and where its head turns, as Vector3s.
const playerVelocity = Symbol('kcds.playerVelocity');
const playerLookDirection = Symbol('kcds.playerLookDirection');
// How the player's body stands: off the ground, crouched, its physics profile.
const playerPosture = Symbol('kcds.playerPosture');
const serverBuffRemovals = Symbol('kcds.serverBuffRemovals');
// playerBuffBlocked fires at most this often per player and buff.
const BUFF_BLOCKED_PER_SECOND = 2;
// The side of the square a CryEngine level lies in, in metres.
const WORLD_EXTENT = 16384;
// The longest kick reason a player's client is told (SESSION_END), in UTF-8 bytes.
const KICK_REASON_MAX_BYTES = 200;
// The reserved client event of the server's player count (Discord presence).
const PLAYER_COUNT_EVENT = 'kcds:playerCount';
// An animation defined by name that is longer than the player field takes: every
// client keeps it by its name ({ name, field }, '' field: it went); scripts never see the event.
const POSE_ANIMATION_EVENT = 'kcds:poseAnimation';
// The most client events held for a player whose client resources do not run yet.
const MAX_HELD_CLIENT_EVENTS = 512;
// The argument of playerConnecting: reject(reason) shows the player 512 bytes of text at most, update(message)
// a line of up to 256.
const REJECT_TEXT_BYTES = 512;
const STATUS_TEXT_BYTES = 256;
// A player's identity (player.steamId and its kin): store and storeAccount from the Master's
// verified licence pass (steamId when the store is Steam, epicId when it is Epic),
// discordId from the pass when the player proved the Discord account theirs (discordVerified),
// else as the client reported it, deviceId as the client reported it, ip the
// address the server saw the connection come from. Empty strings
// when unknown.
const NO_IDENTITY = Object.freeze({
  steamId: '',
  epicId: '',
  store: '',
  storeAccount: '',
  discordId: '',
  discordVerified: false,
  deviceId: '',
  ip: '',
});
const MAX_ADMITTED_IDENTITIES = 4096;
// What a player without a verified Discord account reads on a server that asks for one.
const DISCORD_REFUSAL = 'This server asks for a verified Discord account.';

function playerIdentityOf(connection) {
  return Object.freeze({
    steamId: connection.store === 'steam' ? connection.storeAccount : '',
    epicId: connection.store === 'epic' ? connection.storeAccount : '',
    store: connection.store,
    storeAccount: connection.storeAccount,
    discordId: connection.discordId,
    discordVerified: connection.discordVerified,
    deviceId: connection.deviceId,
    ip: connection.ip,
  });
}

// The identifier kinds a player may have, and whether the server verified each:
// the store account comes from the Master's licence pass (the store checked the proof), the ip is
// where the server saw the connection come from; the Discord id is verified when the pass carries
// it (the player proved the account theirs through the Master), else what the client reported;
// the hardware id (a SHA-256 of the machine's ids) is what the player's client reported.
const STORE_KINDS = Object.freeze(['steam', 'epic', 'xbox', 'gog']);
const IDENTIFIER_KINDS = Object.freeze([...STORE_KINDS, 'discord', 'hardware', 'ip']);

// The identifiers of an identity, the ones it has: [{ kind, id, verified }], frozen.
function identifiersOf(identity) {
  const identifiers = [];
  const add = (kind, id, verified) => {
    if (typeof id === 'string' && id !== '') {
      identifiers.push(Object.freeze({ kind, id, verified }));
    }
  };
  if (STORE_KINDS.includes(identity.store)) {
    add(identity.store, identity.storeAccount, true);
  }
  add('discord', identity.discordId, identity.discordVerified === true);
  add('hardware', identity.deviceId, false);
  add('ip', identity.ip, true);
  return Object.freeze(identifiers);
}

// The id of one kind among an identity's identifiers, or ''.
function identifierOf(identity, kind) {
  if (typeof kind !== 'string' || !IDENTIFIER_KINDS.includes(kind)) {
    throw new TypeError(`getIdentifier takes one of ${IDENTIFIER_KINDS.join(', ')}`);
  }
  const found = identifiersOf(identity).find((identifier) => identifier.kind === kind);
  return found ? found.id : '';
}

class JoinRequest {
  #reject;
  #update;
  #pending;
  // Store identity comes from the verified licence pass; nickname remains client supplied.
  constructor(reject, identity = {}, update = () => {}, pending = () => true) {
    this.#reject = reject;
    this.#update = update;
    this.#pending = pending;
    this.nickname = typeof identity.nickname === 'string' ? identity.nickname : '';
    this.steamId = typeof identity.steamId === 'string' ? identity.steamId : '';
    // The Epic Games account of a player who owns the game there, from the same pass.
    this.epicId = typeof identity.epicId === 'string' ? identity.epicId : '';
    this.store = typeof identity.store === 'string' ? identity.store : '';
    this.storeAccount = typeof identity.storeAccount === 'string' ? identity.storeAccount : '';
    // From the licence pass when the player proved the Discord account theirs (discordVerified),
    // else reported by the client.
    this.discordId = typeof identity.discordId === 'string' ? identity.discordId : '';
    this.discordVerified = identity.discordVerified === true;
    this.deviceId = typeof identity.deviceId === 'string' ? identity.deviceId : '';
    this.ip = typeof identity.ip === 'string' ? identity.ip : '';
    // The KCDS Master's join pass the player's game presented, already checked by the server
    // a resource may hand it to its own backend, which checks it with the Master's
    // public key. It names this server and this connection only.
    this.ticket = typeof identity.licence === 'string' ? identity.licence : '';
    // What the player's launcher gave this join with kcds-launcher --join-token,
    // such as a one-time ticket of a community's own launcher; '' without one. Unverified: the
    // resource checks it, the game sends it only to the server the launcher joined directly.
    this.joinToken = typeof identity.joinToken === 'string' ? identity.joinToken : '';
  }
  // A line of status for the waiting player's connecting screen (at most 256 bytes), and a new
  // admission timeout, so a handler that keeps updating is never timed out.
  update(message) {
    if (typeof message !== 'string') {
      throw new TypeError('update takes a message text');
    }
    this.#update(refusalText(message, STATUS_TEXT_BYTES));
  }
  // Whether the connection still waits on a decision.
  isPending() {
    return this.#pending();
  }
  // Every identifier the joining player has, [{ kind, id, verified }].
  get identifiers() {
    return identifiersOf(this);
  }
  // The id of one kind ('steam', 'epic', 'xbox', 'gog', 'discord', 'hardware', 'ip'), or ''.
  getIdentifier(kind) {
    return identifierOf(this, kind);
  }
  // Turns the player away at once, even while other handlers are still running. The reason is shown to
  // the player as plain text, cut to 512 bytes (the native host cuts the same way).
  reject(reason) {
    if (reason !== undefined && typeof reason !== 'string') {
      throw new TypeError('reject reason must be a string');
    }
    this.#reject(refusalText(reason ?? '', REJECT_TEXT_BYTES));
  }
}
// The text cut to `limit` bytes of UTF-8 without splitting a character.
function refusalText(reason, limit) {
  let text = '';
  let bytes = 0;
  for (const character of reason) {
    const size = Buffer.byteLength(character);
    if (bytes + size > limit) {
      break;
    }
    text += character;
    bytes += size;
  }
  return text;
}
const GLOBAL_WORLD = 0xffffffff;
function validWorld(world) {
  return Number.isSafeInteger(world) && world >= 0 && world <= GLOBAL_WORLD;
}
// The `process` a server resource sees: what Node packages read of it (env, platform,
// versions, nextTick, hrtime, ...), never exit, abort or the host's own event handlers. nextTick
// runs the callback as the resource's own microtask, so a stopped resource's ticks do nothing.
function resourceProcess(folder, logger, queueMicrotask) {
  const hrtime = (previous) => process.hrtime(previous);
  hrtime.bigint = () => process.hrtime.bigint();
  // env, versions and release are getters: the docs extractor lists data, never their contents
  // (the server's environment).
  return Object.freeze({
    get env() {
      return process.env;
    },
    platform: process.platform,
    arch: process.arch,
    version: process.version,
    get versions() {
      return process.versions;
    },
    get release() {
      return process.release;
    },
    pid: process.pid,
    argv: Object.freeze([process.execPath]),
    execArgv: Object.freeze([]),
    title: 'kcds-resource',
    cwd: () => folder,
    uptime: () => process.uptime(),
    memoryUsage: () => process.memoryUsage(),
    cpuUsage: (previous) => process.cpuUsage(previous),
    hrtime,
    nextTick: (callback, ...args) => queueMicrotask(() => callback(...args)),
    emitWarning: (warning) => logger.warn(String(warning?.message ?? warning)),
  });
}

class ResourceHost {
  constructor(root, bridge = { send() {}, log() {} }) {
    // A player's client events wait while their client resources do not run yet: the game
    // reports the player ready while it may still download them, and an event sent then was lost.
    // Player ids whose client resources run, and the events held for the others the server knows.
    this.clientResourcesRunning = new Set();
    this.heldClientEvents = new Map();
    // KCDS-Admin's console: every line the host and the resources log.
    this.adminLog = new AdminLog();
    const forward = bridge.log;
    bridge.log = (resource, text) => {
      this.adminLog.push(resource, text);
      forward.call(bridge, resource, text);
    };
    // The resources' HTTP routes: one listener, only when the server owner gives a port.
    this.httpRoutes = new HttpRoutes({
      port: bridge.httpPort,
      bind: bridge.httpBind,
      log: (text) => bridge.log('host', text),
    });
    // The password a join must bring (KCDS_SERVER_PASSWORD); none without one.
    this.serverPassword = new ServerPassword(bridge.serverPassword, (text) =>
      bridge.log('host', text),
    );
    // Only players whose pass carries a verified Discord account get in (requireDiscord,
    // KCDS_REQUIRE_DISCORD).
    this.requireDiscord = bridge.requireDiscord === true;
    // The server's status on the same listener: info, dynamic and the player list.
    this.statusRoutes = new StatusRoutes(this);
    this.httpRoutes.status = this.statusRoutes;
    // The server's rules on progression: XP from play, rates, budgets, caps, the ledger.
    this.progressionRules = new ProgressionRules();
    // Buffs: the clock `since` counts on, the claimed classes (Buffs.claim) and the recent
    // playerBuffBlocked of each player and buff.
    this.buffClock = () => performance.now() / 1000;
    this.claimedBuffClasses = [];
    this.buffBlocks = new Map();
    // The buffs resources define (Buffs.register) and their one schedule.
    this.customBuffs = new CustomBuffs(this);
    // The players' inventories: the server holds them, their games show them.
    this.playerInventories = new PlayerInventories(this, (player, inventory) => {
      this.projectInventory(player, inventory);
      this.stashes.bodyChanged(player);
    });
    this.playerInventories.sendNames = (id, full, names) =>
      this.bridge.send({ op: 'itemNames', id, full, names });
    // What the game calls places (World.setPlaceName).
    this.placeNames = new PlaceNames(this.playerInventories);
    // Item types resources register (Items).
    this.customItems = new CustomItems(this);
    this.root = path.resolve(root);
    // The server's own bans (bans.json) and its KCDS-Admin (off unless configured).
    this.serverBans = new ServerBans(this.root, (text) => bridge.log('host', text));
    // The same bans for scripts: Bans and player.ban, with a reason and no end.
    this.bansApi = new BansApi(this);
    // Backups of the server's data: from KCDS-Admin and before a planned stop.
    this.serverBackups = new ServerBackups({
      root: this.root,
      configFile: bridge.serverConfig,
      log: (text) => bridge.log('host', text),
    });
    // Reports of a server that ended without stopping in order, for KCDS-Admin;
    // the server's own start (main.cjs) begins them.
    this.crashReports = new CrashReports({
      root: this.root,
      log: (text) => bridge.log('host', text),
    });
    // Updates of the server from the KCDS update feed, for KCDS-Admin. A server
    // package keeps this host in runtime/Backend/scripting/runtimes/js, its binary five folders up.
    this.serverUpdates = new ServerUpdates({
      install: path.resolve(__dirname, '..', '..', '..', '..', '..'),
      version: () => this.statusRoutes.settings.version,
      log: (text) => bridge.log('host', text),
    });
    // Stops and restarts with warnings: KCDS-Admin's and KCDS_RESTART_TIMES.
    this.serverSchedule = new ServerSchedule(this, {
      times: bridge.restartTimes,
      log: (text) => bridge.log('host', text),
    });
    this.adminServer = new AdminServer(this, {
      port: bridge.adminPort,
      bind: bridge.adminBind,
      password: bridge.adminPassword,
      tlsCert: bridge.adminTlsCert,
      tlsKey: bridge.adminTlsKey,
      root: this.root,
      configFile: bridge.serverConfig,
      log: (text) => bridge.log('host', text),
      consoleCommand: (text) => (this.consoleCommand ? this.consoleCommand(text) : undefined),
    });
    this.bridge = bridge;
    // The weights and prices the server gives the game's own items.
    this.itemOverrides = new ItemOverrides(this);
    this.itemOverrides.load(this.root);
    // The server's horse config (horse-config.json, 0.2.4).
    this.horseConfig = new HorseConfig((text) => this.bridge.log('host', text));
    this.horseConfig.load(this.root);
    // A full server's join queue and the whitelist (Queue).
    this.joinQueue = new JoinQueue(this);
    this.joinQueue.loadWhitelist(this.root);
    // Only a legally owned game joins (licence/licence_gate.cjs, licence.json).
    this.licence = new LicenceGate(this);
    // Who each admitted request and then each player is (playerConnecting's identity): request ->
    // identity until the player exists (playerAdmitted), then player id -> identity.
    this.admittedIdentities = new Map();
    this.playerIdentities = new Map();
    this.licence.load(this.root);
    this.context = new AsyncLocalStorage();
    this.realmLinks = new RealmLinks();
    this.resources = new Map();
    this.handlers = new Map();
    this.players = new Map();
    // Each player's round-trip time in milliseconds (player.ping).
    this.pings = new Map();
    this.horses = new Map();
    // The players' horses: thefts, and the removal of a horse whose owner left.
    this.horseOwners = new HorseOwners({
      delayMs: bridge.horseOwnerAbsenceMs ?? OWNER_ABSENCE_MS,
      connected: (id) => this.players.has(id),
      remove: (horse) => horse.destroy(),
      release: (horse) => horse.giveTo(null),
      log: (text) => this.bridge.log('host', text),
    });
    // NPCs resources spawn, which players' clients simulate (Npc).
    this.npcs = new Npcs(this);
    // The players' dogs, which ride on the NPCs.
    this.dogs = new Dogs(this);
    this.horseOrders = new HorseOrders(this, horseOrder);
    // The health, death and knock-out of the horses.
    this.horseHealth = new HorseHealth(this, horseLedger);
    // The game's buffs the server puts on horses.
    this.horseBuffs = new HorseBuffs(this);
    // Stacks lying in the world (GroundItem).
    this.groundItems = new GroundItems(this);
    // Props the server puts in the world (Prop).
    this.props = new Props(this);
    // Player id -> the pieces wearItem put on (class hex -> class).
    this.wornOnly = new Map();
    // Previews of props shown to one player: PropGhost and PropPlacer.
    this.propPreviews = new PropPreviews(this);
    // Trebuchets and cannons the server puts in the world (SiegeEngine).
    this.siege = new SiegeEngines(this);
    // The level's doors, whose state the server owns (Door).
    this.doors = new Doors(this);
    // Carts and wagons the server puts in the world (Cart).
    this.carts = new Carts(this);
    // Particle effects the server puts in the world (Vfx).
    this.vfx = new Vfxs(this);
    // The game's sounds played for the players near a point or a body (Audio).
    this.audio = new ServerAudio(this);
    // Point lights the server puts in the world (Light).
    this.lights = new Lights(this);
    // The level's stations, and the ones a level edit took out of a world (SmartObject).
    this.smartObjects = new SmartObjects(this);
    // Debug shapes drawn in the players' worlds (Gizmo).
    this.gizmos = new Gizmos(this);
    // Edits to the objects the level placed, and the world exports that bring them.
    this.levelEdits = new LevelEdits(this);
    // The level's animated gates, opened and closed for every player (Gate).
    this.gates = new Gates(this);
    // The level's graves, dug out and filled for every player (Grave).
    this.graves = new Graves(this);
    // Tabletop minigames: boards, pieces, seats and pointing (Tabletop).
    this.tabletops = new Tabletops(this);
    this.worldResources = new WorldResources(this);
    // The level's own items, taken up once.
    this.worldPickables = new WorldPickables(this);
    // The level's own items on or off (level-items.json).
    this.worldPickables.load(this.root);
    this.carries = new Carries(this);
    // Containers the server holds and knocked-out players' bodies (Stash).
    this.stashes = new Stashes(this);
    // Player.keyBypass.
    this.playerKeyBypass = new WeakMap();
    // Player.brand.
    this.playerBrands = new WeakMap();
    // Player.setPhysicalProp: the prop each player holds.
    this.physicalProps = new WeakMap();
    // Player.setPoseAnimation: the studio animation each player's body plays.
    this.poseAnimations = new WeakMap();
    // Studio animations by name (Animations.define, playAnimation(name)).
    this.studioAnimations = new StudioAnimations();
    // One longer than the field takes goes to every client whose resources run, by its name (the
    // reserved client event kcds:poseAnimation); a client whose resources start later gets them
    // all then (clientResourcesStarted).
    this.studioAnimations.onLarge = (name, field) => {
      for (const id of this.clientResourcesRunning) {
        this.services.emitToPlayer(id, POSE_ANIMATION_EVENT, { name, field });
      }
    };
    // Siege ladders and stone piles the server puts on a wall.
    this.siegeLadders = new SiegeLadders(this);
    this.stonePiles = new StonePiles(this);
    // The texts KCDS shows to players, per language.
    this.locales = new Locales();
    // Robbing another player with the game's own pickpocketing.
    this.pickpocketing = new Pickpocketing(this);
    // Whether players may open the game's photo mode, its free camera (a KCDS addition).
    this.freecam = new Freecam(this);
    // Whether the players' sleep and food needs run (Needs).
    this.needs = new Needs(this, (player) => Boolean(player[playerDown]));
    // Players carrying other players' knocked-out bodies.
    this.bodyCarries = new BodyCarries(this, (player) => Boolean(player[playerDown]));
    // Whether players see the game's combat marker (CombatMarker).
    this.combatMarker = new CombatMarker(this);
    // The animations resources make players' bodies play (playAnimation).
    this.playerAnimations = new PlayerAnimations(this);
    // Custom recipes and stations the server rules.
    this.customCrafting = new CustomCrafting(this);
    // The stew pots' stock, per world (CookPot).
    this.cookPots = new CookPots(this);
    // Dice matches between two players (Dice).
    this.dice = new DiceMatches(this);
    // Herb gathering and regrowth (Gathering).
    this.gathering = new Gathering(this);
    // Felling the level's trees (Trees).
    this.trees = new Trees(this);
    // Who works at which crafting station and whether they may, and how
    // their batches and workpieces settle.
    this.crafting = new CraftingSessions({
      emit: (name, ...args) => this.emit(name, ...args),
      sendVerdict: (player, request, allowed) =>
        this.bridge.send({ op: 'craftVerdict', id: player.id, request, allowed }),
      give: (player, units) => this.customCrafting.give(player, units),
      remove: (player, units) => {
        const result = this.playerInventories.operate(
          player,
          (inventory) => inventory.remove({ units }),
          { notify: true },
        );
        return Boolean(result?.ok);
      },
      connected: (player) => this.players.get(player.id) === player,
      // A recipe a resource registered: the resource, and its table patch for every
      // client.
      owner: () => this.context.getStore()?.name ?? null,
      sendRecipeTable: (table) => this.sendRecipeTable(table),
      // Whether a level edit took the station out of the world (SmartObject.remove).
      stationRemoved: (station, dimension) => this.smartObjects.removedIn(station, dimension),
      // The alchemy tables and smithies scripts placed (SmartObject.spawn).
      placedStations: (kind) => this.smartObjects.placedStations(kind),
      // Whether the player's server inventory holds these units ({ item, amount }), counted over
      // every row of the item.
      holds: (player, units) => {
        const snapshot = this.playerInventories.operate(player, (inventory) => ({
          result: inventory.snapshot(),
          change: null,
        }));
        if (!snapshot?.items) {
          return true;
        }
        return units.every(({ item, amount }) => {
          let held = 0;
          for (const row of snapshot.items) {
            if (row.item === item) held += row.amount;
          }
          return held >= amount;
        });
      },
      // A smithing recipe the player may forge: its perk owned and its minSkill reached, by the
      // player's last progression report (before the first one, the game's own screen decides).
      mayForge: (player, recipe) => {
        if (!player[playerProgression]) {
          return true;
        }
        return (
          recipe.perks.some((perk) => player.hasPerk(perk)) &&
          player.levels.craftsmanship >= recipe.minSkill
        );
      },
    });
    // Cooking, smoking and drying at the game's own fires and racks, asked of playerPreparing
    this.preparing = new FoodPreparing({
      emit: (name, ...args) => this.emit(name, ...args),
      send: (event) => this.bridge.send(event),
      vector,
      // A drying rack or smokehouse a level edit took out of the player's world.
      stationRemoved: (kind, position, dimension) => {
        const station = this.smartObjects.foodStationAt(kind, position);
        return station !== null && this.smartObjects.removedIn(station.id, dimension);
      },
      // The level's drying rack or smokehouse a preparing is at (Preparation.station).
      stationAt: (kind, position) => this.smartObjects.foodStationAt(kind, position)?.id ?? null,
    });
    // The script areas and who is inside them (Area).
    this.areas = new Areas(this);
    this.people = new People(this);
    // The ground markers and the map blips, and who they are shown to (Marker and Blip).
    this.mapPresence = new Presence(this);
    this.markers = new Markers(this, this.mapPresence);
    this.blips = new Blips(this, this.mapPresence);
    // The quests in the players' journals and which of them they follow (Quest).
    this.quests = new Quests(this, this.mapPresence);
    // The players put into a pillory (Player.putInPillory).
    this.pillories = new Pillories(this);
    // The handcuffed players (Player.setCuffed).
    this.cuffs = new Cuffs(this);
    // The frozen players (Player.frozen).
    this.frozenPlayers = new FrozenPlayers(this);
    // The players attached to others (Player.attachTo).
    this.playerAttachments = new PlayerAttachments(this);
    // The players who may not fly with NoClip (Player.noclipAllowed).
    this.noclipRules = new NoClipRules(this);
    // The game's own interactions the server allows (Interactions.set, Player.setInteraction).
    this.interactionRules = new InteractionRules(this);
    // The players' whistles for their horses (playerWhistle, Player.whistle).
    this.whistles = new Whistles(this);
    // The interactions resources put on things in the world (World.addInteraction).
    this.worldInteractions = new WorldInteractions(this);
    // The server's world time and weather, which every client follows (World).
    this.worldState = new WorldState(this);
    // Whether players see the game's restricted-area marker (RestrictedAreaMarker).
    this.restrictedAreaMarker = new RestrictedAreaMarker(this);
    // What each player's nameplate shows to the others (Player.setNameplate*).
    this.nameplates = new PlayerNameplates(this);
    // Whether players may switch to the third-person camera (ThirdPerson).
    this.thirdPerson = new ThirdPerson(this);
    // Scripted camera shots for one player (Camera.startShot).
    this.cameraShots = new CameraShots(this);
    // Voice chat's rules; the native relay forwards the frames.
    this.voiceChat = new VoiceChat(this);
    // Conversations the server drives on players in the game's dialogue list (Dialogue).
    this.dialogues = new Dialogues(this);
    // Vendors traded on the game's own shop screen (Vendor).
    this.vendors = new Vendors(this);
    // Rays a player's game traces for the server (World.raycast).
    this.worldTraces = new WorldTraces(this);
    // Network ids for entities a resource creates (horses), reserved by the native server so the
    // same id names the entity in server scripts, on the wire and on every client. main.cjs passes
    // the first block; without a server (unit tests) a private high range stands in.
    this.entityIds = [];
    this.entityIdsRequested = false;
    this.addEntityIds(bridge.entityIds ?? { first: 0x70000000, count: 0x0fffffff });
    this.nextWorldRequest = 0;
    this.resourceGeneration = 0;
    this.failedResources = new Set();
    this.restartTimers = new Map();
    this.shuttingDown = false;
    this.defaultRelay = true;
    this.stateStore = new EntityStateStore({
      side: 'server',
      ownerAlive: (owner) =>
        !owner || (this.resources.get(owner.name) === owner && owner.active && !owner.failing),
      run: (owner, fn) => this.context.run(owner, fn),
      onChange: (id, key, value, previous) => {
        const entity = this.players.get(id) ?? this.horses.get(id) ?? this.npcs.npcs.get(id);
        if (entity) this.emit('entityStateChange', entity, key, value, previous);
      },
      onError: (error, owner) => this.bridge.log(owner?.name ?? 'host', String(error)),
    });
    this.api = this.makeAPI();
    this.services = new ResourceServices({
      side: 'server',
      // Resources read their own data relative to it (process.cwd() is the resource's folder).
      serverDir: path.dirname(this.root),
      send: (value) => this.bridge.send(value),
      log: (owner, text) => this.bridge.log(owner, text),
      run: (owner, fn) => this.context.run(this.resources.get(owner), fn),
    });
  }
  makeAPI() {
    const host = this;
    // Entity is the base handle of every networked entity. Its constructor wraps an existing
    // entity and never spawns one; Player and Horse handles inherit from it.
    class Entity {
      constructor(id) {
        if (!Number.isInteger(id) || id < 1 || id > 0xffffffff) {
          throw new TypeError('Invalid entity ID');
        }
        const entity =
          host.players.get(id) ??
          host.horses.get(id) ??
          host.dogs.dogs.get(id) ??
          host.npcs.npcs.get(id) ??
          host.groundItems.stacks.get(id) ??
          host.props.props.get(id) ??
          host.siege.engines.get(id) ??
          host.doors.byId.get(id) ??
          host.carts.carts.get(id) ??
          host.vfx.effects.get(id) ??
          host.lights.lights.get(id) ??
          host.markers.find(id) ??
          host.blips.find(id) ??
          host.quests.find(id) ??
          host.siegeLadders.ladders.get(id) ??
          host.stonePiles.piles.get(id) ??
          host.stashes.stashes.get(id);
        if (!entity) {
          throw new Error('Unknown entity');
        }
        return entity;
      }
      toString() {
        return `Entity(${this.id})`;
      }
    }
    // Player.inRadius and Player.nearest: the players with a body in the world within `radius`
    // metres of `position`, nearest first.
    function playersNear(position, radius, options, name) {
      const finite = (value) => typeof value === 'number' && Number.isFinite(value);
      if (!position || !finite(position.x) || !finite(position.y) || !finite(position.z)) {
        throw new TypeError(`Player.${name} takes a position with finite x, y and z`);
      }
      if (typeof radius !== 'number' || Number.isNaN(radius) || radius <= 0) {
        throw new RangeError(`Player.${name} takes a radius greater than zero`);
      }
      const { dimension, except } = options ?? {};
      const found = [];
      for (const player of host.players.values()) {
        if (!player.ready || player === except) {
          continue;
        }
        if (dimension !== undefined && player.dimension !== dimension) {
          continue;
        }
        const at = player.position;
        const distance = Math.hypot(at.x - position.x, at.y - position.y, at.z - position.z);
        if (distance <= radius) {
          found.push({ player, distance });
        }
      }
      found.sort((a, b) => a.distance - b.distance);
      return found.map((entry) => entry.player);
    }
    class Player {
      constructor(id) {
        if (!host.players.has(id)) throw new Error('Unknown player');
        return host.players.get(id);
      }
      static all() {
        return [...host.players.values()];
      }
      static getById(id) {
        return host.players.get(id) ?? null;
      }
      // The players within `radius` metres of a position, nearest first (a KCDS addition). Only
      // players with a body in the world count. Options: `dimension` keeps the players of that
      // world, `except` leaves one player out (the one asking). Throws for a position without
      // finite x, y and z, or a radius that is not a positive number.
      static inRadius(position, radius, options = {}) {
        return playersNear(position, radius, options, 'inRadius');
      }
      // The player nearest to a position, or null (a KCDS addition). Options as for inRadius, and
      // `radius` (metres, no limit by default).
      static nearest(position, options = {}) {
        const radius = options?.radius ?? Infinity;
        return playersNear(position, radius, options, 'nearest')[0] ?? null;
      }
      // The players a command argument names: the player with that id when the text
      // is a whole number naming one; otherwise the players whose nickname is the text (ignoring
      // case), else those whose nickname starts with it, else those whose nickname contains it.
      // An empty array when none; more than one when the text is ambiguous.
      static find(text) {
        const wanted = String(text ?? '').trim();
        if (wanted === '') {
          return [];
        }
        if (/^\d+$/.test(wanted)) {
          const byId = host.players.get(Number(wanted));
          if (byId) {
            return [byId];
          }
        }
        const lower = wanted.toLowerCase();
        const players = [...host.players.values()];
        const named = (test) => players.filter((player) => test(player.nickname.toLowerCase()));
        for (const test of [
          (name) => name === lower,
          (name) => name.startsWith(lower),
          (name) => name.includes(lower),
        ]) {
          const found = named(test);
          if (found.length) {
            return found;
          }
        }
        return [];
      }
      setDimension(world) {
        host.setPlayerDimension(this, world);
      }
      showOnlyTo(player) {
        host.setPlayerVisibleTo(this, player);
      }
      // Gets the player up after playerDied, with full health. By default every injury, poison
      // and bleeding is cured too (heal()'s cures); revive({ keepEffects: true }) leaves them as
      // the knock-out left them. True when sent; false when the player is not
      // down or its revival was already requested.
      revive(options = {}) {
        return host.revivePlayer(this, options);
      }
      // The dog companion this player owns, or null.
      get dog() {
        return host.dogs.dogOf(this);
      }
      // The server's horse the player rides, or null.
      get horse() {
        for (const horse of host.horses.values()) {
          if (horse.riderId === this.id) {
            return horse;
          }
        }
        return null;
      }
      // Whether the player sits in the saddle of one of the server's horses.
      get mounted() {
        return this.horse !== null;
      }
      // Who is on this player's shoulder, or null.
      get carrying() {
        return host.bodyCarries.carrying(this);
      }
      // The player carrying this one's body, or null.
      get heldBy() {
        return host.bodyCarries.heldBy(this);
      }
      // Has the player put the carried body down with the game's own put-down; playerPutDown
      // follows when it lands. False if they carry no one or are disconnected.
      putDown() {
        return host.bodyCarries.putDown(this);
      }
      // Makes the player's body play one of the game's animations, on every screen; it is state
      // that lasts until stopAnimation or the next playAnimation. playerAnimationEnd follows, once
      // per request. True once sent;
      // false before the body exists, while cuffed (setCuffed) or for an animation the
      // body does not have (Animations.list: male for a man, player for a woman); throws for an
      // argument it cannot use.
      playAnimation(fragment, options) {
        if (host.cuffs.isCuffed(this)) return false;
        // An animation made in the Asset/Animation Studio, by the name Animations.define gave it
        // the body plays it as its pose animation.
        if (host.studioAnimations.has(fragment)) {
          if (!this.ready) return false;
          host.endStudioAnimation(this, 'replaced');
          const animation = host.studioAnimations.animationFor(fragment, options);
          if (host.studioAnimations.large(fragment)) {
            // Longer than the field takes: every client has it by its name.
            host.studioAnimations.playing.delete(this);
            host.studioAnimations.stopEvents(this);
            host.poseAnimations.set(this, animation);
            host.bridge.send({
              op: 'playerPoseAnimation',
              id: this.id,
              animation: poseAnimationReference(fragment, animation.loop),
            });
          } else {
            this.setPoseAnimation(animation);
          }
          // lockMovement or lockRotation, as with the game's clips: the body is held where it
          // stands (Player.frozen; looking around stays) while it plays, a whole-body animation
          // whose legs are its own (a baked game clip).
          if (
            (options?.lockMovement === true || Boolean(options?.lockRotation)) &&
            !host.frozenPlayers.isFrozen(this)
          ) {
            host.frozenPlayers.set(this, true);
            host.studioAnimations.held.add(this);
          }
          host.studioAnimations.playing.set(this, fragment);
          // Its effects at their times, while the player plays it.
          const alive = () =>
            host.studioAnimations.playing.get(this) === fragment &&
            host.players.get(this.id) === this;
          host.studioAnimations.startEvents(host, this, fragment, animation.loop, alive);
          return true;
        }
        host.endStudioAnimation(this, 'replaced');
        // props: 'held' holds what the player holds, each hand's item in the game's grip
        // an executioner cuts with his own sword.
        if (options?.props === 'held') {
          options = { ...options, props: heldProps(this) };
        }
        // An animation this player's body does not have is refused.
        const gender = this[playerAppearance]?.gender ?? 'male';
        if (!animationCatalog.playsOn(fragment, options?.tags, gender)) {
          // A fragment only the game's women have: a woman plays its clip.
          const own = gender === 'female' && animationCatalog.femaleClipOf(fragment, options?.tags);
          if (!own) return false;
          const { tags, ...rest } = options ?? {};
          return host.playerAnimations.play(this, '', { loop: own.loop, ...rest, clip: own.clip });
        }
        return host.playerAnimations.play(this, fragment, options);
      }
      // The running animation plays at `speed` on every screen: 1 as authored, 0
      // holds it still, up to 10; a paused one keeps still until resumeAnimation. True when the
      // order went out; false without a running playAnimation.
      setAnimationSpeed(speed) {
        return host.playerAnimations.setSpeed(this, speed);
      }
      // Holds the running animation still where it is, on every screen, until resumeAnimation
      // True once sent; false without a running playAnimation.
      pauseAnimation() {
        return host.playerAnimations.setPaused(this, true);
      }
      // Plays a paused animation on at its speed. True once sent; false
      // without a running playAnimation.
      resumeAnimation() {
        return host.playerAnimations.setPaused(this, false);
      }
      // The speed the running animation plays at (0 while paused), null without one.
      get animationSpeed() {
        return host.playerAnimations.speedOfPlayer(this);
      }
      // The running animation's props, as playAnimation takes them ('held' too), on every screen
      // without playing it again: tune a held prop while the hand moves. True when
      // the order went out; false without a running playAnimation.
      setAnimationProps(props) {
        if (props === 'held') {
          props = heldProps(this);
        }
        return host.playerAnimations.setProps(this, props);
      }
      // A paired animation with another player, as the game plays its own for two
      // characters: this one (the master) stays where it stands, the other player (the slave) is
      // put where the game's pair has it relative to the master, turned as the pair has it, and
      // both play at once, this one `fragment` and the other its Slave fragment
      // (Quest_StandHlbrdKill and Quest_StandHlbrdKillSlave), or `slaveFragment`; both are held
      // still (lockMovement, true by default) and do not push each other. True when it went out;
      // false for a player with no body, a cuffed one, the same player, players in other worlds or
      // a fragment either body does not have. playerAnimationEnd follows for each.
      playPairedAnimation(other, fragment, options) {
        if (host.cuffs.isCuffed(this) || host.cuffs.isCuffed(other)) return false;
        const slaveFragment = options?.slaveFragment ?? `${fragment}Slave`;
        // The game's own pair: a master without a Slave of its own needs slaveFragment.
        if (options?.slaveFragment === undefined && !animationCatalog.listed(slaveFragment)) {
          return false;
        }
        const genderOf = (player) => player?.[playerAppearance]?.gender ?? 'male';
        if (!animationCatalog.playsOn(fragment, options?.tags, genderOf(this))) return false;
        if (!animationCatalog.playsOn(slaveFragment, options?.slaveTags, genderOf(other))) {
          return false;
        }
        return host.playerAnimations.playPaired(this, other, fragment, options);
      }
      // The face's expression on every screen: an FE_* mood (FE_Default,
      // FE_DialogueIdle, FE_DialogueSpeaking) with `tags` (happy, angry, sad, nervous, pensive,
      // arogant, drunk), held beside the body's animation; "" hands the face back to the game.
      // True once sent; false before the body exists or while cuffed.
      setExpression(fragment, options) {
        if (host.cuffs.isCuffed(this)) return false;
        return host.playerAnimations.face(this, fragment, options);
      }
      // The mouth moves as if talking on every screen until setSpeaking(false): a loop
      // of the game's own face clip, not lip-sync. `style` is neutral, happy, drunk or chew; without
      // it the player's own style (setTalkingStyle). True once sent; false for a
      // player with no body yet.
      setSpeaking(talking, style) {
        return host.playerAnimations.talk(this, talking, style);
      }
      // The style setSpeaking uses for this player when given none (a KCDS addition); no style or
      // 'neutral' is normal speech again. True when set; false for a player who left.
      setTalkingStyle(style) {
        return host.playerAnimations.setTalkStyle(this, style);
      }
      // The player's own talking style (setTalkingStyle), 'neutral' by default.
      get talkingStyle() {
        return host.playerAnimations.talkStyleOf(this);
      }
      // The style the mouth moves in now, null while the player is not talking.
      get talking() {
        return host.playerAnimations.talkingOf(this);
      }
      // Ends what playAnimation started at once and takes its props away; { standUp: true } plays
      // the game's own way back to standing. True once sent; false for a cuffed
      // player.
      stopAnimation(options) {
        if (host.cuffs.isCuffed(this)) return false;
        if (host.endStudioAnimation(this, 'stopped')) return true;
        return host.playerAnimations.stop(this, options);
      }
      // Handcuffs (a KCDS addition): the player's hands are held behind the back on
      // every screen and they can only walk and look around until setCuffed(false); the event
      // playerCuffedChange(player, cuffed) follows. True when set; false for a player who left.
      setCuffed(cuffed) {
        return host.cuffs.set(this, cuffed);
      }
      // Puts the player in a horse's saddle: an order to their game, which mounts them
      // with the game's own get-on (or, with { instant: true }, the instant seat), so the seat is
      // reported like any other mount (horseMounting, horseMount). The player has to stand near
      // the horse. False when the horse is dead or knocked out, someone else rides it, or the
      // player is not in the world.
      mount(horse, options = {}) {
        if (horse === null || typeof horse !== 'object' || host.horses.get(horse.id) !== horse) {
          throw new TypeError('player.mount takes a Horse the server holds');
        }
        if (options === null || typeof options !== 'object') {
          throw new TypeError('player.mount options must be an object');
        }
        const instant = options.instant ?? false;
        if (typeof instant !== 'boolean') {
          throw new TypeError('player.mount instant must be true or false');
        }
        host.requireLivePlayerCall(this);
        if (!this.ready || !horse.alive || horse.knockedOut) {
          return false;
        }
        if (horse.riderId !== 0 && horse.riderId !== this.id) {
          return false;
        }
        host.bridge.send({ op: 'playerMount', id: this.id, horse: horse.id, instant });
        return true;
      }
      // This player opens every door and chest with no key, an admin for one. Their game
      // shows them unlocked, and the server lets them through.
      get keyBypass() {
        return host.playerKeyBypass.get(this) ?? false;
      }
      set keyBypass(value) {
        host.requireLivePlayerCall(this);
        if (typeof value !== 'boolean') {
          throw new TypeError('Player.keyBypass is true or false');
        }
        host.playerKeyBypass.set(this, value);
        host.bridge.send({ op: 'playerKeyBypass', id: this.id, bypass: value });
        host.doors.resendAllTo(this);
      }
      // Dismounts the player through their own client, and
      // horseDismount follows. False when they ride nothing.
      dismount() {
        host.requireLivePlayerCall(this);
        const riding = [...host.horses.values()].some((horse) => horse.riderId === this.id);
        if (!riding) {
          return false;
        }
        host.bridge.send({ op: 'playerDismount', id: this.id });
        return true;
      }
      // The game's own interaction for this player (a KCDS addition: horseWhistle, npcTalk,
      // npcChat, playerTalk; Interactions.set for every player): true or false, or null for the
      // server's setting. True when set; false for a player who left.
      setInteraction(name, allowed) {
        return host.interactionRules.setFor(this, name, allowed);
      }
      // Whether the player has that interaction now.
      interactionAllowed(name) {
        return host.interactionRules.allowedFor(this, name) === true;
      }
      // Lets this player be in the game's private and prohibited areas (a KCDS addition, issue):
      // no "You're in a private area" warning, and the game's AI is told not to react.
      // True or false, or null for the server's setting (Interactions.set('trespass', ...)). True
      // when set; false for a player who left.
      setTrespassAllowed(allowed) {
        return host.interactionRules.setFor(this, 'trespass', allowed);
      }
      // Whether the player may be in private and prohibited areas now.
      get trespassAllowed() {
        return host.interactionRules.allowedFor(this, 'trespass') === true;
      }
      // Whether this player sees the game's combat marker, the crossed swords under the compass in
      // a fight: true or false over CombatMarker.setEnabled, null back to the server's setting.
      setCombatMarker(shown) {
        host.combatMarker.setFor(this, shown);
      }
      // Whether the player sees the combat marker now.
      get combatMarker() {
        return host.combatMarker.shownFor(this);
      }
      // The player whistles for their horse, as the game's companion key does (a KCDS addition):
      // the horse comes and the players near them hear it (playerWhistle). True when the order
      // went out; false for a player not in the world yet.
      whistle() {
        return host.whistles.whistle(this);
      }
      // Whether the player is cuffed. Still readable in playerDisconnect, so a resource can cuff
      // a returning player again.
      get cuffed() {
        return host.cuffs.isCuffed(this);
      }
      // Attaches this player to another, as a rider to a horse (a KCDS addition): their
      // game keeps their body at `offset` ({ x right, y forward, z up }, metres, seen from the other
      // player) and turned by `rotation` degrees, and lets only looking around and the menus
      // through. False for a player who left, the player themselves, or a circle.
      attachTo(other, options) {
        return host.playerAttachments.attach(this, other, options);
      }
      // Ends attachTo: the player has their body back where they are. False when not attached.
      detach() {
        return host.playerAttachments.detach(this);
      }
      // The player this one is attached to, or null.
      get attachedTo() {
        return host.playerAttachments.targetOf(this);
      }
      // Frozen (a KCDS addition), as Npc.frozen: the server holds the player's body and
      // controls. Their game lets only looking around and the menus through, and a body that moves
      // anyway is put back where it was frozen. Ends when the player leaves.
      get frozen() {
        return host.frozenPlayers.isFrozen(this);
      }
      set frozen(value) {
        host.frozenPlayers.set(this, value);
      }
      // Whether the player may fly with NoClip (a KCDS addition, true by default).
      // False ends a flight that runs and makes their NoClip.enable() answer 'notAllowed'. Ends
      // when the player leaves; set it in playerConnect to keep a fly key for some players.
      get noclipAllowed() {
        return host.noclipRules.isAllowed(this);
      }
      set noclipAllowed(value) {
        host.noclipRules.set(this, value);
      }
      // Puts the player into the pillory (the game's stocks) whose holder entity has that name in
      // the level, or without a name into the pillory nearest to them (a KCDS addition, issue).
      // They stay until releaseFromPillory or until
      // another player picks the pillory's lock; the event playerPilloryReleased(player,
      // 'script' | 'lockpicked') follows. True once sent; false for a player with no
      // body yet; throws for a name it cannot send.
      putInPillory(entityName) {
        return host.pillories.putIn(this, entityName);
      }
      // The holder entity name of the pillory the player is in, or null. Still readable in
      // playerDisconnect, so a resource can put a returning player back with putInPillory.
      get pillory() {
        return host.pillories.holderOf(this);
      }
      // Takes the player out of the pillory they were put into. True when they were in one.
      releaseFromPillory() {
        return host.pillories.release(this);
      }
      // Ends the player's session: its client goes back to the main menu at once
      // with "You were kicked from the server: <reason>", and playerDisconnect follows about half
      // a second later. False when the player is already gone.
      kick(reason = '') {
        return host.kickPlayer(this, reason);
      }
      // Lets the player go without the word "kicked": their client goes back to the
      // main menu with `text` as it is ("You left the server." when it is empty), or with
      // `{ quit: true }` closes the game as the menu's Quit does; playerDisconnect follows about
      // half a second later, where a resource saves the player. False when the player is gone.
      disconnect(text = '', options = undefined) {
        return host.disconnectPlayer(this, text, options);
      }
      // Bans the player from this server with a reason and no end (their identifiers but the
      // address, the list KCDS-Admin keeps): they leave at once with "You are banned from this
      // server: <reason>" and are turned away when they come back, until Bans.remove lifts it.
      // Returns the ban { id, identifiers, reason, by, at, until }, or null when the player is gone.
      ban(reason = undefined) {
        return host.bansApi.banPlayer(this, reason);
      }
      // How much of the world the player sees while knocked out, in percent: 0 is a black screen,
      // 1-100 shows the world darkened to that percent (100 is not darkened) in the game's red
      // low-health tint. The client's default is 40.
      setKnockoutVision(percent) {
        host.setKnockoutVision(this, percent);
      }
      // Asks the player's own client to put on the named parts of a look; see
      // True when the request went out.
      setAppearance(appearance) {
        return host.setPlayerAppearance(this, appearance);
      }
      // Asks the player's own client to stop drawing its body (false) or to draw it again (true);
      // only the drawing changes. True when the request went out.
      setVisible(visible) {
        return host.setPlayerVisible(this, visible);
      }
      // Asks the player's own client to draw its body as a prop-catalog mesh, or as itself for
      // null. True when the request went out.
      setDisguise(model) {
        return host.setPlayerDisguise(this, model);
      }
      // Hangs a mesh on a joint of the player's body (a KCDS addition: cuffs on the
      // wrists, a sack on the head, a noose); everyone sees it follow the joint, also while the
      // player is cuffed. The Prop (no physics); it goes when the player leaves.
      attachProp(options) {
        host.requireLivePlayerCall(this);
        return host.props.attach(this, options);
      }
      // The game's own brand on the player's neck: 0 none, 1 fresh, 2 healed. Everyone
      // sees it, a player who joins later too, until it is set back or this player leaves.
      get brand() {
        return host.playerBrands.get(this) ?? 0;
      }
      set brand(value) {
        host.requireLivePlayerCall(this);
        if (value !== 0 && value !== 1 && value !== 2) {
          throw new TypeError('Player.brand is 0 (none), 1 or 2');
        }
        host.playerBrands.set(this, value);
        host.bridge.send({ op: 'playerBrand', id: this.id, brand: value });
      }
      // The rope or whip in the player's hand (Asset/Animation Studio):
      // { kind, segments, length, hand, grip: { pitch, twist }, physics: { gravity, damping,
      // mass, air, bend, stiffness, collide, shape, curl, sway } } or null. Every client shows it on this player, a
      // player who joins later too, until it is set to null or this player leaves.
      get physicalProp() {
        const prop = host.physicalProps.get(this);
        return prop ? structuredClone(prop) : null;
      }
      // Gives the player a physical prop: { kind: 'rope' | 'whip' | 'chain', segments?: 2..64,
      // length?: 0.2..20, hand?: 'RightHand' | 'LeftHand', grip?: { pitch?: 0..180, twist?:
      // -180..180 }, physics?: { gravity?: 0..30, damping?: 0..5, mass?: 0.05..10, air?: 0..2,
      // bend?: 0..90, stiffness?: 0..500, collide?: 0 | 1 | 2, shape?: 0..1, curl?: -540..540,
      // sway?: -540..540 } } (a whip's segments, length and physics are its lash's; shape 1
      // poses a whip's lash instead of moving it by physics, bent curl degrees toward the
      // handle's up side and sway degrees sideways; the defaults are 16 and 1.2 for a rope, 20 and 1.6 for a whip,
      // 24 and 1.5 for a chain, the right hand, grip 90 and 0, and the studio's first physics),
      // or null to take it away. Throws for anything else.
      setPhysicalProp(options) {
        host.requireLivePlayerCall(this);
        if (options === null) {
          host.physicalProps.delete(this);
          host.bridge.send({ op: 'playerPhysicalProp', id: this.id, prop: '' });
          return;
        }
        const prop = physicalPropOf(options);
        host.physicalProps.set(this, prop);
        host.bridge.send({ op: 'playerPhysicalProp', id: this.id, prop: physicalPropField(prop) });
      }
      // The pose animation the player's body plays (Asset/Animation Studio): the
      // studio's { keys, blend, loop } or null. Every client plays it on this player, a player
      // who joins later too, on top of the body's own animation.
      get poseAnimation() {
        const animation = host.poseAnimations.get(this);
        return animation ? structuredClone(animation) : null;
      }
      // Plays a studio animation on the player's body: { keys: [{ time, pose: { joint:
      // [x, y, z] } }], blend?: 'spline' | 'smooth' | 'linear', loop?: true } (turns in degrees
      // about each joint's parent's axes), or null to stop it. Throws for anything else.
      setPoseAnimation(options) {
        host.requireLivePlayerCall(this);
        // A pose animation set here is no longer one played by name.
        host.studioAnimations.playing.delete(this);
        host.studioAnimations.stopEvents(this);
        if (options === null) {
          host.poseAnimations.delete(this);
          host.bridge.send({ op: 'playerPoseAnimation', id: this.id, animation: '' });
          return;
        }
        const { animation, field } = poseAnimationOf(options);
        host.poseAnimations.set(this, animation);
        host.bridge.send({ op: 'playerPoseAnimation', id: this.id, animation: field });
      }
      // A mark on the player's skin (a KCDS addition: a brand on the shoulder, a scar
      // on the cheek): a flat mesh with the mark's texture (one your resource ships, or the game's)
      // on a joint, close to the skin. Everyone sees it, and it stays until removeMark or the
      // player leaves. Returns its id; throws for options it cannot
      // use.
      addMark(options) {
        host.requireLivePlayerCall(this);
        return host.props.addMark(this, options);
      }
      // Takes a mark addMark put on off again; true when the id was one of this player's marks.
      removeMark(id) {
        return host.props.removeMark(this, id);
      }
      // The ids of the player's marks (addMark), oldest first.
      get marks() {
        return host.props.marksOf(this);
      }
      // Takes a prop attachProp hung on this player off again (the Prop or its id); true when it
      // was one.
      detachProp(prop) {
        return host.props.detach(this, prop);
      }
      // Puts a piece of clothing on the player for a while without it being in their inventory
      // (a noose, a sack, shackles); everyone sees it. True when it was not worn so.
      wearItem(item) {
        return host.setPlayerWorn(this, item, true);
      }
      // Takes a piece wearItem put on away again; true when it was worn so.
      removeWorn(item) {
        return host.setPlayerWorn(this, item, false);
      }
      // The classes wearItem put on, by id.
      get wornItems() {
        return [...(host.wornOnly.get(this.id) ?? new Map()).values()].map((entry) => entry.id);
      }
      // The player's nameplate for every other player: shown or hidden, with or
      // without the health bar under the name, a text instead of the name (at most 64 bytes;
      // empty or left out restores the name) and its colour (0xAARRGGBB). Each player can still
      // hide every nameplate for themselves (the client's Nameplates).
      setNameplateVisible(visible) {
        host.nameplates.setVisible(this, visible);
      }
      isNameplateVisible() {
        return host.nameplates.of(this).visible;
      }
      setNameplateHealthVisible(visible) {
        host.nameplates.setHealthVisible(this, visible);
      }
      isNameplateHealthVisible() {
        return host.nameplates.of(this).health;
      }
      // Whether the tag shows the name (a KCDS addition); with the health bar alone the bar takes
      // the name's place.
      setNameplateNameVisible(visible) {
        host.nameplates.setNameVisible(this, visible);
      }
      isNameplateNameVisible() {
        return host.nameplates.of(this).name;
      }
      // What a shown tag carries (a KCDS addition): 'both' (the default), 'name', 'health' or
      // 'none'; the name and health switches together.
      setNameplateMode(mode) {
        host.nameplates.setMode(this, mode);
      }
      getNameplateMode() {
        return host.nameplates.modeOf(this);
      }
      setNameplateText(text) {
        host.nameplates.setText(this, text);
      }
      getNameplateText() {
        return host.nameplates.of(this).text;
      }
      // A title above the name on every screen (a KCDS addition), such as a role: "Town guard",
      // or "Admin" in red (0xffe0402a) and larger (24). 64 UTF-8 bytes is the limit; `color` is packed
      // 0xAARRGGBB, none or 0 for the plate's gold; `size` is pixels at 1080 high, 8 to 48, none
      // or 0 for the plate's own, and a larger title only grows upward; '' or none takes it away.
      setNameplateTitle(title, color, size) {
        host.nameplates.setTitle(this, title, color, size);
      }
      getNameplateTitle() {
        return host.nameplates.of(this).title;
      }
      // The title's colour, 0 for the plate's gold.
      getNameplateTitleColor() {
        return host.nameplates.of(this).titleColor;
      }
      // The title's size, 0 for the plate's own.
      getNameplateTitleSize() {
        return host.nameplates.of(this).titleSize;
      }
      setNameplateColor(color) {
        host.nameplates.setColor(this, color);
      }
      // A line above this player's tag for the players near them, as /me and /do (a KCDS
      // addition); returns how many players it went to.
      showOverheadText(text, durationMs, color, range) {
        return host.nameplates.showOverheadText(this, text, durationMs, color, range);
      }
      getNameplateColor() {
        return host.nameplates.of(this).color;
      }
      // Places the player. From a playerSpawning handler it is the answer to that request: the
      // player appears there as their body comes into the world, behind their loading screen. At
      // any other time it moves a player already in the world. True when accepted; false for a
      // position that is not in a world or a player no longer connected.
      spawn(position, rotation) {
        return host.spawnPlayer(this, position, rotation);
      }
      // Asks the player's own client to put them somewhere else, on their next frame. The label
      // names the destination for display only. True once sent; false for a
      // position that is not in a world or a player no longer connected.
      teleport(position, label) {
        return host.teleportPlayer(this, position, label);
      }
      // The latest reported writable stat (PlayerStat) in native units, 0 before the player's
      // client reported one. Throws for an unknown stat.
      getStat(stat) {
        return getStat(this[playerStats], stat);
      }
      // The latest reported derived stat (DerivedPlayerStat) in native units, 0 before the
      // player's client reported one. Throws for an unknown stat.
      derivedStat(stat) {
        return derivedStat(this[playerStats], stat);
      }
      // One track's state for this player (level, XP so far to the next, that level's cost, unspent
      // points), or null before their client's first report. Throws for an unknown track.
      trackOf(track) {
        return trackProgress(this[playerProgression], track);
      }
      // Whether the player's last report carried the perk (a name or GUID).
      hasPerk(perk) {
        return hasPerk(this[playerProgression], perk);
      }
      // the player knows a smithing or alchemy recipe at once (Recipe.get(id)), as
      // reading its sketch would teach it: the recipe's perk is given. True when the order went
      // out or the player already knows it. Throws for an unknown recipe.
      learnRecipe(recipe) {
        const known = recipes.requireRecipe(recipe);
        if (this.knowsRecipe(recipe)) return true;
        // A row a table patch added has no perk: it is not in the game's book.
        if (known.perks.length === 0) return false;
        return this.addPerk(known.perks[0]);
      }
      // The player forgets a recipe: every perk that makes it known is taken away. True when an
      // order went out; false when the player did not know it. Throws for an unknown recipe.
      forgetRecipe(recipe) {
        const known = recipes.requireRecipe(recipe);
        let sent = false;
        for (const perk of known.perks) {
          if (this.hasPerk(perk)) sent = this.removePerk(perk) || sent;
        }
        return sent;
      }
      // Whether the player's last report shows the recipe known. Throws for an unknown recipe.
      knowsRecipe(recipe) {
        const known = recipes.requireRecipe(recipe);
        return known.perks.some((perk) => this.hasPerk(perk));
      }
      // The item classes the body wears (32 hex digits each); empty for a bare body.
      get equipment() {
        return [...(this[playerEquipment] ?? [])];
      }
      // The item class drawn in the right or left hand (32 hex digits), or '' for an empty hand.
      get rightHandItem() {
        return (this[playerHands] ?? ['', ''])[0];
      }
      get leftHandItem() {
        return (this[playerHands] ?? ['', ''])[1];
      }
      // The weapon in the right hand: its item class GUID, as Items.info and
      // Inventory take it; '' when that hand holds no weapon (empty, a torch, a tool).
      get heldWeapon() {
        const itemClass = lookupClassHex(this.rightHandItem);
        const weapon =
          itemClass?.category === 'meleeWeapon' || itemClass?.category === 'missileWeapon';
        return weapon ? itemClass.id : '';
      }
      // The language the player's game shows ('english', 'czech', ...), which chooses the KCDS
      // texts they see (Locales); '' until their game reported it.
      get language() {
        return this[playerLanguage] ?? '';
      }
      // How dirty, bloody and worn the body and each worn piece are: { body: { dirt,
      // blood: { head, torso, leftArm, rightArm, leftLeg, rightLeg } }, items: [{ item, dirt,
      // blood, wear }] }, 0 .. 1; null until reported.
      get grime() {
        return this[playerGrime] ?? null;
      }
      // Whether the player sits on a seat of the game's (a chair, a bench, a dice table's chair):
      // their game holds it for them. False until reported.
      get seated() {
        return this[playerSeated] === true;
      }
      // The stance tag the player's own game chose for their body, as Player.bodyStance reads it
      // on that player's client (a seated body has the sitting stance); 255 for none or before a
      // report. playerStanceChanged comes with each change.
      get bodyStance() {
        return this[playerStance] ?? 255;
      }
      // Washes the body and/or the worn clothes ({ body, clothes }, both by default) in the
      // player's own game; everybody sees it. False for a player who is gone.
      clean(options) {
        const order = cleanOrder(this.id, options);
        if (!host.connectedPlayer(this)) return false;
        host.bridge.send(order);
        return true;
      }
      // Adds dirt and blood ({ dirt, blood } 0 .. 1, { body, clothes }) in the player's own game.
      soil(options) {
        const order = soilOrder(this.id, options);
        if (!host.connectedPlayer(this)) return false;
        host.bridge.send(order);
        return true;
      }
      // How fast walking dirties their worn clothes, 1 the game's own rate, 0 none, up to 100
      // It lasts until they leave. False for a player who is gone.
      setGrimeRate(rate) {
        return host.customBuffs.setGrimeRate(this, rate);
      }
      // The rate setGrimeRate gave them, 1 by default.
      get grimeRate() {
        return host.customBuffs.grimeRateOf(this);
      }
      // the most units of an item (GUID or table name) this player may hold, over the
      // server's item config; null clears it. Returns the limit that applies now (or null).
      setItemLimit(item, max) {
        return host.itemOverrides.setPlayerLimit(this, item, max);
      }
      // The limit on an item that applies to this player now, or null for none.
      itemLimit(item) {
        return host.itemOverrides.playerLimitOf(this, item);
      }
      // how much of what this player wears counts toward their load, in percent, over
      // the server's (Items.setEquippedWeightPercent); null clears it. The share that applies now.
      setEquippedWeightPercent(percent) {
        return host.itemOverrides.setPlayerEquippedPercent(this, percent);
      }
      get equippedWeightPercent() {
        return host.itemOverrides.equippedPercentOf(this);
      }
      // Grants items of a class (GUID or table name) into the inventory the server holds; false
      // if the item is unknown, the amount is not in 1..10000 or the player lacks an inventory.
      giveItem(item, amount = 1) {
        return host.playerInventories.giveItem(this, item, amount);
      }
      // Shows the game's own notice of units of a class received (or given away) in this player's
      // game, without touching their inventory (a KCDS addition); false for an unknown item, an
      // amount outside 1..1000000 or a player who left.
      showItemNotice(item, amount = 1, received = true) {
        return host.playerInventories.showNotice(this, item, amount, received);
      }
      // Removes units of a class from any of its rows, all or nothing; the promise is settled.
      takeItem(item, amount = 1) {
        return host.playerInventories.takeItem(this, item, amount);
      }
      // The same as Inventory.get(player).
      getInventory() {
        return host.playerInventories.stateOf(this);
      }
      // Moves the player's rows (at most 128) into a container spawned at their body, reason
      // 'drop'; `keepEquipped` keeps what the body wears or holds. The container, or null when
      // nothing went.
      dropAll(options = {}) {
        if (options === null || typeof options !== 'object') {
          throw new TypeError('dropAll options must be an object');
        }
        host.requireLivePlayerCall(this);
        return host.stashes.dropAll(this, { keepEquipped: !!options.keepEquipped });
      }
      // Ends melee combat between this player and the opponent: both clients drop
      // their lock on the other, then playerCombatCancelled. False for a player that is gone or
      // not playing, the same player, or another dimension.
      stopFight(opponent) {
        return host.stopFight(this, opponent);
      }
      // The players they fight: those they swung at, or who swung at them, in the last 15 seconds
      get opponents() {
        return host.opponentsOf(this);
      }
      // Makes peace with everyone they fight, as stopFight with each; returns how many fights it
      // ended. Bind it to a key the player holds to offer the old "make peace".
      makePeace() {
        let ended = 0;
        for (const opponent of host.opponentsOf(this)) {
          if (host.stopFight(this, opponent)) ended += 1;
        }
        return ended;
      }
      // Has the player carry an item in their arms: a new one by the name of an item
      // the game has a carrying walk for, or a carryable GroundItem they take up
      // (carry/player_carry.cjs). False when they cannot act, ride, carry already or are not
      // playing, or the ground item is out of reach or being taken; the carry stands once their
      // client reports it (playerCarryItem). Throws for another item name, or one with no walk
      // for their body.
      holdItem(item) {
        return host.carries.holdItem(this, item);
      }
      // Has the player put the carried item down, with the game's put-down or, `immediate`, let
      // go at once; it becomes a carryable GroundItem where it lands. False when they carry
      // nothing.
      dropHeldItem(options = {}) {
        return host.carries.dropHeldItem(this, options);
      }
      // The name of the item the player carries in their arms, or null.
      get heldItem() {
        return host.carries.heldItem(this);
      }
      // Whether the player lies knocked out (playerDied) and has not been revived.
      get knockedOut() {
        return !!this[playerDown];
      }
      // Whether others may search the player's body with the game's own Search while they are
      // knocked out; true unless a script says otherwise. Changing it while they are
      // down puts the body's container there or takes it away.
      get lootable() {
        return this[playerLootable] !== false;
      }
      set lootable(value) {
        if (typeof value !== 'boolean') {
          throw new TypeError('Player.lootable must be a boolean');
        }
        this[playerLootable] = value;
        if (this[playerDown] && host.players.get(this.id) === this) {
          if (value) {
            host.stashes.knockedOut(this);
          } else {
            host.stashes.revived(this);
          }
        }
      }
      // Dresses the body in one of the game's own outfits for this player's gender: the clothing
      // it wears comes off and stays, the outfit's is added and worn; their game dresses them a
      // moment later. False for an unknown outfit or the other gender's.
      setOutfit(preset) {
        const gender = this[playerAppearance]?.gender ?? 'male';
        return host.playerInventories.setOutfit(this, preset, gender);
      }
      // The game's own outfits, those authored for one gender when it is given.
      static outfitSets(gender) {
        return PlayerInventories.outfitSets(gender);
      }
      // Whether the player's last report carried the status effect (a name or GUID).
      // Which limbs are injured, as the player's own client last reported them (its injury buffs);
      // all false until the first report.
      get injuries() {
        return injuriesOf(this[playerBuffs]);
      }
      hasBuff(buff) {
        const custom = host.customBuffs.resolve(buff);
        if (custom) return host.customBuffs.has(this, custom.handle.name);
        return hasBuff(this[playerBuffs], buff);
      }
      // Asks the player's client to put a status effect on; it lands a moment later
      // (playerBuffAdded). False for an unknown buff, a player with no world, or the game's
      // unconsciousness (setHealth(0) and revive() rule a knock-out).
      addBuff(buff) {
        const custom = host.customBuffs.resolve(buff);
        if (custom) return host.customBuffs.add(this, custom);
        const info = lookupBuff(buff);
        if (!info || isKnockOutBuff(info)) return false;
        return host.sendBuffOrder(this, { kind: 'add', buff: info.id });
      }
      // Sends the order to remove a status effect to their client: the instances the last report
      // named, otherwise all of them. False if the buff is unknown or the player lacks a
      // world.
      removeBuff(buff) {
        const custom = host.customBuffs.resolve(buff);
        if (custom) {
          host.customBuffs.end(this, custom.handle.name, 'server');
          return host.canOrderPlayer(this);
        }
        const info = lookupBuff(buff);
        if (!info) return false;
        const carried = (this[playerBuffs] ?? []).filter((entry) => entry.buff === info);
        const order = {
          kind: 'remove',
          buff: info.id,
          instances: carried.map((entry) => entry.instance),
        };
        return host.sendBuffOrder(this, order, carried);
      }
      // Clears every effect of one family (Buffs.tags). False for a tag no buff table uses or a
      // player with no world.
      clearBuffs(tag) {
        const family = typeof tag === 'string' ? TAG_BY_NAME.get(tag) : undefined;
        if (!family) return false;
        const carried = (this[playerBuffs] ?? []).filter((entry) => entry.buff.aiTag === tag);
        return host.sendBuffOrder(this, { kind: 'clear', tag: family.id }, carried);
      }
      // Grants XP on a track through the game's own path on the player's client (their perks'
      // XP multipliers apply, a level crossed grants its points). True when the order went out.
      // { notify: false } holds back the game's notice of the levels it brings.
      addXp(track, xp, options) {
        return host.sendProgressionOrder(this, withNotice(addXpOrder(track, xp), options));
      }
      // Puts a track at a level; false for a level past the cap. A higher level is the game's
      // level-up ({ notify: false } holds back its notice); a lower one is the exact
      // order: the level and no XP towards the next, with no reward, point or notice.
      setLevel(track, level, options) {
        const current = this.levels[track];
        const order = setLevelOrder(track, level, current ?? 0);
        if (!order) return false;
        // The exact order never announces anything, so it takes no notice option.
        return host.sendProgressionOrder(
          this,
          order.kind === 'setTrack' ? order : withNotice(order, options),
        );
      }
      // puts a track at an absolute, cumulative XP from level 0 (quantized down to 1/128
      // XP), so its level follows, up or down; the exact order, so no level-up reward, point or
      // notice. False before the player's first progression report, or for XP past the track's
      // cap.
      setXp(track, xp) {
        const order = setXpOrder(track, xp);
        if (!order || !this.trackOf(track)) return false;
        return host.sendProgressionOrder(this, order);
      }
      // takes XP off a track's reported total, its level following down; zero at the
      // least. The exact order, so perks and points stay. False before the player's first
      // progression report.
      removeXp(track, xp) {
        const state = this.trackOf(track);
        if (!state) {
          removeXpOrder(track, xp, { level: 0, xp: 0 });
          return false;
        }
        return host.sendProgressionOrder(this, removeXpOrder(track, xp, state));
      }
      // Gives a perk outright (no point spent, no requirement checked by the perk screen).
      addPerk(perk) {
        const { order, name } = perkOrder('addPerk', perk);
        return host.sendProgressionOrder(this, order, { name, change: 'added' });
      }
      // Removes a perk without refunding its point.
      removePerk(perk) {
        const { order, name } = perkOrder('removePerk', perk);
        return host.sendProgressionOrder(this, order, { name, change: 'removed' });
      }
      // Gives unspent perk points in one tree (a track name or 'main').
      addPerkPoints(track, count) {
        return host.sendProgressionOrder(this, addPointsOrder(track, count));
      }
      // The game's own respec: perk-screen perks taken away, every tree's points rebuilt.
      resetPerks() {
        const owned = this[playerProgression]?.perks ?? [];
        const taken = owned.filter(perkScreenPerk).map((perk) => ({
          name: perk.name,
          change: 'removed',
        }));
        return host.sendProgressionOrder(this, respecOrder(), ...taken);
      }
      // Puts a stored progression back; raises a track as the game's level-ups ({ notify: false }
      // holds back their notice). { exact: true } writes the snapshot as it is: a
      // track above it comes down too, and no level-up reward, point or notice is replayed.
      loadProgression(snapshot, options) {
        const exact = options?.exact;
        if (exact !== undefined && typeof exact !== 'boolean') {
          throw new TypeError('exact is true or false');
        }
        const order = withNotice(restoreOrder(snapshot, exact === true), options);
        const owned = new Set(this[playerProgression]?.perks ?? []);
        const given = order.snapshot.perks
          .map((id) => lookupPerk(id))
          .filter((perk) => !owned.has(perk))
          .map((perk) => ({ name: perk.name, change: 'added' }));
        return host.sendProgressionOrder(this, order, ...given);
      }
      // Puts a pace rule on the player: walking as the pace they come back to, running and
      // sprinting forbidden, or both. A key left out is off.
      // True once sent; false if the player is not connected.
      setPace(mode) {
        return host.setPlayerMovementMode(this, mode);
      }
      // Asks the player's own client to set a writable stat.
      // True once sent; false for a player without a living body.
      setStat(stat, value) {
        return host.setPlayerStat(this, stat, value);
      }
      // KCDS: asks the player's own client to add `delta` (negative lowers it) to a
      // writable stat's current value there, within 0 and its maximum, through the game's own
      // setter; health brought to 0 is the game's own lethal damage, as setStat('health', 0).
      // True once sent; false for a player without a living body.
      addStat(stat, delta) {
        return host.addPlayerStat(this, stat, delta);
      }
      // KCDS: setStat to the stat's maximum in the player's own game ('exhaust' a
      // full night's sleep, 'hunger' a full stomach). The same returns as setStat.
      fillStat(stat) {
        return host.fillPlayerStat(this, stat);
      }
      // setStat for health and for nourishment.
      // The second argument is a KCDS addition: setHealth(0, { fall: true }) knocks the player out
      // with the body dropping at once (the game's own ragdoll would stand it about half a second
      // first). `fall` is refused with any value but 0.
      setHealth(value, options = {}) {
        if (options === null || typeof options !== 'object') {
          throw new TypeError('setHealth options must be an object');
        }
        const fall = options.fall === undefined ? false : options.fall;
        if (typeof fall !== 'boolean') throw new TypeError('setHealth fall must be a boolean');
        if (fall && value !== 0) throw new RangeError('setHealth fall needs health 0');
        return host.setPlayerStat(this, 'health', value, fall);
      }
      setHunger(value) {
        return host.setPlayerStat(this, 'hunger', value);
      }
      // Nurses the player back with the game's own recipe: the remove_injuries and
      // remove_all_posions cures, the standalone bleeding off, then health raised (never
      // lowered). Does not revive. True when the orders went out.
      heal(options = {}) {
        return host.healPlayer(this, options);
      }
      toString() {
        return `Player(${this.id}, ${JSON.stringify(this.nickname)})`;
      }
    }
    Object.setPrototypeOf(Player.prototype, Entity.prototype);
    // Who the player is (read-only, '' when unknown): steamId, epicId, store ('steam', 'epic', 'xbox',
    // 'gog') and storeAccount come from the Master's verified licence pass; discordId from it too
    // when discordVerified, else what the client reported; deviceId is what the client reported;
    // ip is the address the server saw their connection come from (server only).
    for (const key of Object.keys(NO_IDENTITY)) {
      Object.defineProperty(Player.prototype, key, {
        enumerable: true,
        get() {
          return host.identityOf(this.id)[key];
        },
      });
    }
    // The connection's round-trip time in milliseconds, measured by the transport and refreshed
    // every 2 seconds; 0 before the first measurement.
    Object.defineProperty(Player.prototype, 'ping', {
      enumerable: true,
      get() {
        return host.pings.get(this.id) ?? 0;
      },
    });
    Object.defineProperty(Player.prototype, 'getIP', {
      value() {
        return host.identityOf(this.id).ip;
      },
    });
    // An event for this player's client only, as Events.broadcast sends one to everyone
    // the client's Events.on(name) handlers get the payload.
    Object.defineProperty(Player.prototype, 'emit', {
      value(name, payload) {
        if (host.players.get(this.id) !== this) {
          return false;
        }
        host.emitToPlayerClient(this.id, name, payload);
        return true;
      },
    });
    // Every identifier the player has, [{ kind, id, verified }]: the store account
    // and the ip verified, the Discord and hardware ids as the client reported them.
    Object.defineProperty(Player.prototype, 'identifiers', {
      enumerable: true,
      get() {
        return identifiersOf(host.identityOf(this.id));
      },
    });
    Object.defineProperty(Player.prototype, 'getIdentifier', {
      value(kind) {
        return identifierOf(host.identityOf(this.id), kind);
      },
    });
    // What the player's body looks like, as its own client last published it (read-only).
    Object.defineProperty(Player.prototype, 'appearance', {
      enumerable: true,
      get() {
        return this[playerAppearance];
      },
    });
    // The mesh the player's body is drawn as, as its own client last published it, or null
    // (read-only).
    Object.defineProperty(Player.prototype, 'disguise', {
      enumerable: true,
      get() {
        return this[playerDisguise];
      },
    });
    // Every track's level as the player's client last reported it; zeros before (read-only).
    Object.defineProperty(Player.prototype, 'levels', {
      enumerable: true,
      get() {
        return levelsOf(this[playerProgression]);
      },
    });
    // health, maxHealth ... poisoning, strength, agility, vitality, skills, scaledSkills, level
    // and healthPercent, as the player's own client last reported them (read-only).
    // A knocked-out player (playerDied, until revive) is down whatever health the game keeps them
    // at (the immortality minimum): not conscious, unable to act and not alive.
    defineReadings(
      Player.prototype,
      (player) => player[playerStats],
      (player) => !!player[playerDown],
    );
    definePostureReadings(Player.prototype, (player) => player[playerPosture]);
    // The status effects on the player's body as their client last reported them (read-only).
    Object.defineProperty(Player.prototype, 'buffs', {
      enumerable: true,
      get() {
        const now = host.buffClock();
        return [...buffStates(this[playerBuffs], now), ...host.customBuffs.statesOf(this, now)];
      },
    });
    // Every perk the player owns, by name (read-only).
    Object.defineProperty(Player.prototype, 'perks', {
      enumerable: true,
      get() {
        return perkNames(this[playerProgression]);
      },
    });
    // The player's whole progression as plain JSON (what loadProgression takes), or null before
    // their client's first report (read-only).
    Object.defineProperty(Player.prototype, 'progression', {
      enumerable: true,
      get() {
        return snapshotOf(this[playerProgression]);
      },
    });
    // Strength, agility and vitality as the engine's relative values, as the player's own client
    // last published them; zeros before it did (read-only).
    Object.defineProperty(Player.prototype, 'scaledStats', {
      enumerable: true,
      get() {
        return scaledStats(this[playerStats]);
      },
    });
    // Whether the player's helmet has its visor down, according to their client;
    // false without a helmet that has one (read-only).
    Object.defineProperty(Player.prototype, 'visorDown', {
      enumerable: true,
      get() {
        return this[playerVisorClosed] === true;
      },
    });
    // Whether the player's body is drawn, as its own client last published it (read-only).
    Object.defineProperty(Player.prototype, 'visible', {
      enumerable: true,
      get() {
        return this[playerVisible];
      },
    });
    // Entity.dimension: read-only for scripts; changed through setDimension.
    Object.defineProperty(Player.prototype, 'dimension', {
      enumerable: true,
      get() {
        return this[playerWorld];
      },
    });
    // Entity.position: where the server holds it in the world; setting it also updates what clients get.
    Object.defineProperty(Player.prototype, 'position', {
      enumerable: true,
      get() {
        return this[playerPosition];
      },
      set(value) {
        host.forcePlayerPosition(this, value);
      },
    });
    // The velocity the player's body moved at, measured from its successive positions by its own
    // client, and where its head and eyes are turned (zero until it reported one) (read-only).
    Object.defineProperty(Player.prototype, 'velocity', {
      enumerable: true,
      get() {
        return this[playerVelocity] ?? new Vector3(0, 0, 0);
      },
    });
    Object.defineProperty(Player.prototype, 'gaze', {
      enumerable: true,
      get() {
        return this[playerLookDirection] ?? new Vector3(0, 0, 0);
      },
    });
    // Entity.rotation: reads return a Quaternion; assignment accepts a Quaternion or Euler angles
    // in degrees and forces the replicated rotation.
    Object.defineProperty(Player.prototype, 'rotation', {
      enumerable: true,
      get() {
        return this[playerRotation];
      },
      set(value) {
        host.forcePlayerRotation(this, value);
      },
    });
    class Horse {
      constructor(id) {
        const h = host.horses.get(id);
        if (!h) throw new Error('Unknown horse');
        return h;
      }
      static all() {
        return [...host.horses.values()];
      }
      static getById(id) {
        return host.horses.get(id) ?? null;
      }
      // The sixth argument is a KCDS addition: { knockOut } picks what a lethal blow does to the
      // horse. false (the default) kills it, as the game does; true only knocks it out. { breed }
      // takes a breed of the server's horse config, { look } (HorseLooks parts),
      // { maxCarryWeight } and { stamina, speed, health } go over the breed's.
      static spawn(position = {}, orientation = {}, soul = '', name = '', gear, options = {}) {
        if (typeof soul !== 'string' || typeof name !== 'string')
          throw new TypeError('Horse soul/name must be strings');
        if (options === null || typeof options !== 'object')
          throw new TypeError('Horse spawn options must be an object');
        // Its tack: the call's, else one of the game's generic sets.
        const tack = gear === undefined ? defaultGear() : gearOf(gear, 'Horse.spawn');
        // A breed of the server's horse config: its look, carry limit and knockOut,
        // with the call's own options over them. A breed's name as the soul names it too
        // (Horse.breeds).
        let breedName = options.breed;
        if (breedName === undefined && host.horseConfig.breeds.has(soul)) {
          breedName = soul;
          soul = '';
        }
        const breed = breedName === undefined ? null : host.horseConfig.breed(breedName);
        const knockOut =
          options.knockOut !== undefined ? options.knockOut : breed?.knockOut ?? false;
        if (typeof knockOut !== 'boolean') throw new TypeError('Horse knockOut must be a boolean');
        // A named horse of the game (its soul's GUID) keeps the soul's own coat, mane and tail
        // (null: the game's own); any other horse gets one at random. A breed's or the call's look
        // goes over either, the parts it leaves out at random.
        let look = isGameHorseSoul(soul) ? null : randomLook();
        for (const request of [breed?.look, options.look]) {
          if (request === undefined) continue;
          look = lookOf(look ?? randomLook(), request);
          if (look === null) throw new TypeError('Horse look parts must be HorseLooks names');
        }
        const carry =
          options.maxCarryWeight !== undefined
            ? checkCarryWeight(options.maxCarryWeight, 'Horse maxCarryWeight')
            : breed?.maxCarryWeight !== undefined
            ? breed.maxCarryWeight
            : host.horseConfig.maxCarryWeight;
        // Its stamina, speed and hit points: the call's, else the breed's.
        const stat = (key, maximum, minimum = 0) =>
          options[key] !== undefined
            ? checkHorseStat(options[key], maximum, `Horse ${key}`, minimum)
            : breed?.[key] ?? null;
        const stats = {
          stamina: stat('stamina', MAX_STAMINA),
          speed: stat('speed', MAX_SPEED, MIN_SPEED),
          health: stat('health', MAX_HIT_POINTS),
        };
        const owner = host.context.getStore();
        if (owner && (host.resources.get(owner.name) !== owner || !owner.active || owner.failing))
          throw new Error('Resource is stopped');
        const h = Object.create(Horse.prototype);
        Object.assign(h, {
          soul,
          ownerId: 0,
          riderId: 0,
          mounted: false,
        });
        Object.defineProperty(h, 'id', { value: host.allocateEntityId(), enumerable: true });
        // Its name, which a script can change: every client's copy of the horse carries the new one
        // An own property, so a spread or JSON of the horse still has it.
        h[horseName] = name;
        Object.defineProperty(h, 'name', {
          enumerable: true,
          get() {
            return this[horseName];
          },
          set(value) {
            host.renameHorse(this, value);
          },
        });
        h[playerPosition] = vector(position);
        h[playerRotation] = rotation(orientation);
        h[playerWorld] = GLOBAL_WORLD;
        // The server chooses the look, so the horse is the same on every client.
        h[horseLook] = look;
        h[horseBreed] = breed ? breedName : null;
        h[horseGear] = tack;
        h[horseGearRows] = {};
        h[horseGearNames] = new Map();
        h[horseGearLocked] = false;
        h[horseKnockOut] = knockOut;
        h[horseCarry] = carry;
        h[horseStats] = stats;
        h[horseDestroyWithOwner] = true;
        h[horseOrder] = host.horseOrders.fresh();
        h[horseLedger] = host.horseHealth.fresh();
        h.resource = host.context.getStore()?.name;
        host.horses.set(h.id, h);
        // Its saddlebags, held by the server.
        h[horseSaddlebags] = host.stashes.saddlebagsFor(h);
        host.bindEntityState(h);
        host.areas.touch('horse', h, h[playerPosition], h[playerWorld]);
        host.bridge.send({
          op: 'horseSpawn',
          id: h.id,
          position: h[playerPosition],
          rotation: h[playerRotation],
          soul,
          name,
          look: lookField(h[horseLook]),
          knockOut,
          maxCarryWeight: h[horseCarry] ?? -1,
          maxStamina: stats.stamina ?? -1,
          speed: stats.speed ?? -1,
        });
        host.bridge.send({ op: 'horseGear', id: h.id, pieces: piecesOf(tack) });
        host.emit('horseSpawn', h);
        return h;
      }
      // The game's sets of horse tack by name (horse_common01, horse_noble05 ...), for setTack and
      // Horse.spawn.
      static gearSets() {
        return gearSets();
      }
      // Every item class a horse wears, of one slot or all of them, saddles first.
      static gearList(slot) {
        return gearList(slot);
      }
      // The breeds of the server's horse config (horses.json), which Horse.spawn takes
      // as its soul or its `breed` option.
      static breeds() {
        return [...host.horseConfig.breeds.keys()];
      }
      get owner() {
        return Player.getById(this.ownerId);
      }
      // The horse rears and throws its rider off, the game's own, seen by everyone: their game does
      // it and its clips go to the others. False with no rider, otherwise true once sent.
      buckRider() {
        const rider = this.rider;
        if (!rider) {
          return false;
        }
        host.bridge.send({ op: 'horseRear', id: rider.id });
        return true;
      }
      // Has a player pull this horse's rider down, the game's own, on both their games. True when the
      // request went out; false when nobody rides it or the attacker is the rider or not near.
      dragRider(attacker) {
        const rider = this.rider;
        if (!attacker || host.players.get(attacker.id) !== attacker) {
          throw new TypeError('Horse.dragRider takes a Player');
        }
        if (!rider || rider === attacker || !attacker.ready || !attacker.position) {
          return false;
        }
        const at = this.position;
        const gap = Math.hypot(
          attacker.position.x - at.x,
          attacker.position.y - at.y,
          attacker.position.z - at.z,
        );
        if (gap > PULL_DOWN_REACH) {
          return false;
        }
        host.bridge.send({ op: 'horsePullDown', attacker: attacker.id, rider: rider.id });
        return true;
      }
      // Dismounts the current rider through their client (horseDismount follows):
      // that player, or null with no rider.
      unseatRider() {
        const rider = this.rider;
        if (!rider) {
          return null;
        }
        return rider.dismount() ? rider : null;
      }
      get rider() {
        return Player.getById(this.riderId);
      }
      // Whether the horse is removed once its owner has left (after the wait of horses/
      // horse_owners.cjs). True by default. With false it is left ownerless the moment its owner
      // disconnects (horseOwnerChanged with a null player) and stays until giveTo hands it on.
      get removedWithOwner() {
        return this[horseDestroyWithOwner];
      }
      set removedWithOwner(value) {
        if (typeof value !== 'boolean') {
          throw new TypeError('Horse removedWithOwner must be a boolean');
        }
        this[horseDestroyWithOwner] = value;
        if (!host.horses.has(this.id)) {
          return;
        }
        // A horse already waiting for an owner who is gone is released now, or it would keep the
        // id of a connection that no longer exists.
        if (!value && this.ownerId && !host.players.has(this.ownerId)) {
          this.giveTo(null);
        } else {
          host.horseOwners.settle(this.ownerId);
        }
      }
      // Makes the horse `player`'s (a connected Player), or nobody's with null.
      giveTo(player) {
        if (player !== null && !host.players.has(player?.id)) {
          throw new Error('Unknown owner');
        }
        const previousOwnerId = this.ownerId;
        this.ownerId = player === null ? 0 : player.id;
        host.horseOwners.owned(this, previousOwnerId);
        host.bridge.send({ op: 'horseOwner', id: this.id, ownerId: this.ownerId });
        // The saddlebags show in the new owner's game, and no longer in the old owner's.
        host.stashes.saddlebagsOwner(this, previousOwnerId);
        host.emit('horseOwnerChanged', this, player);
      }
      // The horse's saddlebags: a Stash the server holds, with the inventory methods
      // and events of any container; its owner moves items in and out in the game's inventory
      // screen. They go with the horse.
      get storage() {
        return this[horseSaddlebags];
      }
      setDimension(world) {
        host.requireLivePlayerCall(this);
        if (!validWorld(world)) throw new TypeError('Invalid dimension');
        this[playerWorld] = world;
        host.areas.touch('horse', this, this[playerPosition], world);
        host.bridge.send({ op: 'horseWorld', id: this.id, dimension: world });
      }
      showOnlyTo(player) {
        host.setPlayerVisibleTo(this, player);
      }
      // What the horse wears by slot: { saddle, head, torso, shoe }, item class names or null.
      get gear() {
        return Object.freeze({ ...this[horseGear] });
      }
      // Whether players' own changes to its gear (in the game's horse inventory screen) are refused,
      // ahead of horseGearChanging; it does not limit scripts.
      get gearFixed() {
        return this[horseGearLocked];
      }
      set gearFixed(value) {
        if (typeof value !== 'boolean') throw new TypeError('Horse gearFixed must be a boolean');
        this[horseGearLocked] = value;
      }
      // Replaces all horse gear on all clients (a preset name, or item classes by
      // slot; {} is a bare horse). False when the horse is gone; throws for what it cannot wear.
      setTack(gear) {
        const tack = gearOf(gear, 'Horse.setTack');
        return host.dressHorse(this, tack, null);
      }
      // Puts one piece in its own slot, keeping the rest. False for an item that is no horse gear,
      // a caparison with no saddle under it, or a horse that is gone.
      fitGear(item) {
        let gear;
        try {
          const name = pieceOf(item, undefined, 'Horse.fitGear');
          gear = { ...this[horseGear], [slotOfItem(name)]: name };
          checkTorso(gear, 'Horse.fitGear');
        } catch (error) {
          if (error instanceof TypeError || error instanceof RangeError) return false;
          throw error;
        }
        return host.dressHorse(this, gear, null);
      }
      // Takes one piece off; the caparison comes off with the saddle it lies on. False when the
      // slot was empty already or the horse is gone.
      takeOffGear(slot) {
        if (!Object.hasOwn(HORSE_GEAR_SLOTS, slot)) {
          throw new TypeError("Horse.takeOffGear takes 'saddle', 'head', 'torso' or 'shoe'");
        }
        if (this[horseGear][slot] === null) return false;
        const gear = { ...this[horseGear], [slot]: null };
        if (slot === 'saddle') gear.torso = null;
        return host.dressHorse(this, gear, null);
      }
      // The components the horse's body is built from: { body, head, hair, tail } (the coat, the
      // breed, the mane and the tail; names of HorseLooks.options). A KCDS addition.
      get look() {
        return this[horseLook] ? Object.freeze({ ...this[horseLook] }) : null;
      }
      // Gives the horse another look on every client; a part left out stays. False for a name
      // the catalog does not have or a horse that is gone.
      setLook(request) {
        const next = lookOf(this[horseLook] ?? randomLook(), request);
        if (next === null || !host.horses.has(this.id)) return false;
        host.requireLivePlayerCall(this);
        this[horseLook] = next;
        host.bridge.send({ op: 'horseLook', id: this.id, look: lookField(next) });
        return true;
      }
      // What a lethal blow does to the horse: false kills it (the game's default), true only knocks
      // it out. Chosen at Horse.spawn and changeable later. A KCDS addition.
      get knockOut() {
        return this[horseKnockOut];
      }
      set knockOut(value) {
        if (typeof value !== 'boolean') throw new TypeError('Horse knockOut must be a boolean');
        if (!host.horses.has(this.id)) return;
        host.requireLivePlayerCall(this);
        this[horseKnockOut] = value;
        host.bridge.send({ op: 'horseKnockOut', id: this.id, knockOut: value });
      }
      // The breed it was spawned as (the server's horse config), or null.
      get breed() {
        return this[horseBreed];
      }
      // The most the horse's saddlebags carry, kg, or null for the game's own: every
      // game's saddlebags refuse what goes over it. The server config's maxCarryWeight at spawn.
      get maxCarryWeight() {
        return this[horseCarry];
      }
      set maxCarryWeight(value) {
        const weight = checkCarryWeight(value, 'Horse maxCarryWeight');
        if (!host.horses.has(this.id)) return;
        this[horseCarry] = weight;
        host.bridge.send({ op: 'horseMaxCarryWeight', id: this.id, weight: weight ?? -1 });
      }
      // The horse's stamina (the game's MaxStamina) and speed (as the inspect screen shows it, 30
      // to 60, the game's horses about 35), or null for the game's own: every game's
      // horse soul answers them (speed as NormalizedRunSpeedBase and the RunSpeedBase it stands
      // for), the inspect screen and the ride go by them.
      get maxStamina() {
        return this[horseStats].stamina;
      }
      // Lets a player ride the horse without owning it (true) or no longer (false): their game
      // offers the plain mount instead of "mount and steal", and the saddlebags stay the owner's.
      // At most MAX_HORSE_RIDERS players. False for a horse that is gone or a full list.
      allowRider(player, allowed = true) {
        if (!(player instanceof host.api.Player)) {
          throw new TypeError('Horse.allowRider takes a Player');
        }
        if (typeof allowed !== 'boolean') {
          throw new TypeError('Horse.allowRider allowed must be a boolean');
        }
        if (!host.horses.has(this.id)) return false;
        const riders = (this[horseRiders] ??= new Set());
        if (allowed && !riders.has(player.id) && riders.size >= MAX_HORSE_RIDERS) return false;
        if (allowed) riders.add(player.id);
        else riders.delete(player.id);
        host.bridge.send({ op: 'horseRiders', id: this.id, riders: [...riders] });
        return true;
      }
      // The players allowed to ride it besides its owner (allowRider) who are connected.
      get riders() {
        return [...(this[horseRiders] ?? [])]
          .map((id) => host.players.get(id))
          .filter((player) => player !== undefined);
      }
      // Its current stamina, as the game that runs it last reported (it falls while galloping and
      // comes back at rest), or null before that game's first report.
      get stamina() {
        return this[horseStamina] ?? null;
      }
      set maxStamina(value) {
        const stamina = checkHorseStat(value, MAX_STAMINA, 'Horse maxStamina');
        if (!host.horses.has(this.id)) return;
        this[horseStats].stamina = stamina;
        host.horseStatsChanged(this);
      }
      get speed() {
        return this[horseStats].speed;
      }
      set speed(value) {
        const speed = checkHorseStat(value, MAX_SPEED, 'Horse speed', MIN_SPEED);
        if (!host.horses.has(this.id)) return;
        this[horseStats].speed = speed;
        host.horseStatsChanged(this);
      }
      // Its hit points: 100 (null) is a horse of the game's. health stays a share of
      // 100; damage the game takes off the horse is scaled by 100 / hitPoints, so a horse of 200
      // loses half as much per blow.
      get hitPoints() {
        return this[horseStats].health;
      }
      set hitPoints(value) {
        this[horseStats].health = checkHorseStat(value, MAX_HIT_POINTS, 'Horse hitPoints');
      }
      // Health: from 0 to maxHealth, what every client shows. Assigning sets it, clamped
      // to maxHealth, and 0 kills (horseDeath); a dead horse ignores the write, revive() brings it
      // back. The simulating client's reports of health the game took change it too (horseDamage).
      get health() {
        return this[horseLedger].health;
      }
      set health(value) {
        host.horseHealth.setHealth(this, value);
      }
      get maxHealth() {
        return HORSE_MAX_HEALTH;
      }
      // False once the horse has died. A knocked-out horse is alive: see knockedOut.
      get alive() {
        return this[horseLedger].state !== DEAD;
      }
      // Whether the horse lies knocked out: a lethal blow does that to a horse with the knockOut
      // option, knockDown() to any alive one. wake() gets it up. A KCDS addition.
      get knockedOut() {
        return this[horseLedger].state === KNOCKED_OUT;
      }
      // Another name of knockedOut: true while the horse lies knocked out.
      get unconscious() {
        return this[horseLedger].state === KNOCKED_OUT;
      }
      // The game's buffs this server put on the horse, by table name, in the order
      // they were added. Every client's body of the horse carries them.
      get buffs() {
        return host.horseBuffs.names(this);
      }
      // Puts a game buff (GUID or table name) on the horse on every client. True when it is on the
      // list afterwards; false if the horse no longer exists or is at its limit of sixteen.
      addBuff(buff) {
        return host.horseBuffs.add(this, buff);
      }
      // Takes one of the server's buffs off the horse. True when it was on the list.
      removeBuff(buff) {
        return host.horseBuffs.remove(this, buff);
      }
      // Whether the server put the buff on the horse (the server's own list).
      hasBuff(buff) {
        return host.horseBuffs.has(this, buff);
      }
      // Takes every buff the server put on the horse off. True when there was one.
      clearBuffs() {
        return host.horseBuffs.clear(this);
      }
      // Kills the horse where it stands (horseDeath with no killer, the reason `scripted`). True
      // when the horse was not dead.
      kill() {
        return host.horseHealth.kill(this);
      }
      // Knocks an alive horse out (horseKnockedOut, reason `scripted`): it lies and nobody rides it
      // until wake(). False for a horse that already lies. A KCDS addition.
      knockDown() {
        return host.horseHealth.knockDown(this);
      }
      // Gets a knocked-out horse up (horseWake), with `health` or the health it has (full when a
      // lethal blow left none). False for a horse that is not knocked out. A KCDS addition.
      wake(health) {
        return host.horseHealth.wake(this, health);
      }
      // Revives a dead horse in place, with full or the given `health` (horseRevive); the
      // clients make its body anew. False for a horse that is not dead.
      revive(health) {
        return host.horseHealth.revive(this, health);
      }
      // How the current order stands: `idle` (none, or a hold), `running` from the moment a moveTo
      // or patrol is given, and the outcome horseIntentDone announced (`reached`, `blocked`,
      // `failed`) after that. Nothing reports progress yet, so it stays `running` until a client
      // does.
      get travelStatus() {
        return this[horseOrder].status;
      }
      // Walks to `position` (a component left out keeps the horse's current one); options.speed
      // `walk` (default), `jog` or `run`, options.radius the arrival distance in metres (1.5). The
      // order travels to every client in the horse's intent field. True when it went out; false
      // for a horse that is gone, dead or ridden, or a destination it cannot use. Throws for an
      // option it cannot use.
      moveTo(position, options) {
        return host.horseOrders.moveTo(this, position, options);
      }
      // Patrols an array of 1 to 256 positions: options.speed as moveTo takes it, options.loop
      // (true by default) and options.waitSeconds (0 to 3600) at each waypoint. True when the
      // order went out; false for a route or waitSeconds it cannot use, or a horse that is gone,
      // dead or ridden. Throws for an option it cannot use.
      patrol(points, options) {
        return host.horseOrders.patrol(this, points, options);
      }
      // Follows a Player, an Npc or another Horse, keeping options.radius metres (3) from it, at
      // options.speed or a pace by the distance, until hold, another order or the target goes
      // A rider keeps control, and the follow goes on once they get off.
      // options.rope: true leads it on a rope from its head to the target's right hand, seen by
      // everyone.
      follow(target, options) {
        return host.horseOrders.follow(this, target, options);
      }
      // The target it follows, or null.
      get following() {
        return host.horseOrders.followed(this);
      }
      // How dirty its coat and tack are, 0 clean to 1, or null for the game's own (4.11 request
      // 3). Everyone sees it. You can assign to it.
      get dirt() {
        return this[horseDirt] ?? null;
      }
      set dirt(value) {
        if (value !== null && (typeof value !== 'number' || !(value >= 0 && value <= 1))) {
          throw new RangeError('Horse dirt is 0 to 1, or null');
        }
        if (!host.horses.has(this.id)) return;
        this[horseDirt] = value;
        host.bridge.send({ op: 'horseDirt', id: this.id, dirt: value === null ? -1 : value });
      }
      // The share of its pace it moves at in every gait, walking to galloping, ridden or led:
      // 0.1 to 2, or null for the game's own. Everyone sees it. You can assign
      // to it.
      get speedScale() {
        return this[horseSpeedScale] ?? null;
      }
      set speedScale(value) {
        const valid =
          value === null ||
          (typeof value === 'number' && value >= SPEED_SCALE_MIN && value <= SPEED_SCALE_MAX);
        if (!valid) {
          throw new RangeError(
            `Horse speedScale is ${SPEED_SCALE_MIN} to ${SPEED_SCALE_MAX}, or null`,
          );
        }
        if (!host.horses.has(this.id)) return;
        this[horseSpeedScale] = value;
        host.bridge.send({
          op: 'horseSpeedScale',
          id: this.id,
          scale: value === null ? -1 : value,
        });
      }
      // Makes it dirtier by `amount` (0 to 1, default 0.25), up to fully dirty. Returns its dirt.
      soil(amount = 0.25) {
        if (typeof amount !== 'number' || !(amount >= 0 && amount <= 1)) {
          throw new RangeError('Horse soil amount is 0 to 1');
        }
        this.dirt = Math.min(1, (this[horseDirt] ?? 0) + amount);
        return this.dirt;
      }
      // Brushes it clean, as a groom does.
      clean() {
        this.dirt = 0;
      }
      // Drops the order and leaves the horse where it stands. A rider keeps control.
      hold() {
        host.horseOrders.hold(this);
      }
      destroy() {
        if (!host.horses.has(this.id)) return;
        host.horseOrders.follows.delete(this);
        host.emit('horseDestroy', this);
        host.horseOwners.destroyed(this);
        host.stashes.destroy(this[horseSaddlebags]);
        host.nameHorsePieces(this, new Map());
        host.horses.delete(this.id);
        host.areas.forget(this.id);
        host.stateStore.destroy(this.id);
        host.bridge.send({ op: 'horseDestroy', id: this.id });
      }
      toString() {
        return `Horse(${this.id})`;
      }
    }
    // A Horse is an Entity: the same dimension, position and rotation members as a Player.
    for (const name of ['dimension', 'position', 'rotation']) {
      Object.defineProperty(
        Horse.prototype,
        name,
        Object.getOwnPropertyDescriptor(Player.prototype, name),
      );
    }
    Object.setPrototypeOf(Horse.prototype, Entity.prototype);
    const Events = {
      on: (name, fn) => host.on(name, fn),
      once: (name, fn) => {
        let off;
        off = host.on(name, (...args) => {
          off();
          return fn(...args);
        });
      },
      off: (name, fn) => {
        for (const h of host.handlers.get(name) ?? []) if (h.fn === fn) h.active = false;
      },
      emit: (name, ...args) => host.emitAsync(name, ...args),
    };
    // A chat colour as the client draws it: 0xRRGGBBAA, from a packed number or a Color (whose
    // channels are 0..1). 0 is the client's default.
    function packedColor(color) {
      if (color === undefined) return 0;
      if (typeof color === 'number') {
        if (!Number.isFinite(color)) throw new TypeError('Chat color must be a finite number');
        return color >>> 0;
      }
      const channels = color && ['r', 'g', 'b', 'a'].map((key) => color[key]);
      if (
        !channels ||
        !channels.every((value) => typeof value === 'number' && Number.isFinite(value))
      )
        throw new TypeError('Chat color must be a Color, or a number packed as 0xRRGGBBAA');
      const [r, g, b, a] = channels.map((value) =>
        Math.round(Math.max(0, Math.min(1, value)) * 255),
      );
      return ((r << 24) | (g << 16) | (b << 8) | a) >>> 0;
    }
    function message(text, options = {}) {
      if (typeof text !== 'string') throw new TypeError('Chat text must be string');
      if (Buffer.byteLength(text) > 2048) throw new RangeError('Chat text too long');
      // The server drops chat text with control characters; say so instead of losing it silently.
      if (/[\x00-\x1f\x7f]/.test(text)) {
        console.warn('Chat text with a line break or control character is not delivered');
      }
      return {
        text,
        author: String(options.author ?? ''),
        color: packedColor(options.color),
      };
    }
    const Chat = {
      sendToAll: (text, options) =>
        host.bridge.send({ op: 'chat', target: 0, ...message(text, options) }),
      sendToPlayer: (player, text, options) => {
        if (!host.players.has(player.id)) return;
        host.bridge.send({ op: 'chat', target: player.id, ...message(text, options) });
      },
      setChatRelay: (value) => {
        host.defaultRelay = !!value;
      },
      relaysChat: () => host.defaultRelay,
    };
    return {
      Entity,
      Player,
      Horse,
      HorseLooks,
      // The world's clock and weather and the things in it (KCDS additions):
      // addInteraction(options, handler).
      World: host.worldState.api(
        {
          addInteraction: (options, handler) => host.worldInteractions.add(options, handler),
          // The points of interest of the level this server runs.
          landmarks: () => landmarks(host.areas.level),
          // The server's own entities near a point.
          replicasAround: (position, radius, dimension) =>
            replicasAround(host.api ?? host.makeAPI(), position, radius, dimension),
          // raycast, traceAll, snapToGround and entitiesAround, asked of a player's game,
          ...host.worldTraces.api(),
          // The level's own items on or off.
          setLevelItemsEnabled: (enabled) => host.worldPickables.setEnabled(enabled),
          // Some of the level's items gone for everyone.
          removeLevelItems: (options) => host.worldPickables.removeArea(options),
          // What the game calls a place.
          setPlaceName: (place, name) => host.placeNames.set(place, name),
          places: () => host.placeNames.list(),
        },
        {
          levelItemsEnabled: () => host.worldPickables.enabled,
          // The level the server runs: 'trosecko', 'kutnohorsko' or 'klaster'.
          level: () => host.statusRoutes.settings.map,
        },
      ),
      // Scripted camera shots for one player.
      Camera: host.cameraShots.api(),
      Voice: host.voiceChat.api(),
      // Conversations in the game's own dialogue list.
      Dialogue: host.dialogues.api(),
      // Vendors on the game's own shop screen.
      Vendor: host.vendors.api(),
      Trade: host.vendors.trades.api(),
      Queue: host.joinQueue.api(),
      Bans: host.bansApi.api(),
      // The game's animations, and the ones made in the Asset/Animation Studio.
      Animations: Object.freeze({ ...animationCatalog.api(), ...host.studioAnimations.api() }),
      Npc: host.npcs.define(Entity),
      PatrolRoute: host.npcs.patrols.routes.define(),
      Area: host.areas.define(),
      Marker: host.markers.define(Entity),
      Blip: host.blips.define(Entity),
      Quest: host.quests.define(Entity),
      // The game's smithing and alchemy recipes.
      Recipe: recipes.define(),
      // The crafting stations, who holds them and the players' sessions.
      Crafting: host.crafting.define((player) => {
        if (!(player instanceof host.api.Player)) {
          throw new TypeError('Crafting takes a Player');
        }
        return player;
      }, host.customCrafting),
      // Dice matches between two players.
      Dice: host.dice.api(),
      // Herb regrowth.
      Gathering: host.gathering.api(),
      Trees: host.trees.api(),
      // The stew pots' stock.
      CookPot: host.cookPots.define((player) => {
        if (!(player instanceof host.api.Player)) {
          throw new TypeError('CookPot takes a Player');
        }
        return player;
      }),
      Dog: host.dogs.define(Entity),
      GroundItem: host.groundItems.define(Entity),
      Prop: host.props.define(Entity),
      ...host.propPreviews.define(),
      SiegeEngine: host.siege.define(Entity),
      Door: host.doors.define(Entity),
      Cart: host.carts.define(Entity),
      Vfx: host.vfx.define(Entity),
      Audio: host.audio.api(),
      Light: host.lights.define(Entity),
      SmartObject: host.smartObjects.define(Entity),
      Gizmo: host.gizmos.define(),
      LevelEdit: host.levelEdits.define(),
      Gate: host.gates.define(),
      Grave: host.graves.define(),
      Tabletop: host.tabletops.define(),
      TabletopPiece: host.tabletops.definePiece(),
      TabletopDeck: host.tabletops.decks.defineDeck(),
      DeckPiece: host.tabletops.decks.definePiece(),
      WorldResource: host.worldResources.define(),
      Stash: host.stashes.define(Entity),
      SiegeLadder: host.siegeLadders.define(Entity),
      StonePile: host.stonePiles.define(Entity),
      CombatMarker: host.combatMarker.api(),
      RestrictedAreaMarker: host.restrictedAreaMarker.api(),
      ThirdPerson: host.thirdPerson.api(),
      SharedState: host.stateStore.SharedState,
      Events,
      Chat,
      Appearances,
      PlayerStat,
      DerivedPlayerStat,
      Progression: this.makeProgression(),
      Buffs: this.makeBuffs(),
      Inventory: Object.freeze({
        ...this.playerInventories.api(),
        // KCDS: whether the game marks items as stolen at all.
        get stolenMarks() {
          return host.pickpocketing.stolenMarks;
        },
        setStolenMarks: (enabled) => host.pickpocketing.setStolenMarks(enabled),
      }),
      Pickpocketing: this.pickpocketing.api(),
      Locales: this.locales.api(),
      Freecam: this.freecam.api(),
      Needs: this.needs.api(),
      Items: Object.freeze({ ...this.customItems.api(), ...this.itemOverrides.api() }),
      Interactions: this.interactionRules.api(),
      Vector2,
      Vector3,
      Vector4,
      Quaternion,
      Color,
    };
  }
  // Progression: the game's catalog, and the server's rules on what players earn from play.
  makeProgression() {
    const host = this;
    const rules = this.progressionRules;
    return Object.freeze({
      ...progressionCatalog,
      get gameXp() {
        return rules.gameXp;
      },
      setGameXp(enabled) {
        if (rules.setGameXp(enabled)) host.sendNativeXp([...host.players.values()]);
      },
      setXpGain: (track, rate) => rules.setXpGain(track, rate),
      // The same rule as a percentage of the normal gain: 25 grants a quarter
      // of every XP gain from play on that track ('*' for all), 100 the game's own.
      setXpPercent: (track, percent) => {
        if (typeof percent !== 'number' || !Number.isFinite(percent)) {
          throw new RangeError('setXpPercent takes a percentage from 0 to 100000');
        }
        rules.setXpGain(track, percent / 100);
      },
      xpPercent: (track) => rules.xpRateOf(track) * 100,
      setXpCap: (track, xpPerMinute) => rules.setXpCap(track, xpPerMinute),
      setMaxLevel: (track, level) => rules.setMaxLevel(track, level),
      get lockedPerks() {
        return rules.lockedPerks;
      },
      lockPerks(perks) {
        if (rules.setBlocked(perks, true)) host.sendBlockedPerks([...host.players.values()]);
      },
      unlockPerks(perks) {
        if (rules.setBlocked(perks, false)) host.sendBlockedPerks([...host.players.values()]);
      },
      // What every player's game calls a perk and how it describes it: its name,
      // description and lore text in the perk tree, in place of the game's own; null restores one.
      // Their texts travel as the items' names do (the game's own localization keys answered from
      // the server's table), so a player who joins later reads them too.
      setPerkText: (perk, texts) => host.setPerkText(perk, texts),
    });
  }
  setPerkText(perk, texts) {
    const found = lookupPerk(perk);
    if (!found || typeof found.uiName !== 'string' || !found.uiName.endsWith('_name')) {
      throw new TypeError(`setPerkText: unknown perk: ${String(perk)}`);
    }
    if (texts === null || typeof texts !== 'object' || Array.isArray(texts)) {
      throw new TypeError('setPerkText takes { name, description, lore }');
    }
    const base = found.uiName.slice(0, -'_name'.length);
    const keys = { name: found.uiName, description: `${base}_desc`, lore: `${base}_lore_desc` };
    const changes = [];
    for (const [field, key] of Object.entries(keys)) {
      if (!Object.hasOwn(texts, field)) {
        continue;
      }
      const text = texts[field];
      if (text !== null) {
        const limit = field === 'name' ? PERK_NAME_BYTES : PERK_TEXT_BYTES;
        if (
          typeof text !== 'string' ||
          text.length === 0 ||
          Buffer.byteLength(text) > limit ||
          /[\x00-\x08\x0b-\x1f\x7f]/.test(text)
        ) {
          throw new TypeError(`setPerkText ${field}: 1 to ${limit} bytes of text, or null`);
        }
      }
      if (text === null) {
        this.playerInventories.itemNames.delete(key);
      } else {
        this.playerInventories.itemNames.set(key, text);
      }
      changes.push([key, text ?? '']);
    }
    if (changes.length > 0) {
      this.playerInventories.sendNames(0, false, changes);
    }
    return changes.length > 0;
  }
  // Buffs: the game's status-effect table, and which kinds of effect this server decides.
  makeBuffs() {
    const host = this;
    return Object.freeze({
      get classes() {
        return buffCatalog.classes;
      },
      get tags() {
        return buffCatalog.tags;
      },
      // The stats a custom buff's modifiers may name.
      get modifiers() {
        return [...BUFF_MODIFIERS];
      },
      find: (query) => host.customBuffs.find(query) ?? buffCatalog.find(query),
      // A buff this resource defines; see buffs/custom_buffs.cjs.
      register: host.customBuffs.api(),
      claim(classNames) {
        const ids = claimableClassIds(classNames);
        host.claimedBuffClasses = [...new Set(classNames)];
        host.sendBuffClaim([...host.players.values()], ids);
        return [...host.claimedBuffClasses];
      },
    });
  }
  // Shows a player's inventory in their game: the whole stock with its revision, which their
  // client brings the game's own inventory to and reports back once it shows it.
  projectInventory(player, inventory) {
    if (!player.ready) return;
    // The names of single rows arrive before the stock that shows them.
    this.playerInventories.sendRowNames(player, inventory);
    const rows = [...inventory.rows.values()].map((row) => [
      row.id,
      this.customItems.shownClassOf(row),
      row.amount,
      row.props.quality,
      row.props.health,
      row.equipped,
    ]);
    const message = {
      op: 'inventoryShow',
      id: player.id,
      revision: inventory.revision,
      replace: this.playerInventories.replaces(player),
      rows,
    };
    // Quickslots a resource restored go once, with the stock that holds their rows.
    const quickslots = inventory.takeQuickslotsRestore();
    if (quickslots) message.quickslots = quickslots;
    this.bridge.send(message);
  }
  // The verdict on one of a player's uses: 'refused', 'native' or 'plain'.
  sendConsumeVerdict(player, request, verdict) {
    if (this.players.get(player.id) !== player) return;
    this.bridge.send({ op: 'inventoryConsumeReply', id: player.id, request, verdict });
  }
  // Tells players' clients which kinds of effect their games may not apply by themselves.
  sendBuffClaim(players, ids = claimableClassIds(this.claimedBuffClasses)) {
    for (const player of players) {
      if (!player.ready) continue;
      this.bridge.send({ op: 'buffOrder', id: player.id, kind: 'claim', classes: ids });
    }
  }
  // Whether the player is connected and has a world their client can be ordered in.
  canOrderPlayer(player) {
    if (this.players.get(player.id) !== player) {
      return false;
    }
    this.requireLivePlayerCall(player);
    return player.ready;
  }
  // Sends one buff order to the player's own client. `removals`: the buffs it takes off, so the
  // report that shows them gone names the server. False for a player without a world.
  sendBuffOrder(player, order, removals = []) {
    if (!this.canOrderPlayer(player)) {
      return false;
    }
    player[serverBuffRemovals] ??= new Set();
    for (const entry of removals) {
      player[serverBuffRemovals].add(entry);
    }
    this.bridge.send({ op: 'buffOrder', id: player.id, ...order });
    return true;
  }
  // The status effects a player's client reported (only when they changed): playerBuffAdded for
  // each new one (everything at the first report), playerBuffRemoved for each one gone.
  updateBuffs(id, raw) {
    const p = this.players.get(id);
    if (!p) return;
    const { kept, added, removed } = applyReport(
      p[playerBuffs],
      buffsFromReport(raw),
      this.buffClock(),
    );
    const injuredBefore = injuriesOf(p[playerBuffs]);
    p[playerBuffs] = kept;
    const removals = p[serverBuffRemovals] ?? new Set();
    for (const entry of removed) {
      const reason = removals.delete(entry) ? 'server' : 'expired';
      this.emit('playerBuffRemoved', p, entry.buff.name, reason);
    }
    for (const entry of added) {
      this.emit('playerBuffAdded', p, entry.buff.name, entry.server ? 'server' : 'native');
    }
    // A limb injured or healed: the flag changed with this report.
    const { injured, healed } = injuryChanges(injuredBefore, injuriesOf(kept));
    for (const limb of healed) {
      this.emit('playerInjuryHealed', p, limb);
    }
    for (const limb of injured) {
      this.emit('playerInjured', p, limb);
    }
  }
  // An effect of a claimed kind the player's game tried to apply and their client turned down:
  // playerBuffBlocked, at most twice a second per player and buff.
  buffBlocked({ id, buff }) {
    const player = this.players.get(id);
    const info = lookupBuff(buff);
    if (!player || !player.ready || !info) return;
    const now = this.buffClock();
    const key = `${id}:${info.name}`;
    const recent = (this.buffBlocks.get(key) ?? []).filter((at) => now - at < 1);
    if (recent.length >= BUFF_BLOCKED_PER_SECOND) return;
    recent.push(now);
    this.buffBlocks.set(key, recent);
    this.emit('playerBuffBlocked', player, info.name);
  }
  // Tells players' clients which perks their games may not grant by themselves.
  sendBlockedPerks(players) {
    const perks = this.progressionRules.lockedPerks.map((name) => lookupPerk(name).id);
    for (const player of players) {
      if (!player.ready) continue;
      this.bridge.send({ op: 'progressionOrder', id: player.id, kind: 'lockPerks', perks });
    }
  }
  // A perk a player confirmed on the perk screen and their game allows, held back for the server:
  // the block list, then playerPerkLearning. What is allowed goes back as a LearnPerk order (the
  // game spends the point), what is not as a RefusePerk order (progressionPerkRefused).
  gatePerk({ id, perk }) {
    const player = this.players.get(id);
    const info = lookupPerk(perk);
    if (!player || !player.ready || !info) return;
    const refuse = (reason) => {
      this.bridge.send({ op: 'progressionOrder', id, kind: 'refusePerk', perk: info.id, reason });
    };
    if (!perkScreenPerk(info)) return refuse('hidden');
    if (this.progressionRules.isBlocked(info.name)) return refuse('blocked');
    if (!this.emit('playerPerkLearning', player, info.name)) return refuse('script');
    player[serverPerkOrders] ??= new Map();
    player[serverPerkOrders].set(info.name, 'learned');
    this.bridge.send({ op: 'progressionOrder', id, kind: 'learnPerk', perk: info.id });
  }
  // Tells players' clients whether their games may earn XP from play.
  sendNativeXp(players) {
    for (const player of players) {
      if (!player.ready) continue;
      this.bridge.send({
        op: 'progressionOrder',
        id: player.id,
        kind: 'gameXp',
        enabled: this.progressionRules.gameXp,
      });
    }
  }
  // XP a player's game produced and held back for the server: playerXpGaining, then the rules;
  // what is granted goes back as an AddXp order.
  gateXp({ id, track, xp, multipliers, source }) {
    const player = this.players.get(id);
    const info = TRACKS[track];
    const valid =
      Number.isInteger(track) && typeof xp === 'number' && Number.isFinite(xp) && xp > 0;
    if (!player || !player.ready || !info || !valid || !this.progressionRules.gameXp) return;
    const cause = typeof source === 'string' ? source : '';
    if (!this.emit('playerXpGaining', player, info.name, xp, cause)) return;
    const state = player[playerProgression]?.tracks[track];
    const granted = this.progressionRules.grant(id, info.name, xp, multipliers === true, state);
    if (!granted) return;
    this.bridge.send({
      op: 'progressionOrder',
      id,
      kind: 'addXp',
      track,
      xp: granted.xp,
      multipliers: granted.multipliers,
    });
  }
  // The ledger's share of the server's own XP orders: how far each track may now rise.
  allowOrder(player, order) {
    const rules = this.progressionRules;
    const tracks = player[playerProgression]?.tracks;
    const current = (index) => {
      const state = tracks?.[index];
      return state ? totalXp(TRACKS[index], state.level) + state.xp : 0;
    };
    if (order.kind === 'addXp') {
      rules.allow(player.id, TRACKS[order.track].name, order.xp * MAX_MULTIPLIER);
    } else if (order.kind === 'setLevel') {
      const track = TRACKS[order.track];
      const level = tracks?.[order.track]?.level ?? 0;
      rules.allow(player.id, track.name, totalXp(track, order.level) - totalXp(track, level));
    } else if (order.kind === 'setTrack') {
      // Lowering earns nothing; only a rise over the track's present total is allowed.
      const track = TRACKS[order.track];
      const rise = totalXp(track, order.level) + order.xp - current(order.track);
      if (rise > 0) rules.allow(player.id, track.name, rise);
    } else if (order.kind === 'restore') {
      order.snapshot.tracks.forEach(([level, xp], index) => {
        const missing = totalXp(TRACKS[index], level) + xp - current(index);
        if (missing > 0) rules.allow(player.id, TRACKS[index].name, missing);
      });
    }
  }
  on(name, fn) {
    if (typeof name !== 'string' || typeof fn !== 'function')
      throw new TypeError('Event subscription');
    const owner = this.context.getStore();
    const h = { fn, resource: owner?.name, owner, active: true };
    const list = this.handlers.get(name) ?? [];
    list.push(h);
    this.handlers.set(name, list);
    return () => {
      h.active = false;
    };
  }
  emit(name, ...args) {
    let accepted = true;
    for (const h of [...(this.handlers.get(name) ?? [])]) {
      if (!h.active) continue;
      try {
        const result = this.context.run(h.owner, () => h.fn(...args));
        if (result === false) accepted = false;
        if (result?.then) result.catch((e) => this.bridge.log(h.resource, e.stack ?? String(e)));
      } catch (e) {
        this.bridge.log(h.resource, e.stack ?? String(e));
      }
    }
    return this.services.emitSync(name, args) && accepted;
  }
  // As emit, and hands what each handler returned (anything but undefined and false) to
  // collect, in the order the handlers ran: npcHarvest's own yield.
  emitCollecting(name, collect, ...args) {
    let accepted = true;
    for (const h of [...(this.handlers.get(name) ?? [])]) {
      if (!h.active) continue;
      try {
        const result = this.context.run(h.owner, () => h.fn(...args));
        if (result === false) accepted = false;
        else if (result !== undefined && !result?.then) collect(result, h.resource);
        if (result?.then) result.catch((e) => this.bridge.log(h.resource, e.stack ?? String(e)));
      } catch (e) {
        this.bridge.log(h.resource, e.stack ?? String(e));
      }
    }
    return this.services.emitSync(name, args) && accepted;
  }
  async emitAsync(name, ...args) {
    const promises = [];
    for (const h of [...(this.handlers.get(name) ?? [])]) {
      if (!h.active) continue;
      try {
        promises.push(Promise.resolve(this.context.run(h.owner, () => h.fn(...args))));
      } catch (e) {
        this.bridge.log(h.resource, e.stack ?? String(e));
        promises.push(Promise.reject(e));
      }
    }
    promises.push(this.services.emit('shared', name, args));
    const results = await Promise.allSettled(promises);
    // The shared bus already aggregates its handler failures; flatten them so the emitter sees one
    // AggregateError carrying every handler error, the same shape as on the client.
    const errors = [];
    for (const result of results) {
      if (result.status !== 'rejected') {
        continue;
      }
      if (result.reason instanceof AggregateError && Array.isArray(result.reason.errors)) {
        errors.push(...result.reason.errors);
      } else {
        errors.push(result.reason);
      }
    }
    if (errors.length) throw new AggregateError(errors, `Event ${name}`);
  }
  bindEntityState(entity) {
    Object.defineProperty(entity, 'id', {
      value: entity.id,
      enumerable: true,
      writable: false,
      configurable: false,
    });
    this.stateStore.attach(entity.id);
    const bag = this.stateStore.bag(entity.id);
    Object.defineProperty(entity, 'state', {
      enumerable: true,
      get: () => this.stateStore.forOwner(bag, this.context.getStore()),
    });
  }
  updatePlayer(data) {
    let p = this.players.get(data.id);
    const fresh = !p;
    if (fresh) {
      p = Object.create(this.api.Player.prototype);
      Object.defineProperty(p, 'id', { value: data.id, enumerable: true });
      this.players.set(data.id, p);
      this.bindEntityState(p);
    }
    const before = p.ready;
    Object.assign(p, {
      nickname: String(data.nickname ?? `Player ${data.id}`),
      // Connection slot (reused after a player leaves), unlike the never-reused network id.
      playerIndex: data.playerIndex ?? 0xffff,
      ready: !!data.ready,
    });
    p[playerPosition] = vector(data.position);
    p[playerRotation] = rotation(data.rotation);
    this.noteTeleportArrival(p);
    p[playerVelocity] = data.velocity ? vector(data.velocity) : new Vector3(0, 0, 0);
    p[playerLookDirection] = data.gaze ? vector(data.gaze) : new Vector3(0, 0, 0);
    p[playerPosture] = postureFromNative(data);
    p[playerEquipment] = Array.isArray(data.equipment) ? data.equipment.map(String) : [];
    p[playerHands] = Array.isArray(data.hands) ? data.hands.map(String) : ['', ''];
    p[playerLanguage] = typeof data.language === 'string' ? data.language.toLowerCase() : '';
    p[playerGrime] = grimeFrom(data.grime);
    p[playerSeated] = data.seated === true;
    const stanceBefore = p[playerStance] ?? 255;
    p[playerStance] =
      Number.isInteger(data.stance) && data.stance >= 0 && data.stance <= 255 ? data.stance : 255;
    if (p.ready && before && p[playerStance] !== stanceBefore) {
      this.emit('playerStanceChanged', p, p[playerStance], stanceBefore);
    }
    p[playerAppearance] = appearanceFrom(data.appearance);
    this.updateLegsCover(p);
    p[playerVisible] = data.visible !== false;
    p[playerVisorClosed] = data.visorDown === true;
    p[playerDisguise] =
      typeof data.disguise === 'string' && data.disguise !== '' ? data.disguise : null;
    const previousStats = p[playerStats];
    p[playerStats] = statsFrom(data);
    // A report produced before a pending setDimension reached the native server is stale.
    if (!p[pendingWorldRequest]) {
      p[playerWorld] = validWorld(data.dimension) ? data.dimension : GLOBAL_WORLD;
    }
    const wasWorking = this.crafting.working.has(p.id);
    this.crafting.observe(p, data.station, p[playerWorld]);
    // A player who starts working at a station cannot be fought: the NPCs fighting them stop
    // as for a dice match.
    if (!wasWorking && this.crafting.working.has(p.id)) {
      this.npcs?.targetDown(p);
    }
    // The areas and markers hear of a player who is in the world and has moved.
    if (p.ready) this.playerPlaced(p, p[playerPosition], p[playerWorld]);
    // A frozen player's body that moved anyway goes back.
    this.frozenPlayers.moved(p);
    // Lifecycle order per connection: playerConnect (body exists, still loading) and
    // playerSpawning (its client waits behind the loading screen), then playerSpawned (standing in
    // the loaded world) and playerReady (the game took its loading screen down). Each fires once.
    if (fresh) {
      // Their inventory exists from playerConnect, so a handler can restore a saved one there.
      this.playerInventories.open(p);
      this.emit('playerConnect', p);
      this.emit('playerSpawning', p);
    }
    if (p.ready && !before) {
      // A server that keeps XP from play to itself, or blocks perks, tells the player's game at
      // once.
      if (!this.progressionRules.gameXp) this.sendNativeXp([p]);
      if (this.progressionRules.lockedPerks.length) this.sendBlockedPerks([p]);
      // A server that claimed kinds of effect tells the player's game at once.
      if (this.claimedBuffClasses.length) this.sendBuffClaim([p]);
      // Their game makes the item types resources registered, then shows the inventory a resource
      // restored; an untouched one takes its census.
      this.customItems.sendAll(p);
      this.itemOverrides.arrived(p);
      this.playerInventories.arrived(p);
      this.stashes.arrived(p);
      // The level's items others took up are gone from their world too.
      this.worldPickables.arrived(p);
      // And whether its game offers Rob on other players.
      this.pickpocketing.arrived(p);
      this.bodyCarries.arrived(p);
      this.freecam.arrived(p);
      // A server with the needs off tells the player's game at once and fills them, before a
      // resource's playerSpawned and playerReady handlers can set stored values.
      this.needs.arrived(p);
      // The animation a resource gave their body is put on the new one.
      this.playerAnimations.arrived(p);
      // And the pillory a resource put them into.
      this.pillories.arrived(p);
      // And the cuffs a resource put on them.
      this.cuffs.arrived(p);
      // And a freeze.
      this.frozenPlayers.arrived(p);
      this.playerAttachments.arrived();
      // And a NoClip a resource forbade them.
      this.noclipRules.arrived(p);
      // And the game's own interactions the server allows them.
      this.interactionRules.arrived(p);
      // A conversation of the world they left is over.
      this.dialogues.arrived(p);
      // So is a trade.
      this.vendors.arrived(p);
      // And the interactions resources put on things in the world.
      this.worldInteractions.arrived(p);
      // A server that hides the combat marker tells the player's game at once.
      this.combatMarker.arrived(p);
      // The markers and blips meant for them are drawn by their game.
      this.mapPresence.arrived(p);
      // And the world's time and weather, which the server owns.
      this.worldState.arrived(p);
      // And the plants picked in their dimension.
      this.gathering.arrived(p);
      // And how the stew pots of their dimension look.
      this.cookPots.arrived(p);
      // The recipes resources registered, for their game's own recipe tables.
      for (const table of recipes.registeredTables()) {
        this.sendRecipeTable(table, p.id);
      }
      // The props, effects and carts the server put in the world, and the player's own previews.
      this.props.arrived(p);
      // And the smart objects scripts placed in their world.
      this.smartObjects.arrived(p);
      // And the doors scripts hung in their world.
      this.doors.arrived(p);
      this.vfx.arrived(p);
      this.lights.arrived(p);
      this.levelEdits.arrived(p);
      this.gates.arrived(p);
      this.graves.arrived(p);
      this.carts.arrived(p);
      this.propPreviews.arrived(p);
      this.siege.arrived(p);
      this.siegeLadders.arrived(p);
      this.stonePiles.arrived(p);
      // And the restricted-area marker the same way.
      this.restrictedAreaMarker.arrived(p);
      // And their nameplate the same way.
      this.nameplates.arrived(p);
      // And the third-person mode the same way.
      this.thirdPerson.arrived(p);
      // And calls items what the server named them.
      this.playerInventories.sendAllNames(p);
      // A placement a playerSpawning handler chose lands as the body comes into the world.
      const pending = p[pendingSpawn];
      p[pendingSpawn] = null;
      if (pending) this.placePlayer(p, pending.position, pending.rotation);
      this.emit('playerSpawned', p);
    }
    if (p.ready && data.gameplayReady && !p[playerPlaying]) {
      p[playerPlaying] = true;
      this.emit('playerReady', p);
    }
    this.carries.update(p, data.carry);
    // One report per player per server tick, so one event carries every change of that tick.
    const changes = statChanges(previousStats, p[playerStats]);
    if (changes.length) {
      this.emit('playerStatsChanged', p, changes);
    }
    return p;
  }
  // A player's progression as their client reported it (sent apart from the player report, only
  // when it changed). Returns the parsed progression for the clients' directories, or null for a
  // player the host does not know.
  updateProgression(id, raw) {
    const p = this.players.get(id);
    if (!p) return null;
    const previous = p[playerProgression];
    p[playerProgression] = progressionFromReport(raw);
    const changes = progressionChanges(previous, p[playerProgression]);
    const nativelyAdded = this.emitProgression(p, changes);
    // Progression the server never granted: the report is kept (the game cannot take a level
    // back); what happens to the player is the resources' call.
    const rules = this.progressionRules;
    const reasons = [
      ...rules.check(id, changes.xp),
      ...rules.checkPerks(previous, p[playerProgression], nativelyAdded),
    ];
    for (const reason of reasons) {
      this.emit('playerProgressionRejected', p, reason);
    }
    return p[playerProgression];
  }
  // The progression events of one report: XP landed, levels reached, perks added and removed.
  // Returns the perks the game added by itself.
  emitProgression(player, changes) {
    for (const { track, xp } of changes.xp) {
      this.emit('playerXpGained', player, track, xp);
    }
    for (const { track, level, perkPoints } of changes.levels) {
      this.emit('playerLevelUp', player, track, level, perkPoints);
    }
    const ordered = player[serverPerkOrders] ?? new Map();
    const sourceOf = (perk, change) => {
      const order = ordered.get(perk);
      if (order === change) {
        ordered.delete(perk);
        return 'server';
      }
      if (change === 'added' && order === 'learned') {
        ordered.delete(perk);
        return 'learned';
      }
      return 'native';
    };
    const nativelyAdded = [];
    for (const perk of changes.added) {
      const source = sourceOf(perk, 'added');
      if (source === 'native') nativelyAdded.push(perk);
      this.emit('playerPerkAdded', player, perk, source);
    }
    for (const perk of changes.removed) {
      this.emit('playerPerkRemoved', player, perk, sourceOf(perk, 'removed'));
    }
    return nativelyAdded;
  }
  // A horse's new tack: every client dresses its body in it, and horseGearChanged
  // follows with the player who changed it (null for a script). False when the horse is gone.
  // Horse.name = value: the horse's network copy on every client takes the name.
  renameHorse(horse, name) {
    if (typeof name !== 'string') {
      throw new TypeError('Horse name must be a string');
    }
    if (this.horses.get(horse.id) !== horse) {
      return;
    }
    this.requireLivePlayerCall(horse);
    horse[horseName] = name;
    this.bridge.send({ op: 'horseName', id: horse.id, name });
  }
  // `rows`: by slot, the metadata of the row each worn piece came from (playerHorseGear); a
  // script's change (none given) keeps it for the slots whose piece stays, and drops it for the
  // rest.
  dressHorse(horse, gear, player, rows = null) {
    if (this.horses.get(horse.id) !== horse) {
      return false;
    }
    this.requireLivePlayerCall(horse);
    const before = horse[horseGear];
    let kept = rows;
    if (kept === null) {
      kept = {};
      for (const slot of Object.keys(HORSE_GEAR_SLOTS)) {
        const row = horse[horseGearRows][slot];
        if (row && gear[slot] !== null && gear[slot] === before[slot]) {
          kept[slot] = row;
        }
      }
    }
    horse[horseGear] = gear;
    horse[horseGearRows] = kept;
    this.bridge.send({ op: 'horseGear', id: horse.id, pieces: piecesOf(gear, kept) });
    this.nameHorsePieces(horse, pieceNames(horse.id, gear, kept));
    this.emit('horseGearChanged', horse, player, Object.freeze({ ...gear }));
    return true;
  }
  // The names a horse's worn pieces show (horses/gear.cjs pieceNames), to every player: the
  // changed ones, and '' for the keys no piece holds any more.
  nameHorsePieces(horse, names) {
    const sent = horse[horseGearNames] ?? new Map();
    const changes = [];
    for (const [key, text] of names) {
      if (sent.get(key) !== text) {
        this.playerInventories.itemNames.set(key, text);
        changes.push([key, text]);
      }
    }
    for (const key of sent.keys()) {
      if (!names.has(key)) {
        this.playerInventories.itemNames.delete(key);
        changes.push([key, '']);
      }
    }
    horse[horseGearNames] = names;
    if (changes.length > 0) {
      this.playerInventories.sendNames(0, false, changes);
    }
  }
  // A horse's owner changed its tack in the game's own horse inventory screen: put on a
  // piece from a row (`row`, of their inventory, or of the horse's saddlebags with `fromBags`),
  // or took a worn piece off (`item`, its class). Their game changed nothing itself. Refused while
  // gearFixed, or when a horseGearChanging handler returns false: every copy of the horse keeps
  // what it wears. Else the piece put on leaves its row, and what came off (the piece it replaced,
  // a caparison with its saddle) goes into the saddlebags, as in the game's screen, or into the
  // player's inventory when the bags refuse it. False when refused.
  playerHorseGear(player, horseId, { row = null, fromBags = false, item = null }) {
    const horse = this.horses.get(horseId);
    if (!player || this.players.get(player.id) !== player || !horse) {
      return false;
    }
    if (horse.ownerId !== player.id) {
      return false;
    }
    const bags = horse[horseSaddlebags];
    const before = horse[horseGear];
    const beforeRows = horse[horseGearRows];
    const gear = { ...before };
    const rows = { ...beforeRows };
    let source = null;
    if (row !== null) {
      source = fromBags ? bags?.stock : this.playerInventories.entryOf(player)?.inventory;
      const piece = source?.rows.get(row);
      const slot = piece ? slotOfItem(piece.itemClass.id) : null;
      if (!slot) {
        return false;
      }
      // The row's own properties go on with it (its name, description, quality, buffs): the
      // same class with other properties is another piece.
      const metadata = metadataOf(piece.itemClass, piece.props);
      const own = Object.keys(metadata).length > 0 ? metadata : null;
      const same = JSON.stringify(own) === JSON.stringify(beforeRows[slot] ?? null);
      if (before[slot] === piece.itemClass.name && same) {
        return false;
      }
      gear[slot] = piece.itemClass.name;
      rows[slot] = own;
    } else {
      const slot = item ? slotOfItem(item) : null;
      if (!slot || before[slot] === null || pieceOf(item, slot, 'horse gear') !== before[slot]) {
        return false;
      }
      gear[slot] = null;
      rows[slot] = null;
      if (slot === 'saddle') {
        gear.torso = null;
        rows.torso = null;
      }
    }
    if (gear.torso !== null && gear.saddle === null) {
      return false;
    }
    if (
      horse[horseGearLocked] ||
      !this.emit('horseGearChanging', horse, player, Object.freeze({ ...gear }))
    ) {
      return false;
    }
    // The change may have been made meanwhile by a handler (setTack): this one is then stale.
    if (horse[horseGear] !== before || this.horses.get(horseId) !== horse) {
      return false;
    }
    if (source !== null) {
      const units = { units: [{ id: row, amount: 1 }] };
      const removed = fromBags
        ? this.changeBags(bags, player, (stock) => stock.remove(units))
        : this.playerInventories.operate(player, (inventory) => inventory.remove(units));
      if (!removed.ok) {
        return false;
      }
    }
    for (const slot of Object.keys(HORSE_GEAR_SLOTS)) {
      if (before[slot] === null) {
        continue;
      }
      if (before[slot] === gear[slot] && rows[slot] === beforeRows[slot]) {
        continue;
      }
      // What came off goes back with the properties of its row (a named saddle stays itself).
      const request = { item: before[slot], amount: 1 };
      if (beforeRows[slot]) {
        request.metadata = beforeRows[slot];
      }
      const kept = bags && this.changeBags(bags, player, (stock) => stock.add(request)).ok;
      if (!kept) {
        this.playerInventories.operate(player, (inventory) => inventory.add(request), {
          notify: true,
        });
      }
    }
    return this.dressHorse(horse, gear, player, rows);
  }
  // A change of a horse's saddlebags' stock by its owner: stashInventoryChanged, and the bags
  // shown again. The change's result.
  changeBags(bags, player, operation) {
    const { result, change } = operation(bags.stock);
    if (change) {
      this.emit('stashInventoryChanged', bags, player, change);
    }
    this.stashes.show(bags);
    return result;
  }
  // Mutations from a stopped resource or through a stale Player or Horse handle never reach native.
  // Ends the studio animation a player plays by name: the body stops it and
  // playerAnimationEnd(player, name, reason) follows. False when they play none by name.
  endStudioAnimation(player, reason) {
    const name = this.studioAnimations.playing.get(player);
    if (name === undefined) {
      return false;
    }
    this.studioAnimations.playing.delete(player);
    player.setPoseAnimation(null);
    // Held while it played (lockMovement): free again.
    if (this.studioAnimations.held.has(player)) {
      this.studioAnimations.held.delete(player);
      this.frozenPlayers.set(player, false);
    }
    this.emit('playerAnimationEnd', player, name, reason);
    return true;
  }

  requireLivePlayerCall(entity) {
    const owner = this.context.getStore();
    if (owner && (this.resources.get(owner.name) !== owner || !owner.active || owner.failing)) {
      throw new Error('Resource is stopped');
    }
    if (!entity) {
      throw new Error('Unknown player');
    }
    if (entity instanceof this.api.Horse) {
      if (this.horses.get(entity.id) !== entity) throw new Error('Unknown horse');
      return;
    }
    if (entity instanceof this.api.Npc) {
      if (!this.npcs.live(entity)) {
        throw new Error('Unknown NPC');
      }
      return;
    }
    if (this.players.get(entity.id) !== entity) {
      throw new Error('Unknown player');
    }
  }
  addEntityIds(block) {
    const valid =
      block &&
      Number.isSafeInteger(block.first) &&
      Number.isSafeInteger(block.count) &&
      block.first >= 1 &&
      block.count >= 1 &&
      block.first + block.count - 1 <= 0xffffffff;
    if (!valid) return;
    this.entityIds.push({ next: block.first, end: block.first + block.count });
    this.entityIdsRequested = false;
  }
  allocateEntityId() {
    while (this.entityIds.length && this.entityIds[0].next >= this.entityIds[0].end) {
      this.entityIds.shift();
    }
    const block = this.entityIds[0];
    if (!block) {
      throw new Error('No entity ids available yet; the server has not answered');
    }
    const id = block.next;
    block.next += 1;
    const left = this.entityIds.reduce((sum, b) => sum + (b.end - b.next), 0);
    if (left < 1024 && !this.entityIdsRequested) {
      this.entityIdsRequested = true;
      this.bridge.send({ op: 'entityIdsRequest' });
    }
    return id;
  }
  // Native report of a horse: the authoritative transform, dimension and rider.
  updateHorse(data) {
    const h = this.horses.get(data.id);
    if (!h) return;
    h[playerPosition] = vector(data.position);
    h[playerRotation] = rotation(data.rotation);
    if (validWorld(data.dimension)) h[playerWorld] = data.dimension;
    this.areas.touch('horse', h, h[playerPosition], h[playerWorld]);
    // Reports come only when something changed. The rider left the saddle, or a new one sat in
    // it: a rider who has left the server is null. horseMounting decides first; a
    // refused rider is told to get off, steals nothing, and their ride raises neither horseMount
    // nor, when they are off again, horseDismount.
    const previousRider = h.riderId;
    if (data.riderId !== previousRider) {
      const rider = data.riderId ? this.players.get(data.riderId) ?? null : null;
      const refused = rider !== null && !this.emit('horseMounting', h, rider);
      if (previousRider && h[horseRefusedRider] !== previousRider) {
        this.emit('horseDismount', h, this.players.get(previousRider) ?? null);
      }
      h[horseRefusedRider] = refused ? data.riderId : 0;
      // A new rider who is not the owner stole it.
      if (!refused) this.horseOwners.riderChanged(h, data.riderId);
      h.riderId = data.riderId;
      h.mounted = !!data.riderId;
      if (refused) {
        this.bridge.send({ op: 'playerDismount', id: data.riderId });
      } else if (data.riderId) {
        this.emit('horseMount', h, rider);
      }
    }
    // Mounting cancels a running order.
    if (h.mounted) this.horseOrders.mounted(h);
  }
  // The native server's report that a horse's simulator changed its health or state:
  // { id, health, state } with the state 0 alive, 1 knocked out, 2 dead (horses/health.cjs).
  horseHealthReported(report) {
    this.horseHealth.reported(report);
  }
  // The stamina the horse's simulator reported (horse.stamina).
  horseStaminaReported(report) {
    const horse = this.horses.get(report.id);
    if (!horse || typeof report.stamina !== 'number' || !Number.isFinite(report.stamina)) return;
    horse[horseStamina] = report.stamina;
  }
  // The simulator's report that a horse's order ended (nothing sends it yet).
  horseIntentDone(report) {
    this.horseOrders.intentDone(report);
  }
  // A player in the world is at `position` in `world`: the areas, the markers and the map blips
  // hear of it. Each acts only on what changed.
  playerPlaced(player, position, world) {
    this.areas.touch('player', player, position, world);
    this.mapPresence.placed(player, world);
    this.markers.touch(player, position, world);
  }
  forcePlayerPosition(player, value) {
    this.requireLivePlayerCall(player);
    const finite =
      value !== null &&
      typeof value === 'object' &&
      [value.x, value.y, value.z].every((c) => typeof c === 'number' && Number.isFinite(c));
    if (!finite) {
      throw new TypeError('Entity position requires finite x, y and z');
    }
    const position = vector(value);
    this.bridge.send({
      op: 'entityPosition',
      id: player.id,
      position: { x: position.x, y: position.y, z: position.z },
    });
    player[playerPosition] = position;
    player[playerTeleport] = { at: position, until: Date.now() + TELEPORT_WAIT_MS };
    if (player.ready) this.playerPlaced(player, position, player[playerWorld]);
  }
  // A report of the player's own game: the body stands where a script moved them, so
  // playerTeleported comes once; a move that never arrives is forgotten after TELEPORT_WAIT_MS.
  noteTeleportArrival(player) {
    const teleport = player[playerTeleport];
    if (!teleport) return;
    const reported = player[playerPosition];
    if (reported.distance(teleport.at) <= TELEPORT_ARRIVED_M) {
      player[playerTeleport] = undefined;
      this.emit('playerTeleported', player, reported);
      return;
    }
    if (Date.now() > teleport.until) player[playerTeleport] = undefined;
  }
  forcePlayerRotation(player, value) {
    this.requireLivePlayerCall(player);
    const isNumber = (c) => typeof c === 'number' && Number.isFinite(c);
    const isObject = value !== null && typeof value === 'object';
    const isQuaternion = isObject && 'w' in value;
    // Euler angles may leave out an axis: { z: 90 } only turns the player.
    const given =
      isObject && !isQuaternion ? { x: value.x ?? 0, y: value.y ?? 0, z: value.z ?? 0 } : value;
    const components = isQuaternion
      ? [value.x, value.y, value.z, value.w]
      : isObject
      ? [given.x, given.y, given.z]
      : [];
    if (!components.length || !components.every(isNumber)) {
      throw new TypeError('Entity rotation requires a quaternion or finite Euler angles');
    }
    const q = rotation(given);
    const length = Math.hypot(q.x, q.y, q.z, q.w);
    if (!Number.isFinite(length) || length < 1e-6) {
      throw new TypeError('Entity rotation quaternion must not be zero');
    }
    const unit = new Quaternion(q.w / length, q.x / length, q.y / length, q.z / length);
    this.bridge.send({
      op: 'entityRotation',
      id: player.id,
      rotation: { x: unit.x, y: unit.y, z: unit.z, w: unit.w },
    });
    player[playerRotation] = unit;
  }
  setPlayerDimension(player, world) {
    this.requireLivePlayerCall(player);
    if (!validWorld(world)) {
      throw new TypeError('Invalid dimension');
    }
    this.nextWorldRequest += 1;
    const request = this.nextWorldRequest;
    this.bridge.send({ op: 'entityWorld', id: player.id, dimension: world, request });
    player[playerWorld] = world;
    player[pendingWorldRequest] = request;
    if (player.ready) this.playerPlaced(player, player[playerPosition], world);
  }
  // Native answer to entityWorld: the authoritative dimension after the request was applied.
  confirmPlayerWorld(result) {
    const player = this.players.get(result.id);
    if (!player || player[pendingWorldRequest] !== result.request) {
      return;
    }
    player[pendingWorldRequest] = 0;
    if (validWorld(result.dimension)) {
      player[playerWorld] = result.dimension;
      if (player.ready) {
        this.playerPlaced(player, player[playerPosition], player[playerWorld]);
      }
      // The plants picked in the world they are in now.
      this.gathering.worldChanged(player);
      // And its stew pots.
      this.cookPots.worldChanged(player);
    }
  }
  setPlayerVisibleTo(player, target) {
    this.requireLivePlayerCall(player);
    if (target !== null && !(target instanceof this.api.Player)) {
      throw new TypeError('showOnlyTo requires a Player or null');
    }
    if (target !== null && this.players.get(target.id) !== target) {
      throw new Error('Unknown player');
    }
    this.bridge.send({
      op: 'entityVisibleTo',
      id: player.id,
      playerId: target === null ? 0 : target.id,
    });
  }
  // playerConnecting: a join request before the connection exists. It is let in once every handler
  // returned and every returned Promise settled; a handler that fails (a throw or a rejection) turns it away,
  // and connection.reject(reason) turns it away at once. Resolves to { allowed, reason }.
  async admitConnection(request, identity = {}) {
    // Authenticate before constructing the identity observed by whitelists and resources.
    const licensed = await this.licence.check(identity.licence, identity);
    if (!licensed.allowed) {
      this.bridge.log(
        'host',
        `playerConnecting refused request ${request}: licence ${licensed.why}`,
      );
      return { allowed: false, reason: licensed.reason };
    }
    // The server's password, before the whitelist and every handler.
    if (!this.serverPassword.accepts(identity.licenceBinding, identity.passwordProof)) {
      const brought = typeof identity.passwordProof === 'string' && identity.passwordProof !== '';
      this.bridge.log(
        'host',
        `playerConnecting refused request ${request}: ${brought ? 'wrong' : 'no'} password`,
      );
      return {
        allowed: false,
        reason: brought ? 'Wrong password.' : 'This server asks for a password.',
        password: true,
      };
    }
    // The Discord account the player proved theirs comes with the pass; without
    // one, the id the client reported, unverified.
    const passDiscord = typeof licensed.pass?.discord === 'string' ? licensed.pass.discord : '';
    const discord = passDiscord
      ? { discordId: passDiscord, discordVerified: true }
      : { discordId: identity.discordId, discordVerified: false };
    // This server's own bans, by every identifier the player has.
    const banned = this.serverBans.match(
      identifiersOf({
        ...identity,
        ...discord,
        store: licensed.pass?.store ?? '',
        storeAccount: licensed.pass?.account ?? '',
      }),
    );
    if (banned) {
      this.bridge.log('host', `playerConnecting refused request ${request}: banned (${banned.id})`);
      return { allowed: false, reason: this.serverBans.refusal(banned), banned: true };
    }
    // A server that asks for a verified Discord account: the player's game offers to verify it.
    if (this.requireDiscord && !discord.discordVerified) {
      this.bridge.log('host', `playerConnecting refused request ${request}: no verified Discord`);
      return { allowed: false, reason: DISCORD_REFUSAL, discord: true };
    }
    const verifiedIdentity = {
      ...identity,
      ...discord,
      steamId: licensed.pass?.store === 'steam' ? licensed.pass.account : '',
      epicId: licensed.pass?.store === 'epic' ? licensed.pass.account : '',
      store: licensed.pass?.store ?? '',
      storeAccount: licensed.pass?.account ?? '',
    };
    let decided = false;
    let rejectedWith = null;
    let rejectNow;
    const rejected = new Promise((resolve) => {
      rejectNow = resolve;
    });
    const connection = Object.freeze(
      new JoinRequest(
        (reason) => {
          if (decided || rejectedWith !== null) {
            return;
          }
          rejectedWith = reason;
          rejectNow();
        },
        verifiedIdentity,
        (message) => {
          if (!decided && rejectedWith === null) {
            this.bridge.send({ op: 'playerConnectingUpdate', request, message });
          }
        },
        () => !decided && rejectedWith === null,
      ),
    );
    // The whitelist turns away who it does not list before any handler runs.
    if (!this.joinQueue.admits(connection)) {
      decided = true;
      this.bridge.log('host', `playerConnecting refused request ${request}: not on the whitelist`);
      return {
        allowed: false,
        reason: refusalText(this.joinQueue.whitelist.reason, REJECT_TEXT_BYTES),
      };
    }
    const handlers = this.emitAsync('playerConnecting', connection);
    // Handlers still running after reject() may fail later; that changes nothing.
    handlers.catch(() => {});
    try {
      await Promise.race([handlers, rejected]);
    } catch (error) {
      if (rejectedWith === null) {
        decided = true;
        this.bridge.log('host', `playerConnecting refused request ${request}: ${error}`);
        return { allowed: false, reason: '' };
      }
    }
    decided = true;
    if (rejectedWith !== null) {
      this.bridge.log('host', `playerConnecting rejected request ${request}: ${rejectedWith}`);
      return { allowed: false, reason: rejectedWith };
    }
    if (this.admittedIdentities.size >= MAX_ADMITTED_IDENTITIES) {
      this.admittedIdentities.delete(this.admittedIdentities.keys().next().value);
    }
    this.admittedIdentities.set(request, playerIdentityOf(connection));
    return { allowed: true, reason: '' };
  }
  // The player an admitted request became (native op playerAdmitted).
  playerAdmitted(request, id) {
    const identity = this.admittedIdentities.get(request);
    this.admittedIdentities.delete(request);
    if (identity) {
      this.playerIdentities.set(id, identity);
    }
  }
  identityOf(id) {
    return this.playerIdentities.get(id) ?? NO_IDENTITY;
  }
  // Whether joining asks for a password (the status API's info.json).
  passwordRequired() {
    return this.serverPassword.required();
  }
  // A line in one player's chat from KCDS-Admin.
  sendAdminMessage(player, text) {
    if (!this.players.has(player.id)) {
      return false;
    }
    this.bridge.send({ op: 'chat', target: player.id, text, author: 'Admin', color: 0xd9a441ff });
    return true;
  }
  // The players' round-trip times (the native playerPings report): [[id, ms], ...].
  setPings(pings) {
    this.pings.clear();
    for (const entry of Array.isArray(pings) ? pings : []) {
      const [id, ms] = Array.isArray(entry) ? entry : [];
      if (Number.isInteger(id) && Number.isFinite(ms) && ms >= 0) {
        this.pings.set(id, Math.round(ms));
      }
    }
  }
  disconnect(id) {
    const p = this.players.get(id);
    this.playerIdentities.delete(id);
    this.pings.delete(id);
    this.wornOnly.delete(id);
    // The props hung on the player go with them, and the effects.
    this.props.leaving(id);
    this.vfx.leaving(id);
    if (!p) return;
    // The body's effects go with it, before the handlers run.
    p[playerBuffs] = null;
    p[serverBuffRemovals] = null;
    this.customBuffs.playerLeft(p);
    this.lastBlows?.delete(p);
    for (const key of [...this.buffBlocks.keys()]) {
      if (key.startsWith(`${id}:`)) this.buffBlocks.delete(key);
    }
    // Every inventory change of theirs is raised before playerDisconnect; the inventory goes
    // just after it.
    this.playerInventories.beforeDisconnect(p);
    // A dice match of theirs ends with the other player winning.
    this.voiceChat.left(id);
    this.dice.endSpectate(id);
    this.dice.leave(id, 'left');
    this.gathering.left({ id });
    this.preparing.left(id);
    this.cookPots.left({ id });
    this.itemOverrides.left(p);
    this.nameplates.left(p);
    this.stashes.left(p);
    this.pickpocketing.left(p);
    this.doors.left(p);
    this.worldTraces.left(p);
    this.cameraShots.left(p);
    // Their conversation ends interrupted while the handle still resolves.
    this.dialogues.left(p);
    this.vendors.left(p);
    this.carts.left(p);
    this.propPreviews.left(p);
    this.carries.left(p);
    this.bodyCarries.left(p);
    this.playerAnimations.left(p);
    this.pillories.left(p);
    this.siege.left(p);
    this.siegeLadders.left(p);
    this.stonePiles.left(p);
    // Their crafting ends while their inventory still takes a refund.
    this.customCrafting.left(p);
    this.crafting.left(p);
    // The rows that are not kept go before the scripts save the inventory.
    this.playerInventories.dropTransient(p);
    // A seated player who leaves forfeits a game in play.
    this.tabletops.playerLeft(p);
    this.emit('playerDisconnect', p);
    this.clientResourcesRunning.delete(id);
    this.heldClientEvents.delete(id);
    // The others' Discord presence counts one player fewer.
    this.broadcastPlayerCount(id);
    // Their dogs go with them, or stay masterless.
    this.dogs.ownerLeft(p);
    this.playerInventories.close(p);
    // Their horses stay; those not stolen go after the owner's absence.
    this.horseOwners.ownerLeft(id);
    this.players.delete(id);
    this.playerAttachments.left(p);
    this.areas.forget(id);
    // The NPCs pinned to them go back to the election.
    this.npcs.playerLeft(id);
    this.markers.forget(id);
    this.quests.left(id);
    this.mapPresence.left(id);
    this.stateStore.destroy(id);
    // A handle kept past the disconnect has no name and no stats; its body is gone.
    p.nickname = '';
    p[pendingSpawn] = null;
    p[playerStats] = NO_STATS;
    p[playerProgression] = null;
    this.progressionRules.forget(id);
  }
  // A position somewhere in a world: finite, inside the 16 km square a CryEngine level lies in
  // and between 1 km below and 10 km above sea level; null for anything else.
  worldPosition(value) {
    if (value === null || typeof value !== 'object') return null;
    const { x, y, z } = value;
    if (![x, y, z].every((c) => typeof c === 'number' && Number.isFinite(c))) return null;
    if (x < 0 || x > WORLD_EXTENT || y < 0 || y > WORLD_EXTENT || z < -1000 || z > 10000) {
      return null;
    }
    return vector(value);
  }
  // A player handle that still has a connection to ask; a stopped resource's call throws.
  connectedPlayer(player) {
    const owner = this.context.getStore();
    if (owner && (this.resources.get(owner.name) !== owner || !owner.active || owner.failing)) {
      throw new Error('Resource is stopped');
    }
    return player && this.players.get(player.id) === player;
  }
  placePlayer(player, position, rotation) {
    this.forcePlayerPosition(player, position);
    if (rotation !== undefined) this.forcePlayerRotation(player, rotation);
  }
  spawnPlayer(player, position, rotation) {
    if (rotation !== undefined && (rotation === null || typeof rotation !== 'object')) {
      throw new TypeError('Player.spawn rotation must be a Quaternion or Euler angles');
    }
    const at = this.worldPosition(position);
    if (!this.connectedPlayer(player) || !at) return false;
    if (!player.ready) {
      // Still loading (playerSpawning): the body is not in the world to move yet.
      player[pendingSpawn] = { position: at, rotation };
      return true;
    }
    this.placePlayer(player, at, rotation);
    return true;
  }
  teleportPlayer(player, position, label) {
    if (label !== undefined && typeof label !== 'string') {
      throw new TypeError('Player.teleport label must be a string');
    }
    const at = this.worldPosition(position);
    if (!this.connectedPlayer(player) || !at) return false;
    // A frozen player is held where a script puts them.
    this.frozenPlayers.teleported(player, at);
    this.forcePlayerPosition(player, at);
    return true;
  }
  // A lethal hit knocked the player out: players do not die in a server world, they stay down
  // until a resource revives them. The killer and the reason are those of the last health their
  // game took (reported just before), else null and 'unknown'.
  playerDied(id) {
    const p = this.players.get(id);
    if (!p || p[playerDown]) return false;
    p[playerDown] = true;
    const last = p[playerLastDamage];
    p[playerLastDamage] = undefined;
    const recent = last && Date.now() - last.at <= KILLING_DAMAGE_MS;
    const killer = (recent && last.attacker && this.players.get(last.attacker)) || null;
    // A knocked-out player's conversation ends interrupted first.
    this.dialogues.knockedOut(p);
    this.vendors.knockedOut(p);
    this.emit('playerDied', p, killer, recent ? last.reason : 'unknown');
    // The NPCs fighting the player stop, or turn to whoever still fights them.
    if (p[playerDown]) this.npcs.targetDown(p);
    // A playerDied handler may have made the body unlootable or revived the player already.
    if (p[playerDown] && this.players.get(id) === p) this.stashes.knockedOut(p);
    return true;
  }
  // Health a player's own game took off them: playerDamage with the attacker (a player,
  // or null for none or anyone else), the amount, the body part and the game's reason.
  playerDamage({ id, attacker, amount, reason, subpart }) {
    const p = this.players.get(id);
    const damage = damageOf({ amount, reason, subpart });
    if (!p || !damage) return false;
    const by = (attacker && this.players.get(attacker)) || null;
    p[playerLastDamage] = { attacker: by?.id ?? 0, reason: damage.reason, at: Date.now() };
    this.emit('playerDamage', p, by, damage.amount, damage.bodyPart, damage.reason);
    return true;
  }
  // A blow from another player the hit player's client holds for a ruling:
  // playerHit, then the verdict to that client. A blow from a player who is gone is taken as the
  // game works it out; one on a player who is gone gets no answer.
  playerHit({ id, request, attacker, damage, stamina, weapon, missile = false, price = null }) {
    const p = this.players.get(id);
    if (!p) return false;
    const verdict = (ruled) => {
      this.bridge.send({ op: 'hitVerdict', id, request, ...ruled });
    };
    const by = this.players.get(attacker);
    // A player in a dice match cannot be fought, nor fight, nor one who smiths, brews or
    // sharpens: the blow is refused.
    const cannotFight = (who) =>
      this.dice.byPlayer.has(who.id) || this.crafting?.working.has(who.id) === true;
    if (cannotFight(p) || (by && cannotFight(by))) {
      verdict({ ruling: RULING.refused, value: 0 });
      return true;
    }
    // The weapon: the client's report, else what the attacker holds in the right hand (a bow: the
    // left).
    const held = weapon || (missile && by?.leftHandItem) || by?.rightHandItem || null;
    const hit = playerHitOf({ damage, stamina, weapon: held, missile: missile === true, price });
    if (!by) {
      verdict({ ruling: RULING.native, value: 0 });
      return true;
    }
    if (!this.emit('playerHit', p, by, hit)) {
      verdict({ ruling: RULING.refused, value: 0 });
      return true;
    }
    verdict(hit.ruling());
    return true;
  }
  // Whether blows between players wait for the server's ruling: only while some
  // resource handles playerHit, so that without one the game works a blow out at once (blocks
  // and parries keep their timing). Sent to the server on a change; checked every tick.
  pumpHitRules() {
    const local = (this.handlers.get('playerHit') ?? []).some((h) => h.active);
    // While a dice match runs or a player works at a station blows wait for the ruling too, so one
    // on such a player is refused.
    const enabled =
      local ||
      this.services.handles('playerHit') ||
      this.dice.byPlayer.size > 0 ||
      (this.crafting?.working.size ?? 0) > 0;
    if (enabled !== this.hitRulesSent) {
      this.hitRulesSent = enabled;
      this.bridge.send({ op: 'hitRules', enabled });
    }
  }
  // Whether some resource answers a dice table's use: the players' games offer the
  // game's "Play dice" only then. Checked every tick.
  pumpDiceTables() {
    const local = (this.handlers.get('diceTableUse') ?? []).some((h) => h.active);
    this.dice.pumpTables(local || this.services.handles('diceTableUse'));
  }
  stopFight(player, opponent) {
    const live = (p) => p && this.players.get(p.id) === p && p.ready;
    if (!live(player) || !live(opponent) || player === opponent) return false;
    if (player.dimension !== opponent.dimension) return false;
    this.bridge.send({ op: 'combatCancel', id: player.id, opponent: opponent.id });
    this.bridge.send({ op: 'combatCancel', id: opponent.id, opponent: player.id });
    this.forgetBlows(player, opponent);
    this.emit('playerCombatCancelled', player, opponent);
    return true;
  }
  // A player's arrow, bolt or ball came to lie in the world (shotLanded): the ground stack it
  // becomes, then playerMissileHit with where it lies and what it struck.
  missileLanded(player, landed) {
    const stack = this.groundItems.landed(player, landed);
    if (!player || this.players.get(player.id) !== player) {
      return;
    }
    const described = landed.hit && typeof landed.hit === 'object';
    const hit = described ? this.worldTraces.hitOf(landed.hit) : null;
    const position = landed.position;
    this.emit('playerMissileHit', player, {
      position: { x: position.x, y: position.y, z: position.z },
      item: String(landed.item ?? ''),
      stuck: landed.stuck === true,
      stack: stack ?? null,
      entity: hit?.entity ?? null,
      surface: hit?.surface ?? '',
      kind: hit?.kind ?? null,
      model: hit?.model ?? null,
      entityGuid: hit?.entityGuid ?? null,
      entityName: hit?.entityName ?? null,
      entityClass: hit?.entityClass ?? null,
      levelObject: hit?.levelObject ?? null,
    });
  }
  // A melee swing a player aimed at another player, admitted by the server.
  playerAttack({ id, opponent }) {
    const p = this.players.get(id);
    const other = this.players.get(opponent);
    if (!p || !other || p === other) return false;
    this.noteBlow(p, other);
    this.emit('playerAttack', p, other);
    return true;
  }
  // Who fights whom (player.opponents): each pair's last swing, either way.
  noteBlow(attacker, victim) {
    this.lastBlows ??= new Map();
    const now = Date.now();
    for (const [one, other] of [
      [attacker, victim],
      [victim, attacker],
    ]) {
      const blows = this.lastBlows.get(one) ?? new Map();
      blows.set(other, now);
      this.lastBlows.set(one, blows);
    }
  }
  forgetBlows(player, opponent) {
    this.lastBlows?.get(player)?.delete(opponent);
    this.lastBlows?.get(opponent)?.delete(player);
  }
  // The connected players this one swung at or was swung at by within FIGHT_MEMORY_MS.
  opponentsOf(player) {
    const blows = this.lastBlows?.get(player);
    if (!blows) return [];
    const now = Date.now();
    const found = [];
    for (const [other, at] of blows) {
      const live = this.players.get(other.id) === other;
      if (!live || now - at > FIGHT_MEMORY_MS) {
        blows.delete(other);
      } else {
        found.push(other);
      }
    }
    return found;
  }
  // False for a player that is gone, not down, or already asked to get up.
  revivePlayer(player, options = {}) {
    if (options === null || typeof options !== 'object' || Array.isArray(options)) {
      throw new TypeError('revive takes an options object');
    }
    const { keepEffects = false } = options;
    if (typeof keepEffects !== 'boolean') {
      throw new TypeError('revive keepEffects takes true or false');
    }
    if (this.players.get(player.id) !== player) {
      return false;
    }
    this.requireLivePlayerCall(player);
    if (!player[playerDown]) return false;
    player[playerDown] = false;
    this.stashes.revived(player);
    this.bodyCarries.revived(player);
    this.bridge.send({ op: 'revive', id: player.id });
    if (!keepEffects) {
      // The revive gives full health; the cures take the knock-out's injuries, poisons and
      // bleeding with it.
      this.healPlayer(player, { health: false });
    }
    return true;
  }
  // Player.disconnect: as a kick, with the text as it is, or the game closed.
  // `banned` (Player.ban, Bans): the client says the player is banned.
  disconnectPlayer(player, text, options, banned = false) {
    if (typeof text !== 'string') {
      throw new TypeError('Player.disconnect takes a text');
    }
    if (Buffer.byteLength(text, 'utf8') > KICK_REASON_MAX_BYTES) {
      throw new RangeError(`A disconnect text is at most ${KICK_REASON_MAX_BYTES} bytes`);
    }
    if (options !== undefined && (options === null || typeof options !== 'object')) {
      throw new TypeError('Player.disconnect takes { quit } as options');
    }
    const quit = options?.quit ?? false;
    if (typeof quit !== 'boolean') {
      throw new TypeError('Player.disconnect quit must be true or false');
    }
    if (this.players.get(player.id) !== player) {
      return false;
    }
    this.requireLivePlayerCall(player);
    const kind = banned ? 'ban' : quit ? 'quit' : 'leave';
    this.bridge.send({ op: 'kick', id: player.id, reason: text, kind });
    return true;
  }
  // Player.kick. The reason is at most 200 bytes of UTF-8.
  kickPlayer(player, reason) {
    if (typeof reason !== 'string') {
      throw new TypeError('Player.kick takes a reason text');
    }
    if (Buffer.byteLength(reason, 'utf8') > KICK_REASON_MAX_BYTES) {
      throw new RangeError(`A kick reason is at most ${KICK_REASON_MAX_BYTES} bytes`);
    }
    if (this.players.get(player.id) !== player) {
      return false;
    }
    this.requireLivePlayerCall(player);
    this.bridge.send({ op: 'kick', id: player.id, reason });
    return true;
  }
  // False for a name outside the catalog, one of the other gender, a beard unavailable on that face
  // or a player that is gone. The face and beard are always sent together.
  setPlayerAppearance(player, request) {
    const named = resolveRequest(player[playerAppearance] ?? defaultAppearance(), request);
    if (this.players.get(player.id) !== player) {
      return false;
    }
    this.requireLivePlayerCall(player);
    if (named === null) {
      return false;
    }
    this.bridge.send({ op: 'appearance', id: player.id, ...named });
    return true;
  }
  // False for a player that is gone, still loading or knocked out; throws for a stat or value
  // setStat refuses.
  setPlayerStat(player, stat, value, fall = false) {
    const request = statRequest(stat, value);
    if (fall) request.fall = true;
    if (this.players.get(player.id) !== player) {
      return false;
    }
    this.requireLivePlayerCall(player);
    if (!player.ready || player[playerDown]) {
      return false;
    }
    this.bridge.send({ op: 'setStat', id: player.id, ...request });
    return true;
  }
  // The same returns as setPlayerStat; throws for a stat or change addStat refuses.
  addPlayerStat(player, stat, delta) {
    const request = statChangeRequest(stat, delta);
    if (this.players.get(player.id) !== player) {
      return false;
    }
    this.requireLivePlayerCall(player);
    if (!player.ready || player[playerDown]) {
      return false;
    }
    this.bridge.send({ op: 'addStat', id: player.id, ...request });
    return true;
  }
  fillPlayerStat(player, stat) {
    const request = fillRequest(stat);
    return this.setPlayerStat(player, stat, request.value);
  }
  healPlayer(player, options) {
    if (options === null || typeof options !== 'object' || Array.isArray(options)) {
      throw new TypeError('heal takes an options object');
    }
    const { health = true, injuries = true, poisons = true, bleeding = true } = options;
    const amount = health === true ? MAX_STAT_VALUE : health;
    if (
      health !== false &&
      (typeof amount !== 'number' || !Number.isFinite(amount) || amount < 0)
    ) {
      throw new TypeError('heal health takes true, false or a finite, non-negative number');
    }
    if (this.players.get(player.id) !== player) {
      return false;
    }
    this.requireLivePlayerCall(player);
    if (!player.ready) {
      return false;
    }
    // Each cure clears all effects of its kind on landing, and counts as a removal by the server.
    const carried = player[playerBuffs] ?? [];
    const cure = (name, kind) => {
      const taken = carried.filter((entry) => entry.buff.class === kind);
      this.sendBuffOrder(player, { kind: 'add', buff: lookupBuff(name).id }, taken);
    };
    if (injuries) cure('remove_injuries', 'injury');
    if (poisons) cure('remove_all_posions', 'poison');
    if (bleeding) player.removeBuff('bleeding');
    // The game clamps health to its maximum; a number only ever raises it.
    if (health !== false && amount > player.health && !player[playerDown]) {
      this.bridge.send({ op: 'setStat', id: player.id, ...statRequest('health', amount) });
    }
    return true;
  }
  setPlayerMovementMode(player, mode) {
    if (mode === null || typeof mode !== 'object' || Array.isArray(mode)) {
      throw new TypeError('setPace takes a MovementMode object');
    }
    const rule = {};
    for (const key of ['walkByDefault', 'walkEnforced']) {
      if (mode[key] !== undefined && typeof mode[key] !== 'boolean') {
        throw new TypeError(`MovementMode.${key} must be a boolean`);
      }
      rule[key] = mode[key] === true;
    }
    if (this.players.get(player.id) !== player) {
      return false;
    }
    this.requireLivePlayerCall(player);
    this.bridge.send({ op: 'movementMode', id: player.id, ...rule });
    return true;
  }
  // Sends one progression order to the player's own client. `perks`: the perk changes it makes, so
  // the report that shows them names the server as the source. False for a player without a world
  // to apply it in.
  sendProgressionOrder(player, order, ...perks) {
    if (this.players.get(player.id) !== player) {
      return false;
    }
    this.requireLivePlayerCall(player);
    if (!player.ready) {
      return false;
    }
    player[serverPerkOrders] ??= new Map();
    for (const perk of perks) {
      player[serverPerkOrders].set(perk.name, perk.change);
    }
    this.allowOrder(player, order);
    this.bridge.send({ op: 'progressionOrder', id: player.id, ...order });
    return true;
  }
  setPlayerVisible(player, visible) {
    if (typeof visible !== 'boolean') {
      throw new TypeError('setVisible takes a boolean');
    }
    if (this.players.get(player.id) !== player) {
      return false;
    }
    this.requireLivePlayerCall(player);
    this.bridge.send({ op: 'visible', id: player.id, visible });
    return true;
  }
  // A woman in a men's top wears hose under it (appearance/legs_cover.cjs).
  updateLegsCover(player) {
    const entry = this.playerInventories.entryOf(player);
    if (!entry || !player.ready) {
      return;
    }
    const appearance = player[playerAppearance];
    const wanted = needsCover(appearance && appearance.gender, entry.inventory.snapshot().items);
    if (wanted !== (player[playerLegsCover] === true)) {
      player[playerLegsCover] = wanted;
      this.setPlayerWorn(player, COVER_ITEM, wanted);
    }
  }
  // Player.wearItem / removeWorn: the player's list of worn-only pieces, sent whole to
  // their own client.
  setPlayerWorn(player, item, on) {
    const itemClass = typeof item === 'string' ? lookupItemClass(item) : undefined;
    if (!itemClass) {
      throw new TypeError('wearItem takes an item class name or id');
    }
    if (!isClothing(itemClass)) {
      throw new RangeError(`${itemClass.name} is not something to wear`);
    }
    if (this.players.get(player.id) !== player) {
      return false;
    }
    this.requireLivePlayerCall(player);
    const worn = this.wornOnly.get(player.id) ?? new Map();
    const hex = classBytesHex(itemClass);
    if (on === worn.has(hex)) {
      return false;
    }
    if (on && worn.size >= WORN_MAX) {
      throw new RangeError(`a player wears at most ${WORN_MAX} pieces this way`);
    }
    if (on) {
      worn.set(hex, itemClass);
    } else {
      worn.delete(hex);
    }
    this.wornOnly.set(player.id, worn);
    this.bridge.send({ op: 'worn', id: player.id, items: [...worn.keys()] });
    return true;
  }
  setPlayerDisguise(player, model) {
    if (model !== null && typeof model !== 'string') {
      throw new TypeError('setDisguise takes a mesh path or stem, or null');
    }
    const mesh = model === null ? '' : resolveMesh(model);
    if (mesh === null) {
      throw new RangeError(`the prop catalog does not carry ${model}`);
    }
    if (this.players.get(player.id) !== player) {
      return false;
    }
    this.requireLivePlayerCall(player);
    this.bridge.send({ op: 'disguise', id: player.id, model: mesh });
    return true;
  }
  setKnockoutVision(player, percent) {
    this.requireLivePlayerCall(player);
    if (!Number.isInteger(percent) || percent < 0 || percent > 100) {
      throw new RangeError('knockout vision must be an integer percent from 0 to 100');
    }
    this.bridge.send({ op: 'knockoutVision', id: player.id, percent });
  }
  receiveChat(id, text) {
    const p = this.players.get(id);
    if (
      !p?.ready ||
      typeof text !== 'string' ||
      Buffer.byteLength(text) > 1024 ||
      /[\x00-\x08\x0b-\x1f\x7f]/.test(text)
    )
      return false;
    if (text.startsWith('/')) {
      const parts = text.slice(1).trim().split(/\s+/);
      const command = parts.shift();
      if (command) this.emit('playerCommand', p, command, parts);
    } else {
      if (this.defaultRelay) this.api.Chat.sendToAll(text, { author: p.nickname });
      this.emit('playerChat', p, text);
    }
    return true;
  }
  refresh() {
    const manifests = new Map();
    for (const entry of fs
      .readdirSync(this.root, { withFileTypes: true })
      .sort((a, b) => a.name.localeCompare(b.name))) {
      if (!entry.isDirectory()) continue;
      const folder = path.join(this.root, entry.name),
        file = path.join(folder, 'package.json');
      if (!fs.existsSync(file)) continue;
      const manifest = normalizeManifest(JSON.parse(fs.readFileSync(file, 'utf8')));
      if (typeof manifest.name !== 'string' || !manifest.name.length)
        throw new Error('Resource name missing');
      if (manifests.has(manifest.name)) {
        this.bridge.log(manifest.name, 'Duplicate resource ignored');
        continue;
      }
      manifests.set(manifest.name, { manifest, folder });
    }
    this.manifests = manifests;
    return manifests;
  }
  order() {
    const result = [],
      visiting = new Set(),
      visited = new Set();
    // `dependent` names the resource that asked for `name`, so a missing required dependency
    // reports both sides: "Resource 'shop' depends on missing resource 'economy'".
    const visit = (name, dependent) => {
      if (visited.has(name)) return;
      if (visiting.has(name)) throw new Error(`Resource dependency cycle ${name}`);
      const m = this.manifests.get(name);
      if (!m) {
        if (dependent !== undefined) {
          throw new Error(`Resource '${dependent}' depends on missing resource '${name}'`);
        }
        throw new Error(`Missing resource ${name}`);
      }
      visiting.add(name);
      for (const dep of m.manifest.kcds?.requires ?? []) {
        const dependency = typeof dep === 'string' ? dep : dep.name;
        if (!this.manifests.has(dependency) && dep.optional) {
          this.bridge.log(name, `Optional dependency missing: ${dependency}`);
          continue;
        }
        visit(dependency, name);
      }
      visiting.delete(name);
      visited.add(name);
      result.push(name);
    };
    for (const name of this.manifests.keys()) visit(name);
    return result;
  }
  async startAll() {
    await this.adminServer.start();
    this.serverSchedule.start();
    // The server's animal population ticks from here until the host shuts down.
    this.npcs.population.start();
    // And the level's own people, the same for every player.
    this.people.start();
    // And so does the world clock's timer.
    this.worldState.start();
    // The level's beehives hold the same honey for everyone.
    this.beehives = new Beehives({
      Stash: this.api.Stash,
      players: () => [...this.players.values()].filter((player) => player.ready),
      level: () => this.stashes.level(),
    });
    this.beehives.start();
    // The owner's settings file goes first, so a resource sees them and may still change them.
    this.npcs.population.loadFile(this.root);
    this.people.loadFile(this.root);
    this.refresh();
    const order = this.order();
    for (const name of order) {
      try {
        await this.start(name);
      } catch (error) {
        this.failedResources.add(name);
        this.bridge.log(name, String(error));
      }
    }
    // The world exports the owner listed load once the resources are running.
    this.worldResources.load(this.bridge.worldResources ?? []);
  }
  async start(name, visiting = new Set()) {
    const existing = this.resources.get(name);
    if (existing) return existing.starting;
    if (visiting.has(name)) throw new Error(`Resource dependency cycle ${name}`);
    const definition = this.manifests.get(name);
    if (!definition) throw new Error(`Unknown resource ${name}`);
    const nextVisit = new Set(visiting).add(name);
    for (const dependency of definition.manifest.kcds.requires) {
      if (!this.manifests.has(dependency.name) || this.failedResources.has(dependency.name)) {
        if (dependency.optional) {
          this.bridge.log(name, `Optional dependency unavailable: ${dependency.name}`);
          continue;
        }
        throw new Error(`Required dependency unavailable: ${dependency.name}`);
      }
      try {
        await this.start(dependency.name, nextVisit);
      } catch (error) {
        if (!dependency.optional) throw error;
        this.bridge.log(name, `Optional dependency failed: ${dependency.name}`);
      }
    }
    const running = this.resources.get(name);
    if (running) return running.starting;
    const state = {
      name,
      folder: definition.folder,
      cache: new Map(),
      timers: new Set(),
      active: true,
      generation: ++this.resourceGeneration,
      starting: null,
    };
    this.resources.set(name, state);
    const operation = this.loadResource(state, definition.manifest);
    state.starting = operation;
    try {
      await operation;
      this.failedResources.delete(name);
    } finally {
      state.starting = null;
    }
  }
  // A horse's stamina or speed changed: every game hears it.
  horseStatsChanged(horse) {
    const stats = horse[horseStats];
    this.bridge.send({
      op: 'horseStats',
      id: horse.id,
      maxStamina: stats.stamina ?? -1,
      speed: stats.speed ?? -1,
    });
  }
  // The resource's rows for the game's smithy and alchemy table: its
  // replace/Libs/Tables/minigame/BlacksmithRecipes__<name>.xml and AlchemyRecipe__<name>.xml files,
  // which every client loads into the game's own recipe tables, join the server's recipe catalog
  // while it runs.
  // A registered recipe's table patch to every client (or the one of player `to`), then each of
  // its pictures in the recipe book as an op of its own: the large one is about 1 MiB.
  sendRecipeTable({ pictures = [], ...table }, to) {
    const target = to === undefined ? {} : { to };
    this.bridge.send({ op: 'recipeTable', ...table, ...target });
    for (const picture of pictures) {
      this.bridge.send({ op: 'recipePicture', ...picture, ...target });
    }
  }

  addRecipeTablePatches(name, folder) {
    const tables = path.join(folder, 'replace', 'Libs', 'Tables', 'minigame');
    if (!fs.existsSync(tables)) {
      return;
    }
    for (const file of fs.readdirSync(tables)) {
      if (!/^(BlacksmithRecipes|AlchemyRecipe)__.+\.xml$/i.test(file)) {
        continue;
      }
      const ids = recipes.addTablePatch(name, fs.readFileSync(path.join(tables, file), 'utf8'));
      this.bridge.log(name, `recipes ${ids.join(', ')} added from ${file}`);
    }
  }
  // The resource's perks: its replace/Libs/Tables/rpg/perk__<name>.xml rows join the
  // progression catalog, so the server's perk members know them; players' games load the same
  // file with the game's tables at their next start (Client/runtime/startup_tables).
  addPerkTablePatches(name, folder) {
    const tables = path.join(folder, 'replace', 'Libs', 'Tables', 'rpg');
    if (!fs.existsSync(tables)) {
      return;
    }
    for (const file of fs.readdirSync(tables)) {
      if (!/^perk__.+\.xml$/i.test(file)) {
        continue;
      }
      const added = addPerkTablePatch(fs.readFileSync(path.join(tables, file), 'utf8'));
      this.bridge.log(name, `perks ${added.join(', ') || '(none new)'} added from ${file}`);
    }
  }
  async loadResource(state, manifest) {
    const { name, folder } = state;
    this.addRecipeTablePatches(name, folder);
    this.addPerkTablePatches(name, folder);
    const services = this.services.add(name, manifest);
    const scoped = (fn, ...args) => this.context.run(state, () => fn(...args));
    const logger = Object.fromEntries(
      ['log', 'info', 'warn', 'error', 'debug'].map((level) => [
        level,
        (...args) => this.bridge.log(name, args.map(String).join(' ')),
      ]),
    );
    const timers = resourceTimers(
      state,
      (fn) => scoped(fn),
      (error) => this.failResource(name, error),
    );
    // fetch and the web platform's classes, and the resource's HTTP routes.
    await this.httpRoutes.start();
    state.web = resourceWeb(name);
    state.http = this.httpRoutes.forResource(name, (fn) => scoped(fn));
    state.vm = vm.createContext(
      withNamespace({
        ...state.web.globals,
        Http: state.http.api,
        ...this.api,
        ...services,
        ...timers,
        console: logger,
        Runtime: this.services.environment,
        Buffer,
        process: resourceProcess(folder, logger, timers.queueMicrotask),
      }),
    );
    // Node code bundled into a resource (npm packages such as pg) reaches the global object as
    // `global`.
    vm.runInContext('globalThis.global = globalThis;', state.vm);
    state.unlinkRealm = this.realmLinks.link(state.vm);
    const nativeRequire = createRequire(path.join(folder, 'package.json'));
    const resolveFile = (file) => {
      const requested = path.resolve(folder, file);
      const candidates = [
        requested,
        requested + '.js',
        requested + '.cjs',
        requested + '.json',
        path.join(requested, 'index.js'),
      ];
      for (const candidate of candidates) {
        if (!fs.existsSync(candidate) || !fs.statSync(candidate).isFile()) continue;
        const resolved = fs.realpathSync(candidate);
        const relative = path.relative(folder, resolved);
        if (relative === '..' || relative.startsWith('..' + path.sep) || path.isAbsolute(relative))
          throw new Error('Resource path escapes folder');
        return resolved;
      }
      throw new Error(`Resource module not found: ${file}`);
    };
    const load = (file) => {
      const resolved = resolveFile(file);
      if (state.cache.has(resolved)) return state.cache.get(resolved).exports;
      const module = { exports: {} };
      state.cache.set(resolved, module);
      try {
        const code = fs.readFileSync(resolved, 'utf8');
        if (resolved.endsWith('.json')) module.exports = JSON.parse(code);
        else {
          const wrapper = new vm.Script(
            `(function(exports,require,module,__filename,__dirname){\n${code}\n})`,
            { filename: resolved },
          ).runInContext(state.vm, { timeout: 5000 });
          const localRequire = (request) => {
            if (typeof request !== 'string') throw new TypeError('Module name must be a string');
            if (request.startsWith('.'))
              return load(path.relative(folder, path.resolve(path.dirname(resolved), request)));
            // Server resources are trusted host code, and retain normal Node
            // built-ins/dependencies. No such require reaches the client worker.
            return nativeRequire(request);
          };
          scoped(() =>
            wrapper.call(
              module.exports,
              module.exports,
              localRequire,
              module,
              resolved,
              path.dirname(resolved),
            ),
          );
        }
        return module.exports;
      } catch (error) {
        state.cache.delete(resolved);
        throw error;
      }
    };
    // Compiled resources reach require through the global object as well (globalThis.require),
    // relative paths from the resource's folder; a module's own require stays as above.
    state.vm.require = (request) => {
      if (typeof request !== 'string') throw new TypeError('Module name must be a string');
      return request.startsWith('.') ? load(request) : nativeRequire(request);
    };
    try {
      if (manifest.kcds.language?.server === 'csharp') {
        // A C# resource: its assemblies in its own context on the side's .NET
        // runtime, over the same scope (csharp_host.cjs).
        const readBytes = (file) => fs.readFileSync(resolveFile(file));
        state.csharp = new CSharpResource({ name, scope: state.vm, readBytes });
        await this.context.run(state, () => state.csharp.start(scriptEntries(manifest, 'server')));
      } else if (manifest.kcds.language?.server === 'lua') {
        // A Lua resource: its own Lua state over the same scope (lua_host.cjs).
        const readFile = (file) => fs.readFileSync(resolveFile(file), 'utf8');
        state.lua = await new LuaResource({ name, scope: state.vm, readFile }).start();
        for (const file of scriptEntries(manifest, 'server')) {
          await this.context.run(state, () => state.lua.run(file));
        }
      } else {
        for (const file of scriptEntries(manifest, 'server')) load(file);
      }
      await this.withTimeout(this.emitAsync('resourceStart', name), 30000);
      this.bridge.log(name, 'started');
    } catch (e) {
      await this.stop(name);
      throw e;
    }
  }
  async stop(name, failure = false) {
    if (!failure) {
      const pending = this.restartTimers.get(name);
      if (pending) clearTimeout(pending.timer);
      this.restartTimers.delete(name);
    }
    const state = this.resources.get(name);
    if (!state) return;
    if (!failure) state.restartSuppressed = true;
    for (const [other, def] of this.manifests ?? [])
      if (
        this.resources.has(other) &&
        other !== name &&
        (def.manifest.kcds?.requires ?? []).some(
          (d) => (typeof d === 'string' ? d : d.name) === name,
        )
      )
        await this.stop(other, failure);
    try {
      await this.withTimeout(this.emitAsync('resourceStop', name), 10000);
    } catch (e) {
      this.bridge.log(name, describeError(e));
    }
    state.active = false;
    for (const list of this.handlers.values())
      for (const h of list) if (h.resource === name) h.active = false;
    for (const timer of state.timers) {
      clearTimeout(timer);
      clearInterval(timer);
      clearImmediate(timer);
    }
    state.web?.stop();
    state.http?.stop();
    // A Lua resource's state goes with it (its handlers were let go above).
    state.lua?.close();
    state.lua = null;
    state.csharp?.stop();
    state.csharp = null;
    this.removeResourceObjects(name);
    this.stateStore.removeResource(state);
    this.services.remove(name);
    state.unlinkRealm?.();
    this.resources.delete(name);
    this.bridge.log(name, 'stopped');
  }
  restartGroup(name) {
    const restart = new Set([name]);
    for (const resource of this.order()) {
      if (
        this.resources.has(resource) &&
        this.manifests.get(resource).manifest.kcds.requires.some((dep) => restart.has(dep.name))
      )
        restart.add(resource);
    }
    return restart;
  }
  async ensure(name) {
    const restart = this.restartGroup(name);
    await this.stop(name);
    this.refresh();
    this.order();
    for (const resource of restart) this.failedResources.delete(resource);
    for (const resource of this.order()) {
      if (restart.has(resource)) await this.start(resource);
    }
  }
  // Everything the resource made in the world and registered with the host goes: on a stop, and
  // on an uncaught error even when the resource continues (its code, handlers and timers stay).
  removeResourceObjects(name) {
    for (const horse of [...this.horses.values()]) {
      if (horse.resource === name) horse.destroy();
    }
    this.npcs.resourceStopped(name);
    this.npcs.patrols.routes.resourceStopped(name);
    this.propPreviews.resourceStopped(name);
    this.props.resourceStopped(name);
    this.smartObjects.resourceStopped(name);
    this.doors.resourceStopped(name);
    this.vfx.resourceStopped(name);
    this.lights.resourceStopped(name);
    this.groundItems.resourceStopped(name);
    this.carts.resourceStopped(name);
    this.stashes.resourceStopped(name);
    this.siege.resourceStopped(name);
    this.siegeLadders.resourceStopped(name);
    this.stonePiles.resourceStopped(name);
    this.areas.resourceStopped(name);
    this.markers.resourceStopped(name);
    this.tabletops.resourceStopped(name);
    this.blips.resourceStopped(name);
    this.customCrafting.resourceStopped(name);
    recipes.removeOwner(name);
    this.worldInteractions.resourceStopped(name);
    this.customBuffs.resourceStopped(name);
    this.quests.resourceStopped(name);
    this.dialogues.resourceStopped(name);
    this.vendors.resourceStopped(name);
    this.gizmos.resourceStopped(name);
  }
  // The server learned of a player whose client resources it has not heard running yet: their
  // client events wait for them (clientResourcesStarted).
  clientResourcesPending(id) {
    if (!this.clientResourcesRunning.has(id) && !this.heldClientEvents.has(id)) {
      this.heldClientEvents.set(id, []);
    }
  }
  // The player's client resources run: the events held for them go now, in order.
  clientResourcesStarted(id) {
    this.clientResourcesRunning.add(id);
    this.broadcastPlayerCount();
    for (const { name, field } of this.studioAnimations.largeOnes()) {
      this.services.emitToPlayer(id, POSE_ANIMATION_EVENT, { name, field });
    }
    const held = this.heldClientEvents.get(id) ?? [];
    this.heldClientEvents.delete(id);
    for (const event of held) {
      this.services.send({ op: 'event', ...event, player: id });
    }
    // Their game learns the tabletops of their dimension.
    const player = this.players.get(id);
    if (player) {
      this.tabletops.playerReady(player);
    }
    return held.length;
  }
  // Every client's Discord presence shows how many play of how many places:
  // the reserved client event kcds:playerCount { players, max }, never raised to scripts. Sent to
  // every client whose resources run when one starts and when a player leaves (`leaving`, still
  // in the list then).
  broadcastPlayerCount(leaving = undefined) {
    const players = [...this.players.keys()].filter((id) => id !== leaving).length;
    const count = { players, max: this.statusRoutes.settings.maxPeers ?? 0 };
    for (const id of this.clientResourcesRunning) {
      this.services.emitToPlayer(id, PLAYER_COUNT_EVENT, count);
    }
  }
  // player.emit: at once, or held while the player's client resources do not run (at most
  // MAX_HELD_CLIENT_EVENTS, the oldest dropped first).
  emitToPlayerClient(id, name, payload) {
    const held = this.heldClientEvents.get(id);
    if (!held) {
      this.services.emitToPlayer(id, name, payload);
      return;
    }
    if (held.length >= MAX_HELD_CLIENT_EVENTS) {
      held.shift();
    }
    held.push(this.services.networkEventOf(name, payload));
  }
  failResource(name, error) {
    const state = this.resources.get(name);
    if (!state || state.failing || this.shuttingDown) return;
    state.failing = true;
    this.failedResources.add(name);
    this.bridge.log(name, error?.stack ?? String(error));
    this.removeResourceObjects(name);
    const behavior = this.manifests.get(name)?.manifest.kcds.errorBehavior ?? 'stop';
    if (behavior === 'continue') {
      state.failing = false;
      return;
    }
    for (const timer of state.timers) {
      clearTimeout(timer);
      clearInterval(timer);
      clearImmediate(timer);
    }
    state.timers.clear();
    state.web?.stop();
    state.http?.stop();
    const restart = this.restartGroup(name);
    this.stop(name, true)
      .then(() => {
        this.onResourcesChanged?.();
        if (behavior !== 'restart' || this.shuttingDown || state.restartSuppressed) return;
        const timer = setTimeout(() => {
          this.restartTimers.delete(name);
          if (this.shuttingDown || this.resources.has(name)) return;
          const start = async () => {
            this.refresh();
            for (const resource of restart) this.failedResources.delete(resource);
            for (const resource of this.order()) {
              if (restart.has(resource)) await this.start(resource);
            }
            this.onResourcesChanged?.();
          };
          start().catch((failure) => this.bridge.log(name, String(failure)));
        }, 1000);
        this.restartTimers.set(name, { timer, restart });
      })
      .catch((failure) => this.bridge.log(name, String(failure)));
  }
  errorOwner(error) {
    const owner = this.context.getStore();
    if (owner) {
      return this.resources.get(owner.name) === owner && owner.active ? owner.name : undefined;
    }
    const stack = typeof error?.stack === 'string' ? error.stack : '';
    for (const line of stack.split('\n').slice(1)) {
      for (const state of this.resources.values()) {
        if (line.includes(state.folder + path.sep)) return state.name;
      }
    }
    return undefined;
  }
  async shutdown() {
    this.shuttingDown = true;
    for (const pending of this.restartTimers.values()) clearTimeout(pending.timer);
    this.restartTimers.clear();
    for (const name of [...this.resources.keys()].reverse()) await this.stop(name);
    this.worldResources.clear();
    this.horseOwners.clear();
    this.npcs.population.stop();
    this.people.stop();
    this.worldState.stop();
    await this.httpRoutes.close();
    await this.adminServer.close();
    this.serverSchedule.stop();
  }
  async refreshAll() {
    const running = new Set(this.resources.keys());
    for (const name of [...this.order()].reverse()) await this.stop(name);
    this.refresh();
    this.order();
    this.failedResources.clear();
    for (const name of this.order()) {
      if (running.has(name)) await this.start(name);
    }
  }
  async withTimeout(promise, ms) {
    let timer;
    try {
      return await Promise.race([
        promise,
        new Promise((_, reject) => {
          timer = setTimeout(() => reject(new Error('Resource lifecycle timeout')), ms);
        }),
      ]);
    } finally {
      clearTimeout(timer);
    }
  }
}
module.exports = { ResourceHost };
