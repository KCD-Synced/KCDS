'use strict';

const { DiskFile } = require('./bundle_cache.cjs');
const { hostMovie, HOST_VERSION, CLIP_EVENTS, TAB_STOPS } = require('./native_host_movie.cjs');

// The client-only NativeUI global and its handles: the game's own
// Scaleform UI driven from client scripts.
//
// NativeElement drives one of the game's UI elements (Client/runtime/native_ui). Calls that
// answer with a value or with whether the movie took them go through the synchronous channel
// (Client/resources/sync_channel.cjs) and answer at once, about one game frame later; element
// events arrive as { op: 'nativeElementEvent', name, instance, event, args } and reach their
// handlers on a later tick.

const MAX_ARGUMENTS = 16;
const MAX_STRING = 65536;
const MAX_NAME = 256;
// Native screens: per resource, their element instances and the depth of their container.
const MAX_SCREENS = 16;
// The resources' own Esc menu buttons: the game's two save buttons' places, which a
// server's world does not show.
const MAX_MENU_BUTTONS = 2;
const MAX_MENU_LABEL = 64;
const MAX_SCREEN_ID = 9999;
const SCREEN_INSTANCE_BASE = 47100;
const SCREEN_DEPTH = 16000;
const DEFAULT_LIBRARY = 'Menu';
const DEFAULT_LIBRARIES = Object.freeze([DEFAULT_LIBRARY, 'hud']);
// Files a screen loads go to the game client, which keeps them in memory and serves the movie
// loader's requests for %USER%/kcds_ui/<resource>/... from there (nothing is written to disk).
const FILE_CHUNK = 24576;
const MAX_FILE = 32 * 1024 * 1024;
const MAX_TEXTURE = 16 * 1024 * 1024;
// Where the game client mounts the pictures resources ship (Book.image).
// The book screen (GeneralBook) loads <img src='name'> as
// img://Libs/UI/Textures/Books/Unique/<name>_ui.dds (the UI picture loader's requests).
function bookPictureName(resource, path) {
  const stem = `kcds_${resource}_${path.replace(/\.dds$/i, '')}`;
  return stem.toLowerCase().replace(/[^a-z0-9_]/g, '_');
}
const HOST_RESOURCE = '_kcds';
const HOST_FILE = `host-${HOST_VERSION}.swf`;
const HOST_WAIT_REQUESTS = 60;
const SCREEN_EVENTS = Object.freeze(['ready', 'unload', 'input', 'event']);
const READY_INTERVAL_MS = 100;
// A ready check waits this long for the game; unanswered (a level loading), it retries later.
const READY_ASK_MS = 100;
const READY_BUSY_MS = 1000;
const READY_TRIES = 150;
const UNLOAD_INTERVAL_MS = 2000;
// NativeClip.set fields: the ActionScript member and the factor from the script's unit.
const CLIP_FIELDS = Object.freeze({
  x: ['_x', 1],
  y: ['_y', 1],
  rotation: ['_rotation', 1],
  scaleX: ['_xscale', 100],
  scaleY: ['_yscale', 100],
  alpha: ['_alpha', 100],
  visible: ['_visible', 1],
});
// The focus actions of a focused screen ('input'), from the keys that drive menus.
const FOCUS_ACTIONS = Object.freeze({
  enter: 'kcds_ui_accept',
  space: 'kcds_ui_accept',
  escape: 'kcds_ui_back',
  backspace: 'kcds_ui_back',
  left: 'kcds_ui_left',
  a: 'kcds_ui_left',
  right: 'kcds_ui_right',
  d: 'kcds_ui_right',
  up: 'kcds_ui_up',
  w: 'kcds_ui_up',
  down: 'kcds_ui_down',
  s: 'kcds_ui_down',
});
const DRAWING = Object.freeze([
  'clear',
  'lineStyle',
  'beginFill',
  'endFill',
  'moveTo',
  'lineTo',
  'curveTo',
]);
// Font names the game maps to its own fonts (Libs/UI/fontconfig.xml).
const FONTS = Object.freeze([
  'DefaultFont',
  'DefaultFontItalic',
  'DefaultFontBold',
  'LightFont',
  'LightFontItalic',
  'LightFontBold',
  'Manuscript',
  'DisplayFont',
]);
const DEFAULT_TEXT_STYLE = Object.freeze({
  font: 'DefaultFont',
  size: 20,
  color: 0xffffff,
  align: 'left',
  bold: false,
  italic: false,
});

function textStyle(style) {
  if (style === null || typeof style !== 'object') {
    throw new TypeError('NativeUI: style must be an object');
  }
  const merged = { ...DEFAULT_TEXT_STYLE, ...style };
  if (!FONTS.includes(merged.font)) {
    throw new TypeError(`NativeUI: font must be one of ${FONTS.join(', ')}`);
  }
  if (!Number.isInteger(merged.size) || merged.size < 1 || merged.size > 256) {
    throw new RangeError('NativeUI: size must be an integer from 1 to 256');
  }
  if (!Number.isInteger(merged.color) || merged.color < 0 || merged.color > 0xffffff) {
    throw new RangeError('NativeUI: color must be an RGB integer');
  }
  if (!['left', 'center', 'right', 'justify'].includes(merged.align)) {
    throw new TypeError('NativeUI: align must be left, center, right or justify');
  }
  if (typeof merged.bold !== 'boolean' || typeof merged.italic !== 'boolean') {
    throw new TypeError('NativeUI: bold and italic must be booleans');
  }
  return Object.freeze({
    font: merged.font,
    size: merged.size,
    color: merged.color,
    align: merged.align,
    bold: merged.bold,
    italic: merged.italic,
  });
}

// A file path inside a resource: plain segments, as the game client writes them.
function shippedPath(value) {
  if (
    typeof value !== 'string' ||
    value.length === 0 ||
    value.length > 240 ||
    !value
      .split('/')
      .every((part) => /^[A-Za-z0-9_.-]+$/.test(part) && part !== '.' && part !== '..')
  ) {
    throw new TypeError('NativeUI: a file is a relative path of the resource');
  }
  return value;
}

// A timer that does not keep a Node process alive (tests); a plain timeout in the worker.
function later(callback, ms) {
  const timer = setTimeout(callback, ms);
  if (timer && typeof timer.unref === 'function') {
    timer.unref();
  }
  return timer;
}

