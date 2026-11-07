'use strict';

// Carts and wagons the server puts in the world: the server API's Cart. A cart is
// built from one of the game's cart prefabs (a blueprint) with its horses already in the shafts,
// spawned as a replicated entity every client in view builds. The server holds the blueprint,
// pose, dimension, who sits in which seat and the driver's top speed; the `driver` seat's
// client runs the cart and reports how it goes.
//
// Server -> client ops (sent only when something changes, no timers; `id` is the cart):
//   { op: 'cartSpawn', id, blueprint, position, rotation, dimension, maxSpeed }
//   { op: 'cartDestroy', id }                      the cart goes, with everyone in it
//   { op: 'cartSeat', id, player, seat }           `player` (a player id) climbs into `seat`;
//                                                  their own game walks to it
//   { op: 'cartSettled', id, player, seat }        their climb on has ended: held on the seat, and
//                                                  a driver takes the reins
//   { op: 'cartClimbOn', id, player, seat }        their climb's clip starts
//   { op: 'cartScootOff', id, player, seat }       they move along the bench to where their climb
//                                                  down starts
//   { op: 'cartClimbOff', id, player, seat }       their climb down starts: no longer held, and a
//                                                  driver has let go of the reins
//   { op: 'cartReinsTaken', id, player, seat }     the climbing driver's hands take the reins
//   { op: 'cartReinsGone', id, player, seat }      the climbing driver's hands let them go
//   { op: 'cartLeave', id, player, seat }          `player` is down from `seat`
//   { op: 'cartDeny', id, player, seat }           a cartEnter report was refused: the game
//                                                  of `player` must not climb in
//   { op: 'cartTeleport', id, position, rotation } the cart stands at the pose, with its riders
//   { op: 'cartSpeed', id, maxSpeed }              the fastest the driver may take it
//   { op: 'cartLoad', id, load }                   what the cart carries ('' for nothing)
//   { op: 'cartPose', id, position, rotation, pace, driver }
//                                                  the driver's report, for every other game
//                                                  (sent only when the pose or pace changed)
// Client -> server ops (a ready player's game; `id` is the player):
//   { op: 'cartEnter', id, cart, seat }            the player used the cart's prompt
//   { op: 'cartLeave', id, cart }                  the player climbed down on their own
//   { op: 'cartAtStart', id, cart }                the player has walked to where their climb
//                                                  starts
//   { op: 'cartDrive', id, cart, position, rotation, pace }
//                                                  the driver's cart moved or changed pace
// `rotation` is a quaternion { x, y, z, w }.

const { Quaternion, Vector3, rotation, vector } = require('../math.cjs');
const { OWNER, removeOwned } = require('../lifecycle/owned_objects.cjs');
// blueprint -> the loads it can carry (the game's own loaded carts, Dev/tools/carts/build_prefabs.py)
const LOADS = require('./loads.json');

const GLOBAL_WORLD = 0xffffffff;
// A cart farther out than this lies outside any level.
const WORLD_LIMIT = 1e6;
const PACES = new Set(['stand', 'walk', 'trot', 'reverse']);
const SPEED_MAX = 15;
const SPEED_DEFAULT = 7;

