'use strict';

// The paths of the game's door leaves Door.spawn hangs (door_models.json, built by
// Dev/tools/doors/build_door_models.py), for World Architect's Game doors folder. Shared by the
// server and the game client's resources.

const MODEL_ROWS = require('./door_models.json');

const DOOR_FOLDER = 'objects/characters/assets/doors/';

// Every door leaf's path, in the file's order.
function doorModelPaths() {
  return MODEL_ROWS.map((row) => row[0]);
}

// Whether a mesh path is one of the game's door leaves.
function isDoorModel(path) {
  return typeof path === 'string' && path.toLowerCase().startsWith(DOOR_FOLDER);
}

module.exports = { doorModelPaths, isDoorModel, DOOR_FOLDER };
