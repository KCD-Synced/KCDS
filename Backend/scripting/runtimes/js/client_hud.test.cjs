'use strict';

// The client Hud global (Client/resources/hud.cjs): message calls, their arguments and the
// HUD-ready gate.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceHud } = require('../../../../Client/resources/hud.cjs');

function setup(ready = true) {
  const sent = [];
  const hud = new ResourceHud({ send: (message) => sent.push(message) });
  hud.receiveHost({ ready });
  return { hud, sent, facade: hud.facade('hud'), shop: hud.facade('shop') };
}

test('without a HUD every message call returns false and sends nothing', () => {
  const { facade, sent } = setup(false);
  assert.equal(facade.showNotification('hello'), false);
  assert.equal(facade.hideBanner(), false);
  assert.equal(facade.showTutorial('OB_O20_Inventory'), false);
  assert.deepEqual(sent, []);
});

test('the HUD-ready report opens and closes the gate', () => {
  const { hud, facade, sent } = setup(false);
  hud.receiveHost({ ready: true });
  assert.equal(facade.clearNotifications(), true);
  hud.receiveHost({ ready: false });
  assert.equal(facade.clearNotifications(), false);
  assert.equal(sent.length, 1);
});

test('showBanner fills in the defaults in the movie order', () => {
  const { facade, sent } = setup();
  assert.equal(facade.showBanner('Market opens'), true);
  assert.equal(facade.showBanner('Hold', 1500.7, 2), true);
  assert.deepEqual(sent, [
    { op: 'hud', call: 'showBanner', args: ['Market opens', 0, 3000, false] },
    { op: 'hud', call: 'showBanner', args: ['Hold', 2, 1500, false] },
  ]);
});

test('toasts, panels and tutorials pass their arguments through', () => {
  const { facade, sent } = setup();
  facade.notifyPerk('perk_alchemist', 'Alchemist');
  facade.notifyReputation(3, 'Rattay likes you');
  facade.notifyEventResult(1, 'Won', 'Dice');
  facade.notifyXp('str', 'Strength');
  facade.hideTutorial('OB_O20_Inventory');
  facade.closeTutorial();
  assert.deepEqual(
    sent.map(({ call, args }) => [call, args]),
    [
      ['notifyPerk', ['perk_alchemist', 'Alchemist']],
      ['notifyReputation', [3, 'Rattay likes you']],
      ['notifyEventResult', [1, 'Won', 'Dice']],
      ['notifyXp', ['str', 'Strength', 5]],
      ['hideTutorial', ['OB_O20_Inventory']],
      ['closeTutorial', []],
    ],
  );
});

test('wrong argument types throw before anything is sent', () => {
  const { facade, sent } = setup();
  assert.throws(() => facade.showNotification(5), TypeError);
  assert.throws(() => facade.showBanner('x', 'long'), TypeError);
  assert.throws(() => facade.notifyReputation(undefined, 'x'), TypeError);
  assert.throws(() => facade.notifyPerkUse('icon'), TypeError);
  assert.deepEqual(sent, []);
});

test('hidden elements are held per resource, retained before the HUD and published as a set', () => {
  const { hud, facade, shop, sent } = setup(false);
  assert.equal(facade.setHudPart('Compass', false), true);
  assert.equal(shop.setHudPart('Compass', false), true);
  assert.equal(shop.setHudPart('Stats', false), true);
  assert.equal(facade.hudPartShown('Compass'), false);
  facade.setHudPart('Compass', true);
  assert.equal(facade.hudPartShown('Compass'), false); // shop still hides it
  shop.setHudPart('Compass', true);
  assert.equal(facade.hudPartShown('Compass'), true);
  hud.removeOwner('shop');
  assert.equal(facade.hudPartShown('Stats'), true);
  assert.deepEqual(
    sent.map(({ op, elements }) => [op, elements]),
    [
      ['hudHidden', ['Compass']],
      ['hudHidden', ['Compass', 'Stats']],
      ['hudHidden', ['Stats']],
      ['hudHidden', []],
    ],
  );
});

