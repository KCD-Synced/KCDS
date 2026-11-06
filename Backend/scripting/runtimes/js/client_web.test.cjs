'use strict';

// The client Web global (Client/resources/web.cjs): resource ownership, the synchronous answers,
// the calls sent to the game client and the routing of browser events and page calls.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceWebViews } = require('../../../../Client/resources/web.cjs');

function setup() {
  const sent = [];
  const emitted = [];
  const logs = [];
  const web = new ResourceWebViews({
    send: (message) => sent.push(message),
    log: (owner, text) => logs.push([owner, text]),
    emit: (owner, name, args) => {
      emitted.push([owner, name, args]);
      return Promise.resolve();
    },
  });
  web.updateHost({ ready: true, width: 1920, height: 1080 });
  return { web, sent, emitted, logs, hud: web.facade('hud'), shop: web.facade('shop') };
}

test('anchor center keeps a view in the middle and on the screen as the size changes', () => {
  const { hud, sent, web } = setup();
  const raised = [];
  web.raise = (name, args) => {
    raised.push([name, args]);
    return Promise.resolve();
  };
  const view = hud.open('kcds://resources/hud/ui/index.html', {
    width: 800,
    height: 600,
    anchor: 'center',
  });
  assert.deepEqual([sent[0].x, sent[0].y, sent[0].width, sent[0].height], [560, 240, 800, 600]);
  sent.length = 0;
  web.updateHost({ width: 640, height: 480 });
  assert.deepEqual(
    sent.map((m) => [m.call, m.x, m.y, m.width, m.height]),
    [
      ['resize', undefined, undefined, 640, 480],
      ['move', 0, 0, undefined, undefined],
    ],
  );
  assert.deepEqual(raised, [['gameResolutionChanged', [640, 480]]]);
  // The same size again raises nothing and moves nothing.
  sent.length = 0;
  web.updateHost({ width: 640, height: 480 });
  assert.deepEqual(sent, []);
  assert.equal(raised.length, 1);
  // Placing it by hand ends the anchor.
  hud.move(view, 5, 6);
  sent.length = 0;
  web.updateHost({ width: 1280, height: 720 });
  assert.deepEqual(sent, []);
  assert.throws(() => hud.open('kcds://resources/hud/a.html', { anchor: 'left' }), TypeError);
  assert.throws(() => hud.open('kcds://resources/hud/a.html', { anchor: 'center' }), TypeError);
});

test('open throws before the browser is ready and for unsupported URLs', () => {
  const web = new ResourceWebViews();
  const hud = web.facade('hud');
  assert.throws(() => hud.open('kcds://resources/hud/ui/index.html'), /not ready/);
  web.updateHost({ ready: true });
  assert.throws(() => hud.open('file:///C:/secret.txt'), /kcds:\/\/resources/);
  assert.throws(() => hud.open(42), TypeError);
  assert.throws(() => hud.open('kcds://resources/hud/ui/a.html', { x: 'left' }), TypeError);
});

test('open numbers views, sends the geometry and applies the defaults', () => {
  const { hud, sent } = setup();
  const first = hud.open('kcds://resources/hud/ui/index.html', { x: 10.4, width: 400 });
  const second = hud.open('https://example.com/');
  assert.equal(first, 1);
  assert.equal(second, 2);
  assert.deepEqual(sent[0], {
    op: 'web',
    call: 'create',
    id: 1,
    owner: 'hud',
    url: 'kcds://resources/hud/ui/index.html',
    x: 10,
    y: 0,
    width: 400,
    height: 0,
    zIndex: 0,
    visible: true,
    focus: false,
  });
  assert.equal(hud.isVisible(first), true);
  assert.equal(hud.isBackground(first), false);
});

test('a kcds:// URL may carry a query and a fragment after the path', () => {
  const { hud, sent } = setup();
  const view = hud.open('kcds://resources/hud/ui/index.html#cs');
  assert.equal(sent[0].url, 'kcds://resources/hud/ui/index.html#cs');
  assert.equal(hud.navigate(view, 'kcds://resources/hud/ui/index.html?lang=cs#top'), true);
  assert.throws(() => hud.open('kcds://resources/#cs'), /kcds:\/\/resources/);
});

