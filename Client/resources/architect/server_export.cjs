'use strict';

// World Architect's Export for server:
// the work as a KCDS resource folder a server runs, written by the game
// client into Documents\KCDS\World Architect\exports\<folder>\ (architect/saves.hpp):
//
//   package.json   the resource architect_<map>, its server and client scripts
//   server.js      what every player sees from the server: buildings and props (Prop.spawn), the
//                  level's objects hidden, moved or scaled (LevelEdit.apply), zones (Area.create),
//                  effects (Vfx.spawn) and lights (Light.spawn); where players appear (the
//                  spawn areas, or on a custom map the author's place)
//   client.js      what each player's game makes for itself when the player is ready, the same
//                  for all: the ground shaped (Terrain), plants (Vegetation), invisible
//                  collision (Collision) and the world sounds placed (Audio.loopAt)
//   README.txt     how to put it on a server
//   maps/<map>/    on a custom map, the map itself (level.pak, map.json), copied by the game
//                  client; the players' games receive it with the resource (assets.cjs)
//
// The work goes into the scripts as data (WORK), so the resource needs no other file.

function serverScript() {
  return `'use strict';
// World Architect export: what every player sees, made by the server when the resource starts.
const { Prop, LevelEdit, Area, Vfx, Light, World } = kcds.world;
const { Npc } = kcds.characters;
const { Vector3, Quaternion } = kcds.math;
const WORK = require('./work.json');

const point = (at) => new Vector3(at.x, at.y, at.z);
const turnOf = (q) => new Quaternion(q.w, q.x, q.y, q.z);
const made = { props: 0, edits: 0, areas: 0, effects: 0, lights: 0, people: 0, failed: 0 };

function attempt(kind, make) {
  try {
    make();
    made[kind] += 1;
  } catch (error) {
    made.failed += 1;
    console.warn(\`architect_\${WORK.map}: \${kind}: \${error.message}\`);
  }
}

for (const building of WORK.buildings) {
  attempt('props', () =>
    Prop.spawn('prefab:' + building.name, point(building.at), { x: 0, y: 0, z: building.turn }),
  );
}
for (const prop of WORK.props) {
  attempt('props', () => {
    const made = Prop.spawn(prop.name, point(prop.at), turnOf(prop.rotation), prop.scale);
    if (prop.door) {
      door(made, prop);
    }
  });
}

// A door or gate of the author's: the use key opens it along its way (the poses, in its seconds)
// and closes it the same way back; it closes by itself after closeAfter seconds (0: it stays open).
function door(made, prop) {
  const { path, seconds, closeAfter, use } = prop.door;
  const poses = [{ at: prop.at, rotation: prop.rotation }, ...path];
  const stepMs = Math.max(50, Math.round((seconds * 1000) / path.length));
  let open = false;
  let moving = false;
  let closer = null;
  const hint = World.addInteraction(
    {
      position: point(use.at),
      // Metres from the camera to the door: a player standing a step before a door leaf is
      // about 2.7 m from it (2.5 showed nothing there, live).
      radius: 3.5,
      text: 'Open',
      targetRadius: use.reach,
      targetHeight: use.height,
    },
    () => go(!open),
  );
  function go(toOpen) {
    if (moving || toOpen === open) {
      return;
    }
    moving = true;
    clearTimeout(closer);
    const way = toOpen ? poses.slice(1) : poses.slice(0, -1).reverse();
    way.forEach((pose, index) => {
      setTimeout(() => made.glideTo(point(pose.at), turnOf(pose.rotation), { ms: stepMs }), index * stepMs);
    });
    setTimeout(() => {
      moving = false;
      open = toOpen;
      hint.setText(open ? 'Close' : 'Open');
      if (open && closeAfter > 0) {
        closer = setTimeout(() => go(false), closeAfter * 1000);
      }
    }, way.length * stepMs);
  }
}
for (const edit of WORK.levelEdits) {
  attempt('edits', () =>
    LevelEdit.apply({
      ...edit,
      at: point(edit.at),
      ...(edit.position ? { position: point(edit.position) } : {}),
    }),
  );
}
for (const area of WORK.areas) {
  attempt('areas', () =>
    Area.create({
      ...area,
      position: point(area.position),
      min: point(area.min),
      max: point(area.max),
    }),
  );
}
for (const effect of WORK.effects) {
  attempt('effects', () =>
    Vfx.spawn(effect.name, point(effect.at), {
      rotation: { x: 0, y: 0, z: effect.turn },
      scale: effect.scale,
      ...(effect.pulse > 0 ? { pulsePeriod: effect.pulse } : {}),
    }),
  );
}
for (const light of WORK.lights) {
  attempt('lights', () => Light.spawn(point(light.at), light.light));
}
// The People the author sent walking in the editor: the same walk on the server, on the level's
// navmesh, simulated by the nearest player's game.
for (const person of WORK.people ?? []) {
  attempt('people', () => {
    const npc = Npc.create({
      position: point(person.at),
      class: person.class,
      name: person.name,
      rotation: { x: 0, y: 0, z: person.turn },
      nameplate: true,
    });
    // Back and forth along the walk, so players who come later see it too (one walk at the
    // server's start was over before anyone looked).
    if (person.walkTo) {
      npc.patrol([point(person.at), point(person.walkTo)], {
        mode: 'pingPong',
        speed: 'walk',
        waitSeconds: 3,
      });
    }
  });
}
console.info(\`architect_\${WORK.map}: \${JSON.stringify(made)}\`);

// Where players appear: the work's spawn areas in turn (on a custom map, without one, the ground
// where the author stood when exporting). Without any the server's own spawn stands.
const spawns = WORK.spawns ?? [];
let nextSpawn = 0;
if (spawns.length > 0) {
  kcds.core.Events.on('playerSpawning', (player) => {
    const spawn = spawns[nextSpawn % spawns.length];
    nextSpawn += 1;
    player.spawn(point(spawn.at), { x: 0, y: 0, z: spawn.turn ?? 0 });
  });
}
`;
}