test('element names are compared exactly', () => {
  const { facade } = setup();
  assert.throws(() => facade.setHudPart('compass', false), RangeError);
  assert.throws(() => facade.hudPartShown('Fader'), RangeError);
  assert.throws(() => facade.setHudPart('Compass', 0), TypeError);
});

test('inConversation follows the game dialogue state the game client reports', () => {
  const { hud, facade } = setup();
  assert.equal(facade.inConversation(), false);
  hud.receiveHost({ ready: true, dialogue: true });
  assert.equal(facade.inConversation(), true);
  hud.receiveHost({ ready: false, dialogue: false });
  assert.equal(facade.inConversation(), false);
});

test('worldToScreen asks for the camera once and projects with it', () => {
  const sent = [];
  const hud = new ResourceHud({
    send: (message) => sent.push(message),
    screenSize: () => ({ width: 1600, height: 900 }),
  });
  const facade = hud.facade('hud');
  assert.equal(facade.worldToScreen({ x: 0, y: 10, z: 0 }), null);
  facade.worldToScreen({ x: 0, y: 10, z: 0 });
  assert.deepEqual(
    sent.filter((message) => message.op === 'cameraWanted'),
    [{ op: 'cameraWanted' }],
  );
  // At the origin, looking along +y, 90 degrees vertical field of view.
  hud.receiveCamera({ m: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0], fov: Math.PI / 2 });
  assert.deepEqual(facade.worldToScreen({ x: 0, y: 10, z: 0 }), { x: 800, y: 450, onScreen: true });
  const above = facade.worldToScreen({ x: 0, y: 10, z: 10 });
  assert.ok(Math.abs(above.y) < 1e-6);
  const aside = facade.worldToScreen({ x: 40, y: 10, z: 0 });
  assert.equal(aside.onScreen, false);
  assert.ok(aside.x > 1600);
  assert.equal(facade.worldToScreen({ x: 0, y: -5, z: 0 }), null);
  hud.receiveCamera({});
  assert.equal(facade.worldToScreen({ x: 0, y: 10, z: 0 }), null);
  assert.throws(() => facade.worldToScreen({ x: 0, y: 'a', z: 0 }), TypeError);
});

test('the monologue is suppressed by default and raises monologueSuppressed until enabled', () => {
  const sent = [];
  const emitted = [];
  const hud = new ResourceHud({
    send: (message) => sent.push(message),
    emit: (name, args) => emitted.push([name, ...args]),
  });
  hud.receiveHost({ ready: true });
  const facade = hud.facade('hud');
  assert.equal(facade.innerVoiceOn(), false);
  hud.receiveMonologueSuppressed({
    textKey: 'nelz_jindrich__kde_nic_ne_FJoo',
    text: "You can't get blood from a stone.",
  });
  hud.receiveMonologueSuppressed({ textKey: 'HRAC_UNKNOWN', text: '' });
  assert.deepEqual(emitted, [
    ['monologueSuppressed', "You can't get blood from a stone.", 'nelz_jindrich__kde_nic_ne_FJoo'],
    ['monologueSuppressed', '', 'HRAC_UNKNOWN'],
  ]);
  facade.setInnerVoice(true);
  assert.equal(facade.innerVoiceOn(), true);
  assert.deepEqual(sent.at(-1), { op: 'hudMonologue', enabled: true });
  hud.receiveMonologueSuppressed({ textKey: 'late', text: 'late' });
  assert.equal(emitted.length, 2);
  hud.receiveHost({ ready: false });
  assert.equal(facade.innerVoiceOn(), false);
  assert.throws(() => facade.setInnerVoice('yes'), TypeError);
});

