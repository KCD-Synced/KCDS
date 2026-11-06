'use strict';

// Dogs: the server API's Dog. A dog is a synced NPC of the actor
// class "Dog" (npcs/npcs.cjs) that belongs to a player. This host keeps what makes it a companion
// (its owner, its mode and whether it goes with its owner) and the NPC holds everything else: the
// body, the name, the soul and the position. Dog.spawn makes the NPC, so a "Dog" NPC the animal
// population or a resource made with Npc.create is no Dog here.
//
// The owner is this host's alone: the native server knows the NPC and not who it follows. A dog
// whose owner leaves is removed, or, with removedWithOwner false, left masterless: it holds where
// it stands until giveTo hands it to someone.
//
// The mode (0 Wait, 1 Follow, 2 Free, 3 Aggressive, 4 Search, 5 Hunt, 6 Guard, 7 Ambush) goes to
// the native server as the NPC's order: Wait holds, Follow trails the owner, Search and Hunt are
// run here every second: Search roams to places around the owner; Hunt chases the
// nearest living animal within HUNT_RANGE at a run, keeps after it until it is HUNT_LOSE_RANGE
// away, and bites it within BITE_RANGE (BITE_DAMAGE a second, the dog the attacker: the game's
// own NPC attack does not set an animal on another), and follows the owner while there is none.
// The others
// are sent as a `dogMode` order ("<sequence>|dogMode|<mode>|<ownerId>") for the simulating client
// to carry out; the dog holds meanwhile. A dog starts in Follow, a KCDS choice.
//
// `objective` is the server's own word for what the dog does: 'waiting', 'following', 'searching',
// 'hunting', or 'idle' for a mode the game carries out.
//
// Player.dog reads an index of the dogs by owner, kept on spawn, giveTo and despawn, so nothing
// scans all dogs.
//
// Events: dogSpawn, dogDestroy (raised while the NPC is despawning, so the handle still resolves),
// dogOwnerChanged and dogModeChanged. The NPC's own events
// (npcDamage, npcDeath and the rest) are still raised with the NPC.

const { ANIMAL_CLASSES, GLOBAL_WORLD, booleanOf, soulOf } = require('../npcs/npc_values.cjs');

const MODE_COUNT = 8;
const MODE_WAIT = 0;
const MODE_FOLLOW = 1;
const MODE_SEARCH = 4;
const MODE_HUNT = 5;
const DEFAULT_MODE = MODE_FOLLOW;

// Search: a new place this far from the owner at most, every SEARCH_EVERY_MS, at a trot.
const SEARCH_RADIUS = 12;
const SEARCH_EVERY_MS = 4000;
// Hunt: the game it goes for is a living animal this close to the dog; once chosen, it chases it
// until it is HUNT_LOSE_RANGE away, and bites it within BITE_RANGE.
const HUNT_RANGE = 40;
const HUNT_LOSE_RANGE = 80;
const BITE_RANGE = 2.5;
const BITE_DAMAGE = 12;
// The dogs' behaviour runs this often.
const BEHAVIOUR_TICK_MS = 1000;

// How a dog trails its owner in Follow: a jog, three metres behind.
const FOLLOW_OPTIONS = Object.freeze({ speed: 'jog', radius: 3 });

const dogState = Symbol('kcds.dogState');

// How far apart two bodies are, in metres.
function distanceOf(one, other) {
  const a = one.position;
  const b = other.position;
  return Math.hypot(a.x - b.x, a.y - b.y, a.z - b.z);
}

function modeOf(value) {
  if (!Number.isInteger(value) || value < 0 || value >= MODE_COUNT) {
    throw new RangeError(`Dog mode must be an integer from 0 to ${MODE_COUNT - 1}`);
  }
  return value;
}

class Dogs {
  // `host`: the resource host; the dogs ride on host.npcs.
  constructor(host) {
    this.host = host;
    // id -> Dog
    this.dogs = new Map();
    // owner id -> the dogs that player owns, oldest first (a masterless dog is under no owner),
    // so Player.dog and a disconnect never scan all dogs
    this.byOwner = new Map();
  }

  // The dog this player owns: the oldest, as the game gives a player one companion. Null when
  // they have none.
  dogOf(player) {
    const dogs = this.byOwner.get(player.id);
    return dogs ? [...dogs][0] : null;
  }

