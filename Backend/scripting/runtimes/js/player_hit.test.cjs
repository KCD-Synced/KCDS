'use strict';

// playerHit: a blow from another player, held by the hit player's client until the
// server rules on it.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { playerHitOf } = require('./combat/player_hit.cjs');

async function fixture(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-hit-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'referee');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'referee', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  await host.startAll();
  for (const id of [7, 8, 9]) {
    host.updatePlayer({
      id,
      ready: true,
      position: { x: 0, y: 0, z: 0 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
    });
  }
  const verdicts = () => {
    return output
      .filter((e) => e.op === 'hitVerdict')
      .map((e) => [e.id, e.request, e.ruling, Math.round(e.value * 100) / 100]);
  };
  const chat = () => output.filter((e) => e.op === 'chat').map((e) => e.text);
  return { host, output, verdicts, chat };
}

const WEAPON = '46db8ef200cca9e319099351ec86bf8c';

test('playerHit shows the blow, and the verdict is what the handlers ruled', async () => {
  const { host, verdicts, chat } = await fixture(`
    kcds.core.Events.on('playerHit', (player, attacker, hit) => {
      kcds.core.Chat.sendToAll([player.id, attacker.id, hit.damage, hit.engineDamage, hit.priced,
        hit.weapon, hit.stamina, hit.reason, hit.source, hit.bodyPart, hit.blocked].join(' '));
      if (attacker.id === 9) return false;
      if (player.id === 8) hit.damage = 0;
      if (attacker.id === 7 && player.id === 9) hit.modifiers.health = 0.5;
    });
  `);
  const blow = { damage: 13.66, stamina: 40, weapon: WEAPON };
  assert.equal(host.playerHit({ id: 7, request: 1, attacker: 8, ...blow }), true);
  assert.equal(host.playerHit({ id: 7, request: 2, attacker: 9, ...blow }), true);
  assert.equal(host.playerHit({ id: 8, request: 3, attacker: 7, ...blow }), true);
  assert.equal(host.playerHit({ id: 9, request: 4, attacker: 7, ...blow }), true);
  // From a player who is gone: taken as the game worked it out, unasked.
  assert.equal(host.playerHit({ id: 7, request: 5, attacker: 42, ...blow }), true);
  // On a player who is gone: no answer.
  assert.equal(host.playerHit({ id: 42, request: 6, attacker: 7, ...blow }), false);
  // Refused 0, as the game works it out 1, set 2, scaled 3.
  assert.deepEqual(verdicts(), [
    [7, 1, 1, 0],
    [7, 2, 0, 0],
    [8, 3, 2, 0],
    [9, 4, 3, 0.5],
    [7, 5, 1, 0],
  ]);
  assert.equal(chat()[0], `7 8 13.66 13.66 false ${WEAPON} 40 combat melee  false`);
  assert.equal(chat().length, 4);
});

test('PlayerHit: damage set stands, modifiers scale the health, bad values refused', () => {
  const hit = playerHitOf({ damage: 20, stamina: 5 });
  assert.equal(hit.weapon, null);
  assert.deepEqual(hit.ruling(), { ruling: 1, value: 0 });
  hit.modifiers.health = 2;
  assert.deepEqual(hit.ruling(), { ruling: 3, value: 2 });
  hit.damage = 3;
  assert.deepEqual(hit.ruling(), { ruling: 2, value: 3 });
  assert.throws(() => {
    hit.damage = -1;
  }, RangeError);
  assert.throws(() => {
    hit.damage = Number.NaN;
  }, RangeError);
  const scaled = playerHitOf({ damage: 10, stamina: 0 });
  scaled.modifiers.health = -1;
  assert.deepEqual(scaled.ruling(), { ruling: 1, value: 0 });
  assert.equal(playerHitOf({ damage: -5, stamina: Number.NaN }).engineDamage, 0);
});

const PRICE = Object.freeze({
  attack: [0, 42.5, 3.25],
  armor: [12, 18.5, 9],
  blockDefense: 21,
  bodyPartCoefficient: 1.5,
  subpart: 24,
  blocked: true,
  perfectBlock: false,
  blockBroken: true,
});

