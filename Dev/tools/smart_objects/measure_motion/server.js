// Measures how far the game's own sit-down, lie-down and stand-up fragments move a body, for
// Dev/tools/smart_objects/build_use_motion.py. Run a test server with Dev/tools/smart_objects as its
// resources folder (this script reads the helpers from the checkout) and one player in the open,
// then type `measuremotion` in the server console: for every down/up pair of the use helpers an
// NPC stands 4 m north of the player facing north, plays the down fragment, then the up fragment,
// and one MEASURE line logs where it was before, after the down and after the up.
const { Events } = kcds.core;
const { Player } = kcds.players;
const { Npc } = kcds.characters;
const helpers = require('../../../../Backend/scripting/runtimes/js/smart_objects/use_helpers.json');

const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// [x, y, z, yaw in radians] of an NPC as the server knows it.
function pose(npc) {
  const { x, y, z } = npc.position;
  const rotation = npc.rotation;
  return [x, y, z, 2 * Math.atan2(rotation.z, rotation.w)];
}

// Resolves with the end reason of the NPC's next animation.
function animationEnd(npc) {
  return new Promise((resolve) => {
    const handler = (who, fragment, reason) => {
      if (who === npc) {
        Events.off('npcAnimationEnd', handler);
        resolve(reason);
      }
    };
    Events.on('npcAnimationEnd', handler);
  });
}

async function measure(at, approach) {
  const npc = Npc.create({ position: at });
  await wait(3000);
  npc.position = at;
  npc.rotation = { x: 0, y: 0, z: 0, w: 1 };
  await wait(1500);
  const start = pose(npc);
  const down = animationEnd(npc);
  npc.playAnimation(approach.down[0], { tags: approach.down[1] });
  const downReason = await down;
  await wait(800);
  const seated = pose(npc);
  const up = animationEnd(npc);
  npc.playAnimation(approach.up[0], { tags: approach.up[1] });
  const upReason = await up;
  await wait(800);
  const stood = pose(npc);
  console.info(
    'MEASURE ' +
      JSON.stringify({
        down: approach.down,
        up: approach.up,
        downReason,
        upReason,
        start,
        seated,
        stood,
      }),
  );
  npc.remove();
  await wait(500);
}

Events.on('consoleCommand', async (command) => {
  if (command !== 'measuremotion') {
    return;
  }
  const [player] = Player.all();
  if (!player) {
    console.info('MEASURE needs a player in the world');
    return;
  }
  // Each down/up pair once.
  const pairs = new Map();
  for (const helper of Object.values(helpers)) {
    for (const approach of helper.approaches) {
      pairs.set(`${approach.down.join('|')}#${approach.up.join('|')}`, approach);
    }
  }
  const p = player.position;
  const at = { x: p.x, y: p.y + 4, z: p.z };
  for (const approach of pairs.values()) {
    await measure(at, approach);
  }
  console.info('MEASURE done');
});
