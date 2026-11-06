'use strict';

// The client-only LocalNpc global (a KCDS addition): NPC bodies only
// this machine shows, for what a resource puts in front of its own player: a figure in a
// character creator, a guide, a preview. The server knows nothing of them; no other player sees
// them.
//
// The game client makes each body as it makes a server NPC's (Client/runtime/npcs/local_npcs.cpp):
// the game's own NPC of the soul, without AI ("NPC_NAI": it stands where it is put) unless the
// resource asks for one that lives on its own (`ai: true`: the soul's class, with its brain; the
// game has no female NPC without AI, so a female one always lives on its own). The
// calls go down as localNpcCreate, localNpcPlace, localNpcAppearance, localNpcAnimation,
// localNpcSay and localNpcRemove; nothing comes back. A resource's NPCs go when it stops.

const {
  soulOf,
  actorClassOf,
  nameOf,
  positionOf,
  facingOf,
  booleanOf,
  soulAppearance,
  npcAppearanceOf,
  animationRequestOf,
  speechOf,
} = require('../../Backend/scripting/runtimes/js/npcs/npc_values.cjs');
const { Quaternion } = require('../../Backend/scripting/runtimes/js/math.cjs');

// At most this many local NPCs on one machine: each is a whole body in the game.
const MAX_LOCAL_NPCS = 64;

const localState = Symbol('kcds.localNpc');

function plainPosition(position) {
  return { x: position.x, y: position.y, z: position.z };
}

function plainRotation(rotation) {
  return { x: rotation.x, y: rotation.y, z: rotation.z, w: rotation.w };
}

class ResourceLocalNpcs {
  constructor({ send, Vector3 }) {
    this.send = send;
    this.Vector3 = Vector3;
    this.nextId = 1;
    this.npcs = new Map();
    this.LocalNpc = this.define();
  }

  define() {
    const registry = this;
    class LocalNpc {
      constructor() {
        throw new TypeError('LocalNpc.create makes a local NPC');
      }
      // Makes a body only this machine shows: { soul?, class?, name?, position, rotation?,
      // appearance?, ai? }. Throws when this machine already shows MAX_LOCAL_NPCS of them.
      static create(options) {
        return registry.create(this.owner, options);
      }
      // This resource's local NPCs.
      static all() {
        return [...registry.npcs.values()].filter((npc) => npc[localState].owner === this.owner);
      }
      get id() {
        return this[localState].id;
      }
      get name() {
        return this[localState].name;
      }
      get position() {
        const { x, y, z } = this[localState].position;
        return new registry.Vector3(x, y, z);
      }
      get rotation() {
        return this[localState].rotation;
      }
      get appearance() {
        return Object.freeze({ ...this[localState].appearance });
      }
      // False once removed.
      get exists() {
        return registry.npcs.get(this.id) === this;
      }
      // Puts the body at the position, turned to the rotation when one is given.
      setPosition(position, rotation) {
        return registry.place(this, position, rotation);
      }
      // As Npc.setAppearance: other character components of the NPC's gender.
      setAppearance(request) {
        return registry.setAppearance(this, request);
      }
      // As Npc.playAnimation: a Mannequin fragment with its tags, or a clip of the game's
      // animations, looped or held at its end.
      playAnimation(fragment, options = {}) {
        return registry.animate(this, fragment, options);
      }
      stopAnimation() {
        return registry.stopAnimation(this);
      }
      // As Npc.say: the game's speech bubble over the head; "" takes it away.
      say(text, options = {}) {
        return registry.say(this, text, options);
      }
      remove() {
        registry.remove(this);
      }
      toString() {
        const name = this[localState].name;
        return name ? `LocalNpc(${this.id}, ${name})` : `LocalNpc(${this.id})`;
      }
    }
    return LocalNpc;
  }

  // The LocalNpc global of one resource.
  facade(owner) {
    const LocalNpc = this.LocalNpc;
    return Object.freeze({
      create: (options) => LocalNpc.create.call({ owner }, options),
      all: () => LocalNpc.all.call({ owner }),
    });
  }

