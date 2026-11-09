'use strict';

// The SDK reference for the website, read from the runtime itself so it never drifts from the code:
//
//   node Dev/tools/web/extract_sdk_api.cjs <checkout> <out api.json>
//
// <checkout> is a mod-backend checkout (normally the release tag being documented). The tool starts a probe
// resource in the real server host (Backend/scripting/runtimes/js/host.cjs) and in the real client runtime
// (Client/resources/runtime.cjs), lists every global a resource gets, and walks each one: classes (static and
// instance members), namespaces, methods, getters and values. A member's summary is the comment block written
// above its definition in the source; issue references are left out. Events come from the emit calls in the
// sources. The output is the website's api.json (schema 1, the format kcd2s-gen's docs/api.json uses).

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const SERVER_DIR = 'Backend/scripting/runtimes/js';
const CLIENT_DIR = 'Client/resources';

// Globals every JavaScript resource has from the language or the host, not from the KCDS SDK.
const LANGUAGE_GLOBALS = new Set([
  'console',
  'Buffer',
  'module',
  'require',
  'exports',
  '__filename',
  '__dirname',
  'globalThis',
  'global',
  'queueMicrotask',
]);

// Where each global shows in the docs sidebar, as "Section/Group", in reading order; anything not listed goes under
// "Other". This table wins over a curated file's `group`.
const GROUPS = {
  'Scripting/Events and messages': ['Events', 'Messages', 'Exports', 'Imports'],
  'Scripting/Runtime': ['Timers', 'Runtime', 'SharedState', 'process'],
  'Scripting/Chat and texts': ['Chat', 'Locales'],
  'Players/Players': ['Player', 'Entity'],
  'Players/Looks and stats': ['Appearances', 'PlayerStat', 'DerivedPlayerStat'],
  'Players/Progression': ['Progression', 'Buffs', 'Needs'],
  'Players/Voice chat': ['Voice', 'LocalVoice'],
  'Characters/NPCs': ['Npc', 'PatrolRoute'],
  'Characters/Animals': ['Horse', 'HorseLooks', 'Dog'],
  'Characters/Animations': ['Animations'],
  'World/Time and weather': ['World'],
  'World/Regions and markers': ['Area', 'Marker', 'Blip'],
  'World/Objects': ['Prop', 'PropGhost', 'PropPlacer', 'GroundItem', 'Vfx', 'Light'],
  'World/Doors and containers': ['Door', 'Gate', 'Grave', 'Stash', 'Cart'],
  'World/Level editing': [
    'LevelEdit',
    'WorldResource',
    'Gizmo',
    'SmartObject',
    'Collision',
    'Prefab',
    'Terrain',
    'Vegetation',
  ],
  'World/Siege': ['SiegeEngine', 'SiegeLadder', 'StonePile'],
  'Gameplay/Items': ['Inventory', 'Items'],
  'Gameplay/Quests and talk': ['Quest', 'Dialogue', 'Vendor'],
  'Gameplay/Crafting': ['Crafting', 'Recipe', 'CookPot', 'Gathering', 'Trees'],
  'Gameplay/Activities': [
    'Dice',
    'Pickpocketing',
    'Trade',
    'Tabletop',
    'TabletopPiece',
    'TabletopDeck',
    'DeckPiece',
  ],
  'Server/Rules': [
    'Interactions',
    'ThirdPerson',
    'Freecam',
    'CombatMarker',
    'RestrictedAreaMarker',
  ],
  'Server/Camera': ['Camera'],
  'Server/Hosting': ['Http', 'Queue', 'Bans'],
  'Client/HUD': ['Hud', 'ActionHint', 'ActionMenu', 'Book', 'WorldMap', 'Compass', 'Travel'],
  'Client/Input': ['Key', 'Controls', 'Keybinds'],
  'Client/Screens': [
    'Web',
    'LoadingScreen',
    'NativeUI',
    'NativeScreen',
    'NativeElement',
    'NativeClip',
  ],
  'Client/Camera and previews': [
    'NoClip',
    'CharacterCreator',
    'LocalNpc',
    'LocalPlayer',
    'Gizmos',
    'Render',
  ],
  'Client/Movement': ['Follow'],
  'Client/Audio and social': ['Audio', 'Discord', 'Nameplates', 'EmoteWheel'],
  'Client/Game': ['Game'],
  'Client/Assets': ['Assets'],
  'Math/Math': ['Vector2', 'Vector3', 'Vector4', 'Quaternion', 'Color'],
  'Scripting/Web platform': [],
};

