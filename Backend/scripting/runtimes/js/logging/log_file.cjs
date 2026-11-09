'use strict';
// The server's log file. start-server.sh sends everything the server prints (its resources'
// lines too) through this program: it goes on to the terminal, or to `docker logs`, unchanged, and
// into a file that never grows past its size. A full file becomes <file>.1, the one before
// <file>.2 and so on; the oldest beyond the number of files kept is removed.
//
//   node log_file.cjs <file> [megabytes per file, 20] [files kept, 5]
//
// It ends when the server's output ends (the start script and the server closed it), never on a
// stop signal, so the server's last lines are written too.
const fs = require('node:fs');
const path = require('node:path');

const kDefaultMegabytes = 20;
const kDefaultFiles = 5;

class RotatingLogFile {
  // file: the log's path. maxBytes: the size a file grows to. files: how many files are kept,
  // the one being written included.
  constructor({ file, maxBytes, files }) {
    this.file = file;
    this.maxBytes = maxBytes;
    this.files = Math.max(1, files);
    fs.mkdirSync(path.dirname(file), { recursive: true });
    this.size = 0;
    try {
      this.size = fs.statSync(file).size;
    } catch {
      // A new log.
    }
    this.fd = fs.openSync(file, 'a');
  }

  // Writes the bytes; the file rotates first when they would take it past its size.
  write(chunk) {
    if (this.size > 0 && this.size + chunk.length > this.maxBytes) {
      this.rotate();
    }
    fs.writeSync(this.fd, chunk);
    this.size += chunk.length;
  }

  // <file> becomes <file>.1, <file>.1 becomes <file>.2, ...; the oldest kept is dropped.
  rotate() {
    fs.closeSync(this.fd);
    const numbered = (index) => `${this.file}.${index}`;
    fs.rmSync(numbered(this.files - 1), { force: true });
    for (let index = this.files - 2; index >= 1; index -= 1) {
      if (fs.existsSync(numbered(index))) {
        fs.renameSync(numbered(index), numbered(index + 1));
      }
    }
    if (this.files > 1) {
      fs.renameSync(this.file, numbered(1));
    } else {
      fs.truncateSync(this.file, 0);
    }
    this.fd = fs.openSync(this.file, 'a');
    this.size = 0;
  }

  close() {
    fs.closeSync(this.fd);
  }
}

function positiveNumber(text, fallback) {
  const value = Number(text);
  return Number.isFinite(value) && value > 0 ? value : fallback;
}

function main(args) {
  const [file, megabytes, files] = args;
  // The server's own stop handling decides when its output ends.
  for (const signal of ['SIGTERM', 'SIGINT', 'SIGHUP']) {
    process.on(signal, () => {});
  }
  let terminal = true;
  process.stdout.on('error', () => {
    // A closed terminal: the file still gets every line.
    terminal = false;
  });
  let log = null;
  try {
    log = new RotatingLogFile({
      file: path.resolve(file),
      maxBytes: positiveNumber(megabytes, kDefaultMegabytes) * 1024 * 1024,
      files: Math.floor(positiveNumber(files, kDefaultFiles)),
    });
  } catch (error) {
    process.stdout.write(`LOG the log file ${file} could not be opened: ${error.message}\n`);
  }
  process.stdin.on('data', (chunk) => {
    if (terminal) {
      process.stdout.write(chunk);
    }
    if (!log) {
      return;
    }
    try {
      log.write(chunk);
    } catch (error) {
      // A full disk or a removed folder: the terminal keeps the output, the file stops.
      log = null;
      if (terminal) {
        process.stdout.write(`LOG writing ${file} stopped: ${error.message}\n`);
      }
    }
  });
  process.stdin.on('end', () => {
    if (log) {
      log.close();
    }
  });
}

if (require.main === module) {
  main(process.argv.slice(2));
}

module.exports = { RotatingLogFile };
