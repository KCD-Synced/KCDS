'use strict';

// A player's stats, the same on the server and on every client: the names
// scripts pass to getStat, derivedStat, setStat, addStat and fillStat, the snapshot the native
// side reports (its own client publishes it, kcd2s/integration/player_stats.hpp), the readings
// Player carries (health, maxHealth, skills, level...) and the changes playerStatsChanged reports.

// Writable stats, in native units.
const PlayerStat = Object.freeze({
  Health: 'health',
  Stamina: 'stamina',
  Exhaust: 'exhaust',
  Hunger: 'hunger',
});

// Read-only computed stats, in native units.
const DerivedPlayerStat = Object.freeze({
  Bleeding: 'bleeding',
  Sleeping: 'sleeping',
  Consciousness: 'consciousness',
  Drunkenness: 'drunkenness',
  Poisoning: 'poisoning',
  Charisma: 'charisma',
  Visibility: 'visibility',
  Conspicuousness: 'conspicuousness',
  Noise: 'noise',
  Dirtiness: 'dirtiness',
  Bloodiness: 'bloodiness',
  Smell: 'smell',
  SmellIntensity: 'smellIntensity',
  Fragrance: 'fragrance',
  CarriedWeight: 'carriedWeight',
  InventoryCapacity: 'inventoryCapacity',
  Encumbrance: 'encumbrance',
  Hangover: 'hangover',
  Alcoholism: 'alcoholism',
  ArmorRating: 'armorRating',
  OverallArmorDefense: 'overallArmorDefense',
  OverallWeaponAttack: 'overallWeaponAttack',
  NormalizedRunSpeed: 'normalizedRunSpeed',
  RunSpeedBase: 'runSpeedBase',
  Morale: 'morale',
});

const WRITABLE = Object.values(PlayerStat);
const DERIVED = Object.values(DerivedPlayerStat);
// The conditions travel with the writable states ("vitals"); the other derived stats are sampled.
const CONDITIONS = DERIVED.slice(0, 5);
const SAMPLED = DERIVED.slice(5);
const VITALS = [...WRITABLE, ...CONDITIONS];
const RELATIVE = ['strength', 'agility', 'vitality'];
// The abilities ("abilities"): maxima, the main level, the attributes as the game evaluates them,
// and the nine SoulSkills (as levels and as the engine's relative values).
const MAXIMA = ['maxHealth', 'maxStamina', 'staminaCap', 'maxExhaust', 'maxHunger'];
const SKILLS = [
  'fencing',
  'survival',
  'defense',
  'sword',
  'heavyWeapons',
  'marksmanship',
  'dagger',
  'largeWeapons',
  'unarmed',
];
const ABILITY_VALUES = [...MAXIMA, 'level', ...RELATIVE];

// setStat's limits (player_stats.hpp): a protocol bound, not a gameplay maximum, and the smallest
// positive health the game's state setter does not treat as zero.
const MAX_STAT_VALUE = 1000000;
const MIN_POSITIVE_HEALTH = 0.00001;

const NO_STATS = Object.freeze({ vitals: null, sampled: null, relative: null, abilities: null });

// A group of the snapshot: every name a finite number, or null for a group not reported yet.
function group(value, names, what) {
  if (value === undefined || value === null) {
    return null;
  }
  if (typeof value !== 'object') {
    throw new TypeError(`Invalid native player ${what}`);
  }
  const values = {};
  for (const name of names) {
    if (typeof value[name] !== 'number' || !Number.isFinite(value[name])) {
      throw new TypeError(`Invalid native player ${what}`);
    }
    values[name] = value[name];
  }
  return Object.freeze(values);
}

// The abilities of a native report ({ maxHealth, ... level, strength, agility, vitality,
// skills: {...}, scaledSkills: {...} }), or null for none yet.
function abilitiesFrom(value) {
  if (value === undefined || value === null) {
    return null;
  }
  if (typeof value !== 'object') {
    throw new TypeError('Invalid native player abilities');
  }
  const values = group(value, ABILITY_VALUES, 'abilities');
  // The ceiling the injuries allow never exceeds the capacity (the game reports them separately).
  const staminaCap = Math.min(values.staminaCap, values.maxStamina);
  return Object.freeze({
    ...values,
    staminaCap,
    skills: group(value.skills, SKILLS, 'skills') ?? invalidAbilities(),
    scaledSkills: group(value.scaledSkills, SKILLS, 'skills') ?? invalidAbilities(),
  });
}

function invalidAbilities() {
  throw new TypeError('Invalid native player abilities');
}

// The snapshot of a native report ({ vitals, stats, scaledStats, abilities }, each by name); a
// malformed one throws.
function statsFrom(report) {
  return Object.freeze({
    vitals: group(report.vitals, VITALS, 'vitals'),
    sampled: group(report.stats, SAMPLED, 'stats'),
    relative: group(report.scaledStats, RELATIVE, 'relative stats'),
    abilities: abilitiesFrom(report.abilities),
  });
}