  create(owner, options) {
    if (options === null || typeof options !== 'object') {
      throw new TypeError('LocalNpc.create takes an options object');
    }
    const soul = soulOf(options.soul);
    const actorClass = actorClassOf(options.class);
    const name = nameOf(options.name);
    const position = positionOf(options.position, 'LocalNpc.create');
    let rotation = new Quaternion();
    if (options.rotation !== undefined) {
      rotation = facingOf(options.rotation, 'LocalNpc.create');
    }
    let ai = false;
    if (options.ai !== undefined) {
      ai = booleanOf(options.ai, 'LocalNpc.create ai');
    }
    let appearance = soulAppearance();
    if (options.appearance !== undefined) {
      appearance = npcAppearanceOf(appearance, options.appearance, genderOf(actorClass));
      if (appearance === null) {
        throw new RangeError('LocalNpc.create appearance names a part the catalog does not have');
      }
    }
    if (this.npcs.size >= MAX_LOCAL_NPCS) {
      throw new RangeError(`at most ${MAX_LOCAL_NPCS} local NPCs at a time`);
    }
    const npc = Object.create(this.LocalNpc.prototype);
    const id = this.nextId;
    this.nextId += 1;
    npc[localState] = {
      id,
      owner,
      name,
      actorClass,
      position,
      rotation,
      appearance,
      animationSequence: 0,
      speechSequence: 0,
    };
    this.npcs.set(id, npc);
    // The game has no female NPC without AI: a female local NPC lives on its own.
    const bodyClass = ai || actorClass === 'NPC_Female' ? actorClass : 'NPC_NAI';
    this.send({
      op: 'localNpcCreate',
      id,
      soul,
      actorClass: bodyClass,
      name,
      position: plainPosition(position),
      rotation: plainRotation(rotation),
    });
    if (options.appearance !== undefined) {
      this.sendAppearance(npc);
    }
    return npc;
  }

  live(npc) {
    return Boolean(npc) && this.npcs.get(npc[localState]?.id) === npc;
  }

  place(npc, position, rotation) {
    const at = positionOf(position, 'LocalNpc.setPosition');
    const facing = rotation === undefined ? null : facingOf(rotation, 'LocalNpc.setPosition');
    if (!this.live(npc)) {
      return false;
    }
    const state = npc[localState];
    state.position = at;
    if (facing) {
      state.rotation = facing;
    }
    this.send({
      op: 'localNpcPlace',
      id: state.id,
      position: plainPosition(at),
      rotation: plainRotation(state.rotation),
    });
    return true;
  }

  setAppearance(npc, request) {
    const state = npc[localState];
    const next = npcAppearanceOf(state.appearance, request, genderOf(state.actorClass));
    if (next === null || !this.live(npc)) {
      return false;
    }
    state.appearance = next;
    this.sendAppearance(npc);
    return true;
  }

  sendAppearance(npc) {
    const { id, appearance } = npc[localState];
    const { body, head, hair, beard } = appearance;
    this.send({ op: 'localNpcAppearance', id, appearance: `${body}|${head}|${hair}|${beard}` });
  }

  animate(npc, fragment, options) {
    const request = animationRequestOf(fragment, options);
    if (request === null || !this.live(npc)) {
      return false;
    }
    const state = npc[localState];
    state.animationSequence += 1;
    // A fragment goes after an empty clip, as for a server NPC.
    const played =
      request.clip === undefined ? `|${request.fragment}|${request.tags}` : request.clip;
    const animation = `${state.animationSequence}|${request.loop ? 1 : 0}|${played}`;
    this.send({ op: 'localNpcAnimation', id: state.id, animation });
    return true;
  }

  stopAnimation(npc) {
    if (!this.live(npc)) {
      return false;
    }
    this.send({ op: 'localNpcAnimation', id: npc[localState].id, animation: '' });
    return true;
  }

  say(npc, given, options) {
    const { text, duration } = speechOf(given, options);
    if (!this.live(npc)) {
      return false;
    }
    const state = npc[localState];
    let speech = '';
    if (text.length > 0) {
      state.speechSequence += 1;
      speech = `${state.speechSequence}|${duration}|${text}`;
    }
    this.send({ op: 'localNpcSay', id: state.id, speech });
    return true;
  }

  remove(npc) {
    if (!this.live(npc)) {
      return;
    }
    const { id } = npc[localState];
    this.npcs.delete(id);
    this.send({ op: 'localNpcRemove', id });
  }

  // The resource stopped: its local NPCs go.
  removeOwner(owner) {
    for (const npc of [...this.npcs.values()]) {
      if (npc[localState].owner === owner) {
        this.remove(npc);
      }
    }
  }
}

// The appearance catalog's gender of an NPC class.
function genderOf(actorClass) {
  return actorClass === 'NPC_Female' ? 'female' : 'male';
}

module.exports = { ResourceLocalNpcs, MAX_LOCAL_NPCS };
