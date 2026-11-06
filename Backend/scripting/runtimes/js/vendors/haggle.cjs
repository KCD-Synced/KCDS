'use strict';

// Haggling with a server vendor: the player haggles in the game's own haggle
// (the shop screen's haggle and the trader's dialogue), and every price they offer comes to the
// server, which answers it. A vendor haggles when Vendor.create has `haggle`:
//
//   haggle: true | { tolerance?, rounds? }
//     tolerance  the most the vendor gives off the basket's list price, as a share of the basket's
//                volume (what the player buys plus what they sell): 0 to 0.35, 0.1 by default
//     rounds     the offers the vendor hears before it haggles no more: 1 to 20, 5 by default
//
// The vendorHaggle handlers answer an offer first: (vendor, player, basket, offered), basket
// { bought, sold, balance, price, asked, round, mood }. A handler returns true or { accept: true }
// to take the offer, { counter: price, mood? } to ask another price, false or { refuse: true } to
// haggle no more, or nothing to leave the offer to the next handler. Without an answer the
// vendor's own rule answers: the game's (Scripts/Systems/KCD1/Negotiation.lua, WHGame 1.5.5),
// without its dice:
//
//   - an offer at or above the vendor's last price is taken;
//   - the same offer twice is mocked, and the trader grows impatient;
//   - otherwise the vendor comes towards the offer by 0.35 + 0.15 * dominance of the way, in the
//     basket's price steps, and takes it once the two are within 0.025 + 0.025 * dominance of the
//     volume or a step apart;
//   - every offer far from the vendor's price makes the trader more impatient; at 0.95, or after
//     `rounds` offers, the trader refuses and haggles no more in this session.
//
// The vendor never takes or asks less than the list price minus `tolerance` of the volume, nor
// more than its purse pays. The dominance is the player's Speech: -1 at level 0, 0 at 15, 1 at 30.
//
// Prices are what the player pays, in money units; a negative price is what the vendor pays.

const MAX_PRICE = 1000000000;
const DEFAULT_TOLERANCE = 0.1;
const MAX_TOLERANCE = 0.35;
const DEFAULT_ROUNDS = 5;
const MAX_ROUNDS = 20;
// The game's impatience: an offer made twice, the hard limit, and the growth per round.
const MOCK_IMPATIENCE = 0.3;
const IMPATIENCE_LIMIT = 0.95;
const IMPATIENCE_EXPONENT = 0.4;
const SPEECH_MIDDLE = 15;

const ACCEPT = 1;
const COUNTER = 2;
const MOCK = 3;
const REFUSE = 4;

function isObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

// Vendor.create's haggle option: null (no haggling) or { tolerance, rounds }; throws for an
// unusable one.
function haggleOptionsOf(value) {
  if (value === undefined || value === false || value === null) {
    return null;
  }
  if (value === true) {
    return { tolerance: DEFAULT_TOLERANCE, rounds: DEFAULT_ROUNDS };
  }
  if (!isObject(value)) {
    throw new TypeError('Vendor.create options.haggle is true, false or { tolerance, rounds }');
  }
  const { tolerance = DEFAULT_TOLERANCE, rounds = DEFAULT_ROUNDS } = value;
  if (typeof tolerance !== 'number' || !(tolerance >= 0) || tolerance > MAX_TOLERANCE) {
    throw new RangeError(
      `Vendor.create options.haggle.tolerance is a number from 0 to ${MAX_TOLERANCE}`,
    );
  }
  if (!Number.isInteger(rounds) || rounds < 1 || rounds > MAX_ROUNDS) {
    throw new RangeError(
      `Vendor.create options.haggle.rounds is a whole number from 1 to ${MAX_ROUNDS}`,
    );
  }
  return { tolerance, rounds };
}

// The game's price step for a basket's volume (Negotiation:CalcStep).
function stepOf(volume) {
  if (volume < 100) {
    return 1;
  }
  if (volume < 500) {
    return 5;
  }
  if (volume < 1000) {
    return 10;
  }
  if (volume < 5000) {
    return 50;
  }
  return 100;
}

// A price to the nearest step (Negotiation:Round).
function toStep(value, step) {
  const rest = value - Math.floor(value / step) * step;
  if (rest < step / 2) {
    return Math.floor(value / step) * step;
  }
  return Math.ceil(value / step) * step;
}

function clamp(value, low, high) {
  return Math.min(Math.max(value, low), high);
}

// The player's barter dominance from their Speech level, -1 to 1.
function dominanceOf(speech) {
  const level = Number.isFinite(speech) ? speech : 0;
  return clamp((level - SPEECH_MIDDLE) / SPEECH_MIDDLE, -1, 1);
}

