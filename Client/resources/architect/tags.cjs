'use strict';

// What a catalogue item is for, read from its model's path: the game files its props by use
// (common_furniture/chairs/high/..., common_illumination/..., food/...) and names them by what
// they are (altar_..., candlestick_...). An AI assistant's catalogue filters by these tags or a
// room's kind, and the content search box matches them too ("seating", "church").
//
// Pure: the editor (editor.cjs) and an AI assistant's commands (ai_commands.cjs) use it.

// Each tag and the words of a path that give it. A word matches as it is or as its plural
// (chair, chairs; bench, benches).
const TAGS = [
  ['seating', ['chair', 'bench', 'stool', 'pew', 'kneeler', 'throne', 'seat']],
  ['table', ['table', 'desk', 'counter']],
  ['bed', ['bed', 'cradle', 'bunk', 'mattress']],
  [
    'storage',
    [
      'barrel',
      'basket',
      'cabinet',
      'chest',
      'crate',
      'shelf',
      'shelves',
      'sack',
      'tub',
      'bucket',
      'cage',
      'hanger',
      'wardrobe',
      'box',
      'coffer',
      'rack',
      'stash',
    ],
  ],
  [
    'lighting',
    [
      'candle',
      'candlestick',
      'chandelier',
      'lantern',
      'torch',
      'lamp',
      'illumination',
      'candelabra',
      'sconce',
      'brazier',
    ],
  ],
  [
    'textile',
    ['carpet', 'fur', 'pillow', 'rag', 'tapestry', 'curtain', 'banner', 'flag', 'rug', 'blanket'],
  ],
  [
    'decoration',
    ['decoration', 'deco', 'painting', 'picture', 'vase', 'flower', 'statue', 'trophy', 'antler'],
  ],
  [
    'religious',
    [
      'altar',
      'cross',
      'crucifix',
      'saint',
      'reliquary',
      'relic',
      'monstrance',
      'chalice',
      'pulpit',
      'church',
      'chapel',
      'kneeler',
      'pew',
      'sarcophagus',
      'tomb',
      'censer',
      'theological',
      'religious',
    ],
  ],
  [
    'kitchen',
    [
      'pot',
      'pan',
      'kettle',
      'plate',
      'bowl',
      'cup',
      'jug',
      'mug',
      'cauldron',
      'ladle',
      'dish',
      'spoon',
      'oven',
      'hearth',
      'pitcher',
      'tankard',
      'cooking',
      'eating',
    ],
  ],
  [
    'food',
    ['food', 'bread', 'cake', 'meat', 'fish', 'fruit', 'vegetable', 'cheese', 'drink', 'bagel'],
  ],
  [
    'tools',
    [
      'tool',
      'anvil',
      'hammer',
      'tongs',
      'saw',
      'axe',
      'pitchfork',
      'scythe',
      'shovel',
      'rake',
      'repairkit',
      'workbench',
      'grindstone',
      'loom',
    ],
  ],
  [
    'smithy',
    ['anvil', 'forge', 'bellows', 'tongs', 'smith', 'blacksmith', 'grindstone', 'smithing'],
  ],
  ['fixture', ['door', 'window', 'stair', 'ladder', 'fireplace', 'chimney', 'sill', 'fixture']],
  ['books', ['book', 'scroll', 'parchment', 'inkwell', 'quill', 'lectern']],
  [
    'weapons',
    ['weapon', 'sword', 'spear', 'shield', 'bow', 'arrow', 'armour', 'armor', 'scabbard', 'mace'],
  ],
];

// The tags a kind of room is furnished from, most needed first.
const ROOMS = {
  church: ['religious', 'seating', 'lighting', 'textile', 'decoration', 'books'],
  tavern: ['table', 'seating', 'kitchen', 'food', 'lighting', 'storage'],
  smithy: ['smithy', 'tools', 'storage', 'lighting', 'fixture'],
  bedroom: ['bed', 'storage', 'textile', 'seating', 'lighting', 'decoration'],
  kitchen: ['kitchen', 'food', 'table', 'storage', 'fixture', 'lighting'],
  storeroom: ['storage', 'food', 'tools'],
  study: ['books', 'table', 'seating', 'lighting', 'storage', 'decoration'],
  hall: ['table', 'seating', 'textile', 'decoration', 'lighting', 'weapons'],
};

const WORDS = new Map();
for (const [tag, words] of TAGS) {
  for (const word of words) {
    for (const form of [word, `${word}s`, `${word}es`]) {
      if (!WORDS.has(form)) {
        WORDS.set(form, new Set());
      }
      WORDS.get(form).add(tag);
    }
  }
}

// The tags of a catalogue item's name (a model's path or a prefab's name), in TAGS' order.
function tagsOf(name) {
  const words = String(name ?? '')
    .toLowerCase()
    .split(/[^a-z]+/)
    .filter((word) => word.length > 1);
  const found = new Set();
  for (const word of words) {
    for (const tag of WORDS.get(word) ?? []) {
      found.add(tag);
    }
  }
  return TAGS.map(([tag]) => tag).filter((tag) => found.has(tag));
}

// Whether a name matches a search: in its text, one of its tags, or a kind of room it furnishes.
function matchesSearch(name, query) {
  const wanted = String(query ?? '')
    .trim()
    .toLowerCase();
  if (wanted === '') {
    return true;
  }
  if (String(name).toLowerCase().includes(wanted)) {
    return true;
  }
  const tags = tagsOf(name);
  if (tags.includes(wanted)) {
    return true;
  }
  return furnishes(wanted, tags);
}

// Whether a thing of these tags belongs in a kind of room: by its main tag, the first (a bed with
// its pillows is a bed, no church's cloth).
function furnishes(room, tags) {
  return tags.length > 0 && (ROOMS[room] ?? []).includes(tags[0]);
}

const TAG_NAMES = TAGS.map(([tag]) => tag);

module.exports = { tagsOf, matchesSearch, furnishes, TAG_NAMES, ROOMS };
