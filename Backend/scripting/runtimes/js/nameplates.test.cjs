'use strict';

// A player's nameplate as the server sets it: Player.setNameplate* and their readers,
// the field every client draws, and a tag set before the body was in the world.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');
const { cleanText } = require('./nameplates/player_nameplates.cjs');

function fields(sent) {
  return sent.filter((message) => message.op === 'playerNameplate').map((m) => [m.id, m.field]);
}

test('a nameplate is shown, white, with health and the name until the server changes it', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const player = report(3);
  assert.equal(player.isNameplateVisible(), true);
  assert.equal(player.isNameplateHealthVisible(), true);
  assert.equal(player.getNameplateText(), '');
  assert.equal(player.getNameplateColor(), 0xffffffff);
  player.setNameplateText('[Guard] Hans');
  player.setNameplateColor(0xffc9a227);
  player.setNameplateHealthVisible(false);
  player.setNameplateVisible(false);
  assert.deepEqual(fields(sent), [
    [3, '11;FFFFFFFF;[Guard] Hans'],
    [3, '11;FFC9A227;[Guard] Hans'],
    [3, '10;FFC9A227;[Guard] Hans'],
    [3, '00;FFC9A227;[Guard] Hans'],
  ]);
  assert.equal(player.isNameplateVisible(), false);
  assert.equal(player.getNameplateText(), '[Guard] Hans');
  assert.equal(player.getNameplateColor(), 0xffc9a227);
  // Back to the default tag: the field is empty again.
  sent.length = 0;
  player.setNameplateVisible(true);
  player.setNameplateHealthVisible(true);
  player.setNameplateColor(0xffffffff);
  player.setNameplateText();
  assert.deepEqual(fields(sent).at(-1), [3, '']);
  assert.equal(host.nameplates.tags.size, 0);
});

test('nameplate arguments are checked and the text cleaned and cut to 64 bytes', async (t) => {
  const { host, report } = serverFixture(t);
  await host.startAll();
  const player = report(4);
  assert.throws(() => player.setNameplateVisible(1), /takes a boolean/);
  assert.throws(() => player.setNameplateHealthVisible('no'), /takes a boolean/);
  assert.throws(() => player.setNameplateText(5), /takes a text/);
  assert.throws(() => player.setNameplateColor(-1), /0xAARRGGBB/);
  assert.throws(() => player.setNameplateColor(0x100000000), /0xAARRGGBB/);
  assert.equal(cleanText('a\nb\tc'), 'abc');
  assert.equal(cleanText('ř'.repeat(40)), 'ř'.repeat(32));
  player.setNameplateText('x'.repeat(70));
  assert.equal(player.getNameplateText(), 'x'.repeat(64));
});

test('a tag set early is sent again when the player gets ready, and forgotten when they leave', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const player = report(6);
  player.setNameplateColor(0xff00ff00);
  sent.length = 0;
  host.nameplates.arrived(player);
  assert.deepEqual(fields(sent), [[6, '11;FF00FF00;']]);
  host.nameplates.left(player);
  assert.equal(host.nameplates.tags.size, 0);
});

test('the tag mode shows the name, the health bar, both or neither on every screen', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const player = report(5);
  assert.equal(player.getNameplateMode(), 'both');
  assert.equal(player.isNameplateNameVisible(), true);
  player.setNameplateMode('health');
  player.setNameplateMode('name');
  player.setNameplateMode('none');
  player.setNameplateMode('both');
  assert.deepEqual(fields(sent), [
    [5, '110;FFFFFFFF;'],
    [5, '10;FFFFFFFF;'],
    [5, '100;FFFFFFFF;'],
    [5, ''],
  ]);
  player.setNameplateNameVisible(false);
  assert.equal(player.getNameplateMode(), 'health');
  assert.equal(player.isNameplateNameVisible(), false);
  assert.throws(() => player.setNameplateMode('hp'), /setNameplateMode takes one of/);
  assert.throws(() => player.setNameplateNameVisible('no'), /takes a boolean/);
});

test('a title above the name goes with the tag as its own line', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const player = report(6);
  assert.equal(player.getNameplateTitle(), '');
  player.setNameplateTitle('Town guard');
  player.setNameplateText('Hans');
  assert.deepEqual(fields(sent), [
    [6, '11;FFFFFFFF;\n00000000;0;Town guard'],
    [6, '11;FFFFFFFF;Hans\n00000000;0;Town guard'],
  ]);
  assert.equal(player.getNameplateTitle(), 'Town guard');
  assert.equal(player.getNameplateTitleColor(), 0);
  // A title in a colour and a size of its own.
  player.setNameplateTitle('Admin', 0xffe0402a, 30);
  assert.equal(fields(sent).at(-1)[1], '11;FFFFFFFF;Hans\nFFE0402A;30;Admin');
  assert.equal(player.getNameplateTitleColor(), 0xffe0402a);
  assert.equal(player.getNameplateTitleSize(), 30);
  player.setNameplateTitle('Smith\nof Trosky');
  assert.equal(player.getNameplateTitle(), 'Smithof Trosky');
  assert.equal(player.getNameplateTitleColor(), 0);
  assert.equal(player.getNameplateTitleSize(), 0);
  player.setNameplateTitle();
  assert.equal(fields(sent).at(-1)[1], '11;FFFFFFFF;Hans');
  assert.throws(() => player.setNameplateTitle(5), /takes a text/);
  assert.throws(() => player.setNameplateTitle('Admin', -1), /0xAARRGGBB/);
  assert.throws(() => player.setNameplateTitle('Admin', 0, 49), /8 to 48 pixels/);
  assert.throws(() => player.setNameplateTitle('Admin', 0, 7.5), /8 to 48 pixels/);
});

