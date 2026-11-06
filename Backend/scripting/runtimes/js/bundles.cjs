'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { createHash } = require('node:crypto');
const { clientFiles, scriptEntries } = require('./manifest.cjs');
const { seal: sealFile, secretFor } = require('./resource_seal.cjs');

const LIMITS = Object.freeze({
  // Counts twenty times what 0.3 allowed. The files download over HTTP
  // (web/file_server.cjs) and big ones stay on the disk, on the server and in the players' games
  // (Client/resources/bundle_cache.cjs), so a file may be 512 MB and all of them 4 GB.
  resources: 5120,
  files: 81920,
  // What a resource's loading page may send first (kcds.loadingScreen): a player waits for all of
  // it before they see the page.
  loadingScreenBytes: 32 * 1024 * 1024,
  fileBytes: 512 * 1024 * 1024,
  totalBytes: 4 * 1024 * 1024 * 1024,
  catalogBytes: 20 * 1024 * 1024,
  chunkBytes: 16 * 1024,
  // A file up to this size is read into memory and sealed; a bigger one stays on
  // the disk and goes as it is (IN_MEMORY_BYTES).
  inMemoryBytes: 4 * 1024 * 1024,
});

function digest(bytes) {
  return createHash('sha256').update(bytes).digest('hex');
}

// A file up to this size is kept in memory; a bigger one stays on the disk and is read when a
// player asks for it, checked to be the file that was hashed.
const IN_MEMORY_BYTES = LIMITS.inMemoryBytes;
const READ_PIECE = 1024 * 1024;
const OPEN_FLAGS = fs.constants.O_RDONLY | (fs.constants.O_NOFOLLOW ?? 0);

// The SHA-256 of an open file, read in pieces.
function digestFile(fd, size) {
  const hash = createHash('sha256');
  const piece = Buffer.alloc(Math.min(READ_PIECE, Math.max(size, 1)));
  for (let offset = 0; offset < size; ) {
    const read = fs.readSync(fd, piece, 0, Math.min(piece.length, size - offset), offset);
    if (read <= 0) {
      throw new Error('Client resource file shrank while it was read');
    }
    hash.update(piece.subarray(0, read));
    offset += read;
  }
  return hash.digest('hex');
}

// The size of a catalog file: its bytes, or its place on the disk.
function blobSize(blob) {
  return Buffer.isBuffer(blob) ? blob.length : blob.size;
}

// `length` bytes of a catalog file from `offset`. A file on the disk must still be the one that
// was hashed (its size and modification time), or the read throws.
function readBlob(blob, offset, length) {
  if (Buffer.isBuffer(blob)) {
    return blob.subarray(offset, offset + length);
  }
  const fd = fs.openSync(blob.path, OPEN_FLAGS);
  try {
    const info = fs.fstatSync(fd);
    if (info.size !== blob.size || info.mtimeMs !== blob.mtimeMs) {
      throw new Error('Client resource file changed since the catalog was built');
    }
    const bytes = Buffer.alloc(Math.max(0, Math.min(length, blob.size - offset)));
    const read = bytes.length === 0 ? 0 : fs.readSync(fd, bytes, 0, bytes.length, offset);
    return bytes.subarray(0, read);
  } finally {
    fs.closeSync(fd);
  }
}

// The names of the files a resource's bundle sends to clients: its client files without the raw
// manifest and without a distinct server-only script folder (which also holds private imports,
// not just its entry point).
function bundledFiles(folder, manifest) {
  const clientFolders = scriptEntries(manifest, 'client').map((entry) => path.posix.dirname(entry));
  const serverFolders = manifest.kcds.serverScripts
    .map((entry) => path.posix.dirname(entry))
    .filter(
      (directory) =>
        directory !== '.' &&
        !clientFolders.some((client) => client === directory || client.startsWith(directory + '/')),
    );
  return clientFiles(folder, manifest).filter(
    (name) =>
      name !== 'package.json' &&
      !serverFolders.some((directory) => name.startsWith(directory + '/')),
  );
}

// The files a client side's scripts must be, by its language.
const CLIENT_SCRIPTS = Object.freeze({
  javascript: {
    pattern: /\.(?:c?js)$/,
    problem: 'Compile client TypeScript to CommonJS JavaScript before serving',
  },
  lua: { pattern: /\.lua$/, problem: 'Client Lua scripts must be .lua files' },
  csharp: {
    pattern: /\.dll$/i,
    problem: 'A C# client side lists the .dll files it was built into, not its sources',
  },
});

