'use strict';

// The level entities that make one thing with a level entity you move or hide.
//
// A chair the level built from a prefab is several entities: the chair you see (Chair), the seat a
// body sits on (StanceSmartObject), the trigger that offers Sit (ActionTrigger), where a body sits
// down and gets up (TagPoint), its schedule's hub. Moved alone, the chair stood somewhere else
// while its seat and its Sit stayed where it had been: nobody could sit on it any more. The
// entities one prefab made share their GUID's first 16 and last 24 bits (the middle tells them
// apart); those near the moved entity move, turn and hide with it.

// How far from the entity its companions stand at most (a bench's far end, a table's chairs are
// their own prefabs).
const COMPANION_REACH = 3;

// Whether two entity GUIDs (16 hex digits) come from one prefab: their first 16 and last 24 bits.
function sameInstance(a, b) {
  if (typeof a !== 'string' || typeof b !== 'string' || a.length !== 16 || b.length !== 16) {
    return false;
  }
  const first = a.toLowerCase();
  const second = b.toLowerCase();
  return (
    first !== second &&
    first.slice(0, 4) === second.slice(0, 4) &&
    first.slice(10) === second.slice(10)
  );
}

// The companions of a level entity among the entities around it (World.entitiesAround's,
// each with its rotation): what a level edit needs to find, move and turn each.
function companionsOf(object, entities) {
  const out = [];
  for (const entity of entities ?? []) {
    if (!sameInstance(object.guid, entity.guid) || !entity.position) {
      continue;
    }
    const at = entity.position;
    const away = Math.hypot(at.x - object.at.x, at.y - object.at.y, at.z - object.at.z);
    if (away > COMPANION_REACH) {
      continue;
    }
    const rotation = entity.rotation ?? { x: 0, y: 0, z: 0, w: 1 };
    out.push({
      kind: 'entity',
      name: entity.name ?? '',
      guid: entity.guid.toLowerCase(),
      class: entity.class ?? '',
      at: { x: at.x, y: at.y, z: at.z },
      rotation: { x: rotation.x, y: rotation.y, z: rotation.z, w: rotation.w },
      edit: 0,
    });
  }
  return out;
}

// Where a companion stands and how it is turned when its entity, at home at `home`, stands at `at`
// turned by `degrees` from the level's turn: shifted with it and turned about it.
function companionPose(companion, home, at, degrees, multiplied) {
  const radians = (degrees * Math.PI) / 180;
  const [s, c] = [Math.sin(radians), Math.cos(radians)];
  const dx = companion.at.x - home.x;
  const dy = companion.at.y - home.y;
  const half = radians / 2;
  const turn = { x: 0, y: 0, z: Math.sin(half), w: Math.cos(half) };
  return {
    position: {
      x: at.x + dx * c - dy * s,
      y: at.y + dx * s + dy * c,
      z: at.z + (companion.at.z - home.z),
    },
    rotation: multiplied(turn, companion.rotation),
  };
}

// A companion as a work saves it and reads it back (no edit number: those are this game's).
function savedCompanion(companion) {
  return {
    kind: 'entity',
    name: companion.name,
    guid: companion.guid,
    class: companion.class,
    at: { ...companion.at },
    rotation: { ...companion.rotation },
  };
}

function readCompanions(list) {
  const out = [];
  for (const item of Array.isArray(list) ? list : []) {
    if (typeof item?.guid !== 'string' || !item.at || !item.rotation) {
      continue;
    }
    out.push({ ...savedCompanion(item), edit: 0 });
  }
  return out;
}

module.exports = {
  COMPANION_REACH,
  sameInstance,
  companionsOf,
  companionPose,
  savedCompanion,
  readCompanions,
};