// The standard Web APIs a resource has (fetch and its kin): documented by the web
// platform itself, so the reference names each with one line and a link instead of its members.
const WEB_GLOBALS = {
  fetch: ['Makes an HTTP request and resolves to a Response.', 'API/Window/fetch'],
  Request: ['An HTTP request for fetch.', 'API/Request'],
  Response: ['The answer to a fetch.', 'API/Response'],
  Headers: ['HTTP headers of a Request or a Response.', 'API/Headers'],
  FormData: ['Form fields and files for a request body.', 'API/FormData'],
  URL: ['Parses and builds URLs.', 'API/URL'],
  URLSearchParams: ['The query part of a URL.', 'API/URLSearchParams'],
  AbortController: [
    'Cancels a fetch or another operation through its signal.',
    'API/AbortController',
  ],
  AbortSignal: ['The signal an AbortController (or AbortSignal.timeout) gives.', 'API/AbortSignal'],
  Blob: ['Immutable binary data.', 'API/Blob'],
  File: ['A Blob with a name and a date.', 'API/File'],
  ReadableStream: ['A stream of data to read, such as a response body.', 'API/ReadableStream'],
  WritableStream: ['A stream of data to write.', 'API/WritableStream'],
  TransformStream: ['A stream that changes the data passing through it.', 'API/TransformStream'],
  TextEncoder: ['Encodes text as UTF-8 bytes.', 'API/TextEncoder'],
  TextDecoder: ['Decodes bytes into text.', 'API/TextDecoder'],
  Event: ['An event of an EventTarget.', 'API/Event'],
  EventTarget: ['Something that dispatches events to its listeners.', 'API/EventTarget'],
  DOMException: ['The error the web APIs throw.', 'API/DOMException'],
  WebSocket: ['A WebSocket client connection, closed when the resource stops.', 'API/WebSocket'],
  MessageEvent: ['A message a WebSocket received.', 'API/MessageEvent'],
  CloseEvent: ['The close of a WebSocket.', 'API/CloseEvent'],
  atob: ['Decodes base64 into a binary string.', 'API/Window/atob'],
  btoa: ['Encodes a binary string as base64.', 'API/Window/btoa'],
  structuredClone: ['A deep copy of a value.', 'API/Window/structuredClone'],
  crypto: ['Random values, UUIDs and the Web Crypto API.', 'API/Crypto'],
  performance: ['High-resolution time (performance.now()).', 'API/Performance'],
};
const MDN = 'https://developer.mozilla.org/en-US/docs/Web/';
GROUPS['Scripting/Web platform'] = Object.keys(WEB_GLOBALS);
const GROUP_ORDER = Object.keys(GROUPS);

// The members every class or object has from JavaScript itself.
const SKIP_MEMBERS = new Set(['constructor', 'toString', 'toJSON', 'valueOf']);
// What a function (a class's statics) has besides: an instance getter of the same name is real.
const SKIP_FUNCTION_MEMBERS = new Set(['length', 'name', 'prototype', 'caller', 'arguments']);

const TIMER_GLOBALS = new Set([
  'setTimeout',
  'clearTimeout',
  'setInterval',
  'clearInterval',
  'setImmediate',
  'clearImmediate',
  'nextTick',
  'everyTick',
  'clearEveryTick',
]);

function groupOf(name) {
  for (const [group, names] of Object.entries(GROUPS)) {
    if (names.includes(name)) {
      return group;
    }
  }
  return 'Other';
}

// ---- source text: where a function is written, and the comment above it

function readSources(checkout) {
  const files = [];
  const visit = (dir) => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) {
        visit(full);
      } else if (entry.name.endsWith('.cjs') && !entry.name.endsWith('.test.cjs')) {
        files.push({ file: full, text: fs.readFileSync(full, 'utf8') });
      }
    }
  };
  visit(path.join(checkout, SERVER_DIR));
  visit(path.join(checkout, CLIENT_DIR));
  return files;
}

// Comment lines (//) directly above `index`, joined, with issue references removed.
function commentAbove(text, index) {
  const lines = text.slice(0, index).split('\n');
  lines.pop();
  const comment = [];
  while (lines.length) {
    const line = lines.pop().trim();
    if (!line.startsWith('//')) {
      break;
    }
    comment.unshift(line.replace(/^\/\/\s?/, ''));
  }
  return publicSummary(cleanComment(comment.join(' ')));
}

// A source comment that talks about how KCDS works inside (wire ops, the native host, engine and file
// names) is not public documentation: such a summary is dropped and the curated
// text in sdk-docs/ stands instead. The public docs describe the resource SDK only.
const INTERNAL_WORDS =
  /\bops?\b|\bop:|\bnative\b|\bbridge\b|\bengine\b|\bhost\b|\bregistry\b|\bprotocol\b|\bwire\b|\bdirectory\b|\breference\b|\brefsdk\b|\bframework\b|\bIDA\b|\bhook|\bsnapshot of\b|\bphase\b|\bacceptance\b|\.cjs\b|\.hpp\b|\.cpp\b|\bDocs\//i;

function publicSummary(text) {
  return INTERNAL_WORDS.test(text) ? '' : text;
}