function publicManifest(manifest) {
  const source = manifest.kcds;
  // Never distribute the original package.json: it can contain server paths,
  // dependency secrets and arbitrary host-specific configuration.
  return {
    name: manifest.name,
    version: manifest.version,
    description: manifest.description,
    author: manifest.author,
    kcds: {
      serverScripts: [],
      clientScripts: [...source.clientScripts],
      sharedScripts: [...source.sharedScripts],
      // The language players' games run the client scripts in; the server's is
      // not theirs to know.
      language: { client: source.language?.client ?? 'javascript' },
      files: [],
      exports: [...source.exports],
      requires: source.requires.map(({ name, version, optional }) => ({
        name,
        ...(version === undefined ? {} : { version }),
        optional,
      })),
      errorBehavior: source.errorBehavior,
      priority: source.priority,
    },
  };
}

class ResourceBundleStore {
  constructor(limits = {}) {
    this.limits = { ...LIMITS, ...limits };
    this.revision = 0;
    this.fingerprint = null;
    this.catalog = null;
    this.catalogBytes = null;
    this.blobs = new Map();
    // Per resource its seal for this run: { generation, secret, sealed: Map(plain
    // hash -> { hash, bytes }) }. A resource started anew (another generation) gets a new secret.
    this.seals = new Map();
    // Where an error about one resource's client files goes.
    this.report = (text) => console.error(text);
  }

  // The resource's seal for this generation: the one it has, else a new secret.
  sealOf(name, generation, seals) {
    const kept = this.seals.get(name);
    const seal =
      kept && kept.generation === generation
        ? kept
        : { generation, secret: secretFor(), sealed: new Map() };
    seals.set(name, seal);
    return seal;
  }

  publish(definitions) {
    if (!Array.isArray(definitions) || definitions.length > this.limits.resources)
      throw new RangeError('Resource catalog limit exceeded');
    const names = new Set();
    const blobs = new Map();
    const seals = new Map();
    const resources = [];
    let fileCount = 0;
    let totalBytes = 0;
    for (const { manifest, folder, generation = 0 } of definitions) {
      if (names.has(manifest.name)) throw new Error('Duplicate resource in catalog');
      names.add(manifest.name);
      // A resource that cannot go to players (a file over the limit, say) is left out with an
      // error; every other resource and every join go on.
      const countsBefore = { fileCount, totalBytes };
      const ownBlobs = new Map();
      try {
        const entries = scriptEntries(manifest, 'client');
        // A client side runs CommonJS JavaScript, or Lua or C# assemblies when
        // that is its language.
        const language = manifest.kcds.language?.client ?? 'javascript';
        const runnable = CLIENT_SCRIPTS[language] ?? CLIENT_SCRIPTS.javascript;
        if (entries.some((entry) => !runnable.pattern.test(entry))) {
          throw new TypeError(runnable.problem);
        }
        const files = [];
        const seal = this.sealOf(manifest.name, generation, seals);
        for (const name of bundledFiles(folder, manifest)) {
          // Selection excludes links escaping the resource. O_NOFOLLOW also
          // rejects a leaf symlink substituted between selection and opening.
          const resolved = path.resolve(folder, name);
          const fd = fs.openSync(resolved, OPEN_FLAGS);
          let blob;
          let hash;
          try {
            const info = fs.fstatSync(fd);
            if (!info.isFile() || info.size > this.limits.fileBytes)
              throw new RangeError('Client resource file limit exceeded');
            if (info.size <= this.limits.inMemoryBytes) {
              blob = fs.readFileSync(fd);
              if (blob.length > this.limits.fileBytes)
                throw new RangeError('Client resource file limit exceeded');
              hash = digest(blob);
            } else {
              hash = digestFile(fd, info.size);
              blob = { path: resolved, size: info.size, mtimeMs: info.mtimeMs };
            }
          } finally {
            fs.closeSync(fd);
          }
          if (Buffer.isBuffer(blob)) {
            // Players get it sealed with the resource's secret, sealed once per run.
            let sealed = seal.sealed.get(hash);
            if (!sealed) {
              const sealedBytes = sealFile(blob, manifest.name, seal.secret);
              sealed = { hash: digest(sealedBytes), bytes: sealedBytes };
              seal.sealed.set(hash, sealed);
            }
            fileCount++;
            totalBytes += sealed.bytes.length;
            if (fileCount > this.limits.files || totalBytes > this.limits.totalBytes)
              throw new RangeError('Client resource bundle limit exceeded');
            ownBlobs.set(sealed.hash, sealed.bytes);
            files.push({ name, hash: sealed.hash, size: sealed.bytes.length });
          } else {
            // A file over inMemoryBytes stays on the disk and goes as it is:
            // `plain` tells the player's game not to open it with the seal.
            const size = blobSize(blob);
            fileCount++;
            totalBytes += size;
            if (fileCount > this.limits.files || totalBytes > this.limits.totalBytes)
              throw new RangeError('Client resource bundle limit exceeded');
            ownBlobs.set(hash, blob);
            files.push({ name, hash, size, plain: true });
          }
        }
        const shared = publicManifest(manifest);
        const screen = manifest.kcds.loadingScreen;
        if (screen) {
          const first = files.filter(
            (file) =>
              file.name === screen.page ||
              screen.files.some((pattern) => path.posix.matchesGlob(file.name, pattern)),
          );
          const bytes = first.reduce((sum, file) => sum + file.size, 0);
          // A page too big is the page's problem: the resource and every join go on without it.
          if (bytes > this.limits.loadingScreenBytes) {
            this.report(
              `${manifest.name}: kcds.loadingScreen is ${bytes} bytes, the limit is ` +
                `${this.limits.loadingScreenBytes}: players see the game's own loading screen`,
            );
          } else {
            shared.loadingScreen = { page: screen.page, files: first.map((file) => file.name) };
          }
        }
        // `seal`: the resource's secret for this run; the catalog goes to players over their
        // session only (main.cjs catalogChunk).
        const resource = { manifest: shared, files, generation, seal: seal.secret };
        resources.push({ ...resource, hash: digest(Buffer.from(JSON.stringify(resource))) });
        for (const [hash, bytes] of ownBlobs) {
          blobs.set(hash, bytes);
        }
      } catch (error) {
        fileCount = countsBefore.fileCount;
        totalBytes = countsBefore.totalBytes;
        // A refresh that went wrong: players keep the version they have.
        const previous = this.catalog?.resources.find(
          (entry) => entry.manifest.name === manifest.name,
        );
        this.report(
          `${manifest.name}: ${previous ? 'not updated' : 'not sent to players'}: ${error.message}`,
        );
        if (previous) {
          resources.push(previous);
          if (this.seals.has(manifest.name)) {
            seals.set(manifest.name, this.seals.get(manifest.name));
          }
          for (const file of previous.files) {
            blobs.set(file.hash, this.blobs.get(file.hash));
            fileCount++;
            totalBytes += file.size;
          }
        }
      }
    }
    const fingerprint = digest(Buffer.from(JSON.stringify(resources)));
    const catalog = { revision: this.revision + 1, hash: fingerprint, resources };
    if (Buffer.byteLength(JSON.stringify(catalog)) > this.limits.catalogBytes)
      throw new RangeError('Client resource catalog byte limit exceeded');
    if (fingerprint === this.fingerprint) return false;
    // Build the entire replacement before publishing. A failed refresh keeps
    // the old catalog and its immutable files available to connected clients.
    this.blobs = blobs;
    this.seals = seals;
    this.catalog = catalog;
    this.catalogBytes = Buffer.from(JSON.stringify(catalog));
    this.revision = catalog.revision;
    this.fingerprint = fingerprint;
    return true;
  }