// The seats a body has (prefabs.json `body`, by its meshes): the covered
// b body has long side benches for four, the open b body two shelves at the back, an a body none;
// on a c body (its bed covers the axle's plank) and on a cart the driver walks beside the horses.
// A loaded wagon seats no passengers.
const FRONT_SEATS = ['driver', 'bench'];
const BODY_SEATS = {
  coveredB: [
    'driver',
    'bench',
    'rightBack',
    'leftBack',
    'rightFront',
    'leftFront',
    'rightMiddle',
    'leftMiddle',
  ],
  openB: ['driver', 'bench', 'rightBack', 'leftBack'],
  a: FRONT_SEATS,
  c: ['driver'],
  cart: ['driver'],
};
const WALKING_BODIES = new Set(['c', 'cart']);
// The game's carts and wagons (prefabs.json, made by Dev/tools/carts/build_prefabs.py from its
// Prefabs/manmade/vehicles): a wagon has two horses, a cart one. cart_a, one mesh without horses,
// stays for the scripts that spawn it. `horses`: how many it is harnessed for; `driverWalks`: the
// driver leads it on foot; `bodyPart`: the index of its body's entity (kcds_cart_<id>_<index>).
const BLUEPRINTS = new Map([
  ...require('./prefabs.json').map((prefab) => [
    prefab.name,
    {
      horses: prefab.chassis === 'wagon' ? 2 : 1,
      seats: prefab.loaded
        ? FRONT_SEATS.filter((seat) => BODY_SEATS[prefab.body].includes(seat))
        : BODY_SEATS[prefab.body],
      driverWalks: WALKING_BODIES.has(prefab.body),
      bodyPart: prefab.bodyPart,
    },
  ]),
  [
    'cart_a',
    { horses: 0, seats: ['driver', 'rightBack', 'leftBack'], driverWalks: true, bodyPart: 0 },
  ],
]);
// The cart's own interactions (World.addInteraction's hint): on its horses to take or leave the
// reins, on its body to get on or off; how near, in metres.
const HORSE_REACH = 2.5;
const BODY_REACH = 3;
const DEFAULT_BLUEPRINT = 'wagon_b_covered';
// The pose a player takes on their seat, held until they climb down, as the game's own wagons
// seat their people (WagonIdle): on the front plank the driver with the reins and the
// co-driver, on the side benches the passengers facing across. A body the game has no such pose
// for sits without a table.
// Walking is off (looking around stays). The driver's movement keys drive the cart before the lock
// sees them (placed_carts.hpp take_action); without the lock their walking clips fought the seat's pose
// on every other screen, and their legs flickered.
const SITTING = { fragment: 'SittingIdle', tags: 'sittingNoTable+player', lockMovement: true };
// How a player climbs onto a seat and down from it, the game's own clips of each wagon place. The
// clips carry the body's way between the ground and the seat: the game's table gives each climb's
// whole movement, and placed_carts.cpp kClimbs starts the body where its clip then ends on the seat.
// The middle benches use the clip of the seat further along, whose way is long enough.
const CLIMB_ON = new Map([
  ['driver', 'driver'],
  ['bench', 'player+codriver'],
  ['rightBack', 'player+rightBack'],
  ['leftBack', 'leftBack'],
  ['rightFront', 'rightFront'],
  ['leftFront', 'leftFront'],
  ['rightMiddle', 'rightFront'],
  ['leftMiddle', 'leftFront'],
]);
const CLIMB_OFF = new Map([
  ['driver', { fragment: 'WagonOut', tags: 'cart+driver' }],
  ['bench', { fragment: 'WagonOut', tags: 'cart+player+codriver' }],
  ['rightBack', { fragment: 'WagonOut', tags: 'cart+player+rightBack' }],
  ['leftBack', { fragment: 'WagonOut', tags: 'cart+leftBack' }],
  ['rightFront', { fragment: 'WagonOut', tags: 'cart+rightFront' }],
  ['leftFront', { fragment: 'WagonOut', tags: 'cart+leftFront' }],
  ['rightMiddle', { fragment: 'WagonOut', tags: 'cart+rightFront' }],
  ['leftMiddle', { fragment: 'WagonOut', tags: 'cart+leftFront' }],
]);
// When the driver's hands reach the reins in the game's clips (ms from the clip's start; their Attach
// and Detach events at 0.65 of wagon_driver_boarding's 7.27 s and 0.30 of wagon_driver_exit's 7.73 s).
const REINS_TAKEN_AFTER = 4690;
const REINS_GONE_AFTER = 2320;
// The benches' seats whose clips do not end on them: a rider moves along the bench to the clip's
// place before climbing down, in this time (ms; placed_carts.cpp kClipEndY, kScootSeconds).
const SCOOT_SEATS = new Set(['leftBack', 'rightFront', 'leftFront', 'rightMiddle', 'leftMiddle']);
const SCOOT_MS = 1200;
// A player whose game does not say it has walked to the climb's start climbs after this long, and
// a climb that never reports its end still ends after this long (ms).
const WALK_LONGEST = 16000;
const CLIMB_ON_LONGEST = 12000;
const CLIMB_OFF_LONGEST = 10000;
// A driver who walks leads by the reins in their right hand (the game's teamster hand,
// HoldItemRight with cart+r_whip, the upper body only), walking as they like.
const LEADING = { fragment: 'HoldItemRight', tags: 'cart+r_whip', lockMovement: false };
// The seated driver's pose locks no movement: the lock is the game's "no_move" action filter,
// which kept the movement keys from the driver's client, and those keys drive the cart there
// (placed_carts.cpp take_action takes them; their client holds them on the seat).
const SEAT_POSES = new Map([
  ['driver', { fragment: 'WagonIdle', tags: 'cart+driver', lockMovement: false }],
  // Everyone but the driver sits in this pose, mirrored by the seat's side (placed_carts.cpp): the game's
  // co-driver idle lifts one leg, and its other passengers' sink into the benches. This one keeps both
  // feet on the footboard.
  ['bench', { fragment: 'WagonIdle', tags: 'cart+player+rightBack', lockMovement: true }],
  ['leftBack', { fragment: 'WagonIdle', tags: 'cart+player+rightBack', lockMovement: true }],
  ['rightBack', { fragment: 'WagonIdle', tags: 'cart+player+rightBack', lockMovement: true }],
  ['leftFront', { fragment: 'WagonIdle', tags: 'cart+player+rightBack', lockMovement: true }],
  ['rightFront', { fragment: 'WagonIdle', tags: 'cart+player+rightBack', lockMovement: true }],
  ['rightMiddle', { fragment: 'WagonIdle', tags: 'cart+player+rightBack', lockMovement: true }],
  ['leftMiddle', { fragment: 'WagonIdle', tags: 'cart+player+rightBack', lockMovement: true }],
]);

