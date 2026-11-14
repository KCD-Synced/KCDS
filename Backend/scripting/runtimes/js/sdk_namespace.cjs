'use strict';

// What a resource script sees of the SDK: one `kcds` object, split by domain. The language's and
// the web platform's own globals (console, setTimeout, fetch, ...) stay where scripts expect them.
//
// Server and client share this table, so a type has one path on both sides.

const DOMAINS = Object.freeze({
  core: [
    'Assets',
    'Bans',
    'Chat',
    'Database',
    'Events',
    'Exports',
    'Game',
    'Http',
    'Imports',
    'Locales',
    'Messages',
    'Queue',
    'Runtime',
    'SharedState',
  ],
  math: ['Color', 'Quaternion', 'Vector2', 'Vector3', 'Vector4'],
  players: [
    'Appearances',
    'Buffs',
    'DerivedPlayerStat',
    'Entity',
    'Interactions',
    'Inventory',
    'Items',
    'LocalPlayer',
    'Needs',
    'Pickpocketing',
    'Player',
    'PlayerStat',
    'Progression',
    'Trade',
    'Voice',
  ],
  characters: [
    'Animations',
    'Dialogue',
    'Dog',
    'Follow',
    'Horse',
    'HorseLooks',
    'LocalNpc',
    'Npc',
    'PatrolRoute',
    'Quest',
    'Vendor',
  ],
  world: [
    'Area',
    'Audio',
    'Blip',
    'Cart',
    'Collision',
    'CombatMarker',
    'CookPot',
    'Crafting',
    'Dice',
    'Door',
    'Gate',
    'Gathering',
    'Gizmo',
    'Gizmos',
    'Grave',
    'Tabletop',
    'TabletopPiece',
    'TabletopDeck',
    'DeckPiece',
    'GroundItem',
    'LevelEdit',
    'Light',
    'Marker',
    'Prefab',
    'Prop',
    'PropGhost',
    'PropPlacer',
    'Recipe',
    'Render',
    'RestrictedAreaMarker',
    'SiegeEngine',
    'SiegeLadder',
    'SmartObject',
    'Stash',
    'StonePile',
    'Terrain',
    'Travel',
    'Trees',
    'Vegetation',
    'Vfx',
    'Watermark',
    'World',
    'WorldMap',
    'WorldResource',
  ],
  ui: [
    'ActionHint',
    'ActionMenu',
    'Book',
    'Camera',
    'CharacterCreator',
    'Compass',
    'Controls',
    'Discord',
    'EmoteWheel',
    'Freecam',
    'Hud',
    'Key',
    'Keybinds',
    'LoadingScreen',
    'Nameplates',
    'NativeClip',
    'NativeElement',
    'NativeScreen',
    'NativeUI',
    'LocalVoice',
    'NoClip',
    'ThirdPerson',
    'Web',
  ],
});

const DOMAIN_OF = new Map();
for (const [domain, names] of Object.entries(DOMAINS)) {
  for (const name of names) {
    DOMAIN_OF.set(name, domain);
  }
}

// A script's globals from a flat table: SDK names move under `kcds`, everything else stays.
// The domains read through to the table so a value that changes later (LocalPlayer) stays live.
function withNamespace(globals) {
  const scope = {};
  const domains = {};
  for (const domain of Object.keys(DOMAINS)) {
    domains[domain] = {};
  }
  for (const key of Object.keys(globals)) {
    const domain = DOMAIN_OF.get(key);
    if (domain) {
      Object.defineProperty(domains[domain], key, {
        get: () => globals[key],
        enumerable: true,
      });
    } else {
      Object.defineProperty(scope, key, Object.getOwnPropertyDescriptor(globals, key));
    }
  }
  for (const domain of Object.keys(domains)) {
    Object.freeze(domains[domain]);
  }
  scope.kcds = Object.freeze(domains);
  return scope;
}

module.exports = { DOMAINS, DOMAIN_OF, withNamespace };