// A new haggle over one basket: `price` the list price, `volume` what is bought plus what is sold.
function startHaggle(key, price, volume) {
  return {
    key,
    price,
    volume,
    step: stepOf(volume),
    asked: price,
    lastOffer: price,
    impatience: 0,
    round: 0,
    agreed: null,
    refused: false,
  };
}

// The lowest price the vendor takes or asks: `tolerance` of the volume off the list price, and
// no more than its purse pays (null: a purse that never runs out).
function floorOf(haggle, options, purse) {
  let floor = toStep(haggle.price - options.tolerance * haggle.volume, haggle.step);
  if (floor > haggle.price) {
    floor = haggle.price;
  }
  if (purse !== null && floor < -purse) {
    floor = -purse;
  }
  return floor;
}

function answer(kind, price, impatience) {
  return { answer: kind, price, mood: Math.round(clamp(impatience, 0, 1) * 100) };
}

// The vendor's own answer to `offered` (the game's Negotiation:CalcReaction without its dice).
function ruleAnswer(haggle, offered, options, dominance, purse) {
  const floor = floorOf(haggle, options, purse);
  if (haggle.volume === 0 || offered >= haggle.asked) {
    return answer(ACCEPT, Math.max(offered, floor), haggle.impatience);
  }
  if (haggle.round > 1 && offered === haggle.lastOffer) {
    const impatience = haggle.impatience + MOCK_IMPATIENCE;
    if (impatience >= IMPATIENCE_LIMIT || haggle.round >= options.rounds) {
      return answer(REFUSE, haggle.asked, impatience);
    }
    return answer(MOCK, haggle.asked, impatience);
  }
  const ratio = 0.35 + dominance * 0.15;
  let asked = toStep(haggle.asked + (offered - haggle.asked) * ratio, haggle.step);
  if (asked < floor) {
    asked = floor;
  }
  const tolerance = 0.025 + dominance * 0.025;
  if (offered >= floor && (asked - offered) / haggle.volume <= tolerance) {
    return answer(ACCEPT, offered, haggle.impatience);
  }
  const k = 5;
  const growth = Math.pow((haggle.asked - offered) / (haggle.volume / k), 2) / k;
  const factor = Math.pow(1.75 - dominance * 0.5, 1.15);
  const impatience = Math.pow(haggle.impatience + growth * factor, IMPATIENCE_EXPONENT);
  if (impatience >= IMPATIENCE_LIMIT || haggle.round >= options.rounds) {
    return answer(REFUSE, asked, impatience);
  }
  if (offered >= floor && asked - offered <= haggle.step) {
    return answer(ACCEPT, offered, impatience);
  }
  return answer(COUNTER, asked, impatience);
}

function wholePrice(value) {
  return Number.isInteger(value) && Math.abs(value) <= MAX_PRICE;
}

// A vendorHaggle handler's answer as { answer, price, mood }, or null for none; throws for an
// unusable one.
function scriptAnswer(result, haggle, offered) {
  if (result === true) {
    return answer(ACCEPT, offered, haggle.impatience);
  }
  if (!isObject(result)) {
    throw new TypeError('vendorHaggle answers true, false, { accept }, { counter } or { refuse }');
  }
  if (result.refuse === true) {
    return answer(REFUSE, haggle.asked, haggle.impatience);
  }
  if (result.accept === true) {
    return answer(ACCEPT, offered, haggle.impatience);
  }
  if (result.counter !== undefined) {
    if (!wholePrice(result.counter)) {
      throw new RangeError(
        `vendorHaggle counter is a whole number from -${MAX_PRICE} to ${MAX_PRICE}`,
      );
    }
    const mood = result.mood ?? haggle.impatience;
    if (typeof mood !== 'number' || !(mood >= 0) || mood > 1) {
      throw new RangeError('vendorHaggle mood is a number from 0 to 1');
    }
    // A counter at or below the offer takes the offer.
    if (result.counter <= offered) {
      return answer(ACCEPT, offered, mood);
    }
    return answer(COUNTER, result.counter, mood);
  }
  throw new TypeError('vendorHaggle answers true, false, { accept }, { counter } or { refuse }');
}

module.exports = {
  ACCEPT,
  COUNTER,
  MOCK,
  REFUSE,
  MAX_ROUNDS,
  haggleOptionsOf,
  dominanceOf,
  startHaggle,
  ruleAnswer,
  scriptAnswer,
  wholePrice,
};
