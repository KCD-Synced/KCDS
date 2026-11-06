'use strict';

// Hud screen effects (Client/resources/screen_effects.cjs): parameters, fades, timers,
// ownership and how several effects combine into the engine's post-effect values.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceHud } = require('../../../../Client/resources/hud.cjs');

function setup() {
  const sent = [];
  let time = 1000;
  const pending = [];
  const timers = {
    setTimeout(callback, ms) {
      pending.push({ at: time + ms, callback });
      return pending.length;
    },
  };
  const hud = new ResourceHud({ send: (message) => sent.push(message), now: () => time, timers });
  // Moves the clock on, running the ticks that fall due.
  function advance(ms) {
    const end = time + ms;
    for (;;) {
      pending.sort((a, b) => a.at - b.at);
      if (pending.length === 0 || pending[0].at > end) {
        break;
      }
      const next = pending.shift();
      time = next.at;
      next.callback();
    }
    time = end;
  }
  const last = () => sent.filter((message) => message.op === 'screenFx').at(-1)?.values;
  return { hud, sent, advance, last, a: hud.facade('a'), b: hud.facade('b') };
}

test('effectList lists the four effects with their parameters', () => {
  const { a } = setup();
  const effects = a.effectList();
  assert.deepEqual(
    effects.map((effect) => effect.name),
    ['colorGrade', 'blur', 'ghosting', 'screenBlood'],
  );
  const saturation = effects[0].params.find((entry) => entry.name === 'saturation');
  assert.deepEqual(
    [saturation.min, saturation.max, saturation.default, saturation.neutral],
    [0, 2, 1, 1],
  );
  assert.equal(effects[1].params[0].default, 0.5);
});

test('an effect without fades applies at once and clamps its parameters', () => {
  const { a, last } = setup();
  const handle = a.startEffect('colorGrade', { saturation: 5, hue: -3 });
  assert.ok(handle > 0);
  assert.equal(last().Global_User_Saturation, 2);
  assert.equal(last().Global_User_ColorHue, -1);
  assert.equal(last().Global_User_Contrast, 1);
  assert.equal(a.effectRunning(handle), true);
});

test('unknown effects and parameters throw', () => {
  const { a } = setup();
  assert.throws(() => a.startEffect('vignette'), RangeError);
  assert.throws(() => a.startEffect('blur', { saturation: 1 }), RangeError);
  assert.throws(() => a.startEffect('blur', { amount: 'x' }), TypeError);
  assert.throws(() => a.startEffect('blur', {}, { fadeInMs: -1 }), TypeError);
});

test('effects combine: multiply, add and strongest', () => {
  const { a, b, last } = setup();
  a.startEffect('colorGrade', { saturation: 0.5, yellow: 0.2 });
  b.startEffect('colorGrade', { saturation: 0.5, yellow: 0.1 });
  a.startEffect('blur', { amount: 0.3 });
  b.startEffect('blur', { amount: 0.6 });
  assert.equal(last().Global_User_Saturation, 0.25);
  assert.equal(last().Global_User_ColorY, 0.3);
  assert.equal(last().FilterBlurring_Amount, 0.6);
});

test('fade in, update with a fade, and remove with the fade-out given at creation', () => {
  const { a, advance, last } = setup();
  const blur = a.startEffect('blur', { amount: 0.8 }, { fadeInMs: 1000, fadeOutMs: 500 });
  assert.equal(last().FilterBlurring_Amount, 0);
  advance(500);
  assert.ok(Math.abs(last().FilterBlurring_Amount - 0.4) < 0.03);
  advance(600);
  assert.equal(last().FilterBlurring_Amount, 0.8);
  assert.equal(a.changeEffect(blur, { amount: 0.2 }, { fadeMs: 1000 }), true);
  advance(1100);
  assert.equal(last().FilterBlurring_Amount, 0.2);
  assert.equal(a.endEffect(blur), true);
  assert.equal(a.effectRunning(blur), true);
  advance(600);
  assert.equal(last().FilterBlurring_Amount, 0);
  assert.equal(a.effectRunning(blur), false);
});

test('a duration ends the effect, then it fades out', () => {
  const { a, advance, last } = setup();
  const blood = a.startEffect('screenBlood', { amount: 1 }, { durationMs: 250, fadeOutMs: 900 });
  assert.equal(last().Global_User_ColorM, 0.35);
  assert.equal(last().Global_User_ColorY, 0.35);
  assert.equal(last().Global_User_Brightness, 0.85);
  assert.equal(last().ScreenBlood_Amount, undefined);
  advance(300);
  assert.equal(a.effectRunning(blood), true);
  assert.ok(last().Global_User_ColorM < 0.35);
  advance(1000);
  assert.equal(last().Global_User_ColorM, 0);
  assert.equal(last().Global_User_Brightness, 1);
  assert.equal(a.effectRunning(blood), false);
});

test('only the owner updates or removes; removeAll counts its own', () => {
  const { a, b, last } = setup();
  const mine = a.startEffect('ghosting', { amount: 0.5 });
  b.startEffect('ghosting', { amount: 0.2 });
  assert.equal(b.changeEffect(mine, { amount: 1 }), false);
  assert.equal(b.endEffect(mine), false);
  assert.equal(a.endAllEffects(), 1);
  assert.equal(last().ImageGhosting_Amount, 0.2);
  assert.equal(a.changeEffect(mine, { amount: 1 }), false);
});

test('64 effects at most; a stopped resource and a level change take theirs', () => {
  const { hud, a, b, last } = setup();
  for (let index = 0; index < 64; index += 1) {
    assert.ok(a.startEffect('blur', { amount: 0.1 }) > 0);
  }
  assert.equal(b.startEffect('blur'), 0);
  hud.removeOwner('a');
  assert.equal(last().FilterBlurring_Amount, 0);
  hud.receiveHost({ ready: true });
  const handle = b.startEffect('screenBlood', { amount: 0.7 });
  hud.receiveHost({ ready: false });
  assert.equal(b.effectRunning(handle), false);
  assert.equal(last().Global_User_ColorM, 0);
});
