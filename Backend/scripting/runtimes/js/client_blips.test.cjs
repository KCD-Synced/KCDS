'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceBlips } = require('../../../../Client/resources/blips.cjs');
const { Vector3 } = require('./math.cjs');

function fixture() {
  const sent = [];
  const blips = new ResourceBlips({ send: (message) => sent.push(message), Vector3 });
  return { blips, sent };
}

test('client Blip.create: defaults, the show op and a handle', () => {
  const { blips, sent } = fixture();
  const Blip = blips.facade('demo');
  const blip = Blip.create({ position: { x: 1, y: 2, z: 3 } });
  assert.equal(blip.id, 1);
  assert.equal(blip.valid, true);
  assert.equal(blip.live, false);
  assert.equal(blip.type, 'GeneralPoi');
  assert.equal(blip.state, 0);
  assert.equal(blip.label, '');
  assert.equal(blip.compass, true);
  assert.equal(blip.map, true);
  assert.ok(blip.position instanceof Vector3);
  assert.deepEqual({ ...blip.position }, { x: 1, y: 2, z: 3 });
  assert.deepEqual(sent, [
    {
      op: 'clientBlipShow',
      blip: 1,
      type: 'GeneralPoi',
      markState: 0,
      label: '',
      compass: true,
      map: true,
      legend: true,
      position: { x: 1, y: 2, z: 3 },
    },
  ]);
  assert.equal(String(blip), 'Blip(1, GeneralPoi, 1, 2, 3)');
  assert.equal(Blip.getById(1), blip);
  assert.equal(new Blip(1), blip);
  assert.throws(() => new Blip(99), /Unknown blip/);
});

test('client Blip: changes draw it again; bad values throw', () => {
  const { blips, sent } = fixture();
  const Blip = blips.facade('demo');
  const blip = Blip.create({ position: { x: 0, y: 0, z: 0 }, type: 'Shop', label: 'Stall' });
  blip.position = { x: 5, y: 6, z: 7 };
  blip.type = 'Blacksmith';
  blip.state = 1;
  blip.label = 'x'.repeat(80);
  blip.compass = false;
  blip.map = false;
  blip.legend = false;
  assert.equal(blip.legend, false);
  assert.equal(sent.length, 8);
  assert.deepEqual(sent.at(-1), {
    op: 'clientBlipShow',
    blip: 1,
    type: 'Blacksmith',
    markState: 1,
    label: 'x'.repeat(64),
    compass: false,
    map: false,
    legend: false,
    position: { x: 5, y: 6, z: 7 },
  });
  assert.throws(() => (blip.legend = 'no'), /legend/);
  assert.throws(() => Blip.create({ position: { x: 0, y: 0, z: 0 }, legend: 0 }), /legend/);
  assert.throws(() => (blip.type = 'Castle'), /type/);
  assert.throws(() => (blip.state = 1.5), /state/);
  assert.throws(() => (blip.compass = 1), /compass/);
  assert.throws(() => (blip.position = { x: Number.NaN, y: 0, z: 0 }), /position/);
  assert.throws(() => Blip.create({ type: 'Shop' }), /position/);
  assert.throws(() => Blip.create({ position: { x: 0, y: 0, z: 0 }, type: 'Nope' }), /type/);
  assert.equal(sent.length, 8);
  // markState is its name now, as on the server; state still answers for 0.3.4.
  blip.markState = 2;
  assert.equal(blip.state, 2);
  assert.equal(blip.markState, 2);
  assert.throws(() => (blip.markState = 'x'), /state/);
  assert.equal(Blip.create({ position: { x: 0, y: 0, z: 0 }, markState: 3 }).markState, 3);
});

test('client Blip: live from the game client, remove, removeAll and a stopped resource', () => {
  const { blips, sent } = fixture();
  const Blip = blips.facade('demo');
  const Other = blips.facade('other');
  const a = Blip.create({ position: { x: 0, y: 0, z: 0 } });
  const b = Other.create({ position: { x: 1, y: 0, z: 0 } });
  blips.receiveLive({ blip: a.id, live: true });
  assert.equal(a.live, true);
  assert.deepEqual(
    Blip.all().map((blip) => blip.id),
    [1, 2],
  );
  assert.equal(a.remove(), true);
  assert.equal(a.remove(), false);
  assert.deepEqual(sent.at(-1), { op: 'clientBlipHide', blip: 1 });
  // A removed blip keeps reading, with defaults.
  assert.equal(a.valid, false);
  assert.equal(a.live, false);
  assert.equal(a.type, 'GeneralPoi');
  assert.deepEqual({ ...a.position }, { x: 0, y: 0, z: 0 });
  assert.equal(String(a), 'Blip(1, removed)');
  a.label = 'ignored';
  assert.equal(sent.at(-1).op, 'clientBlipHide');
  const c = Blip.create({ position: { x: 2, y: 0, z: 0 } });
  assert.equal(c.id, 3);
  blips.removeOwner('other');
  assert.equal(b.valid, false);
  assert.equal(c.valid, true);
  Blip.removeAll();
  assert.deepEqual(Blip.all(), []);
});

test('client Blip.create: null at the limit of 256', () => {
  const { blips } = fixture();
  const Blip = blips.facade('demo');
  for (let i = 0; i < 256; i += 1) {
    assert.ok(Blip.create({ position: { x: i, y: 0, z: 0 } }));
  }
  assert.equal(Blip.create({ position: { x: 0, y: 0, z: 0 } }), null);
});

test('client Blip image: a shipped .dds is the map icon (KCDS)', () => {
  const sent = [];
  const shipped = [];
  const blips = new ResourceBlips({
    send: (message) => sent.push(message),
    Vector3,
    shipImage: (resource, path) => {
      if (path !== 'images/logo.dds') {
        throw new Error('Blip image: this resource does not ship it');
      }
      shipped.push([resource, path]);
      return 'kcds_demo_images_logo';
    },
  });
  const Blip = blips.facade('demo');
  const blip = Blip.create({ position: { x: 1, y: 2, z: 3 }, image: 'images/logo.dds' });
  assert.deepEqual(shipped, [['demo', 'images/logo.dds']]);
  assert.equal(blip.image, 'images/logo.dds');
  assert.equal(sent.at(-1).image, 'kcds_demo_images_logo');
  blip.image = '';
  assert.equal(blip.image, '');
  assert.equal('image' in sent.at(-1), false);
  assert.throws(() => (blip.image = 'images/missing.dds'), /does not ship/);
  assert.throws(() => Blip.create({ position: { x: 0, y: 0, z: 0 }, image: 5 }), /image/);
  assert.equal(Blip.all().length, 1);
});
