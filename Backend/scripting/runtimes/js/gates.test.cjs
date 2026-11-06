'use strict';

// Gate: the levels' gates, moved into their states for every player.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { Gates, GATES } = require('./gates/gates.cjs');
const { ResourceHost } = require('./host.cjs');

// A host with two ready players and one still loading, on a clock the test moves.
function fakeHost() {
  const output = [];
  const events = [];
  const clock = { ms: 1000 };
  const host = {
    players: new Map([
      [1, { id: 1, ready: true }],
      [2, { id: 2, ready: true }],
      [3, { id: 3, ready: false }],
    ]),
    bridge: { send: (e) => output.push(e) },
    emit: (...args) => {
      events.push(args);
      return true;
    },
  };
  const gates = new Gates(host, () => clock.ms);
  return { gates, Gate: gates.define(), output, events, clock };
}

test('the animated gates come first, in the client order, standing open', () => {
  const { Gate } = fakeHost();
  assert.deepEqual(
    Gate.all()
      .slice(0, 2)
      .map((gate) => gate.key),
    ['malesovDrawbridge', 'suchdolPortcullis'],
  );
  const bridge = Gate.find('malesovDrawbridge');
  assert.equal(bridge, Gate.all()[0]);
  assert.deepEqual(bridge.states, ['closed', 'open']);
  assert.equal(bridge.state, 'open');
  assert.equal(bridge.isOpen, true);
  assert.equal(bridge.moving, false);
  assert.equal(bridge.secondsTo('closed'), GATES[0].seconds[0]);
  assert.equal(Gate.find('nothing'), null);
});

test('the series of both levels follow, standing as the level has them', () => {
  const { Gate } = fakeHost();
  const trosky = Gate.find('ttro_trosky/lowerCastle/courtyard2/series_gate');
  assert.equal(trosky.level, 'trosecko');
  assert.deepEqual(trosky.states, ['closed', 'open']);
  assert.equal(trosky.state, null);
  assert.ok(Gate.all('kutnohorsko').length >= 4);
  assert.ok(Gate.all('trosecko').every((gate) => gate.level === 'trosecko'));
});

test('closing tells every ready player once and refuses while it moves', () => {
  const { Gate, output, events, clock } = fakeHost();
  const bridge = Gate.find('malesovDrawbridge');
  assert.equal(bridge.open(), false);
  assert.equal(bridge.close(), true);
  assert.deepEqual(output, [
    {
      op: 'gateState',
      id: 1,
      gate: 0,
      state: 0,
      elapsedMs: 0,
      barOut: false,
      barElapsedMs: 600000,
    },
    {
      op: 'gateState',
      id: 2,
      gate: 0,
      state: 0,
      elapsedMs: 0,
      barOut: false,
      barElapsedMs: 600000,
    },
  ]);
  assert.deepEqual(events, [['gateChanged', bridge, 'closed']]);
  assert.equal(bridge.isOpen, false);
  assert.equal(bridge.moving, true);
  clock.ms += 3000;
  assert.equal(bridge.open(), false);
  clock.ms += 5000;
  assert.equal(bridge.moving, false);
  assert.equal(bridge.open(), true);
  assert.equal(output.at(-1).state, 1);
});

test('toggle opens a shut gate and shuts an open one; openness follows the move', () => {
  const { Gate, clock } = fakeHost();
  const bridge = Gate.find('malesovDrawbridge');
  const seconds = bridge.secondsTo('closed');
  assert.equal(bridge.openness, 1, 'standing open as the level has it');
  assert.equal(bridge.toggle(), true);
  assert.equal(bridge.isOpen, false);
  assert.equal(bridge.openness, 1, 'it has just begun to close');
  clock.ms += (seconds * 1000) / 2;
  assert.ok(Math.abs(bridge.openness - 0.5) < 1e-9);
  assert.equal(bridge.toggle(), false, 'refused while it moves');
  clock.ms += (seconds * 1000) / 2;
  assert.equal(bridge.openness, 0);
  assert.equal(bridge.toggle(), true);
  clock.ms += bridge.secondsTo('open') * 1000;
  assert.equal(bridge.openness, 1);
  assert.equal(bridge.isOpen, true);
});