function base64(bytes) {
  let text = '';
  for (let offset = 0; offset < bytes.length; offset += 8192) {
    text += String.fromCharCode(...bytes.subarray(offset, offset + 8192));
  }
  return btoa(text);
}

// FNV-1a over the bytes: a file is sent again only when it changed.
function fingerprint(bytes) {
  let hash = 0x811c9dc5;
  for (const byte of bytes) {
    hash = Math.imul(hash ^ byte, 0x01000193) >>> 0;
  }
  return hash;
}

// The game's own menu pages (Client/runtime/native_ui/native_ui.hpp).
const MENU_PAGES = Object.freeze({
  crafting: 1,
  inventory: 2,
  player: 4,
  map: 6,
  codex: 7,
  journal: 10,
});

function menuName(value) {
  if (typeof value !== 'string' || !Object.prototype.hasOwnProperty.call(MENU_PAGES, value)) {
    throw new TypeError(`NativeUI: unknown menu ${String(value)}`);
  }
  return value;
}

function scalar(value, what) {
  if (value === null || typeof value === 'boolean') {
    return value;
  }
  if (typeof value === 'number') {
    if (!Number.isFinite(value)) {
      throw new TypeError(`NativeUI: ${what} must be a finite number`);
    }
    return value;
  }
  if (typeof value === 'string') {
    if (value.length > MAX_STRING) {
      throw new RangeError(`NativeUI: ${what} is longer than ${MAX_STRING} characters`);
    }
    return value;
  }
  throw new TypeError(`NativeUI: ${what} must be a string, number, boolean or null`);
}

// Scalar arguments, as one array or loose, one per parameter.
function argumentList(args, loose) {
  const list = Array.isArray(args) ? args : args === undefined ? loose : [args, ...loose];
  if (list.length > MAX_ARGUMENTS) {
    throw new RangeError(`NativeUI: at most ${MAX_ARGUMENTS} arguments`);
  }
  return list.map((value, index) => scalar(value, `argument ${index + 1}`));
}

// An ActionScript identifier: a clip, member or method name.
function clipName(value) {
  if (typeof value !== 'string' || !/^[A-Za-z_][A-Za-z0-9_]{0,63}$/.test(value)) {
    throw new TypeError('NativeUI: clip, member and method names are ActionScript identifiers');
  }
  return value;
}

function name(value, what) {
  if (typeof value !== 'string' || value.length === 0 || value.length > MAX_NAME) {
    throw new TypeError(`NativeUI: ${what} must be a non-empty string`);
  }
  return value;
}

// Integral numbers reach the movie as integers, other numbers as floats (the game client keeps
// the distinction the message carries).
function wire(value) {
  return value;
}

function encodeArguments(list) {
  return list.map(wire);
}

class ResourceNativeUI {
  constructor({
    sync,
    send,
    log,
    shippedFile = () => null,
    screenSize = () => null,
    input = null,
    emit = () => {},
  }) {
    this.input = input;
    this.emit = emit;
    this.sync = sync;
    this.send = send;
    this.log = log;
    this.shippedFile = shippedFile;
    this.screenSize = screenSize;
    // clip mouse handlers: { owner, screen, key, id, event, handler }
    this.clipHandlers = [];
    this.nextClipId = 1;
    // `${resource}/${path}` -> fingerprint of the copy the game client has
    this.mountedTextures = new Map();
    this.sentFiles = new Map();
    // `${name}#${instance}` -> event -> Map(owner -> [handlers])
    this.elementHandlers = new Map();
    // menu -> owners that disable it
    this.menuHolds = new Map();
    // The resources' own buttons in the game's Esc menu, in the order they came:
    // { id, owner, label, handler }.
    this.menuButtons = [];
    this.nextMenuButton = 1;
    // owner -> ids of its open screens; id -> screen state
    this.screens = new Map();
    this.screenState = new Map();
    this.nextScreen = 1;
    this.focused = null;
    // `${screen}:${clip path}` -> text style of a text field
    this.textStyles = new Map();
    // The game's own screens open now (inventory, map, journal ...), by element name, from the
    // game client's nativeScreen messages.
    this.openGameScreens = new Set();
  }

  setMenuEnabled(owner, menu, enabled) {
    const key = menuName(menu);
    if (typeof enabled !== 'boolean') {
      throw new TypeError('NativeUI: enabled must be a boolean');
    }
    const holders = this.menuHolds.get(key) || new Set();
    const before = holders.size;
    if (enabled) {
      holders.delete(owner);
    } else {
      holders.add(owner);
    }
    if (holders.size === 0) {
      this.menuHolds.delete(key);
    } else {
      this.menuHolds.set(key, holders);
    }
    if ((before === 0) !== (holders.size === 0)) {
      this.sendMenus();
    }
  }

  isMenuEnabled(menu) {
    return !this.menuHolds.has(menuName(menu));
  }

  // A button of the resource's own in the game's Esc menu: the game client adds it
  // each time the menu is built, above "Report a bug". The game has room for MAX_MENU_BUTTONS of
  // them; null when they are taken.
  addMenuButton(owner, label, handler) {
    if (typeof label !== 'string' || label.length === 0 || label.length > MAX_MENU_LABEL) {
      throw new TypeError(
        `NativeUI.addMenuButton: label must be 1 to ${MAX_MENU_LABEL} characters`,
      );
    }
    if (typeof handler !== 'function') {
      throw new TypeError('NativeUI.addMenuButton: handler must be a function');
    }
    if (this.menuButtons.length >= MAX_MENU_BUTTONS) {
      return null;
    }
    const id = this.nextMenuButton++;
    this.menuButtons.push({ id, owner, label, handler });
    this.sendMenuButtons();
    return id;
  }

  removeMenuButton(owner, id) {
    const before = this.menuButtons.length;
    this.menuButtons = this.menuButtons.filter(
      (button) => button.id !== id || button.owner !== owner,
    );
    if (this.menuButtons.length === before) {
      return false;
    }
    this.sendMenuButtons();
    return true;
  }

  sendMenuButtons() {
    const buttons = this.menuButtons.map((button) => ({
      key: String(button.id),
      label: button.label,
    }));
    this.send({ op: 'nativeMenuButtons', buttons });
  }

  // The game's Esc menu (the "Menu" element) showed or went away: { op: 'nativeMenu', open },
  // raised as nativeMenuOpened or nativeMenuClosed.
  receiveMenu({ open }) {
    if (open) {
      this.emit('nativeMenuOpened', []);
    } else {
      this.emit('nativeMenuClosed', []);
    }
  }

