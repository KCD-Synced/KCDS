'use strict';

// What a player wears on their head besides hair and clothing: Player.crown and Player.hairCrown.
//
// The server keeps both in the player's NetworkPlayer field "headPiece"
// (kcd2s/integration/head_pieces.hpp): "" for the default, else "<crown>|<hair>" with <crown> one
// of CROWNS or "" and <hair> 1 when a hair that carries a crown shows it, 0 when not. Every client
// draws it on that player's body, players who join later too.

// The crowns of the game a player can wear, by name: Sigismund's (the crown of his hair) and the
// two wreaths for a man's head that no game item puts on (kcd2s/integration/head_pieces.hpp
// kCrowns and Client/runtime/appearance/head_pieces.cpp keep the same list).
const CROWNS = Object.freeze(['sigismund', 'wreath', 'twitchWreath']);

function defaults() {
  return { crown: null, hairCrown: true };
}

function crownOf(value) {
  if (value === null || value === undefined) {
    return null;
  }
  if (typeof value !== 'string' || !CROWNS.includes(value)) {
    throw new TypeError(`Player.crown is null or one of: ${CROWNS.join(', ')}`);
  }
  return value;
}

function hairCrownOf(value) {
  if (typeof value !== 'boolean') {
    throw new TypeError('Player.hairCrown is true or false');
  }
  return value;
}

function fieldOf(piece) {
  if (piece.crown === null && piece.hairCrown) {
    return '';
  }
  return `${piece.crown ?? ''}|${piece.hairCrown ? 1 : 0}`;
}

module.exports = { CROWNS, defaults, crownOf, hairCrownOf, fieldOf };
