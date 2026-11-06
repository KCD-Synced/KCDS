'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { resourceTimers } = require('./timers.cjs');

function fixture(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-lifecycle-'));
  const logs = [];
  const host = new ResourceHost(root, { send() {}, log: (...args) => logs.push(args) });
  const write = (name, code, options = {}) => {
    const folder = path.join(root, name);
    fs.mkdirSync(folder, { recursive: true });
    fs.writeFileSync(
      path.join(folder, 'package.json'),
      JSON.stringify({ name, kcds: { serverScripts: ['main.js'], ...options } }),
    );
    fs.writeFileSync(path.join(folder, 'main.js'), code);
  };
  t.after(async () => {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  });
  return { host, write, logs };
}

test('async resourceStart finishes before a dependent executes its scripts', async (t) => {
  const { host, write, logs } = fixture(t);
  write(
    'library',
    `kcds.core.Events.on('resourceStart',async name=>{
    if(name==='library') {await new Promise(resolve=>setTimeout(resolve,10));kcds.core.Exports.register('ready',42);}
  });`,
    { exports: ['ready'] },
  );
  write('mode', `console.log(kcds.core.Exports.get('library','ready'));`, {
    requires: ['library'],
  });
  await host.startAll();
  assert.ok(logs.some(([name, value]) => name === 'mode' && value === '42'));
});

test('ensure restarts all running dependents and refresh alone does not start new resources', async (t) => {
  const { host, write, logs } = fixture(t);
  write('library', `kcds.core.Exports.register('value',1);`, { exports: ['value'] });
  write(
    'mode',
    `console.log('value='+kcds.core.Exports.get('library','value'));kcds.core.Events.on('resourceStop',name=>{
    if(name==='mode') console.log('mode-stop');});`,
    { requires: ['library'] },
  );
  await host.startAll();
  const oldGeneration = host.resources.get('mode').generation;
  write('library', `kcds.core.Exports.register('value',2);`, { exports: ['value'] });
  write('discovered', `throw Error('refresh must not start me');`);
  host.refresh();
  assert.equal(host.resources.has('discovered'), false);
  await host.ensure('library');
  assert.ok(host.resources.get('mode').generation > oldGeneration);
  assert.ok(logs.some(([name, value]) => name === 'mode' && value === 'value=2'));
  assert.ok(logs.some(([name, value]) => name === 'mode' && value === 'mode-stop'));
  assert.equal(host.resources.has('discovered'), false);
});

test('failed optional dependencies are skipped and independent resources still start', async (t) => {
  const { host, write } = fixture(t);
  write('broken', `throw Error('expected startup failure');`);
  write('optional', `console.log('optional alive');`, {
    requires: [{ name: 'broken', optional: true }],
  });
  write('required', `throw Error('must not execute');`, { requires: ['broken'] });
  write('independent', `console.log('alive');`);
  await host.startAll();
  assert.equal(host.resources.has('broken'), false);
  assert.equal(host.resources.has('required'), false);
  assert.equal(host.resources.has('optional'), true);
  assert.equal(host.resources.has('independent'), true);
});

test('resource stop rejects pending messages and removes exports and refreshed timers', async (t) => {
  const { host, write } = fixture(t);
  write(
    'library',
    `kcds.core.Messages.handle('never',()=>{});kcds.core.Exports.register('answer',42);`,
    {
      exports: ['answer'],
    },
  );
  write('mode', `console.log('started');`);
  await host.startAll();
  const sender = host.services.facade(host.services.owners.get('mode'));
  const pending = sender.Messages.request('library', 'never');
  const rejected = assert.rejects(pending, /stopped/);
  await host.stop('library');
  await rejected;
  assert.throws(() => sender.Exports.get('library', 'answer'), /Resource is not available/);
  const state = { active: true, timers: new Set() };
  let fired = 0;
  const timers = resourceTimers(
    state,
    (fn) => fn(),
    (error) => {
      throw error;
    },
  );
  const handle = timers.setTimeout(() => fired++, 1);
  await new Promise((resolve) => setTimeout(resolve, 5));
  assert.equal(fired, 1);
  handle.refresh();
  assert.equal(state.timers.has(handle), true);
  state.active = false;
  timers.clearTimeout(handle);
  handle.refresh();
  await new Promise((resolve) => setTimeout(resolve, 5));
  assert.equal(fired, 1);
  assert.equal(state.timers.size, 0);
});