  manifest() {
    return this.catalog === null ? null : structuredClone(this.catalog);
  }

  info() {
    return this.catalogBytes === null
      ? null
      : {
          revision: this.revision,
          hash: this.fingerprint,
          catalogHash: digest(this.catalogBytes),
          size: this.catalogBytes.length,
        };
  }

  catalogChunk(revision, offset, length = this.limits.chunkBytes) {
    if (revision !== this.revision || !this.catalogBytes)
      throw new Error('Stale resource catalog request');
    if (
      !Number.isSafeInteger(offset) ||
      offset < 0 ||
      offset > this.catalogBytes.length ||
      !Number.isSafeInteger(length) ||
      length < 1 ||
      length > this.limits.chunkBytes
    )
      throw new RangeError('Invalid resource catalog chunk range');
    return {
      revision,
      offset,
      size: this.catalogBytes.length,
      data: this.catalogBytes.subarray(offset, offset + length).toString('base64'),
    };
  }

  chunk(revision, hash, offset, length = this.limits.chunkBytes) {
    if (revision !== this.revision || !/^[a-f0-9]{64}$/.test(hash))
      throw new Error('Stale or invalid resource download');
    const blob = this.blobs.get(hash);
    if (!blob) throw new Error('File is not in the active client resource catalog');
    const size = blobSize(blob);
    if (
      !Number.isSafeInteger(offset) ||
      offset < 0 ||
      offset > size ||
      !Number.isSafeInteger(length) ||
      length < 1 ||
      length > this.limits.chunkBytes
    )
      throw new RangeError('Invalid resource chunk range');
    return {
      revision,
      hash,
      offset,
      size,
      data: readBlob(blob, offset, length).toString('base64'),
    };
  }
}

module.exports = {
  ResourceBundleStore,
  LIMITS,
  digest,
  bundledFiles,
  blobSize,
  readBlob,
  IN_MEMORY_BYTES,
};
