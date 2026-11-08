'use strict';

// Hud.rect: where one of the game's HUD parts is on the screen, in pixels, read from the game's
// own HUD movie (the UI element "hud") so a resource can put its own HUD beside it. The movie's
// _root is in window pixels: the game places its corner and centre clips (bc, the bottom centre)
// and scales them for the window. Each part is a clip, by the chain of clips down to it, and the
// box its art takes in that clip's own units (measured on the game's art); the movie's positions
// and scales carry the box to the screen, so it follows the resolution and the HUD's scale.

const ELEMENT = 'hud';
const PARTS = Object.freeze({
  // The health and stamina frame at the bottom (hud.gfx mc_StatsEle with its bars).
  vitals: {
    chain: ['_root.bc', '_root.bc.mc_StatsContainer', '_root.bc.mc_StatsContainer.mc_StatsEle'],
    box: { x: -190, y: -30, width: 380, height: 54 },
  },
});

function finite(value) {
  return typeof value === 'number' && Number.isFinite(value);
}

// { x, y, width, height } in screen pixels, or null when the HUD part is not shown or the game
// does not answer. `ask` is a synchronous request to the game client.
function hudRect(part, ask) {
  const known = Object.hasOwn(PARTS, part) ? PARTS[part] : null;
  if (!known) {
    throw new TypeError(`Hud.rect part is one of ${Object.keys(PARTS).join(', ')}`);
  }
  let x = 0;
  let y = 0;
  let scaleX = 1;
  let scaleY = 1;
  for (const clip of known.chain) {
    const info = ask({ op: 'elementGetClip', name: ELEMENT, instance: 0, clip });
    if (
      !info ||
      info.visible !== true ||
      ![info.x, info.y, info.scaleX, info.scaleY].every(finite)
    ) {
      return null;
    }
    x += scaleX * info.x;
    y += scaleY * info.y;
    scaleX *= info.scaleX;
    scaleY *= info.scaleY;
  }
  const { box } = known;
  return {
    x: Math.round(x + box.x * scaleX),
    y: Math.round(y + box.y * scaleY),
    width: Math.round(box.width * scaleX),
    height: Math.round(box.height * scaleY),
  };
}

module.exports = { hudRect, HUD_PARTS: Object.keys(PARTS) };