function validWorld(world) {
  return Number.isSafeInteger(world) && world >= 0 && world <= GLOBAL_WORLD;
}

const finitePoint = (v) =>
  [v.x, v.y, v.z].every((n) => Number.isFinite(n) && Math.abs(n) <= WORLD_LIMIT);
const finiteTurn = (q) => [q.x, q.y, q.z, q.w].every(Number.isFinite);
const turnOf = (q) => ({ x: q.x, y: q.y, z: q.z, w: q.w });

// A position and a rotation (quaternion or Euler degrees) as plain { at, facing }, or null when
// either is malformed or out of the world.
function poseOf(position, orientation) {
  try {
    const at = vector(position);
    const facing = turnOf(rotation(orientation));
    if (!finitePoint(at) || !finiteTurn(facing)) {
      return null;
    }
    return { at: { x: at.x, y: at.y, z: at.z }, facing };
  } catch {
    return null;
  }
}

class Carts {
  // `host`: the resource host (bridge, emit, allocateEntityId, the running resource, players).
  constructor(host) {
    this.host = host;
    // id -> Cart
    this.carts = new Map();
    // player id -> the Cart they sit in
    this.riding = new Map();
    // player id -> the sitting request a passenger seat started (SITTING), while it is theirs
    this.sitting = new Map();
  }

  // The Cart class of the server API, an Entity.
  define(Entity) {
    const registry = this;
    class Cart {
      constructor(id) {
        const cart = registry.carts.get(id);
        if (!cart) {
          throw new Error('Unknown cart');
        }
        return cart;
      }
      static spawn(blueprint, position, orientation, dimension) {
        return registry.spawn(Cart, { blueprint, position, orientation, dimension });
      }
      static all(dimension) {
        if (dimension !== undefined && !validWorld(dimension)) {
          throw new TypeError('Invalid dimension');
        }
        return [...registry.carts.values()].filter(
          (cart) => dimension === undefined || cart.dimension === dimension,
        );
      }
      static getById(id) {
        return registry.carts.get(id) ?? null;
      }
      static destroyAll(dimension) {
        return registry.destroyAll(dimension);
      }
      static blueprints() {
        return [...BLUEPRINTS.keys()];
      }
      get horseCount() {
        return BLUEPRINTS.get(this.blueprint).horses;
      }
      get seats() {
        return [...BLUEPRINTS.get(this.blueprint).seats];
      }
      // The driver walks beside the horses (a c wagon, a cart) rather than sitting on the front.
      get driverWalks() {
        return BLUEPRINTS.get(this.blueprint).driverWalks;
      }
      get driver() {
        return this.whoSits('driver');
      }
      get pace() {
        return this.currentPace;
      }
      get position() {
        return new Vector3(this.at.x, this.at.y, this.at.z);
      }
      get rotation() {
        return new Quaternion(this.facing.w, this.facing.x, this.facing.y, this.facing.z);
      }
      get maxSpeed() {
        return this.topSpeed;
      }
      set maxSpeed(value) {
        registry.setSpeed(this, value);
      }
      // What the cart carries, one of `loads` ('' for nothing).
      get load() {
        return this.currentLoad;
      }
      set load(value) {
        registry.setLoad(this, value);
      }
      get loads() {
        return [...(LOADS[this.blueprint] ?? [])];
      }
      // The cart's own stock (a virtual Stash), or null; addStorage() gives it one.
      get storage() {
        return this.stock ?? null;
      }
      addStorage() {
        return registry.addStorage(this);
      }
      removeStorage() {
        registry.removeStorage(this);
      }
      seatPlayer(player, seat) {
        return registry.put(this, player, seat, true);
      }
      unseatPlayer(player) {
        return registry.remove(this, player, true);
      }
      whoSits(seat) {
        return this.sitters.get(seat) ?? null;
      }
      seatOf(player) {
        for (const [seat, sitter] of this.sitters) {
          if (sitter === player) return seat;
        }
        return null;
      }
      teleport(position, orientation) {
        return registry.teleport(this, position, orientation);
      }
      destroy() {
        registry.destroy(this);
      }
      toString() {
        const driver = this.driver ? this.driver.id : 'none';
        return `Cart(${this.id}, ${this.blueprint}, driver ${driver}, ${this.pace})`;
      }
    }
    Object.setPrototypeOf(Cart.prototype, Entity.prototype);
    return Cart;
  }

