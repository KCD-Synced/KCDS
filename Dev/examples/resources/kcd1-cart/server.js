'use strict';
// A cart in a corner of the map takes players to Rataje, the Kingdom Come: Deliverance map. Aim at
// it and press E ("Travel to Rataje"): Player.setMap sends the player there, and their game loads
// the map from their own copy of Kingdom Come: Deliverance. A blip marks the cart on the map and the
// compass of the players on its map.
//
// The server must run the map: in server.json, "maps": ["kcd1_rataje"] next to its first map. Only
// players who own Kingdom Come: Deliverance on Steam can go; the others are told so.
//
// The cart stands where CORNERS says on each map, on the ground the first player on that map finds
// (World.snapToGround). Change a corner to put it somewhere else.

const { World, Prop, Blip } = kcds.world;
const { Events, Chat } = kcds.core;
const { Player } = kcds.players;
const { Vector3 } = kcds.math;

const KCD1 = 'kcd1_rataje';
const CART_MODEL = 'objects/manmade/vehicles/carts/cart_a.cgf';
// Where the cart stands on each map, by x and y (its height comes from the ground), and which way it
// faces, in degrees around z.
const CORNERS = {
  kutnohorsko: { x: 345, y: 560, heading: 45 },
  trosecko: { x: 760, y: 1675, heading: 45 },
};
// How often a map without its cart yet asks again for the ground under it.
const PLACE_EVERY_MS = 5000;

// map -> { prop, blip, interaction }, once the cart stands there.
const carts = new Map();
// The maps whose ground is being asked for now.
const asking = new Set();

function playersOn(map) {
  return Player.all().filter((player) => player.map === map);
}

function travel(player) {
  if (player.map === KCD1) {
    return;
  }
  if (!player.setMap(KCD1)) {
    Chat.sendToPlayer(player, 'The cart goes to Rataje only for players who own Kingdom Come: Deliverance on Steam.', {
      author: 'Carter',
    });
    return;
  }
  Chat.sendToPlayer(player, 'The cart takes you to Rataje.', { author: 'Carter' });
}

async function placeCart(map) {
  const corner = CORNERS[map];
  if (!corner || carts.has(map) || asking.has(map) || playersOn(map).length === 0) {
    return;
  }
  asking.add(map);
  try {
    const ground = await World.snapToGround(null, { x: corner.x, y: corner.y }, { map });
    if (!ground.answered || ground.z === null || carts.has(map)) {
      return;
    }
    const position = new Vector3(corner.x, corner.y, ground.z);
    const prop = Prop.spawn(CART_MODEL, position, new Vector3(0, 0, corner.heading), 1, 'static', undefined, map);
    const interaction = World.addInteraction({ entity: prop, text: 'Travel to Rataje', radius: 4 }, travel);
    const blip = Blip.create({ position, type: 'FastTravel', label: 'Cart to Rataje' });
    blip.showOnlyTo(playersOn(map));
    carts.set(map, { prop, blip, interaction });
    console.info(`kcd1-cart: the cart stands on ${map} at ${position.x}, ${position.y}, ${position.z.toFixed(1)}`);
  } finally {
    asking.delete(map);
  }
}

// The blip of each map's cart shows to the players on that map only.
function showBlips() {
  for (const [map, cart] of carts) {
    cart.blip.showOnlyTo(playersOn(map));
  }
}

function placeCarts() {
  for (const map of World.maps) {
    placeCart(map).catch((error) => console.warn(`kcd1-cart: ${map}: ${error.message}`));
  }
}

function playerArrived() {
  placeCarts();
  showBlips();
}

if (!World.maps.includes(KCD1)) {
  console.warn(`kcd1-cart: the server does not run ${KCD1}; add it to "maps" in server.json`);
} else {
  // Again while a map has no cart: its first ground answer can fail (the player still loading).
  setInterval(placeCarts, PLACE_EVERY_MS);
  Events.on('playerReady', playerArrived);
  Events.on('playerMapChanged', playerArrived);
  // After the player has left, so they are no longer counted.
  Events.on('playerDisconnect', () => setTimeout(showBlips, 0));
}