// Source comments are written for developers: issue numbers, file names and a leading "Type.member:" label
// are dropped so only the description of the behaviour stays.
function cleanComment(text) {
  return (
    text
      .replace(/\s*\(a KCDS addition[^)]*\)/gi, '')
      // a note of who asked for it and when
      .replace(/\s*\((?:the )?(?:user|owner)\b[^)]*\d{4}-\d{2}-\d{2}[^)]*\)/gi, '')
      // "A KCDS addition on top of it: ..." starting a sentence: the sentence starts at its next word
      .replace(
        /(^|[.!?]\s+)A KCDS addition\s+([a-z])/g,
        (match, start, letter) => `${start}${letter.toUpperCase()}`,
      )
      .replace(/\s*\ba KCDS addition[,;:]?/gi, '')
      .replace(/\s*A KCDS addition\.?/g, '')
      .replace(/^[A-Z][\w$]*(?:\.[\w$]+)+(?:\s+and\s+[\w$.]+)?\s*:\s*/, '')
      .replace(/\s*\([^)]*\.(?:cjs|cpp|hpp|md|json)[^)]*\)/g, '')
      .replace(/\s*\((?:issues?|#)[^)]*#?\d+[^)]*\)/gi, '')
      .replace(/\s*\bissues? #\d+(?:\s*(?:,|and)\s*#\d+)*/gi, '')
      .replace(/\s*#\d+\b/g, '')
      .replace(/\s+([.,;:])/g, '$1')
      // a parenthesis whose issue reference was dropped: (the config) -> (the config)
      .replace(/,\)/g, ')')
      // a sentence whose last words were dropped ends in one full stop; ranges (1..100) and an
      // ellipsis stay
      .replace(/(?<![.\d])\.\.(?=\s|$)/g, '.')
      .replace(/\s{2,}/g, ' ')
      .trim()
  );
}

// The comment written directly above this exact function in the source. Nothing is guessed from a same-named
// method elsewhere: that usually belongs to another type, and a wrong summary is worse than none (the curated
// descriptions in sdk-docs/ fill the rest).
function findSummary(sources, fn, key) {
  const text = Function.prototype.toString.call(fn);
  if (!text || text.includes('[native code]')) {
    return '';
  }
  // the key in front of an arrow or a method narrows a short body that occurs many times
  const candidates = [`${key}: ${text}`, `${key} = ${text}`, text];
  for (const needle of candidates) {
    const comments = new Set();
    for (const source of sources) {
      let at = source.text.indexOf(needle);
      while (at >= 0) {
        comments.add(commentAbove(source.text, at));
        at = source.text.indexOf(needle, at + needle.length);
      }
    }
    if (comments.size === 1) {
      return [...comments][0];
    }
    // the same body written above different comments: which one is this member's is unknown
    if (comments.size > 1) {
      return '';
    }
  }
  return '';
}

// ---- signatures

function parameterList(fn) {
  const text = Function.prototype.toString.call(fn);
  const patterns = [
    /^(?:async\s*)?function\s*\*?\s*[\w$]*\s*\(([^)]*)\)/,
    /^(?:async\s*)?\(([^)]*)\)\s*=>/,
    /^(?:async\s+)?([\w$]+)\s*=>/,
    /^(?:static\s+)?(?:async\s+)?\*?\s*(?:get\s+|set\s+)?[\w$]+\s*\(([^)]*)\)/,
    /^class\b[^{]*\{[\s\S]*?constructor\s*\(([^)]*)\)/,
  ];
  for (const pattern of patterns) {
    const match = pattern.exec(text);
    if (match) {
      return (match[1] ?? '').replace(/\s+/g, ' ').trim();
    }
  }
  return '';
}

function parseParams(list) {
  if (!list) {
    return [];
  }
  const params = [];
  let depth = 0;
  let current = '';
  for (const char of list) {
    if ('([{'.includes(char)) {
      depth += 1;
    } else if (')]}'.includes(char)) {
      depth -= 1;
    }
    if (char === ',' && depth === 0) {
      params.push(current.trim());
      current = '';
    } else {
      current += char;
    }
  }
  if (current.trim()) {
    params.push(current.trim());
  }
  return params.map((raw) => {
    const [name, defaultValue] = raw.split(/\s*=\s*(.*)/s);
    return {
      name,
      type: '',
      optional: defaultValue !== undefined,
      default: defaultValue === undefined ? null : defaultValue,
      description: '',
    };
  });
}

function valueType(value) {
  if (value === null) {
    return 'null';
  }
  if (Array.isArray(value)) {
    return 'array';
  }
  return typeof value;
}

// ---- walking the globals

function memberBase(side) {
  return {
    type: '',
    params: [],
    returns: '',
    readonly: false,
    side,
    stability: 'stable',
    since: '',
    throws: [],
    raises: [],
    examples: [],
    kcds: true,
  };
}

