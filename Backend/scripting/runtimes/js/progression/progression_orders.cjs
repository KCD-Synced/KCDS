'use strict';

// The server's progression orders: Player.addXp, setLevel, setXp,
// removeXp, addPerk, removePerk, addPerkPoints, resetPerks and loadProgression, checked here and turned into the
// native server's progressionOrder message (kcd2s/integration/progression.hpp,
// PROGRESSION_ORDER). The player's own client applies them through the game's own paths.

const { MAIN_BANK, TRACKS, requireTrack, requirePerk, totalXp } = require('./catalog.cjs');

const TRACK_INDEX = new Map(TRACKS.map((track, index) => [track.name, index]));
// The main level's bank on the wire.
const MAIN_BANK_INDEX = 0xff;
// What an order may carry: a million XP, 65535 points.
const MAX_XP = 1000000;
const MAX_POINTS = 0xffff;

// A track XP can be given to: not fencing, which the game derives from the weapon skills.
function earnedTrack(name) {
  const track = requireTrack(name);
  if (track.induced) {
    throw new TypeError(`${name} is derived from the weapon skills and takes no XP`);
  }
  return track;
}

// The order with the caller's options: { notify: false } holds back the game's notice of the
// levels it brings (KCDS addition; the game announces them otherwise).
function withNotice(order, options) {
  if (options === undefined) {
    return order;
  }
  if (options === null || typeof options !== 'object') {
    throw new TypeError('the options are an object: { notify }');
  }
  if (options.notify !== undefined && typeof options.notify !== 'boolean') {
    throw new TypeError('notify is true or false');
  }
  return options.notify === false ? { ...order, notify: false } : order;
}

function addXpOrder(trackName, xp) {
  earnedTrack(trackName);
  if (typeof xp !== 'number' || !Number.isFinite(xp) || xp <= 0) {
    throw new TypeError('addXp takes a positive number of XP');
  }
  if (xp > MAX_XP || xp * 128 < 1) {
    throw new RangeError(`addXp takes from 1/128 to ${MAX_XP} XP`);
  }
  return { kind: 'addXp', track: TRACK_INDEX.get(trackName), xp, multipliers: true };
}

// The exact order: the track is put at `level` with `xp` towards the next one, going
// down as well as up. Unlike addXp and setLevel it is no game XP gain: the client writes the
// track's level and XP and rebuilds the rest from them, without a level-up reward, a perk point
// or a notice ("setTrack").
function setTrackOrder(trackName, level, xp) {
  return { kind: 'setTrack', track: TRACK_INDEX.get(trackName), level, xp };
}

// Null for a level past the cap (setLevel returns false). A level above the current one is the
// game's own level-up (setLevel); a lower one is the exact order, with no XP towards the next.
function setLevelOrder(trackName, level, currentLevel) {
  const track = earnedTrack(trackName);
  if (!Number.isInteger(level) || level < 0) {
    throw new TypeError('setLevel takes a whole level');
  }
  if (level > track.cap) {
    return null;
  }
  if (level < currentLevel) {
    return setTrackOrder(trackName, level, 0);
  }
  return { kind: 'setLevel', track: TRACK_INDEX.get(trackName), level };
}

// The track put at a total XP from level 0 (setXp and removeXp): the level that much
// XP reaches and the XP towards the next, as the exact order, quantized down to 1/128 XP. Null past
// what the track's cap costs.
function totalXpOrder(trackName, total) {
  const track = earnedTrack(trackName);
  const quantized = Math.floor(total * 128) / 128;
  if (quantized > totalXp(track, track.cap)) {
    return null;
  }
  let level = 0;
  while (level < track.cap && totalXp(track, level + 1) <= quantized) {
    level += 1;
  }
  return setTrackOrder(trackName, level, quantized - totalXp(track, level));
}

