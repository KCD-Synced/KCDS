'use strict';

// The client's Assets global: a resource's stream/ files sent and mounted once, and
// Assets.has answering from what the game client mounted.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceAssets } = require('../../../../Client/resources/assets.cjs');
const { DiskFile } = require('../../../../Client/resources/bundle_cache.cjs');

test('client Assets: stream files go to the game once and has() answers what it mounted', () => {
  const sent = [];
  const files = [];
  const assets = new ResourceAssets({
    send: (message) => sent.push(message),
    sendFile: (resource, file) => files.push(`${resource}/${file}`),
    log: () => {},
  });
  const read = (file) => (file.endsWith('.missing') ? null : new Uint8Array([1]));
  const shipped = ['client/main.js', 'stream/objects/kcds/mode/chair.cgf', 'stream/a.missing'];
  assets.ship('mode', shipped, read);
  assets.ship('mode', shipped, read);
  assert.deepEqual(files, ['mode/stream/objects/kcds/mode/chair.cgf']);
  assert.deepEqual(sent, [
    {
      op: 'assetsMount',
      resource: 'mode',
      files: ['stream/objects/kcds/mode/chair.cgf', 'stream/a.missing'],
    },
  ]);
  const api = assets.facade();
  assert.equal(api.has('objects/kcds/mode/chair.cgf'), false);
  assets.receiveMounted({
    op: 'assetsMounted',
    resource: 'mode',
    paths: ['objects/kcds/mode/Chair.cgf'],
  });
  assert.equal(api.has('Objects/kcds/mode/chair.cgf'), true);
  assert.equal(api.has('objects/kcds/mode/table.cgf'), false);
  assets.ship('quiet', ['client/main.js'], read);
  assert.equal(sent.length, 1);
  assert.throws(() => api.has(5), /takes a path/);
});

test('client Assets: replace/ files ship and mount as replacements, apart from the stream', () => {
  const sent = [];
  const assets = new ResourceAssets({
    send: (m) => sent.push(m),
    sendFile: () => {},
    log: () => {},
  });
  assets.ship(
    'mode',
    ['replace/objects/a/b.cgf', 'stream/objects/kcds/mode/c.cgf'],
    () => new Uint8Array([1]),
  );
  assert.deepEqual(sent, [
    { op: 'assetsMount', resource: 'mode', files: ['stream/objects/kcds/mode/c.cgf'] },
    { op: 'assetsMount', resource: 'mode', files: ['replace/objects/a/b.cgf'], replace: true },
  ]);
});

test('client Assets: a custom map a resource brings goes to the game in pieces, map.json last, once', () => {
  const sent = [];
  const assets = new ResourceAssets({
    send: (message) => sent.push(message),
    sendFile() {},
    log() {},
  });
  const pack = new Uint8Array(30000).fill(7);
  const about = new TextEncoder().encode('{"size": 1024}');
  const read = (file) => (file.endsWith('.pak') ? pack : about);
  const files = [
    'maps/kcds_map_prague/map.json',
    'maps/kcds_map_prague/level.pak',
    'maps/kcds_map_Bad/level.pak',
    'maps/kcds_map_prague/other.pak',
    'maps/kcds_map_prague/recast.pak',
    'server.js',
  ];
  assets.shipMaps('architect_kcds_map_prague', files, read);
  assets.shipMaps('architect_kcds_map_prague', files, read);
  assert.deepEqual(
    sent.map((message) => [message.op, message.map, message.name, message.offset, message.total]),
    [
      ['customMapFile', 'kcds_map_prague', 'level.pak', 0, 30000],
      ['customMapFile', 'kcds_map_prague', 'level.pak', 24576, 30000],
      ['customMapFile', 'kcds_map_prague', 'recast.pak', 0, 30000],
      ['customMapFile', 'kcds_map_prague', 'recast.pak', 24576, 30000],
      ['customMapFile', 'kcds_map_prague', 'map.json', 0, about.length],
    ],
  );
  const joined = sent
    .filter((message) => message.name === 'level.pak')
    .map((message) => atob(message.data).length)
    .reduce((a, b) => a + b, 0);
  assert.equal(joined, 30000);
});

test('client Assets: a big map file in the game cache goes by its hash', () => {
  const sent = [];
  const assets = new ResourceAssets({
    send: (message) => sent.push(message),
    sendFile() {},
    log() {},
  });
  const pack = new DiskFile('a'.repeat(64), 600 * 1024 * 1024);
  const about = new TextEncoder().encode('{"size": 1024}');
  const read = (file) => (file.endsWith('.pak') ? pack : about);
  assets.shipMaps('maps', ['maps/kcds_map_big/map.json', 'maps/kcds_map_big/level.pak'], read);
  assert.deepEqual(sent[0], {
    op: 'customMapFile',
    resource: 'maps',
    map: 'kcds_map_big',
    name: 'level.pak',
    cached: 'a'.repeat(64),
  });
  assert.equal(sent[1].name, 'map.json');
  assert.equal(sent[1].total, about.length);
});
