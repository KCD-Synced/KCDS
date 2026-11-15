'use strict';

// The horses in a cart's shafts: Cart.harness, Cart.unharness, Cart.harnessed, Cart.horses and
// Horse.harnessedTo.
//
// A cart spawns with horses of its own in the shafts (each game makes them, nobody's), or with
// none (Cart.spawn's `harnessed: false`). A script harnesses real horses to it: the players'
// Horse entities, as many as the cart is made for. While in the shafts a horse leaves its world
// (it stands in HARNESSED_WORLD, where no player is) and follows the cart, and the cart's horse in
// its place wears its look. Unharnessed, it is back in its own world beside the cart. A cart
// without horses is not driven: its driver seat is refused, its other seats are not.
//
// Server -> client: { op: 'cartHarness', id, looks } where `looks` holds a horse look field per
// horse in the shafts ('' for the game's own look), and is empty for a cart without horses. A cart
// with its own horses sends none, as before harnessing existed.

const { Vector3 } = require('../math.cjs');
const { lookField } = require('../horses/looks.cjs');

// The world a harnessed horse stands in while it is in the shafts: no player's.
const HARNESSED_WORLD = 0xfffffffe;
const GLOBAL_WORLD = 0xffffffff;
// How near the cart a horse must stand to be harnessed (metres).
const HARNESS_REACH = 15;
// Where an unharnessed horse is put, in the cart's frame (x right, y forward): beside the shafts,
// one behind the other.
const UNHARNESSED_SIDE = 2.5;
const UNHARNESSED_AHEAD = 3;
const UNHARNESSED_STEP = 2.5;

// `v` turned by the quaternion `q` ({ x, y, z, w }).
function turned(q, v) {
  const tx = 2 * (q.y * v.z - q.z * v.y);
  const ty = 2 * (q.z * v.x - q.x * v.z);
  const tz = 2 * (q.x * v.y - q.y * v.x);
  return {
    x: v.x + q.w * tx + (q.y * tz - q.z * ty),
    y: v.y + q.w * ty + (q.z * tx - q.x * tz),
    z: v.z + q.w * tz + (q.x * ty - q.y * tx),
  };
}

class CartHarness {
  // `carts`: the cart registry; `blueprintOf(cart)` gives the cart's catalog entry ({ horses }).
  constructor(carts, blueprintOf) {
    this.carts = carts;
    this.host = carts.host;
    this.blueprintOf = blueprintOf;
    // Horse -> { cart, world }: the cart it is in and the world it came from.
    this.harnessed = new Map();
  }

  // A new cart: with its own horses unless `harnessed` is false.
  spawned(cart, harnessed) {
    cart.team = harnessed ? null : [];
    if (!harnessed) {
      this.send(cart);
    }
  }

  // Whether the cart has horses in its shafts (its own or harnessed ones).
  isHarnessed(cart) {
    return cart.team === null || cart.team.length > 0;
  }

  // The Horse entities in its shafts (none for a cart with its own horses or none).
  horsesOf(cart) {
    return cart.team ? [...cart.team] : [];
  }

  cartOf(horse) {
    return this.harnessed.get(horse)?.cart ?? null;
  }

  // Whether the cart can be driven: a cart made without horses (led by hand) always can.
  drivable(cart) {
    return this.blueprintOf(cart).horses === 0 || this.isHarnessed(cart);
  }

  // The horses a harness call names, as an array; throws for anything but Horses.
  horsesGiven(given) {
    const horses = Array.isArray(given) ? given : [given];
    const Horse = this.host.api.Horse;
    for (const horse of horses) {
      if (!(horse instanceof Horse)) {
        throw new TypeError('Cart.harness takes a Horse or an array of Horses');
      }
    }
    return horses;
  }