function clientScript() {
  return `'use strict';
// World Architect export: what each player's game makes for itself, the same for all.
const { Terrain, Vegetation, Collision, Audio } = kcds.world;
const { Vector3 } = kcds.math;
const WORK = require('./work.json');

const point = (at) => new Vector3(at.x, at.y, at.z);
// A handler may run a few seconds only: the work is made in slices, the game going on between.
const SLICE = 40;
const PLANTS_AT_ONCE = 256;
let built = false;

function steps() {
  const list = [];
  for (const edit of WORK.terrain) {
    list.push(() => {
      if (edit.mode === 'pad') {
        Terrain.flatten(point(edit.at), edit.size, edit.at.z, edit.fade);
      } else if (edit.mode.startsWith('paint:')) {
        Terrain.paint(point(edit.at), edit.size / 2, edit.mode.slice(6), edit.size / 2);
      } else if (edit.mode === 'flatten') {
        Terrain.flatten(point(edit.at), edit.size, edit.at.z, edit.fade ?? edit.size);
      } else {
        const amount = edit.mode === 'raise' ? edit.strength : -edit.strength;
        Terrain.raise(point(edit.at), edit.size, amount, edit.size);
      }
    });
  }
  // The plants many to a question to the game (one by one, thousands took minutes).
  for (let first = 0; first < WORK.plants.length; first += PLANTS_AT_ONCE) {
    const part = WORK.plants.slice(first, first + PLANTS_AT_ONCE);
    list.push(() => {
      const planted = Vegetation.plantMany(
        part.map((plant) => ({
          kind: plant.name,
          position: point(plant.at),
          turn: plant.turn,
          scale: plant.scale ?? 1,
        })),
      );
      return planted.filter((handle) => !handle).length;
    });
  }
  for (const shape of WORK.shapes) {
    list.push(() =>
      Collision.addBox({
        center: point(shape.center),
        size: new Vector3(shape.size.x, shape.size.y, shape.size.z),
        rotation: { x: shape.pitch ?? 0, y: 0, z: shape.turn },
      }),
    );
  }
  for (const sound of WORK.sounds ?? []) {
    list.push(() => {
      if (Audio.loopAt(sound.name, point(sound.at)) === 0) {
        throw new Error(\`no sound \${sound.name}\`);
      }
    });
  }
  return list;
}

function build() {
  if (built) {
    return;
  }
  built = true;
  const list = steps();
  let next = 0;
  let failed = 0;
  const slice = () => {
    const end = Math.min(list.length, next + SLICE);
    for (; next < end; next += 1) {
      try {
        // A step that makes many says how many of them failed.
        failed += list[next]() || 0;
      } catch {
        failed += 1;
      }
    }
    if (next < list.length) {
      setTimeout(slice, 0);
      return;
    }
    console.info(\`architect_\${WORK.map}: the ground, plants, collision and sounds made (\${failed} failed)\`);
  };
  slice();
}

kcds.core.Events.on('playerReady', build);
`;
}

function readme(name, map) {
  return [
    `${name}: World Architect work on ${map}, exported for a KCDS server.`,
    '',
    `1. Copy this folder into the server's resources folder (resources/${name}).`,
    `2. Run the server on the map ${map} and start the resource (it starts with the server when it is`,
    '   listed in the server config, or with the console command: start ' + name + ').',
    '3. Every player who joins sees the buildings, props, hidden and moved level objects, zones,',
    '   effects and lights (from the server) and the shaped ground, plants and collision (each',
    "   player's game makes them when the player is ready), and the sounds you placed.",
    '',
    'On a custom map (kcds_map_...) the map is in maps/ here: the server sends it to every player',
    'who joins, before their world loads.',
    '',
    'Your own models (Documents\\KCDS\\World Architect\\assets) go into this resource as',
    `stream/<top folder>/kcds/${name}/... for every player to receive them.`,
    '',
  ].join('\r\n');
}

module.exports = { serverScript, clientScript, readme };
