'use strict';

// The client-only WorldMap global: the map screen at this machine
// and the player's own marker on it, which the game calls the checkpoint.
//
// The game client reports the map's state when it changes ({ op: 'worldMap', open, waypoint })
// and the player's own marker changes ({ op: 'worldMapEvent', name, where, moved }); the
// calls that change the marker go back as { op: 'worldMapMove', x, y, z } and
// { op: 'worldMapClear' } and run in the game's own code on the next frame. Nothing here is
// replicated.

// The game's own compass and map mark types, by name.
const MARK_TYPES = Object.freeze({
  Checkpoint: 0,
  Main: 1,
  Side: 2,
  Micro: 3,
  QuestGiver: 4,
  ActivityGiver: 5,
  Hub: 6,
  FastTravel: 7,
  FastTravelLevel: 8,
  Dog: 9,
  Dlcs: 10,
  Home: 11,
  Shop: 12,
  Blacksmith: 13,
  Armourer: 14,
  Weaponsmiths: 15,
  Shoemaker: 16,
  Tailor: 17,
  Pub: 18,
  Apothecary: 19,
  Herbalist: 20,
  Miller: 21,
  HorseTrader: 22,
  Saddler: 23,
  Bailiff: 24,
  Arena: 25,
  ArcheryArena: 26,
  FistFight: 27,
  Alchemy: 28,
  SharpeningWheel: 29,
  Baths: 30,
  HuntingSpot: 31,
  HuntingSpotBoar: 32,
  FishingSpot: 34,
  Hive: 36,
  ForestGarden: 37,
  Camp: 38,
  CampEnemy: 39,
  MineEnrtrance: 42,
  Grave: 43,
  Nest: 44,
  ConcCross: 45,
  Shrine: 46,
  Bed: 47,
  GeneralPoi: 48,
  Bakery: 49,
  Butchery: 50,
  VegetableShop: 51,
  Tanner: 52,
  Hunter: 53,
  Scribe: 55,
  Hotel: 56,
  HuntingSpotRoe: 57,
  HuntingSpotWolf: 58,
  Smithy: 59,
  SkillTeacher: 69,
  FightArena: 70,
  PoiTipster: 71,
  SmokingFood: 72,
  DryingFood: 73,
  Washing: 74,
  Indulgences: 75,
  Gunsmith: 76,
  DiceTable: 77,
  FistFightArena: 78,
  DLC0: 79,
  BedPlayer: 80,
  Barber: 81,
  ShieldPainter: 82,
  DLC1: 83,
  DLC2: 84,
  DLC3: 85,
  Racing: 86,
  DLC2_smithing: 87,
  DLC2_dice: 88,
  DLC2_acquiringPackages: 89,
  DLC2_archery: 90,
  DLC2_donations: 91,
  DLC2_duels: 92,
  DLC2_stealingPackages: 93,
  DLC2_activities: 94,
  SellingChest: 95,
  FastTravelSedlec: 96,
});
// Multiplayer shows only town names (Hub) unless a resource turns a category on.
const SHOWN_BY_DEFAULT = new Set(['Hub']);

// A GUID as the game writes one, and the place id scripts get of it (locationDiscovered,
// poiDiscovered): the GUID's fields, the last eight bytes in their order.
const GUID_TEXT = /^([0-9a-f]{8})-([0-9a-f]{4})-([0-9a-f]{4})-([0-9a-f]{4})-([0-9a-f]{12})$/i;

function locationIdOf(text) {
  const parts = typeof text === 'string' ? GUID_TEXT.exec(text) : null;
  if (!parts) {
    return null;
  }
  const tail = parts[4] + parts[5];
  const data4 = new Uint8Array(8);
  for (let index = 0; index < 8; index++) {
    data4[index] = parseInt(tail.slice(index * 2, index * 2 + 2), 16);
  }
  return {
    data1: parseInt(parts[1], 16),
    data2: parseInt(parts[2], 16),
    data3: parseInt(parts[3], 16),
    data4,
  };
}

function markType(type) {
  if (typeof type !== 'string' || !Object.hasOwn(MARK_TYPES, type)) {
    throw new RangeError(`WorldMap: unknown mark type ${type}`);
  }
  return type;
}

// The game's text of a LocationId { data1, data2, data3, data4 } (8-4-4-4-12 hex digits), or
// null for anything else.
function guidText(id) {
  if (!id || typeof id !== 'object') {
    return null;
  }
  const { data1, data2, data3, data4 } = id;
  const word = (value, max) => Number.isInteger(value) && value >= 0 && value <= max;
  if (!word(data1, 0xffffffff) || !word(data2, 0xffff) || !word(data3, 0xffff)) {
    return null;
  }
  // A script's Uint8Array may come from another realm: any view of eight bytes.
  if (!ArrayBuffer.isView(data4) || data4.length !== 8) {
    return null;
  }
  const bytes = [...data4];
  if (!bytes.every((byte) => word(byte, 0xff))) {
    return null;
  }
  const hex = (value, digits) => value.toString(16).padStart(digits, '0');
  const tail = bytes.map((byte) => hex(byte, 2)).join('');
  return `${hex(data1, 8)}-${hex(data2, 4)}-${hex(data3, 4)}-${tail.slice(0, 4)}-${tail.slice(4)}`;
}

class ResourceWorldMap {
  constructor({ send, sync, emit, Vector3 }) {
    this.send = send;
    this.sync = sync;
    this.emit = emit;
    this.Vector3 = Vector3;
    this.open = false;
    this.waypoint = null;
    this.compassMarks = 0;
    // mark type -> visible, set by resources
    this.categories = new Map();
    this.inWorld = false;
    this.travel = { active: false, x: 0, y: 0, z: 0, canStart: null };
  }