  // One of the game's own screens showed or went away: { op: 'nativeScreen', name, open }, raised
  // as nativeScreenOpened(name) or nativeScreenClosed(name) when it changes what is open.
  receiveScreen({ name: screen, open }) {
    if (typeof screen !== 'string' || screen === '' || typeof open !== 'boolean') {
      return;
    }
    if (open === this.openGameScreens.has(screen)) {
      return;
    }
    if (open) {
      this.openGameScreens.add(screen);
      this.emit('nativeScreenOpened', [screen]);
    } else {
      this.openGameScreens.delete(screen);
      this.emit('nativeScreenClosed', [screen]);
    }
  }

  // The player pressed one of the resources' Esc menu buttons: { op: 'nativeMenuButton', key }.
  receiveMenuButton({ key }) {
    const button = this.menuButtons.find((entry) => String(entry.id) === key);
    if (!button) {
      return;
    }
    try {
      button.handler(button.id);
    } catch (error) {
      this.log(button.owner, String(error));
    }
  }

  // Sends a file to the game client, which writes it for the movie loader; once per content.
  sendFile(resource, path, bytes) {
    if (bytes instanceof DiskFile) {
      // A big file the game keeps in its download cache: it copies the file there itself.
      const key = `${resource}/${path}`;
      if (this.sentFiles.get(key) !== bytes.hash) {
        this.send({ op: 'nativeFile', resource, path, cached: bytes.hash });
        this.sentFiles.set(key, bytes.hash);
      }
      return;
    }
    if (bytes.length > MAX_FILE) {
      throw new RangeError(`NativeUI: a file a screen loads is at most ${MAX_FILE} bytes`);
    }
    const key = `${resource}/${path}`;
    const hash = fingerprint(bytes);
    if (this.sentFiles.get(key) === hash) {
      return;
    }
    for (let offset = 0; offset === 0 || offset < bytes.length; offset += FILE_CHUNK) {
      this.send({
        op: 'nativeFile',
        resource,
        path,
        offset,
        total: bytes.length,
        data: base64(bytes.subarray(offset, offset + FILE_CHUNK)),
      });
    }
    this.sentFiles.set(key, hash);
  }

  // A .dds a resource ships, for a book's <img src> (Book.image) or, `itemIcon`, as the icon of
  // a registered item type: the file is sent, the game client packs and mounts it
  // as Libs/UI/Textures/Books/Unique/<name>_ui.dds or Libs/UI/Textures/Icons/Items/
  // <name>_icon.dds, and the name is answered. Throws when the file is missing or larger than
  // 16 MiB.
  // `mapIcon`: a blip's own icon, mounted as Libs/UI/Textures/Icons/Map/<name>_icon.dds
  // with its undiscovered and small copies.
  shipTexture(resource, path, { itemIcon = false, mapIcon = false } = {}) {
    const what = mapIcon ? 'Blip image' : itemIcon ? 'Item icon' : 'Book';
    const file = shippedPath(path);
    const bytes = this.shippedFile(resource, file);
    if (!bytes) {
      throw new Error(`${what}: this resource does not ship ${file}`);
    }
    if (bytes.length > MAX_TEXTURE) {
      throw new RangeError(`${what}: a picture is at most 16 MiB`);
    }
    const key = `${mapIcon ? 'map' : itemIcon ? 'icon' : 'book'}:${resource}/${file}`;
    const hash = bytes instanceof DiskFile ? bytes.hash : fingerprint(bytes);
    if (this.mountedTextures.get(key) !== hash) {
      this.sendFile(resource, file, bytes);
      this.send({
        op: 'nativeMount',
        resource,
        path: file,
        itemIcon,
        ...(mapIcon ? { mapIcon } : {}),
      });
      this.mountedTextures.set(key, hash);
    }
    return bookPictureName(resource, file);
  }

  // The loader's address of a sent file: the game's URL builder takes "/<path>" from the root of
  // the game's file system, where %USER% is this game instance's user folder.
  static fileUrl(resource, path) {
    return `/%USER%/kcds_ui/${resource}/${path}`;
  }

  setFocus(screen) {
    const was = this.focused !== null;
    this.focused = screen;
    if (this.input) {
      // A focused screen takes over the keys, as its focus actions, and pauses Key.bind.
      this.input.paused = screen !== null;
      if (screen !== null && typeof this.input.ensureTable === 'function') {
        this.input.ensureTable(); // the game client sends key edges once a table exists
      }
    }
    if (was !== (screen !== null)) {
      this.send({ op: 'nativeScreenFocus', focused: screen !== null });
    }
  }

  // A screen notification, to its handlers on a later tick.
  raise(id, event, args) {
    const state = this.screenState.get(id);
    if (!state) {
      return;
    }
    for (const handler of [...state.handlers[event]]) {
      setTimeout(() => {
        try {
          handler(...args);
        } catch (error) {
          this.log(state.owner, String(error));
        }
      }, 0);
    }
  }

  // Key edges while a screen holds focus: its 'input' with the action and 'press' or 'release'.
  receiveEdges({ edges }) {
    if (this.focused === null || !Array.isArray(edges)) {
      return;
    }
    for (const [key, edge] of edges) {
      const action = FOCUS_ACTIONS[key];
      if (action && (edge === 'down' || edge === 'up')) {
        this.raise(this.focused, 'input', [action, edge === 'down' ? 'press' : 'release']);
      }
    }
  }

  sendMenus() {
    const disabled = [...this.menuHolds.keys()].map((menu) => MENU_PAGES[menu]);
    this.send({ op: 'nativeMenus', disabled });
  }

  request(body, timeoutMs = undefined) {
    const value = this.sync.call(body, timeoutMs);
    return value === undefined ? null : value;
  }

  // A change whose answer nobody needs: it does not hold the resource while the game is busy
  // (a level loading does not answer for seconds).
  post(body) {
    this.sync.post(body);
  }

  receiveElementEvent({ name: element, instance, event, args }) {
    const key = `${element}#${instance}`;
    const byEvent = this.elementHandlers.get(key);
    const list = Array.isArray(args) ? args : [];
    for (const [owner, handlers] of byEvent ? byEvent.get('*') || [] : []) {
      for (const handler of [...handlers]) {
        try {
          handler([...list], event);
        } catch (error) {
          this.log(owner, String(error));
        }
      }
    }
    const owners = byEvent && byEvent.get(event);
    if (!owners) {
      return;
    }
    for (const [owner, handlers] of owners) {
      for (const handler of [...handlers]) {
        try {
          handler([...list]);
        } catch (error) {
          this.log(owner, String(error));
        }
      }
    }
  }

