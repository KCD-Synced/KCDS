'use strict';
const fs = require('node:fs');
const path = require('node:path');
const { loadedFiles } = require('./manifest_imports.cjs');

function strings(value, field) {
  if (value === undefined) return [];
  if (!Array.isArray(value) || value.some((v) => typeof v !== 'string' || !v.length)) {
    throw new TypeError(`${field} must be an array of nonempty strings`);
  }
  return [...value];
}

function relativePath(value, field, glob = false) {
  // Canonical wire names use '/' even on Windows. Do not accept drive/UNC paths.
  const name = value.replaceAll('\\', '/');
  if (
    name.startsWith('/') ||
    name.includes(':') ||
    name.includes('\0') ||
    name.split('/').some((part) => part === '..') ||
    (!glob && /[*?\[\]{}]/.test(name))
  ) {
    throw new TypeError(`Invalid resource path in ${field}: ${value}`);
  }
  const normalized = path.posix.normalize(name).replace(/^\.\//, '');
  if (normalized === '.' || normalized.endsWith('/'))
    throw new TypeError(`Invalid resource path in ${field}`);
  return normalized;
}

// kcds.loadingScreen: the page's path, or { page, files } with the patterns of what else it needs.
function loadingScreenOf(value) {
  if (value === undefined) {
    return undefined;
  }
  const source = typeof value === 'string' ? { page: value } : value;
  if (
    !source ||
    typeof source !== 'object' ||
    Array.isArray(source) ||
    typeof source.page !== 'string' ||
    !source.page.length
  ) {
    throw new TypeError('kcds.loadingScreen must be a page path or { page, files }');
  }
  return {
    page: relativePath(source.page, 'loadingScreen.page'),
    files: strings(source.files, 'loadingScreen.files').map((v) =>
      relativePath(v, 'loadingScreen.files', true),
    ),
  };
}

// The languages a resource's scripts are written in, by the file each script is.
const LANGUAGES = Object.freeze({
  javascript: ['.js', '.cjs', '.mjs'],
  lua: ['.lua'],
  csharp: ['.cs', '.csproj', '.dll'],
});

function languageOfFile(file) {
  const extension = path.posix.extname(file).toLowerCase();
  const found = Object.entries(LANGUAGES).find(([, extensions]) => extensions.includes(extension));
  return found ? found[0] : null;
}

// Each side's language: kcds.language as one name for both sides or { server, client }; a side it
// does not name takes its scripts' language (javascript when it has none). The scripts of a side
// must all be in its language.
function languagesOf(m, sides) {
  const given = m.language;
  if (
    given !== undefined &&
    typeof given !== 'string' &&
    (!given || typeof given !== 'object' || Array.isArray(given))
  ) {
    throw new TypeError('kcds.language must be a language name or { server, client }');
  }
  const languages = {};
  for (const [side, files] of Object.entries(sides)) {
    const named = typeof given === 'string' ? given : given?.[side];
    if (named !== undefined && !Object.hasOwn(LANGUAGES, named)) {
      const known = Object.keys(LANGUAGES).join(', ');
      throw new TypeError(`kcds.language: '${named}' is not one of ${known}`);
    }
    const found = new Set(files.map(languageOfFile).filter(Boolean));
    if (found.size > 1) {
      throw new TypeError(`The ${side} scripts mix languages (${[...found].join(', ')})`);
    }
    const language = named ?? [...found][0] ?? 'javascript';
    for (const file of files) {
      const own = languageOfFile(file);
      if (own !== null && own !== language) {
        throw new TypeError(`${file} is ${own}, but the ${side} side is ${language}`);
      }
    }
    languages[side] = language;
  }
  return languages;
}

function normalizeManifest(source) {
  if (
    !source ||
    typeof source !== 'object' ||
    Array.isArray(source) ||
    typeof source.name !== 'string' ||
    !source.name.trim()
  )
    throw new TypeError('Resource name missing');
  const m = source.kcds ?? {};
  if (typeof m !== 'object' || Array.isArray(m)) throw new TypeError('kcds must be an object');
  const scripts = (field, alias) => {
    if (m[alias] !== undefined && (typeof m[alias] !== 'string' || !m[alias].length)) {
      throw new TypeError(`kcds.${alias} must be a nonempty string`);
    }
    return [...(m[alias] === undefined ? [] : [m[alias]]), ...strings(m[field], field)].map((v) =>
      relativePath(v, field),
    );
  };
  const dependencies = m.requires ?? [];
  if (!Array.isArray(dependencies)) throw new TypeError('requires must be an array');
  const requires = dependencies.map((value) => {
    const d = typeof value === 'string' ? { name: value } : value;
    if (
      !d ||
      typeof d !== 'object' ||
      typeof d.name !== 'string' ||
      !d.name.length ||
      (d.optional !== undefined && typeof d.optional !== 'boolean') ||
      (d.version !== undefined && typeof d.version !== 'string')
    )
      throw new TypeError('Invalid resource dependency');
    return { ...d, optional: d.optional ?? false };
  });
  const errorBehavior = m.errorBehavior ?? 'stop';
  if (!['stop', 'restart', 'continue'].includes(errorBehavior))
    throw new TypeError('Invalid errorBehavior');
  const priority = m.priority ?? 0;
  if (!Number.isFinite(priority)) throw new TypeError('Invalid priority');
  const loadingScreen = loadingScreenOf(m.loadingScreen);
  const serverScripts = scripts('serverScripts', 'server');
  const clientScripts = scripts('clientScripts', 'client');
  const sharedScripts = strings(m.sharedScripts, 'sharedScripts').map((v) =>
    relativePath(v, 'sharedScripts'),
  );
  const languages = languagesOf(m, {
    server: [...sharedScripts, ...serverScripts],
    client: [...sharedScripts, ...clientScripts],
  });
  return {
    ...source,
    version: source.version ?? '1.0.0',
    description: source.description ?? '',
    author: source.author ?? '',
    kcds: {
      ...m,
      serverScripts,
      clientScripts,
      sharedScripts,
      language: languages,
      files: [...strings(m.files, 'files'), ...strings(m.clientFiles, 'clientFiles')].map((v) =>
        relativePath(v, 'files', true),
      ),
      exports: strings(m.exports, 'exports'),
      requires,
      errorBehavior,
      priority,
      ...(loadingScreen ? { loadingScreen } : {}),
    },
  };
}

function scriptEntries(manifest, side) {
  if (side !== 'server' && side !== 'client') throw new TypeError('Invalid resource side');
  return [...manifest.kcds.sharedScripts, ...manifest.kcds[`${side}Scripts`]];
}

// Code and data a default client folder sends only when a client script loads it: a
// server helper or a server's config beside a client script stays on the server.
const CODE_OR_DATA =
  /\.((c|m)?js|ts|lua|json|dll|pdb|cs|env|ini|ya?ml|toml|sql|db|sqlite|key|pem)$/i;

// Files a default client folder never sends, even when a client script loads them: a server's
// data and keys (a `files` pattern still may).
const SERVER_DATA =
  /(?:^|\/)(?:package\.json|package-lock\.json|server\.json|[^/]+\.(?:sql|db|sqlite3?|pem|key|p12|pfx|log))$/i;

function clientFiles(folder, manifest) {
  const root = fs.realpathSync(folder),
    config = manifest.kcds;
  const scripts = scriptEntries(manifest, 'client');
  // What the client scripts load, and what only the server's do: never sent to players, and a
  // `files` pattern naming such a file is refused.
  const clientLoaded = loadedFiles(root, scripts);
  const serverOnly = new Set(
    [...loadedFiles(root, scriptEntries(manifest, 'server'))].filter(
      (file) => !clientLoaded.has(file),
    ),
  );
  // A C# client assembly loads the assemblies built beside it (its packages' DLLs).
  const assemblyFolders = scripts
    .filter((file) => /\.dll$/i.test(file))
    .map((file) => path.posix.dirname(file));
  const besideAssembly = (name) =>
    /\.dll$/i.test(name) && assemblyFolders.includes(path.posix.dirname(name));
  const selected = new Set([...scripts, ...clientLoaded]);
  const loadingScreen = config.loadingScreen;
  if (loadingScreen) {
    selected.add(loadingScreen.page);
  }
  const patterns = [...config.files, ...(loadingScreen?.files ?? [])];
  const named = [];
  const defaultFolders = config.files.length
    ? []
    : config.clientScripts.map((file) => path.posix.dirname(file));
  // A default folder never sends hidden files (.env, .git) or installed packages (node_modules):
  // a client script beside the server's files would hand players its secrets. A
  // `files` pattern may still name them on purpose.
  // Nor the folders of the server's scripts (server/, lib/ beside server.js: not the client's
  // own), nor files that hold a server's data or keys: a database, a certificate, a log, the
  // package files (the catalog carries the manifest itself).
  const clientFolders = new Set(defaultFolders);
  const serverFolders = config.serverScripts
    .map((file) => path.posix.dirname(file))
    .filter((folder) => folder !== '.' && !clientFolders.has(folder));
  const privateByDefault = (name) =>
    name.split('/').some((part) => part.startsWith('.') || part === 'node_modules') ||
    serverFolders.some((folder) => name.startsWith(folder + '/')) ||
    SERVER_DATA.test(name);
  const visit = (directory, prefix = '') => {
    for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
      const name = prefix + entry.name;
      // Never follow resource symlinks/junctions into credentials outside the resource.
      if (entry.isSymbolicLink()) continue;
      if (entry.isDirectory()) {
        visit(path.join(directory, entry.name), name + '/');
        continue;
      }
      if (!entry.isFile()) {
        continue;
      }
      // A resource's streamed and replacing game files always go to clients.
      if (name.startsWith('stream/') || name.startsWith('replace/')) {
        selected.add(name);
        continue;
      }
      const matched = patterns.some((pattern) => path.posix.matchesGlob(name, pattern));
      if (serverOnly.has(name)) {
        // Named exactly (no wildcard) it is a mistake to tell; a wider pattern just leaves it out.
        if (patterns.some((pattern) => !/[*?[\]{}]/.test(pattern) && pattern === name)) {
          named.push(name);
        }
        continue;
      }
      const inDefaultFolder =
        !privateByDefault(name) &&
        defaultFolders.some((p) => p === '.' || name.startsWith(p + '/'));
      const loaded = clientLoaded.has(name) || besideAssembly(name);
      if (matched || (inDefaultFolder && (!CODE_OR_DATA.test(name) || loaded))) {
        selected.add(name);
      }
    }
  };
  visit(root);
  if (named.length > 0) {
    throw new Error(
      `files names server code, which is never sent to players: ${named.sort().join(', ')} ` +
        '(a server script loads it and no client script does)',
    );
  }
  const result = [...selected].filter((file) => !serverOnly.has(file)).sort();
  for (const file of result) {
    const resolved = fs.realpathSync(path.resolve(root, file));
    const relative = path.relative(root, resolved);
    if (
      relative.startsWith('..' + path.sep) ||
      relative === '..' ||
      path.isAbsolute(relative) ||
      !fs.statSync(resolved).isFile()
    ) {
      throw new Error(`Client file escapes resource: ${file}`);
    }
  }
  return result;
}

module.exports = { normalizeManifest, scriptEntries, clientFiles, LANGUAGES, languageOfFile };