  vector({ x, y, z }) {
    return new this.Vector3(x, y, z);
  }

  receiveState({ open, waypoint, compassMarks, travel, inWorld }) {
    const wasOpen = this.open;
    this.inWorld = Boolean(inWorld);
    this.travel =
      travel && typeof travel === 'object'
        ? {
            active: Boolean(travel.active),
            x: travel.x,
            y: travel.y,
            z: travel.z,
            canStart: typeof travel.canStart === 'number' ? travel.canStart : null,
          }
        : { active: false, x: 0, y: 0, z: 0 };
    this.compassMarks = Number.isInteger(compassMarks) ? compassMarks : 0;
    this.open = Boolean(open);
    this.waypoint =
      waypoint && typeof waypoint === 'object'
        ? { x: waypoint.x, y: waypoint.y, z: waypoint.z, mapId: waypoint.mapId }
        : null;
    if (this.open && !wasOpen) {
      this.emit('mapOpened', []);
    } else if (!this.open && wasOpen) {
      this.emit('mapClosed', []);
    }
  }

  receiveEvent({ name, where, moved }) {
    if (!where || typeof where !== 'object') {
      return;
    }
    if (name === 'mapWaypointSet') {
      this.emit('mapWaypointSet', [this.vector(where), where.mapId, Boolean(moved)]);
    } else if (name === 'mapWaypointCleared') {
      this.emit('mapWaypointCleared', [this.vector(where), where.mapId]);
    }
  }

  // A place the game discovered: { kind: 'location' | 'poi', id: its GUID text }.
  receiveDiscovered({ kind, id }) {
    const place = locationIdOf(id);
    if (!place) {
      return;
    }
    if (kind === 'location') {
      this.emit('locationDiscovered', [place]);
    } else if (kind === 'poi') {
      this.emit('poiDiscovered', [place]);
    }
  }

  facade() {
    const map = this;
    return Object.freeze({
      get isOpen() {
        return map.open;
      },
      getWaypoint() {
        if (!map.waypoint) {
          return null;
        }
        return { position: map.vector(map.waypoint), mapId: map.waypoint.mapId };
      },
      shiftWaypoint(position) {
        if (position === null || typeof position !== 'object') {
          throw new TypeError('WorldMap: position must be a Vector3');
        }
        const { x, y, z } = position;
        if (![x, y, z].every((value) => typeof value === 'number' && Number.isFinite(value))) {
          throw new TypeError('WorldMap: position must be a Vector3');
        }
        if (!map.waypoint) {
          return false;
        }
        map.waypoint = { x, y, z, mapId: map.waypoint.mapId };
        map.send({ op: 'worldMapMove', x, y, z });
        return true;
      },
      mapCategoryShown(type) {
        const name = markType(type);
        return map.categories.has(name) ? map.categories.get(name) : SHOWN_BY_DEFAULT.has(name);
      },
      setMapCategory(type, visible) {
        const name = markType(type);
        if (typeof visible !== 'boolean') {
          throw new TypeError('WorldMap: visible must be a boolean');
        }
        map.categories.set(name, visible);
        const shown = [];
        const hidden = [];
        for (const [category, value] of map.categories) {
          (value ? shown : hidden).push(MARK_TYPES[category]);
        }
        map.send({ op: 'worldMapCategories', shown, hidden });
        return map.inWorld;
      },
      clearWaypoint() {
        if (!map.waypoint) {
          return false;
        }
        map.send({ op: 'worldMapClear' });
        return true;
      },
      // Clears the whole level's fog of war, the fog only: places are still
      // discovered as the player finds them. False before the level is up.
      clearAllFog() {
        if (!map.inWorld) {
          return false;
        }
        map.send({ op: 'worldMapRevealFog' });
        return true;
      },
      // Marks a place discovered on this player's map silently: no toast, no XP, no
      // trigger. True when the id named a place of the loaded level.
      discover(id) {
        const guid = guidText(id);
        if (guid === null) {
          throw new TypeError('WorldMap.discover takes a LocationId');
        }
        if (!map.inWorld || !map.sync) {
          return false;
        }
        return map.sync.call({ op: 'worldMapDiscover', guid }) === true;
      },
      // Removes map fog around a point using the normal visit radius, as walking there
      // does; only x and y count. False before the level is up.
      clearFog(position) {
        const x = position?.x;
        const y = position?.y;
        if (typeof x !== 'number' || typeof y !== 'number' || !Number.isFinite(x + y)) {
          throw new TypeError('WorldMap.clearFog takes a position');
        }
        if (!map.inWorld) {
          return false;
        }
        map.send({ op: 'worldMapRevealFogAt', x, y });
        return true;
      },
    });
  }
}

// The client-only Compass global: the mark count the game client samples, and the strip's
// visibility as a hold on the HUD's Compass clip (hud.cjs), which hides the whole strip.
function compassFacade(map, hud) {
  return Object.freeze({
    get markerTotal() {
      return map.compassMarks;
    },
    get visible() {
      return hud.hudPartShown('Compass');
    },
    set visible(value) {
      hud.setHudPart('Compass', Boolean(value));
    },
  });
}

// The client-only Travel global: the local player's fast travel, to read.
function travelFacade(map) {
  return Object.freeze({
    get active() {
      return map.travel.active;
    },
    get position() {
      return map.travel.active ? map.vector(map.travel) : new map.Vector3(0, 0, 0);
    },
    canStart() {
      return map.travel.canStart;
    },
  });
}

module.exports = { ResourceWorldMap, compassFacade, travelFacade, MARK_TYPES };
