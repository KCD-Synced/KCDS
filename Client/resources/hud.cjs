'use strict';

const { ScreenEffects } = require('./screen_effects.cjs');
const { hudRect } = require('./hud_rects.cjs');

// Points nearer the camera than this, or behind it, have no place on screen.
const NEAR_PLANE = 0.01;

// The character panel's limits (the game client keeps to the same).
const STAT_MOST = 64;
const STAT_LABEL_MOST = 64;
const STAT_VALUE_MOST = 9999;

function statName(name) {
  return typeof name === 'string' && name.length > 0 && name.length <= STAT_LABEL_MOST;
}

// The rows of Hud.setCharacterStats, checked and copied: null, or { hide, set }.
function characterStatsOf(rows) {
  if (rows === null) {
    return null;
  }
  if (typeof rows !== 'object' || Array.isArray(rows)) {
    throw new TypeError('Hud.setCharacterStats takes { hide, set } or null');
  }
  for (const key of Object.keys(rows)) {
    if (key !== 'hide' && key !== 'set') {
      throw new TypeError(`Hud.setCharacterStats: unknown key ${key}`);
    }
  }
  const hide = rows.hide ?? [];
  if (!Array.isArray(hide) || hide.length > STAT_MOST || !hide.every(statName)) {
    throw new TypeError(`Hud.setCharacterStats: hide is up to ${STAT_MOST} stat names`);
  }
  const set = rows.set ?? [];
  if (!Array.isArray(set) || set.length > STAT_MOST) {
    throw new TypeError(`Hud.setCharacterStats: set is up to ${STAT_MOST} rows`);
  }
  return {
    hide: [...hide],
    set: set.map((row) => {
      const { stat, label } = row ?? {};
      const value = row?.value ?? 0;
      if (!statName(stat)) {
        throw new TypeError("Hud.setCharacterStats: a set row's stat is a stat name");
      }
      if (!statName(label)) {
        throw new TypeError(
          `Hud.setCharacterStats: a set row's label is text of 1 to ${STAT_LABEL_MOST}`,
        );
      }
      if (!Number.isFinite(value) || Math.abs(value) > STAT_VALUE_MOST) {
        throw new RangeError(
          `Hud.setCharacterStats: a set row's value is a number up to ${STAT_VALUE_MOST}`,
        );
      }
      return { stat, label, value: Math.trunc(value) };
    }),
  };
}

// Projects a world point with the view camera the game client reported: matrix rows
// [rx fx ux px, ry fy uy py, rz fz uz pz] (right, forward and up axes, position) and the
// vertical field of view in radians, onto a client area of size pixels.
function project(camera, size, point) {
  const { m, fov } = camera;
  const dx = point.x - m[3];
  const dy = point.y - m[7];
  const dz = point.z - m[11];
  const right = dx * m[0] + dy * m[4] + dz * m[8];
  const forward = dx * m[1] + dy * m[5] + dz * m[9];
  const up = dx * m[2] + dy * m[6] + dz * m[10];
  if (forward <= NEAR_PLANE) {
    return null;
  }
  const tanHalf = Math.tan(fov / 2);
  const aspect = size.width / size.height;
  const x = (1 + right / (forward * tanHalf * aspect)) * 0.5 * size.width;
  const y = (1 - up / (forward * tanHalf)) * 0.5 * size.height;
  const onScreen = x >= 0 && x <= size.width && y >= 0 && y <= size.height;
  return { x, y, onScreen };
}

// The client-only Hud global: the game's own HUD messages.
//
// Each message call goes to the game client ({ op: 'hud', call, args }), which runs it in the
// engine's script system on the engine thread: the HUD movie's methods through the game's
// FlashUI binding, tutorials through the game's own tutorial functions. The game client
// reports whether the HUD exists ({ op: 'hudHost', ready }); without it, in the main menu and
// across a level load, every message call returns false and sends nothing.
//
// HUD elements are hidden per resource: the game client gets every element some resource hides
// ({ op: 'hudHidden', elements }) and hides them again before every frame, since the game sets
// their visibility itself. A stopping resource releases its elements.
//
// Screen effects (screen_effects.cjs) need no HUD; they go when the level changes, which is
// when the HUD goes away.

