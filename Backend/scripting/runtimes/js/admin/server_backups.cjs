'use strict';

// Backups of the server's data: the resources folder (the resources' own data files
// and databases with them, without node_modules), server.json, bans.json and admin-accounts.json,
// copied to backups/<date-time>/ next to the resources folder. Made from KCDS-Admin and before
// every planned restart; the newest kKeep are kept. A file over kMaxFileBytes (a big asset) is left
// out and logged. A database a resource holds open is copied as it is on disk at that moment.

const fs = require('node:fs');
const path = require('node:path');

const kKeep = 10;
const kMaxFileBytes = 64 * 1024 * 1024;
const NAME = /^\d{8}-\d{6}(-\d+)?$/;

function stamp(time) {
  const iso = new Date(time).toISOString();
  return iso.slice(0, 10).replaceAll('-', '') + '-' + iso.slice(11, 19).replaceAll(':', '');
}

function folderSize(folder) {
  let total = 0;
  for (const entry of fs.readdirSync(folder, { withFileTypes: true })) {
    const full = path.join(folder, entry.name);
    total += entry.isDirectory() ? folderSize(full) : fs.statSync(full).size;
  }
  return total;
}

class ServerBackups {
  // root: the resources folder; configFile: server.json (may be ''); log(text).
  constructor({ root, configFile = '', log = () => {}, now = () => Date.now(), keep = kKeep }) {
    this.root = root;
    this.folder = path.join(path.dirname(root), 'backups');
    this.configFile = configFile;
    this.log = log;
    this.now = now;
    this.keep = keep;
  }

  // Makes a backup now: { name, bytes, skipped } (skipped: files left out for their size).
  create(by) {
    const base = stamp(this.now());
    let name = base;
    // Two in the same second: base-2, base-3, ...
    for (let count = 2; fs.existsSync(path.join(this.folder, name)); count += 1) {
      name = `${base}-${count}`;
    }
    const target = path.join(this.folder, name);
    const skipped = [];
    fs.mkdirSync(target, { recursive: true });
    fs.cpSync(this.root, path.join(target, 'resources'), {
      recursive: true,
      filter: (source) => {
        if (path.basename(source) === 'node_modules') {
          return false;
        }
        const stats = fs.statSync(source);
        if (stats.isFile() && stats.size > kMaxFileBytes) {
          skipped.push(path.relative(this.root, source));
          return false;
        }
        return true;
      },
    });
    if (this.configFile && fs.existsSync(this.configFile)) {
      fs.copyFileSync(this.configFile, path.join(target, 'server.json'));
    }
    const bytes = folderSize(target);
    this.log(`backup ${name} made (${by}): ${Math.round(bytes / 1024)} KiB`);
    for (const file of skipped) {
      this.log(`backup ${name}: ${file} left out (over ${kMaxFileBytes / 1048576} MiB)`);
    }
    this.prune();
    return { name, bytes, skipped };
  }

  // The backups, newest first.
  list() {
    if (!fs.existsSync(this.folder)) {
      return [];
    }
    return fs
      .readdirSync(this.folder, { withFileTypes: true })
      .filter((entry) => entry.isDirectory() && NAME.test(entry.name))
      .map((entry) => ({
        name: entry.name,
        bytes: folderSize(path.join(this.folder, entry.name)),
      }))
      .sort((a, b) => (a.name < b.name ? 1 : -1));
  }

  // Only the newest `keep` stay.
  prune() {
    for (const old of this.list().slice(this.keep)) {
      fs.rmSync(path.join(this.folder, old.name), { recursive: true, force: true });
      this.log(`backup ${old.name} removed (the newest ${this.keep} are kept)`);
    }
  }
}

module.exports = { ServerBackups };
