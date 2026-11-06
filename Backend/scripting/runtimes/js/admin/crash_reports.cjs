'use strict';

// Crash reports for KCDS-Admin: a server that ended without stopping in order.
//
// The resource host keeps a marker, crashes/running.json next to the resources folder, while the
// server runs. The native server tells the host `serverStopping` on every orderly stop (the stop
// command, Ctrl+C, KCDS-Admin, a planned restart); without it, the host's input closing means
// the native server is gone, and the host writes a report at once with the last lines of its log.
// A marker found at the next start means the host ended with it (on Windows the native server's
// job ends the host too) or by itself: the report is written then, with the log lines the host
// last kept (crashes/last-log.json, written every few seconds while new lines come). Reports are
// crashes/<date-time>.json; the newest kKeep are kept.

const fs = require('node:fs');
const path = require('node:path');

const kKeep = 20;
const kLogLines = 200;
const MARKER = 'running.json';
const LAST_LOG = 'last-log.json';
const NAME = /^\d{8}-\d{6}(-\d+)?\.json$/;

function stamp(time) {
  const iso = new Date(time).toISOString();
  return iso.slice(0, 10).replaceAll('-', '') + '-' + iso.slice(11, 19).replaceAll(':', '');
}

// The last kLogLines admin log lines as a report keeps them.
function entries(lines) {
  return lines.slice(-kLogLines).map((line) => ({
    time: new Date(line.time).toISOString(),
    source: line.source,
    text: line.text,
  }));
}

class CrashReports {
  // root: the resources folder; log(text).
  constructor({ root, log = () => {}, now = () => Date.now(), keep = kKeep }) {
    this.folder = path.join(path.dirname(root), 'crashes');
    this.log = log;
    this.now = now;
    this.keep = keep;
    this.startedAt = null;
    this.orderly = false;
  }

  // The server starts: a marker left by the last run is reported, and a new one is written.
  start() {
    fs.mkdirSync(this.folder, { recursive: true });
    const marker = path.join(this.folder, MARKER);
    if (fs.existsSync(marker)) {
      let previous = {};
      try {
        previous = JSON.parse(fs.readFileSync(marker, 'utf8'));
      } catch {
        previous = {};
      }
      if (!previous.reported) {
        const log = this.lastLog();
        this.write({
          kind: 'ended unexpectedly',
          startedAt: previous.startedAt ?? null,
          endedAt: log.length > 0 ? log.at(-1).time : null,
          detail:
            'The last run ended without stopping in order (a crash or a killed process); the log ' +
            'is its last lines the server kept.',
          log,
        });
      }
    }
    fs.rmSync(path.join(this.folder, LAST_LOG), { force: true });
    this.startedAt = this.now();
    this.orderly = false;
    this.keptSeq = 0;
    this.mark({ startedAt: new Date(this.startedAt).toISOString(), pid: process.pid });
  }

  // Keeps the last log lines on disk while the server runs (every few seconds): what a report of a
  // run that ended with no word shows. Written only when new lines came.
  keepLog(lines) {
    if (this.startedAt === null || lines.length === 0 || lines.at(-1).seq === this.keptSeq) {
      return;
    }
    this.keptSeq = lines.at(-1).seq;
    fs.writeFileSync(path.join(this.folder, LAST_LOG), JSON.stringify(entries(lines)));
  }

  lastLog() {
    try {
      const kept = JSON.parse(fs.readFileSync(path.join(this.folder, LAST_LOG), 'utf8'));
      return Array.isArray(kept) ? kept : [];
    } catch {
      return [];
    }
  }

  // The native server stops in order.
  stopping() {
    this.orderly = true;
  }

  // The host's input closed: in order, the marker goes; else the native server is gone, and it is
  // reported with the last log lines (adminLog lines: {time, source, text}).
  closed(lines = []) {
    if (this.startedAt === null) {
      return;
    }
    const marker = path.join(this.folder, MARKER);
    if (this.orderly) {
      fs.rmSync(marker, { force: true });
      fs.rmSync(path.join(this.folder, LAST_LOG), { force: true });
      return;
    }
    this.write({
      kind: 'native server gone',
      startedAt: new Date(this.startedAt).toISOString(),
      endedAt: new Date(this.now()).toISOString(),
      detail: 'The native server ended without stopping in order (a crash or a killed process).',
      log: entries(lines),
    });
    this.mark({ startedAt: new Date(this.startedAt).toISOString(), reported: true });
  }

  // The reports, newest first: [{ name, kind, startedAt, endedAt }].
  list() {
    if (!fs.existsSync(this.folder)) {
      return [];
    }
    const reports = [];
    for (const name of this.names().reverse()) {
      const report = this.read(name);
      if (report) {
        reports.push({
          name,
          kind: report.kind,
          startedAt: report.startedAt,
          endedAt: report.endedAt,
        });
      }
    }
    return reports;
  }

  // One report by its file name, or null.
  read(name) {
    if (typeof name !== 'string' || !NAME.test(name)) {
      return null;
    }
    try {
      return JSON.parse(fs.readFileSync(path.join(this.folder, name), 'utf8'));
    } catch {
      return null;
    }
  }

  names() {
    return fs
      .readdirSync(this.folder)
      .filter((name) => NAME.test(name))
      .sort();
  }

  write(report) {
    fs.mkdirSync(this.folder, { recursive: true });
    const base = stamp(this.now());
    let name = `${base}.json`;
    for (let count = 2; fs.existsSync(path.join(this.folder, name)); count += 1) {
      name = `${base}-${count}.json`;
    }
    fs.writeFileSync(path.join(this.folder, name), JSON.stringify(report, null, 2));
    this.log(`crash report ${name}: ${report.kind}`);
    const names = this.names();
    for (const old of names.slice(0, Math.max(0, names.length - this.keep))) {
      fs.rmSync(path.join(this.folder, old), { force: true });
    }
    return name;
  }

  mark(content) {
    fs.writeFileSync(path.join(this.folder, MARKER), JSON.stringify(content));
  }
}

module.exports = { CrashReports };