  // Keeps the owner index: the dog is now `ownerId`'s, and was `previousId`'s.
  reindex(dog, previousId, ownerId) {
    const previous = this.byOwner.get(previousId);
    if (previous) {
      previous.delete(dog);
      if (!previous.size) {
        this.byOwner.delete(previousId);
      }
    }
    if (ownerId) {
      const dogs = this.byOwner.get(ownerId) ?? new Set();
      dogs.add(dog);
      this.byOwner.set(ownerId, dogs);
    }
  }

  // The Dog class of the server API, an Entity.
  define(Entity) {
    const registry = this;
    class Dog {
      // A wrapper for an existing dog; Dog.spawn makes one.
      constructor(id) {
        const dog = registry.dogs.get(id);
        if (!dog) {
          throw new Error('Unknown dog');
        }
        return dog;
      }
      // Spawns a dog for `owner` (a connected Player) at `position` (components left out keep the
      // owner's), facing `rotation` (Euler degrees or a Quaternion), in the owner's
      // dimension. `soul` is a soul GUID and `name` the name every client shows. A new dog is
      // not lootable and follows its owner (mode 1).
      static spawn(owner, position, rotation, soul, name) {
        return registry.spawn(Dog, owner, position, rotation, soul, name);
      }
      static all() {
        return [...registry.dogs.values()];
      }
      static getById(id) {
        return registry.dogs.get(id) ?? null;
      }
      get soul() {
        return this[dogState].npc.soul;
      }
      get name() {
        return this[dogState].npc.name;
      }
      set name(value) {
        registry.requireLive(this);
        this[dogState].npc.name = value;
      }
      get ownerId() {
        return this[dogState].ownerId;
      }
      get owner() {
        return registry.host.players.get(this[dogState].ownerId) ?? null;
      }
      get removedWithOwner() {
        return this[dogState].removedWithOwner;
      }
      set removedWithOwner(value) {
        registry.requireLive(this);
        this[dogState].removedWithOwner = booleanOf(value, 'Dog.removedWithOwner');
      }
      get mode() {
        return this[dogState].mode;
      }
      // Sets the companion mode and sends the order of it again, so an assignment of the mode it
      // has already is a fresh order. dogModeChanged is raised only when the mode differs.
      set mode(value) {
        registry.setMode(this, modeOf(value));
      }
      // What the dog is doing, as the server runs it: 'waiting', 'following', 'searching',
      // 'hunting', or 'idle' for a mode the game carries out. Its morale is not reported yet: 0.
      get objective() {
        return this[dogState].objective;
      }
      get morale() {
        return 0;
      }
      // Entity.dimension, position and rotation come from the NPC: assigning a position or a
      // rotation teleports the dog.
      get dimension() {
        return this[dogState].npc.dimension;
      }
      get position() {
        return this[dogState].npc.position;
      }
      set position(value) {
        registry.requireLive(this);
        this[dogState].npc.position = value;
      }
      get rotation() {
        return this[dogState].npc.rotation;
      }
      set rotation(value) {
        registry.requireLive(this);
        this[dogState].npc.rotation = value;
      }
      get state() {
        return this[dogState].npc.state;
      }
      // Hands the dog to another connected player, or to nobody (null) to leave it masterless.
      // Throws for anything else. dogOwnerChanged follows when the owner differs.
      giveTo(player) {
        registry.giveTo(this, player);
      }
      // Despawns the dog everywhere, after dogDestroy.
      destroy() {
        registry.host.npcs.requireRunning();
        registry.destroy(this);
      }
      toString() {
        const { npc, ownerId, mode } = this[dogState];
        return `Dog(${this.id}, soul ${npc.soul || 'none'}, owner ${ownerId}, mode ${mode})`;
      }
    }
    Object.setPrototypeOf(Dog.prototype, Entity.prototype);
    this.Dog = Dog;
    return Dog;
  }

  live(dog) {
    return Boolean(dog) && this.dogs.get(dog.id) === dog;
  }

  requireLive(dog) {
    this.host.npcs.requireRunning();
    if (!this.live(dog)) {
      throw new Error('Unknown dog');
    }
  }

