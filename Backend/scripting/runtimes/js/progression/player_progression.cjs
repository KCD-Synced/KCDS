'use strict';

// A player's progression as the server keeps it from the player's own
// reports, and as every client's directory carries it: Player.levels, perks, progression,
// trackOf, hasPerk and the changes the progression events report.
//
// The parsed form: { tracks: [{ level, xp, points }] in Progression.tracks() order, mainPoints,
// perks: [catalog perks] }. xp, points and mainPoints are null where they are not known (another
// player's progression on a client).

const { MAIN_BANK, TRACKS, lookupPerk, requireTrack, totalXp } = require('./catalog.cjs');
const catalog = require('./catalog.json');

const TRACK_INDEX = new Map(TRACKS.map((track, index) => [track.name, index]));
const PERK_INDEX = new Map(catalog.perks.map((perk, index) => [perk, index]));
const MAX_PERKS = 1024;

function validLevel(level, track) {
  return Number.isInteger(level) && level >= 0 && level <= track.cap;
}

function validCount(value) {
  return Number.isInteger(value) && value >= 0 && value <= 0xffff;
}

function invalid(what) {
  return new TypeError(`Invalid native player progression: ${what}`);
}

// From the native server's report ({ tracks: [[level, xp, points]], mainPoints, perks: [GUID]
// }); null for none. A perk the catalog does not carry is left out.
function progressionFromReport(raw) {
  if (raw === undefined || raw === null) {
    return null;
  }
  if (typeof raw !== 'object' || !Array.isArray(raw.tracks) || !Array.isArray(raw.perks)) {
    throw invalid('shape');
  }
  if (raw.tracks.length !== TRACKS.length || raw.perks.length > MAX_PERKS) {
    throw invalid('sizes');
  }
  const tracks = raw.tracks.map((entry, index) => {
    const [level, xp, points] = Array.isArray(entry) ? entry : [];
    if (
      !validLevel(level, TRACKS[index]) ||
      typeof xp !== 'number' ||
      !Number.isFinite(xp) ||
      xp < 0 ||
      !validCount(points)
    ) {
      throw invalid(`track ${TRACKS[index].name}`);
    }
    return { level, xp, points };
  });
  if (!validCount(raw.mainPoints)) {
    throw invalid('main points');
  }
  const perks = [];
  for (const id of raw.perks) {
    if (typeof id !== 'string') {
      throw invalid('perk');
    }
    const perk = lookupPerk(id);
    if (perk) {
      perks.push(perk);
    }
  }
  return Object.freeze({ tracks, mainPoints: raw.mainPoints, perks });
}

// The owned perks as a bit set over the catalog's perks, in hex: about 240 characters whatever
// the number of perks, where names would take kilobytes in every directory that shows the player.
const PERK_SET_DIGITS = Math.ceil(catalog.perks.length / 4);

// A resource's own perks are no bits of the game's catalog: they follow the bits as
// "+<perk GUID>" each. A viewer whose catalog does not have one gets it by its GUID alone.
const PERK_ID = /^[0-9a-fA-F-]{36}$/;
const UNKNOWN_PERKS = new Map();

function perkSet(perks) {
  const nibbles = new Array(PERK_SET_DIGITS).fill(0);
  const custom = [];
  for (const perk of perks) {
    const index = PERK_INDEX.get(perk);
    if (index === undefined) {
      custom.push(perk.id);
      continue;
    }
    nibbles[index >> 2] |= 1 << (index & 3);
  }
  return [nibbles.map((nibble) => nibble.toString(16)).join(''), ...custom].join('+');
}

// A perk the set names by GUID: the catalog's, else one made of the GUID (the same object each
// time).
function perkOfId(id) {
  const known = lookupPerk(id);
  if (known) {
    return known;
  }
  const key = id.toLowerCase();
  if (!UNKNOWN_PERKS.has(key)) {
    UNKNOWN_PERKS.set(
      key,
      Object.freeze({
        name: id,
        id,
        uiName: '',
        track: null,
        level: 0,
        parent: null,
        visibility: 'visible',
        kinds: Object.freeze([]),
        autolearnable: false,
      }),
    );
  }
  return UNKNOWN_PERKS.get(key);
}

function perksOfSet(text) {
  const [bits, ...ids] = typeof text === 'string' ? text.split('+') : [];
  if (bits === undefined || bits.length !== PERK_SET_DIGITS || !/^[0-9a-f]*$/.test(bits)) {
    throw invalid('perk set');
  }
  if (!ids.every((id) => PERK_ID.test(id))) {
    throw invalid('perk set');
  }
  const perks = [];
  for (let digit = 0; digit < bits.length; ++digit) {
    const nibble = parseInt(bits[digit], 16);
    for (let bit = 0; bit < 4; ++bit) {
      if (nibble & (1 << bit)) {
        const perk = catalog.perks[digit * 4 + bit];
        if (!perk) {
          throw invalid('perk set');
        }
        perks.push(perk);
      }
    }
  }
  for (const id of ids) {
    perks.push(perkOfId(id));
  }
  return perks;
}

