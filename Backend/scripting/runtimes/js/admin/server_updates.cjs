'use strict';

// Server updates from the KCDS update feed: KCDS-Admin's one-click update.
//
// Servers are hosted on Linux only: the release publishes the Linux server package to the signed
// feed the launcher reads, on the channel server-linux-<channel>:
//
//   GET {origin}/v1/updates/{channel}/manifest   an Ed25519-signed envelope of the release: its
//                                                 label, sequence and files (path, sha256, size)
//   GET {origin}/v1/artifacts/{sha256}           a file's bytes
//
// The managed files are the server binary, runtime/ (this resource host) and README.txt; the
// owner's server.json, resources, data, backups and start scripts are never touched. An update is
// staged first: the manifest is verified with the publisher keys shipped beside this file
// (update_keys.json, the same as Dev/tools/release/update-publisher-keys.json), every file that
// differs from the installed one is downloaded into updates/staged/ and checked against its hash.
// It is applied by the next planned restart, just before the server stops: each file goes in
// place by a rename (the installed file is renamed aside first), and the start script starts the
// new server (exit 75). The files set aside go at the next start. A manifest whose sequence is
// lower than the one applied last is refused (no rollback). Off on Windows (there is no Windows
// server package), in Docker (KCDS_DOCKER=1: the image is the update) and in a source tree (no
// server binary beside runtime/).

const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');

const DEFAULT_ORIGIN = 'https://updates.kcdsynced.com';
const MAX_MANIFEST_BYTES = 256 * 1024;
const MAX_FILE_BYTES = 256 * 1024 * 1024;
const MAX_FILES = 4096;
const SET_ASIDE = '.kcds-old';
// The server binary of the Linux server package.
const BINARY = 'kcd2s-server';

// The manifest's release label of a version: launchers accept only x.y.z or x.y.z-tag, so a
// hotfix x.y.z.w is x.y.z-hotfix.w.
function releaseLabel(version) {
  const parts = String(version).split('.');
  if (parts.length === 4) {
    return `${parts[0]}.${parts[1]}.${parts[2]}-hotfix.${parts[3]}`;
  }
  return String(version);
}

// A managed path as the feed names it: forward slashes, no '.', '..' or empty parts.
function safePath(value) {
  if (
    typeof value !== 'string' ||
    !value ||
    value.length > 240 ||
    !/^[a-zA-Z0-9._/-]+$/.test(value)
  ) {
    return false;
  }
  return value.split('/').every((part) => part && part !== '.' && part !== '..');
}

// Whether a path of the feed is one the server update may write.
function managed(file) {
  return file === BINARY || file === 'README.txt' || file.startsWith('runtime/');
}

// The release manifest of a signed envelope, or throws.
function verifyEnvelope(envelope, publicKeys) {
  if (
    !envelope ||
    envelope.schemaVersion !== 1 ||
    envelope.algorithm !== 'Ed25519' ||
    !Object.hasOwn(publicKeys, envelope.keyId) ||
    typeof envelope.payload !== 'string' ||
    envelope.payload.length > MAX_MANIFEST_BYTES ||
    typeof envelope.signature !== 'string'
  ) {
    throw new Error('the update manifest is not signed by a KCDS publisher key');
  }
  const bytes = Buffer.from(envelope.payload, 'base64');
  const signature = Buffer.from(envelope.signature, 'base64');
  const key = crypto.createPublicKey(publicKeys[envelope.keyId]);
  if (
    key.asymmetricKeyType !== 'ed25519' ||
    bytes.toString('base64') !== envelope.payload ||
    signature.length !== 64 ||
    !crypto.verify(null, bytes, key, signature)
  ) {
    throw new Error('the update manifest signature does not verify');
  }
  const manifest = JSON.parse(bytes.toString('utf8'));
  if (
    manifest.schemaVersion !== 1 ||
    typeof manifest.release !== 'string' ||
    !Number.isSafeInteger(manifest.sequence) ||
    manifest.sequence < 1 ||
    !Array.isArray(manifest.files) ||
    manifest.files.length === 0 ||
    manifest.files.length > MAX_FILES
  ) {
    throw new Error('the update manifest is not a release');
  }
  for (const file of manifest.files) {
    if (
      !safePath(file.path) ||
      !/^[a-f0-9]{64}$/.test(file.sha256 ?? '') ||
      !Number.isSafeInteger(file.size) ||
      file.size < 0 ||
      file.size > MAX_FILE_BYTES
    ) {
      throw new Error('the update manifest has an invalid file');
    }
  }
  return manifest;
}

function sha256(bytes) {
  return crypto.createHash('sha256').update(bytes).digest('hex');
}

function fileHash(file) {
  try {
    return sha256(fs.readFileSync(file));
  } catch {
    return null;
  }
}

class ServerUpdates {
  // install: the server's folder (its binary beside runtime/); version: the running server's;
  // log(text); fetch: the global fetch (tests give their own).
  constructor({
    install,
    version = () => '',
    platform = process.platform,
    env = process.env,
    publicKeys = null,
    log = () => {},
    fetch = globalThis.fetch,
  }) {
    this.install = install;
    this.version = version;
    this.platform = platform;
    this.origin = (env.KCDS_UPDATE_ORIGIN || DEFAULT_ORIGIN).replace(/\/+$/, '');
    const channel = env.KCDS_UPDATE_CHANNEL === 'preview' ? 'preview' : 'stable';
    this.channel = `server-linux-${channel}`;
    this.docker = env.KCDS_DOCKER === '1';
    this.publicKeys =
      publicKeys ?? JSON.parse(fs.readFileSync(path.join(__dirname, 'update_keys.json'), 'utf8'));
    this.log = log;
    this.fetch = fetch;
    this.folder = path.join(install, 'updates');
    this.busy = false;
  }

