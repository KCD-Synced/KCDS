'use strict';

const assert = require('node:assert/strict');
const { test } = require('node:test');

const { VoiceChat } = require('./voice/voice.cjs');

function rig() {
  const sent = [];
  const events = [];
  const one = { id: 1 };
  const two = { id: 2 };
  const host = {
    players: new Map([
      [1, one],
      [2, two],
    ]),
    bridge: { send: (op) => sent.push(op) },
    emit: (...args) => {
      events.push(args);
      return host.answer ?? true;
    },
  };
  return { chat: new VoiceChat(host), host, sent, events, one, two };
}

test('voice is on by default, and off goes to the native side once', () => {
  const { chat, sent } = rig();
  const voice = chat.api();
  assert.equal(voice.isEnabled(), true);
  voice.setEnabled(true);
  assert.equal(sent.length, 0);
  voice.setEnabled(false);
  voice.setEnabled(false);
  assert.deepEqual(sent, [{ op: 'voiceEnabled', enabled: false }]);
  assert.equal(voice.isEnabled(), false);
  assert.throws(() => voice.setEnabled('off'), TypeError);
});

// a server hides the game's voice indicator for a resource's own view.
test('the indicator is shown by default, and hiding it goes to the native side once', () => {
  const { chat, sent } = rig();
  const voice = chat.api();
  assert.equal(voice.isIndicatorShown(), true);
  voice.setIndicator(true);
  assert.equal(sent.length, 0);
  voice.setIndicator(false);
  voice.setIndicator(false);
  assert.deepEqual(sent, [{ op: 'voiceIndicator', shown: false }]);
  assert.equal(voice.isIndicatorShown(), false);
  assert.equal(voice.isEnabled(), true, 'voice chat itself stays on');
  assert.throws(() => voice.setIndicator('off'), TypeError);
});

test('switching off ends everyone talking', () => {
  const { chat, events, one } = rig();
  chat.talkingChanged(1, true);
  assert.equal(chat.api().isTalking(one), true);
  chat.api().setEnabled(false);
  assert.equal(chat.api().isTalking(one), false);
  assert.deepEqual(events.at(-1), ['voiceTalkingChanged', one, false]);
});

test('ranges have defaults and limits', () => {
  const { chat, sent } = rig();
  const voice = chat.api();
  assert.equal(voice.getRange('whisper'), 2);
  assert.equal(voice.getRange('normal'), 10);
  assert.equal(voice.getRange('shout'), 30);
  voice.setRange('shout', 50);
  assert.equal(voice.getRange('shout'), 50);
  assert.deepEqual(sent.at(-1), { op: 'voiceRange', tier: 2, meters: 50 });
  assert.throws(() => voice.setRange('loud', 5), TypeError);
  assert.throws(() => voice.setRange('normal', -1), RangeError);
  assert.throws(() => voice.setRange('normal', 501), RangeError);
});

test('tier, mute and deaf are kept per player', () => {
  const { chat, sent, one, two } = rig();
  const voice = chat.api();
  assert.equal(voice.getPlayerTier(one), 'normal');
  voice.setPlayerTier(one, 'shout');
  assert.equal(voice.getPlayerTier(one), 'shout');
  assert.equal(voice.getPlayerTier(two), 'normal');
  assert.deepEqual(sent.at(-1), { op: 'voiceTier', id: 1, tier: 2 });
  voice.setPlayerMuffled(one, 0.8);
  assert.equal(voice.getPlayerMuffled(one), 0.8);
  assert.equal(voice.getPlayerMuffled(two), 0);
  voice.setPlayerMuffled(one, false);
  assert.equal(voice.getPlayerMuffled(one), 0);
  assert.throws(() => voice.setPlayerMuffled(one, 2), RangeError);
  voice.setPlayerMuted(one, true);
  voice.setPlayerDeaf(two, true);
  assert.equal(voice.isPlayerMuted(one), true);
  assert.equal(voice.isPlayerMuted(two), false);
  assert.equal(voice.isPlayerDeaf(two), true);
  assert.throws(() => voice.setPlayerTier({ id: 9 }, 'normal'), TypeError);
});

test('a listener mute touches that listener alone', () => {
  const { chat, sent, one, two } = rig();
  const voice = chat.api();
  voice.setListenerMuted(one, two, true);
  assert.equal(voice.isListenerMuted(one, two), true);
  assert.equal(voice.isListenerMuted(two, one), false);
  assert.deepEqual(sent.at(-1), { op: 'voiceListenerMute', id: 1, talker: 2, muted: true });
  voice.setListenerMuted(one, two, false);
  assert.equal(voice.isListenerMuted(one, two), false);
});

test('a talking change is told once', () => {
  const { chat, events, one } = rig();
  chat.talkingChanged(1, true);
  chat.talkingChanged(1, true);
  assert.deepEqual(events, [['voiceTalkingChanged', one, true]]);
});

test("a player's tier request can be refused by a script", () => {
  const { chat, host, sent, one } = rig();
  chat.tierRequested(1, 2);
  assert.equal(chat.api().getPlayerTier(one), 'shout');
  assert.equal(sent.length, 1);
  host.answer = false;
  chat.tierRequested(1, 0);
  assert.equal(chat.api().getPlayerTier(one), 'shout');
  assert.equal(sent.length, 1);
});

test('a player who left is forgotten everywhere', () => {
  const { chat, one, two } = rig();
  const voice = chat.api();
  voice.setPlayerMuted(two, true);
  voice.setListenerMuted(one, two, true);
  chat.talkingChanged(2, true);
  chat.left(2);
  assert.equal(chat.talking.has(2), false);
  assert.equal(chat.players.has(2), false);
  assert.equal(voice.isListenerMuted(one, two), false);
});