  spawn(Cart, { blueprint = DEFAULT_BLUEPRINT, position = {}, orientation = {}, dimension }) {
    const owner = this.host.context.getStore();
    if (
      owner &&
      (this.host.resources.get(owner.name) !== owner || !owner.active || owner.failing)
    ) {
      throw new Error('Resource is stopped');
    }
    if (!BLUEPRINTS.has(blueprint)) {
      throw new RangeError(`the cart catalog does not carry ${blueprint}`);
    }
    const world = dimension ?? GLOBAL_WORLD;
    if (!validWorld(world)) {
      throw new TypeError('Invalid dimension');
    }
    const at = vector(position);
    if (!finitePoint(at)) {
      throw new RangeError('Invalid cart position');
    }
    const cart = Object.create(Cart.prototype);
    Object.defineProperty(cart, 'id', { value: this.host.allocateEntityId(), enumerable: true });
    Object.defineProperty(cart, 'blueprint', { value: blueprint, enumerable: true });
    Object.defineProperty(cart, 'dimension', { value: world, enumerable: true });
    // The changeable state, kept by the registry.
    cart.at = { x: at.x, y: at.y, z: at.z };
    cart.facing = turnOf(rotation(orientation));
    cart.currentPace = 'stand';
    cart.topSpeed = SPEED_DEFAULT;
    cart.currentLoad = '';
    cart.stock = null;
    // seat name -> Player
    cart.sitters = new Map();
    cart[OWNER] = owner?.name ?? null;
    this.carts.set(cart.id, cart);
    this.host.bridge.send(this.spawnMessage(cart));
    cart.interactions = this.addInteractions(cart);
    this.updateInteractions(cart);
    this.host.emit('cartSpawn', cart);
    return cart;
  }

  // The hints a player near the cart gets, pressed with the game's use key: on each horse to take
  // the reins (Drive, or Lead when the driver walks) or let them go, on the body to get on the
  // first free passenger seat or off again. The game's own entities of the cart carry their
  // names (Client/runtime/carts: kcds_cart_<id>_<part>, kcds_cart_<id>_horse_<n>).
  addInteractions(cart) {
    const blueprint = BLUEPRINTS.get(cart.blueprint);
    const interactions = { take: [], letGo: [], climb: null };
    const add = (entity, radius, text, handler) =>
      this.host.worldInteractions.add({ entity, radius, text }, handler);
    for (let horse = 0; horse < blueprint.horses; horse += 1) {
      const entity = `kcds_cart_${cart.id}_horse_${horse}`;
      const take = add(entity, HORSE_REACH, blueprint.driverWalks ? 'Lead' : 'Drive', (player) =>
        this.put(cart, player, 'driver', false),
      );
      const letGo = add(entity, HORSE_REACH, 'Let go', (player) => {
        if (cart.driver === player) this.climbDown(cart, player);
      });
      interactions.take.push(take);
      interactions.letGo.push(letGo);
    }
    const passengers = blueprint.seats.filter((seat) => seat !== 'driver');
    if (passengers.length > 0) {
      const part = blueprint.bodyPart > 0 ? `_${blueprint.bodyPart}` : '';
      interactions.climb = add(`kcds_cart_${cart.id}${part}`, BODY_REACH, 'Get on', (player) => {
        const free = passengers.find((seat) => !cart.sitters.has(seat));
        if (free && cart.seatOf(player) === null) this.put(cart, player, free, false);
      });
    }
    return interactions;
  }