// The directory form for one viewer: levels and the perk set for everyone; XP and points only in
// the player's own directory.
function directoryProgression(progression, own) {
  if (!progression) {
    return null;
  }
  const entry = {
    levels: progression.tracks.map((track) => track.level),
    perks: perkSet(progression.perks),
  };
  if (own) {
    entry.xp = progression.tracks.map((track) => track.xp);
    entry.points = progression.tracks.map((track) => track.points);
    entry.mainPoints = progression.mainPoints;
  }
  return entry;
}

// From a client directory entry's progression; null for none.
function progressionFromDirectory(raw) {
  if (raw === undefined || raw === null) {
    return null;
  }
  if (
    typeof raw !== 'object' ||
    !Array.isArray(raw.levels) ||
    raw.levels.length !== TRACKS.length ||
    !raw.levels.every((level, index) => validLevel(level, TRACKS[index]))
  ) {
    throw invalid('directory');
  }
  const perks = perksOfSet(raw.perks);
  const own = raw.xp !== undefined;
  if (
    own &&
    (!Array.isArray(raw.xp) ||
      raw.xp.length !== TRACKS.length ||
      !raw.xp.every((xp) => typeof xp === 'number' && Number.isFinite(xp) && xp >= 0) ||
      !Array.isArray(raw.points) ||
      raw.points.length !== TRACKS.length ||
      !raw.points.every(validCount) ||
      !validCount(raw.mainPoints))
  ) {
    throw invalid('directory');
  }
  return Object.freeze({
    tracks: raw.levels.map((level, index) => ({
      level,
      xp: own ? raw.xp[index] : null,
      points: own ? raw.points[index] : null,
    })),
    mainPoints: own ? raw.mainPoints : null,
    perks,
  });
}

// Player.levels: every track's level, zeros without a report.
function levelsOf(progression) {
  const levels = {};
  TRACKS.forEach((track, index) => {
    levels[track.name] = progression ? progression.tracks[index].level : 0;
  });
  return levels;
}

// Player.perks: the owned perks by name.
function perkNames(progression) {
  return progression ? progression.perks.map((perk) => perk.name) : [];
}

// Player.hasPerk: whether the last report carried the perk (by name or GUID).
function hasPerk(progression, query) {
  if (typeof query !== 'string') {
    throw new TypeError('hasPerk takes a perk name or GUID');
  }
  const perk = lookupPerk(query);
  return !!(progression && perk && progression.perks.includes(perk));
}

// Player.trackOf: level, XP towards the next level, its cost and the tree's unspent points;
// null without a report or for a track whose XP this side does not know.
function trackProgress(progression, name) {
  const track = requireTrack(name);
  const state = progression?.tracks[TRACK_INDEX.get(name)];
  if (!state || state.xp === null) {
    return null;
  }
  return {
    level: state.level,
    xp: state.xp,
    xpToNext: track.base + track.diff * (state.level + 1),
    perkPoints: state.points,
  };
}

// Player.progression: the snapshot loadProgression takes; null without a full report.
function snapshotOf(progression) {
  if (!progression || progression.mainPoints === null) {
    return null;
  }
  const tracks = {};
  const perkPoints = { [MAIN_BANK]: progression.mainPoints };
  TRACKS.forEach((track, index) => {
    const state = progression.tracks[index];
    tracks[track.name] = { level: state.level, xp: state.xp };
    perkPoints[track.name] = state.points;
  });
  return { tracks, perks: perkNames(progression), perkPoints };
}

// What changed from one report to the next: XP landed per track (after every multiplier, the
// level-ups included), each level reached, perks added and removed. Nothing for a first report.
function progressionChanges(previous, current) {
  const changes = { xp: [], levels: [], added: [], removed: [] };
  if (!previous || !current) {
    return changes;
  }
  TRACKS.forEach((track, index) => {
    const before = previous.tracks[index];
    const after = current.tracks[index];
    if (before.xp !== null && after.xp !== null) {
      const gained =
        totalXp(track, after.level) + after.xp - (totalXp(track, before.level) + before.xp);
      if (gained > 0) {
        changes.xp.push({ track: track.name, xp: gained });
      }
    }
    for (let level = before.level + 1; level <= after.level; ++level) {
      changes.levels.push({ track: track.name, level, perkPoints: after.points });
    }
  });
  const had = new Set(previous.perks);
  const has = new Set(current.perks);
  changes.added = current.perks.filter((perk) => !had.has(perk)).map((perk) => perk.name);
  changes.removed = previous.perks.filter((perk) => !has.has(perk)).map((perk) => perk.name);
  return changes;
}

module.exports = {
  progressionFromReport,
  progressionFromDirectory,
  directoryProgression,
  levelsOf,
  perkNames,
  hasPerk,
  trackProgress,
  snapshotOf,
  progressionChanges,
};
