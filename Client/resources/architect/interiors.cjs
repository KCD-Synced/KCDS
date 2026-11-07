'use strict';

// The game's own interiors around a point, as the level marks them: a building with rooms has an
// environment probe (EnvironmentLight, its box the room's) and a room shape (AreaShape) per room,
// and doors (AnimDoor); each such entity names its building in brackets after its own name
// ("envprobe_vlasska_11_house_6[structures/living/houses/.../vlasska_11_house1_<guid>]"). A
// building of the level without them (St James's church) has no inside to enter: open_model
// opens it.
//
// Pure: the editor (editor.cjs) and an AI assistant's commands (ai_commands.cjs) give it the
// entities and a reader of an entity's properties.

// The building an entity belongs to (its prefab), or null for one of the level's own.
function buildingOf(name) {
  const found = /\[([^\]]+)\]/.exec(String(name ?? ''));
  return found ? found[1] : null;
}

// A building's readable name: its prefab's last part without the trailing id
// (vlasska_11_house1_4d58f1d4-...: vlasska_11_house1).
function labelOf(building) {
  const last = building.split('/').pop();
  return last.replace(/_[0-9a-f]{8}-[0-9a-f-]{27}$/i, '');
}

// The number of a property (as entityProperties answers them), or null.
function numberOf(properties, key) {
  const found = (properties ?? []).find((property) => property.key === key);
  return found && typeof found.value === 'number' ? found.value : null;
}

// The interiors among entities ({ class, name, position }): [{ building, label, at (the middle
// of its rooms), rooms: [{ name, at, box: { x, y, z } | null }], doors: [{ name, at }], shapes }]
// nearest to `from` first. `properties(entity)` reads a probe's (its room's box); `most` probes
// are read at most.
function interiorsOf(entities, from, properties, most = 24) {
  const buildings = new Map();
  const of = (name) => {
    const building = buildingOf(name);
    if (!building) {
      return null;
    }
    if (!buildings.has(building)) {
      buildings.set(building, {
        building,
        label: labelOf(building),
        rooms: [],
        doors: [],
        shapes: [],
      });
    }
    return buildings.get(building);
  };
  let read = 0;
  for (const entity of entities) {
    const entry = of(entity.name);
    if (!entry) {
      continue;
    }
    const at = { x: entity.position.x, y: entity.position.y, z: entity.position.z };
    const own = String(entity.name).split('[')[0];
    if (entity.class === 'EnvironmentLight') {
      let box = null;
      if (read < most && typeof properties === 'function') {
        read += 1;
        const given = properties(entity);
        const x = numberOf(given, 'BoxSizeX');
        const y = numberOf(given, 'BoxSizeY');
        const z = numberOf(given, 'BoxSizeZ');
        box = x && y && z ? { x, y, z } : null;
      }
      entry.rooms.push({ name: own, at, box });
    } else if (entity.class === 'AnimDoor') {
      entry.doors.push({ name: own, at });
    } else if (entity.class === 'AreaShape') {
      entry.shapes.push(own);
    }
  }
  const away = (at) => Math.hypot(at.x - from.x, at.y - from.y);
  return [...buildings.values()]
    .filter((entry) => entry.rooms.length > 0 || entry.shapes.length > 0)
    .map((entry) => {
      const points = entry.rooms.length > 0 ? entry.rooms.map((room) => room.at) : [];
      const middle =
        points.length > 0
          ? {
              x: points.reduce((sum, at) => sum + at.x, 0) / points.length,
              y: points.reduce((sum, at) => sum + at.y, 0) / points.length,
              z: Math.min(...points.map((at) => at.z)),
            }
          : null;
      return { ...entry, at: middle };
    })
    .sort((a, b) => (a.at ? away(a.at) : Infinity) - (b.at ? away(b.at) : Infinity));
}

// The room of an interior whose box holds `at` (a probe's box is centred on it), or null.
function roomAt(interiors, at) {
  for (const interior of interiors) {
    for (const room of interior.rooms) {
      if (!room.box) {
        continue;
      }
      const inside =
        Math.abs(at.x - room.at.x) <= room.box.x / 2 &&
        Math.abs(at.y - room.at.y) <= room.box.y / 2 &&
        Math.abs(at.z - room.at.z) <= room.box.z / 2;
      if (inside) {
        return { interior, room };
      }
    }
  }
  return null;
}

// A room this near a level building's place (metres) is one of its rooms.
const ROOM_NEAR = 20;

// The game's own interior of a level building (its model `mesh` standing at `at`), one of
// `interiors` (interiorsOf), or null for a building closed inside. An interior's entities name
// the building's prefab: a path past objects/<kind>/ and an id. The prefab bundles several models
// under its own name (vlasska_11_house1 of vlasska_11_house.cgf and its parts), so a model's
// interior is one of a prefab in the same folder with a room near it; a neighbour's rooms are not
// its own.
function interiorOfModel(mesh, at, interiors, near = ROOM_NEAR) {
  const folderOf = (path) => path.slice(0, path.lastIndexOf('/'));
  const key = String(mesh ?? '')
    .replace(/\\/g, '/')
    .toLowerCase()
    .replace(/^objects\/[^/]+\//, '');
  return (
    interiors.find(
      (interior) =>
        folderOf(interior.building.toLowerCase()) === folderOf(key) &&
        interior.rooms.some((room) => Math.hypot(room.at.x - at.x, room.at.y - at.y) <= near),
    ) ?? null
  );
}

module.exports = { interiorsOf, interiorOfModel, roomAt, buildingOf, labelOf };