  // The hints as the seats stand now: the reins are taken only while nobody drives, and let go
  // only by the driver; getting on shows while a passenger seat is free, to those not on board.
  // A seated player gets down with their use key (their game's CART_REPORT Leave).
  updateInteractions(cart) {
    const interactions = cart.interactions;
    if (!interactions) {
      return;
    }
    const driver = cart.driver;
    for (const take of interactions.take) {
      take.setEnabled(!driver);
    }
    for (const letGo of interactions.letGo) {
      letGo.setPlayers(driver ? [driver] : []);
      letGo.setEnabled(Boolean(driver));
    }
    if (interactions.climb) {
      const seats = BLUEPRINTS.get(cart.blueprint).seats;
      const free = seats.some((seat) => seat !== 'driver' && !cart.sitters.has(seat));
      interactions.climb.setEnabled(free);
      const riders = [...cart.sitters.values()].map((player) => player.id);
      this.host.worldInteractions.hideFrom(interactions.climb, riders);
    }
  }

  removeInteractions(cart) {
    const interactions = cart.interactions;
    if (!interactions) {
      return;
    }
    for (const handle of [...interactions.take, ...interactions.letGo, interactions.climb]) {
      handle?.remove();
    }
    cart.interactions = null;
  }

  // The cartSpawn op for a cart as it stands now.
  spawnMessage(cart) {
    return {
      op: 'cartSpawn',
      id: cart.id,
      blueprint: cart.blueprint,
      position: { ...cart.at },
      rotation: { ...cart.facing },
      dimension: cart.dimension,
      maxSpeed: cart.topSpeed,
    };
  }

  // A player's world became ready: their game lost the carts, so it is told of each again with
  // the riders in it. The cartSpawn names the player, so nobody else hears it.
  arrived(player) {
    for (const cart of this.carts.values()) {
      this.host.bridge.send({ ...this.spawnMessage(cart), to: player.id });
      if (cart.currentLoad) {
        this.host.bridge.send({
          op: 'cartLoad',
          id: cart.id,
          load: cart.currentLoad,
          to: player.id,
        });
      }
      for (const [seat, rider] of cart.sitters) {
        this.host.bridge.send({
          op: 'cartSeat',
          id: cart.id,
          player: rider.id,
          seat,
          to: player.id,
        });
        // Nobody on their way up or down is held: their own reports place them.
        if (!cart.climbing?.has(rider.id) && !cart.leaving?.has(rider.id)) {
          this.host.bridge.send({
            op: 'cartSettled',
            id: cart.id,
            player: rider.id,
            seat,
            to: player.id,
          });
        }
      }
    }
  }

  // Seats `player` in `seat`, taking them out of any cart they were in. `script`: a script asked
  // (a refused seat returns false) rather than the player's own game, which gets a cartDeny.
  put(cart, player, seat, script) {
    const deny = () => {
      if (!script) {
        this.host.bridge.send({ op: 'cartDeny', id: cart.id, player: player.id, seat });
      }
      return false;
    };
    if (this.carts.get(cart.id) !== cart || !this.host.connectedPlayer(player)) {
      return false;
    }
    if (!BLUEPRINTS.get(cart.blueprint).seats.includes(seat)) {
      if (script) throw new RangeError(`Cart ${cart.blueprint} has no seat ${seat}`);
      return deny();
    }
    if (cart.sitters.get(seat) === player) {
      return true;
    }
    if (cart.sitters.has(seat) || player.dimension !== cart.dimension) {
      return deny();
    }
    if (!this.host.emit('cartEntering', cart, player, seat)) {
      return deny();
    }
    // A handler may have changed things.
    if (cart.sitters.has(seat) || this.carts.get(cart.id) !== cart) {
      return deny();
    }
    const before = this.riding.get(player.id);
    if (before) this.remove(before, player, true);
    cart.sitters.set(seat, player);
    this.riding.set(player.id, cart);
    this.host.bridge.send({ op: 'cartSeat', id: cart.id, player: player.id, seat });
    // A driver who walks beside the horses holds the reins; one of a cart without horses
    // walks as they like.
    const blueprint = BLUEPRINTS.get(cart.blueprint);
    if (seat !== 'driver' || !blueprint.driverWalks) {
      this.sit(cart, player, seat);
    } else if (blueprint.horses > 0) {
      this.sit(cart, player, seat, LEADING);
    } else {
      this.settled(cart, player, seat);
    }
    this.updateInteractions(cart);
    this.host.emit('cartEnter', cart, player, seat);
    return true;
  }