// The HUD movie's clips a resource may hide, by the game's own names.
const ELEMENTS = new Set([
  'Compass',
  'Stats',
  'QAMWeapon',
  'QAMFood',
  'Subtitles',
  'InfoText',
  'GameLog',
  'Hints',
  'DialogLeft',
  'DialogRight',
  'Cursor',
  'Crime',
  'Wanted',
  'PopUpBackground',
  'TutorialMessage',
  'FancyEvent',
  'SkillCheck',
  'ItemTransfer',
  'Buffs',
  'CommonEvent',
  'DiceCursor',
  'Trespassing',
  'RatioStrips',
  'ShootingContest',
  'Bubbles',
  'TutorialInDialog',
  'DiceContainer',
  'Vignette',
]);

function element(value) {
  if (typeof value !== 'string') {
    throw new TypeError('Hud: element must be a string');
  }
  if (!ELEMENTS.has(value)) {
    throw new RangeError(`Hud: unknown HUD element ${value}`);
  }
  return value;
}

function text(value, name) {
  if (typeof value !== 'string') {
    throw new TypeError(`Hud: ${name} must be a string`);
  }
  return value;
}

// A whole number; fallback when it is left out and the parameter is optional.
function integer(value, name, fallback) {
  if (value === undefined && fallback !== undefined) {
    return fallback;
  }
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new TypeError(`Hud: ${name} must be a finite number`);
  }
  return Math.trunc(value);
}

// The game's shooting contest table (its HUD's ShowShootingContestTable and the others): the
// player and at most two opponents. A seat left out shows nobody.
const CONTEST_OPPONENTS = 2;

function contestant(value, name, fields) {
  if (!value || typeof value !== 'object') {
    throw new TypeError(`Hud: ${name} must be an object`);
  }
  const out = {};
  for (const field of fields) {
    out[field] =
      field === 'name'
        ? text(value.name ?? '', `${name}.name`)
        : integer(value[field], `${name}.${field}`, 0);
  }
  return out;
}

// { player, opponents } of a contest table: the player and two opponent seats, each with fields.
function contestTable(table, fields) {
  if (!table || typeof table !== 'object') {
    throw new TypeError('Hud: the shooting contest table must be an object');
  }
  const opponents = table.opponents ?? [];
  if (!Array.isArray(opponents) || opponents.length > CONTEST_OPPONENTS) {
    throw new TypeError(`Hud: opponents must be an array of at most ${CONTEST_OPPONENTS}`);
  }
  const seats = [];
  for (let index = 0; index < CONTEST_OPPONENTS; index += 1) {
    const opponent = opponents[index];
    seats.push(
      opponent === undefined
        ? contestant({}, `opponents[${index}]`, fields)
        : contestant(opponent, `opponents[${index}]`, fields),
    );
  }
  return { player: contestant(table.player, 'player', fields), opponents: seats };
}

// A place on the target, -1 to 1 across and up.
function targetSpot(value, name) {
  const spot = finiteNumber(value, name);
  if (spot < -1 || spot > 1) {
    throw new RangeError(`Hud: ${name} must be from -1 to 1`);
  }
  return spot;
}

function finiteNumber(value, name) {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new TypeError(`Hud: ${name} must be a finite number`);
  }
  return value;
}

// The game's own ShowLevelXpGain types: 5 for the four stats, 7 for skills (WHGame 1.5.5).
// Types 0-4 are never used by the game and draw off the HUD's edge.
const STAT_XP_TYPE = 5;
const SKILL_XP_TYPE = 7;
const STATS = new Set(['str', 'strength', 'agi', 'agility', 'vit', 'vitality', 'spe', 'speech']);

function xpGainType(stat) {
  const name = stat.toLowerCase().replace(/^stat_/, '');
  return STATS.has(name) ? STAT_XP_TYPE : SKILL_XP_TYPE;
}

