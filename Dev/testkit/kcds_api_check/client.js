'use strict';

// The game client half of the live kcds.* check: the types, the HUD, nameplates and a web view
// loaded from kcds:// that talks back through window.sendToGame.

const { Events, Runtime } = kcds.core;
const { Web, Hud, Nameplates, Key, Controls, Camera, ActionHint, Discord } = kcds.ui;

const CLIENT_TYPES = {
  core: [
    'Assets',
    'Chat',
    'Events',
    'Exports',
    'Game',
    'Imports',
    'Messages',
    'Runtime',
    'SharedState',
  ],
  math: ['Color', 'Quaternion', 'Vector2', 'Vector3', 'Vector4'],
  players: ['Inventory', 'LocalPlayer', 'Player', 'Progression'],
  characters: ['Follow', 'LocalNpc'],
  world: ['Audio', 'Blip', 'Gizmos', 'Light', 'Render', 'Travel', 'Vfx', 'World', 'WorldMap'],
  ui: [
    'ActionHint',
    'Book',
    'Camera',
    'CharacterCreator',
    'Compass',
    'Controls',
    'Discord',
    'EmoteWheel',
    'Hud',
    'Key',
    'Keybinds',
    'LoadingScreen',
    'Nameplates',
    'NoClip',
    'Web',
  ],
};
const PAGE = 'kcds://resources/kcds_api_check/ui/index.html';
const PAGE_TIMEOUT_MS = 12000;

function send(results, name, ok, detail = '') {
  results.push({ name, ok });
  console.log(`[api-check] ${ok ? 'PASS' : 'FAIL'} client ${name}${detail ? ` (${detail})` : ''}`);
  Events.toServer('kcds_api_check:result', { name, ok, detail });
}

function check(results, name, body) {
  try {
    const result = body();
    send(results, name, result !== false, typeof result === 'string' ? result : '');
  } catch (problem) {
    send(results, name, false, String(problem && problem.message));
  }
}

// Opens the page and resolves with what happened: the ready event, the message from the page, and
// the reply the page sends when it hears Web.emit.
function checkPage() {
  return new Promise((resolve) => {
    const seen = { ready: false, hello: false, pong: false, failure: '' };
    const view = Web.open(PAGE, { width: 420, height: 160, x: 40, y: 40 });
    const finish = () => {
      clearTimeout(timer);
      Web.close(view);
      resolve(seen);
    };
    const timer = setTimeout(finish, PAGE_TIMEOUT_MS);
    Events.on('webViewReady', (event) => {
      if (event.viewId === view) {
        seen.ready = true;
      }
    });
    Events.on('webViewFailed', (event) => {
      if (event.viewId === view) {
        seen.failure = event.description;
        finish();
      }
    });
    Web.on(view, 'hello', () => {
      seen.hello = true;
      Web.emit(view, 'ping', { value: 5 });
    });
    Web.on(view, 'pong', (payload) => {
      seen.pong = payload && payload.value === 5;
      finish();
    });
  });
}

let running = false;

Events.on('kcds_api_check:run', async () => {
  if (running) {
    return;
  }
  running = true;
  const results = [];
  check(results, 'all domain types exist', () => {
    const missing = [];
    for (const [domain, names] of Object.entries(CLIENT_TYPES)) {
      for (const name of names) {
        if (!(name in kcds[domain])) {
          missing.push(`${domain}.${name}`);
        }
      }
    }
    return missing.length === 0 ? true : false;
  });
  check(results, 'old flat globals are gone', () => {
    return ['Vector3', 'Player', 'LocalPlayer'].every((name) => globalThis[name] === undefined);
  });
  check(
    results,
    'Runtime',
    () => Runtime.isClient && !Runtime.isServer && typeof Runtime.apiVersion === 'string',
  );
  check(results, 'LocalPlayer', () => {
    const me = kcds.players.LocalPlayer;
    return me !== null && typeof me.id === 'number';
  });
  check(results, 'Web.screenSize', () => Web.screenSize().width > 0);
  check(results, 'Hud', () => {
    Hud.showNotification('KCDS API check is running');
    return true;
  });
  check(results, 'Nameplates', () => {
    const shown = Nameplates.isVisible();
    const ownTag = Nameplates.isSelfVisible();
    Nameplates.setSelfVisible(!ownTag);
    const changed = Nameplates.isSelfVisible() === !ownTag;
    Nameplates.setSelfVisible(ownTag);
    return typeof shown === 'boolean' && changed && Nameplates.isSelfVisible() === ownTag;
  });
  check(results, 'Key and Controls', () => {
    return Key.getLabel('e') !== undefined && typeof Controls.isEnabled() === 'boolean';
  });
  check(results, 'Camera', () => Camera.rayFromScreen({ range: 10 }) !== undefined);
  check(results, 'ActionHint', () => ActionHint.keys().includes('script_action'));
  check(results, 'Discord', () => typeof Discord.isAvailable() === 'boolean');
  try {
    const page = await checkPage();
    send(results, 'kcds:// page loads (webViewReady)', page.ready, page.failure);
    send(results, 'window.sendToGame reaches the script', page.hello);
    send(results, 'Web.emit reaches the page and returns', page.pong === true);
  } catch (problem) {
    send(results, 'kcds:// page', false, String(problem && problem.message));
  }
  const failed = results.filter((result) => !result.ok).map((result) => result.name);
  Events.toServer('kcds_api_check:done', {
    passed: results.length - failed.length,
    total: results.length,
    failed,
  });
  running = false;
});