  listen(owner, element, instance, event, handler) {
    const key = `${element}#${instance}`;
    if (!this.elementHandlers.has(key)) {
      this.elementHandlers.set(key, new Map());
    }
    const byEvent = this.elementHandlers.get(key);
    if (!byEvent.has(event)) {
      byEvent.set(event, new Map());
    }
    const owners = byEvent.get(event);
    if (!owners.has(owner)) {
      owners.set(owner, []);
      this.send({ op: 'nativeElementListen', name: element, instance, event });
    }
    owners.get(owner).push(handler);
  }

  unlisten(owner, element, instance, event) {
    const key = `${element}#${instance}`;
    const owners = this.elementHandlers.get(key)?.get(event);
    if (!owners || !owners.has(owner)) {
      return;
    }
    owners.delete(owner);
    this.send({ op: 'nativeElementUnlisten', name: element, instance, event });
    if (owners.size === 0) {
      this.elementHandlers.get(key).delete(event);
    }
  }

  removeOwner(owner) {
    this.clipHandlers = this.clipHandlers.filter((entry) => entry.owner !== owner);
    for (const id of [...(this.screens.get(owner) || [])]) {
      this.screenState.get(id)?.handle.close();
    }
    this.screens.delete(owner);
    let menus = false;
    for (const [menu, holders] of this.menuHolds) {
      if (holders.delete(owner)) {
        menus = true;
        if (holders.size === 0) {
          this.menuHolds.delete(menu);
        }
      }
    }
    if (menus) {
      this.sendMenus();
    }
    const buttons = this.menuButtons.length;
    this.menuButtons = this.menuButtons.filter((button) => button.owner !== owner);
    if (this.menuButtons.length !== buttons) {
      this.sendMenuButtons();
    }
    for (const [key, byEvent] of this.elementHandlers) {
      const [element, instance] = [
        key.slice(0, key.lastIndexOf('#')),
        Number(key.slice(key.lastIndexOf('#') + 1)),
      ];
      for (const event of [...byEvent.keys()]) {
        this.unlisten(owner, element, instance, event);
      }
    }
  }

  elementClass(owner) {
    const ui = this;
    class NativeElement {
      #name;
      #instance;

      constructor(elementName, instanceId = 0) {
        this.#name = name(elementName, 'element name');
        if (!Number.isInteger(instanceId) || instanceId < 0 || instanceId > 0x7fffffff) {
          throw new RangeError('NativeUI: instanceId must be a non-negative integer');
        }
        this.#instance = instanceId;
      }

      get name() {
        return this.#name;
      }

      get instanceId() {
        return this.#instance;
      }

      #info() {
        const info = ui.request({ op: 'elementInfo', name: this.#name, instance: this.#instance });
        return info && typeof info === 'object' ? info : { valid: false, visible: false };
      }

      get valid() {
        return Boolean(this.#info().valid);
      }

      get visible() {
        return Boolean(this.#info().visible);
      }

      toString() {
        return `NativeElement(${this.#name}#${this.#instance})`;
      }

      #ask(op, fields) {
        return ui.request({ op, name: this.#name, instance: this.#instance, ...fields });
      }

      call(functionName, args = [], ...loose) {
        const list = argumentList(args, loose);
        return this.#ask('elementCall', {
          fn: name(functionName, 'function name'),
          args: encodeArguments(list),
        });
      }

      setVariable(variable, value) {
        return (
          this.#ask('elementSetVariable', {
            variable: name(variable, 'variable name'),
            value: wire(scalar(value, 'value')),
          }) === true
        );
      }

      getVariable(variable) {
        return this.#ask('elementGetVariable', { variable: name(variable, 'variable name') });
      }

      setArray(array, values) {
        if (!Array.isArray(values)) {
          throw new TypeError('NativeUI: values must be an array');
        }
        const list = values.map((value, index) => wire(scalar(value, `value ${index + 1}`)));
        return (
          this.#ask('elementSetArray', { array: name(array, 'array name'), values: list }) === true
        );
      }

      getArray(array) {
        const values = this.#ask('elementGetArray', { array: name(array, 'array name') });
        return Array.isArray(values) ? values : null;
      }

      setClip(clip, options) {
        if (options === null || typeof options !== 'object') {
          throw new TypeError('NativeUI: options must be an object');
        }
        const fields = {};
        for (const key of ['x', 'y', 'rotation', 'scaleX', 'scaleY', 'alpha']) {
          if (options[key] !== undefined) {
            if (typeof options[key] !== 'number' || !Number.isFinite(options[key])) {
              throw new TypeError(`NativeUI: ${key} must be a finite number`);
            }
            fields[key] = options[key];
          }
        }
        if (options.visible !== undefined) {
          if (typeof options.visible !== 'boolean') {
            throw new TypeError('NativeUI: visible must be a boolean');
          }
          fields.visible = options.visible;
        }
        return this.#ask('elementSetClip', { clip: name(clip, 'clip name'), ...fields }) === true;
      }

      getClip(clip) {
        const info = this.#ask('elementGetClip', { clip: name(clip, 'clip name') });
        return info && typeof info === 'object' ? info : null;
      }

      gotoClip(clip, frameOrLabel, play = false) {
        const frame =
          typeof frameOrLabel === 'string'
            ? name(frameOrLabel, 'frame label')
            : Number.isInteger(frameOrLabel) && frameOrLabel >= 1
            ? frameOrLabel
            : null;
        if (frame === null) {
          throw new TypeError('NativeUI: frameOrLabel must be a one-based frame or a label');
        }
        return (
          this.#ask('elementGotoClip', {
            clip: name(clip, 'clip name'),
            frame,
            play: Boolean(play),
          }) === true
        );
      }

      show() {
        return this.#ask('elementState', { action: 'show' }) === true;
      }

      hide() {
        return this.#ask('elementState', { action: 'hide' }) === true;
      }

      askHide() {
        return this.#ask('elementState', { action: 'askHide' }) === true;
      }

      reload() {
        return this.#ask('elementState', { action: 'reload' }) === true;
      }

      unload() {
        return this.#ask('elementState', { action: 'unload' }) === true;
      }

