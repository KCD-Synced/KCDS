'use strict';

// Point lights the server puts in the world: the server API's Light.
// A light is a replicated entity every client with a ready world shows (Client/runtime/lights/
// lights.hpp), a player who arrives later too, until it is destroyed or the resource that made it
// stops. The lights of a world export (WorldResource) belong to the server and stay.
//
// Display goes through these bridge ops, sent only when something changes:
//   { op: 'lightSpawn', id, position, color, intensity, radius, castShadows, body, joint }
//   { op: 'lightUpdate', id, position, color, intensity, radius, castShadows, body, joint }
// body is a player's or NPC's network id the light hangs on, 0 for none; position is
// then the offset from it or from its joint.
//   { op: 'lightDestroy', id }
// A spawn sent to one player (a world that became ready) names them in `to`.

const { OWNER, runningResource, removeOwned } = require('../lifecycle/owned_objects.cjs');
const { Vector3 } = require('../math.cjs');
const { CHECKS, lightOf } = require('./light_values.cjs');

const plain = (v) => ({ x: v.x, y: v.y, z: v.z });

class Lights {
  // `host`: the resource host (bridge, emit, allocateEntityId, the running resource).
  constructor(host) {
    this.host = host;
    // id -> Light
    this.lights = new Map();
  }

  checkOwner() {
    const owner = this.host.context.getStore();
    if (
      owner &&
      (this.host.resources.get(owner.name) !== owner || !owner.active || owner.failing)
    ) {
      throw new Error('Resource is stopped');
    }
  }

  // The Light class of the server API, an Entity.
  define(Entity) {
    const registry = this;
    class Light {
      // A handle for an existing light by its network id.
      constructor(id) {
        const light = registry.lights.get(id);
        if (!light) {
          throw new Error('Unknown light');
        }
        return light;
      }
      static spawn(position, options) {
        return registry.spawn(Light, position, options);
      }
      get position() {
        return new Vector3(this.state.position.x, this.state.position.y, this.state.position.z);
      }
      set position(value) {
        registry.update(this, 'position', value);
      }
      get color() {
        return new Vector3(this.state.color.x, this.state.color.y, this.state.color.z);
      }
      set color(value) {
        registry.update(this, 'color', value);
      }
      get intensity() {
        return this.state.intensity;
      }
      set intensity(value) {
        registry.update(this, 'intensity', value);
      }
      get radius() {
        return this.state.radius;
      }
      set radius(value) {
        registry.update(this, 'radius', value);
      }
      get castShadows() {
        return this.state.castShadows;
      }
      set castShadows(value) {
        registry.update(this, 'castShadows', value);
      }
      // Removes the light from every client; safe to repeat.
      destroy() {
        registry.destroy(this);
      }
      toString() {
        return `Light(${this.id})`;
      }
    }
    Object.setPrototypeOf(Light.prototype, Entity.prototype);
    this.Light = Light;
    return Light;
  }

  spawn(Light, position, options) {
    this.checkOwner();
    const state = lightOf(position, options);
    const light = Object.create(Light.prototype);
    Object.defineProperty(light, 'id', { value: this.host.allocateEntityId(), enumerable: true });
    Object.defineProperty(light, 'state', { value: state });
    light[OWNER] = runningResource(this.host);
    this.lights.set(light.id, light);
    this.host.bridge.send(this.message('lightSpawn', light));
    return light;
  }

  // A light's op as it stands now.
  message(op, light) {
    const { position, color, intensity, radius, castShadows, body, joint } = light.state;
    return {
      op,
      id: light.id,
      position: plain(position),
      color: plain(color),
      intensity,
      radius,
      castShadows,
      body,
      joint,
    };
  }

  // A player's world became ready: their game lost the lights, so it is told of each again. The
  // op names the player, so nobody else hears it.
  arrived(player) {
    for (const light of this.lights.values()) {
      this.host.bridge.send({ ...this.message('lightSpawn', light), to: player.id });
    }
  }

  // One property checked, then changed and told to every client; nothing is sent when it stays.
  update(light, name, value) {
    const checked = CHECKS[name](value);
    if (this.lights.get(light.id) !== light) {
      return;
    }
    const before = light.state[name];
    const same =
      typeof checked === 'object'
        ? before.x === checked.x && before.y === checked.y && before.z === checked.z
        : before === checked;
    if (same) {
      return;
    }
    light.state[name] = checked;
    this.host.bridge.send(this.message('lightUpdate', light));
  }

  destroy(light) {
    if (this.lights.get(light.id) !== light) {
      return;
    }
    this.lights.delete(light.id);
    this.host.bridge.send({ op: 'lightDestroy', id: light.id });
  }

  // The lights a stopping resource made go with it.
  resourceStopped(name) {
    removeOwned(this.lights.values(), name, (light) => this.destroy(light));
  }
}

module.exports = { Lights };
