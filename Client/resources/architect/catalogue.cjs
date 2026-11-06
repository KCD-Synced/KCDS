'use strict';

// World Architect's catalogue (first form): what the editor places, by category, from
// the game's own data. Buildings, structures and doors are the game's prefabs (the catalogue of
// Backend/scripting/runtimes/js/props/prefabs.json); trees and bushes are the level's vegetation
// kinds (Vegetation.kinds(), asked of the game).

const CATEGORIES_EFFECTS = [];
const CATEGORIES_LIST = [
  {
    key: 'houses',
    label: 'Houses',
    source: 'prefab',
    match: (path) => path.includes('living/houses'),
  },
  {
    key: 'structures',
    label: 'Structures',
    source: 'prefab',
    match: (path) => path.startsWith('structures/') && !path.includes('living/houses'),
  },
  { key: 'doors', label: 'Doors', source: 'prefab', match: (path) => path.startsWith('door/') },
  { key: 'trees', label: 'Trees', source: 'vegetation', match: (path) => path.includes('/trees/') },
  {
    key: 'bushes',
    label: 'Bushes',
    source: 'vegetation',
    match: (path) => path.includes('/bushes/') || path.includes('/shrubs/'),
  },
  // Props: any static mesh of the game's prop catalog (props/catalog.json), placed solid.
  {
    key: 'furniture',
    label: 'Furniture',
    source: 'prop',
    match: (path) => path.includes('/common_furniture/'),
  },
  {
    key: 'decorations',
    label: 'Decorations',
    source: 'prop',
    match: (path) => path.includes('/common_decorations/'),
  },
  {
    key: 'fixtures',
    label: 'Fixtures and lights',
    source: 'prop',
    match: (path) => path.includes('/common_fixtures/') || path.includes('/common_illumination/'),
  },
  {
    key: 'goods',
    label: 'Tools, food, carts',
    source: 'prop',
    match: (path) =>
      ['/common_tools/', '/manmade/food/', '/manmade/vehicles/', '/manmade/weapons/'].some((part) =>
        path.includes(part),
      ),
  },
  {
    key: 'work',
    label: 'Work props',
    source: 'prop',
    match: (path) =>
      path.includes('/task_specific_props/') || path.includes('/quest_specific_props/'),
  },
  {
    key: 'rocks',
    label: 'Rocks and stones',
    source: 'prop',
    match: (path) =>
      ['/natural/rocks/', '/natural/stones/', '/natural/landslips/'].some((part) =>
        path.includes(part),
      ),
  },
  {
    key: 'parts',
    label: 'Building parts',
    source: 'prop',
    match: (path) => path.startsWith('objects/manmade/structures/'),
  },
  // The author's own models (Documents\KCDS\World Architect\assets, <top>/kcds/<name>/...).
  {
    key: 'yours',
    label: 'Yours',
    source: 'prop',
    match: (path) => path.split('/')[1] === 'kcds',
  },
];

// Effects: the game's particle effects (Vfx.list, vfx/catalog.json) by their group
// ("WH_Particels.<group>.<name>").
const EFFECT_GROUPS = Object.freeze({
  fire: ['fires', 'explosion', 'hand_cannon', 'arrows_and_firearms'],
  smoke: ['smokes', 'dust', 'dirt', 'test_smoke'],
  weather: ['weather', 'water', 'streams', 'rapids', 'waterfalls', 'falling', 'flying'],
  work: [
    'particles_smithery',
    'blacksmith',
    'bathhouse',
    'woodwork',
    'wood',
    'cutting',
    'minting',
    'butcher',
    'stonemason',
    'washing',
    'housework',
  ],
  combat: ['combat', 'blood', 'destructibles', 'horse', 'body_and_fecal', 'animals'],
});
const groupOf = (name) => name.split('.')[1] ?? '';
const effectCategory = (key, label, test) => ({ key, label, source: 'effect', match: test });
CATEGORIES_EFFECTS.push(
  effectCategory('fire', 'Fire and explosions', (n) => EFFECT_GROUPS.fire.includes(groupOf(n))),
  effectCategory('smoke', 'Smoke and dust', (n) => EFFECT_GROUPS.smoke.includes(groupOf(n))),
  effectCategory('weather', 'Weather and water', (n) => EFFECT_GROUPS.weather.includes(groupOf(n))),
  effectCategory('effects_work', 'Work', (n) => EFFECT_GROUPS.work.includes(groupOf(n))),
  effectCategory('combat', 'Combat and blood', (n) => EFFECT_GROUPS.combat.includes(groupOf(n))),
  effectCategory(
    'effects_other',
    'Other',
    (n) => !Object.values(EFFECT_GROUPS).some((groups) => groups.includes(groupOf(n))),
  ),
);

const CATEGORIES = Object.freeze([...CATEGORIES_LIST, ...CATEGORIES_EFFECTS]);

// Meshes no one places: physics and shadow proxies, level-of-detail copies, editor helpers.
const UNPLACEABLE = ['proxy', '_lod', 'collision', '/special/', '_occluder', 'helper'];

// The last part of a path, without its extension: "house_1s_2r_a".
function shortName(path) {
  if (!path.includes('/')) {
    // An effect: "WH_Particels.fires.candle" -> "candle".
    return path.slice(path.lastIndexOf('.') + 1);
  }
  return path.slice(path.lastIndexOf('/') + 1).replace(/\.cgf$/, '');
}

class Catalogue {
  // prefabNames(): the buildable prefab paths; vegetationKinds(): the level's vegetation kinds
  // (asked again only when the level changed it, see refresh()).
  constructor({ prefabNames, vegetationKinds, propMeshes, effectNames, userMeshes }) {
    this.prefabNames = prefabNames;
    this.vegetationKinds = vegetationKinds;
    this.propMeshes = propMeshes ?? (() => []);
    this.effectNames = effectNames ?? (() => []);
    this.userMeshes = userMeshes ?? (() => []);
    this.lists = new Map();
  }

  categories() {
    return CATEGORIES;
  }

  // The entries of a category: { label, name, source }, in catalogue order.
  entries(key) {
    if (!this.lists.has(key)) {
      const category = CATEGORIES.find((entry) => entry.key === key);
      if (!category) {
        return [];
      }
      let names = [];
      if (category.source === 'prefab') {
        names = this.prefabNames();
      } else if (category.source === 'vegetation') {
        names = this.vegetationKinds();
      } else if (category.source === 'effect') {
        names = this.effectNames();
      } else {
        names = [...this.propMeshes(), ...this.userMeshes()].filter(
          (name) => !UNPLACEABLE.some((part) => name.includes(part)),
        );
      }
      const entries = names
        .filter((name) => name !== '' && category.match(name))
        .map((name) => ({ label: shortName(name), name, source: category.source }));
      this.lists.set(key, entries);
    }
    return this.lists.get(key);
  }

  // A new world: the vegetation kinds are asked again.
  refresh() {
    this.lists.clear();
  }
}

module.exports = { Catalogue, CATEGORIES, shortName };