test("a priced blow: the game's numbers, and modifiers priced again by the game", async () => {
  const { host, output, chat } = await fixture(`
    kcds.core.Events.on('playerHit', (player, attacker, hit) => {
      kcds.core.Chat.sendToAll([hit.priced, hit.damage, hit.engineDamage, hit.attack.slash, hit.armor.stab,
        hit.blockDefense, hit.bodyPartCoefficient, hit.bodyPart, hit.blocked, hit.perfectBlock,
        hit.blockBroken].join(' '));
      if (player.id === 8) hit.modifiers.attack = 2;
      if (player.id === 9) hit.modifiers.health = 0.5;
    });
  `);
  const blow = { damage: 13.66, stamina: 40, weapon: WEAPON, price: PRICE };
  host.playerHit({ id: 7, request: 1, attacker: 9, ...blow });
  host.playerHit({ id: 8, request: 2, attacker: 9, ...blow });
  host.playerHit({ id: 9, request: 3, attacker: 7, ...blow });
  assert.equal(chat()[0], 'true 13.66 13.66 42.5 12 21 1.5 head true false true');
  const verdicts = output.filter((e) => e.op === 'hitVerdict');
  assert.deepEqual(verdicts[0], { op: 'hitVerdict', id: 7, request: 1, ruling: 1, value: 0 });
  // The attack scaled: priced again (4) with every modifier.
  assert.deepEqual(verdicts[1], {
    op: 'hitVerdict',
    id: 8,
    request: 2,
    ruling: 4,
    value: 0,
    attack: 2,
    defense: 1,
    block: 1,
    health: 1,
  });
  // Only the health scaled: no need to price it again.
  assert.deepEqual(verdicts[2], { op: 'hitVerdict', id: 9, request: 3, ruling: 3, value: 0.5 });
});

test('PlayerHit: a priced blow ignores bad modifiers, an arrow is never priced', () => {
  const hit = playerHitOf({ damage: 20, stamina: 5, price: PRICE });
  assert.equal(hit.priced, true);
  assert.deepEqual(hit.attack, { stab: 0, slash: 42.5, smash: 3.25 });
  hit.modifiers.defense = -1;
  hit.modifiers.block = Number.NaN;
  assert.deepEqual(hit.ruling(), { ruling: 1, value: 0 });
  hit.modifiers.block = 0;
  assert.deepEqual(hit.ruling(), {
    ruling: 4,
    value: 0,
    attack: 1,
    defense: 1,
    block: 0,
    health: 1,
  });
  hit.damage = 4;
  assert.deepEqual(hit.ruling(), { ruling: 2, value: 4 });
  const arrow = playerHitOf({ damage: 9, stamina: 0, missile: true, price: PRICE });
  assert.equal(arrow.priced, false);
  assert.equal(arrow.bodyPart, null);
  // Not priced: the attack modifier changes nothing.
  const unpriced = playerHitOf({ damage: 9, stamina: 0 });
  unpriced.modifiers.attack = 3;
  assert.deepEqual(unpriced.ruling(), { ruling: 1, value: 0 });
});

test("an arrow's hit: the hit player's own health, source missile, modifiers ignored", async () => {
  const BOW = '0f2bd31b30afbd3d5c6a1e8f61a7b0c2';
  const { host, verdicts, chat } = await fixture(`
    kcds.core.Events.on('playerHit', (player, attacker, hit) => {
      kcds.core.Chat.sendToAll([hit.source, hit.damage, hit.engineDamage, hit.priced, hit.weapon].join(' '));
      hit.modifiers.health = 0.5;
      if (player.id === 9) hit.damage = 2;
    });
  `);
  host.updatePlayer({ id: 8, hands: '|' + BOW.toUpperCase() });
  host.playerHit({ id: 7, request: 1, attacker: 8, damage: 16.2, stamina: 0, missile: true });
  host.playerHit({ id: 9, request: 2, attacker: 8, damage: 16.2, stamina: 0, missile: true });
  // Native for the first (the modifiers do not apply to an arrow), set for the second.
  assert.deepEqual(verdicts(), [
    [7, 1, 1, 0],
    [9, 2, 2, 2],
  ]);
  assert.match(chat()[0], /^missile 16\.2 16\.2 false/);
  const missile = playerHitOf({ damage: 9, stamina: 0, missile: true });
  missile.modifiers.health = 3;
  assert.deepEqual(missile.ruling(), { ruling: 1, value: 0 });
});

test('blows wait for a ruling only while a resource handles playerHit', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-hit-rules-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'referee');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'referee', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), "kcds.core.Events.on('playerDamage', () => {});");
  await host.startAll();
  const rules = () => output.filter((e) => e.op === 'hitRules').map((e) => e.enabled);
  host.pumpHitRules();
  host.pumpHitRules();
  assert.deepEqual(rules(), [false]);
  fs.writeFileSync(path.join(dir, 'index.js'), "kcds.core.Events.on('playerHit', () => {});");
  await host.stop('referee');
  await host.start('referee');
  host.pumpHitRules();
  assert.deepEqual(rules(), [false, true]);
  await host.stop('referee');
  host.pumpHitRules();
  assert.deepEqual(rules(), [false, true, false]);
});