// The directory form of a report: each group as an array in name order, which a client
// directory carries for every player it sees.
function directoryStats(report) {
  const entry = {};
  const pack = (key, value, names) => {
    if (value && typeof value === 'object') {
      entry[key] = names.map((name) => value[name]);
    }
  };
  pack('vitals', report.vitals, VITALS);
  pack('stats', report.stats, SAMPLED);
  pack('scaledStats', report.scaledStats, RELATIVE);
  const abilities = report.abilities;
  if (abilities && typeof abilities === 'object' && abilities.skills && abilities.scaledSkills) {
    entry.abilities = [
      ...ABILITY_VALUES.map((name) => abilities[name]),
      ...SKILLS.map((name) => abilities.skills[name]),
      ...SKILLS.map((name) => abilities.scaledSkills[name]),
    ];
  }
  return entry;
}

// A group of a directory entry: an array in name order, or null for one not reported yet.
function packedGroup(value, names, what) {
  if (value === undefined || value === null) {
    return null;
  }
  if (!Array.isArray(value) || value.length !== names.length) {
    throw new TypeError(`Invalid native player ${what}`);
  }
  return group(Object.fromEntries(names.map((name, index) => [name, value[index]])), names, what);
}

// The abilities of a directory entry (ABILITY_VALUES, then the skills, then the relative skills),
// or null for none yet.
function packedAbilities(value) {
  if (value === undefined || value === null) {
    return null;
  }
  const length = ABILITY_VALUES.length + 2 * SKILLS.length;
  if (!Array.isArray(value) || value.length !== length) {
    throw new TypeError('Invalid native player abilities');
  }
  const skillsAt = ABILITY_VALUES.length;
  const relativeAt = skillsAt + SKILLS.length;
  return abilitiesFrom({
    ...Object.fromEntries(ABILITY_VALUES.map((name, index) => [name, value[index]])),
    skills: Object.fromEntries(SKILLS.map((name, index) => [name, value[skillsAt + index]])),
    scaledSkills: Object.fromEntries(
      SKILLS.map((name, index) => [name, value[relativeAt + index]]),
    ),
  });
}

// The snapshot of a client directory entry; a malformed one throws.
function statsFromDirectory(entry) {
  return Object.freeze({
    vitals: packedGroup(entry.vitals, VITALS, 'vitals'),
    sampled: packedGroup(entry.stats, SAMPLED, 'stats'),
    relative: packedGroup(entry.scaledStats, RELATIVE, 'relative stats'),
    abilities: packedAbilities(entry.abilities),
  });
}

// SoulSkills: a fresh object, zeros without a snapshot.
function soulSkills(snapshot, relative) {
  const skills = relative ? snapshot?.abilities?.scaledSkills : snapshot?.abilities?.skills;
  return Object.fromEntries(SKILLS.map((name) => [name, skills?.[name] ?? 0]));
}

// Defines Player's readings on a prototype, the same on the server and on every
// client: `snapshotOf(handle)` is the handle's stats snapshot. Each reads 0 (zeros) without one.
// `downOf(handle)`, where given, says the player is knocked out: `ableToAct` reads
// false and `consciousness` 0, and `alive` reads false.
function defineReadings(prototype, snapshotOf, downOf = () => false) {
  const reading = (name, read) => {
    Object.defineProperty(prototype, name, {
      enumerable: true,
      get() {
        return read(snapshotOf(this), downOf(this));
      },
    });
  };
  for (const name of [...WRITABLE, ...CONDITIONS]) {
    if (name === 'consciousness') {
      reading(name, (snapshot, down) => (down ? 0 : snapshot?.vitals?.[name] ?? 0));
    } else {
      reading(name, (snapshot) => snapshot?.vitals?.[name] ?? 0);
    }
  }
  for (const name of [...MAXIMA, 'level', ...RELATIVE]) {
    reading(name, (snapshot) => snapshot?.abilities?.[name] ?? 0);
  }
  // What the nameplate bar draws: health against its maximum, 0 to 100.
  reading('healthPercent', (snapshot) => {
    const health = snapshot?.vitals?.health ?? 0;
    const max = snapshot?.abilities?.maxHealth ?? 0;
    if (!(max > 0)) {
      return 0;
    }
    return Math.round(Math.min(100, Math.max(0, (health / max) * 100)) * 100) / 100;
  });
  // Whether the player is alive: health left, not knocked out and not unconscious.
  // A knocked-out player keeps the game's lowest health (1), so health alone would read
  // them as alive. False while no report arrived.
  const alive = (snapshot, down) => {
    if (down) {
      return false;
    }
    const health = snapshot?.vitals?.health ?? 0;
    const consciousness = snapshot?.vitals?.consciousness ?? 0;
    return health > 0 && consciousness > 0;
  };
  reading('alive', alive);
  // True if the body can act, meaning it lives, is conscious and is awake.
  reading(
    'ableToAct',
    (snapshot, down) => alive(snapshot, down) && !((snapshot?.vitals?.sleeping ?? 0) > 0),
  );
  reading('skills', (snapshot) => soulSkills(snapshot, false));
  reading('scaledSkills', (snapshot) => soulSkills(snapshot, true));
}

