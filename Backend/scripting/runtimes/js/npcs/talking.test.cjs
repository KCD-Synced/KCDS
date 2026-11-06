'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const { ResourceHost } = require('../host.cjs');

test('Npc.setSpeaking goes to the clients; off with false, refused for a bad style', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-npc-talking-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  await host.startAll();
  const { Npc } = host.api;
  const npc = Npc.create({ position: { x: 1, y: 2, z: 3 } });
  const talks = () => output.filter((e) => e.op === 'npcTalking');
  assert.equal(npc.setSpeaking(true), true);
  assert.deepEqual(talks().at(-1), { op: 'npcTalking', id: npc.id, style: 'neutral' });
  assert.equal(npc.setSpeaking(true, 'drunk'), true);
  assert.equal(talks().at(-1).style, 'drunk');
  assert.equal(npc.setSpeaking(false), true);
  assert.equal(talks().at(-1).style, '');
  const sent = talks().length;
  assert.throws(() => npc.setSpeaking('yes'), TypeError);
  assert.throws(() => npc.setSpeaking(true, 'shout'), TypeError);
  assert.equal(talks().length, sent);

  const faces = () => output.filter((e) => e.op === 'npcFace');
  assert.equal(npc.setExpression('FE_DialogueIdle', { tags: 'angry' }), true);
  assert.deepEqual(faces().at(-1), { op: 'npcFace', id: npc.id, face: 'FE_DialogueIdle|angry' });
  assert.equal(npc.setExpression(''), true);
  assert.equal(faces().at(-1).face, '');
  assert.throws(() => npc.setExpression(3), TypeError);
  assert.throws(() => npc.setExpression('FE_Default', 'angry'), TypeError);
});

test('Npc.lootOf lists the game preset of an animal, null for anything else', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-npc-loot-'));
  const host = new ResourceHost(root, { send: () => {}, log: () => {} });
  await host.startAll();
  const { Npc } = host.api;
  const boar = Npc.create({
    position: { x: 1, y: 2, z: 3 },
    class: 'Boar',
    soul: '9988bac0-35be-4e4c-b1af-2fc16c4168c2',
  });
  const loot = boar.lootOf();
  const meat = loot.find((row) => row.name === 'boarMeat');
  assert.deepEqual(meat, {
    item: 'ba4b74ad-f8f1-427e-905b-1bc8c27163e7',
    name: 'boarMeat',
    amount: 45,
    fraction: 1,
    variation: 0,
    food: true,
    harvester: false,
  });
  const teeth = loot.find((row) => row.name === 'special_teethBoar');
  assert.equal(teeth.food, false);
  assert.equal(teeth.harvester, true);
  const person = Npc.create({ position: { x: 1, y: 2, z: 3 } });
  assert.equal(person.lootOf(), null);
});
