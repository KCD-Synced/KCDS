'use strict';

// NativeScreen and NativeClip (Client/resources/native_ui.cjs) against a fake game client: the
// screen's movie instance, its host movie, clip keys, events, focus and the shipped files.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceNativeUI } = require('../../../../Client/resources/native_ui.cjs');
const { hostMovie, HOST_VERSION } = require('../../../../Client/resources/native_host_movie.cjs');

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function setup({ files = {}, answer = () => undefined } = {}) {
  const sent = [];
  const asked = [];
  const posted = [];
  const logged = [];
  const input = { paused: false };
  const ui = new ResourceNativeUI({
    sync: {
      post(request) {
        asked.push(request);
        posted.push(request);
      },
      call(request) {
        asked.push(request);
        const special = answer(request);
        if (special !== undefined) {
          return special;
        }
        if (request.action === 'get' && request.member === 'kcdsVersion') {
          return HOST_VERSION;
        }
        if (request.action === 'invoke' && request.method === 'kcdsStage') {
          return '1920,1080';
        }
        if (request.op === 'elementInfo') {
          return { valid: true, visible: true };
        }
        return true;
      },
    },
    send: (message) => sent.push(message),
    log: (owner, text) => logged.push([owner, text]),
    shippedFile: (owner, path) => files[path] ?? null,
    screenSize: () => ({ width: 960, height: 600 }),
    input,
  });
  return { ui, sent, asked, posted, logged, input, api: ui.facade('a') };
}

async function readyScreen(state, options) {
  const screen = state.api.NativeUI.createScreen(options);
  const seen = [];
  screen.on('ready', () => seen.push('ready'));
  await sleep(250);
  assert.deepEqual(seen, ['ready']);
  return screen;
}

test('a screen is its own movie instance, empty, with a host movie, and ready later', async () => {
  const state = setup();
  const { NativeUI, NativeScreen, NativeClip } = state.api;
  const screen = NativeUI.createScreen();
  assert.ok(screen instanceof NativeScreen);
  assert.equal(screen.ready, false);
  assert.equal(screen.root, null);
  const instance = 47100 + screen.id;
  assert.deepEqual(state.asked.slice(0, 5), [
    // A movie the last session left in this instance goes first: the screen starts fresh.
    { op: 'elementState', name: 'Menu', instance, action: 'unload' },
    { op: 'elementState', name: 'Menu', instance, action: 'show' },
    { op: 'elementState', name: 'Menu', instance, action: 'noCursor' },
    { op: 'elementClip', name: 'Menu', instance, path: [], action: 'isolate' },
    {
      op: 'elementClip',
      name: 'Menu',
      instance,
      path: [],
      action: 'create',
      child: `kcds_screen_${screen.id}`,
      depth: 16000,
    },
  ]);
  const host = state.sent.find((message) => message.op === 'nativeFile');
  assert.equal(host.path, `host-${HOST_VERSION}.swf`);
  assert.equal(Buffer.from(host.data, 'base64').length, hostMovie().length);
  await sleep(250);
  assert.equal(screen.ready, true);
  assert.ok(screen.root instanceof NativeClip);
  assert.equal(screen.root.screen, screen.id);
  assert.equal(screen.root.key, `kcds_screen_${screen.id}`);
  assert.equal(new NativeScreen(screen.id).ready, true);
  assert.equal(screen.close(), true);
  assert.equal(screen.close(), false);
});

