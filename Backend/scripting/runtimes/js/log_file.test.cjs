'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { RotatingLogFile } = require('./logging/log_file.cjs');

function freshFolder() {
  return fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-log-file-'));
}

test('a log file starts over in a numbered file when full, keeping only the newest files', () => {
  const folder = freshFolder();
  const file = path.join(folder, 'logs', 'server.log');
  const log = new RotatingLogFile({ file, maxBytes: 10, files: 3 });
  for (const line of ['aaaaaaaa\n', 'bbbbbbbb\n', 'cccccccc\n', 'dddddddd\n']) {
    log.write(Buffer.from(line));
  }
  log.close();
  assert.equal(fs.readFileSync(file, 'utf8'), 'dddddddd\n');
  assert.equal(fs.readFileSync(`${file}.1`, 'utf8'), 'cccccccc\n');
  assert.equal(fs.readFileSync(`${file}.2`, 'utf8'), 'bbbbbbbb\n');
  // Three files in all: the oldest lines are gone.
  assert.equal(fs.existsSync(`${file}.3`), false);
  fs.rmSync(folder, { recursive: true, force: true });
});

test('a log file continues the file a server started before left', () => {
  const folder = freshFolder();
  const file = path.join(folder, 'server.log');
  fs.writeFileSync(file, 'before\n');
  const log = new RotatingLogFile({ file, maxBytes: 100, files: 2 });
  log.write(Buffer.from('after\n'));
  log.close();
  assert.equal(fs.readFileSync(file, 'utf8'), 'before\nafter\n');
  fs.rmSync(folder, { recursive: true, force: true });
});

test('a log file kept alone empties itself when full', () => {
  const folder = freshFolder();
  const file = path.join(folder, 'server.log');
  const log = new RotatingLogFile({ file, maxBytes: 8, files: 1 });
  log.write(Buffer.from('first\n'));
  log.write(Buffer.from('second\n'));
  log.close();
  assert.equal(fs.readFileSync(file, 'utf8'), 'second\n');
  assert.deepEqual(fs.readdirSync(folder), ['server.log']);
  fs.rmSync(folder, { recursive: true, force: true });
});

test('the log program passes the output on and writes it to the file', () => {
  const folder = freshFolder();
  const file = path.join(folder, 'server.log');
  const result = childProcess.spawnSync(
    process.execPath,
    [path.join(__dirname, 'logging', 'log_file.cjs'), file, '1', '2'],
    { input: 'RESOURCE HOST ready\nSTOP\n' },
  );
  assert.equal(result.status, 0);
  assert.equal(result.stdout.toString(), 'RESOURCE HOST ready\nSTOP\n');
  assert.equal(fs.readFileSync(file, 'utf8'), 'RESOURCE HOST ready\nSTOP\n');
  fs.rmSync(folder, { recursive: true, force: true });
});
