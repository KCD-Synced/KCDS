'use strict';

// Browser-compatible acquisition. The native bridge supplies the authenticated
// session's downloader and SHA-256 implementation; resources never supply paths.
const { openSealed } = require('./sealed_files.cjs');
const BUNDLE_LIMITS = Object.freeze({
  resources: 5120,
  files: 81920,
  fileBytes: 512 * 1024 * 1024,
  totalBytes: 4 * 1024 * 1024 * 1024,
  chunkBytes: 16 * 1024,
  // Larger files stay in the game's download cache; scripts and the loading
  // page's files are always held here.
  inMemoryBytes: 4 * 1024 * 1024,
  // The largest file still taken in chunks over the game connection when the game cannot keep
  // it on the disk (a server without HTTP files).
  chunkedFileBytes: 32 * 1024 * 1024,
});

// A file of the catalog the game keeps in its download cache: the bytes never
// come here. A reader hands it out in place of the bytes; the game takes the file by its hash,
// and only while it is one of the current server's files.
class DiskFile {
  constructor(hash, size) {
    this.hash = hash;
    this.size = size;
    Object.freeze(this);
  }

  get length() {
    return this.size;
  }
}

function validHash(value) {
  return typeof value === 'string' && /^[a-f0-9]{64}$/.test(value);
}

function validPath(value) {
  return (
    typeof value === 'string' &&
    value.length > 0 &&
    !value.includes('\\') &&
    !value.includes(':') &&
    !value.includes('\0') &&
    !value.startsWith('/') &&
    value.split('/').every((part) => part !== '' && part !== '.' && part !== '..')
  );
}

// How many chunks of a file are asked for at once.
const PARALLEL_CHUNKS = 8;

// What sealing adds to a file at most: the container's header and the resource's
// name (sealed_files.cjs).
const SEAL_OVERHEAD = 42 + 0xffff;

// The files a client side's scripts are, by its language (the server's bundles.cjs checks the same).
const CLIENT_SCRIPTS = Object.freeze({
  javascript: /\.(?:c?js)$/,
  lua: /\.lua$/,
  csharp: /\.dll$/i,
});

class BundleCache {
  constructor(hash, limits = {}) {
    if (typeof hash !== 'function') throw new TypeError('SHA-256 implementation required');
    this.hash = hash;
    this.limits = { ...BUNDLE_LIMITS, ...limits };
    this.files = new Map();
    // The current catalog's files opened in memory (`${resource}\0${file}` -> bytes).
    this.opened = null;
    this.generation = 0;
    this.active = null;
    this.encoder = new TextEncoder();
  }

  async validate(catalog) {
    if (
      !catalog ||
      !Number.isSafeInteger(catalog.revision) ||
      catalog.revision < 1 ||
      !validHash(catalog.hash) ||
      !Array.isArray(catalog.resources) ||
      catalog.resources.length > this.limits.resources
    )
      throw new TypeError('Invalid client resource catalog');
    const names = new Set();
    let count = 0;
    let total = 0;
    const expected = new Map();
    for (const resource of catalog.resources) {
      // `seal`: the resource's secret for this run, which opens its sealed files; a
      // server without it sends them plain.
      const { manifest, files, hash, generation = 0, seal } = resource ?? {};
      if (
        !manifest ||
        typeof manifest.name !== 'string' ||
        !manifest.name.length ||
        names.has(manifest.name) ||
        !Array.isArray(files) ||
        !validHash(hash) ||
        (seal !== undefined && (typeof seal !== 'string' || !seal.length))
      )
        throw new TypeError('Invalid client resource descriptor');
      const largest = this.limits.fileBytes + (seal === undefined ? 0 : SEAL_OVERHEAD);
      if (!Number.isSafeInteger(generation) || generation < 0)
        throw new TypeError('Invalid resource generation');
      names.add(manifest.name);
      const config = manifest.kcds;
      if (
        !config ||
        !Array.isArray(config.serverScripts) ||
        config.serverScripts.length ||
        !Array.isArray(config.clientScripts) ||
        !Array.isArray(config.sharedScripts) ||
        !Array.isArray(config.requires) ||
        !Array.isArray(config.exports)
      )
        throw new TypeError('Invalid client resource manifest');
      const paths = new Set();
      for (const file of files) {
        if (
          !validPath(file.name) ||
          paths.has(file.name) ||
          !validHash(file.hash) ||
          !Number.isSafeInteger(file.size) ||
          file.size < 0 ||
          file.size > largest ||
          (file.plain !== undefined && typeof file.plain !== 'boolean')
        )
          throw new TypeError('Invalid client resource file');
        paths.add(file.name);
        count++;
        total += file.size;
        if (expected.has(file.hash) && expected.get(file.hash) !== file.size)
          throw new TypeError('Inconsistent content-addressed resource file');
        expected.set(file.hash, file.size);
      }
      // The scripts are CommonJS JavaScript, or Lua or C# assemblies when
      // that is the client language.
      const runnable = CLIENT_SCRIPTS[config.language?.client] ?? CLIENT_SCRIPTS.javascript;
      for (const entry of [...config.sharedScripts, ...config.clientScripts]) {
        if (!validPath(entry) || !runnable.test(entry) || !paths.has(entry)) {
          throw new TypeError('Missing compiled client resource entry');
        }
      }
      if (
        (await this.hash(
          this.encoder.encode(JSON.stringify({ manifest, files, generation, seal })),
        )) !== hash
      )
        throw new Error('Resource descriptor hash mismatch');
    }
    if (count > this.limits.files || total > this.limits.totalBytes)
      throw new RangeError('Client resource catalog limit exceeded');
    if ((await this.hash(this.encoder.encode(JSON.stringify(catalog.resources)))) !== catalog.hash)
      throw new Error('Resource catalog hash mismatch');
    return expected;
  }

