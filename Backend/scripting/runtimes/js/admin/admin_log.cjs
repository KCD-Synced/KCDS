'use strict';

// The resource host's recent log lines for KCDS-Admin's console: every line the
// host and the resources log, the newest kLimit kept, each with a growing sequence number so the
// page asks only for what it has not shown yet.

const kLimit = 2000;
const kMaxLineBytes = 4096;

class AdminLog {
  constructor(limit = kLimit, now = () => Date.now()) {
    this.limit = limit;
    this.now = now;
    this.lines = [];
    this.next = 1;
  }

  push(source, text) {
    let line = String(text);
    if (Buffer.byteLength(line) > kMaxLineBytes) {
      line = line.slice(0, kMaxLineBytes) + ' …';
    }
    this.lines.push({
      seq: this.next,
      time: this.now(),
      source: String(source ?? 'host'),
      text: line,
    });
    this.next += 1;
    if (this.lines.length > this.limit) {
      this.lines.splice(0, this.lines.length - this.limit);
    }
  }

  // The lines after sequence number `after`, at most `count`.
  after(after, count = 500) {
    const from = Number.isSafeInteger(after) ? after : 0;
    return this.lines.filter((line) => line.seq > from).slice(-count);
  }
}

module.exports = { AdminLog };
