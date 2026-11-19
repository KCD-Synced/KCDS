'use strict';

// The game menu's server list (Client/ui/menu/index.html): which servers match this game's
// version and which side has to update. The page's own functions are read from it and run here.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const PAGE = path.join(__dirname, '../../../../Client/ui/menu/index.html');

function pageFunctions(version, protocol) {
  const html = fs.readFileSync(PAGE, 'utf8');
  const source = ['networkVersion', 'compatible', 'serverNewer'].map((name) => {
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
  });
  const make = new Function(
    'state',
    `${source.join('\n')}\nreturn { compatible: compatible, serverNewer: serverNewer };`,
  );
  return make({ version, protocol });
}

test('a server of another release does not match even with the same protocol', () => {
  const { compatible } = pageFunctions('1.1.2', 3);
  assert.equal(compatible({ version: '1.1.2', protocol: 3 }), true);
  assert.equal(compatible({ version: '1.1.1', protocol: 3 }), false);
  assert.equal(compatible({ version: '1.1.2', protocol: 2 }), false);
});

test('a client-only hotfix matches the servers of its release', () => {
  assert.equal(pageFunctions('1.1.2.1', 3).compatible({ version: '1.1.2', protocol: 3 }), true);
  assert.equal(pageFunctions('1.1.2', 3).compatible({ version: '1.1.2.1', protocol: 3 }), true);
});

test('a server that tells no version or protocol is not turned away', () => {
  const { compatible } = pageFunctions('1.1.2', 3);
  assert.equal(compatible({}), true);
  assert.equal(compatible({ protocol: 3 }), true);
  assert.equal(pageFunctions('', 3).compatible({ version: '1.1.1', protocol: 3 }), true);
});

test('the side to update follows the release, part by part as numbers', () => {
  const { serverNewer } = pageFunctions('1.1.2', 3);
  assert.equal(serverNewer({ version: '1.1.1', protocol: 3 }), false);
  assert.equal(serverNewer({ version: '1.1.10', protocol: 3 }), true);
  assert.equal(serverNewer({ version: '1.2.0', protocol: 3 }), true);
  assert.equal(serverNewer({ version: '0.9.9', protocol: 4 }), true, 'a newer protocol wins');
  assert.equal(serverNewer({ version: '2.0.0', protocol: 2 }), false, 'an older protocol loses');
});