      on(event, handler) {
        if (typeof handler !== 'function') {
          throw new TypeError('NativeUI: handler must be a function');
        }
        ui.listen(owner, this.#name, this.#instance, name(event, 'event name'), handler);
      }

      off(event) {
        ui.unlisten(owner, this.#name, this.#instance, name(event, 'event name'));
      }
    }
    return NativeElement;
  }

  // Screens: each is its own instance of a game movie (the library) with a container clip on its
  // _root that is the screen's root; clips are addressed by keys, their path of names below
  // _root. A host movie on the same _root gives what needs ActionScript objects.
  screenClasses(owner, NativeElement) {
    const ui = this;
    if (!this.screens.has(owner)) {
      this.screens.set(owner, new Set());
    }
    const owned = this.screens.get(owner);

    // Drawing calls run together in one frame (the pen does not last between frames): invoke of
    // a drawing method queues it until the next other operation or the end of this task.
    const drawing = new Map(); // `${screen}|${key}` -> { screen, path, commands }
    let flushScheduled = false;
    const flushDrawing = () => {
      flushScheduled = false;
      const batches = [...drawing.values()];
      drawing.clear();
      for (const batch of batches) {
        send(batch.screen, batch.path, 'draw', { commands: batch.commands });
      }
    };
    const queueDrawing = (screen, path, method, args) => {
      const key = `${screen}|${path.join('.')}`;
      if (!drawing.has(key)) {
        drawing.set(key, { screen, path, commands: [] });
      }
      drawing.get(key).commands.push({ method, args });
      if (!flushScheduled) {
        flushScheduled = true;
        queueMicrotask(() => {
          if (flushScheduled) {
            flushDrawing();
          }
        });
      }
    };
    const request = (screen, path, action, fields = {}) => {
      if (drawing.size > 0) {
        flushDrawing();
      }
      return send(screen, path, action, fields);
    };
    const send = (screen, path, action, fields = {}) => {
      const state = ui.screenState.get(screen);
      if (!state || !state.open) {
        return null;
      }
      return ui.request({
        op: 'elementClip',
        name: state.library,
        instance: SCREEN_INSTANCE_BASE + screen,
        path,
        action,
        ...fields,
      });
    };

    class NativeClip {
      #screen;
      #key;

      constructor(screen, key) {
        if (!Number.isInteger(screen) || screen < 1) {
          throw new TypeError('NativeUI: screen must be a screen id');
        }
        if (
          typeof key !== 'string' ||
          !/^kcds_screen_\d+(\.[A-Za-z_][A-Za-z0-9_]{0,63})*$/.test(key)
        ) {
          throw new TypeError('NativeUI: key must be a clip key of a screen');
        }
        this.#screen = screen;
        this.#key = key;
      }

      get screen() {
        return this.#screen;
      }

      get key() {
        return this.#key;
      }

      get valid() {
        return this.#op('exists') === true;
      }

      toString() {
        return `NativeClip(${this.#screen}:${this.#key})`;
      }

      #path() {
        return this.#key.split('.');
      }

      #op(action, fields = {}) {
        return request(this.#screen, this.#path(), action, fields);
      }

      // The ActionScript path of this clip from the movie's _root.
      #target() {
        return `_root.${this.#key}`;
      }

      #child(name) {
        return new NativeClip(this.#screen, `${this.#key}.${name}`);
      }

      #childName() {
        const state = ui.screenState.get(this.#screen);
        if (!state) {
          return null;
        }
        state.nextChild += 1;
        return `c${state.nextChild}`;
      }

      attach(exportName) {
        const child = this.#childName();
        if (child === null) {
          return null;
        }
        const made =
          this.#op('attach', { symbol: name(exportName, 'export name'), child }) === true;
        return made ? this.#child(child) : null;
      }

      createClip() {
        const child = this.#childName();
        if (child === null) {
          return null;
        }
        return this.#op('create', { child }) === true ? this.#child(child) : null;
      }

      createText(options) {
        if (options === null || typeof options !== 'object') {
          throw new TypeError('NativeUI: options must be an object');
        }
        const box = {};
        for (const key of ['x', 'y', 'width', 'height']) {
          if (typeof options[key] !== 'number' || !Number.isFinite(options[key])) {
            throw new TypeError(`NativeUI: ${key} must be a finite number`);
          }
          box[key] = options[key];
        }
        const child = this.#childName();
        if (child === null || this.#op('text', { child, ...box }) !== true) {
          return null;
        }
        // The game's fonts are embedded (Libs/UI/fontconfig.xml); the host movie gives the field
        // its TextFormat.
        const text = this.#child(child);
        text.setMember('embedFonts', true);
        text.setMember('selectable', false);
        text.setMember('multiline', true);
        text.setMember('wordWrap', true);
        text.setTextStyle({});
        if (options.html !== undefined) {
          text.setText(String(options.html), true);
        } else if (options.text !== undefined) {
          text.setText(String(options.text));
        }
        return text;
      }

      set(options) {
        if (options === null || typeof options !== 'object') {
          throw new TypeError('NativeUI: options must be an object');
        }
        let all = true;
        for (const [field, [member, scale]] of Object.entries(CLIP_FIELDS)) {
          const value = options[field];
          if (value === undefined) {
            continue;
          }
          if (field === 'visible') {
            if (typeof value !== 'boolean') {
              throw new TypeError('NativeUI: visible must be a boolean');
            }
            all = this.setMember(member, value) && all;
            continue;
          }
          if (typeof value !== 'number' || !Number.isFinite(value)) {
            throw new TypeError(`NativeUI: ${field} must be a finite number`);
          }
          all = this.setMember(member, value * scale) && all;
        }
        return all;
      }

      get(member) {
        return this.#op('get', { member: clipName(member) });
      }

      setMember(member, value) {
        return (
          this.#op('set', { member: clipName(member), value: scalar(value, 'value') }) === true
        );
      }

