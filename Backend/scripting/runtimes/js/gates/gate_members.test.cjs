'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const { ResourceHost } = require('../host.cjs');

async function hostWith() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-gates-'));
  const host = new ResourceHost(root, { send: () => {}, log: () => {} });
  await host.startAll();
  return host;
}

test('a gate names itself, its kind, its cycle and its durations', async () => {
  const host = await hostWith();
  const { Gate } = host.api;
  const portcullis = Gate.find('suchdolPortcullis');
  assert.equal(portcullis.kind, 'portcullis');
  assert.equal(portcullis.guid, '4b5d68eb986fda15');
  assert.equal(Gate.find('0x4B5D68EB986FDA15'), portcullis);
  assert.equal(Gate.getById(portcullis.id), portcullis);
  assert.equal(Gate.getById(9999), null);
  assert.equal(portcullis.openingTime, 17.033);
  assert.equal(portcullis.closingTime, 6.967);
  assert.equal(Gate.find('malesovDrawbridge').kind, 'drawbridge');
  const series = Gate.all('trosecko')[0];
  assert.equal(series.kind, 'gate');
  assert.equal(series.openingTime, 4);
  assert.equal(series.cycle, 'closed');
  series.open();
  assert.equal(series.cycle, 'opening');
});

test('Gate.nearest finds by place, and refuses what it cannot use', async () => {
  const host = await hostWith();
  const { Gate } = host.api;
  const series = Gate.all('trosecko')[0];
  const at = series.position;
  assert.equal(Gate.nearest({ x: at.x + 5, y: at.y, z: at.z }, 50), series);
  assert.equal(Gate.nearest({ x: 1, y: 1, z: 1 }, 5), null);
  assert.throws(() => Gate.nearest(null, 5), TypeError);
  assert.throws(() => Gate.nearest(at, -1), TypeError);
});

test('Npc.placements lists the level people, and nothing without a level', async () => {
  const host = await hostWith();
  const { Npc } = host.api;
  host.areas.level = 'trosecko';
  const rows = Npc.placements();
  assert.ok(rows.length > 500);
  const row = rows.find((placement) => placement.name === 'ttkc_gerta');
  assert.equal(row.actorClass, 'NPC_Female');
  assert.equal(row.level, 'trosecko');
  assert.ok(row.soul.length > 30);
  row.position.x = -1;
  assert.notEqual(
    Npc.placements().find((placement) => placement.name === 'ttkc_gerta').position.x,
    -1,
  );
  host.areas.level = 'nowhere';
  assert.deepEqual(Npc.placements(), []);
});