function requireName(stat, names) {
  if (typeof stat !== 'string' || !names.includes(stat)) {
    throw new TypeError(`Unknown player stat: ${String(stat)}`);
  }
}

// Player.getStat: the latest reported value, 0 without a snapshot.
function getStat(snapshot, stat) {
  requireName(stat, WRITABLE);
  return snapshot?.vitals?.[stat] ?? 0;
}

// Player.derivedStat: the latest reported value, 0 without a snapshot.
function derivedStat(snapshot, stat) {
  requireName(stat, DERIVED);
  if (CONDITIONS.includes(stat)) {
    return snapshot?.vitals?.[stat] ?? 0;
  }
  return snapshot?.sampled?.[stat] ?? 0;
}

// Player.scaledStats: a fresh SoulStats, zeros without a snapshot.
function scaledStats(snapshot) {
  const relative = snapshot?.relative;
  return {
    strength: relative?.strength ?? 0,
    agility: relative?.agility ?? 0,
    vitality: relative?.vitality ?? 0,
  };
}

// playerStatsChanged's changes from one snapshot to the next, in the event's stat order. A group
// reported for the first time (a new connection, a new body) is a baseline, not a change.
function statChanges(previous, current) {
  const changes = [];
  const compare = (names, before, after) => {
    if (!before || !after) {
      return;
    }
    for (const stat of names) {
      if (before[stat] !== after[stat]) {
        changes.push({ stat, previous: before[stat], current: after[stat] });
      }
    }
  };
  compare(VITALS, previous?.vitals, current.vitals);
  compare(SAMPLED, previous?.sampled, current.sampled);
  return changes;
}

// Player.setStat's request: { stat: its index (0 health, 1 stamina, 2 exhaust, 3 hunger), value }.
// Throws for a derived or unknown stat and for a value the native side would refuse.
function statRequest(stat, value) {
  if (typeof stat === 'string' && DERIVED.includes(stat)) {
    throw new TypeError(`${stat} is a derived stat and cannot be set`);
  }
  requireName(stat, WRITABLE);
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new TypeError('setStat takes a finite number');
  }
  if (value < 0 || value > MAX_STAT_VALUE) {
    throw new RangeError(`setStat takes a value from 0 to ${MAX_STAT_VALUE}`);
  }
  // What travels is a float: a positive value must stay positive, positive health above the
  // smallest the game keeps.
  const native = Math.fround(value);
  if (value > 0 && native === 0) {
    throw new RangeError('setStat value is too small to be represented');
  }
  if (stat === PlayerStat.Health && native > 0 && native <= Math.fround(MIN_POSITIVE_HEALTH)) {
    throw new RangeError(`Positive health must be above ${MIN_POSITIVE_HEALTH}`);
  }
  return { stat: WRITABLE.indexOf(stat), value };
}

// Player.addStat's request: { stat: its index, delta }, a change the owner's game
// adds to its current value, within 0 and the state's maximum. Throws for a derived or unknown
// stat and for a change the native side would refuse.
function statChangeRequest(stat, delta) {
  if (typeof stat === 'string' && DERIVED.includes(stat)) {
    throw new TypeError(`${stat} is a derived stat and cannot be changed`);
  }
  requireName(stat, WRITABLE);
  if (typeof delta !== 'number' || !Number.isFinite(delta)) {
    throw new TypeError('addStat takes a finite number');
  }
  if (Math.abs(delta) > MAX_STAT_VALUE) {
    throw new RangeError(`addStat takes a change from -${MAX_STAT_VALUE} to ${MAX_STAT_VALUE}`);
  }
  // What travels is a float: a change must not vanish on the way.
  if (delta !== 0 && Math.fround(delta) === 0) {
    throw new RangeError('addStat change is too small to be represented');
  }
  return { stat: WRITABLE.indexOf(stat), delta };
}

// Player.fillStat's request: setStat to the largest value it takes, which the owner's game
// clamps to the state's own maximum.
function fillRequest(stat) {
  if (typeof stat === 'string' && DERIVED.includes(stat)) {
    throw new TypeError(`${stat} is a derived stat and cannot be filled`);
  }
  return statRequest(stat, MAX_STAT_VALUE);
}

module.exports = {
  PlayerStat,
  DerivedPlayerStat,
  MAX_STAT_VALUE,
  MIN_POSITIVE_HEALTH,
  NO_STATS,
  defineReadings,
  statsFrom,
  directoryStats,
  statsFromDirectory,
  getStat,
  derivedStat,
  scaledStats,
  statChanges,
  statRequest,
  statChangeRequest,
  fillRequest,
};
