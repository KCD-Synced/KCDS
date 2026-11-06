'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');

// Carrying a knocked-out player's body.

function hostWith(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-bodycarry-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'gamemode');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'gamemode', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  return { host, output };
}

function player(host, id) {
  return host.updatePlayer({
    id,
    nickname: 'P' + id,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
}

const carries = (output) => output.filter((e) => e.op === 'bodyCarry').map((e) => ({ ...e }));
const said = (output) => output.filter((e) => e.op === 'chat').map((e) => e.text);

test('a carry of a knocked-out body is asked, shown to the others and ended', async () => {
  const { host, output } = hostWith(`
    kcds.core.Events.on('playerCarrying', (c, p) => kcds.core.Chat.sendToAll('carrying ' + c.id + ' ' + p.id));
    kcds.core.Events.on('playerCarry', (c, p) => kcds.core.Chat.sendToAll('carry ' + c.id + ' ' + p.id));
    kcds.core.Events.on('playerPutDown', (c, p) => kcds.core.Chat.sendToAll('putdown ' + c.id + ' ' + p.id));
  `);
  await host.startAll();
  const carrier = player(host, 7);
  const body = player(host, 8);
  player(host, 9);
  // Not knocked out: refused.
  host.bodyCarries.report(carrier, { kind: 'started', carried: 8 });
  assert.deepEqual(carries(output).at(-1), {
    op: 'bodyCarry',
    id: 7,
    kind: 'refuse',
    carrier: 7,
    carried: 8,
  });
  host.playerDied(8);
  host.bodyCarries.report(carrier, { kind: 'started', carried: 8 });
  assert.equal(carrier.carrying, body);
  assert.equal(body.heldBy, carrier);
  assert.deepEqual(
    carries(output)
      .filter((e) => e.kind === 'carry')
      .map((e) => e.id),
    [8, 9],
  );
  assert.equal(carrier.putDown(), true);
  assert.equal(carries(output).at(-1).kind, 'putDown');
  host.bodyCarries.report(carrier, { kind: 'ended', carried: 8 });
  assert.equal(carrier.carrying, null);
  assert.equal(carrier.putDown(), false);
  assert.deepEqual(
    said(output).filter((t) => /^(carrying|carry|putdown) /.test(t)),
    ['carrying 7 8', 'carry 7 8', 'putdown 7 8'],
  );
});

test('a gamemode refuses a carry; a revive or a leave ends one', async () => {
  const { host, output } = hostWith(`
    kcds.core.Events.on('playerCarrying', (c) => c.id !== 10);
  `);
  await host.startAll();
  const carrier = player(host, 7);
  const refused = player(host, 10);
  player(host, 8);
  host.playerDied(8);
  host.bodyCarries.report(refused, { kind: 'started', carried: 8 });
  assert.equal(refused.carrying, null);
  assert.equal(carries(output).at(-1).kind, 'refuse');
  host.bodyCarries.report(carrier, { kind: 'started', carried: 8 });
  host.players.get(8).revive();
  assert.equal(carrier.carrying, null);
  assert.ok(carries(output).some((e) => e.kind === 'putDown' && e.id === 7));
  host.playerDied(8);
  host.bodyCarries.report(carrier, { kind: 'started', carried: 8 });
  host.bodyCarries.left(carrier);
  assert.equal(host.players.get(8).heldBy, null);
  assert.equal(carries(output).at(-1).kind, 'drop');
});