test('uncaught timer failures stop only their resource; continue removes entities but retains code', async (t) => {
  const { host, write, logs } = fixture(t);
  write('bad', `setTimeout(()=>{throw Error('timer fault');},1);`);
  write('alive', `console.log('still running');`);
  write(
    'continue',
    `kcds.characters.Horse.spawn();setTimeout(()=>{throw Error('continue fault');},1);`,
    {
      errorBehavior: 'continue',
    },
  );
  await host.startAll();
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(host.resources.has('bad'), false);
  assert.equal(host.resources.has('alive'), true);
  assert.equal(host.resources.has('continue'), true);
  assert.equal(host.horses.size, 0);
  assert.ok(logs.some(([name, text]) => name === 'bad' && text.includes('timer fault')));
});

test('a stopped resource takes everything it spawned with it and starts again cleanly', async (t) => {
  const { host, write } = fixture(t);
  const spawnAll = `
    const at = { x: 1, y: 2, z: 3 };
    kcds.world.Prop.spawn('barrel_a', at);
    kcds.world.Vfx.spawn('WH_Particels.other.magic', at);
    kcds.world.GroundItem.spawn('apple', at);
    kcds.world.Stash.spawn(at);
    kcds.world.Stash.createStorage();
    kcds.world.Cart.spawn('wagon_b_covered', at);
    kcds.world.Area.create({ id: 'yard', type: 'sphere', position: at, radius: 5 });
    kcds.world.Blip.create({ position: at });
    kcds.world.Marker.place('materials/decals/x', at);
    kcds.characters.PatrolRoute.create({ id: 'yard.loop', points: [at, { x: 5, y: 2, z: 3 }] });
    kcds.world.SiegeEngine.spawn('cannon', at);
    kcds.world.SiegeLadder.spawn('siege', at);
    kcds.world.StonePile.spawn(at);
  `;
  write('spawner', spawnAll);
  write('keeper', `kcds.world.Prop.spawn('barrel_a', { x: 9, y: 9, z: 9 });`);
  await host.startAll();
  const counts = () => ({
    props: host.props.props.size,
    effects: host.vfx.effects.size,
    groundItems: host.groundItems.stacks.size,
    stashes: host.stashes.stashes.size,
    carts: host.carts.carts.size,
    areas: host.areas.byId.size,
    blips: host.blips.byId.size,
    markers: host.markers.byId.size,
    routes: host.npcs.patrols.routes.records.size,
    engines: host.siege.engines.size,
    ladders: host.siegeLadders.ladders.size,
    piles: host.stonePiles.piles.size,
  });
  const running = counts();
  assert.equal(running.props, 2);
  assert.equal(running.stashes, 2);
  await host.stop('spawner');
  assert.deepEqual(counts(), {
    props: 1,
    effects: 0,
    groundItems: 0,
    stashes: 0,
    carts: 0,
    areas: 0,
    blips: 0,
    markers: 0,
    routes: 0,
    engines: 0,
    ladders: 0,
    piles: 0,
  });
  // The same named patrol route and area can be made again by the restarted resource.
  await host.start('spawner');
  assert.deepEqual(counts(), running);
});

test('compiled server code reaches require through the global object too', async (t) => {
  const { host, write, logs } = fixture(t);
  write(
    'compiled',
    `const nodeRequire = globalThis.require;
    console.log(nodeRequire('node:path').basename('/a/b.txt'));
    console.log(nodeRequire('./settings').value);`,
  );
  fs.writeFileSync(path.join(host.root, 'compiled', 'settings.json'), JSON.stringify({ value: 7 }));
  await host.startAll();
  assert.equal(host.resources.has('compiled'), true);
  assert.ok(logs.some(([name, text]) => name === 'compiled' && text === 'b.txt'));
  assert.ok(logs.some(([name, text]) => name === 'compiled' && text === '7'));
});

