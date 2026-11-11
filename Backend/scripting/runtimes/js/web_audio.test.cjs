'use strict';

// Player.webVolume and webMuted (web/web_audio.cjs): the server's share of a player's web view
// sound, told to that player's game when it changes.

const test = require('node:test');
const assert = require('node:assert/strict');

const { WebAudio, WEB_AUDIO_EVENT } = require('./web/web_audio.cjs');

function setup() {
  const told = [];
  const host = {
    services: { emitToPlayer: (id, name, payload) => told.push({ id, name, payload }) },
  };
  return { audio: new WebAudio(host), told };
}

test('a player starts at 100 and not muted, and nothing is told', () => {
  const { audio, told } = setup();
  assert.deepEqual(audio.of(7), { volume: 100, muted: false });
  assert.equal(told.length, 0);
});

test('a change is told to that player only, the same value again is not', () => {
  const { audio, told } = setup();
  audio.setVolume(7, 40);
  audio.setVolume(7, 40);
  audio.setMuted(7, true);
  assert.deepEqual(told, [
    { id: 7, name: WEB_AUDIO_EVENT, payload: { volume: 40, muted: false } },
    { id: 7, name: WEB_AUDIO_EVENT, payload: { volume: 40, muted: true } },
  ]);
  assert.deepEqual(audio.of(8), { volume: 100, muted: false });
});

test('volumes outside 0 to 100 and a muted that is no boolean are refused', () => {
  const { audio } = setup();
  assert.throws(() => audio.setVolume(7, 101), RangeError);
  assert.throws(() => audio.setVolume(7, -1), RangeError);
  assert.throws(() => audio.setVolume(7, Number.NaN), RangeError);
  assert.throws(() => audio.setMuted(7, 'yes'), TypeError);
  assert.deepEqual(audio.of(7), { volume: 100, muted: false });
});

test('a player who leaves is forgotten', () => {
  const { audio } = setup();
  audio.setVolume(7, 10);
  audio.left(7);
  assert.deepEqual(audio.of(7), { volume: 100, muted: false });
});