  // Dog.spawn: Npc.create checks the position, rotation, soul and name and throws, before anything
  // spawns, for one it cannot use.
  spawn(Dog, owner, position, rotation, soul, name) {
    this.host.npcs.requireRunning();
    if (!this.host.connectedPlayer(owner)) {
      throw new Error('Dog.spawn needs a connected player as owner');
    }
    // A dog's soul is a GUID: the NPC roles are people.
    soulOf(soul);
    const options = {
      soul,
      name,
      class: 'Dog',
      position: this.spawnPositionOf(owner, position),
      dimension: owner.dimension,
      lootable: false,
    };
    if (rotation !== undefined) {
      options.rotation = rotation;
    }
    const npc = this.host.npcs.Npc.create(options);
    const dog = Object.create(Dog.prototype);
    Object.defineProperty(dog, 'id', { value: npc.id, enumerable: true });
    dog[dogState] = {
      npc,
      ownerId: owner.id,
      mode: DEFAULT_MODE,
      removedWithOwner: true,
      destroying: false,
      objective: 'idle',
      // Search and Hunt: when the dog was last sent, and the animal it hunts.
      orderedAt: 0,
      prey: 0,
    };
    this.dogs.set(dog.id, dog);
    this.reindex(dog, 0, owner.id);
    this.order(dog);
    this.host.emit('dogSpawn', dog);
    return dog;
  }

  // The spawn position: the owner's, with the components `position` names in place of theirs.
  spawnPositionOf(owner, position) {
    const at = owner.position;
    if (position === undefined) {
      return { x: at.x, y: at.y, z: at.z };
    }
    if (position === null || typeof position !== 'object') {
      throw new TypeError('Dog.spawn position must be a Vector3 or its components');
    }
    return { x: position.x ?? at.x, y: position.y ?? at.y, z: position.z ?? at.z };
  }

  // Sends the order the dog's mode and owner ask for. A masterless dog has nobody to follow and
  // holds where it stands.
  order(dog) {
    const state = dog[dogState];
    const { npc, mode, ownerId } = state;
    const owner = this.host.players.get(ownerId);
    const npcs = this.host.npcs;
    state.orderedAt = 0;
    state.prey = 0;
    if (!owner || mode === MODE_WAIT) {
      state.objective = 'waiting';
      npcs.hold(npc);
      return;
    }
    // hold froze the body; any other order needs it free to move.
    if (npc.frozen) {
      npcs.freeze(npc, false);
    }
    if (mode === MODE_FOLLOW) {
      state.objective = 'following';
      npcs.follow(npc, owner, FOLLOW_OPTIONS);
      return;
    }
    if (mode === MODE_SEARCH || mode === MODE_HUNT) {
      this.startBehaviour();
      this.behave(dog, Date.now());
      return;
    }
    state.objective = 'idle';
    // A KCDS addition: the modes the NPC orders have no word for. It reads as `hold`; the native
    // side follows with the dog's behaviour.
    npcs.giveOrder(npc, 'dogMode', [mode, owner.id], { shown: 'hold', ends: false });
  }

  // Runs the dogs' Search and Hunt every BEHAVIOUR_TICK_MS from the first one that needs it.
  startBehaviour() {
    if (this.timer) {
      return;
    }
    this.timer = setInterval(() => this.runBehaviour(), BEHAVIOUR_TICK_MS);
    this.timer.unref?.();
  }

  // One tick, outside any resource and never throwing: an error in a timer would take the server
  // down.
  runBehaviour() {
    this.host.context.exit(() => {
      try {
        const now = Date.now();
        for (const dog of this.dogs.values()) {
          this.behave(dog, now);
        }
      } catch (error) {
        this.host.bridge.log('host', `dogs: ${error.stack ?? error}`);
      }
    });
  }