test('clips are created with their own names and set takes stage units and multipliers', async () => {
  const state = setup();
  const screen = await readyScreen(state);
  const panel = screen.root.createClip();
  const label = panel.createText({ x: 10, y: 20, width: 300, height: 40, text: 'Hello' });
  const button = panel.attach('BasicButton');
  assert.equal(panel.key, `${screen.root.key}.c1`);
  assert.equal(label.key, `${panel.key}.c2`);
  assert.equal(button.key, `${panel.key}.c3`);
  assert.equal(panel.set({ x: 100, scaleX: 1.5, alpha: 0.5, visible: false }), true);
  const sets = state.asked.filter(
    (request) => request.action === 'set' && request.path.at(-1) === 'c1',
  );
  assert.deepEqual(
    sets.map((request) => [request.member, request.value]),
    [
      ['_x', 100],
      ['_xscale', 150],
      ['_alpha', 50],
      ['_visible', false],
    ],
  );
  assert.ok(state.asked.some((request) => request.member === 'text' && request.value === 'Hello'));
  assert.equal(label.setText('<b>bold</b>', true), true);
  assert.deepEqual(state.asked.at(-1), {
    op: 'elementClip',
    name: 'Menu',
    instance: 47100 + screen.id,
    path: label.key.split('.'),
    action: 'set',
    member: 'htmlText',
    value: '<b>bold</b>',
  });
  assert.throws(() => panel.set({ x: 'left' }), TypeError);
  assert.equal(screen.root.invoke('getNextHighestDepth'), true);
  assert.equal(panel.invoke('swapDepths', [10, 20]), true);
  assert.deepEqual(state.asked.at(-1).args, [10, 20]);
});

test('text styles, masks and hit areas go through the host movie with paths', async () => {
  const state = setup();
  const screen = await readyScreen(state);
  const panel = screen.root.createClip();
  const mask = screen.root.createClip();
  const text = panel.createText({ x: 0, y: 0, width: 100, height: 20 });
  assert.equal(
    text.setTextStyle({ font: 'DisplayFont', size: 30, bold: true, align: 'justify' }),
    true,
  );
  assert.deepEqual(state.asked.at(-1).args, [
    `_root.${text.key}`,
    'DisplayFont',
    30,
    0xffffff,
    'justify',
    true,
    false,
  ]);
  assert.equal(panel.setMask(mask), true);
  assert.deepEqual(state.asked.at(-1).args, [`_root.${panel.key}`, `_root.${mask.key}`]);
  assert.equal(panel.setMask(null), true);
  assert.equal(state.asked.at(-1).args[1], '');
  assert.equal(panel.setHitArea(mask), true);
  assert.equal(state.asked.at(-1).method, 'kcdsHit');
  assert.throws(() => text.setTextStyle({ font: 'Arial' }), TypeError);
});

test('clip mouse events reach their handlers with the clip, and off removes them', async () => {
  const state = setup();
  const screen = await readyScreen(state);
  const button = screen.root.createClip();
  const seen = [];
  assert.equal(
    button.on('press', (clip) => seen.push(['press', clip.key])),
    true,
  );
  assert.equal(
    button.on('dragOut', () => seen.push(['dragOut'])),
    true,
  );
  const watch = state.asked.find((request) => request.method === 'kcdsWatch');
  const id = watch.args[1];
  const instance = 47100 + screen.id;
  state.ui.receiveElementEvent({
    name: 'Menu',
    instance,
    event: 'kcdsClip',
    args: [`${id},press`],
  });
  state.ui.receiveElementEvent({
    name: 'Menu',
    instance,
    event: 'kcdsClip',
    args: [id, 'dragOut'],
  });
  state.ui.receiveElementEvent({ name: 'Menu', instance, event: 'kcdsClip', args: ['999,press'] });
  assert.deepEqual(seen, [['press', button.key], ['dragOut']]);
  button.off('press');
  button.off('dragOut');
  assert.equal(state.asked.at(-1).method, 'kcdsUnwatch');
  assert.throws(() => button.on('hover', () => {}), TypeError);
});

