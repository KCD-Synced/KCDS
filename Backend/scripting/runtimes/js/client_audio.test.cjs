'use strict';

// The client's Audio global: what each call asks the game and what it returns.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceAudio } = require('../../../../Client/resources/audio.cjs');

function fixture(answer = true) {
  const asked = [];
  const sync = {
    call(request) {
      asked.push(request);
      return answer;
    },
  };
  return { asked, api: new ResourceAudio({ sync }).facade() };
}

test('client Audio: each call is one synchronous request with its fields', () => {
  const { asked, api } = fixture();
  assert.equal(api.play('a_o_bell_kkut_kostelni'), true);
  assert.equal(api.playAt('c_torch_whoosh1', { x: 1, y: 2.5, z: 3 }), true);
  assert.equal(api.playFrom('f_ge_cough_woman', 42), true);
  assert.equal(api.stopFrom('f_ge_cough_woman', 42), true);
  assert.equal(api.silenceEntity(42), true);
  assert.equal(api.setSoundParameter(42, 'horse_speed', 0.5), true);
  assert.equal(api.setSoundSwitch(42, 'sword_mat', 'steel'), true);
  assert.equal(api.triggerExists('c_torch_whoosh1'), true);
  assert.equal(api.playFile('sounds/kcds/demo/line', 42), true);
  assert.equal(api.playFile('sounds/kcds/demo/line.ogg', 42, 'sfp_dialog'), true);
  assert.deepEqual(asked, [
    { op: 'audioPlay', trigger: 'a_o_bell_kkut_kostelni' },
    { op: 'audioPlayAt', trigger: 'c_torch_whoosh1', x: 1, y: 2.5, z: 3 },
    { op: 'audioPlayOnEntity', trigger: 'f_ge_cough_woman', entity: 42 },
    { op: 'audioStopOnEntity', trigger: 'f_ge_cough_woman', entity: 42 },
    { op: 'audioStopAll', entity: 42 },
    { op: 'audioParameter', entity: 42, parameter: 'horse_speed', value: 0.5 },
    { op: 'audioSwitch', entity: 42, switch: 'sword_mat', state: 'steel' },
    { op: 'audioHasTrigger', trigger: 'c_torch_whoosh1' },
    { op: 'audioPlayFile', file: 'sounds/kcds/demo/line', entity: 42, purpose: '50' },
    { op: 'audioPlayFile', file: 'sounds/kcds/demo/line.ogg', entity: 42, purpose: 'sfp_dialog' },
  ]);
});

test('client Audio: no answer or a refusal is false; arguments are checked', () => {
  assert.equal(fixture(null).api.play('x_y'), false);
  assert.equal(fixture(false).api.triggerExists('nope'), false);
  const { asked, api } = fixture();
  assert.throws(() => api.playFile('../outside', 42), /game-root path/);
  assert.throws(() => api.playFile('/sounds/line', 42), /game-root path/);
  assert.throws(() => api.playFile('C:/sounds/line', 42), /game-root path/);
  assert.throws(() => api.playFile('sounds\\line', 42), /game-root path/);
  assert.throws(() => api.playFile('sounds/line', 42, '75'), /a purpose is one of/);
  assert.throws(() => api.playFile('sounds/line', 0), /entity id/);
  assert.deepEqual(asked, []);
  assert.throws(() => api.play(''), /non-empty name/);
  assert.throws(() => api.playAt('x', { x: 1, y: Number.NaN, z: 0 }), /finite metres/);
  assert.throws(() => api.playAt('x', { x: 20000, y: 0, z: 0 }), /within/);
  assert.throws(() => api.playFrom('x', 0), /entity id/);
  assert.throws(() => api.setSoundParameter(1, 'p', Infinity), /finite number/);
});

test('client Audio: loopAt keeps a sound at a point under a number, stopLoop ends it', () => {
  const { asked, api } = fixture();
  assert.equal(api.loopAt('a_l_village_amb01', { x: 1, y: 2, z: 3 }), 1);
  assert.equal(api.loopAt('a_l_fire_torch', { x: 4, y: 5, z: 6 }), 2);
  assert.equal(api.stopLoop(1), true);
  assert.deepEqual(asked, [
    { op: 'audioLoopAt', trigger: 'a_l_village_amb01', x: 1, y: 2, z: 3, loop: 1 },
    { op: 'audioLoopAt', trigger: 'a_l_fire_torch', x: 4, y: 5, z: 6, loop: 2 },
    { op: 'audioLoopStop', loop: 1 },
  ]);
  assert.equal(fixture(false).api.loopAt('nope', { x: 0, y: 0, z: 0 }), 0);
  assert.throws(() => api.stopLoop(0), /loopAt answered/);
});

test("the server's sound plays from the body, or at the point where this game has none", () => {
  const asked = [];
  let bodyHere = true;
  const sync = {
    call(request) {
      asked.push(request);
      return request.op === 'audioPlayOnEntity' ? bodyHere : true;
    },
  };
  const audio = new ResourceAudio({ sync });
  const at = { x: 1, y: 2, z: 3 };
  assert.equal(audio.playForServer({ trigger: 'a_o_bell_kkut_kostelni', at }), true);
  assert.equal(audio.playForServer({ trigger: 'c_w_sword_hit', entity: 9, at }), true);
  bodyHere = false;
  assert.equal(audio.playForServer({ trigger: 'c_w_sword_hit', entity: 9, at }), true);
  assert.deepEqual(asked, [
    { op: 'audioPlayAt', trigger: 'a_o_bell_kkut_kostelni', x: 1, y: 2, z: 3 },
    { op: 'audioPlayOnEntity', trigger: 'c_w_sword_hit', entity: 9 },
    { op: 'audioPlayOnEntity', trigger: 'c_w_sword_hit', entity: 9 },
    { op: 'audioPlayAt', trigger: 'c_w_sword_hit', x: 1, y: 2, z: 3 },
  ]);
  assert.throws(() => audio.playForServer({ trigger: 'x' }));
  assert.throws(() => audio.playForServer(null));
});