test('a resource reaches only its own views', () => {
  const { hud, shop, sent } = setup();
  const view = hud.open('kcds://resources/hud/ui/index.html');
  sent.length = 0;
  assert.equal(shop.hide(view), false);
  assert.equal(shop.isVisible(view), false);
  assert.equal(shop.close(view), false);
  assert.equal(shop.emit(view, 'x'), false);
  assert.throws(() => shop.on(view, 'x', () => {}), /Unknown web view/);
  assert.deepEqual(sent, []);
  assert.equal(hud.isVisible(view), true);
});

test('show, hide, focus and offscreen update the answers and the game client', () => {
  const { hud, sent } = setup();
  const view = hud.open('kcds://resources/hud/ui/index.html', { visible: false });
  sent.length = 0;
  assert.equal(hud.isVisible(view), false);
  assert.equal(hud.show(view), true);
  assert.equal(hud.isVisible(view), true);
  assert.equal(hud.focus(view), true);
  assert.equal(hud.setBackground(view), true);
  assert.equal(hud.isBackground(view), true);
  assert.equal(hud.focus(view, false), true);
  assert.deepEqual(
    sent.map(({ call, value }) => [call, value]),
    [
      ['visible', true],
      ['focus', true],
      ['offscreen', true],
      ['focus', false],
    ],
  );
  assert.equal(hud.show(99), false);
});

test('resize, move and navigate validate their arguments', () => {
  const { hud, sent } = setup();
  const view = hud.open('kcds://resources/hud/ui/index.html');
  sent.length = 0;
  assert.equal(hud.resize(view, 640.6, 480), true);
  assert.equal(hud.move(view, 5, 6), true);
  assert.equal(hud.navigate(view, 'kcds://resources/hud/ui/other.html'), true);
  assert.throws(() => hud.resize(view, NaN, 1), TypeError);
  assert.throws(() => hud.navigate(view, 'javascript:alert(1)'), Error);
  assert.deepEqual(sent, [
    { op: 'web', call: 'resize', id: view, width: 641, height: 480 },
    { op: 'web', call: 'move', id: view, x: 5, y: 6 },
    { op: 'web', call: 'load', id: view, url: 'kcds://resources/hud/ui/other.html' },
  ]);
});

test('setViewWorldPosition anchors a view to a world point and null releases it', () => {
  const { hud, sent } = setup();
  const view = hud.open('kcds://resources/hud/ui/index.html');
  sent.length = 0;
  assert.equal(hud.setViewWorldPosition(view, { x: 120.25, y: 2040.5, z: 56.75 }, -7, -7), true);
  assert.equal(hud.setViewWorldPosition(view, null), true);
  assert.throws(() => hud.setViewWorldPosition(view, { x: 1, y: NaN, z: 0 }), TypeError);
  assert.throws(() => hud.setViewWorldPosition(view, 5), TypeError);
  assert.equal(hud.setViewWorldPosition(99, null), false);
  assert.deepEqual(sent, [
    {
      op: 'web',
      call: 'anchor',
      id: view,
      value: true,
      x: 120.25,
      y: 2040.5,
      z: 56.75,
      offsetX: -7,
      offsetY: -7,
    },
    { op: 'web', call: 'anchor', id: view, value: false, x: 0, y: 0, z: 0, offsetX: 0, offsetY: 0 },
  ]);
});

test('emit sends JSON text and null without a payload', () => {
  const { hud, sent } = setup();
  const view = hud.open('kcds://resources/hud/ui/index.html');
  sent.length = 0;
  hud.emit(view, 'shop:purse', { gold: 3 });
  hud.emit(view, 'ping');
  assert.deepEqual(sent, [
    { op: 'web', call: 'emit', id: view, name: 'shop:purse', payload: '{"gold":3}' },
    { op: 'web', call: 'emit', id: view, name: 'ping', payload: 'null' },
  ]);
});