test('story cutscenes are suppressed by default and raise storyCutsceneSuppressed until enabled', () => {
  const sent = [];
  const emitted = [];
  const hud = new ResourceHud({
    send: (message) => sent.push(message),
    emit: (name, args) => emitted.push([name, ...args]),
  });
  hud.receiveHost({ ready: true });
  const facade = hud.facade('hud');
  assert.equal(facade.cutscenesOn(), false);
  hud.receiveStoryCutsceneSuppressed({ cutscene: 'q_prologue_intro' });
  hud.receiveStoryCutsceneSuppressed({ cutscene: 7 });
  assert.deepEqual(emitted, [['storyCutsceneSuppressed', 'q_prologue_intro']]);
  facade.setCutscenes(true);
  assert.equal(facade.cutscenesOn(), true);
  assert.deepEqual(sent.at(-1), { op: 'hudStoryCutscenes', enabled: true });
  hud.receiveStoryCutsceneSuppressed({ cutscene: 'late' });
  assert.equal(emitted.length, 1);
  hud.receiveHost({ ready: false });
  assert.equal(facade.cutscenesOn(), false);
  assert.throws(() => facade.setCutscenes(1), TypeError);
});

test('notifyXp takes the game type unless one is given', () => {
  const { facade, sent } = setup();
  facade.notifyXp('strength', 'Strength');
  facade.notifyXp('stat_agility', 'Agility');
  facade.notifyXp('alchemy', 'Alchemy');
  facade.notifyXp('str', 'Strength', 2);
  assert.deepEqual(
    sent.map(({ args }) => args[2]),
    [5, 5, 7, 2],
  );
});

test('the horse inspect comparison is on by default, turned off by a resource, on again next session', () => {
  const sent = [];
  const hud = new ResourceHud({ send: (message) => sent.push(message), emit: () => {} });
  hud.receiveHost({ ready: true });
  const facade = hud.facade('hud');
  assert.equal(facade.isHorseCompare(), true);
  facade.setHorseCompare(false);
  assert.deepEqual(sent.at(-1), { op: 'hudHorseCompare', enabled: false });
  assert.equal(facade.isHorseCompare(), false);
  assert.throws(() => facade.setHorseCompare('no'), TypeError);
  hud.receiveHost({ ready: false });
  hud.receiveHost({ ready: true });
  assert.deepEqual(sent.at(-1), { op: 'hudHorseCompare', enabled: true });
  assert.equal(facade.isHorseCompare(), true);
});

test('the shooting contest table goes to the movie in its own order, empty seats filled', () => {
  const { facade, sent } = setup();
  facade.showShootingContest({
    player: { name: 'Henry', arrows: 5 },
    opponents: [{ name: 'Jindra', arrows: 5 }],
  });
  facade.updateShootingContest({
    player: { arrows: 4, points: 9 },
    opponents: [{ arrows: 5, points: 0 }],
  });
  facade.showShootingHit(0.25, -0.5);
  facade.hideShootingContest();
  assert.deepEqual(
    sent.map(({ call, args }) => [call, args]),
    [
      ['showShootingContest', ['Henry', 'Jindra', '', 5, 5, 0, true]],
      ['updateShootingContest', [4, 5, 0, 9, 0, 0]],
      ['showShootingHit', [0.25, -0.5]],
      ['hideShootingContest', []],
    ],
  );
});

test('the shooting contest refuses a third opponent and a hit off the target', () => {
  const { facade, sent } = setup();
  const three = [{ name: 'a' }, { name: 'b' }, { name: 'c' }];
  assert.throws(
    () => facade.showShootingContest({ player: { name: 'Henry' }, opponents: three }),
    /at most 2/,
  );
  assert.throws(() => facade.showShootingHit(1.5, 0), /from -1 to 1/);
  assert.deepEqual(sent, []);
});

test('the horse race table takes the player, one opponent and the track', () => {
  const { facade, sent } = setup();
  facade.showHorseContest({
    player: { name: 'Henry', score: 0 },
    opponent: { name: 'Mutt', score: 2 },
    track: 'Kuttenberg',
  });
  facade.updateHorseContest(3);
  facade.hideHorseContest();
  assert.deepEqual(
    sent.map(({ call, args }) => [call, args]),
    [
      ['showHorseContest', ['Henry', 'Mutt', 0, 2, 'Kuttenberg']],
      ['updateHorseContest', [3]],
      ['hideHorseContest', []],
    ],
  );
});
