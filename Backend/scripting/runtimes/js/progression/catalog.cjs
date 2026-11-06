'use strict';

// The game's skills, stats and perks, the same on the server and in every
// client's resource runtime: what Progression.tracks, perkNamed, perks and xpNeeded answer.
// catalog.json is built from a game installation by Dev/tools/progression/build_catalog.py.

const catalog = require('./catalog.json');

const TRACKS = catalog.tracks;
const TRACK_BY_NAME = new Map(TRACKS.map((track) => [track.name, track]));
// A copy: a resource's perks join it (addPerkTablePatch), the game's list stays as it is.
const PERKS = [...catalog.perks];
const PERK_BY_NAME = new Map(PERKS.map((perk) => [perk.name, perk]));
const PERK_BY_ID = new Map(PERKS.map((perk) => [perk.id.toLowerCase(), perk]));
// The perk-point bank of the main level, beside one per track.
const MAIN_BANK = 'main';

function trackInfo(track) {
  return { name: track.name, kind: track.kind, cap: track.cap, induced: track.induced };
}

function perkInfo(perk) {
  return {
    name: perk.name,
    id: perk.id,
    uiName: perk.uiName,
    track: perk.track,
    level: perk.level,
    parent: perk.parent,
    visibility: perk.visibility,
    kinds: [...perk.kinds],
    autolearnable: perk.autolearnable,
  };
}

// The catalog's track of that name; throws for anything else.
function requireTrack(name) {
  const track = typeof name === 'string' ? TRACK_BY_NAME.get(name) : undefined;
  if (!track) {
    throw new TypeError(`Unknown track: ${String(name)}`);
  }
  return track;
}

// The catalog's perk of that name or GUID, or undefined.
function lookupPerk(query) {
  if (typeof query !== 'string') {
    return undefined;
  }
  return PERK_BY_NAME.get(query) ?? PERK_BY_ID.get(query.toLowerCase());
}

// The catalog's perk of that name or GUID; throws for anything else.
function requirePerk(query) {
  const perk = lookupPerk(query);
  if (!perk) {
    throw new TypeError(`Unknown perk: ${String(query)}`);
  }
  return perk;
}

// The total XP a track costs from level 0 to `level`: reaching level n costs base + diff * n.
function totalXp(track, level) {
  return level * track.base + (track.diff * level * (level + 1)) / 2;
}

// Progression's read members, the same on both sides.
const progressionCatalog = Object.freeze({
  tracks() {
    return TRACKS.map(trackInfo);
  },
  perkNamed(query) {
    if (typeof query !== 'string') {
      throw new TypeError('perkNamed takes a perk name or GUID');
    }
    const perk = lookupPerk(query);
    return perk ? perkInfo(perk) : null;
  },
  perks(filter = {}) {
    if (filter === null || typeof filter !== 'object') {
      throw new TypeError('perks takes a filter object');
    }
    const { track, visibleOnly } = filter;
    if (track !== undefined && track !== MAIN_BANK) {
      requireTrack(track);
    }
    if (visibleOnly !== undefined && typeof visibleOnly !== 'boolean') {
      throw new TypeError('perks filter visibleOnly must be a boolean');
    }
    return PERKS.filter(
      (perk) =>
        (track === undefined || perk.track === track) &&
        (!visibleOnly || perk.visibility === 'visible'),
    ).map(perkInfo);
  },
  xpNeeded(trackName, level) {
    const track = requireTrack(trackName);
    if (!Number.isInteger(level) || level < 0 || level > track.cap) {
      throw new RangeError(`xpNeeded takes a level from 0 to ${track.cap}`);
    }
    return totalXp(track, level);
  },
});

// A perk table row's visibility, as the catalog names it (Dev/tools/progression/build_catalog.py).
const VISIBILITY = Object.freeze({ 0: 'system', 1: 'hidden', 2: 'visible', 3: 'obsolete' });
// The perks a resource's perk table patch adds, by perk GUID in lower case.
const CUSTOM_PERK_IDS = new Set();

function attributesOf(tag) {
  const attributes = {};
  for (const [, name, value] of tag.matchAll(/([A-Za-z_][\w.-]*)="([^"]*)"/g)) {
    attributes[name] = value
      .replaceAll('&quot;', '"')
      .replaceAll('&apos;', "'")
      .replaceAll('&lt;', '<')
      .replaceAll('&gt;', '>')
      .replaceAll('&amp;', '&');
  }
  return attributes;
}

// The track a perk row belongs to, as the catalog builder decides it.
function trackOfRow(row, level) {
  const tree = (kind, selector) =>
    TRACKS.find((track) => track.kind === kind && track.id === Number(selector))?.name ?? null;
  if (row.skill_selector !== undefined) {
    return tree('skill', row.skill_selector);
  }
  if (row.stat_selector !== undefined) {
    return tree('stat', row.stat_selector);
  }
  return level > 0 ? MAIN_BANK : null;
}

// A resource's patch of the game's perk table (replace/Libs/Tables/rpg/perk__<name>.xml): its new
// perks join the catalog, so Progression.perks, perkNamed and a player's perk members know them.
// A row of a perk the catalog has already is left to the game's own entry. Answers the new perks'
// names.
function addPerkTablePatch(text) {
  const added = [];
  for (const [tag] of String(text).matchAll(/<perk\s[^>]*>/g)) {
    const row = attributesOf(tag);
    const id = typeof row.perk_id === 'string' ? row.perk_id.toLowerCase() : '';
    const name = row.perk_name;
    if (!/^[0-9a-f-]{36}$/.test(id) || !name || PERK_BY_ID.has(id) || PERK_BY_NAME.has(name)) {
      continue;
    }
    const level = Number.parseInt(row.level ?? '0', 10) || 0;
    const perk = Object.freeze({
      name,
      id: row.perk_id,
      uiName: row.perk_ui_name ?? '',
      track: trackOfRow(row, level),
      level,
      parent: null,
      visibility: VISIBILITY[row.visibility] ?? 'visible',
      kinds: Object.freeze([]),
      autolearnable: row.autolearnable === 'true',
    });
    PERKS.push(perk);
    PERK_BY_NAME.set(name, perk);
    PERK_BY_ID.set(id, perk);
    CUSTOM_PERK_IDS.add(id);
    added.push(name);
  }
  return added;
}

// Whether a perk came from a resource's perk table patch.
function isCustomPerk(perk) {
  return Boolean(perk) && CUSTOM_PERK_IDS.has(String(perk.id).toLowerCase());
}

module.exports = {
  MAIN_BANK,
  TRACKS,
  progressionCatalog,
  requireTrack,
  requirePerk,
  lookupPerk,
  totalXp,
  addPerkTablePatch,
  isCustomPerk,
};
