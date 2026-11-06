'use strict';

// Dirt, blood and wear of a player's body and clothes. The player's own client reports them (the owner
// field "grime", which the native server passes as { body: { dirt, blood: [6] }, items: [{ item,
// dirt, blood: [6], wear }] }, values 0 .. 1; blood is read per body part); Player.clean and
// Player.soil send that client an order (op 'grimeOrder'), and what its game then shows reaches
// everybody.

// The game's six blood zones in its order (AddBlood's body part names head, torso, arm_left,
// arm_right, leg_left, leg_right), named as Injuries names them.
const ZONES = ['head', 'torso', 'leftArm', 'rightArm', 'leftLeg', 'rightLeg'];

// A reported grime, frozen; null before the player's client reported one.
function grimeFrom(report) {
  if (!report || typeof report !== 'object' || !report.body) {
    return null;
  }
  const state = (s) => {
    const blood = Array.isArray(s?.blood) ? s.blood : [];
    return Object.freeze({
      dirt: Number(s?.dirt) || 0,
      blood: Object.freeze(
        Object.fromEntries(ZONES.map((zone, i) => [zone, Number(blood[i]) || 0])),
      ),
    });
  };
  const items = (Array.isArray(report.items) ? report.items : []).map((piece) =>
    Object.freeze({
      item: String(piece.item ?? ''),
      ...state(piece),
      wear: Number(piece.wear) || 0,
    }),
  );
  return Object.freeze({ body: state(report.body), items: Object.freeze(items) });
}

function parts(options, method) {
  if (options === undefined) {
    return { body: true, clothes: true };
  }
  if (options === null || typeof options !== 'object') {
    throw new TypeError(`${method} takes an options object`);
  }
  const body = options.body ?? true;
  const clothes = options.clothes ?? true;
  if (typeof body !== 'boolean' || typeof clothes !== 'boolean') {
    throw new TypeError(`${method}: body and clothes are true or false`);
  }
  if (!body && !clothes) {
    throw new TypeError(`${method}: body, clothes or both`);
  }
  return { body, clothes };
}

function amount(value, name) {
  if (value === undefined) {
    return 0;
  }
  if (typeof value !== 'number' || !Number.isFinite(value) || value < 0 || value > 1) {
    throw new RangeError(`soil: ${name} is a number from 0 to 1`);
  }
  return Math.round(value * 255);
}

// Player.clean({ body, clothes }): the order to wash the body and/or the worn clothes.
function cleanOrder(id, options) {
  return { op: 'grimeOrder', id, kind: 'clean', ...parts(options, 'clean') };
}

// Player.soil({ dirt, blood, body, clothes }): the order to add dirt and blood (0 .. 1 each).
function soilOrder(id, options = {}) {
  if (options === null || typeof options !== 'object') {
    throw new TypeError('soil takes an options object');
  }
  const dirt = amount(options.dirt, 'dirt');
  const blood = amount(options.blood, 'blood');
  return {
    op: 'grimeOrder',
    id,
    kind: 'soil',
    ...parts({ body: options.body, clothes: options.clothes }, 'soil'),
    dirt,
    blood,
  };
}

module.exports = { grimeFrom, cleanOrder, soilOrder };