  // One step of a Search or Hunt dog.
  behave(dog, now) {
    const state = dog[dogState];
    const owner = this.host.players.get(state.ownerId);
    const npcs = this.host.npcs;
    if (!owner || state.destroying || !npcs.live(state.npc) || !state.npc.alive) {
      return;
    }
    if (state.mode === MODE_SEARCH) {
      state.objective = 'searching';
      if (now - state.orderedAt < SEARCH_EVERY_MS) {
        return;
      }
      state.orderedAt = now;
      const angle = Math.random() * 2 * Math.PI;
      const away = SEARCH_RADIUS * Math.sqrt(Math.random());
      const at = owner.position;
      npcs.moveTo(
        state.npc,
        { x: at.x + Math.cos(angle) * away, y: at.y + Math.sin(angle) * away, z: at.z },
        { speed: 'jog' },
      );
      return;
    }
    if (state.mode !== MODE_HUNT) {
      return;
    }
    const prey = this.huntedOf(dog) ?? this.preyOf(dog);
    if (!prey) {
      // Nothing to hunt: it stays with its owner.
      if (state.objective !== 'following' || state.prey !== 0) {
        state.objective = 'following';
        state.prey = 0;
        npcs.follow(state.npc, owner, FOLLOW_OPTIONS);
      }
      return;
    }
    state.objective = 'hunting';
    state.prey = prey.id;
    state.orderedAt = now;
    if (distanceOf(state.npc, prey) <= BITE_RANGE) {
      npcs.scriptedDamage(prey, BITE_DAMAGE, state.npc);
      return;
    }
    npcs.moveTo(state.npc, prey.position, { speed: 'run', radius: 1 });
  }

  // The animal the dog hunts already, while it lives and is not HUNT_LOSE_RANGE away.
  huntedOf(dog) {
    const state = dog[dogState];
    const prey = state.prey ? this.host.npcs.npcs.get(state.prey) : undefined;
    if (!prey || prey.alive === false || distanceOf(state.npc, prey) > HUNT_LOSE_RANGE) {
      return null;
    }
    return prey;
  }

  // The nearest living animal within HUNT_RANGE of the dog that shares its world (the global
  // world is in every world, as the population's animals are), not a dog of a player.
  preyOf(dog) {
    const npc = dog[dogState].npc;
    let best = null;
    let bestDistance = HUNT_RANGE;
    for (const other of this.host.npcs.npcs.values()) {
      if (
        other === npc ||
        this.dogs.has(other.id) ||
        !ANIMAL_CLASSES.includes(other.actorClass) ||
        other.alive === false ||
        !(
          other.dimension === npc.dimension ||
          other.dimension === GLOBAL_WORLD ||
          npc.dimension === GLOBAL_WORLD
        )
      ) {
        continue;
      }
      const distance = distanceOf(npc, other);
      if (distance <= bestDistance) {
        best = other;
        bestDistance = distance;
      }
    }
    return best;
  }

  setMode(dog, mode) {
    this.requireLive(dog);
    const state = dog[dogState];
    const changed = state.mode !== mode;
    state.mode = mode;
    this.order(dog);
    if (changed) {
      this.host.emit('dogModeChanged', dog, mode);
    }
  }

  giveTo(dog, player) {
    this.requireLive(dog);
    if (player !== null && !this.host.connectedPlayer(player)) {
      throw new Error('Dog.giveTo needs a connected player or null');
    }
    this.changeOwner(dog, player);
  }

  // Makes `player` (or nobody) the owner, orders the dog for them and raises dogOwnerChanged when
  // that is another owner than before.
  changeOwner(dog, player) {
    const state = dog[dogState];
    const ownerId = player === null ? 0 : player.id;
    if (ownerId === state.ownerId) {
      return;
    }
    this.reindex(dog, state.ownerId, ownerId);
    state.ownerId = ownerId;
    this.order(dog);
    this.host.emit('dogOwnerChanged', dog, player);
  }

  // Dog.destroy, and everything that removes the NPC: raises dogDestroy once, while the handle
  // still resolves, and then lets the NPC go.
  destroy(dog) {
    if (!this.live(dog) || dog[dogState].destroying) {
      return;
    }
    this.host.npcs.destroy(dog[dogState].npc);
  }

  // The NPC is being despawned (npcs.cjs destroy), whoever asked: the dog goes with it.
  npcDestroying(npc) {
    const dog = this.dogs.get(npc.id);
    if (!dog || dog[dogState].destroying) {
      return;
    }
    dog[dogState].destroying = true;
    this.host.emit('dogDestroy', dog);
    this.reindex(dog, dog[dogState].ownerId, 0);
    this.dogs.delete(npc.id);
  }

  // A player disconnected (after playerDisconnect): their dogs go, or stay masterless when they
  // were set to stay.
  ownerLeft(player) {
    for (const dog of [...(this.byOwner.get(player.id) ?? [])]) {
      if (dog[dogState].removedWithOwner) {
        this.destroy(dog);
      } else {
        this.changeOwner(dog, null);
      }
    }
  }
}

module.exports = { Dogs };
