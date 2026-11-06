'use strict';

// What the game calls a place (World.setPlaceName): the server's name replaces the text
// of the place's localization keys in every player's game, a player who joins later too. It goes
// the way Inventory.renameItem's names do (ITEM_NAMES). The catalog is places.json, built from the
// game's own text tables by Dev/tools/world/build_places.py.

const CATALOG = require('./places.json');

const NAME_BYTES = 128;
const PLACES = new Map(CATALOG.map((place) => [place.id.toLowerCase(), place]));

class PlaceNames {
  constructor(inventories) {
    this.inventories = inventories;
    // Place id -> the name the server gave it.
    this.names = new Map();
  }

  // Sets a place's name; null gives the game's own back. False for a place the game does not have.
  set(place, name) {
    if (typeof place !== 'string') {
      throw new TypeError('World.setPlaceName: the place is a string');
    }
    if (name !== null) {
      if (
        typeof name !== 'string' ||
        name.length === 0 ||
        Buffer.byteLength(name) > NAME_BYTES ||
        /[\x00-\x1f\x7f]/.test(name)
      ) {
        throw new TypeError(
          'World.setPlaceName: a name is 1 to 128 bytes without control characters, or null',
        );
      }
    }
    const found = PLACES.get(place.toLowerCase());
    if (!found) {
      return false;
    }
    const names = this.inventories.itemNames;
    if (name === null) {
      this.names.delete(found.id);
    } else {
      this.names.set(found.id, name);
    }
    for (const key of found.keys) {
      if (name === null) {
        names.delete(key);
      } else {
        names.set(key, name);
      }
    }
    this.inventories.sendNames(
      0,
      false,
      found.keys.map((key) => [key, name ?? '']),
    );
    return true;
  }

  // Every place: its id, the game's English name and what it is called now.
  list() {
    return CATALOG.map((place) => ({
      id: place.id,
      gameName: place.name,
      name: this.names.get(place.id) ?? place.name,
    }));
  }
}

module.exports = { PlaceNames };
