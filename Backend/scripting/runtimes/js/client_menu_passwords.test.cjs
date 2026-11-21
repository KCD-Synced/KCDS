'use strict';

// The game menu remembers a server's password when the player asks it to
// (Client/ui/menu/index.html). The page's own functions are read from it and run here.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const PAGE = path.join(__dirname, '../../../../Client/ui/menu/index.html');
const NAMES = ['passwordKey', 'storedPasswords', 'rememberedPassword', 'rememberPassword'];

function functionSource(html, name) {
  const start = html.indexOf(`function ${name}(`);
  assert.ok(start >= 0, `${name} is in the menu page`);
  let depth = 0;
  let end = html.indexOf('{', start);
  for (; end < html.length; end++) {
    if (html[end] === '{') {
      depth++;
    } else if (html[end] === '}') {
      depth--;
      if (depth === 0) {
        break;
      }
    }
  }
  return html.slice(start, end + 1);
}

function page(storage) {
  const html = fs.readFileSync(PAGE, 'utf8');
  const source = NAMES.map((name) => functionSource(html, name)).join('\n');
  const window = {
    localStorage: {
      getItem: (key) => (key in storage ? storage[key] : null),
      setItem: (key, value) => {
        storage[key] = String(value);
      },
    },
  };
  return new Function(
    'window',
    `var PASSWORDS_KEY = 'kcds.serverPasswords';\n${source}\nreturn { ${NAMES.join(', ')} };`,
  )(window);
}

test('a password is remembered by server id or address, and forgotten', () => {
  const storage = {};
  const menu = page(storage);
  assert.equal(menu.rememberedPassword({ id: 'abc' }), '');
  menu.rememberPassword({ id: 'abc' }, 'secret', true);
  menu.rememberPassword({ address: 'Play.Example.org:7777' }, 'other', true);
  assert.equal(menu.rememberedPassword({ id: 'abc' }), 'secret');
  // An address is the same server whatever its case.
  assert.equal(menu.rememberedPassword({ address: 'play.example.org:7777' }), 'other');
  assert.deepEqual(JSON.parse(storage['kcds.serverPasswords']), {
    'id:abc': 'secret',
    'address:play.example.org:7777': 'other',
  });
  // Joined without Remember password: forgotten.
  menu.rememberPassword({ id: 'abc' }, 'secret', false);
  assert.equal(menu.rememberedPassword({ id: 'abc' }), '');
  assert.equal(menu.rememberedPassword({ address: 'play.example.org:7777' }), 'other');
});

test('broken storage and unknown targets remember nothing', () => {
  const storage = { 'kcds.serverPasswords': 'not json' };
  const menu = page(storage);
  assert.equal(menu.rememberedPassword({ id: 'abc' }), '');
  assert.equal(menu.rememberedPassword({}), '');
  assert.equal(menu.rememberedPassword(null), '');
  menu.rememberPassword({}, 'secret', true);
  assert.equal(storage['kcds.serverPasswords'], 'not json');
  // A good write replaces the broken value.
  menu.rememberPassword({ id: 'abc' }, 'secret', true);
  assert.equal(menu.rememberedPassword({ id: 'abc' }), 'secret');
});
