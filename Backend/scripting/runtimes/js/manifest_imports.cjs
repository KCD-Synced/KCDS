'use strict';

// The files a resource's scripts load (no server code reaches players): from each
// entry its imports, file by file, as the runtimes resolve them. JavaScript: relative require(),
// import ... from, export ... from and import() with a literal path, tried as the name itself, .js,
// .cjs, .mjs, .json, /index.js and /index.cjs (Client/resources/runtime.cjs resolveModule, and
// Node's on the server). Lua: require("folder.name") as folder/name.lua from the resource's root
// (languages/lua_host.cjs moduleFile). Other files (C# assemblies, data) load nothing here.
//
// A path built at run time is not followed: such a file is not counted as loaded.

const fs = require('node:fs');
const path = require('node:path');

const JAVASCRIPT = /\.(c|m)?js$/i;
const LUA = /\.lua$/i;
const JS_IMPORTS = [
  /\brequire\s*\(\s*(['"`])([^'"`\n]+)\1\s*\)/g,
  /\bimport\s*\(\s*(['"`])([^'"`\n]+)\1\s*\)/g,
  /\b(?:import|export)\b[^'"`;]*?\bfrom\s*(['"])([^'"\n]+)\1/g,
  /\bimport\s*(['"])([^'"\n]+)\1/g,
];
const LUA_IMPORT = /\brequire\s*\(?\s*(['"])([A-Za-z0-9_.]+)\1/g;
// The most files followed from one resource's entries (a resource sends at most 4096).
const MOST_FILES = 8192;

// The resource file a JavaScript module path names from `parent`, or null (not relative, outside
// the resource, or no such file).
function javascriptFile(root, parent, request) {
  if (!request.startsWith('./') && !request.startsWith('../')) {
    return null;
  }
  const base = path.posix.normalize(path.posix.join(path.posix.dirname(parent), request));
  if (base.startsWith('../') || base === '..' || path.posix.isAbsolute(base)) {
    return null;
  }
  const candidates = [
    base,
    `${base}.js`,
    `${base}.cjs`,
    `${base}.mjs`,
    `${base}.json`,
    `${base}/index.js`,
    `${base}/index.cjs`,
  ];
  return candidates.find((candidate) => isFile(root, candidate)) ?? null;
}

function luaFile(root, name) {
  const file = `${name.replaceAll('.', '/')}.lua`;
  return isFile(root, file) ? file : null;
}

function isFile(root, relative) {
  try {
    return fs.statSync(path.join(root, ...relative.split('/'))).isFile();
  } catch {
    return false;
  }
}

// The files `entries` load, the entries among them (each '/' separated, relative to `root`).
function loadedFiles(root, entries) {
  const loaded = new Set();
  const queue = [...entries];
  while (queue.length > 0 && loaded.size < MOST_FILES) {
    const file = queue.shift();
    if (loaded.has(file) || !isFile(root, file)) {
      continue;
    }
    loaded.add(file);
    let text;
    try {
      text = fs.readFileSync(path.join(root, ...file.split('/')), 'utf8');
    } catch {
      continue;
    }
    if (JAVASCRIPT.test(file)) {
      for (const pattern of JS_IMPORTS) {
        for (const match of text.matchAll(pattern)) {
          const found = javascriptFile(root, file, match[2]);
          if (found && !loaded.has(found)) {
            queue.push(found);
          }
        }
      }
    } else if (LUA.test(file)) {
      for (const match of text.matchAll(LUA_IMPORT)) {
        const found = luaFile(root, match[2]);
        if (found && !loaded.has(found)) {
          queue.push(found);
        }
      }
    }
  }
  return loaded;
}

module.exports = { loadedFiles, javascriptFile, luaFile };
