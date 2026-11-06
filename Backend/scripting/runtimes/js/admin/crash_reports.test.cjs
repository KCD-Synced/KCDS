'use strict';

// crash reports of a server that ended without stopping in order.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { CrashReports } = require('./crash_reports.cjs');

function setup() {
  const base = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-crashes-'));
  const root = path.join(base, 'resources');
  fs.mkdirSync(root);
  let time = Date.UTC(2026, 9, 26, 8, 0, 0);
  const logged = [];
  const make = (keep) =>
    new CrashReports({ root, log: (text) => logged.push(text), now: () => time, keep });
  const advance = (seconds) => {
    time += seconds * 1000;
  };
  return { base, make, advance, logged };
}

test('an orderly stop leaves no report and no marker', () => {
  const { base, make } = setup();
  const reports = make();
  reports.start();
  assert.ok(fs.existsSync(path.join(base, 'crashes', 'running.json')));
  reports.stopping();
  reports.closed([{ time: 0, source: 'host', text: 'bye' }]);
  assert.deepEqual(reports.list(), []);
  assert.ok(!fs.existsSync(path.join(base, 'crashes', 'running.json')));
  make().start();
  assert.deepEqual(make().list(), [], 'the next start finds nothing to report');
});

test('the native server gone: a report with the last log lines, once', () => {
  const { make, advance, logged } = setup();
  const reports = make();
  reports.start();
  advance(90);
  const lines = Array.from({ length: 250 }, (_, index) => ({
    time: Date.UTC(2026, 9, 26, 8, 1, 0),
    source: 'host',
    text: `line ${index}`,
  }));
  reports.closed(lines);
  const [listed] = reports.list();
  assert.equal(listed.name, '20261026-080130.json');
  assert.equal(listed.kind, 'native server gone');
  assert.equal(listed.startedAt, '2026-10-26T08:00:00.000Z');
  assert.equal(listed.endedAt, '2026-10-26T08:01:30.000Z');
  const report = reports.read(listed.name);
  assert.equal(report.log.length, 200);
  assert.equal(report.log.at(-1).text, 'line 249');
  assert.match(logged.at(-1), /crash report 20261026-080130\.json/);
  // The next start does not report the same run again.
  advance(30);
  const next = make();
  next.start();
  assert.equal(next.list().length, 1);
});

test('a run that ended with no word is reported at the next start, with the log it kept', () => {
  const { make, advance } = setup();
  const first = make();
  first.start();
  const line = (seq, text) => ({
    seq,
    time: Date.UTC(2026, 9, 26, 8, 0, 30),
    source: 'host',
    text,
  });
  first.keepLog([line(1, 'one'), line(2, 'two')]);
  advance(60);
  const next = make();
  next.start();
  const [listed] = next.list();
  assert.equal(listed.kind, 'ended unexpectedly');
  assert.equal(listed.startedAt, '2026-10-26T08:00:00.000Z');
  assert.equal(listed.endedAt, '2026-10-26T08:00:30.000Z', 'the last line kept');
  assert.deepEqual(
    next.read(listed.name).log.map((entry) => entry.text),
    ['one', 'two'],
  );
  // A run with no log kept is still reported, without lines.
  advance(60);
  const third = make();
  third.start();
  assert.equal(third.list().length, 2);
  assert.deepEqual(third.read(third.list()[0].name).log, []);
});

test('the newest reports are kept, and only report names are read', () => {
  const { make, advance } = setup();
  for (let index = 0; index < 4; index += 1) {
    const reports = make(3);
    reports.start();
    advance(10);
    reports.closed([]);
    advance(10);
  }
  const reports = make(3);
  assert.equal(reports.list().length, 3);
  assert.equal(reports.list()[0].name, '20261026-080110.json', 'newest first');
  assert.equal(reports.read('../server.json'), null);
  assert.equal(reports.read('running.json'), null);
  assert.equal(reports.read('20991231-000000.json'), null);
});
