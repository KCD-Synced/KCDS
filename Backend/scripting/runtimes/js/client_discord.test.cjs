'use strict';

// The client's Discord global: staged fields, what is published, availability.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceDiscord } = require('../../../../Client/resources/discord.cjs');

function fixture() {
  const sent = [];
  const discord = new ResourceDiscord({ send: (message) => sent.push(message) });
  return { discord, sent, api: discord.facade() };
}

test('client Discord: nothing is published while Discord is unavailable', () => {
  const { sent, api } = fixture();
  assert.equal(api.isAvailable(), false);
  assert.equal(api.getUserId(), '');
  api.setState('Blue team, round 3');
  assert.equal(api.update(), false);
  assert.equal(api.setPresence({ state: 'x y' }), false);
  assert.equal(api.clear(), false);
  assert.deepEqual(sent, []);
});

test('client Discord: the status line and small image are published', () => {
  const { discord, sent, api } = fixture();
  discord.receiveStatus({ op: 'discordStatus', available: true, userId: '80351110224678912' });
  assert.equal(api.isAvailable(), true);
  assert.equal(api.getUserId(), '80351110224678912');
  // Only the name, status line, images and one button are the scripts'.
  for (const gone of [
    'setType',
    'setDetails',
    'setStartTimestamp',
    'setEndTimestamp',
    'setSupportedPlatforms',
    'setPartyId',
    'setPartySize',
    'setPartyPrivacy',
    'setMatchSecret',
    'setJoinSecret',
    'setSpectateSecret',
    'setInstance',
    'setParty',
    'setSecrets',
  ]) {
    assert.equal(api[gone], undefined, gone);
  }
  api.setAssets({ smallImage: 'blue', smallText: 'Blue team' });
  api.setState('Blue team, round 3');
  assert.equal(api.update(), true);
  assert.equal(api.setPresence({ state: 'Waiting for the next round' }), true);
  const published = (fields) => ({
    op: 'discordPresence',
    name: '',
    state: '',
    largeImage: '',
    largeText: '',
    smallImage: '',
    smallText: '',
    buttonLabel: '',
    buttonUrl: '',
    ...fields,
  });
  assert.deepEqual(sent, [
    published({ state: 'Blue team, round 3', smallImage: 'blue', smallText: 'Blue team' }),
    published({ state: 'Waiting for the next round', smallImage: 'blue', smallText: 'Blue team' }),
  ]);
  // reset() unstages without publishing; clear() also takes the published fields down.
  sent.length = 0;
  api.reset();
  assert.deepEqual(sent, []);
  assert.equal(api.update(), true);
  assert.deepEqual(sent.at(-1), published({}));
  assert.equal(api.clear(), true);
  assert.deepEqual(sent.at(-1), { op: 'discordClear' });
});

test('client Discord: the server name, logo and one button are published', () => {
  const { discord, sent, api } = fixture();
  discord.receiveStatus({ op: 'discordStatus', available: true, userId: '' });
  api.setName('Example');
  api.setAssets({ largeImage: 'https://example.com/logo.png', largeText: 'Example' });
  api.setButton('Our Discord', 'https://discord.gg/example');
  assert.equal(api.update(), true);
  assert.deepEqual(sent.at(-1), {
    op: 'discordPresence',
    name: 'Example',
    state: '',
    largeImage: 'https://example.com/logo.png',
    largeText: 'Example',
    smallImage: '',
    smallText: '',
    buttonLabel: 'Our Discord',
    buttonUrl: 'https://discord.gg/example',
  });
  assert.equal(api.setPresence({ button: null }), true);
  assert.equal(sent.at(-1).buttonLabel, '');
  assert.equal(
    api.setPresence({ button: { label: 'Rules', url: 'https://example.com/rules' } }),
    true,
  );
  assert.equal(sent.at(-1).buttonUrl, 'https://example.com/rules');
  assert.throws(() => api.setButton('Shop', 'http://example.com'), /https link/);
  assert.throws(() => api.setButton('x'.repeat(40), 'https://example.com'), /at most 32/);
  assert.throws(() => api.setButton('', 'https://example.com'), /label is a text/);
});

test('client Discord: arguments are checked', () => {
  const { api } = fixture();
  assert.throws(() => api.setState(5), /setState takes a text/);
  assert.throws(() => api.setState('x'.repeat(200)), /at most 128/);
  assert.throws(() => api.setPresence({ colour: 1 }), /unknown field colour/);
  assert.throws(() => api.setPresence({ secrets: { join: 'abc' } }), /unknown field secrets/);
  assert.throws(() => api.setPresence({ startTimestamp: 1 }), /unknown field startTimestamp/);
});
