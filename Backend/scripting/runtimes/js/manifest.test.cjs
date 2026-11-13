'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict');
const fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { normalizeManifest, scriptEntries, clientFiles, clientFileSet } = require('./manifest.cjs');
const { ResourceHost } = require('./host.cjs');

test('manifest defaults and legacy entries preserve shared/side ordering without mutating input', () => {
  const input = {
    name: 'mode',
    kcds: {
      server: 'old.js',
      serverScripts: ['new.js'],
      client: 'ui/old.js',
      clientScripts: ['ui/new.js'],
      sharedScripts: ['common.js'],
      files: ['ui/**'],
      clientFiles: ['fonts/*'],
    },
  };
  const snapshot = JSON.stringify(input),
    m = normalizeManifest(input);
  assert.deepEqual(scriptEntries(m, 'server'), ['common.js', 'old.js', 'new.js']);
  assert.deepEqual(scriptEntries(m, 'client'), ['common.js', 'ui/old.js', 'ui/new.js']);
  assert.deepEqual(m.kcds.files, ['ui/**', 'fonts/*']);
  assert.equal(JSON.stringify(input), snapshot);
  const defaults = normalizeManifest({ name: 'empty' });
  assert.equal(defaults.version, '1.0.0');
  assert.deepEqual(defaults.kcds.requires, []);
  assert.deepEqual(defaults.kcds.exports, []);
  assert.deepEqual(defaults.kcds.files, []);
  assert.equal(defaults.kcds.errorBehavior, 'stop');
  assert.equal(defaults.kcds.priority, 0);
});

test('dependency versions are recorded, optional defaults false, invalid manifests fail before execution', () => {
  const m = normalizeManifest({
    name: 'mode',
    kcds: {
      requires: ['core', { name: 'other', version: '^4', optional: true }],
      priority: 900,
    },
  });
  assert.deepEqual(m.kcds.requires, [
    { name: 'core', optional: false },
    { name: 'other', version: '^4', optional: true },
  ]);
  for (const config of [
    { serverScripts: 'one.js' },
    { server: [] },
    { sharedScripts: ['*.js'] },
    { clientScripts: ['../secret.js'] },
    { clientScripts: ['C:\\secret.js'] },
    { files: ['/etc/**'] },
    { files: ['file.js:stream'] },
    { requires: [{ name: 'a', optional: 'yes' }] },
    { errorBehavior: 'ignore' },
    { priority: NaN },
  ])
    assert.throws(() => normalizeManifest({ name: 'bad', kcds: config }));
});