class ResourceHud {
  constructor({
    send,
    now,
    timers,
    screenSize = () => ({ width: 0, height: 0 }),
    emit = () => {},
    ask = () => null,
  }) {
    this.send = send;
    // A synchronous request to the game client (Hud.rect reads the HUD movie).
    this.ask = ask;
    this.emit = emit;
    // The local character's monologue: suppressed until a resource enables it for the session.
    this.monologue = false;
    // Story cutscenes a quest plays: suppressed until a resource enables them for the session.
    this.storyCutscenes = false;
    // The game's own XP notices: off until a resource enables them for the session.
    this.xpNotices = false;
    // The horse inspect screen compares with the player's horse (setHorseCompare).
    this.horseCompare = true;
    // The character panel's rows a resource set (setCharacterStats), null for the game's own.
    this.characterStats = null;
    this.screenSize = screenSize;
    this.camera = null;
    this.cameraWanted = false;
    this.screen = new ScreenEffects({ send, now, timers });
    this.ready = false;
    this.gameDialogue = false;
    // element -> the resources hiding it
    this.hidden = new Map();
  }

  publishHidden() {
    this.send({ op: 'hudHidden', elements: [...this.hidden.keys()].sort() });
  }

  setHidden(owner, name, hide) {
    const owners = this.hidden.get(name) ?? new Set();
    const before = this.hidden.has(name);
    if (hide) {
      owners.add(owner);
      this.hidden.set(name, owners);
    } else {
      owners.delete(owner);
      if (owners.size === 0) {
        this.hidden.delete(name);
      }
    }
    if (before !== this.hidden.has(name)) {
      this.publishHidden();
    }
  }

  // The resource stopped: the elements it hid are released.
  removeOwner(owner) {
    this.screen.removeAll(owner);
    let changed = false;
    for (const [name, owners] of this.hidden) {
      if (owners.delete(owner) && owners.size === 0) {
        this.hidden.delete(name);
        changed = true;
      }
    }
    if (changed) {
      this.publishHidden();
    }
  }

  // The view camera this frame: { m: [12 numbers], fov }, or none outside a world.
  receiveCamera({ m, fov }) {
    const valid = Array.isArray(m) && m.length === 12 && m.every(Number.isFinite);
    this.camera = valid && Number.isFinite(fov) && fov > 0 ? { m, fov } : null;
  }

  worldToScreen(point) {
    if (point === null || typeof point !== 'object') {
      throw new TypeError('Hud: point must be a Vector3');
    }
    for (const key of ['x', 'y', 'z']) {
      if (typeof point[key] !== 'number' || !Number.isFinite(point[key])) {
        throw new TypeError('Hud: point must be a Vector3');
      }
    }
    if (!this.cameraWanted) {
      // The game client streams the camera only once a resource projects a point.
      this.cameraWanted = true;
      this.send({ op: 'cameraWanted' });
    }
    const size = this.screenSize();
    if (!this.camera || !(size.width > 0) || !(size.height > 0)) {
      return null;
    }
    return project(this.camera, size, point);
  }

  receiveHost({ ready, dialogue }) {
    if (this.ready && !ready) {
      this.screen.clear();
      this.monologue = false;
      this.storyCutscenes = false;
      this.xpNotices = false;
    }
    // A new session compares with the player's horse again, as the game does.
    if (!this.ready && ready && !this.horseCompare) {
      this.setHorseCompare(true);
    }
    if (!this.ready && ready && this.characterStats !== null) {
      this.setCharacterStats(null);
    }
    this.ready = Boolean(ready);
    this.gameDialogue = Boolean(dialogue);
  }

  setMonologue(enabled) {
    this.monologue = enabled;
    this.send({ op: 'hudMonologue', enabled });
  }

  setStoryCutscenes(enabled) {
    this.storyCutscenes = enabled;
    this.send({ op: 'hudStoryCutscenes', enabled });
  }

  setXpNotices(enabled) {
    this.xpNotices = enabled;
    this.send({ op: 'hudXpNotices', enabled });
  }

  // { op: 'hudStoryCutsceneSuppressed', cutscene }: a quest's cutscene was kept from playing (its
  // quest went on as if it had played).
  receiveStoryCutsceneSuppressed({ cutscene }) {
    if (typeof cutscene !== 'string' || this.storyCutscenes) {
      return;
    }
    this.emit('storyCutsceneSuppressed', [cutscene]);
  }

