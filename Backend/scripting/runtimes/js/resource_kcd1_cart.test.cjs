'use strict';

// The kcd1-cart example resource (Dev/examples/resources/kcd1-cart, not shipped with the server):
// the cart stands on the ground of its map's corner with an E hint and a blip, and its use sends a
// player who owns Kingdom Come: Deliverance to Rataje and tells one who does not.

const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const RESOURCE = path.join(__dirname, '../../../../Dev/examples/resources/kcd1-cart');
const KCD1_APP = 379430;

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function until(check, what, milliseconds = 6000) {
  const end = Date.now() + milliseconds;
  while (!check()) {
    if (Date.now() > end) {
      assert.fail(`timed out waiting for ${what}`);
    }
    await delay(25);
  }
}

test('kcd1-cart resource: a cart on the corner ground, its blip, and a trip for KCD1 owners', async (t) => {
  const f = serverFixture(t);
  f.host.setMaps(['kutnohorsko', 'kcd1_rataje']);
  fs.cpSync(RESOURCE, path.join(f.root, 'kcd1-cart'), { recursive: true });
  await f.host.startAll();
  f.host.playerIdentities.set(8, { apps: [KCD1_APP] });
  const stranger = f.report(7);
  const owner = f.report(8);
  // The ground under the corner, from a player on the map (asked again every 5 seconds until a
  // map has its cart).
  await until(
    () => f.sent.some((message) => message.op === 'worldRaycast'),
    'the ground question',
    12000,
  );
  const asked = f.sent.find((message) => message.op === 'worldRaycast');
  assert.equal(asked.start.x, 345);
  assert.equal(asked.start.y, 560);
  f.host.worldTraces.reply(f.host.players.get(asked.id), {
    request: asked.request,
    status: 'hit',
    hits: [
      {
        position: { x: 345, y: 560, z: 101.5 },
        normal: { x: 0, y: 0, z: 1 },
        distance: 10,
        terrain: true,
        entity: 0,
        surface: 'mat_grass',
      },
    ],
  });
  await until(() => f.host.api.Prop.all().length === 1, 'the cart');
  const [cart] = f.host.api.Prop.all();
  assert.equal(cart.position.z, 101.5);
  const [blip] = f.host.api.Blip.all();
  assert.equal(blip.label, 'Cart to Rataje');
  const hints = f.sent.filter((message) => message.op === 'interactions').at(-1).list;
  assert.equal(hints.length, 1);
  assert.equal(hints[0].text, 'Travel to Rataje');
  // Without Kingdom Come: Deliverance the cart only tells them; its owner goes to Rataje.
  f.host.worldInteractions.fired(stranger, hints[0].id);
  assert.equal(stranger.map, 'kutnohorsko');
  f.host.worldInteractions.fired(owner, hints[0].id);
  assert.equal(owner.map, 'kcd1_rataje');
  assert.ok(f.sent.some((message) => message.op === 'playerMap' && message.id === 8));
});
