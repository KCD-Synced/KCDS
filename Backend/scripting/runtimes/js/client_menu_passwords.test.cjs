'use strict';

// The game menu remembers a server's password when the player asks it to
// (Client/ui/menu/index.html): the page holds them in memory and tells the game, which keeps them
// sealed. The page's own functions are read from it and run here.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const PAGE = path.join(__dirname, '../../../../Client/ui/menu/index.html');
const NAMES = ['passwordKey', 'takePasswords', 'rememberedPassword', 'rememberPassword'];

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

function page(bridge) {
  const html = fs.readFileSync(PAGE, 'utf8');
  assert.ok(!/localStorage[^\n]*[Pp]assword/.test(html), 'no password in the page storage');
  const source = NAMES.map((name) => functionSource(html, name)).join('\n');
  const sent = [];
  const menu = new Function(
    'BRIDGE',
    'send',
    `var passwords = {};\n${source}\nreturn { ${NAMES.join(', ')} };`,
  )(bridge, (type, data) => sent.push({ type, ...data }));
  return { menu, sent };
}

test('a password is remembered by server id or address and told to the game', () => {
  const { menu, sent } = page(true);
  assert.equal(menu.rememberedPassword({ id: 'abc' }), '');
  menu.rememberPassword({ id: 'abc' }, 'secret', true);
  menu.rememberPassword({ address: 'Play.Example.org:7777' }, 'other', true);
  assert.equal(menu.rememberedPassword({ id: 'abc' }), 'secret');
  // An address is the same server whatever its case.
  assert.equal(menu.rememberedPassword({ address: 'play.example.org:7777' }), 'other');
  // Joined without Remember password: forgotten, and the game told so.
  menu.rememberPassword({ id: 'abc' }, 'secret', false);
  assert.equal(menu.rememberedPassword({ id: 'abc' }), '');
  assert.deepEqual(sent, [
    { type: 'rememberPassword', key: 'id:abc', password: 'secret' },
    { type: 'rememberPassword', key: 'address:play.example.org:7777', password: 'other' },
    { type: 'rememberPassword', key: 'id:abc', password: '' },
  ]);
});

test("the game's passwords are taken; bad values and unknown targets remember nothing", () => {
  const { menu, sent } = page(false);
  menu.takePasswords({ 'id:abc': 'secret', 'id:bad': 5 });
  assert.equal(menu.rememberedPassword({ id: 'abc' }), 'secret');
  assert.equal(menu.rememberedPassword({ id: 'bad' }), '');
  assert.equal(menu.rememberedPassword({}), '');
  assert.equal(menu.rememberedPassword(null), '');
  menu.takePasswords(null);
  assert.equal(menu.rememberedPassword({ id: 'abc' }), '');
  menu.rememberPassword({}, 'secret', true);
  // Without the game (the design preview) nothing is sent.
  menu.rememberPassword({ id: 'x' }, 'y', true);
  assert.equal(sent.length, 0);
  assert.equal(menu.rememberedPassword({ id: 'x' }), 'y');
});