  // Every game holds the rider on their seat from now on, and a driver takes the reins.
  settled(cart, player, seat) {
    cart.climbing?.delete(player.id);
    this.host.bridge.send({ op: 'cartSettled', id: cart.id, player: player.id, seat });
  }

  // A seated player takes their seat's pose (SEAT_POSES) on every screen, before cartEnter so a
  // handler's own playAnimation replaces it.
  sit(cart, player, seat, given) {
    const animations = this.host.playerAnimations;
    const play = (pose) =>
      player.playAnimation(pose.fragment, {
        tags: pose.tags,
        loop: true,
        lockMovement: pose.lockMovement,
      });
    const pose = given ?? SEAT_POSES.get(seat) ?? SITTING;
    // A walking pose the body does not have is left out; a seat falls back to sitting.
    const fallback = given ? null : SITTING;
    const settle = () => {
      if (!play(pose) && (!fallback || pose === fallback || !play(fallback))) return;
      this.sitting.set(player.id, animations.stateOf(player).current);
    };
    // A seat is climbed onto first; its pose follows when the climb ends (or at once when the
    // body has no such climb). Their game walks them to where the climb starts first (atStart).
    const climb = given ? undefined : CLIMB_ON.get(seat);
    if (!climb) {
      this.settled(cart, player, seat);
      settle();
      return;
    }
    cart.climbing ??= new Set();
    cart.climbing.add(player.id);
    cart.walking ??= new Map();
    const start = () => {
      if (cart.walking.get(player.id) !== start) {
        return;
      }
      cart.walking.delete(player.id);
      if (this.carts.get(cart.id) !== cart || cart.seatOf(player) !== seat) {
        cart.climbing.delete(player.id);
        return;
      }
      this.climbOn(cart, player, seat, climb, settle);
    };
    cart.walking.set(player.id, start);
    setTimeout(start, WALK_LONGEST).unref?.();
  }

  // The player's game says they stand where their climb starts.
  atStart(player, cartId) {
    const cart = this.carts.get(cartId);
    if (player) {
      cart?.walking?.get(player.id)?.();
    }
  }

  // The climb itself; something a handler plays by now stays, and they are on the seat at once.
  climbOn(cart, player, seat, climb, settle) {
    const animations = this.host.playerAnimations;
    const busy = animations.stateOf(player).current;
    this.host.bridge.send({ op: 'cartClimbOn', id: cart.id, player: player.id, seat });
    const climbing =
      !busy && player.playAnimation('WagonIn', { tags: climb, loop: false, lockMovement: true });
    if (!climbing) {
      this.settled(cart, player, seat);
      if (!busy) {
        settle();
      }
      return;
    }
    const request = animations.stateOf(player).current;
    this.sitting.set(player.id, request);
    if (seat === 'driver') {
      const taken = () => {
        if (cart.climbing?.has(player.id) && cart.seatOf(player) === seat) {
          this.host.bridge.send({ op: 'cartReinsTaken', id: cart.id, player: player.id, seat });
        }
      };
      setTimeout(taken, REINS_TAKEN_AFTER).unref?.();
    }
    const up = () => {
      if (!cart.climbing?.has(player.id)) return;
      if (this.carts.get(cart.id) !== cart || cart.seatOf(player) !== seat) {
        cart.climbing.delete(player.id);
        return;
      }
      this.settled(cart, player, seat);
      if (this.sitting.get(player.id) === request) {
        settle();
      }
    };
    request.onEnd = up;
    setTimeout(up, CLIMB_ON_LONGEST).unref?.();
  }

