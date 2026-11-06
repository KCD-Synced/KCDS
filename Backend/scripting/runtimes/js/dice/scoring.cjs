'use strict';

// The dice game's scoring: the rules the game's own dice table plays by (WHGame
// 1.5.5: its combinations, and its evaluator), without dice perks;
// badges.cjs gives a player's badge rules.
//
// The combinations:
// - A single 1 is 100, a single 5 is 50.
// - Three of a kind: three 1s are 1000, three of another face 100 times the face; every die
//   past three doubles it (four of a kind twice, five four times, six eight times).
// - Straights: 1 to 5 is 500, 2 to 6 is 750, 1 to 6 is 1500.
// - A joker (face JOKER: the joker side of the game's joker dice) stands in for any die of three
//   of a kind or a straight that has at least one real die; it is never a single 1 or 5.
//
// A selection scores when it splits into combinations with every die in one; it takes its best
// split. A throw where no combination can be made is a bust.
//
// Badge rules (rules, optional): `formations`, one more combination of real dice ({ faces,
// points }: the Formations badge's Cut 3+5, Gallows 4-5-6, Eye 1-3-5), `boosts`, a face's three
// of a kind (and more) multiplied ({ 1: 3 } the Emperor's 1+1+1), and `sixes`, Tyche's factor: a
// selection with n >= 3 sixes scores 600 x (n - 2) that many times (the game's reckoning).

const FACES = [1, 2, 3, 4, 5, 6];
const JOKER = 7;
// The most dice a selection may have: six, seven with a Might badge's die.
const MAX_DICE = 7;
const STRAIGHTS = Object.freeze([
  Object.freeze({ faces: Object.freeze([1, 2, 3, 4, 5]), points: 500 }),
  Object.freeze({ faces: Object.freeze([2, 3, 4, 5, 6]), points: 750 }),
  Object.freeze({ faces: Object.freeze([1, 2, 3, 4, 5, 6]), points: 1500 }),
]);

const NO_RULES = Object.freeze({ formations: Object.freeze([]), boosts: Object.freeze({}) });

// counts[face] for the faces 1 to 6 and JOKER.
function countsOf(faces) {
  const counts = [0, 0, 0, 0, 0, 0, 0, 0];
  for (const face of faces) {
    counts[face] += 1;
  }
  return counts;
}

function diceIn(counts) {
  return counts.reduce((sum, count) => sum + count, 0);
}

// Every combination that has a real die of this face and fits in the counts: [taken counts,
// points].
function combinationsWith(face, counts, rules) {
  const found = [];
  const take = (taken, points) => {
    found.push([taken, points]);
  };
  // Singles.
  if (face === 1 || face === 5) {
    const taken = countsOf([face]);
    take(taken, face === 1 ? 100 : 50);
  }
  // Three of a kind and more, jokers standing in for all but one die.
  const triple = (face === 1 ? 1000 : face * 100) * (rules.boosts[face] ?? 1);
  for (let size = 3; size <= MAX_DICE; size++) {
    for (let real = 1; real <= Math.min(size, counts[face]); real++) {
      const jokers = size - real;
      if (jokers > counts[JOKER]) {
        continue;
      }
      const taken = countsOf([]);
      taken[face] = real;
      taken[JOKER] = jokers;
      take(taken, triple * 2 ** (size - 3));
    }
  }
  // Straights, jokers standing in for the faces not taken real.
  for (const straight of STRAIGHTS) {
    if (!straight.faces.includes(face)) {
      continue;
    }
    const others = straight.faces.filter((other) => other !== face);
    for (let realMask = 0; realMask < 1 << others.length; realMask++) {
      const taken = countsOf([face]);
      others.forEach((other, index) => {
        if (realMask & (1 << index)) {
          taken[other] += 1;
        } else {
          taken[JOKER] += 1;
        }
      });
      take(taken, straight.points);
    }
  }
  // The badge's formations, real dice only.
  for (const formation of rules.formations) {
    if (formation.faces.includes(face)) {
      take(countsOf(formation.faces), formation.points);
    }
  }
  return found.filter(([taken]) => taken.every((count, index) => count <= counts[index]));
}

// The best split of dice of these counts into combinations, every die in one; -1 when none.
function bestSplit(counts, rules, memo) {
  if (diceIn(counts) === 0) {
    return 0;
  }
  const key = counts.join(',');
  if (memo.has(key)) {
    return memo.get(key);
  }
  let best = -1;
  // The lowest real face must be in some combination (jokers left alone never score).
  const face = FACES.find((each) => counts[each] > 0);
  if (face !== undefined) {
    for (const [taken, points] of combinationsWith(face, counts, rules)) {
      const rest = counts.map((count, index) => count - taken[index]);
      const more = bestSplit(rest, rules, memo);
      if (more >= 0) {
        best = Math.max(best, points + more);
      }
    }
  }
  memo.set(key, best);
  return best;
}

// Tyche's sixes, on a selection's score.
function boostedSixes(faces, points, rules) {
  const sixes = faces.filter((face) => face === 6).length;
  if (!rules.sixes || points === 0 || sixes < 3) {
    return points;
  }
  const boosted = 600 * sixes - 1200;
  return Math.trunc(points - boosted + boosted * Math.fround(rules.sixes));
}

function validFaces(faces) {
  if (!Array.isArray(faces) || faces.length < 1 || faces.length > MAX_DICE) {
    return false;
  }
  return faces.every((face) => face === JOKER || FACES.includes(face));
}

// The score of a selection of faces (1 to 7 dice, JOKER a joker), every die counting, by the
// player's badge rules; 0 when it does not score.
function score(faces, rules = NO_RULES) {
  if (!validFaces(faces)) {
    return 0;
  }
  const points = bestSplit(countsOf(faces), rules, new Map());
  return points > 0 ? boostedSixes(faces, points, rules) : 0;
}

// Whether a throw has any combination to keep, by the player's badge rules.
function canScore(faces, rules = NO_RULES) {
  if (!validFaces(faces)) {
    return false;
  }
  const counts = countsOf(faces);
  return FACES.some((face) => counts[face] > 0 && combinationsWith(face, counts, rules).length > 0);
}

module.exports = { score, canScore, JOKER, NO_RULES };
