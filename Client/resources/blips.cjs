'use strict';

// The client-only Blip global: marks this client's scripts put on its own compass and
// map screen, which the server and the other players know nothing of. The game client draws them
// as the server's blips are drawn (Client/runtime/markers/blips.cpp), under a location id of their
// own, and keeps them: the game empties its registry with every level load, and the blips are
// drawn again when the next world is ready. The runtime asks with
//
//   { op: 'clientBlipShow', blip, type, markState, label, compass, map, legend, position }
//   { op: 'clientBlipHide', blip }
//
// and the game client says whether the compass carries the mark with { op: 'blipLive', blip,
// live }. A stopped resource's blips go with it.
//
// KCDS addition: `image`, a .dds the resource ships (64 x 64, or an uncompressed 32-bit one of
// another size), is the blip's own icon on the map screen in place of its type's. The file goes to
// the game client as an item icon does and is mounted where the map loads icons
// (Libs/UI/Textures/Icons/Map/<name>_icon.dds); clientBlipShow then carries `image`, that name.
//
// `legend`: the map legend's switch for the blip's type hides it on the map screen as
// it hides the game's own marks of that type, unless `legend` is false.

const {
  DEFAULT_BLIP_TYPE,
  isBlipType,
} = require('../../Backend/scripting/runtimes/js/markers/blip_types.cjs');

const MAX_BLIPS = 256;
const LABEL_BYTES = 64;
const MAX_COORDINATE = 100000;

const encoder = new TextEncoder();

function fail(message) {
  throw new TypeError(`Blip: ${message}`);
}

function positionOf(value) {
  const valid =
    value !== null &&
    typeof value === 'object' &&
    [value.x, value.y, value.z].every(
      (axis) =>
        typeof axis === 'number' && Number.isFinite(axis) && Math.abs(axis) <= MAX_COORDINATE,
    );
  if (!valid) {
    fail('a position is a point { x, y, z } in the world');
  }
  return { x: value.x, y: value.y, z: value.z };
}

function typeOf(value) {
  if (typeof value !== 'string' || !isBlipType(value)) {
    fail(`type is one of the game's compass mark names, such as ${DEFAULT_BLIP_TYPE}`);
  }
  return value;
}

function stateOf(value) {
  if (!Number.isInteger(value)) {
    fail('state is a whole number');
  }
  return value;
}

// The label cut to 64 UTF-8 bytes, whole characters only.
function labelOf(value) {
  if (typeof value !== 'string') {
    fail('label is a string');
  }
  let label = value;
  while (encoder.encode(label).length > LABEL_BYTES) {
    label = Array.from(label).slice(0, -1).join('');
  }
  return label;
}

function flagOf(value, name) {
  if (typeof value !== 'boolean') {
    fail(`${name} is true or false`);
  }
  return value;
}

const DEFAULTS = Object.freeze({
  type: DEFAULT_BLIP_TYPE,
  state: 0,
  label: '',
  image: '',
  compass: true,
  map: true,
  legend: true,
  position: Object.freeze({ x: 0, y: 0, z: 0 }),
});

class ResourceBlips {
  // `shipImage(resource, path)`: sends and mounts a shipped .dds as a map icon and answers its
  // name; throws when the resource does not ship it.
  constructor({ send, Vector3, shipImage = null }) {
    this.send = send;
    this.Vector3 = Vector3;
    this.shipImage = shipImage;
    this.nextId = 1;
    // blip id -> { id, owner, type, state, label, compass, map, legend, position, live }, in
    // creation order
    this.blips = new Map();
  }

  show(blip) {
    const message = {
      op: 'clientBlipShow',
      blip: blip.id,
      type: blip.type,
      markState: blip.state,
      label: blip.label,
      compass: blip.compass,
      map: blip.map,
      legend: blip.legend,
      position: { ...blip.position },
    };
    if (blip.icon) {
      message.image = blip.icon;
    }
    this.send(message);
  }

  // The map icon's name of a shipped image path ('' for none).
  iconOf(owner, image) {
    if (image === '') {
      return '';
    }
    if (typeof image !== 'string') {
      fail('image is the path of a .dds the resource ships');
    }
    if (!this.shipImage) {
      fail('image needs the game client');
    }
    return this.shipImage(owner, image);
  }

  create(owner, options) {
    if (options === null || typeof options !== 'object') {
      fail('create takes an options object with a position');
    }
    const blip = {
      id: 0,
      owner,
      type: options.type === undefined ? DEFAULT_BLIP_TYPE : typeOf(options.type),
      // markState, as on the server; state is its old name, kept for 0.3.4.
      state: stateOf(options.markState ?? options.state ?? 0),
      label: options.label === undefined ? '' : labelOf(options.label),
      compass: options.compass === undefined ? true : flagOf(options.compass, 'compass'),
      map: options.map === undefined ? true : flagOf(options.map, 'map'),
      legend: options.legend === undefined ? true : flagOf(options.legend, 'legend'),
      position: positionOf(options.position),
      image: '',
      icon: '',
      live: false,
    };
    if (this.blips.size >= MAX_BLIPS) {
      return null;
    }
    if (options.image !== undefined) {
      blip.icon = this.iconOf(owner, options.image);
      blip.image = options.image;
    }
    blip.id = this.nextId++;
    this.blips.set(blip.id, blip);
    this.show(blip);
    return blip.id;
  }