test('client file selection includes shared/legacy scripts, glob assets and excludes server entries', () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-manifest-'));
  const write = (name) => {
    fs.mkdirSync(path.dirname(path.join(root, name)), { recursive: true });
    fs.writeFileSync(path.join(root, name), '');
  };
  try {
    [
      'common.js',
      'server.js',
      'client/main.js',
      'client/deep/helper.js',
      'assets/main.css',
      'assets/deep/font.woff',
      'secret.txt',
    ].forEach(write);
    const m = normalizeManifest({
      name: 'mode',
      kcds: {
        serverScripts: ['server.js'],
        client: 'client/main.js',
        sharedScripts: ['common.js'],
        files: ['assets/**/*.css'],
        clientFiles: ['assets/**/*.woff'],
      },
    });
    assert.deepEqual(clientFiles(root, m), [
      'assets/deep/font.woff',
      'assets/main.css',
      'client/main.js',
      'common.js',
    ]);
    const defaults = normalizeManifest({
      name: 'mode',
      kcds: {
        clientScripts: ['client/main.js'],
        sharedScripts: ['common.js'],
        serverScripts: ['client/deep/helper.js'],
      },
    });
    assert.deepEqual(clientFiles(root, defaults), ['client/main.js', 'common.js']);
    const broad = normalizeManifest({
      name: 'mode',
      kcds: { clientScripts: ['client/main.js'], serverScripts: ['server.js'], files: ['**'] },
    });
    assert.ok(!clientFiles(root, broad).includes('server.js'));
    assert.throws(
      () =>
        clientFiles(
          root,
          normalizeManifest({ name: 'mode', kcds: { clientScripts: ['missing.js'] } }),
        ),
      /ENOENT/,
    );
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('production host executes shared then legacy then modern server entries', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-manifest-host-')),
    output = [];
  const folder = path.join(root, 'different-folder-name');
  fs.mkdirSync(folder);
  fs.writeFileSync(
    path.join(folder, 'package.json'),
    JSON.stringify({
      name: 'mode',
      kcds: { server: 'old.js', serverScripts: ['new.js'], sharedScripts: ['common.js'] },
    }),
  );
  for (const [name, text] of [
    ['common.js', 'shared'],
    ['old.js', 'legacy'],
    ['new.js', 'modern'],
  ])
    fs.writeFileSync(path.join(folder, name), `console.log('${text}');`);
  const host = new ResourceHost(root, {
    send() {},
    log: (name, text) => output.push([name, text]),
  });
  try {
    await host.startAll();
    assert.deepEqual(output.slice(0, 3), [
      ['mode', 'shared'],
      ['mode', 'legacy'],
      ['mode', 'modern'],
    ]);
  } finally {
    await host.stop('mode');
    fs.rmSync(root, { recursive: true, force: true });
  }
});

// macOS's /tmp is a symlink to /private/tmp: a resources folder given through a link in its path
// still runs its own files.
test('a resources folder reached through a symlink runs its scripts', async (t) => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-manifest-linked-')),
    output = [];
  const real = path.join(root, 'real');
  const folder = path.join(real, 'shop');
  fs.mkdirSync(folder, { recursive: true });
  fs.writeFileSync(
    path.join(folder, 'package.json'),
    JSON.stringify({ name: 'shop', kcds: { serverScripts: ['main.js', 'lib'] } }),
  );
  fs.writeFileSync(path.join(folder, 'main.js'), "console.log('main');");
  fs.mkdirSync(path.join(folder, 'lib'));
  fs.writeFileSync(path.join(folder, 'lib', 'index.js'), "console.log('lib');");
  const linked = path.join(root, 'linked');
  try {
    // A junction needs no privilege on Windows; elsewhere it is a plain directory symlink.
    fs.symlinkSync(real, linked, 'junction');
  } catch (error) {
    fs.rmSync(root, { recursive: true, force: true });
    if (error.code === 'EPERM') {
      t.skip('no symlink privilege here');
      return;
    }
    throw error;
  }
  const host = new ResourceHost(linked, {
    send() {},
    log: (name, text) => output.push([name, text]),
  });
  try {
    await host.startAll();
    assert.deepEqual(output.slice(0, 2), [
      ['shop', 'main'],
      ['shop', 'lib'],
    ]);
  } finally {
    await host.stop('shop');
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('file selection rejects an explicit client entry pointing outside its resource', (t) => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-manifest-link-'));
  const folder = path.join(root, 'resource');
  fs.mkdirSync(folder);
  fs.writeFileSync(path.join(root, 'private.js'), 'secret');
  try {
    try {
      fs.symlinkSync(path.join(root, 'private.js'), path.join(folder, 'entry.js'));
    } catch (error) {
      if (error.code === 'EPERM') {
        t.skip('Windows symlink privilege unavailable; runs in Linux CI');
        return;
      }
      throw error;
    }
    assert.throws(
      () =>
        clientFiles(
          folder,
          normalizeManifest({ name: 'mode', kcds: { clientScripts: ['entry.js'] } }),
        ),
      /escapes resource/,
    );
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('invalid discovery is atomic: no scripts run and previous manifests stay intact', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-manifest-atomic-')),
    output = [];
  for (const name of ['a', 'z']) fs.mkdirSync(path.join(root, name));
  fs.writeFileSync(
    path.join(root, 'a/package.json'),
    JSON.stringify({ name: 'a', kcds: { serverScripts: ['a.js'] } }),
  );
  fs.writeFileSync(path.join(root, 'a/a.js'), "console.log('must not run')");
  fs.writeFileSync(
    path.join(root, 'z/package.json'),
    JSON.stringify({ name: 'z', kcds: { clientScripts: ['../secret.js'] } }),
  );
  const host = new ResourceHost(root, { send() {}, log: (...args) => output.push(args) });
  try {
    await assert.rejects(host.startAll(), /Invalid resource path/);
    assert.equal(host.resources.size, 0);
    assert.deepEqual(output, []);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('asset globs cover braces, character classes, globstar and canonical Windows separators', () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-globs-'));
  try {
    for (const name of [
      'ui/base.css',
      'ui/deep/font.woff',
      'ui/locale1.json',
      'ui/locale2.json',
      'ui/locale3.json',
      'ui/image.png',
      'ui/.secret',
      'server.js',
    ]) {
      fs.mkdirSync(path.dirname(path.join(root, name)), { recursive: true });
      fs.writeFileSync(path.join(root, name), '');
    }
    const manifest = normalizeManifest({
      name: 'assets',
      kcds: {
        files: ['ui\\**\\*.{css,woff}', 'ui/locale[1-2].json'],
        clientFiles: ['ui/image.?ng', 'server.*'],
        serverScripts: ['server.js'],
      },
    });
    assert.deepEqual(clientFiles(root, manifest), [
      'ui/base.css',
      'ui/deep/font.woff',
      'ui/image.png',
      'ui/locale1.json',
      'ui/locale2.json',
    ]);
    assert.deepEqual(
      clientFiles(
        root,
        normalizeManifest({
          name: 'assets',
          kcds: {
            files: ['ui/**'],
            serverScripts: ['server.js'],
          },
        }),
      ),
      [
        'ui/base.css',
        'ui/deep/font.woff',
        'ui/image.png',
        'ui/locale1.json',
        'ui/locale2.json',
        'ui/locale3.json',
      ],
    );
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("a resource's stream/ files always go to clients", () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-stream-'));
  for (const file of ['client/main.js', 'stream/objects/kcds/mode/chair.cgf', 'other/x.txt']) {
    fs.mkdirSync(path.dirname(path.join(root, file)), { recursive: true });
    fs.writeFileSync(path.join(root, file), 'x');
  }
  const m = normalizeManifest({ name: 'mode', kcds: { clientScripts: ['client/main.js'] } });
  assert.deepEqual(clientFiles(root, m), ['client/main.js', 'stream/objects/kcds/mode/chair.cgf']);
});

test('a default client folder never sends hidden files or node_modules', () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-private-'));
  const files = [
    'client.js',
    'server.js',
    'ui/menu.html',
    '.env',
    '.git/config',
    'ui/.secret.json',
    'node_modules/pg/index.js',
  ];
  for (const file of files) {
    fs.mkdirSync(path.dirname(path.join(root, file)), { recursive: true });
    fs.writeFileSync(path.join(root, file), 'x');
  }
  try {
    const flat = normalizeManifest({
      name: 'mode',
      kcds: { serverScripts: ['server.js'], clientScripts: ['client.js'] },
    });
    assert.deepEqual(clientFiles(root, flat), ['client.js', 'ui/menu.html']);
    // A pattern may still name them on purpose.
    const named = normalizeManifest({
      name: 'mode',
      kcds: { clientScripts: ['client.js'], files: ['node_modules/pg/*.js'] },
    });
    assert.deepEqual(clientFiles(root, named), ['client.js', 'node_modules/pg/index.js']);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('no server code reaches players: only what client scripts load, in every language', () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-server-code-'));
  const write = (name, text = 'x') => {
    fs.mkdirSync(path.dirname(path.join(root, name)), { recursive: true });
    fs.writeFileSync(path.join(root, name), text);
  };
  try {
    // A flat resource: the server's helper and config beside the client script.
    write('server.js', "const lib = require('./lib');\nconst config = require('./config.json');");
    write('lib.js', "module.exports = require('./db/pool.cjs');");
    write('db/pool.cjs', 'SECRET');
    write('config.json', '{"password":"x"}');
    write('client.js', "const util = require('./util.js');\nimport('./later.js');");
    write('util.js', "exports.both = require('./both.js');");
    write('both.js', 'shared by both sides');
    write('later.js', 'loaded later');
    write('admin_tools.js', 'loaded by nobody (a path built at run time, say)');
    write('ui/page.html', '<p>');
    write('ui/look.css', 'p {}');
    write('notes.txt', 'hi');
    const flat = normalizeManifest({
      name: 'mode',
      kcds: { serverScripts: ['server.js'], clientScripts: ['client.js'] },
    });
    assert.deepEqual(clientFiles(root, flat), [
      'both.js',
      'client.js',
      'later.js',
      'notes.txt',
      'ui/look.css',
      'ui/page.html',
      'util.js',
    ]);
    // The server also loading both.js does not keep it from the client that needs it.
    write('server.js', "require('./lib'); require('./both.js'); require('./config.json');");
    assert.ok(clientFiles(root, flat).includes('both.js'));
    // A pattern naming server code is refused, a wide one leaves it out.
    const named = normalizeManifest({
      name: 'mode',
      kcds: {
        serverScripts: ['server.js'],
        clientScripts: ['client.js'],
        files: ['lib.js', 'ui/*'],
      },
    });
    assert.throws(
      () => clientFiles(root, named),
      /files names server code, which is never sent to players: lib\.js/,
    );
    const wide = normalizeManifest({
      name: 'mode',
      kcds: { serverScripts: ['server.js'], clientScripts: ['client.js'], files: ['**'] },
    });
    const sent = clientFiles(root, wide);
    for (const secret of ['server.js', 'lib.js', 'db/pool.cjs', 'config.json']) {
      assert.ok(!sent.includes(secret), secret);
    }
    // Lua: require("folder.name") from the resource's root.
    write('server.lua', 'local db = require("srv.db")');
    write('srv/db.lua', 'SECRET');
    write('client.lua', 'local menu = require("ui.menu")');
    write('ui/menu.lua', 'return {}');
    const lua = normalizeManifest({
      name: 'mode',
      kcds: { serverScripts: ['server.lua'], clientScripts: ['client.lua'] },
    });
    const luaSent = clientFiles(root, lua);
    assert.ok(luaSent.includes('ui/menu.lua'));
    assert.ok(!luaSent.includes('srv/db.lua'));
    assert.ok(!luaSent.includes('server.lua'));
    // C#: the server's assembly and every source stay home; the client's assembly goes.
    write('cs/bin/server/Server.dll', 'MZ');
    write('cs/bin/client/Client.dll', 'MZ');
    write('cs/bin/client/Client.pdb', 'PDB');
    write('cs/bin/client/Main.cs', 'source');
    write('cs/bin/client/Newtonsoft.Json.dll', 'MZ');
    const csharp = normalizeManifest({
      name: 'mode',
      kcds: {
        serverScripts: ['cs/bin/server/Server.dll'],
        clientScripts: ['cs/bin/client/Client.dll'],
      },
    });
    // Its package's assembly beside it goes too; the debug symbols do not.
    assert.deepEqual(clientFiles(root, csharp), [
      'cs/bin/client/Client.dll',
      'cs/bin/client/Newtonsoft.Json.dll',
    ]);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('kcds.loadingScreen is a page, or a page with its files, and a bad one is refused', () => {
  const short = normalizeManifest({ name: 'a', kcds: { loadingScreen: 'ui/l.html' } });
  assert.deepEqual(short.kcds.loadingScreen, { page: 'ui/l.html', files: [] });
  const full = normalizeManifest({
    name: 'a',
    kcds: { loadingScreen: { page: 'ui/l.html', files: ['ui/art/**'] } },
  });
  assert.deepEqual(full.kcds.loadingScreen.files, ['ui/art/**']);
  assert.equal(normalizeManifest({ name: 'a' }).kcds.loadingScreen, undefined);
  for (const bad of [5, {}, [], { page: '../x.html' }, { page: 'x.html', files: 'a' }]) {
    assert.throws(() => normalizeManifest({ name: 'a', kcds: { loadingScreen: bad } }), TypeError);
  }
});

test('a default client folder never sends the server folders, data or keys', () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-private-'));
  const files = [
    'client.js',
    'server/main.js',
    'server/db.js',
    'shared/util.js',
    'package.json',
    'data/players.sqlite',
    'certs/server.key',
    'ui/menu.html',
    'ui/items.json',
  ];
  for (const file of files) {
    fs.mkdirSync(path.dirname(path.join(root, file)), { recursive: true });
    fs.writeFileSync(path.join(root, file), 'x');
  }
  // The client script loads its helper and its data: code and data go only so.
  fs.writeFileSync(
    path.join(root, 'client.js'),
    "require('./shared/util.js');\nrequire('./ui/items.json');\n",
  );
  try {
    const m = normalizeManifest({
      name: 'mode',
      kcds: { serverScripts: ['server/main.js'], clientScripts: ['client.js'] },
    });
    assert.deepEqual(clientFiles(root, m), [
      'client.js',
      'shared/util.js',
      'ui/items.json',
      'ui/menu.html',
    ]);
    // A pattern may still name them on purpose.
    const named = normalizeManifest({
      name: 'mode',
      kcds: {
        serverScripts: ['server/main.js'],
        clientScripts: ['client.js'],
        files: ['*.js', 'server/db.js'],
      },
    });
    // What the client script loads goes too.
    assert.deepEqual(clientFiles(root, named), [
      'client.js',
      'server/db.js',
      'shared/util.js',
      'ui/items.json',
    ]);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('client files are worked out once per loaded manifest, again after a new load', (t) => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-client-set-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  fs.mkdirSync(path.join(root, 'client'));
  fs.mkdirSync(path.join(root, 'icons'));
  fs.writeFileSync(path.join(root, 'client', 'main.js'), '');
  fs.writeFileSync(path.join(root, 'icons', 'a.dds'), '');
  const manifest = normalizeManifest({
    name: 'icons',
    kcds: { clientScripts: ['client/main.js'], files: ['icons/*.dds'] },
  });
  const walks = t.mock.method(fs, 'readdirSync');
  const first = clientFileSet(root, manifest);
  const walked = walks.mock.callCount();
  assert.ok(walked > 0);
  assert.ok(first.has('icons/a.dds'));
  // Hundreds of icon checks walk the folder once.
  for (let i = 0; i < 500; i++) {
    assert.ok(clientFileSet(root, manifest).has('icons/a.dds'));
  }
  assert.equal(walks.mock.callCount(), walked);
  // A restart reads the manifest again: a file added since is seen.
  fs.writeFileSync(path.join(root, 'icons', 'b.dds'), '');
  assert.ok(!clientFileSet(root, manifest).has('icons/b.dds'));
  const reloaded = normalizeManifest({
    name: 'icons',
    kcds: { clientScripts: ['client/main.js'], files: ['icons/*.dds'] },
  });
  assert.ok(clientFileSet(root, reloaded).has('icons/b.dds'));
  assert.deepEqual([...clientFileSet(root, reloaded)], clientFiles(root, reloaded));
});