  // A loading page's files download first; when they are verified, firstFiles, when given, is called
  // with { resource, page, read(fileName) }, whatever the rest of the catalog still needs.
  // progress, when given, is called with { resourcesDone, resourcesTotal, bytesDone, bytesTotal }
  // once before the first download and after every verified chunk. A resource is done once all
  // of its files are verified; bytes count each distinct file once, cached files included.
  // ensure, when given, is called with { revision, hash, size } for a file larger than
  // inMemoryBytes that is neither a script nor the loading page's, and answers { ok } once the
  // game holds the verified file in its download cache; the file is then a DiskFile here.
  async acquire(input, download, progress = () => {}, firstFiles = null, ensure = null) {
    // Do not allow an asynchronous caller to change the catalog while validating.
    const catalog = structuredClone(input);
    const generation = ++this.generation;
    const expected = await this.validate(catalog);
    const staged = new Map();
    const checkSession = () => {
      if (generation !== this.generation) throw new Error('Resource session changed');
    };
    const resourceFiles = catalog.resources.map(
      (resource) => new Set(resource.files.map((file) => file.hash)),
    );
    const totals = { resourcesTotal: resourceFiles.length, bytesTotal: 0 };
    for (const size of expected.values()) totals.bytesTotal += size;
    let bytesDone = 0;
    const report = () => {
      const resourcesDone = resourceFiles.filter((hashes) =>
        [...hashes].every((hash) => staged.has(hash)),
      ).length;
      progress({ ...totals, resourcesDone, bytesDone });
    };
    report();
    const page = catalog.resources.find((resource) => resource.manifest.loadingScreen);
    const pageFiles = new Map(
      page
        ? page.files
            .filter((file) => page.manifest.loadingScreen.files.includes(file.name))
            .map((file) => [file.name, file.hash])
        : [],
    );
    const firstHashes = new Set(pageFiles.values());
    // Each resource's files opened in memory: sealed ones with the resource's
    // secret, those of a server that does not seal as they came. Only the sealed bytes are kept
    // for the cache; the plain ones live here. A big file the server sends as it is (`plain`)
    // stays a DiskFile in the game's download cache.
    const opened = new Map();
    const openFile = async (resource, file) => {
      const key = `${resource.manifest.name}\0${file.name}`;
      if (opened.has(key)) {
        return opened.get(key);
      }
      const bytes = staged.get(file.hash);
      const plain =
        resource.seal === undefined || file.plain === true || bytes instanceof DiskFile
          ? bytes
          : await openSealed(bytes, resource.manifest.name, resource.seal);
      if (!plain) {
        throw new Error(`Resource file did not open: ${resource.manifest.name}/${file.name}`);
      }
      opened.set(key, plain);
      return plain;
    };
    // The files that stay in memory however big: the loading page's and the scripts. Only a file
    // the server sent unsealed (`plain`, or every file of a server that does not seal) may stay
    // on the disk: a sealed one is opened in memory.
    const keptHashes = new Set(firstHashes);
    const diskHashes = new Set();
    for (const resource of catalog.resources) {
      const config = resource.manifest.kcds;
      const scripts = new Set([...config.sharedScripts, ...config.clientScripts]);
      for (const file of resource.files) {
        if (scripts.has(file.name)) {
          keptHashes.add(file.hash);
        }
        if (resource.seal === undefined || file.plain === true) {
          diskHashes.add(file.hash);
        }
      }
    }
    let pageTold = !firstFiles || !page || !pageFiles.has(page.manifest.loadingScreen.page);
    const tellPage = async () => {
      if (pageTold || [...firstHashes].some((hash) => !staged.has(hash))) {
        return;
      }
      pageTold = true;
      for (const file of page.files.filter((entry) => pageFiles.has(entry.name))) {
        await openFile(page, file);
      }
      await firstFiles({
        resource: page.manifest.name,
        page: page.manifest.loadingScreen.page,
        read: (fileName) => {
          // The page reads its own files only.
          const bytes = pageFiles.has(fileName)
            ? opened.get(`${page.manifest.name}\0${fileName}`)
            : null;
          return bytes ? bytes.slice() : null;
        },
      });
    };
    const ordered = [...expected].sort(
      ([a], [b]) => Number(firstHashes.has(b)) - Number(firstHashes.has(a)),
    );
    for (const [hash, size] of ordered) {
      await tellPage();
      checkSession();
      const onDisk =
        size > this.limits.inMemoryBytes && !keptHashes.has(hash) && diskHashes.has(hash);
      if (onDisk && ensure) {
        const ready = await ensure({ revision: catalog.revision, hash, size });
        checkSession();
        if (ready?.ok === true) {
          staged.set(hash, new DiskFile(hash, size));
          bytesDone += size;
          report();
          continue;
        }
      }
      if (size > this.limits.chunkedFileBytes) {
        throw new RangeError("A resource file this large needs the server's HTTP files");
      }
      const cached = this.files.get(hash);
      if (
        cached instanceof Uint8Array &&
        cached.length === size &&
        (await this.hash(cached)) === hash
      ) {
        staged.set(hash, cached);
        bytesDone += size;
        report();
        continue;
      }
      const bytes = new Uint8Array(size);
      let nextOffset = 0;
      let left = size;
      // Several chunks are on their way at once: one at a time waits a round trip for each.
      const fetchChunks = async () => {
        while (nextOffset < size) {
          const offset = nextOffset;
          const length = Math.min(this.limits.chunkBytes, size - offset);
          nextOffset += length;
          const chunk = await download({ revision: catalog.revision, hash, offset, length });
          checkSession();
          if (
            chunk.revision !== catalog.revision ||
            chunk.hash !== hash ||
            chunk.offset !== offset ||
            chunk.size !== size ||
            !(chunk.bytes instanceof Uint8Array) ||
            chunk.bytes.length !== length
          )
            throw new Error('Invalid resource download chunk');
          bytes.set(chunk.bytes, offset);
          bytesDone += length;
          left -= length;
          if (left > 0) report();
        }
      };
      await Promise.all(Array.from({ length: PARALLEL_CHUNKS }, fetchChunks));
      if ((await this.hash(bytes)) !== hash) throw new Error('Resource file hash mismatch');
      staged.set(hash, bytes);
      report();
    }
    await tellPage();
    for (const resource of catalog.resources) {
      for (const file of resource.files) {
        await openFile(resource, file);
      }
    }
    checkSession();
    // Publish only complete, verified resources. Bound cache memory to the current
    // catalog rather than retaining every file encountered across servers.
    this.files = staged;
    this.opened = opened;
    this.active = catalog;
    return structuredClone(catalog);
  }

  reader() {
    const catalog = this.active;
    const opened = this.opened;
    return (resourceName, fileName) => {
      const resource = catalog?.resources.find((entry) => entry.manifest.name === resourceName);
      const file = resource?.files.find((entry) => entry.name === fileName);
      const bytes = file && opened?.get(`${resourceName}\0${fileName}`);
      if (!bytes) throw new Error('File is not in the captured client resource revision');
      if (bytes instanceof DiskFile) {
        return bytes;
      }
      return bytes.slice();
    };
  }

  read(resourceName, fileName) {
    return this.reader()(resourceName, fileName);
  }

  disconnect() {
    this.generation++;
    this.active = null;
    // The plain files go with the session; the sealed ones stay for a reconnect.
    this.opened = null;
    // Cached bytes contain no running code or native handles. Reconnect may reuse
    // verified files; acquire rechecks their hashes before the next activation.
  }
}

module.exports = { BundleCache, DiskFile, BUNDLE_LIMITS, validHash, validPath };