  // Why updates are off here, or '' when they can run.
  unavailable() {
    if (this.platform === 'win32') {
      return 'server updates are for the Linux server package; update this server by hand';
    }
    if (this.docker) {
      return 'this server runs in Docker: pull the new image instead';
    }
    if (!fs.existsSync(path.join(this.install, BINARY))) {
      return 'this server does not run from a KCDS server package';
    }
    return '';
  }

  // The state KCDS-Admin shows: { available, reason, current, channel, staged }.
  state() {
    const reason = this.unavailable();
    return {
      available: reason === '',
      reason,
      current: this.version(),
      channel: this.channel,
      staged: this.staged()?.release ?? null,
    };
  }

  // The newest release on the feed: { release, sequence, newer }.
  async check() {
    const manifest = await this.manifest();
    return {
      release: manifest.release,
      sequence: manifest.sequence,
      newer: manifest.release !== releaseLabel(this.version()),
    };
  }

  async manifest() {
    const reason = this.unavailable();
    if (reason) {
      throw new Error(reason);
    }
    const answer = await this.fetch(`${this.origin}/v1/updates/${this.channel}/manifest`);
    if (!answer.ok) {
      throw new Error(`the update feed answered ${answer.status}`);
    }
    const text = await answer.text();
    if (Buffer.byteLength(text) > MAX_MANIFEST_BYTES * 2) {
      throw new Error('the update manifest is too big');
    }
    const manifest = verifyEnvelope(JSON.parse(text), this.publicKeys);
    if (manifest.sequence < this.appliedSequence()) {
      throw new Error('the feed offers an older release than the one installed (refused)');
    }
    return manifest;
  }

  // Downloads the newest release's changed files into updates/staged/: { release, files }.
  async stage() {
    if (this.busy) {
      throw new Error('an update is being downloaded already');
    }
    this.busy = true;
    try {
      const manifest = await this.manifest();
      const files = manifest.files.filter((file) => managed(file.path));
      if (!files.some((file) => file.path === BINARY)) {
        throw new Error('the release has no server binary');
      }
      const target = path.join(this.folder, 'staged');
      fs.rmSync(target, { recursive: true, force: true });
      const changed = files.filter(
        (file) => fileHash(path.join(this.install, file.path)) !== file.sha256,
      );
      for (const file of changed) {
        const answer = await this.fetch(`${this.origin}/v1/artifacts/${file.sha256}`);
        if (!answer.ok) {
          throw new Error(`the update feed answered ${answer.status} for ${file.path}`);
        }
        const bytes = Buffer.from(await answer.arrayBuffer());
        if (bytes.length !== file.size || sha256(bytes) !== file.sha256) {
          throw new Error(`${file.path} does not match the signed release`);
        }
        const destination = path.join(target, ...file.path.split('/'));
        fs.mkdirSync(path.dirname(destination), { recursive: true });
        fs.writeFileSync(destination, bytes);
      }
      fs.writeFileSync(
        path.join(this.folder, 'staged.json'),
        JSON.stringify({
          release: manifest.release,
          sequence: manifest.sequence,
          files: changed.map((file) => file.path),
        }),
      );
      this.log(`update ${manifest.release} downloaded: ${changed.length} files changed`);
      return { release: manifest.release, files: changed.length };
    } finally {
      this.busy = false;
    }
  }

  // The staged update, or null.
  staged() {
    try {
      return JSON.parse(fs.readFileSync(path.join(this.folder, 'staged.json'), 'utf8'));
    } catch {
      return null;
    }
  }

  appliedSequence() {
    try {
      const applied = JSON.parse(fs.readFileSync(path.join(this.folder, 'applied.json'), 'utf8'));
      return Number.isSafeInteger(applied.sequence) ? applied.sequence : 0;
    } catch {
      return 0;
    }
  }

  // Puts a staged update in place (the planned restart, just before the server stops). True when
  // one was applied; a failure leaves the installed files as they were where it can.
  apply() {
    const staged = this.staged();
    if (!staged || this.unavailable()) {
      return false;
    }
    const source = path.join(this.folder, 'staged');
    for (const file of staged.files) {
      if (!safePath(file) || !managed(file)) {
        throw new Error(`the staged update names ${file}`);
      }
      const from = path.join(source, ...file.split('/'));
      const to = path.join(this.install, ...file.split('/'));
      fs.mkdirSync(path.dirname(to), { recursive: true });
      if (fs.existsSync(to)) {
        fs.rmSync(to + SET_ASIDE, { force: true });
        fs.renameSync(to, to + SET_ASIDE);
      }
      fs.renameSync(from, to);
      if (file === BINARY) {
        fs.chmodSync(to, 0o755);
      }
    }
    fs.writeFileSync(
      path.join(this.folder, 'applied.json'),
      JSON.stringify({ release: staged.release, sequence: staged.sequence, files: staged.files }),
    );
    fs.rmSync(source, { recursive: true, force: true });
    fs.rmSync(path.join(this.folder, 'staged.json'), { force: true });
    this.log(`update ${staged.release} applied: ${staged.files.length} files; the server restarts`);
    return true;
  }

  // At the start: the files the last update set aside go.
  cleanup() {
    let applied;
    try {
      applied = JSON.parse(fs.readFileSync(path.join(this.folder, 'applied.json'), 'utf8'));
    } catch {
      return;
    }
    for (const file of applied.files ?? []) {
      if (safePath(file) && managed(file)) {
        fs.rmSync(path.join(this.install, ...file.split('/')) + SET_ASIDE, { force: true });
      }
    }
  }
}

module.exports = { ServerUpdates, releaseLabel, verifyEnvelope };