test('the event notification carries any movie event; ready, unload and input arrive later', async () => {
  let valid = true;
  const state = setup({
    answer: (request) => (request.op === 'elementInfo' ? { valid, visible: valid } : undefined),
  });
  const screen = await readyScreen(state);
  const seen = [];
  screen.on('event', (name, args) => seen.push(['event', name, args]));
  screen.on('input', (action, mode) => seen.push(['input', action, mode]));
  screen.on('unload', () => seen.push(['unload']));
  const instance = 47100 + screen.id;
  assert.deepEqual(state.sent.at(-1), {
    op: 'nativeElementListen',
    name: 'Menu',
    instance,
    event: '*',
  });
  state.ui.receiveElementEvent({ name: 'Menu', instance, event: 'OnButton', args: ['first'] });
  state.ui.receiveElementEvent({ name: 'Menu', instance, event: 'kcdsClip', args: ['1,press'] });
  assert.equal(screen.focus(), true);
  assert.equal(state.input.paused, true);
  state.ui.receiveEdges({
    edges: [
      ['escape', 'down'],
      ['q', 'down'],
      ['enter', 'up'],
    ],
  });
  assert.deepEqual(seen, []); // a later tick
  await sleep(10);
  assert.deepEqual(seen, [
    ['event', 'OnButton', ['first']],
    ['input', 'kcds_ui_back', 'press'],
    ['input', 'kcds_ui_accept', 'release'],
  ]);
  screen.blur();
  assert.equal(state.input.paused, false);
  valid = false;
  await sleep(2200);
  assert.deepEqual(seen.at(-1), ['unload']);
  assert.throws(() => screen.on('resize', () => {}), TypeError);
});

test('stage and screenToStage use the movie size and the fitted, centred viewport', async () => {
  const state = setup();
  const screen = await readyScreen(state);
  assert.deepEqual(screen.stage(), { width: 1920, height: 1080 });
  assert.deepEqual(screen.screenToStage(0.5, 0.5), { x: 960, y: 540 });
  assert.deepEqual(screen.screenToStage(0, 0.05), { x: 0, y: 0 });
});

test('one screen holds the focus; closing it gives the focus back', async () => {
  const state = setup();
  const first = await readyScreen(state);
  const second = await readyScreen(state);
  first.focus();
  second.focus();
  assert.equal(first.hasFocus(), false);
  assert.equal(second.hasFocus(), true);
  second.close();
  assert.equal(second.focus(), false);
  assert.deepEqual(
    state.sent
      .filter((message) => message.op === 'nativeScreenFocus')
      .map((message) => message.focused),
    [true, false],
  );
});

test('load takes image and archive urls; shipped movies and assets open a screen', async () => {
  const movie = new Uint8Array(30000).fill(7);
  const state = setup({ files: { 'ui/panel.swf': movie, 'ui/seal.png': new Uint8Array(10) } });
  const screen = await readyScreen(state);
  const clip = screen.root.createClip();
  assert.equal(clip.load('img://Libs/UI/Textures/KCDLogo.dds'), true);
  assert.deepEqual(state.asked.at(-1).args, ['img://Libs/UI/Textures/KCDLogo.dds']);
  assert.equal(clip.load('Libs/UI/Menu.gfx'), true);
  assert.deepEqual(state.asked.at(-1).args, ['/Libs/UI/Menu.gfx']);
  assert.throws(() => clip.load('C:/secret.gfx'), TypeError);
  assert.throws(() => clip.load('../up.gfx'), TypeError);
  const shipped = state.api.NativeUI.createScreen({
    movie: 'ui/panel.swf',
    assets: ['ui/seal.png'],
  });
  const files = state.sent.filter(
    (message) => message.op === 'nativeFile' && message.resource === 'a',
  );
  assert.deepEqual(
    files.map((message) => [message.path, message.offset]),
    [
      ['ui/seal.png', 0],
      ['ui/panel.swf', 0],
      ['ui/panel.swf', 24576],
    ],
  );
  assert.ok(
    state.asked.some(
      (request) =>
        request.method === 'loadMovie' && request.args[0] === '/%USER%/kcds_ui/a/ui/panel.swf',
    ),
  );
  assert.throws(() => state.api.NativeUI.createScreen({ movie: 'ui/missing.swf' }), Error);
  shipped.close();
});

