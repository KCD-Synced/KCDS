'use strict';

// What the studio's character can wear: the game's own outfits (its clothing
// presets, from the server catalogue's outfits, Backend/scripting/runtimes/js/inventory), each
// piece its item class in the scripts' 32 hex digits (catalog.cjs classBytesHex). The game
// client puts them on (studio/studio_outfit.hpp: { op: 'studioOutfit', pieces: [hex, ...] }).

const OUTFITS = Object.freeze([
  {
    key: 'henry',
    label: 'Henry',
    preset: '_gcbuild_henry',
    pieces: [
      { name: 'Long tunic', class: '4195742b07de606ebf494d88196df1a8' },
      { name: 'Hose', class: '4cc881371299d38976d8150243dadf97' },
      { name: 'Shoes', class: '405d72f51149024d749fb5581d04db8e' },
      { name: 'Hood', class: '4dd6ad16403287114ae0a1026e452398' },
    ],
  },
  {
    key: 'traveller',
    label: 'Henry travelling',
    preset: '_haste_startingHenryScout',
    pieces: [
      { name: 'Gambeson', class: '47310056d92e8830b22acf21407ad884' },
      { name: 'Coat', class: '4b63a259c22bffa9662083cdfac993af' },
      { name: 'Hose', class: '432802027f0bddf111028aa654521295' },
      { name: 'Ankle boots', class: '429f4eaed87e00655a28b7b11ddf7a91' },
      { name: 'Hood', class: '45484c4bbf354b49d23c5de1e44451a8' },
      { name: 'Cap', class: '48f871aacf9ec8c39e5cb211ab6db398' },
      { name: 'Hand wraps', class: '40e4ee4613eccab38c33deacf373d1b0' },
      { name: 'Belt', class: '476767c47da54a045e46cc1192ee608b' },
    ],
  },
  { key: 'none', label: 'Nothing', preset: null, pieces: [] },
]);

const FIRST = 'henry';

function outfitOf(key) {
  return OUTFITS.find((outfit) => outfit.key === key) ?? OUTFITS.find((one) => one.key === FIRST);
}

// The clothing a kept or given choice means: { outfit, off: [piece names taken off] }.
function clothingOf(given) {
  const outfit = outfitOf(given?.outfit);
  const names = new Set(outfit.pieces.map((piece) => piece.name));
  const off = Array.isArray(given?.off) ? given.off.filter((name) => names.has(name)) : [];
  return { outfit: outfit.key, off };
}

// The game client's message for a clothing choice.
function clothingMessage(clothing) {
  const outfit = outfitOf(clothing.outfit);
  const pieces = outfit.pieces
    .filter((piece) => !clothing.off.includes(piece.name))
    .map((piece) => piece.class);
  return { op: 'studioOutfit', pieces };
}

module.exports = { OUTFITS, outfitOf, clothingOf, clothingMessage };
