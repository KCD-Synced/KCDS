'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { resourceDatabases, kMostOpen } = require('./database/database.cjs');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

// Database: a resource's SQLite files in its own folder, the same from every language.

function folder(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-database-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  return root;
}

test('rows go in and come out, with ? and :name parameters', (t) => {
  const root = folder(t);
  const { api: Database, stop } = resourceDatabases(root, 'shop');
  const db = Database.open('data/shop.db');
  assert.ok(fs.existsSync(path.join(root, 'data', 'shop.db')));
  assert.equal(db.file, 'data/shop.db');
  db.exec('CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, price INTEGER, sold INTEGER)');
  assert.deepEqual(
    db.run('INSERT INTO items (name, price, sold) VALUES (?, ?, ?)', ['sword', 30, false]),
    {
      changes: 1,
      lastInsertId: 1,
    },
  );
  db.run('INSERT INTO items (name, price, sold) VALUES (:name, :price, :sold)', {
    name: 'shield',
    price: 20,
    sold: true,
  });
  assert.deepEqual(db.query('SELECT name, price, sold FROM items ORDER BY id'), [
    { name: 'sword', price: 30, sold: 0 },
    { name: 'shield', price: 20, sold: 1 },
  ]);
  assert.deepEqual(db.get('SELECT price FROM items WHERE name = ?', ['shield']), { price: 20 });
  assert.equal(db.get('SELECT price FROM items WHERE name = ?', ['axe']), null);
  assert.equal(Object.getPrototypeOf(db.get('SELECT 1 AS one')), Object.prototype);
  // A transaction that throws leaves nothing behind.
  assert.throws(
    () =>
      db.transaction(() => {
        db.run('DELETE FROM items');
        throw new Error('changed my mind');
      }),
    /changed my mind/,
  );
  assert.equal(db.query('SELECT * FROM items').length, 2);
  assert.equal(
    db.transaction(() => db.run('UPDATE items SET price = price + 1').changes),
    2,
  );
  // The file keeps it.
  db.close();
  assert.equal(db.isOpen, false);
  assert.throws(() => db.query('SELECT 1'), /closed/);
  const again = Database.open('data/shop.db');
  assert.deepEqual(again.get('SELECT price FROM items WHERE name = ?', ['sword']), { price: 31 });
  stop();
  assert.equal(again.isOpen, false);
});

test('only database files inside the resource folder, and a few at once', (t) => {
  const root = folder(t);
  const { api: Database, stop } = resourceDatabases(root, 'shop');
  assert.throws(() => Database.open('../other.db'), RangeError);
  assert.throws(() => Database.open(path.join(os.tmpdir(), 'x.db')), RangeError);
  assert.throws(() => Database.open('notes.txt'), RangeError);
  assert.throws(() => Database.open('main.js'), RangeError);
  assert.throws(() => Database.open(5), TypeError);
  const memory = Database.open();
  assert.equal(memory.file, ':memory:');
  assert.throws(() => memory.query(''), TypeError);
  assert.throws(() => memory.query('SELECT ?', [{}]), TypeError);
  assert.throws(() => memory.query('SELECT 1', 'x'), TypeError);
  for (let index = 1; index < kMostOpen; index += 1) {
    Database.open(`more${index}.sqlite`);
  }
  assert.throws(() => Database.open('one-too-many.db'), RangeError);
  memory.close();
  Database.open('one-more.sqlite3').close();
  stop();
});

test('a Lua resource runs SQL and gets its rows back at once', async (t) => {
  const f = serverFixture(t);
  const directory = path.join(f.root, 'ledger');
  fs.mkdirSync(directory, { recursive: true });
  fs.writeFileSync(
    path.join(directory, 'package.json'),
    JSON.stringify({ name: 'ledger', kcds: { server: 'main.lua' } }),
  );
  fs.writeFileSync(
    path.join(directory, 'main.lua'),
    `
      local db = kcds.core.Database.open("ledger.db")
      db:exec("CREATE TABLE IF NOT EXISTS coins (player TEXT PRIMARY KEY, amount INTEGER)")
      db:run("INSERT OR REPLACE INTO coins VALUES (?, ?)", { "Hans", 12 })
      db:run("INSERT OR REPLACE INTO coins VALUES (:player, :amount)", { player = "Theresa", amount = 7 })
      local rows = db:query("SELECT player, amount FROM coins ORDER BY player")
      print("rows", #rows, rows[1].player, rows[1].amount, rows[2].player, rows[2].amount)
      local hans = db:get("SELECT amount FROM coins WHERE player = ?", { "Hans" })
      print("hans", hans.amount, db:get("SELECT amount FROM coins WHERE player = ?", { "Nobody" }) == nil)
    `,
  );
  await f.host.startAll();
  assert.ok(
    f.linesOf('ledger').includes('rows\t2\tHans\t12\tTheresa\t7'),
    f.linesOf('ledger').join('\n'),
  );
  assert.ok(f.linesOf('ledger').includes('hans\t12\ttrue'), f.linesOf('ledger').join('\n'));
  assert.ok(fs.existsSync(path.join(directory, 'ledger.db')));
});