  // Changes one field of a live blip and draws it again; a removed blip is left alone.
  change(id, field, value) {
    const blip = this.blips.get(id);
    if (!blip) {
      return;
    }
    blip[field] = value;
    this.show(blip);
  }

  changeImage(owner, id, image) {
    const blip = this.blips.get(id);
    if (!blip) {
      return;
    }
    blip.icon = this.iconOf(owner, image);
    blip.image = image;
    this.show(blip);
  }

  read(id, field) {
    const blip = this.blips.get(id);
    return blip ? blip[field] : DEFAULTS[field];
  }

  remove(id) {
    if (!this.blips.delete(id)) {
      return false;
    }
    this.send({ op: 'clientBlipHide', blip: id });
    return true;
  }

  removeAll() {
    for (const id of [...this.blips.keys()]) {
      this.remove(id);
    }
  }

  removeOwner(owner) {
    for (const [id, blip] of [...this.blips]) {
      if (blip.owner === owner) {
        this.remove(id);
      }
    }
  }

  // { blip, live } from the game client: whether the compass carries the mark.
  receiveLive({ blip, live }) {
    const kept = this.blips.get(blip);
    if (kept && typeof live === 'boolean') {
      kept.live = live;
    }
  }

  // The Blip class of one resource; every resource sees every blip of this client.
  facade(owner) {
    const registry = this;
    const { Vector3 } = this;
    const handles = new Map();
    const handleOf = (id) => {
      let handle = handles.get(id);
      if (!handle) {
        handle = Object.create(Blip.prototype);
        Object.defineProperty(handle, 'id', { value: id, enumerable: true });
        handles.set(id, handle);
      }
      return handle;
    };
    class Blip {
      constructor(id) {
        if (!registry.blips.has(id)) {
          throw new Error('Unknown blip');
        }
        return handleOf(id);
      }
      get valid() {
        return registry.blips.has(this.id);
      }
      get live() {
        return registry.blips.get(this.id)?.live ?? false;
      }
      get position() {
        const { x, y, z } = registry.read(this.id, 'position');
        return new Vector3(x, y, z);
      }
      set position(value) {
        registry.change(this.id, 'position', positionOf(value));
      }
      get type() {
        return registry.read(this.id, 'type');
      }
      set type(value) {
        registry.change(this.id, 'type', typeOf(value));
      }
      get markState() {
        return registry.read(this.id, 'state');
      }
      set markState(value) {
        registry.change(this.id, 'state', stateOf(value));
      }
      // The old name of markState, kept for 0.3.4: an entity's state is its SharedState.
      get state() {
        return this.markState;
      }
      set state(value) {
        this.markState = value;
      }
      get label() {
        return registry.read(this.id, 'label');
      }
      set label(value) {
        registry.change(this.id, 'label', labelOf(value));
      }
      get compass() {
        return registry.read(this.id, 'compass');
      }
      set compass(value) {
        registry.change(this.id, 'compass', flagOf(value, 'compass'));
      }
      get map() {
        return registry.read(this.id, 'map');
      }
      set map(value) {
        registry.change(this.id, 'map', flagOf(value, 'map'));
      }
      // Whether the map legend's switch for its type hides it.
      get legend() {
        return registry.read(this.id, 'legend');
      }
      set legend(value) {
        registry.change(this.id, 'legend', flagOf(value, 'legend'));
      }
      // KCDS: the shipped .dds that is its map icon, '' for its type's.
      get image() {
        return registry.read(this.id, 'image');
      }
      set image(value) {
        registry.changeImage(owner, this.id, value);
      }
      toString() {
        const blip = registry.blips.get(this.id);
        if (!blip) {
          return `Blip(${this.id}, removed)`;
        }
        const { x, y, z } = blip.position;
        return `Blip(${this.id}, ${blip.type}, ${x}, ${y}, ${z})`;
      }
      remove() {
        return registry.remove(this.id);
      }
      static create(options) {
        const id = registry.create(owner, options);
        return id === null ? null : handleOf(id);
      }
      static getById(id) {
        return registry.blips.has(id) ? handleOf(id) : null;
      }
      static all() {
        return [...registry.blips.keys()].map(handleOf);
      }
      static removeAll() {
        registry.removeAll();
      }
    }
    return Blip;
  }
}

module.exports = { ResourceBlips };
