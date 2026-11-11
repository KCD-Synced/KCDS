'use strict';

// The level's story layers as states of places (World Architect's Story tab and its AI's
// story_layers). The level splits one state of a place into several layers: a fortress yard
// before its siege is .../series_Predhradi/beforeSiege with its meshes, and under it _enviro,
// _common and script/... with more. A state is a layer's place in the map's tree up to its first
// part that starts with "_" or is "script"; a layer the files give no place (only meshes) whose
// name is a state's last part belongs to that state, else it is a state of its own.

const HELPER_PART = /^(_|script$)/;

// The state a layer's place in the tree belongs to: its path up to the first helper part.
function statePath(path) {
  const parts = String(path ?? '')
    .split('/')
    .filter((part) => part !== '');
  const helper = parts.findIndex((part, index) => index > 0 && HELPER_PART.test(part));
  return (helper === -1 ? parts : parts.slice(0, helper)).join('/');
}

// The layers (as the editor's levelLayers answers them, nearest first) grouped into states, in the
// order their nearest layer came: [{ key, short, where, on, brushes, shown, at, layers }]. A state
// is on while the game draws any of its meshes, or, with no meshes, while any of its layers is on.
function storyStates(layers) {
  const states = new Map();
  const byLast = new Map();
  const loose = [];
  for (const layer of layers ?? []) {
    const path = statePath(layer.path);
    if (path === '') {
      loose.push(layer);
      continue;
    }
    let state = states.get(path);
    if (!state) {
      const parts = path.split('/');
      state = {
        key: path,
        short: parts[parts.length - 1],
        where: parts.slice(1, -1).join(' / '),
        layers: [],
        at: layer.at,
      };
      states.set(path, state);
      if (!byLast.has(state.short)) {
        byLast.set(state.short, state);
      }
    }
    state.layers.push(layer);
  }
  for (const layer of loose) {
    const short = layer.short || layer.name;
    const owner = byLast.get(short);
    if (owner) {
      owner.layers.push(layer);
      continue;
    }
    const key = `layer:${layer.name}`;
    states.set(key, { key, short, where: '', layers: [layer], at: layer.at });
  }
  return [...states.values()].map((state) => {
    const brushes = state.layers.reduce((sum, layer) => sum + (layer.brushes ?? 0), 0);
    const shown = state.layers.reduce((sum, layer) => sum + (layer.shown ?? 0), 0);
    return {
      ...state,
      brushes,
      shown,
      on: brushes > 0 ? shown > 0 : state.layers.some((layer) => layer.on),
    };
  });
}

module.exports = { statePath, storyStates };