// setXp: the track at an absolute, cumulative XP from level 0, up or down; the exact order (no
// level-up reward, point or notice). Null past the cap.
function setXpOrder(trackName, total) {
  earnedTrack(trackName);
  if (typeof total !== 'number' || !Number.isFinite(total) || total < 0) {
    throw new TypeError('setXp takes a total XP, 0 or more');
  }
  return totalXpOrder(trackName, total);
}

// removeXp: the track's reported total XP less `xp`, at zero at the least; the exact order.
function removeXpOrder(trackName, xp, state) {
  const track = earnedTrack(trackName);
  if (typeof xp !== 'number' || !Number.isFinite(xp) || xp <= 0) {
    throw new TypeError('removeXp takes a positive number of XP');
  }
  if (xp > MAX_XP || xp * 128 < 1) {
    throw new RangeError(`removeXp takes from 1/128 to ${MAX_XP} XP`);
  }
  const current = totalXp(track, state.level) + state.xp;
  return totalXpOrder(trackName, Math.max(0, current - xp));
}

function perkOrder(kind, query) {
  const perk = requirePerk(query);
  return { order: { kind, perk: perk.id }, name: perk.name };
}

function addPointsOrder(bank, count) {
  if (bank !== MAIN_BANK) {
    requireTrack(bank);
  }
  if (!Number.isInteger(count) || count < 1 || count > MAX_POINTS) {
    throw new RangeError(`addPerkPoints takes from 1 to ${MAX_POINTS} points`);
  }
  const track = bank === MAIN_BANK ? MAIN_BANK_INDEX : TRACK_INDEX.get(bank);
  return { kind: 'addPoints', track, count };
}

function respecOrder() {
  return { kind: 'respec' };
}

function wholeNumber(value, limit) {
  return Number.isInteger(value) && value >= 0 && value <= limit;
}

// A stored ProgressionSnapshot as the restore order: every track's level and XP (a track the
// snapshot leaves out stays where it is), its perks and unspent points. Throws for a malformed one.
// `exact`: a track above the snapshot is brought down to it; the client then writes
// levels and XP directly, so a restore replays no level-up reward or notice.
function restoreOrder(snapshot, exact = false) {
  if (snapshot === null || typeof snapshot !== 'object') {
    throw new TypeError('loadProgression takes a ProgressionSnapshot');
  }
  const { tracks = {}, perks = [], perkPoints = {} } = snapshot;
  if (typeof tracks !== 'object' || !Array.isArray(perks) || typeof perkPoints !== 'object') {
    throw new TypeError('loadProgression takes a ProgressionSnapshot');
  }
  for (const name of Object.keys(tracks)) {
    requireTrack(name);
  }
  for (const name of Object.keys(perkPoints)) {
    if (name !== MAIN_BANK) requireTrack(name);
  }
  const wire = TRACKS.map((track) => {
    const state = tracks[track.name] ?? { level: 0, xp: 0 };
    const points = perkPoints[track.name] ?? 0;
    if (
      state === null ||
      typeof state !== 'object' ||
      !wholeNumber(state.level, track.cap) ||
      typeof state.xp !== 'number' ||
      !Number.isFinite(state.xp) ||
      state.xp < 0 ||
      state.xp > MAX_XP ||
      !wholeNumber(points, MAX_POINTS)
    ) {
      throw new TypeError(`Invalid snapshot track ${track.name}`);
    }
    return [state.level, state.xp, points];
  });
  const mainPoints = perkPoints[MAIN_BANK] ?? 0;
  if (!wholeNumber(mainPoints, MAX_POINTS)) {
    throw new TypeError('Invalid snapshot main points');
  }
  const ids = perks.map((name) => requirePerk(name).id);
  const order = { kind: 'restore', snapshot: { tracks: wire, mainPoints, perks: ids } };
  return exact ? { ...order, exact: true, notify: false } : order;
}

module.exports = {
  addXpOrder,
  setLevelOrder,
  setXpOrder,
  removeXpOrder,
  perkOrder,
  addPointsOrder,
  respecOrder,
  restoreOrder,
  withNotice,
};