  // The player climbs down from their seat (their use key, Let go), then is let down: every game
  // is told when the climb ends. A seat without a climb lets them down at once.
  climbDown(cart, player) {
    const seat = cart.seatOf(player);
    if (seat === null || cart.leaving?.has(player.id)) {
      return;
    }
    const blueprint = BLUEPRINTS.get(cart.blueprint);
    const off = seat === 'driver' && blueprint.driverWalks ? undefined : CLIMB_OFF.get(seat);
    if (off && SCOOT_SEATS.has(seat)) {
      cart.leaving ??= new Set();
      cart.leaving.add(player.id);
      this.host.bridge.send({ op: 'cartScootOff', id: cart.id, player: player.id, seat });
      const go = () => {
        if (this.carts.get(cart.id) === cart && cart.seatOf(player) === seat) {
          cart.leaving.delete(player.id);
          this.startClimbOff(cart, player, seat, off);
        }
      };
      setTimeout(go, SCOOT_MS).unref?.();
      return;
    }
    this.startClimbOff(cart, player, seat, off);
  }

  // The clip of the way down. The seat lets go before the body starts on it.
  startClimbOff(cart, player, seat, off) {
    if (off) {
      this.host.bridge.send({ op: 'cartClimbOff', id: cart.id, player: player.id, seat });
    }
    const climbing =
      off &&
      player.playAnimation(off.fragment, { tags: off.tags, loop: false, lockMovement: true });
    if (!climbing) {
      this.remove(cart, player, true);
      return;
    }
    cart.climbing?.delete(player.id);
    if (seat === 'driver') {
      const gone = () => {
        if (cart.leaving?.has(player.id)) {
          this.host.bridge.send({ op: 'cartReinsGone', id: cart.id, player: player.id, seat });
        }
      };
      setTimeout(gone, REINS_GONE_AFTER).unref?.();
    }
    cart.leaving ??= new Set();
    cart.leaving.add(player.id);
    const request = this.host.playerAnimations.stateOf(player).current;
    this.sitting.set(player.id, request);
    const down = () => {
      if (!cart.leaving?.delete(player.id)) return;
      if (this.carts.get(cart.id) === cart && cart.seatOf(player) === seat) {
        this.remove(cart, player, true);
      }
    };
    request.onEnd = down;
    setTimeout(down, CLIMB_OFF_LONGEST).unref?.();
  }

  // The passenger gets up from the sitting pose, unless something else plays by now.
  standUp(player) {
    const request = this.sitting.get(player.id);
    this.sitting.delete(player.id);
    if (!request || !this.host.connectedPlayer(player)) return;
    if (this.host.playerAnimations.stateOf(player).current === request) {
      player.stopAnimation();
    }
  }

  // Lets `player` out; `send`: their game still has to climb down (it does not when the player
  // climbed down on their own, left, or the cart is gone).
  remove(cart, player, send) {
    const seat = cart.seatOf(player);
    if (seat === null) {
      return false;
    }
    cart.sitters.delete(seat);
    cart.climbing?.delete(player.id);
    cart.walking?.delete(player.id);
    this.riding.delete(player.id);
    if (seat === 'driver') cart.currentPace = 'stand';
    this.standUp(player);
    if (send) {
      this.host.bridge.send({ op: 'cartLeave', id: cart.id, player: player.id, seat });
    }
    this.updateInteractions(cart);
    this.host.emit('cartExit', cart, player, seat);
    return true;
  }

  teleport(cart, position, orientation) {
    if (this.carts.get(cart.id) !== cart || !position) {
      return false;
    }
    const pose = poseOf(position, orientation ?? cart.facing);
    if (!pose) {
      return false;
    }
    cart.at = pose.at;
    cart.facing = pose.facing;
    cart.currentPace = 'stand';
    this.host.bridge.send({
      op: 'cartTeleport',
      id: cart.id,
      position: { ...cart.at },
      rotation: { ...cart.facing },
    });
    return true;
  }

