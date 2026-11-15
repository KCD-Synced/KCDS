'use strict';

// NPC daily schedules (Npc.setSchedule): a list of entries by the hour of the world clock, each a
// place to go and, optionally, what to do there. When the world's hour reaches an entry, the NPC is
// ordered there: a useHere with the entry's action, or a moveTo without one. npcScheduleEntry
// tells a script that an entry started. Between entries the NPC keeps whatever order it has, so a
// script may give it others; the next entry orders it again.
//
// One timer every kTickMs looks only at the NPCs that have a schedule; an NPC that is gone leaves
// the list on that tick.

const { moveOptionsOf, animationRequestOf } = require('./npc_values.cjs');

const kTickMs = 2000;
const kMostEntries = 48;
const kWalkKeys = ['speed', 'radius', 'pathfinding'];
const kActionKeys = ['fragment', 'tags', 'clip', 'loop', 'facing'];
const kEntryKeys = new Set(['hour', 'position', ...kWalkKeys, ...kActionKeys]);

function finitePosition(position) {
  return (
    position !== null &&
    typeof position === 'object' &&
    [position.x, position.y, position.z].every(
      (value) => typeof value === 'number' && Number.isFinite(value),
    )
  );
}

// One entry as it is kept: { hour, position, walk, action | null }. Throws for what it cannot use.
function entryOf(entry, index) {
  const where = `setSchedule entry ${index}`;
  if (entry === null || typeof entry !== 'object') {
    throw new TypeError(`${where} must be an object`);
  }
  for (const key of Object.keys(entry)) {
    if (!kEntryKeys.has(key)) {
      throw new TypeError(`${where} has an unknown option ${key}`);
    }
  }
  const { hour, position } = entry;
  if (typeof hour !== 'number' || !Number.isFinite(hour) || hour < 0 || hour >= 24) {
    throw new RangeError(`${where} hour must be from 0 to below 24`);
  }
  if (!finitePosition(position)) {
    throw new TypeError(`${where} position must be { x, y, z }`);
  }
  const walk = {};
  for (const key of kWalkKeys) {
    if (entry[key] !== undefined) {
      walk[key] = entry[key];
    }
  }
  moveOptionsOf(walk, where);
  let action = null;
  if (kActionKeys.some((key) => entry[key] !== undefined)) {
    action = {};
    for (const key of kActionKeys) {
      if (entry[key] !== undefined) {
        action[key] = entry[key];
      }
    }
    const { fragment = '', facing, ...played } = action;
    if (animationRequestOf(fragment, played) === null) {
      throw new TypeError(`${where} needs a fragment or a clip for its action`);
    }
    if (facing !== undefined && !(typeof facing === 'number' && Number.isFinite(facing))) {
      throw new TypeError(`${where} facing must be a number of degrees`);
    }
  }
  return {
    hour,
    position: { x: position.x, y: position.y, z: position.z },
    walk,
    action,
  };
}

// The index of the entry that holds at `hour`: the last one that started at or before it, or,
// before the first of the day, the day's last one (it began yesterday).
function activeIndex(entries, hour) {
  let found = entries.length - 1;
  for (let index = 0; index < entries.length; index += 1) {
    if (entries[index].hour <= hour) {
      found = index;
    }
  }
  return found;
}

class NpcSchedules {
  // `registry` is the NPC registry (npcs.cjs); `hour` reads the world clock's hour.
  constructor(registry, hour) {
    this.registry = registry;
    this.hour = hour;
    // npc -> { entries, active }
    this.schedules = new Map();
    this.timer = null;
  }

  // Npc.setSchedule: replaces the NPC's schedule; null or [] removes it.
  set(npc, entries) {
    this.registry.requireLive(npc);
    if (entries === null || entries === undefined) {
      this.remove(npc);
      return;
    }
    if (!Array.isArray(entries)) {
      throw new TypeError('setSchedule takes an array of entries, or null');
    }
    if (entries.length > kMostEntries) {
      throw new RangeError(`setSchedule takes at most ${kMostEntries} entries`);
    }
    const kept = entries.map(entryOf).sort((a, b) => a.hour - b.hour);
    if (kept.length === 0) {
      this.remove(npc);
      return;
    }
    this.schedules.set(npc, { entries: kept, active: -1 });
    this.step(npc, this.schedules.get(npc));
    this.ensureTimer();
  }

  // Npc.schedule: a copy of the entries, by hour, or null.
  get(npc) {
    const schedule = this.schedules.get(npc);
    if (!schedule) {
      return null;
    }
    return schedule.entries.map((entry) => ({
      hour: entry.hour,
      position: { ...entry.position },
      ...entry.walk,
      ...(entry.action ?? {}),
    }));
  }

  remove(npc) {
    this.schedules.delete(npc);
    if (this.schedules.size === 0) {
      this.stop();
    }
  }

  ensureTimer() {
    if (!this.timer) {
      this.timer = setInterval(() => this.tick(), kTickMs);
      this.timer.unref?.();
    }
  }

  stop() {
    if (this.timer) {
      clearInterval(this.timer);
      this.timer = null;
    }
  }

  tick() {
    for (const [npc, schedule] of [...this.schedules]) {
      if (!this.registry.live(npc)) {
        this.remove(npc);
        continue;
      }
      this.step(npc, schedule);
    }
  }

  // Orders the NPC to the entry that holds now when it is another than the last one ordered.
  step(npc, schedule) {
    const index = activeIndex(schedule.entries, this.hour());
    if (index === schedule.active) {
      return;
    }
    schedule.active = index;
    const entry = schedule.entries[index];
    try {
      if (entry.action) {
        npc.useHere(entry.position, { ...entry.walk, ...entry.action });
      } else {
        npc.moveTo(entry.position, entry.walk);
      }
    } catch (error) {
      this.registry.host.log?.(`schedule of ${npc}: ${error.message}`);
      return;
    }
    this.registry.host.emit(
      'npcScheduleEntry',
      npc,
      Object.freeze({ index, hour: entry.hour, position: { ...entry.position } }),
    );
  }
}

module.exports = { NpcSchedules, activeIndex, kMostEntries };