test('page calls reach the owning resource handlers with the parsed payload', () => {
  const { web, hud } = setup();
  const view = hud.open('kcds://resources/hud/ui/index.html');
  const received = [];
  const handler = (payload) => received.push(payload);
  hud.on(view, 'shop:buy', handler);
  web.receive({
    type: 'web-call',
    owner: 'hud',
    viewId: view,
    name: 'shop:buy',
    payload: '{"item":"apple","amount":3}',
  });
  web.receive({ type: 'web-call', owner: 'hud', viewId: view, name: 'shop:buy', payload: null });
  web.receive({ type: 'web-call', owner: 'shop', viewId: view, name: 'shop:buy', payload: '1' });
  assert.deepEqual(received, [{ item: 'apple', amount: 3 }, null]);
});

test('off removes one handler or every handler of the event', () => {
  const { web, hud } = setup();
  const view = hud.open('kcds://resources/hud/ui/index.html');
  const calls = [];
  const a = () => calls.push('a');
  const b = () => calls.push('b');
  hud.on(view, 'tick', a);
  hud.on(view, 'tick', b);
  assert.equal(hud.off(view, 'tick', a), true);
  assert.equal(hud.off(view, 'tick', a), false);
  web.receive({ type: 'web-call', owner: 'hud', viewId: view, name: 'tick', payload: null });
  assert.equal(hud.off(view, 'tick'), true);
  assert.equal(hud.off(view, 'tick'), false);
  web.receive({ type: 'web-call', owner: 'hud', viewId: view, name: 'tick', payload: null });
  assert.deepEqual(calls, ['b']);
});

test('browser events go to the view owner only', () => {
  const { web, hud, emitted } = setup();
  const view = hud.open('kcds://resources/hud/ui/index.html');
  web.receive({
    type: 'web-event',
    owner: 'hud',
    event: 'webViewReady',
    args: [{ viewId: view, url: 'kcds://resources/hud/ui/index.html' }],
  });
  web.receive({
    type: 'web-event',
    owner: 'shop',
    event: 'webViewReady',
    args: [{ viewId: view, url: 'x' }],
  });
  assert.deepEqual(emitted, [
    ['hud', 'webViewReady', [{ viewId: view, url: 'kcds://resources/hud/ui/index.html' }]],
  ]);
});

test('webViewFailed reaches the owner with the URL, error and frame', () => {
  const { web, hud, emitted } = setup();
  const view = hud.open('https://chat.example.com/');
  const failed = {
    viewId: view,
    url: 'https://chat.example.com/',
    description: 'ERR_CONNECTION_REFUSED',
    errorCode: -102,
    isMainFrame: true,
  };
  web.receive({ type: 'web-event', owner: 'hud', event: 'webViewFailed', args: [failed] });
  assert.deepEqual(emitted, [['hud', 'webViewFailed', [failed]]]);
});

test('close and a stopped resource drop the views and their handlers', () => {
  const { web, hud, shop, sent } = setup();
  const one = hud.open('kcds://resources/hud/ui/index.html');
  const two = shop.open('kcds://resources/shop/ui/index.html');
  assert.equal(hud.close(one), true);
  assert.equal(hud.close(one), false);
  assert.equal(hud.isVisible(one), false);
  web.removeOwner('shop');
  assert.equal(shop.isVisible(two), false);
  assert.deepEqual(sent.at(-1), { op: 'web', call: 'destroyOwned', owner: 'shop' });
});

test('screenSize answers the last frame size the game reported', () => {
  const { web, hud } = setup();
  assert.deepEqual(hud.screenSize(), { width: 1920, height: 1080 });
  web.updateHost({ width: 2560, height: 1440 });
  assert.deepEqual(hud.screenSize(), { width: 2560, height: 1440 });
});

test('anyFocused sees a shown focused view of any resource', () => {
  const { hud, shop } = setup();
  const page = shop.open('kcds://resources/shop/ui/index.html');
  assert.equal(hud.anyFocused(), false);
  shop.focus(page);
  assert.equal(hud.anyFocused(), true);
  shop.hide(page);
  assert.equal(hud.anyFocused(), false);
  shop.show(page);
  assert.equal(shop.anyFocused(), true);
  shop.focus(page, false);
  assert.equal(shop.anyFocused(), false);
});
