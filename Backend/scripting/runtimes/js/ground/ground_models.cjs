'use strict';

// The model an item class lies as on the ground (Items.setGroundModel, Items.model).
//
// The game lays a stack with its class's own drop model. A class without one (a worn piece whose
// clothing gives no drop model, such as some bascinets and belts, or the alchemy bases) lay with
// no mesh and no body: unseen, hanging where it was dropped, and no one could take it up again, so
// such drops were refused. Now every player's game gives that stack the game's item sack
// (DEFAULT_MODEL), and a server can name any model for a class: one of the game's meshes, or a
// resource's own streamed one. The set goes to every player's game (the reserved client event
// kcds:groundModels): the whole set when its client scripts start, and again on each change.

const { lookupItemClass } = require('../inventory/catalog.cjs');

// The game's item sack, as each player's game lays a class without a model of its own
// (Client/runtime/ground/ground_items.hpp kDefaultGroundModel).
const DEFAULT_MODEL = 'objects/manmade/common_furniture/sacks/sack_items/sack_items.cgf';
const EVENT = 'kcds:groundModels';
const MAX_MODEL_LENGTH = 256;
const MAX_MODELS = 4096;
const MODEL_FILE = /\.(cgf|cga)$/i;

// A model path as the game loads it: relative, forward slashes, a .cgf or .cga mesh.
function checkedModel(model) {
  if (typeof model !== 'string' || model.length === 0 || model.length > MAX_MODEL_LENGTH) {
    throw new TypeError(
      `Items.setGroundModel: the model is a mesh path of 1 to ${MAX_MODEL_LENGTH} characters, or null`,
    );
  }
  const path = model.replace(/\\/g, '/');
  if (path.startsWith('/') || /^[a-z]:/i.test(path) || path.split('/').includes('..')) {
    throw new TypeError('Items.setGroundModel: the model is a path inside the game data');
  }
  if (!MODEL_FILE.test(path)) {
    throw new TypeError('Items.setGroundModel: the model is a .cgf or .cga mesh');
  }
  return path;
}

class GroundModels {
  // host: players, emitToPlayerClient.
  constructor(host) {
    this.host = host;
    // Class GUID -> the model its stacks lie as.
    this.models = new Map();
  }

  // Sets the model a class's stacks lie as (null: the class's own again). Throws a TypeError for
  // an unknown class or a model that is no mesh path.
  set(item, model) {
    const itemClass = lookupItemClass(item);
    if (!itemClass) {
      throw new TypeError(`Items.setGroundModel: unknown item class: ${String(item)}`);
    }
    if (model === null || model === undefined) {
      if (!this.models.delete(itemClass.id)) {
        return false;
      }
    } else {
      const path = checkedModel(model);
      if (!this.models.has(itemClass.id) && this.models.size >= MAX_MODELS) {
        throw new RangeError(`Items.setGroundModel: at most ${MAX_MODELS} classes`);
      }
      if (this.models.get(itemClass.id) === path) {
        return true;
      }
      this.models.set(itemClass.id, path);
    }
    this.sendAll();
    return true;
  }

  // The model a class's stacks lie as: the server's, else its own, else the item sack.
  modelOf(itemClass) {
    return this.models.get(itemClass.id) ?? (itemClass.model || DEFAULT_MODEL);
  }

  payload() {
    return { models: [...this.models.entries()] };
  }

  // A player's client scripts started (again): the whole set, when there is any.
  sendTo(player) {
    if (this.models.size > 0) {
      this.host.emitToPlayerClient(player.id, EVENT, this.payload());
    }
  }

  sendAll() {
    const payload = this.payload();
    for (const player of this.host.players.values()) {
      if (player.ready) {
        this.host.emitToPlayerClient(player.id, EVENT, payload);
      }
    }
  }
}

module.exports = { GroundModels, DEFAULT_MODEL, checkedModel };