// 'void' when the function's own body is a block that never returns a value; '' when that cannot be told from the
// source (an arrow with an expression body, native code), so a curated `returns` has to say it.
function returnsOf(fn) {
  const text = Function.prototype.toString.call(fn);
  if (!text || text.includes('[native code]')) {
    return '';
  }
  const open = text.indexOf('{');
  const arrow = text.indexOf('=>');
  if (open < 0 || (arrow >= 0 && arrow < open && !/=>\s*\{/.test(text))) {
    return '';
  }
  // nested functions return for themselves: only a return at the outer body's depth counts
  let depth = 0;
  for (let i = open; i < text.length; i += 1) {
    const char = text[i];
    if (char === '{') {
      depth += 1;
    } else if (char === '}') {
      depth -= 1;
    } else if (depth === 1 && text.startsWith('return', i) && !/\w/.test(text[i - 1] ?? '')) {
      if (!/^return\s*;/.test(text.slice(i))) {
        return '';
      }
    }
  }
  return 'void';
}

function methodMember(sources, side, owner, key, fn, isStatic) {
  const list = parameterList(fn);
  const prefix = isStatic ? `${owner}.` : '';
  return {
    ...memberBase(side),
    kind: 'method',
    name: key,
    signature: `${prefix}${key}(${list})`,
    params: parseParams(list),
    returns: returnsOf(fn),
    summary: findSummary(sources, fn, key),
  };
}

function sameParams(a, b) {
  const names = (member) => member.params.map((param) => param.name).join(',');
  return names(a) === names(b);
}

// A side's own call of a member the other side has too: its signature, parameters and return.
function variantOf(member) {
  return {
    signature: member.signature,
    params: member.params,
    returns: member.returns,
    summary: member.summary,
  };
}

function propertyMember(sources, side, owner, key, descriptor, sampleValue) {
  const getter = descriptor.get;
  let summary = '';
  if (getter) {
    summary = findSummary(sources, getter, `get ${key}`);
  } else {
    summary = '';
  }
  const type = getter ? '' : valueType(sampleValue);
  return {
    ...memberBase(side),
    kind: 'property',
    name: key,
    signature: type ? `${key}: ${type}` : key,
    type,
    readonly: Boolean(getter && !descriptor.set),
    summary,
  };
}

function ownKeys(value) {
  const isFunction = typeof value === 'function';
  return Object.getOwnPropertyNames(value).filter((key) => {
    if (SKIP_MEMBERS.has(key) || key.startsWith('_')) {
      return false;
    }
    return !(isFunction && SKIP_FUNCTION_MEMBERS.has(key));
  });
}

// The members of one global: statics, instance members of a class, or the entries of a namespace object
// (nested namespaces are flattened as "sub.member").
function membersOf(sources, side, name, value) {
  const members = [];
  const visitObject = (object, prefix, depth) => {
    for (const key of ownKeys(object)) {
      const descriptor = Object.getOwnPropertyDescriptor(object, key);
      const label = prefix ? `${prefix}.${key}` : key;
      let current;
      try {
        current = descriptor.get ? undefined : object[key];
      } catch {
        current = undefined;
      }
      // A namespace's entries and a class's statics: TypeScript declares them static.
      if (typeof current === 'function') {
        members.push({ ...methodMember(sources, side, name, label, current, true), static: true });
      } else if (current && typeof current === 'object' && !Array.isArray(current) && depth < 2) {
        visitObject(current, label, depth + 1);
      } else {
        members.push({
          ...propertyMember(sources, side, name, label, descriptor, current),
          static: true,
        });
      }
    }
  };
  if (typeof value === 'function') {
    visitObject(value, '', 0);
    const seen = new Set();
    for (
      let proto = value.prototype;
      proto && proto !== Object.prototype;
      proto = Object.getPrototypeOf(proto)
    ) {
      for (const key of ownKeys(proto)) {
        if (seen.has(key)) {
          continue;
        }
        seen.add(key);
        const descriptor = Object.getOwnPropertyDescriptor(proto, key);
        if (typeof descriptor.value === 'function') {
          members.push({
            ...methodMember(sources, side, name, key, descriptor.value, false),
            static: false,
          });
        } else {
          members.push({
            ...propertyMember(sources, side, name, key, descriptor, undefined),
            static: false,
          });
        }
      }
    }
  } else if (value && typeof value === 'object') {
    visitObject(value, '', 0);
  }
  return members;
}

// ---- events

// The text between the parenthesis that opens at `start` and its matching close.
function balancedArguments(text, start) {
  let depth = 0;
  for (let i = start; i < text.length; i += 1) {
    const char = text[i];
    if (char === '(') {
      depth += 1;
    } else if (char === ')') {
      depth -= 1;
      if (depth === 0) {
        return text.slice(start + 1, i);
      }
    }
  }
  return '';
}

// A readable name for an argument expression: `this.event(match)` -> event, `Object.freeze({...})` -> details.
function argumentName(expression) {
  const text = expression.trim();
  if (/^(?:Object\.)?freeze\(|^\{/.test(text)) {
    return 'details';
  }
  const call = /([\w$]+)\(/.exec(text);
  const plain = /^[\w$.]+$/.test(text) ? text.split('.').pop() : null;
  return plain ?? (call ? call[1] : 'value');
}

function eventMembers(sources, side, folder) {
  const events = new Map();
  // host.emit, emitAsync and emitCollecting, and the modules' own relays to it: this.events.emit
  // (crafting) and this.raise (areas). The name is the first argument: a string, or a choice
  // between strings (state === DEAD ? 'horseDeath' : 'horseKnockedOut'), so every event name
  // written there counts.
  const pattern = /\b(?:host|this|events)\.(?:emit(?:Async|Collecting)?|raise)\(/g;
  for (const source of sources) {
    if (!source.file.includes(path.normalize(folder))) {
      continue;
    }
    for (const match of source.text.matchAll(pattern)) {
      const open = match.index + match[0].length - 1;
      const parts = parseParams(balancedArguments(source.text, open));
      const first = parts[0]
        ? `${parts[0].name}${parts[0].default === null ? '' : `=${parts[0].default}`}`
        : '';
      const names = [...first.matchAll(/'([a-z][a-zA-Z]+)'/g)].map((m) => m[1]);
      const args = parts
        .slice(1)
        .map((param) =>
          argumentName(param.default === null ? param.name : `${param.name}=${param.default}`),
        )
        .filter(Boolean);
      for (const eventName of names) {
        if (!/[A-Z]/.test(eventName) || events.has(eventName)) {
          continue;
        }
        events.set(eventName, {
          ...memberBase(side),
          kind: 'event',
          name: eventName,
          signature: `on(event: "${eventName}", cb: (${args.join(', ')}) => void): void`,
          params: args.map((arg) => ({
            name: arg,
            type: '',
            optional: false,
            default: null,
            description: '',
          })),
          summary: commentAbove(source.text, match.index),
        });
      }
    }
  }
  return [...events.values()].sort((a, b) => a.name.localeCompare(b.name));
}

// The client runtime's own events: its modules call this.emit('name', [args]) or the shared bus with
// services.emit('shared', 'name', [args]).
function clientEventMembers(sources) {
  const events = new Map();
  const patterns = [
    /\bthis\.emit\(\s*'([a-z][a-zA-Z]+)'\s*,\s*\[/g,
    /\bthis\.raise\(\s*'([a-z][a-zA-Z]+)'\s*,\s*\[/g,
    /\.emit\(\s*'shared'\s*,\s*'([a-z][a-zA-Z]+)'\s*,\s*\[/g,
  ];
  for (const source of sources) {
    if (!source.file.includes(path.normalize(CLIENT_DIR))) {
      continue;
    }
    for (const pattern of patterns) {
      for (const match of source.text.matchAll(pattern)) {
        const eventName = match[1];
        if (events.has(eventName)) {
          continue;
        }
        const bracket = match.index + match[0].length - 1;
        const inner = balancedBrackets(source.text, bracket);
        const args = parseParams(inner).map((param) =>
          argumentName(param.default === null ? param.name : `${param.name}=${param.default}`),
        );
        events.set(eventName, {
          ...memberBase('client'),
          kind: 'event',
          name: eventName,
          signature: `on(event: "${eventName}", cb: (${args.join(', ')}) => void): void`,
          params: args.map((arg) => ({
            name: arg,
            type: '',
            optional: false,
            default: null,
            description: '',
          })),
          summary: commentAbove(source.text, match.index),
        });
      }
    }
  }
  return [...events.values()];
}

function balancedBrackets(text, start) {
  let depth = 0;
  for (let i = start; i < text.length; i += 1) {
    if (text[i] === '[' || text[i] === '(') {
      depth += 1;
    } else if (text[i] === ']' || text[i] === ')') {
      depth -= 1;
      if (depth === 0) {
        return text.slice(start + 1, i);
      }
    }
  }
  return '';
}

// ---- probe resources: the globals a resource really gets

async function serverGlobals(checkout) {
  const { ResourceHost } = require(path.join(checkout, SERVER_DIR, 'host.cjs'));
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-sdk-probe-'));
  const folder = path.join(root, 'sdk-probe');
  fs.mkdirSync(folder);
  fs.writeFileSync(
    path.join(folder, 'package.json'),
    JSON.stringify({ name: 'sdk-probe', kcds: { serverScripts: ['main.js'] } }),
  );
  fs.writeFileSync(path.join(folder, 'main.js'), '');
  const host = new ResourceHost(root);
  await host.startAll();
  const context = host.resources.get('sdk-probe').vm;
  const globals = {};
  for (const key of Object.keys(context)) {
    globals[key] = context[key];
  }
  return flattenNamespace(globals);
}

// The walk below lists types by name; the `kcds` object only groups them.
function flattenNamespace(globals) {
  const { kcds, ...flat } = globals;
  for (const domain of Object.values(kcds ?? {})) {
    for (const key of Object.keys(domain)) {
      try {
        flat[key] = domain[key];
      } catch {
        flat[key] = undefined;
      }
    }
  }
  return flat;
}

async function clientGlobals(checkout) {
  const { BrowserResourceRuntime } = require(path.join(checkout, CLIENT_DIR, 'runtime.cjs'));
  const { normalizeManifest } = require(path.join(checkout, SERVER_DIR, 'manifest.cjs'));
  let captured = null;
  const runtime = new BrowserResourceRuntime({
    evaluate(code, globals) {
      captured = globals;
    },
  });
  const probe = {
    hash: 'sdk-probe',
    manifest: normalizeManifest({ name: 'sdk-probe', kcds: { clientScripts: ['main.js'] } }),
    files: [{ name: 'main.js' }],
  };
  await runtime.apply({ resources: [probe] }, () => new Uint8Array());
  const globals = {};
  for (const key of Object.keys(captured ?? {})) {
    try {
      globals[key] = captured[key];
    } catch {
      globals[key] = undefined;
    }
  }
  await runtime.disconnect();
  return flattenNamespace(globals);
}

// ---- assembling api.json

// Instance properties a side's host gives every handle of a class at run time instead of through
// its prototype, so the walk of the prototype chain cannot see them: on the server
// host.bindEntityState gives every networked entity its id and state.
const BOUND_MEMBERS = {
  server: {
    Entity: ['id', 'state'],
    Door: ['guid', 'name', 'dimension', 'lockKey', 'canBePicked', 'builtLocked'],
  },
  client: { Entity: ['id'] },
};

// The global class a class inherits from (Player, Npc, Door and the other handles inherit from
// Entity), or '' (the declarations say `extends`).
function baseOf(name, value, globals) {
  if (typeof value !== 'function' || !value.prototype) {
    return '';
  }
  const parent = Object.getPrototypeOf(value.prototype);
  for (const [other, candidate] of Object.entries(globals)) {
    if (other !== name && typeof candidate === 'function' && candidate.prototype === parent) {
      return other;
    }
  }
  return '';
}

function typeEntry(name, members, side) {
  return {
    name,
    base: '',
    group: groupOf(name),
    summary: '',
    since: '',
    side,
    members,
  };
}

async function main() {
  const [checkout, out, ...rest] = process.argv.slice(2);
  if (!checkout || !out) {
    console.error(
      'usage: node extract_sdk_api.cjs <checkout> <out api.json> [--version x.y.z] [--report missing.txt]',
    );
    process.exit(2);
  }
  const option = (flag) => {
    const at = rest.indexOf(flag);
    return at >= 0 ? rest[at + 1] : undefined;
  };
  const version = option('--version');
  const report = option('--report');
  const root = path.resolve(checkout);
  const sources = readSources(root);
  const server = await serverGlobals(root);
  const client = await clientGlobals(root);

  const types = new Map();
  const timers = [];
  const add = (side, globals) => {
    for (const [name, value] of Object.entries(globals)) {
      if (LANGUAGE_GLOBALS.has(name)) {
        continue;
      }
      if (Object.hasOwn(WEB_GLOBALS, name)) {
        const existing = types.get(name);
        if (existing) {
          existing.side = existing.side === side ? side : 'shared';
          continue;
        }
        const [summary, page] = WEB_GLOBALS[name];
        const entry = typeEntry(name, [], side);
        entry.summary = `${summary} The standard Web API, as in browsers and Node.js: see [MDN](${MDN}${page}).`;
        types.set(name, entry);
        continue;
      }
      if (TIMER_GLOBALS.has(name)) {
        const timer = timers.find((known) => known.name === name);
        if (!timer) {
          timers.push(methodMember(sources, side, 'Timers', name, value, false));
        } else if (timer.side !== side) {
          // setTimeout and the others client scripts have as well.
          timer.side = 'shared';
        }
        continue;
      }
      const members = membersOf(sources, side, name, value);
      // Properties the host binds on every handle rather than its prototype.
      for (const key of BOUND_MEMBERS[side]?.[name] ?? []) {
        if (!members.some((member) => member.name === key)) {
          const descriptor = { get() {}, enumerable: true, configurable: false };
          members.push({
            ...propertyMember(sources, side, name, key, descriptor, undefined),
            static: false,
          });
        }
      }
      const existing = types.get(name);
      if (!existing) {
        const entry = typeEntry(name, members, side);
        entry.base = baseOf(name, value, globals);
        types.set(name, entry);
        continue;
      }
      existing.base = existing.base || baseOf(name, value, globals);
      // on both sides: one type, each member marked with the sides that have it
      existing.side = 'shared';
      for (const member of members) {
        const same = existing.members.find((m) => m.name === member.name && m.kind === member.kind);
        if (!same) {
          existing.members.push(member);
          continue;
        }
        if (same.side !== member.side && member.kind === 'method' && !sameParams(same, member)) {
          // the same name taking other parameters on this side (World.raycast: the server asks a
          // player's game, a client script its own): one entry, with this side's own call kept
          same.variants = { ...(same.variants ?? {}), [member.side]: variantOf(member) };
        }
        same.side = same.side === member.side ? same.side : 'shared';
      }
    }
  };
  add('server', server);
  add('client', client);

  const events = types.get('Events') ?? typeEntry('Events', [], 'shared');
  events.members.push(...eventMembers(sources, 'server', SERVER_DIR));
  for (const clientEvent of clientEventMembers(sources)) {
    const both = events.members.find((m) => m.kind === 'event' && m.name === clientEvent.name);
    if (both) {
      both.side = 'shared';
    } else {
      events.members.push(clientEvent);
    }
  }
  events.members.sort((a, b) =>
    a.kind === b.kind ? a.name.localeCompare(b.name) : a.kind === 'event' ? 1 : -1,
  );
  types.set('Events', events);
  if (timers.length) {
    types.set('Timers', typeEntry('Timers', timers, 'shared'));
  }

  const unknown = applyCurated(types, readCurated(CURATED_DIR));
  const { DOMAIN_OF } = require(path.join(root, SERVER_DIR, 'sdk_namespace.cjs'));
  for (const type of types.values()) {
    if (groupOf(type.name) !== 'Other') {
      type.group = groupOf(type.name);
    }
    if (DOMAIN_OF.has(type.name)) {
      type.domain = DOMAIN_OF.get(type.name);
    }
  }
  // Sidebar order: the table's groups in order, then the rest by name; inside a group, the table's order.
  const rank = (type) => {
    const index = GROUP_ORDER.indexOf(type.group);
    return index < 0 ? GROUP_ORDER.length : index;
  };
  const position = (type) => (GROUPS[type.group] ?? []).indexOf(type.name);
  const list = [...types.values()].sort(
    (a, b) =>
      rank(a) - rank(b) ||
      a.group.localeCompare(b.group) ||
      position(a) - position(b) ||
      a.name.localeCompare(b.name),
  );
  const api = {
    schema: 1,
    generator: 'extract_sdk_api',
    apiVersion: version ?? '',
    types: list,
  };
  fs.mkdirSync(path.dirname(path.resolve(out)), { recursive: true });
  fs.writeFileSync(out, JSON.stringify(api, null, 1) + '\n');

  const missing = [];
  for (const type of list) {
    if (!type.summary) {
      missing.push(`${type.name} (type)`);
    }
    for (const member of type.members) {
      if (!member.summary) {
        missing.push(`${type.name}.${member.name}`);
      }
    }
  }
  const memberCount = list.reduce((sum, type) => sum + type.members.length, 0);
  console.log(
    `extract_sdk_api: ${list.length} types, ${memberCount} members, ${missing.length} without a description`,
  );
  for (const entry of unknown) {
    console.log(`  curated entry for something the SDK does not have: ${entry}`);
  }
  if (report) {
    fs.writeFileSync(report, missing.join('\n') + '\n');
  }
  process.exit(0);
}

// ---- curated descriptions (sdk-docs/<Type>.json): written by hand from the source, in English

const CURATED_DIR = path.join(__dirname, 'sdk-docs');

function readCurated(dir) {
  if (!fs.existsSync(dir)) {
    return [];
  }
  return fs
    .readdirSync(dir)
    .filter((name) => name.endsWith('.json'))
    .sort()
    .map((name) => ({
      file: name,
      data: JSON.parse(fs.readFileSync(path.join(dir, name), 'utf8')),
    }));
}

// A curated entry's example in each language a resource can be written in: `example` (JavaScript),
// `exampleLua` and `exampleCs`, on one side.
function examplesOf(entry, side) {
  const examples = [{ lang: 'js', side, code: entry.example }];
  if (entry.exampleLua) {
    examples.push({ lang: 'lua', side, code: entry.exampleLua });
  }
  if (entry.exampleCs) {
    examples.push({ lang: 'cs', side, code: entry.exampleCs });
  }
  return examples;
}

// Curated text wins over source comments. Returns the curated entries that name nothing in the SDK, so a renamed
// or removed member is noticed instead of documenting something that no longer exists.
function applyCurated(types, curated) {
  const unknown = [];
  for (const { file, data } of curated) {
    const type = types.get(data.type);
    if (!type) {
      unknown.push(`${file}: type ${data.type}`);
      continue;
    }
    if (data.summary) {
      type.summary = data.summary;
    }
    if (data.group) {
      type.group = data.group;
    }
    // Fields each handle carries itself (set when the object is made, e.g. a player's id and nickname): a probe
    // with no player connected cannot see them, so the curated file names them.
    for (const [name, entry] of Object.entries(data.fields ?? {})) {
      const seen = type.members.find((m) => m.name === name);
      if (seen) {
        // a getter on one side (the client's handle) and a plain field on the other: one entry for both
        seen.side = 'shared';
        seen.summary = entry.summary ?? seen.summary;
        seen.type = entry.type ?? seen.type;
        continue;
      }
      type.members.push({
        ...memberBase(entry.side ?? type.side),
        kind: 'property',
        name,
        signature: entry.type ? `${name}: ${entry.type}` : name,
        type: entry.type ?? '',
        readonly: entry.readonly !== false,
        summary: entry.summary ?? '',
      });
    }
    // `propertyTypes`: { name: type } for properties whose entry only lacks its type
    for (const [name, propertyType] of Object.entries(data.propertyTypes ?? {})) {
      const member = type.members.find((m) => m.name === name);
      if (member) {
        member.type = propertyType;
      } else {
        unknown.push(`${file}: ${data.type}.${name} (propertyTypes)`);
      }
    }
    // `methodReturns`: { name: type } for methods whose entry only lacks what they return
    for (const [name, returned] of Object.entries(data.methodReturns ?? {})) {
      const member = type.members.find((m) => m.name === name);
      if (member) {
        member.returns = returned;
      } else {
        unknown.push(`${file}: ${data.type}.${name} (methodReturns)`);
      }
    }
    for (const [name, entry] of Object.entries(data.members ?? {})) {
      const member = type.members.find((m) => m.name === name);
      if (!member) {
        unknown.push(`${file}: ${data.type}.${name}`);
        continue;
      }
      if (entry.summary) {
        member.summary = entry.summary;
      }
      if (entry.returns) {
        member.returns = entry.returns;
      }
      if (entry.type) {
        member.type = entry.type;
      }
      if (entry.since) {
        member.since = entry.since;
      }
      if (entry.stability) {
        member.stability = entry.stability;
      }
      if (entry.kcds !== undefined) {
        member.kcds = entry.kcds;
      }
      // an event's arguments as the handler receives them ([name, description] pairs): the emit call in the
      // source may build them inline, which reads badly as a parameter list
      if (Array.isArray(entry.args)) {
        member.params = entry.args.map(([argName, description]) => ({
          name: argName,
          type: '',
          optional: false,
          default: null,
          description,
        }));
        const list = member.params.map((p) => p.name).join(', ');
        member.signature =
          member.kind === 'event'
            ? `on(event: "${name}", cb: (${list}) => void): void`
            : `${name}(${list})`;
      }
      if (entry.cancellable) {
        member.raises = [];
        member.throws = [];
        member.summary = `${member.summary} ${entry.cancellable}`.trim();
      }
      // a wrapper that takes (...args) hides the real parameters: the curated names stand for them
      const curatedParams = Object.keys(entry.params ?? {});
      const wrapped = member.params.length === 1 && member.params[0].name === '...args';
      if (wrapped && curatedParams.some((param) => param !== '...args')) {
        member.params = curatedParams.map((param) => ({
          name: param,
          type: '',
          optional: false,
          default: null,
          description: '',
        }));
        member.signature = member.signature.replace(
          /\(\.\.\.args\)/,
          `(${curatedParams.join(', ')})`,
        );
      }
      for (const [param, description] of Object.entries(entry.params ?? {})) {
        const target = member.params.find((p) => p.name === param);
        if (target) {
          target.description = description;
        } else {
          unknown.push(`${file}: ${data.type}.${name} parameter ${param}`);
        }
      }
      if (entry.example) {
        member.examples = examplesOf(entry, member.side === 'client' ? 'client' : 'server');
      }
    }
    // `clientMembers`: the client's own call of a member both sides have under one name with other
    // parameters (variants.client): its summary, parameters, return and example
    for (const [name, entry] of Object.entries(data.clientMembers ?? {})) {
      const member = type.members.find((m) => m.name === name);
      const variant = member?.variants?.client;
      if (!variant) {
        unknown.push(`${file}: ${data.type}.${name} (clientMembers)`);
        continue;
      }
      if (entry.summary) {
        variant.summary = entry.summary;
      }
      if (entry.returns) {
        variant.returns = entry.returns;
      }
      for (const [param, description] of Object.entries(entry.params ?? {})) {
        const target = variant.params.find((p) => p.name === param);
        if (target) {
          target.description = description;
        } else {
          unknown.push(`${file}: ${data.type}.${name} client parameter ${param}`);
        }
      }
      if (entry.example) {
        member.examples = [...member.examples, ...examplesOf(entry, 'client')];
      }
    }
  }
  return unknown;
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
