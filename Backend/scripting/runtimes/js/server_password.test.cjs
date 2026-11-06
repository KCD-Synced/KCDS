'use strict';

// the server's password (admission/server_password.cjs) and its place in admission.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { ResourceHost } = require('./host.cjs');
const { ServerPassword, passwordProof } = require('./admission/server_password.cjs');

const BINDING = '0123456789abcdef';

function passwordHost(t, serverPassword) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-password-'));
  const logs = [];
  const host = new ResourceHost(root, {
    send: () => {},
    log: (resource, text) => logs.push(`${resource} ${text}`),
    serverPassword,
  });
  host.licence.check = async () => ({ allowed: true, reason: '', pass: null });
  t.after(async () => {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  });
  return { host, root, logs };
}

test('a proof of the right password for this binding is accepted, nothing else', () => {
  const password = new ServerPassword('hunter2');
  assert.equal(password.required(), true);
  assert.equal(password.accepts(BINDING, passwordProof(BINDING, 'hunter2')), true);
  assert.equal(password.accepts(BINDING, passwordProof(BINDING, 'hunter3')), false);
  // A proof made for another connection's binding.
  assert.equal(password.accepts(BINDING, passwordProof('fedcba9876543210', 'hunter2')), false);
  assert.equal(password.accepts(BINDING, ''), false);
  assert.equal(password.accepts(BINDING, 'hunter2'), false);
  assert.equal(password.accepts('', passwordProof('', 'hunter2')), false);
  assert.equal(password.accepts(BINDING, passwordProof(BINDING, 'hunter2').toUpperCase()), false);
});

test('without a password every join passes; an invalid one lets nobody in', () => {
  const open = new ServerPassword(undefined);
  assert.equal(open.required(), false);
  assert.equal(open.accepts(undefined, undefined), true);
  const logs = [];
  const broken = new ServerPassword('x'.repeat(65), (text) => logs.push(text));
  assert.equal(broken.required(), true);
  assert.equal(broken.accepts(BINDING, passwordProof(BINDING, 'x'.repeat(65))), false);
  assert.match(logs[0], /nobody can join/);
  assert.equal(new ServerPassword('tab\there').required(), true);
  assert.equal(
    new ServerPassword('Kraľovstvo').accepts(BINDING, passwordProof(BINDING, 'Kraľovstvo')),
    true,
  );
});

test('admission refuses a missing or wrong password before the handlers, with its own flag', async (t) => {
  const { host, root, logs } = passwordHost(t, 'hunter2');
  const dir = path.join(root, 'gate');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'gate', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(
    path.join(dir, 'index.js'),
    "kcds.core.Events.on('playerConnecting', () => console.log('handler ran'));",
  );
  await host.startAll();
  assert.equal(host.passwordRequired(), true);
  assert.deepEqual(await host.admitConnection(1, { nickname: 'Henry', licenceBinding: BINDING }), {
    allowed: false,
    reason: 'This server asks for a password.',
    password: true,
  });
  const wrong = { licenceBinding: BINDING, passwordProof: passwordProof(BINDING, 'guess') };
  assert.deepEqual(await host.admitConnection(2, wrong), {
    allowed: false,
    reason: 'Wrong password.',
    password: true,
  });
  assert.ok(!logs.some((line) => line.includes('handler ran')));
  const right = { licenceBinding: BINDING, passwordProof: passwordProof(BINDING, 'hunter2') };
  assert.deepEqual(await host.admitConnection(3, right), { allowed: true, reason: '' });
  // The password itself is never in the log.
  assert.ok(!logs.some((line) => line.includes('hunter2')));
  assert.ok(logs.some((line) => line.includes('refused request 2: wrong password')));
});

test('info.json says whether joining asks for a password', async (t) => {
  const { host } = passwordHost(t, 'hunter2');
  assert.equal(host.statusRoutes.info().password, true);
  const { host: open } = passwordHost(t, undefined);
  assert.equal(open.statusRoutes.info().password, false);
});
