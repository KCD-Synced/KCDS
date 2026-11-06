'use strict';

// The NPC roles: a name for one real soul of the game's own tables, so
// Npc.create({ soul: 'guard' }) spawns a body that already looks the part. roles.json is built
// and checked against a game installation by Dev/tools/npcs/build_roles.py:
// [{ role, soul, actorClass, look }]. Any other soul text is taken as a soul GUID.

const ROLES = require('./roles.json');

const byName = new Map(ROLES.map((entry) => [entry.role, Object.freeze({ ...entry })]));

// The role's { role, soul, actorClass, look }, or null when `name` names none.
function roleOf(name) {
  return typeof name === 'string' ? byName.get(name) ?? null : null;
}

function roleNames() {
  return [...byName.keys()];
}

module.exports = { roleOf, roleNames };
