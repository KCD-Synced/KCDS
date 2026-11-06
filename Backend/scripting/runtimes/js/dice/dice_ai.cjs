'use strict';

// The dice opponent the server plays (Dice.startAgainstNpc): which dice it keeps and
// whether it throws on or banks, by its skill (0 to 100; easy 20, normal 50, hard 85). It keeps the
// combination worth the most (of those, the one that keeps fewest dice, to throw more), and banks
// once the turn's points reach what the dice it would throw are worth risking. A low skill banks
// early and now and then keeps a single scoring die where more were there; a high skill risks more
// when it is far behind and banks at once when banking wins.

const { score } = require('./scoring.cjs');

const SKILLS = Object.freeze({ easy: 20, normal: 50, hard: 85 });
// The turn's points it banks at, by the dice it would throw next (1 to 6): an unskilled player's
// and a skilled player's (Farkle's known good play, the game's scores being Farkle's).
const CAUTIOUS = Object.freeze([0, 150, 200, 250, 350, 600, 1000]);
const SKILLED = Object.freeze([0, 300, 300, 400, 700, 1500, 3000]);
// How often (0 to 1) an unskilled player keeps one scoring die where it could keep more.
const SLIP = 0.3;
// Far behind: a skilled player risks this much more.
const BEHIND = 1000;
const BEHIND_RISK = 1.5;

// The skill of a difficulty: 'easy', 'normal', 'hard' or a number 0 to 100.
function skillOf(difficulty) {
  if (difficulty === undefined) {
    return SKILLS.normal;
  }
  if (typeof difficulty === 'string' && SKILLS[difficulty] !== undefined) {
    return SKILLS[difficulty];
  }
  if (typeof difficulty === 'number' && Number.isFinite(difficulty)) {
    if (difficulty < 0 || difficulty > 100) {
      throw new RangeError('Dice difficulty is a skill from 0 to 100');
    }
    return difficulty;
  }
  throw new TypeError("Dice difficulty is 'easy', 'normal', 'hard' or a skill from 0 to 100");
}

function bits(mask) {
  let count = 0;
  for (let die = 0; die < 7; die++) {
    if (mask & (1 << die)) {
      count += 1;
    }
  }
  return count;
}

// The combinations it may keep of the dice just thrown: { held, points }, best first.
function keeps(faces, rolledMask, rules) {
  const dice = [];
  for (let die = 0; die < 7; die++) {
    if (rolledMask & (1 << die)) {
      dice.push(die);
    }
  }
  const found = [];
  for (let subset = 1; subset < 1 << dice.length; subset++) {
    let held = 0;
    const kept = [];
    dice.forEach((die, index) => {
      if (subset & (1 << index)) {
        held |= 1 << die;
        kept.push(faces[die]);
      }
    });
    const points = score(kept, rules);
    if (points > 0) {
      found.push({ held, points });
    }
  }
  found.sort((a, b) => b.points - a.points || bits(a.held) - bits(b.held));
  return found;
}

// Its move on the dice just thrown: { held, kind: 'continue' | 'pass' }, or null when nothing
// scores. `turn`: { faces, rolledMask, available, allDice, rules, turnScore, own, other, target }.
function choose(turn, skill, random = Math.random) {
  const options = keeps(turn.faces, turn.rolledMask, turn.rules);
  if (options.length === 0) {
    return null;
  }
  let pick = options[0];
  const slip = SLIP * (1 - skill / 100);
  if (random() < slip) {
    // One scoring die where more were there.
    pick = options.filter((option) => bits(option.held) === 1).at(-1) ?? pick;
  }
  const banked = turn.turnScore + pick.points;
  if (turn.own + banked >= turn.target) {
    return { held: pick.held, kind: 'pass' };
  }
  let left = bits(turn.available & ~pick.held);
  if (left === 0) {
    // Every die scored: all of them are thrown again.
    left = bits(turn.allDice);
  }
  const t = skill / 100;
  let threshold = CAUTIOUS[Math.min(left, 6)] * (1 - t) + SKILLED[Math.min(left, 6)] * t;
  if (turn.other - turn.own >= BEHIND) {
    threshold *= 1 + (BEHIND_RISK - 1) * t;
  }
  return { held: pick.held, kind: banked >= threshold ? 'pass' : 'continue' };
}

module.exports = { SKILLS, skillOf, choose, keeps };