test("a player's nameplate goes to the clients' directories, and the plain one again", async (t) => {
  const { host, report } = serverFixture(t);
  const published = [];
  host.nameplateChanged = (id, nameplate) => published.push([id, nameplate && { ...nameplate }]);
  await host.startAll();
  const player = report(3);
  player.setNameplateText('[Guard] Hans');
  player.setNameplateTitle('Town guard');
  player.setNameplateText('');
  player.setNameplateTitle('');
  assert.deepEqual(
    published.map(([id, nameplate]) => [
      id,
      nameplate && nameplate.text,
      nameplate && nameplate.title,
    ]),
    [
      [3, '[Guard] Hans', ''],
      [3, '[Guard] Hans', 'Town guard'],
      [3, '', 'Town guard'],
      [3, null, null],
    ],
  );
  assert.equal(published[0][1].visible, true);
  assert.equal(published[0][1].color, 0xffffffff);
});

// Player.showOverheadText: a line above the tag for the players near the speaker,
// in their dimension, whose client resources run; a reserved event their game draws.
test('an overhead text goes to the players near the speaker', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const speaker = report(3, { position: { x: 0, y: 0, z: 0 } });
  report(4, { position: { x: 12, y: 0, z: 0 } });
  report(5, { position: { x: 40, y: 0, z: 0 } });
  report(6, { position: { x: 1, y: 0, z: 0 }, dimension: 7 });
  report(8, { position: { x: 2, y: 0, z: 0 } });
  for (const id of [3, 4, 5, 6]) {
    host.clientResourcesStarted(id);
  }
  const reached = speaker.showOverheadText('* draws his sword', 5000, 0xffc9a227);
  const events = sent.filter(
    (message) => message.op === 'event' && message.name === 'kcds:overheadText',
  );
  // 4 is near; 5 is too far, 6 is in another world, 8 has no client resources yet.
  assert.equal(reached, 2);
  assert.deepEqual(
    events.map((event) => event.player),
    [3, 4],
  );
  assert.equal(events[0].name, 'kcds:overheadText');
  assert.deepEqual(JSON.parse(events[0].payload), {
    entity: 3,
    text: '* draws his sword',
    durationMs: 5000,
    color: 0xffc9a227,
  });
  assert.equal(speaker.showOverheadText('far', 1000, 0, 50), 3);
  assert.throws(() => speaker.showOverheadText(5), TypeError);
  assert.throws(() => speaker.showOverheadText('x'.repeat(300)), RangeError);
  assert.throws(() => speaker.showOverheadText('x', 0), RangeError);
  assert.throws(() => speaker.showOverheadText('x', 1000, -1), TypeError);
  assert.throws(() => speaker.showOverheadText('x', 1000, 0, 501), RangeError);
});

test('every client hears the player count and places for its Discord presence', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  host.statusRoutes.configure({ ...host.statusRoutes.settings, maxPeers: 32 });
  report(3);
  report(4);
  host.clientResourcesStarted(3);
  host.clientResourcesStarted(4);
  const counts = () =>
    sent
      .filter((message) => message.op === 'event' && message.name === 'kcds:playerCount')
      .map((message) => [message.player, JSON.parse(message.payload)]);
  assert.deepEqual(counts().slice(-2), [
    [3, { players: 2, max: 32 }],
    [4, { players: 2, max: 32 }],
  ]);
  host.disconnect(4);
  assert.deepEqual(counts().at(-1), [3, { players: 1, max: 32 }]);
});

test('Player.brand goes to every client, 0, 1 or 2 only', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const player = report(3);
  assert.equal(player.brand, 0);
  player.brand = 1;
  assert.equal(player.brand, 1);
  assert.deepEqual(sent.filter((message) => message.op === 'playerBrand').pop(), {
    op: 'playerBrand',
    id: 3,
    brand: 1,
  });
  player.brand = 0;
  assert.equal(sent.filter((message) => message.op === 'playerBrand').pop().brand, 0);
  for (const wrong of [3, -1, 0.5, '1', null]) {
    assert.throws(() => {
      player.brand = wrong;
    }, TypeError);
  }
  assert.equal(player.brand, 0);
});