  // The horse inspect screen's "My horse" column: the player's own horse
  // (a server horse given to them included) next to the one looked at, or "-" when turned off.
  setHorseCompare(enabled) {
    this.horseCompare = enabled;
    this.send({ op: 'hudHorseCompare', enabled });
  }

  // The inventory's character panel: the game's stats hidden, and a resource's label and value in
  // a stat's place, from the next time the game fills the panel.
  setCharacterStats(rows) {
    this.characterStats = rows;
    this.send({ op: 'hudCharacterStats', rows });
  }

  // { op: 'hudMonologueSuppressed', textKey, text }: a remark of the local character was kept
  // silent. The game client picks one line of the request's pool, as the game would have, and
  // localizes it; a request with no line of its own comes as its metarole with no text.
  receiveMonologueSuppressed({ textKey, text }) {
    if (typeof textKey !== 'string' || this.monologue) {
      return;
    }
    this.emit('monologueSuppressed', [typeof text === 'string' ? text : '', textKey]);
  }

  call(name, args) {
    if (!this.ready) {
      return false;
    }
    this.send({ op: 'hud', call: name, args });
    return true;
  }

  facade(owner) {
    const hud = this;
    return Object.freeze({
      // The game's own conversations; resource dialogues (Dialogue) are not exposed yet.
      inConversation() {
        return hud.gameDialogue;
      },
      showNotification(message) {
        return hud.call('showNotification', [text(message, 'message')]);
      },
      showBanner(message, durationMs, priority) {
        const duration = integer(durationMs, 'durationMs', 3000);
        const order = integer(priority, 'priority', 0);
        // The movie's ShowInfoText(Text, Priority, Duration, Background).
        return hud.call('showBanner', [text(message, 'message'), order, duration, false]);
      },
      hideBanner() {
        return hud.call('hideBanner', []);
      },
      // Where a part of the game's HUD is on the screen: { x, y, width, height } in pixels, or null.
      rect(part) {
        return hudRect(part, hud.ask);
      },
      notifyPerkUse(iconName, name) {
        return hud.call('notifyPerkUse', [text(iconName, 'iconName'), text(name, 'name')]);
      },
      notifyPerk(iconName, name) {
        return hud.call('notifyPerk', [text(iconName, 'iconName'), text(name, 'name')]);
      },
      notifyCombo(iconName, name) {
        return hud.call('notifyCombo', [text(iconName, 'iconName'), text(name, 'name')]);
      },
      notifyReputation(eventId, message) {
        const id = integer(eventId, 'eventId', undefined);
        return hud.call('notifyReputation', [id, text(message, 'message')]);
      },
      notifyEventResult(type, result, name) {
        const kind = integer(type, 'type', undefined);
        return hud.call('notifyEventResult', [kind, text(result, 'result'), text(name, 'name')]);
      },
      notifyXp(stat, statName, type) {
        const name = text(stat, 'stat');
        const bar = integer(type, 'type', xpGainType(name));
        return hud.call('notifyXp', [name, text(statName, 'statName'), bar]);
      },
      showTutorial(name) {
        return hud.call('showTutorial', [text(name, 'name')]);
      },
      hideTutorial(name) {
        return hud.call('hideTutorial', [text(name, 'name')]);
      },
      closeTutorial() {
        return hud.call('closeTutorial', []);
      },
      hideCodexHint() {
        return hud.call('hideCodexHint', []);
      },
      clearNotifications() {
        return hud.call('clearNotifications', []);
      },
      // The game's shooting contest table, bottom left: names and arrows left of the player and
      // up to two opponents (the movie's ShowShootingContestTable).
      showShootingContest(table) {
        const { player, opponents } = contestTable(table, ['name', 'arrows']);
        const targetVisible = table.targetVisible ?? true;
        if (typeof targetVisible !== 'boolean') {
          throw new TypeError('Hud: targetVisible must be a boolean');
        }
        return hud.call('showShootingContest', [
          player.name,
          opponents[0].name,
          opponents[1].name,
          player.arrows,
          opponents[0].arrows,
          opponents[1].arrows,
          targetVisible,
        ]);
      },
      // Its arrows left and points (UpdateShootingContestTable).
      updateShootingContest(table) {
        const { player, opponents } = contestTable(table, ['arrows', 'points']);
        return hud.call('updateShootingContest', [
          player.arrows,
          opponents[0].arrows,
          opponents[1].arrows,
          player.points,
          opponents[0].points,
          opponents[1].points,
        ]);
      },
      hideShootingContest() {
        return hud.call('hideShootingContest', []);
      },
      // Where the arrow struck the target on the table's target picture (ShowShootingHit).
      showShootingHit(x, y) {
        return hud.call('showShootingHit', [targetSpot(x, 'x'), targetSpot(y, 'y')]);
      },
      // The game's horse race table: the player's and one opponent's score, and the track's name.
      showHorseContest(table) {
        if (!table || typeof table !== 'object') {
          throw new TypeError('Hud: the horse contest table must be an object');
        }
        const player = contestant(table.player, 'player', ['name', 'score']);
        const opponent = contestant(table.opponent ?? {}, 'opponent', ['name', 'score']);
        return hud.call('showHorseContest', [
          player.name,
          opponent.name,
          player.score,
          opponent.score,
          text(table.track ?? '', 'track'),
        ]);
      },
      updateHorseContest(playerScore) {
        return hud.call('updateHorseContest', [integer(playerScore, 'playerScore', undefined)]);
      },
      hideHorseContest() {
        return hud.call('hideHorseContest', []);
      },
      setHudPart(name, visible) {
        if (typeof visible !== 'boolean') {
          throw new TypeError('Hud: visible must be a boolean');
        }
        hud.setHidden(owner, element(name), !visible);
        return true;
      },
      hudPartShown(name) {
        return !hud.hidden.has(element(name));
      },
      setInnerVoice(enabled) {
        if (typeof enabled !== 'boolean') {
          throw new TypeError('Hud: enabled must be a boolean');
        }
        hud.setMonologue(enabled);
      },
      innerVoiceOn() {
        return hud.monologue;
      },
      // Whether the story cutscenes quests queue play (suppressed by default, again for each
      // session; storyCutsceneSuppressed reports each one kept from playing).
      setCutscenes(enabled) {
        if (typeof enabled !== 'boolean') {
          throw new TypeError('Hud: enabled must be a boolean');
        }
        hud.setStoryCutscenes(enabled);
      },
      cutscenesOn() {
        return hud.storyCutscenes;
      },
      // Whether the game's own "increased" notices with the XP bar show when a stat or skill
      // gains XP or a level (off by default, again for each session; notifyXp always shows).
      setXpNotices(enabled) {
        if (typeof enabled !== 'boolean') {
          throw new TypeError('Hud: enabled must be a boolean');
        }
        hud.setXpNotices(enabled);
      },
      xpNoticesOn() {
        return hud.xpNotices;
      },
      // Whether the horse inspect screen shows the player's own horse beside the one looked at
      // (KCDS addition; on by default, again for each session).
      setHorseCompare(enabled) {
        if (typeof enabled !== 'boolean') {
          throw new TypeError('Hud: enabled must be a boolean');
        }
        hud.setHorseCompare(enabled);
      },
      isHorseCompare() {
        return hud.horseCompare;
      },
      // The inventory's character panel (KCDS addition): { hide, set } hides the game's stats
      // named in hide and shows { stat, label, value } in a stat's place; null brings the game's
      // own panel back. Again the game's own for each session.
      setCharacterStats(rows) {
        hud.setCharacterStats(characterStatsOf(rows));
      },
      characterStats() {
        return hud.characterStats === null ? null : JSON.parse(JSON.stringify(hud.characterStats));
      },
      effectList() {
        return hud.screen.list();
      },
      startEffect(effect, params, options) {
        return hud.screen.add(owner, effect, params, options);
      },
      changeEffect(handle, params, options) {
        return hud.screen.change(owner, handle, params, options);
      },
      endEffect(handle, options) {
        return hud.screen.remove(owner, handle, options);
      },
      endAllEffects() {
        return hud.screen.removeAll(owner);
      },
      effectRunning(handle) {
        return hud.screen.isActive(handle);
      },
      worldToScreen(point) {
        return hud.worldToScreen(point);
      },
    });
  }
}

module.exports = { ResourceHud, project, HUD_ELEMENTS: ELEMENTS };
