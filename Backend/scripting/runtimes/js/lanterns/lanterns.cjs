'use strict';

// A lantern a player carries: Player.setLantern and Player.lantern.
//
// The game's lanterns are carried only by its NPCs: the NPC tools lamp_tool and lamp_toolFancy
// (Libs/Tables/item/item.xml) put a wearable lamp's model in the hand's item slot, and its entity
// script lights it with the archetype Standard.Light_Lantern (Libs/EntityArchetypes/Lights.xml:
// radius 6, a warm orange). A player's lantern is made of the same: that model hung in the hand's
// item slot (Player.attachProp with a hand) and a light like the archetype's on the same joint.
// Both hang on the body, so the lantern goes with the player on foot, on a horse or in a cart,
// and every client sees it, a player who joins later too. It goes when the player leaves, when it
// is set to null, or when the resource that lit it stops (its prop and light are that resource's).

// The game's carried lamps, by the name a script gives.
const MODELS = Object.freeze({
  leather: 'lamp_leather_wearable',
  plain: 'lamp_new_wearable',
  plainB: 'lamp_new_wearable_b',
  fancy: 'lamp_new_wearable_fancy',
});
const DEFAULT_MODEL = 'leather';
const DEFAULT_HAND = 'left';
// The joints of the hands' item slots, where the game's item meshes sit.
const SLOT_JOINTS = Object.freeze({ left: 'LeftHandSlot', right: 'RightHandSlot' });
// Standard.Light_Lantern's colour and radius; the brightness as a lantern's in Light's examples.
const LIGHT = Object.freeze({
  color: Object.freeze({ x: 0.888, y: 0.337, z: 0.125 }),
  intensity: 2,
  radius: 6,
});

function lanternOptions(options) {
  if (options === undefined) {
    return { model: DEFAULT_MODEL, hand: DEFAULT_HAND };
  }
  if (options === null || typeof options !== 'object' || Array.isArray(options)) {
    throw new TypeError('setLantern takes { model, hand }, or null to put it out');
  }
  const model = options.model ?? DEFAULT_MODEL;
  const hand = options.hand ?? DEFAULT_HAND;
  if (typeof model !== 'string' || !Object.hasOwn(MODELS, model)) {
    throw new TypeError(`setLantern model is one of ${Object.keys(MODELS).join(', ')}`);
  }
  if (hand !== 'left' && hand !== 'right') {
    throw new TypeError("setLantern hand is 'left' or 'right'");
  }
  return { model, hand };
}

class Lanterns {
  // `host`: the resource host (its props, lights and the Light class of the API).
  constructor(host) {
    this.host = host;
    // Player -> { model, hand, prop, light }
    this.carried = new WeakMap();
  }

  // The player's lantern as { model, hand }, or null. A lantern whose resource stopped took its
  // prop and light with it: it is forgotten.
  get(player) {
    const lantern = this.carried.get(player);
    if (!lantern) {
      return null;
    }
    const lit =
      this.host.props.props.get(lantern.prop.id) === lantern.prop &&
      this.host.lights.lights.get(lantern.light.id) === lantern.light;
    if (!lit) {
      this.carried.delete(player);
      return null;
    }
    return { model: lantern.model, hand: lantern.hand };
  }

  // Lights the player's lantern ({ model, hand }, both optional), or puts it out for null. A new
  // one replaces the old.
  set(player, options) {
    if (options === null) {
      this.putOut(player);
      return;
    }
    const { model, hand } = lanternOptions(options);
    this.putOut(player);
    const prop = this.host.props.attach(player, { model: MODELS[model], hand });
    let light;
    try {
      if (!this.host.lights.Light) {
        this.host.makeAPI();
      }
      light = this.host.lights.spawn(
        this.host.lights.Light,
        { x: 0, y: 0, z: 0 },
        {
          ...LIGHT,
          entity: player,
          joint: SLOT_JOINTS[hand],
        },
      );
    } catch (error) {
      this.host.props.destroy(prop);
      throw error;
    }
    this.carried.set(player, { model, hand, prop, light });
  }

  putOut(player) {
    const lantern = this.carried.get(player);
    if (!lantern) {
      return;
    }
    this.carried.delete(player);
    this.host.props.destroy(lantern.prop);
    lantern.light.destroy();
  }

  // The player left: its light goes (the prop goes with the player's attached props).
  playerLeft(player) {
    this.putOut(player);
  }
}

module.exports = { Lanterns, MODELS };
