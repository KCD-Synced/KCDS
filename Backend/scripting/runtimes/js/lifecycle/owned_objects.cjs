'use strict';

// The resource whose code made a world object: props, effects, ground items, chests,
// carts, areas, blips, markers, patrol routes and siege objects go when that resource stops, as
// its NPCs and horses do. The name is kept under a symbol, so scripts never see it.
const OWNER = Symbol('owning resource');

// The name of the resource whose code runs now, or null for the host's own work.
function runningResource(host) {
  return host.context.getStore()?.name ?? null;
}

// Removes, with `remove`, each of `objects` that the stopping resource `name` made.
function removeOwned(objects, name, remove) {
  for (const object of [...objects]) {
    if (object[OWNER] === name) {
      remove(object);
    }
  }
}

module.exports = { OWNER, runningResource, removeOwned };
