'use strict';

// Database: SQLite files a server resource keeps in its own folder, through Node's own node:sqlite,
// so JavaScript, TypeScript, Lua and C# resources run SQL the same way and get their rows back at
// once (no promise to wait for).
//
//   Database.open(file)          opens or creates a file in the resource's folder ('.db',
//                                '.sqlite' or '.sqlite3'), or ':memory:' (the default)
//   db.query(sql, params)        every row, as objects
//   db.get(sql, params)          the first row, or null
//   db.run(sql, params)          { changes, lastInsertId }
//   db.exec(sql)                 runs statements without parameters (a schema)
//   db.transaction(fn)           runs fn in BEGIN ... COMMIT, ROLLBACK when it throws
//   db.close()                   closes it; a resource's databases close when it stops
//
// Parameters are an array for `?` placeholders or an object for `:name` ones; values are numbers,
// strings, booleans (stored as 1 and 0) or null. The file stays in the resource's folder: a path
// that leaves it, or another extension, is refused. Those extensions are never sent to players.
// At most kMostOpen databases are open per resource at once. The calls run on the resource
// runtime's thread, not the server's simulation; a long query holds that resource's scripts (and a
// Lua call's time limit counts it).

const fs = require('node:fs');
const path = require('node:path');
const { DatabaseSync } = require('node:sqlite');

const kExtensions = new Set(['.db', '.sqlite', '.sqlite3']);
const kMostOpen = 8;
const kMemory = ':memory:';

// A value SQLite takes: booleans become 1 and 0, undefined becomes null.
function bound(value, where) {
  if (value === undefined || value === null) {
    return null;
  }
  if (typeof value === 'boolean') {
    return value ? 1 : 0;
  }
  if (typeof value === 'number' || typeof value === 'string' || typeof value === 'bigint') {
    return value;
  }
  if (value instanceof Uint8Array) {
    return value;
  }
  throw new TypeError(`Database ${where}: a parameter must be a number, string, boolean or null`);
}

// The statement's arguments for node:sqlite: none, each `?` value in turn, or one object of the
// `:name` values.
function parametersOf(params, where) {
  if (params === undefined || params === null) {
    return [];
  }
  if (Array.isArray(params)) {
    return params.map((value) => bound(value, where));
  }
  if (typeof params === 'object') {
    const named = {};
    for (const [key, value] of Object.entries(params)) {
      named[key] = bound(value, where);
    }
    return [named];
  }
  throw new TypeError(`Database ${where}: params must be an array or an object`);
}

function requireSql(sql, where) {
  if (typeof sql !== 'string' || sql.trim() === '') {
    throw new TypeError(`Database ${where} takes the SQL as a string`);
  }
}

// A row as a plain object (node:sqlite gives objects without a prototype).
function plainRow(row) {
  return row ? { ...row } : null;
}

// The Database type of one resource: `folder` is its folder, `name` its name.
function resourceDatabases(folder, name) {
  const open = new Set();
  const connections = new WeakMap();

  const connectionOf = (db, where) => {
    const connection = connections.get(db);
    if (!connection) {
      throw new Error(`Database ${where}: the database is closed`);
    }
    return connection;
  };

  // The file a resource may open: inside its folder, with a database extension.
  const fileOf = (file) => {
    if (file === kMemory) {
      return kMemory;
    }
    if (typeof file !== 'string' || file === '') {
      throw new TypeError("Database.open takes a file name such as 'shop.db', or ':memory:'");
    }
    const full = path.resolve(folder, file);
    const inside = path.relative(folder, full);
    if (inside === '' || inside.startsWith('..') || path.isAbsolute(inside)) {
      throw new RangeError(`Database.open: ${file} is outside the resource's folder`);
    }
    if (!kExtensions.has(path.extname(full).toLowerCase())) {
      throw new RangeError(`Database.open: ${file} must end in .db, .sqlite or .sqlite3`);
    }
    return full;
  };

  class Database {
    // Opens or creates a database file in the resource's folder, or one in memory.
    static open(file = kMemory) {
      const where = fileOf(file);
      if (open.size >= kMostOpen) {
        throw new RangeError(`Database.open: ${name} has ${kMostOpen} databases open already`);
      }
      if (where !== kMemory) {
        fs.mkdirSync(path.dirname(where), { recursive: true });
      }
      const db = new Database();
      connections.set(db, { sqlite: new DatabaseSync(where), file: file });
      open.add(db);
      return db;
    }

    // The file it was opened with, as given ('shop.db', ':memory:').
    get file() {
      return connections.get(this)?.file ?? '';
    }

    // Whether it is still open.
    get isOpen() {
      return connections.has(this);
    }

    // Every row the statement answers, as objects keyed by column.
    query(sql, params) {
      requireSql(sql, 'query');
      const { sqlite } = connectionOf(this, 'query');
      return sqlite
        .prepare(sql)
        .all(...parametersOf(params, 'query'))
        .map(plainRow);
    }

    // The first row the statement answers, or null.
    get(sql, params) {
      requireSql(sql, 'get');
      const { sqlite } = connectionOf(this, 'get');
      return plainRow(sqlite.prepare(sql).get(...parametersOf(params, 'get')) ?? null);
    }

    // Runs one statement that changes rows: { changes, lastInsertId }.
    run(sql, params) {
      requireSql(sql, 'run');
      const { sqlite } = connectionOf(this, 'run');
      const result = sqlite.prepare(sql).run(...parametersOf(params, 'run'));
      return { changes: Number(result.changes), lastInsertId: Number(result.lastInsertRowid) };
    }

    // Runs statements without parameters, such as a schema.
    exec(sql) {
      requireSql(sql, 'exec');
      connectionOf(this, 'exec').sqlite.exec(sql);
    }

    // Runs fn inside one transaction and returns what it returns; when fn throws, nothing it did
    // stays and the error goes on.
    transaction(fn) {
      if (typeof fn !== 'function') {
        throw new TypeError('Database transaction takes a function');
      }
      const { sqlite } = connectionOf(this, 'transaction');
      sqlite.exec('BEGIN');
      try {
        const result = fn();
        sqlite.exec('COMMIT');
        return result;
      } catch (error) {
        sqlite.exec('ROLLBACK');
        throw error;
      }
    }

    // Closes it; later calls throw. Closing twice does nothing.
    close() {
      const connection = connections.get(this);
      if (!connection) {
        return;
      }
      connections.delete(this);
      open.delete(this);
      connection.sqlite.close();
    }
  }

  return {
    api: Database,
    // The resource stopped: its databases close.
    stop() {
      for (const db of [...open]) {
        db.close();
      }
    },
  };
}

module.exports = { resourceDatabases, kMostOpen };
