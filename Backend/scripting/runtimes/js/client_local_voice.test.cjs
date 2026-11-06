'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceLocalVoice, flatZones } = require('../../../../Client/resources/local_voice.cjs');

function rig() {
  const sent = [];
  const events = [];
  const voice = new ResourceLocalVoice({
    send: (message) => sent.push(message),
    emit: (name, args) => events.push([name, ...args]),
  });
  return { voice, api: voice.facade(), sent, events };
}

test('the state follows the game client, and a change of transmitting is told', () => {
  const { voice, api, events } = rig();
  assert.equal(api.isEnabled(), false);
  voice.update({ enabled: true, tier: 2, transmitting: true, hasMicrophone: true, mode: 'voice' });
  assert.equal(api.isEnabled(), true);
  assert.equal(api.getTier(), 'shout');
  assert.equal(api.isTransmitting(), true);
  assert.equal(api.hasMicrophone(), true);
  assert.equal(api.getMode(), 'voice');
  assert.deepEqual(events, [['localVoiceTransmitting', true]]);
  voice.update({ enabled: true, tier: 2, transmitting: true, hasMicrophone: true });
  assert.equal(events.length, 1);
});

test('asking for a tier and setting the mode go to the game client, checked', () => {
  const { api, sent } = rig();
  api.requestTier('whisper');
  api.setMode('voice');
  assert.deepEqual(sent, [
    { op: 'voiceTierAsk', tier: 0 },
    { op: 'voiceSettings', mode: 'voice' },
  ]);
  assert.throws(() => api.requestTier('loud'), TypeError);
  assert.throws(() => api.setMode('always'), TypeError);
});

test('zones are sent flat', () => {
  const min = { x: 1, y: 2, z: 3 };
  const max = { x: 4, y: 5, z: 6 };
  assert.deepEqual(
    flatZones([{ min, max, reverb: 'church', occlusion: 0.5 }]),
    [1, 2, 3, 4, 5, 6, 4, 0.5],
  );
  assert.deepEqual(flatZones([{ min, max }]), [1, 2, 3, 4, 5, 6, 0, 0]);
});

test('a bad zone is refused with a reason and nothing is sent', () => {
  const { api, sent } = rig();
  const min = { x: 1, y: 2, z: 3 };
  const max = { x: 4, y: 5, z: 6 };
  assert.throws(() => api.setZones('nope'), TypeError);
  assert.throws(() => api.setZones([{ min }]), TypeError);
  assert.throws(() => api.setZones([{ min: max, max: min }]), RangeError);
  assert.throws(() => api.setZones([{ min, max, reverb: 'cave' }]), TypeError);
  assert.throws(() => api.setZones([{ min, max, occlusion: 2 }]), RangeError);
  assert.equal(sent.length, 0);
  api.setZones([{ min, max, reverb: 'hut' }]);
  assert.equal(sent.length, 1);
});
