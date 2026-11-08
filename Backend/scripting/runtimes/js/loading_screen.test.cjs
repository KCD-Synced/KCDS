'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { LoadingScreen, HOLD_LIMIT_MS } = require('../../../../Client/resources/loading_screen.cjs');

function fixture() {
  const calls = [];
  const events = [];
  const web = {
    facade: (owner) => ({
      open: (url) => {
        calls.push(['create', owner, url]);
        return 7;
      },
      close: (id) => calls.push(['destroy', id]),
      emit: (id, name, payload) => calls.push(['emit', id, name, payload]),
    }),
  };
  const screen = new LoadingScreen({
    web,
    emit: (stage, percent) => events.push([stage, percent]),
  });
  return { screen, calls, events, api: screen.facade('demo') };
}

test('the page hears the load and goes when the world is ready', () => {
  const { screen, calls, events, api } = fixture();
  assert.equal(api.show('kcds://resources/demo/ui/l.html'), true);
  screen.progress(10.4);
  screen.progress(10.2);
  screen.progress(5);
  screen.progress(60);
  screen.ready();
  assert.deepEqual(calls, [
    ['create', 'demo', 'kcds://resources/demo/ui/l.html'],
    ['emit', 7, 'kcdsLoading', { stage: 'level', percent: 10 }],
    ['emit', 7, 'kcdsLoading', { stage: 'level', percent: 60 }],
    ['emit', 7, 'kcdsLoading', { stage: 'ready', percent: 100 }],
    ['destroy', 7],
  ]);
  assert.deepEqual(events.at(-1), ['ready', 100]);
  assert.equal(api.show('kcds://resources/demo/ui/l.html'), false, 'too late');
});

test('a hold keeps the page after ready, until release', () => {
  const { screen, calls, api } = fixture();
  api.show('kcds://resources/demo/ui/l.html');
  assert.equal(api.hold(), true);
  screen.ready();
  assert.equal(calls.filter((call) => call[0] === 'destroy').length, 0);
  assert.equal(api.release(), true);
  assert.equal(calls.filter((call) => call[0] === 'destroy').length, 1);
});

test('a hold ends by itself', (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const { screen, calls, api } = fixture();
  api.show('kcds://resources/demo/ui/l.html');
  api.hold();
  screen.ready();
  t.mock.timers.tick(HOLD_LIMIT_MS);
  assert.equal(calls.filter((call) => call[0] === 'destroy').length, 1);
});

test('a package.json page shows before the resources run, hears the download, and starts over', () => {
  const calls = [];
  const sent = [];
  const web = {
    facade: (owner) => ({
      open: (url) => {
        calls.push(['create', owner, url]);
        return 9;
      },
      close: (id) => calls.push(['destroy', id]),
      emit: (id, name, payload) => calls.push(['emit', payload]),
    }),
  };
  const heard = [];
  const screen = new LoadingScreen({
    web,
    emit: (stage, percent) => heard.push([stage, percent]),
    send: (message) => sent.push(message),
  });
  screen.showManifestPage('mode', 'ui/loading.html');
  assert.deepEqual(calls[0], ['create', 'mode', 'kcds://resources/mode/ui/loading.html']);
  assert.deepEqual(sent, [{ op: 'loadingPageShown' }]);
  const progress = { resourcesDone: 1, resourcesTotal: 4, bytesDone: 50, bytesTotal: 200 };
  screen.download(progress);
  assert.deepEqual(calls.at(-1), ['emit', { stage: 'download', percent: 25, ...progress }]);
  assert.deepEqual(heard, [], 'no script runs yet');
  screen.resourcesStarting();
  assert.deepEqual(calls.at(-1), ['emit', { stage: 'resources', percent: 0 }]);
  screen.progress(40);
  assert.deepEqual(heard, [['level', 40]], 'the level counts from nothing after the download');
  screen.ready();
  assert.equal(screen.stage, 'ready');
  screen.reset();
  assert.equal(screen.stage, 'resources');
  assert.equal(
    screen.facade('mode').show('kcds://resources/mode/a.html'),
    true,
    'a new connection',
  );
});

test('a world ready during the download keeps the page until the download is over', () => {
  const calls = [];
  const web = {
    facade: () => ({
      open: () => 9,
      close: (id) => calls.push(['destroy', id]),
      emit: (id, name, payload) => calls.push(['emit', payload.stage, payload.percent]),
    }),
  };
  const screen = new LoadingScreen({ web, emit: () => {} });
  screen.showManifestPage('mode', 'page.html');
  screen.download({ resourcesDone: 0, resourcesTotal: 1, bytesDone: 1, bytesTotal: 4 });
  screen.progress(50);
  assert.equal(screen.stage, 'download', 'the level does not take the bar from the download');
  screen.ready();
  assert.equal(screen.stage, 'download');
  assert.equal(calls.filter(([what]) => what === 'destroy').length, 0, 'the page stays');
  screen.resourcesStarting();
  assert.equal(screen.stage, 'ready');
  assert.equal(calls.filter(([what]) => what === 'destroy').length, 1, 'then the page goes');
});

test('the game client hears when the page goes, so its own windows may come back', () => {
  const sent = [];
  const web = { facade: () => ({ open: () => 9, close: () => {}, emit: () => {} }) };
  const screen = new LoadingScreen({ web, emit: () => {}, send: (message) => sent.push(message) });
  screen.close();
  assert.deepEqual(sent, [], 'no page, nothing to tell');
  screen.showManifestPage('mode', 'page.html');
  screen.close();
  assert.deepEqual(sent, [{ op: 'loadingPageShown' }, { op: 'loadingPageClosed' }]);
});

test('a player the server holds out of the world keeps the page until let in', () => {
  const calls = [];
  const web = {
    facade: () => ({
      open: () => 9,
      close: (id) => calls.push(['destroy', id]),
      emit: () => {},
    }),
  };
  const screen = new LoadingScreen({ web, emit: () => {} });
  screen.showManifestPage('mode', 'page.html');
  screen.setLobby(true);
  screen.ready();
  assert.equal(calls.length, 0, 'held: the page stays at ready');
  assert.equal(screen.facade('mode').show('kcds://resources/mode/creator.html'), true);
  screen.setLobby(false);
  assert.equal(calls.filter(([what]) => what === 'destroy').length, 2, 'let in: the page goes');
});

test('the level left during the download goes on from where it was', () => {
  const web = {
    facade: () => ({ open: () => 9, close: () => {}, emit: () => {} }),
  };
  const heard = [];
  const screen = new LoadingScreen({ web, emit: (stage, percent) => heard.push([stage, percent]) });
  screen.download({ resourcesDone: 0, resourcesTotal: 1, bytesDone: 1, bytesTotal: 4 });
  screen.progress(40);
  assert.deepEqual(heard, []);
  screen.resourcesStarting();
  assert.deepEqual(heard, [['level', 40]]);
});