      setTextStyle(options) {
        const styleKey = `${this.#screen}:${this.#key}`;
        const merged = textStyle({ ...(ui.textStyles.get(styleKey) || {}), ...options });
        ui.textStyles.set(styleKey, merged);
        const state = ui.screenState.get(this.#screen);
        return (
          state !== undefined &&
          state.handle.host(
            'kcdsFormat',
            this.#target(),
            merged.font,
            merged.size,
            merged.color,
            merged.align,
            merged.bold,
            merged.italic,
          ) !== undefined
        );
      }

      setText(text, html = false) {
        if (typeof text !== 'string') {
          throw new TypeError('NativeUI: text must be a string');
        }
        if (html) {
          this.setMember('html', true);
          return this.setMember('htmlText', text);
        }
        return this.setMember('text', text);
      }

      getText(html = false) {
        const text = this.get(html ? 'htmlText' : 'text');
        return typeof text === 'string' ? text : null;
      }

      goto(frameOrLabel, play = false) {
        const frame =
          typeof frameOrLabel === 'string'
            ? name(frameOrLabel, 'frame label')
            : Number.isInteger(frameOrLabel) && frameOrLabel >= 1
            ? frameOrLabel
            : null;
        if (frame === null) {
          throw new TypeError('NativeUI: frameOrLabel must be a one-based frame or a label');
        }
        if (!this.valid) {
          return false;
        }
        this.#op('invoke', { method: play ? 'gotoAndPlay' : 'gotoAndStop', args: [frame] });
        return true;
      }

      invoke(method, args = [], ...loose) {
        const list = argumentList(args, loose);
        if (DRAWING.includes(method)) {
          queueDrawing(this.#screen, this.#path(), method, list);
          return null;
        }
        return this.#op('invoke', { method: clipName(method), args: list });
      }

      // ActionScript drawing in one frame: [method, ...args] with clear, lineStyle, beginFill,
      // endFill, moveTo, lineTo or curveTo (separate invoke calls lose the pen between frames).
      draw(commands) {
        if (!Array.isArray(commands) || commands.length > 1024) {
          throw new TypeError('NativeUI: commands must be an array of at most 1024 commands');
        }
        const list = commands.map((command) => {
          if (!Array.isArray(command) || !DRAWING.includes(command[0])) {
            throw new TypeError(`NativeUI: a drawing command is [${DRAWING.join('|')}, ...args]`);
          }
          return { method: command[0], args: argumentList(undefined, command.slice(1)) };
        });
        return this.#op('draw', { commands: list }) === true;
      }

      // The names of this clip's members: child clips, text fields and variables.
      members() {
        const names = this.#op('members');
        return Array.isArray(names) ? names : null;
      }

      on(event, handler) {
        if (!Object.hasOwn(CLIP_EVENTS, event)) {
          throw new TypeError(`NativeUI: clip events are ${Object.keys(CLIP_EVENTS).join(', ')}`);
        }
        if (typeof handler !== 'function') {
          throw new TypeError('NativeUI: handler must be a function');
        }
        const state = ui.screenState.get(this.#screen);
        if (!state || !state.open) {
          return false;
        }
        const watched = ui.clipHandlers.find(
          (entry) => entry.screen === this.#screen && entry.key === this.#key,
        );
        const id = watched ? watched.id : ui.nextClipId++;
        if (!watched) {
          state.handle.watchClips();
          if (state.handle.host('kcdsWatch', this.#target(), id) === undefined) {
            return false;
          }
        }
        ui.clipHandlers.push({
          owner,
          screen: this.#screen,
          key: this.#key,
          id,
          event,
          handler,
          clip: this,
        });
        return true;
      }

      off(event) {
        const mine = (entry) => entry.screen === this.#screen && entry.key === this.#key;
        ui.clipHandlers = ui.clipHandlers.filter(
          (entry) => !(mine(entry) && entry.event === event),
        );
        if (!ui.clipHandlers.some(mine)) {
          ui.screenState.get(this.#screen)?.handle.host('kcdsUnwatch', this.#target());
        }
      }

      #sameScreen(clip) {
        if (clip === null) {
          return '';
        }
        if (!(clip instanceof NativeClip) || clip.screen !== this.#screen) {
          throw new TypeError('NativeUI: the clip must belong to the same screen');
        }
        return `_root.${clip.key}`;
      }

      setMask(mask) {
        const path = this.#sameScreen(mask);
        const state = ui.screenState.get(this.#screen);
        return (
          state !== undefined && state.handle.host('kcdsMask', this.#target(), path) !== undefined
        );
      }

      setHitArea(area) {
        const path = this.#sameScreen(area);
        const state = ui.screenState.get(this.#screen);
        return (
          state !== undefined && state.handle.host('kcdsHit', this.#target(), path) !== undefined
        );
      }

      // A text field's leading: pixels added between its lines (negative pulls them together), so
      // its lines can match rows drawn behind them.
      setLeading(pixels) {
        if (!Number.isInteger(pixels) || pixels < -64 || pixels > 256) {
          throw new TypeError('NativeUI: leading must be whole pixels from -64 to 256');
        }
        const state = ui.screenState.get(this.#screen);
        return (
          state !== undefined &&
          state.handle.host('kcdsLeading', this.#target(), pixels) !== undefined
        );
      }

      // A text field's tab stops, pixels from its left: a tab in its text moves on to the next
      // one, so one field can hold columns.
      setTabStops(stops) {
        const valid =
          Array.isArray(stops) &&
          stops.length <= TAB_STOPS &&
          stops.every((stop) => Number.isInteger(stop) && stop >= 0 && stop <= 8192);
        if (!valid) {
          throw new TypeError(`NativeUI: stops must be at most ${TAB_STOPS} whole pixel offsets`);
        }
        // The host takes all of them: the unused ones go on 64 pixels apart after the last (stops
        // far past the field's width make the game ignore every stop).
        const all = [...stops];
        let next = all.length > 0 ? all[all.length - 1] : 0;
        while (all.length < TAB_STOPS) {
          next += 64;
          all.push(next);
        }
        const state = ui.screenState.get(this.#screen);
        return (
          state !== undefined && state.handle.host('kcdsTabs', this.#target(), ...all) !== undefined
        );
      }

      // img:// or imgps:// for the engine's image loader, or a movie path in the game's archives.
      load(url) {
        if (typeof url !== 'string' || url.length === 0 || url.length > 1024) {
          throw new TypeError('NativeUI: url must be a non-empty string');
        }
        const image = /^img(ps)?:\/\//i.test(url);
        if (!image && (url.includes('..') || url.includes(':') || url.includes('\\'))) {
          throw new TypeError('NativeUI: a movie url is a path inside the game archives');
        }
        const address = image || url.startsWith('/') ? url : `/${url}`;
        if (!this.valid) {
          return false;
        }
        this.#op('invoke', { method: 'loadMovie', args: [address] });
        return true;
      }

      // A shipped file into this clip (createScreen({ movie, assets }) loads through this).
      loadShipped(path) {
        const file = shippedPath(path);
        const bytes = ui.shippedFile(owner, file);
        if (!bytes) {
          throw new Error(`NativeUI: this resource does not ship ${file}`);
        }
        ui.sendFile(owner, file, bytes);
        this.#op('invoke', { method: 'loadMovie', args: [ui.constructor.fileUrl(owner, file)] });
        return true;
      }

      remove() {
        if (this.#path().length <= 1) {
          throw new Error('NativeUI: close the screen instead of removing its root');
        }
        if (!this.valid) {
          return false;
        }
        ui.clipHandlers = ui.clipHandlers.filter(
          (entry) => !(entry.screen === this.#screen && entry.key.startsWith(this.#key)),
        );
        return this.#op('remove') === true;
      }
    }

    class NativeScreen {
      #id;

      constructor(id) {
        if (!Number.isInteger(id) || id < 1) {
          throw new TypeError('NativeUI: id must be a screen id');
        }
        this.#id = id;
      }

      #state() {
        return ui.screenState.get(this.#id);
      }

      get id() {
        return this.#id;
      }

      get valid() {
        const state = this.#state();
        return Boolean(state && state.open && state.element.valid);
      }

      get ready() {
        const state = this.#state();
        return Boolean(state && state.open && state.ready);
      }

      get root() {
        return this.ready ? new NativeClip(this.#id, `kcds_screen_${this.#id}`) : null;
      }

      toString() {
        const state = this.#state();
        return `NativeScreen(${this.#id}:${state ? state.library : 'closed'})`;
      }

      // Calls a function of the screen's host movie; undefined when it is not there.
      host(method, ...args) {
        const state = this.#state();
        if (!state || !state.open) {
          return undefined;
        }
        const path = [`kcds_host_${this.#id}`];
        for (let attempt = 0; attempt < HOST_WAIT_REQUESTS; attempt += 1) {
          if (request(this.#id, path, 'get', { member: 'kcdsVersion' }) === HOST_VERSION) {
            const result = request(this.#id, path, 'invoke', {
              method,
              args: argumentList(undefined, args),
            });
            return result === undefined ? null : result;
          }
        }
        return undefined;
      }

      setVisible(visible = true) {
        if (typeof visible !== 'boolean') {
          throw new TypeError('NativeUI: visible must be a boolean');
        }
        if (!this.ready) {
          return false;
        }
        return this.root.setMember('_visible', visible);
      }

      focus() {
        if (!this.valid) {
          return false;
        }
        ui.setFocus(this.#id);
        return true;
      }

      blur() {
        if (ui.focused === this.#id) {
          ui.setFocus(null);
        }
      }

      hasFocus() {
        return ui.focused === this.#id;
      }

      stage() {
        if (!this.ready) {
          return null;
        }
        const size = String(this.host('kcdsStage') ?? '')
          .split(',')
          .map(Number);
        return size[0] > 0 && size[1] > 0 ? { width: size[0], height: size[1] } : null;
      }

      // A point of the game window as viewport fractions (0 to 1) on the stage: the movie fills the
      // window as large as it fits, centred (the game's fullscreen alignment).
      screenToStage(x, y) {
        if (typeof x !== 'number' || typeof y !== 'number' || !Number.isFinite(x + y)) {
          throw new TypeError('NativeUI: x and y must be finite numbers');
        }
        const stage = this.stage();
        const screen = ui.screenSize();
        if (!stage || !screen || !(screen.width > 0) || !(screen.height > 0)) {
          return null;
        }
        const scale = Math.min(screen.width / stage.width, screen.height / stage.height);
        return {
          x: (x * screen.width - (screen.width - stage.width * scale) / 2) / scale,
          y: (y * screen.height - (screen.height - stage.height * scale) / 2) / scale,
        };
      }

      on(event, handler) {
        if (!SCREEN_EVENTS.includes(event)) {
          throw new TypeError(`NativeUI: screen events are ${SCREEN_EVENTS.join(', ')}`);
        }
        if (typeof handler !== 'function') {
          throw new TypeError('NativeUI: handler must be a function');
        }
        const state = this.#state();
        if (!state) {
          return;
        }
        state.handlers[event].push(handler);
        if (event === 'event' && !state.listening) {
          state.listening = true;
          ui.listen(owner, state.library, SCREEN_INSTANCE_BASE + this.#id, '*', (args, name) => {
            if (name !== 'kcdsClip') {
              ui.raise(this.#id, 'event', [name, args]);
            }
          });
        }
      }

      off(event) {
        const state = this.#state();
        if (state && Object.hasOwn(state.handlers, event)) {
          state.handlers[event] = [];
        }
      }

      // The host movie's fscommand("kcdsClip", "<id>,<event>") reaches the clip handlers.
      watchClips() {
        const state = this.#state();
        if (!state || state.watching) {
          return;
        }
        state.watching = true;
        state.element.on('kcdsClip', (args) => {
          const [id, event] = args.map(String).join(',').split(',');
          for (const entry of ui.clipHandlers.filter(
            (candidate) =>
              candidate.screen === this.#id &&
              String(candidate.id) === id &&
              candidate.event === event,
          )) {
            try {
              entry.handler(entry.clip);
            } catch (error) {
              ui.log(entry.owner, String(error));
            }
          }
        });
      }

      close() {
        const state = this.#state();
        if (!state || !state.open) {
          return false;
        }
        for (const path of [[`kcds_screen_${this.#id}`], [`kcds_host_${this.#id}`]]) {
          request(this.#id, path, 'remove');
        }
        this.blur();
        ui.clipHandlers = ui.clipHandlers.filter((entry) => entry.screen !== this.#id);
        if (state.watching) {
          state.element.off('kcdsClip');
        }
        if (state.listening) {
          ui.unlisten(owner, state.library, SCREEN_INSTANCE_BASE + this.#id, '*');
        }
        state.element.hide();
        state.open = false;
        clearTimeout(state.timer);
        owned.delete(this.#id);
        ui.screenState.delete(this.#id);
        return true;
      }
    }

    const createScreen = (options = {}) => {
      if (options === null || typeof options !== 'object') {
        throw new TypeError('NativeUI: options must be an object');
      }
      const library =
        options.library === undefined || options.library === '' ? DEFAULT_LIBRARY : options.library;
      name(library, 'library');
      const movie = options.movie === undefined ? null : shippedPath(options.movie);
      const assets = options.assets === undefined ? [] : options.assets;
      if (!Array.isArray(assets) || assets.length > 64) {
        throw new TypeError('NativeUI: assets must be an array of at most 64 files');
      }
      if (options.layer !== undefined && !Number.isInteger(options.layer)) {
        throw new TypeError('NativeUI: layer must be an integer');
      }
      for (const path of [...assets.map(shippedPath), ...(movie ? [movie] : [])]) {
        if (!ui.shippedFile(owner, path)) {
          throw new Error(`NativeUI: this resource does not ship ${path}`);
        }
      }
      if (owned.size >= MAX_SCREENS) {
        throw new RangeError(`NativeUI: a resource may hold at most ${MAX_SCREENS} native screens`);
      }
      if (ui.nextScreen > MAX_SCREEN_ID) {
        throw new RangeError('NativeUI: no native screen ids left in this session');
      }
      const id = ui.nextScreen;
      ui.nextScreen += 1;
      const element = new NativeElement(library, SCREEN_INSTANCE_BASE + id);
      const handle = new NativeScreen(id);
      const state = {
        owner,
        library,
        element,
        handle,
        open: true,
        ready: false,
        listening: false,
        watching: false,
        nextChild: 0,
        timer: null,
        handlers: Object.fromEntries(SCREEN_EVENTS.map((event) => [event, []])),
      };
      ui.screenState.set(id, state);
      owned.add(id);
      // Opening steps are posted: in order, without holding the resource while a level loads.
      const post = (path, action, fields = {}) =>
        ui.post({
          op: 'elementClip',
          name: library,
          instance: SCREEN_INSTANCE_BASE + id,
          path,
          action,
          ...fields,
        });
      const postState = (action, fields = {}) =>
        ui.post({
          op: 'elementState',
          name: library,
          instance: SCREEN_INSTANCE_BASE + id,
          action,
          ...fields,
        });
      // Every session numbers its screens from 1, so a screen's element instance is the one the
      // last session's screen used, and the game keeps its movie: a movie left by an earlier
      // session (a tool's, before its Leave) could take no clicks at all. Each screen starts from
      // a fresh movie.
      postState('unload');
      postState('show');
      postState('noCursor');
      if (options.layer !== undefined) {
        // Where the screen sits in the game's own UI stack (the element's layer).
        postState('layer', { layer: options.layer });
      }
      // A screen starts empty: the library movie's own clips are hidden.
      post([], 'isolate');
      post([], 'create', { child: `kcds_screen_${id}`, depth: SCREEN_DEPTH });
      ui.sendFile(HOST_RESOURCE, HOST_FILE, hostMovie());
      post([], 'create', { child: `kcds_host_${id}`, depth: SCREEN_DEPTH - 1 });
      post([`kcds_host_${id}`], 'invoke', {
        method: 'loadMovie',
        args: [ui.constructor.fileUrl(HOST_RESOURCE, HOST_FILE)],
      });
      for (const path of assets.map(shippedPath)) {
        ui.sendFile(owner, path, ui.shippedFile(owner, path));
      }
      if (movie) {
        // The shipped movie's assets sit next to it, so its relative paths find them.
        new NativeClip(id, `kcds_screen_${id}`).loadShipped(movie);
      }
      // ready: the container and the host movie are there, a few frames later.
      let tries = 0;
      const poll = () => {
        if (!state.open) {
          return;
        }
        tries += 1;
        if (!state.ready) {
          const ask = (path, action, fields = {}) =>
            ui.request(
              {
                op: 'elementClip',
                name: library,
                instance: SCREEN_INSTANCE_BASE + id,
                path,
                action,
                ...fields,
              },
              READY_ASK_MS,
            );
          const root = ask([`kcds_screen_${id}`], 'exists');
          if (root === null) {
            // The game is busy (a level loading): ask again later, without counting a try.
            tries -= 1;
            state.timer = later(poll, READY_BUSY_MS);
            return;
          }
          const host = ask([`kcds_host_${id}`], 'get', { member: 'kcdsVersion' });
          if (root === true && host === HOST_VERSION) {
            state.ready = true;
            ui.raise(id, 'ready', []);
          } else if (tries > READY_TRIES) {
            ui.log(owner, `NativeUI: screen ${id} did not become ready`);
            return;
          }
          state.timer = later(poll, READY_INTERVAL_MS);
          return;
        }
        // The movie went away: the element itself, or its movie unloaded (the element stays valid
        // and the screen's container is gone).
        if (!element.valid || request(id, [`kcds_screen_${id}`], 'exists') !== true) {
          state.open = false;
          ui.raise(id, 'unload', []);
          return;
        }
        state.timer = later(poll, UNLOAD_INTERVAL_MS);
      };
      state.timer = later(poll, READY_INTERVAL_MS);
      return handle;
    };

    return { NativeClip, NativeScreen, createScreen };
  }

  facade(owner) {
    const NativeElement = this.elementClass(owner);
    const { NativeClip, NativeScreen, createScreen } = this.screenClasses(owner, NativeElement);
    const ui = this;
    const nativeUI = Object.freeze({
      element(elementName, instanceId = 0) {
        return new NativeElement(elementName, instanceId);
      },
      // The game's own screens open now, by element name (ApseInventoryList, ApseMap, ...).
      openScreens() {
        return [...ui.openGameScreens];
      },
      createScreen,
      // The symbols game movies export for NativeClip.attach: { name, movie, exports } for each
      // UI element named, by default the default host ('' , Menu) and hud; null when unreadable.
      libraries(names = DEFAULT_LIBRARIES) {
        if (!Array.isArray(names) || names.length > 64) {
          throw new TypeError('NativeUI: names must be an array of at most 64 element names');
        }
        return names.map((elementName) => {
          const library = ui.request({
            op: 'elementLibrary',
            name: name(elementName, 'element name'),
          });
          if (!library || typeof library !== 'object') {
            return null;
          }
          return elementName === DEFAULT_LIBRARY ? { ...library, name: '' } : library;
        });
      },
      setMenuEnabled(menu, enabled) {
        ui.setMenuEnabled(owner, menu, enabled);
      },
      isMenuEnabled(menu) {
        return ui.isMenuEnabled(menu);
      },
      addMenuButton(label, handler) {
        return ui.addMenuButton(owner, label, handler);
      },
      removeMenuButton(id) {
        return ui.removeMenuButton(owner, id);
      },
    });
    return { NativeUI: nativeUI, NativeElement, NativeScreen, NativeClip };
  }
}

module.exports = { ResourceNativeUI, MENU_PAGES };