  // A value outside 0..15 is ignored; nothing is sent when the speed stays.
  setSpeed(cart, value) {
    if (
      this.carts.get(cart.id) !== cart ||
      typeof value !== 'number' ||
      !(value >= 0 && value <= SPEED_MAX) ||
      value === cart.topSpeed
    ) {
      return;
    }
    cart.topSpeed = value;
    this.host.bridge.send({ op: 'cartSpeed', id: cart.id, maxSpeed: value });
  }

  // A load this blueprint cannot carry throws; nothing is sent when the load stays.
  setLoad(cart, value) {
    if (this.carts.get(cart.id) !== cart) {
      return;
    }
    if (value !== '' && !(LOADS[cart.blueprint] ?? []).includes(value)) {
      throw new RangeError(`Cart ${cart.blueprint} cannot carry load ${String(value)}`);
    }
    if (value === cart.currentLoad) {
      return;
    }
    cart.currentLoad = value;
    this.host.bridge.send({ op: 'cartLoad', id: cart.id, load: value });
  }

  // The cart's storage: a container on the cart (stash/stashes.cjs), searched with the game's own
  // prompt at it; made once and kept until removeStorage or the cart's end (its contents go with
  // it).
  addStorage(cart) {
    if (this.carts.get(cart.id) !== cart) {
      return null;
    }
    if (!cart.stock) {
      const stashes = this.host.stashes;
      if (!stashes.Stash) {
        this.host.makeAPI();
      }
      cart.stock = stashes.spawn(stashes.Stash, {
        position: cart.at,
        orientation: {},
        dimension: cart.dimension,
        bodyOf: null,
        cartOf: cart.id,
      });
    }
    return cart.stock;
  }

  removeStorage(cart) {
    const stock = cart.stock;
    if (!stock) {
      return;
    }
    cart.stock = null;
    this.host.stashes.destroy(stock);
  }

  // The carts a stopping resource made go with it.
  resourceStopped(name) {
    removeOwned(this.carts.values(), name, (cart) => this.destroy(cart));
  }

  destroy(cart) {
    if (this.carts.get(cart.id) !== cart) {
      return;
    }
    // Everyone is let out first; their games go with the cart, so no cartLeave is sent.
    for (const player of [...cart.sitters.values()]) {
      this.remove(cart, player, false);
    }
    this.host.emit('cartDestroy', cart);
    this.removeStorage(cart);
    this.removeInteractions(cart);
    this.carts.delete(cart.id);
    this.host.bridge.send({ op: 'cartDestroy', id: cart.id });
  }

  destroyAll(dimension) {
    if (dimension !== undefined && !validWorld(dimension)) {
      throw new TypeError('Invalid dimension');
    }
    let removed = 0;
    for (const cart of [...this.carts.values()]) {
      if (dimension === undefined || cart.dimension === dimension) {
        this.destroy(cart);
        removed += 1;
      }
    }
    return removed;
  }

  // A player's game says they used a cart's prompt.
  enter(player, cartId, seat) {
    const cart = this.carts.get(cartId);
    if (!player || !cart || typeof seat !== 'string') return;
    this.put(cart, player, seat, false);
  }

  // A player's game says they want down (its use key on a seat): every game is told.
  leave(player, cartId) {
    const cart = this.carts.get(cartId);
    if (player && cart) this.climbDown(cart, player);
  }

  // The driver's game reports how the cart goes; only the driver is heard (no event: the pose
  // and pace are read on demand). The other games follow it through cartPose.
  drive(player, cartId, position, orientation, pace) {
    const cart = this.carts.get(cartId);
    if (!player || !cart || cart.driver !== player) return;
    const before = JSON.stringify([cart.at, cart.facing, cart.currentPace]);
    if (PACES.has(pace)) cart.currentPace = pace;
    const pose = position && orientation ? poseOf(position, orientation) : null;
    if (pose) {
      cart.at = pose.at;
      cart.facing = pose.facing;
    }
    if (JSON.stringify([cart.at, cart.facing, cart.currentPace]) === before) return;
    this.host.bridge.send({
      op: 'cartPose',
      id: cart.id,
      position: { ...cart.at },
      rotation: { ...cart.facing },
      pace: cart.currentPace,
      driver: player.id,
    });
  }

  // A player left: they climb down (their game is gone), before playerDisconnect.
  left(player) {
    const cart = this.riding.get(player.id);
    if (cart) this.remove(cart, player, false);
  }
}

module.exports = { Carts };