test('screens are capped per resource and close when the resource stops', async () => {
  const state = setup();
  const screens = [];
  for (let index = 0; index < 16; index += 1) {
    screens.push(state.api.NativeUI.createScreen());
  }
  assert.throws(() => state.api.NativeUI.createScreen(), RangeError);
  state.ui.removeOwner('a');
  assert.ok(screens.every((screen) => !screen.valid));
});

test('libraries answers the default host as the empty name', () => {
  const state = setup({
    answer: (request) =>
      request.op === 'elementLibrary'
        ? { name: request.name, movie: `Libs/UI/${request.name}.gfx`, exports: ['X'] }
        : undefined,
  });
  const list = state.api.NativeUI.libraries();
  assert.deepEqual(
    list.map((library) => library.name),
    ['', 'hud'],
  );
  assert.equal(list[0].movie, 'Libs/UI/Menu.gfx');
});

test('separate drawing invokes run together as one batch', async () => {
  const state = setup();
  const screen = await readyScreen(state);
  const panel = screen.root.createClip();
  panel.invoke('beginFill', [0x1a1109, 92]);
  panel.invoke('moveTo', [0, 0]);
  panel.invoke('lineTo', [840, 0]);
  panel.invoke('endFill');
  await Promise.resolve();
  const draw = state.asked.filter((request) => request.action === 'draw');
  assert.equal(draw.length, 1);
  assert.deepEqual(
    draw[0].commands.map((command) => command.method),
    ['beginFill', 'moveTo', 'lineTo', 'endFill'],
  );
  panel.invoke('lineTo', [1, 1]);
  panel.set({ x: 5 });
  const order = state.asked.slice(-2).map((request) => request.action);
  assert.deepEqual(order, ['draw', 'set']);
});

test('a screen layer sets its place in the game UI stack', () => {
  const state = setup();
  const screen = state.api.NativeUI.createScreen({ layer: 7 });
  const instance = 47100 + screen.id;
  assert.deepEqual(
    state.asked.find((request) => request.action === 'layer'),
    { op: 'elementState', name: 'Menu', instance, action: 'layer', layer: 7 },
  );
  assert.throws(() => state.api.NativeUI.createScreen({ layer: 1.5 }), TypeError);
  const plain = state.api.NativeUI.createScreen();
  assert.equal(
    state.asked.filter((request) => request.action === 'layer').length,
    1,
    `screen ${plain.id} without a layer keeps the movie's own`,
  );
  screen.close();
  plain.close();
});

test('a screen raises unload when its movie unloads and the element stays valid', async () => {
  let unloaded = false;
  const state = setup({
    answer: (request) =>
      unloaded && request.action === 'exists' && request.path.length === 1 ? false : undefined,
  });
  const screen = await readyScreen(state);
  const seen = [];
  screen.on('unload', () => seen.push('unload'));
  unloaded = true;
  await sleep(2300);
  assert.deepEqual(seen, ['unload']);
});

test('a screen opens without waiting and its ready check backs off while the game is busy', async () => {
  let busy = true;
  const state = setup();
  const calls = [];
  const call = state.ui.sync.call.bind(state.ui.sync);
  state.ui.sync.call = (request, timeoutMs) => {
    calls.push([request.action, timeoutMs]);
    return busy && request.action === 'exists' ? undefined : call(request, timeoutMs);
  };
  const screen = state.api.NativeUI.createScreen({ layer: 3 });
  assert.deepEqual(
    state.posted.map((request) => request.action),
    ['unload', 'show', 'noCursor', 'layer', 'isolate', 'create', 'create', 'invoke'],
  );
  assert.equal(calls.length, 0);
  await sleep(250);
  assert.deepEqual(calls, [['exists', 100]]);
  assert.equal(screen.ready, false);
  busy = false;
  await sleep(1200);
  assert.equal(screen.ready, true);
  screen.close();
});