  // Whether `horse` may go into the cart's shafts now.
  fits(cart, horse) {
    if (this.host.horses.get(horse.id) !== horse || this.harnessed.has(horse)) {
      return false;
    }
    if (horse.alive === false || horse.knockedOut || horse.riderId !== 0) {
      return false;
    }
    const worlds = [horse.dimension, cart.dimension];
    if (worlds[0] !== worlds[1] && !worlds.includes(GLOBAL_WORLD)) {
      return false;
    }
    const at = horse.position;
    const distance = Math.hypot(at.x - cart.at.x, at.y - cart.at.y, at.z - cart.at.z);
    return distance <= HARNESS_REACH;
  }

  // Cart.harness: the horses (as many as the cart is made for, each alive, unridden, free and
  // within HARNESS_REACH) go into the shafts of a cart that has none. cartHarness may refuse.
  harness(cart, given) {
    const horses = this.horsesGiven(given);
    if (this.carts.carts.get(cart.id) !== cart || this.isHarnessed(cart)) {
      return false;
    }
    const needed = this.blueprintOf(cart).horses;
    if (needed === 0 || horses.length !== needed || new Set(horses).size !== horses.length) {
      return false;
    }
    if (!horses.every((horse) => this.fits(cart, horse))) {
      return false;
    }
    if (!this.host.emit('cartHarness', cart, [...horses])) {
      return false;
    }
    // A handler may have changed things.
    if (this.isHarnessed(cart) || !horses.every((horse) => this.fits(cart, horse))) {
      return false;
    }
    for (const horse of horses) {
      this.harnessed.set(horse, { cart, world: horse.dimension });
      horse.setDimension(HARNESSED_WORLD);
    }
    cart.team = [...horses];
    this.send(cart);
    return true;
  }

  // Cart.unharness: the horses come out of the shafts and stand beside the cart in their own
  // world again. False for a cart with nobody's horses or none, or while someone drives it.
  unharness(cart) {
    if (!cart.team || cart.team.length === 0 || cart.sitters.has('driver')) {
      return false;
    }
    this.release(cart);
    return true;
  }

  // Takes every harnessed horse out of the cart, its driver or not, and tells the games: the
  // horses go beside the cart, where it stands now.
  release(cart) {
    if (cart.team === null) {
      return;
    }
    const horses = cart.team;
    horses.forEach((horse, index) => {
      const entry = this.harnessed.get(horse);
      this.harnessed.delete(horse);
      if (this.host.horses.get(horse.id) !== horse) {
        return;
      }
      const beside = turned(cart.facing, {
        x: UNHARNESSED_SIDE,
        y: UNHARNESSED_AHEAD - index * UNHARNESSED_STEP,
        z: 0,
      });
      horse.position = new Vector3(
        cart.at.x + beside.x,
        cart.at.y + beside.y,
        cart.at.z + beside.z,
      );
      horse.setDimension(entry?.world ?? cart.dimension);
    });
    cart.team = [];
    if (this.carts.carts.get(cart.id) === cart) {
      this.send(cart);
    }
    if (horses.length > 0) {
      this.host.emit('cartUnharness', cart, [...horses]);
    }
  }

  // A harnessed horse is removed: its cart is left without horses, the others come out.
  horseGone(horse) {
    const entry = this.harnessed.get(horse);
    if (!entry) {
      return;
    }
    this.harnessed.delete(horse);
    const cart = entry.cart;
    cart.team = cart.team.filter((other) => other !== horse);
    // Nobody drives a cart without its horses: the driver climbs down.
    const driver = cart.sitters.get('driver');
    if (driver && this.blueprintOf(cart).horses > 0) {
      this.carts.climbDown(cart, driver);
    }
    this.release(cart);
  }

  // The cartHarness op for the cart as it stands, to one player or to everyone.
  message(cart) {
    return {
      op: 'cartHarness',
      id: cart.id,
      looks: (cart.team ?? []).map((horse) => lookField(horse.look)),
    };
  }

  send(cart, to) {
    if (cart.team === null) {
      return;
    }
    const message = this.message(cart);
    this.host.bridge.send(to === undefined ? message : { ...message, to });
  }
}

module.exports = { CartHarness, HARNESSED_WORLD, HARNESS_REACH };