test('server CommonJS this and relative JSON modules match compiled resource expectations', async (t) => {
  const { host, write, logs } = fixture(t);
  write(
    'module',
    `
    "use strict";
    if(this!==module.exports) throw Error('CommonJS this is wrong');
    console.log(require('./settings').value);
    console.log(require('node:path').basename('/own/resource'));
  `,
  );
  fs.writeFileSync(path.join(host.root, 'module', 'settings.json'), JSON.stringify({ value: 42 }));
  await host.startAll();
  assert.equal(host.resources.has('module'), true);
  assert.ok(logs.some(([name, text]) => name === 'module' && text === '42'));
  assert.ok(logs.some(([name, text]) => name === 'module' && text === 'resource'));
});

test('a late rejection from an old timer cannot fail the restarted resource', async (t) => {
  const { host, write } = fixture(t);
  write(
    'mode',
    `setTimeout(async()=>{
    await new Promise(resolve=>require('node:timers').setTimeout(resolve,40));
    throw Error('old generation failure');
  },1);`,
  );
  await host.startAll();
  await new Promise((resolve) => setTimeout(resolve, 10));
  write('mode', `console.log('new generation');`);
  await host.ensure('mode');
  const replacement = host.resources.get('mode');
  await new Promise((resolve) => setTimeout(resolve, 70));
  assert.equal(host.resources.get('mode'), replacement);
  assert.equal(host.failedResources.has('mode'), false);
});

test('automatic restart waits one second and restores the dependents it stopped', async (t) => {
  const { host, write, logs } = fixture(t);
  write(
    'library',
    `
    const fs=require('node:fs');
    const marker=__dirname+'/attempt.txt';
    const attempt=fs.existsSync(marker)?Number(fs.readFileSync(marker,'utf8'))+1:1;
    fs.writeFileSync(marker,String(attempt));
    kcds.core.Exports.register('attempt',attempt);
    if(attempt===1) setTimeout(()=>{throw Error('restart fault');},1);
  `,
    { exports: ['attempt'], errorBehavior: 'restart' },
  );
  write('consumer', `console.log('attempt='+kcds.core.Exports.get('library','attempt'));`, {
    requires: ['library'],
  });
  await host.startAll();
  const start = Date.now();
  await new Promise((resolve) => setTimeout(resolve, 50));
  assert.equal(host.resources.has('library'), false);
  assert.equal(host.resources.has('consumer'), false);
  while (!host.resources.has('consumer') && Date.now() - start < 3000) {
    await new Promise((resolve) => setTimeout(resolve, 20));
  }
  assert.ok(Date.now() - start >= 1000);
  assert.equal(host.resources.has('consumer'), true);
  assert.ok(logs.some(([name, text]) => name === 'consumer' && text === 'attempt=2'));
});

test('explicit stop cancels a pending automatic restart', async (t) => {
  const { host, write } = fixture(t);
  write('mode', `setTimeout(()=>{throw Error('restart fault');},1);`, { errorBehavior: 'restart' });
  await host.startAll();
  await new Promise((resolve) => setTimeout(resolve, 30));
  assert.equal(host.restartTimers.size, 1);
  await host.stop('mode');
  assert.equal(host.restartTimers.size, 0);
  await new Promise((resolve) => setTimeout(resolve, 1100));
  assert.equal(host.resources.has('mode'), false);
});

test('async error ownership fences the old VM generation after ensure', async (t) => {
  const { host, write } = fixture(t);
  write('mode', `console.log('first');`);
  await host.startAll();
  const old = host.resources.get('mode');
  write('mode', `console.log('second');`);
  await host.ensure('mode');
  const current = host.resources.get('mode');
  const error = Error('old asynchronous continuation');
  error.stack = `Error: old asynchronous continuation\n    at ${old.folder}/main.js:1:1`;
  assert.equal(
    host.context.run(old, () => host.errorOwner(error)),
    undefined,
  );
  assert.equal(
    host.context.run(current, () => host.errorOwner(error)),
    'mode',
  );
  assert.throws(() => host.context.run(old, () => host.api.Horse.spawn()), /stopped/);
  assert.equal(host.resources.get('mode'), current);
});
