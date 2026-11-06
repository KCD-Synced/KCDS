'use strict';

// The client-side Light: point lights a client script shows in this
// game only, as the server's Light does for everyone (Backend/scripting/runtimes/js/lights/
// lights.cjs, the same checks). The game client shows them as it shows the server's
// (Client/runtime/lights/lights.hpp):
//   { op: 'lightLocal', kind: 'spawn' | 'update', id, position, color, intensity, radius,
//     castShadows }
//   { op: 'lightLocal', kind: 'destroy', id }
// and tells which of them it no longer shows (a world change, or no room left in the game's
// budget): { op: 'lightsGone', ids }. A light lasts until destroy(), its resource stops or the
// game drops it; `valid` turns false then. Ids come from FIRST_ID up, a range the server never
// gives.

const { CHECKS, lightOf } = require('../../Backend/scripting/runtimes/js/lights/light_values.cjs');
const { Vector3 } = require('../../Backend/scripting/runtimes/js/math.cjs');

const FIRST_ID = 0xf0000000;
const LAST_ID = 0xffffffff;
// The lights one game shows at most, the server's and the scripts' together.
const MAX_LIGHTS = 192;

const plain = (v) => ({ x: v.x, y: v.y, z: v.z });
const handleState = Symbol('kcds.light');

class ResourceLights {
  constructor({ send = () => {} } = {}) {
    this.send = send;
    this.nextId = FIRST_ID;
    // id -> { owner, handle, state }
    this.lights = new Map();
  }

  allocate() {
    const id = this.nextId;
    this.nextId = this.nextId === LAST_ID ? FIRST_ID : this.nextId + 1;
    return id;
  }

  message(kind, id, state) {
    return {
      op: 'lightLocal',
      kind,
      id,
      position: plain(state.position),
      color: plain(state.color),
      intensity: state.intensity,
      radius: state.radius,
      castShadows: state.castShadows,
      body: state.body,
      joint: state.joint,
    };
  }

  // The Light class of one resource.
  facade(owner) {
    const registry = this;
    const live = (light) => registry.lights.get(light.id)?.handle === light;
    class Light {
      constructor(token) {
        if (token !== handleState) {
          throw new TypeError('Light.spawn makes a light');
        }
      }
      // Shows a point light in this game until destroy(), the resource stops or the world goes.
      static spawn(position, options) {
        const state = lightOf(position, options);
        if (registry.lights.size >= MAX_LIGHTS) {
          throw new RangeError(`This game shows at most ${MAX_LIGHTS} lights`);
        }
        const id = registry.allocate();
        const light = new Light(handleState);
        Object.defineProperty(light, 'id', { value: id, enumerable: true });
        registry.lights.set(id, { owner, handle: light, state });
        registry.send(registry.message('spawn', id, state));
        return light;
      }
      get valid() {
        return live(this);
      }
      get position() {
        const { position } = registry.stateOf(this);
        return new Vector3(position.x, position.y, position.z);
      }
      set position(value) {
        registry.update(this, 'position', value);
      }
      get color() {
        const { color } = registry.stateOf(this);
        return new Vector3(color.x, color.y, color.z);
      }
      set color(value) {
        registry.update(this, 'color', value);
      }
      get intensity() {
        return registry.stateOf(this).intensity;
      }
      set intensity(value) {
        registry.update(this, 'intensity', value);
      }
      get radius() {
        return registry.stateOf(this).radius;
      }
      set radius(value) {
        registry.update(this, 'radius', value);
      }
      get castShadows() {
        return registry.stateOf(this).castShadows;
      }
      set castShadows(value) {
        registry.update(this, 'castShadows', value);
      }
      // Removes the light; safe to repeat.
      destroy() {
        registry.destroy(this.id);
      }
    }
    return Light;
  }

  // The last state of a light, kept after it went so a handle still reads it.
  stateOf(light) {
    const entry = this.lights.get(light.id);
    if (entry && entry.handle === light) {
      return entry.state;
    }
    return light[handleState] ?? { position: { x: 0, y: 0, z: 0 }, color: { x: 0, y: 0, z: 0 } };
  }

  update(light, name, value) {
    const checked = CHECKS[name](value);
    const entry = this.lights.get(light.id);
    if (!entry || entry.handle !== light) {
      return;
    }
    entry.state[name] = checked;
    this.send(this.message('update', light.id, entry.state));
  }

  // Forgets a light; `tell` sends the destroy down (not for one the game dropped by itself).
  forget(id, tell) {
    const entry = this.lights.get(id);
    if (!entry) {
      return false;
    }
    entry.handle[handleState] = entry.state;
    this.lights.delete(id);
    if (tell) {
      this.send({ op: 'lightLocal', kind: 'destroy', id });
    }
    return true;
  }

  destroy(id) {
    return this.forget(id, true);
  }

  // The game no longer shows these lights: { ids }.
  receiveGone(message) {
    for (const id of Array.isArray(message.ids) ? message.ids : []) {
      this.forget(id, false);
    }
  }

  // The resource stopped: its lights go with it.
  removeOwner(owner) {
    for (const [id, entry] of [...this.lights]) {
      if (entry.owner === owner) {
        this.destroy(id);
      }
    }
  }
}

module.exports = { ResourceLights, FIRST_ID, MAX_LIGHTS };