test('a gate moves for as long as its direction takes: the portcullis drops fast, winches slowly', () => {
  const { Gate, clock } = fakeHost();
  const bars = Gate.find('suchdolPortcullis');
  assert.equal(bars.close(), true);
  clock.ms += 7000;
  assert.equal(bars.moving, false);
  assert.equal(bars.open(), true);
  clock.ms += 10000;
  assert.equal(bars.moving, true);
  clock.ms += 7100;
  assert.equal(bars.moving, false);
  assert.equal(bars.isOpen, true);
});

test('a series moves into any of its states by name', () => {
  const { Gate, output, clock } = fakeHost();
  const jewish = Gate.find('kkut_kutnaHora/city/zidovska/series_gates');
  assert.deepEqual(jewish.states, ['afterPogrom', 'barricaded', 'open']);
  assert.equal(jewish.setState('nowhere'), false);
  assert.equal(jewish.setState('barricaded'), true);
  assert.equal(output[0].gate, jewish.index);
  assert.equal(output[0].state, 1);
  assert.equal(jewish.close(), false);
  clock.ms += 4100;
  assert.equal(jewish.open(), true);
  assert.equal(jewish.state, 'open');
});

test('a barred gate: the bar is taken out and put back while it stays shut', () => {
  const { Gate, output, events, clock } = fakeHost();
  const trosky = Gate.find('ttro_trosky/lowerCastle/courtyard2/series_gate');
  assert.equal(trosky.hasBar, true);
  assert.equal(trosky.barState, 'closed');
  assert.equal(trosky.barred, null);
  assert.equal(trosky.removeBar(), false);
  assert.equal(trosky.setState('closed'), true);
  assert.equal(trosky.barred, true);
  assert.equal(trosky.removeBar(), false);
  clock.ms += 4100;
  output.length = 0;
  assert.equal(trosky.removeBar(), true);
  assert.equal(trosky.barred, false);
  assert.equal(trosky.moving, true);
  assert.deepEqual(events.at(-1), ['gateBarChanged', trosky, false]);
  assert.equal(output[0].barOut, true);
  assert.equal(output[0].barElapsedMs, 0);
  assert.equal(trosky.open(), false);
  clock.ms += 2000;
  assert.equal(trosky.placeBar(), true);
  clock.ms += 2000;
  assert.equal(trosky.removeBar(), true);
  clock.ms += 2000;
  assert.equal(trosky.open(), true);
  assert.equal(trosky.barred, false);
  clock.ms += 4100;
  assert.equal(trosky.close({ bar: false }), true);
  assert.equal(trosky.barred, false);
  clock.ms += 4100;
  assert.equal(trosky.placeBar(), true);
  assert.equal(trosky.barred, true);
  assert.equal(Gate.find('malesovDrawbridge').hasBar, false);
  assert.equal(Gate.find('malesovDrawbridge').removeBar(), false);
});

test('a player whose world gets ready hears each set gate with how long ago it began', () => {
  const { gates, Gate, output, clock } = fakeHost();
  Gate.find('suchdolPortcullis').close();
  output.length = 0;
  clock.ms += 1500;
  gates.arrived({ id: 3 });
  assert.deepEqual(output, [
    {
      op: 'gateState',
      id: 3,
      gate: 1,
      state: 0,
      elapsedMs: 1500,
      barOut: false,
      barElapsedMs: 600000,
    },
  ]);
  output.length = 0;
  clock.ms += 1e9;
  gates.arrived({ id: 3 });
  assert.equal(output[0].elapsedMs, 600000);
});

test('resources get the Gate class', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-gate-'));
  const host = new ResourceHost(root, { send: () => {}, log: () => {} });
  await host.startAll();
  const { Gate } = host.makeAPI();
  assert.equal(Gate.find('suchdolPortcullis').isOpen, true);
});
